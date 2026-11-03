#!/usr/bin/env python3
"""!GBA memory by owner (D-42 §4.6; docs/USAGE.md): what each namespace of a linked GBA program holds in IWRAM (code
and data) and EWRAM, read from its sized symbol listing (`nm -S --defined-only`, the build's capsule_<game>.nm), and
the check of the program against the regions' limits and the game's budgets ("build": {"memory_budgets": ...} of its
reference/project.json, tools/build_settings.py).

    usage.py memory LISTING [--project project.json] [--stack-reserve BYTES] [--game NAME] [--out FILE] [--json]
                             [--check] [--demangler C++FILT]

An **owner** is a symbol's first two namespaces (capsule::audio, pocket::battle, a game's own namespace), taken from
its mangled name; the engine's namespaces that live in another module count as that module (capsule::rng is
capsule::core). Symbols outside any namespace (C code, the C library) are GLOBAL_OWNER. A budget may name a namespace
at any depth and covers every symbol below it.

The regions' totals come from the linker's own symbols when the listing has them (__iheap_start: the end of IWRAM's
code and data; __sbss_end__: the end of EWRAM's data), else from the sum of the sized symbols. The IWRAM limit is the
region less the stack reserve, as cmake/gba_memory.ld checks it. The result is capsule.usage-memory/1.
"""
from __future__ import annotations

import re
import shutil
import subprocess
from collections.abc import Iterable, Mapping, Sequence
from typing import Any

## The memory report's schema.
MEMORY_SCHEMA = "capsule.usage-memory/1"
## GBA EWRAM's first address (gba_cart.ld's ewram region).
EWRAM_BASE = 0x02000000
## GBA IWRAM's first address (its iwram region).
IWRAM_BASE = 0x03000000
## GBA ROM's first address (the cartridge's wait state 0 window).
ROM_BASE = 0x08000000
## Bytes of each region window the address's top byte selects (the GBA's memory map is laid out by it).
REGION_WINDOW = 0x01000000
## GBA EWRAM bytes.
EWRAM_BYTES = 256 * 1024
## GBA IWRAM bytes.
IWRAM_BYTES = 32 * 1024
## The engine's default stack reserve (CMake's CAPSULE_GBA_STACK_RESERVE).
DEFAULT_STACK_RESERVE = 2048
## The columns of an owner: IWRAM code, IWRAM data, EWRAM, ROM.
IWRAM_CODE = "iwram_code"
## See IWRAM_CODE.
IWRAM_DATA = "iwram_data"
## See IWRAM_CODE.
EWRAM = "ewram"
## See IWRAM_CODE.
ROM = "rom"
## IWRAM code and data together (a region budget's name).
IWRAM = "iwram"
## The columns, in the table's order.
COLUMNS = (IWRAM_CODE, IWRAM_DATA, EWRAM, ROM)
## The columns each budget region sums.
REGION_COLUMNS = {IWRAM: (IWRAM_CODE, IWRAM_DATA), IWRAM_CODE: (IWRAM_CODE,), IWRAM_DATA: (IWRAM_DATA,),
                  EWRAM: (EWRAM,)}
## Each region's name in text.
REGION_TEXT = {IWRAM: "IWRAM (code + data)", IWRAM_CODE: "IWRAM code", IWRAM_DATA: "IWRAM data", EWRAM: "EWRAM"}
## nm kinds of code (text, weak functions); every other sized kind is data.
CODE_KINDS = frozenset("tTWi")
## nm kinds that take no memory of their own (absolute symbols, debugging, weak references to nothing).
SKIPPED_KINDS = frozenset("aANnUwv-")
## A sized line of the listing: address, size, kind, name.
SIZED_LINE = re.compile(r"^(?P<addr>[0-9a-fA-F]+)\s+(?P<size>[0-9a-fA-F]+)\s+(?P<kind>\S)\s+(?P<name>\S+)$")
## A line without a size (the linker script's symbols): address, kind, name.
UNSIZED_LINE = re.compile(r"^(?P<addr>[0-9a-fA-F]+)\s+(?P<kind>\S)\s+(?P<name>\S+)$")
## The linker symbol at the end of IWRAM's code and data (gba_crt0 starts the IWRAM heap there).
IWRAM_END_SYMBOL = "__iheap_start"
## The linker symbol at the end of EWRAM's data (.ewram + .sbss).
EWRAM_END_SYMBOL = "__sbss_end__"
## Hex base.
HEX = 16
## The owner of a symbol outside every namespace (C code, the C library, the start-up code).
GLOBAL_OWNER = "(global)"
## Namespaces an owner keeps (capsule::audio, not capsule::audio::detail).
OWNER_DEPTH = 2
## Engine namespaces that live in a module of another name (tools/usage.py NAMESPACE_MODULES).
ENGINE_NAMESPACE = "capsule"
## See ENGINE_NAMESPACE.
NAMESPACE_MODULES = {"rng": "core", "clock": "core", "rules": "battle", "video": "gfx"}
## The Itanium ABI's special-name prefixes before an entity (guard variables, vtables, typeinfo, thunks).
SPECIAL_PREFIXES = ("GV", "TV", "TI", "TS", "TT", "TH", "TW")
## The name the Itanium ABI gives an anonymous namespace.
ANONYMOUS_NAMESPACE = "_GLOBAL__N_1"
## Qualifiers a nested name may start with (restrict, volatile, const, ref-qualifiers).
NESTED_QUALIFIERS = "rVKRO"
## The Itanium ABI's abbreviation of `std::`.
STD_ABBREVIATION = "St"
## The length that starts a source name (`7capsule`).
NAME_LENGTH = re.compile(r"\d+")
## The Itanium ABI's mark of an entity with internal linkage (`L6g_save`).
INTERNAL_LINKAGE = "L"
## The suffixes the compiler gives a symbol's copies (link-time optimization's private copies, clones, split parts):
## `name.lto_priv.0`, `name.constprop.1`, `name.part.0`, `name.cold`.
CLONE_SUFFIX = re.compile(r"(\.(lto_priv|constprop|isra|part|cold|clone)(\.\d+)?)+$")
## An interworking veneer the linker adds for an ARM / Thumb call (`__<target>_from_arm`): it counts as its target's.
VENEER = re.compile(r"^__(?P<target>.+)_from_(arm|thumb)$")
## What a shown veneer's name ends with.
VENEER_TEXT = " (veneer)"
## A demangled anonymous namespace (left out of shown names).
SHOWN_ANONYMOUS = "(anonymous namespace)::"
## Owners the text lists per table.
LISTED_OWNERS = 12
## Contributors a failed budget names.
LISTED_CONTRIBUTORS = 5
## Symbols listed per contributor.
LISTED_SYMBOLS = 3
## Symbols kept per owner and column in the JSON.
KEPT_SYMBOLS = 20
## Width of the owner column of the text table.
OWNER_WIDTH = 34
## Width of a number column of the text table.
NUMBER_WIDTH = 12


def namespace_path(name: str) -> list[str]:
   """!The scopes of a mangled name (namespaces, then classes, then the entity), anonymous namespaces left out.
   @param name  the symbol (`_ZN7capsule5audio6g_ringE`); an unmangled name has none
   @return e.g. ["capsule", "audio", "g_ring"]; the entity alone for a name in the global namespace; [] for a name
           that is not mangled
   """
   if not name.startswith("_Z"):
      return []
   i = len("_Z")
   # Special names (`_ZGV...` guard variables, `_ZTV...` vtables) and local entities (`_ZZ<function>E<name>`) name
   # their scopes after the prefix.
   progress = True
   while progress:
      progress = False
      for prefix in SPECIAL_PREFIXES:
         if name.startswith(prefix, i):
            i += len(prefix)
            progress = True
      if name.startswith("Z", i):
         i += 1
         progress = True
   path: list[str] = []

   def source_name(at: int) -> int:
      """!Reads one source name (an internal linkage mark before it skipped) into path.
      @return the index after it, or -1 when none starts there"""
      if name.startswith(INTERNAL_LINKAGE, at):
         at += len(INTERNAL_LINKAGE)
      m = NAME_LENGTH.match(name, at)
      if m is None:
         return -1
      start = m.end()
      part = name[start:start + int(m.group())]
      if part != ANONYMOUS_NAMESPACE:
         path.append(part)
      return start + len(part)

   if name.startswith(STD_ABBREVIATION, i):   # `_ZSt4moveIRiE...`: std::move
      path.append("std")
      source_name(i + len(STD_ABBREVIATION))
      return path
   if not name.startswith("N", i):   # an unqualified name (`_Z3fooi`, `_ZL5g_foo`): the global namespace
      source_name(i)
      return path
   i += 1
   while i < len(name) and name[i] in NESTED_QUALIFIERS:
      i += 1
   if name.startswith(STD_ABBREVIATION, i):
      path.append("std")
      i += len(STD_ABBREVIATION)
   # Each scope until template arguments, a substitution or the nested name's end.
   while i >= 0 and i < len(name):
      i = source_name(i)
   return path


def owner_of(name: str) -> str:
   """!The owner of a symbol: its first OWNER_DEPTH scopes, the entity itself left out.
   @param name  the mangled symbol
   @return e.g. "capsule::audio", or GLOBAL_OWNER
   """
   scopes = namespace_path(name)[:-1]
   if not scopes:
      return GLOBAL_OWNER
   owner = scopes[:OWNER_DEPTH]
   if owner[0] == ENGINE_NAMESPACE and len(owner) > 1:
      owner[1] = NAMESPACE_MODULES.get(owner[1], owner[1])
   return "::".join(owner)


def target_of(name: str) -> str:
   """!The symbol a veneer leads to (VENEER), or the name itself.
   @param name  a symbol
   @return its target
   """
   m = VENEER.match(name)
   return m["target"] if m is not None else name


def column_of(addr: int, kind: str) -> str | None:
   """!The column a symbol counts in.
   @param addr  its address
   @param kind  its nm kind
   @return IWRAM_CODE, IWRAM_DATA, EWRAM, ROM, or None outside those regions
   """
   window = addr - addr % REGION_WINDOW
   if window == IWRAM_BASE:
      return IWRAM_CODE if kind in CODE_KINDS else IWRAM_DATA
   if window == EWRAM_BASE:
      return EWRAM
   if window >= ROM_BASE:
      return ROM
   return None


class Symbol:
   """!One sized symbol of the listing."""

   def __init__(self, name: str, size: int, column: str) -> None:
      """!A symbol.
      @param name    its mangled name
      @param size    its bytes
      @param column  its column (column_of())
      """
      ## The mangled name.
      self.name = name
      ## Its bytes.
      self.size = size
      ## Its column.
      self.column = column
      ## Its scopes (namespace_path(); a veneer's are its target's).
      self.scopes = namespace_path(target_of(name))
      ## Its owner (owner_of()).
      self.owner = owner_of(target_of(name))


def read_listing(text: str) -> tuple[list[Symbol], dict[str, int]]:
   """!The sized symbols and the linker's symbols of a listing.
   @param text  `nm -S --defined-only` output
   @return (symbols in the GBA's regions, {linker symbol: address} of the unsized ones)
   """
   symbols: list[Symbol] = []
   marks: dict[str, int] = {}
   for line in text.splitlines():
      m = SIZED_LINE.match(line.strip())
      if m is not None:
         if m["kind"] in SKIPPED_KINDS:
            continue
         column = column_of(int(m["addr"], HEX), m["kind"])
         size = int(m["size"], HEX)
         if column is not None and size:
            symbols.append(Symbol(m["name"], size, column))
         continue
      u = UNSIZED_LINE.match(line.strip())
      if u is not None:
         marks[u["name"]] = int(u["addr"], HEX)
   return symbols, marks


def covers(budget_owner: str, symbol: Symbol) -> bool:
   """!Whether an owner budget covers a symbol (the symbol's scopes start with the budget's namespaces).
   @param budget_owner  the budget's namespace ("capsule::audio", "pocket", GLOBAL_OWNER)
   @param symbol        the symbol
   @return bool
   """
   if budget_owner == GLOBAL_OWNER:
      return symbol.owner == GLOBAL_OWNER
   wanted = budget_owner.split("::")
   if wanted[0] == ENGINE_NAMESPACE and len(wanted) > 1:
      # The engine's namespaces of another module (capsule::rng) count as that module (capsule::core).
      scopes = list(symbol.scopes)
      if scopes[:1] == [ENGINE_NAMESPACE] and len(scopes) > 1:
         scopes[1] = NAMESPACE_MODULES.get(scopes[1], scopes[1])
      return scopes[:len(wanted)] == wanted and len(scopes) > len(wanted)
   return symbol.scopes[:len(wanted)] == wanted and len(symbol.scopes) > len(wanted)


def short_name(shown: str) -> str:
   """!A demangled name without anonymous namespaces and without a function's parameter list.
   @param shown  c++filt's text of a symbol: its scopes (anonymous namespaces among them), its name and, for a
                 function, its parameter list
   @return e.g. `capsule::audio::detail::mix_pcm`
   """
   text = shown.replace(SHOWN_ANONYMOUS, "")
   if text.endswith(" const"):
      text = text[:-len(" const")]
   if text.endswith(")"):
      # Back to the parenthesis that opens the parameter list (parameters may hold parentheses of their own).
      depth = 0
      for i in range(len(text) - 1, -1, -1):
         depth += {")": 1, "(": -1}.get(text[i], 0)
         if depth == 0:
            return text[:i] if i else text
   return text


def demangle(names: Iterable[str], demangler: str | None) -> dict[str, str]:
   """!Readable names, through a c++filt (short_name(); a copy's suffix such as `.lto_priv.0` left out).
   @param names      mangled names
   @param demangler  the c++filt program, or None to keep the names as they are (without the copy suffixes)
   @return mangled -> shown
   """
   unique = sorted(set(names))
   bases = [CLONE_SUFFIX.sub("", target_of(n)) for n in unique]
   marks = [VENEER_TEXT if VENEER.match(n) else "" for n in unique]
   plain = {n: b + m for n, b, m in zip(unique, bases, marks)}
   tool = shutil.which(demangler) if demangler else None
   if tool is None or not unique:
      return plain
   try:
      done = subprocess.run([tool], input="\n".join(bases) + "\n", capture_output=True, text=True, check=True)
   except (OSError, subprocess.CalledProcessError):
      return plain
   shown = done.stdout.splitlines()
   if len(shown) != len(unique):
      return plain
   return {n: short_name(t) + m for n, t, m in zip(unique, shown, marks)}


def sum_columns(symbols: Iterable[Symbol], region: str) -> int:
   """!Bytes of symbols in a budget region.
   @param symbols  the symbols
   @param region   a REGION_COLUMNS name
   @return the sum
   """
   columns = REGION_COLUMNS[region]
   return sum(s.size for s in symbols if s.column in columns)


def contributors(symbols: Sequence[Symbol], region: str, shown: Mapping[str, str]) -> list[dict[str, Any]]:
   """!The owners that hold the most of a region, with their largest symbols.
   @param symbols  the symbols counted
   @param region   a REGION_COLUMNS name
   @param shown    demangled names
   @return [{"owner", "bytes", "largest": [[name, bytes], ...]}], largest first, LISTED_CONTRIBUTORS of them
   """
   columns = REGION_COLUMNS[region]
   by_owner: dict[str, list[Symbol]] = {}
   for s in symbols:
      if s.column in columns:
         by_owner.setdefault(s.owner, []).append(s)
   ranked = sorted(by_owner.items(), key=lambda kv: (-sum(s.size for s in kv[1]), kv[0]))
   out = []
   for owner, items in ranked[:LISTED_CONTRIBUTORS]:
      largest = sorted(items, key=lambda s: (-s.size, s.name))[:LISTED_SYMBOLS]
      out.append({"owner": owner, "bytes": sum(s.size for s in items),
                  "largest": [[shown.get(s.name, s.name), s.size] for s in largest]})
   return out


def report(text: str, budgets: Mapping[str, Any] | None = None, stack_reserve: int = DEFAULT_STACK_RESERVE,
           game: str = "", demangler: str | None = None) -> dict[str, Any]:
   """!A program's memory by owner, checked against the regions and the budgets (capsule.usage-memory/1).
   @param text           the listing
   @param budgets        tools/build_settings.py memory_budgets()'s object, or None
   @param stack_reserve  IWRAM bytes kept for the stack
   @param game           the game's name
   @param demangler      a c++filt for readable names, or None
   @return {"schema", "game", "regions": {region: {"used", "limit", "free"}}, "owners": {owner: {column: bytes,
           "largest": {column: {name: bytes}}}}, "checks": [{"owner", "region", "budget", "used", "over",
           "contributors"}], "over": the checks that failed}
   """
   symbols, marks = read_listing(text)
   budgets = budgets or {"regions": {}, "owners": {}}
   shown = demangle((s.name for s in symbols), demangler)

   # The regions' totals: the linker's ends when the listing has them (padding and unsized parts included).
   iwram_used = marks[IWRAM_END_SYMBOL] - IWRAM_BASE if IWRAM_END_SYMBOL in marks else sum_columns(symbols, IWRAM)
   ewram_used = marks[EWRAM_END_SYMBOL] - EWRAM_BASE if EWRAM_END_SYMBOL in marks else sum_columns(symbols, EWRAM)
   used = {IWRAM: iwram_used, IWRAM_CODE: sum_columns(symbols, IWRAM_CODE),
           IWRAM_DATA: sum_columns(symbols, IWRAM_DATA), EWRAM: ewram_used}
   limits = {IWRAM: IWRAM_BYTES - stack_reserve, EWRAM: EWRAM_BYTES}
   regions = {r: {"used": used[r], "limit": limits.get(r), "free": None if r not in limits else limits[r] - used[r]}
              for r in (IWRAM, IWRAM_CODE, IWRAM_DATA, EWRAM)}

   owners: dict[str, dict[str, Any]] = {}
   for s in symbols:
      entry = owners.setdefault(s.owner, {**{c: 0 for c in COLUMNS}, "largest": {c: {} for c in COLUMNS}})
      entry[s.column] += s.size
      items = entry["largest"][s.column]
      label = shown.get(s.name, s.name)
      items[label] = items.get(label, 0) + s.size
   for entry in owners.values():
      for column, items in entry["largest"].items():
         ranked = sorted(items.items(), key=lambda kv: (-kv[1], kv[0]))[:KEPT_SYMBOLS]
         entry["largest"][column] = dict(ranked)

   # The checks: each region's limit, then the game's region budgets, then its owner budgets.
   checks = []

   def check(owner: str | None, region: str, budget: int, counted: Sequence[Symbol], total: int) -> None:
      """!Adds one check."""
      checks.append({"owner": owner, "region": region, "budget": budget, "used": total,
                     "over": max(0, total - budget),
                     "contributors": contributors(counted, region, shown) if total > budget else []})

   for region, limit in limits.items():
      check(None, region, limit, symbols, used[region])
   for region, budget in budgets.get("regions", {}).items():
      check(None, region, budget, symbols, used[region])
   for owner, regions_of in budgets.get("owners", {}).items():
      counted = [s for s in symbols if covers(owner, s)]
      for region, budget in regions_of.items():
         check(owner, region, budget, counted, sum_columns(counted, region))
   ordered = dict(sorted(owners.items(), key=lambda kv: (-(kv[1][IWRAM_CODE] + kv[1][IWRAM_DATA]),
                                                         -kv[1][EWRAM], kv[0])))
   return {"schema": MEMORY_SCHEMA, "game": game, "stack_reserve": stack_reserve, "regions": regions,
           "owners": ordered, "checks": checks, "over": [c for c in checks if c["over"]]}


def number(n: int) -> str:
   """!A byte count with thousands separators.
   @param n  bytes
   @return text
   """
   return f"{n:,}"


def report_text(result: Mapping[str, Any], listed: int = LISTED_OWNERS) -> str:
   """!A memory report as text: the regions, the owners holding the most RAM, then every failed check with the largest
   contributors.
   @param result  report()'s object
   @param listed  owners shown in the table
   @return the text
   """
   regions = result["regions"]
   iw = regions[IWRAM]
   ew = regions[EWRAM]

   def margin(r: Mapping[str, Any]) -> str:
      """!Free bytes or the overflow."""
      return f"{number(r['free'])} B free" if r["free"] >= 0 else f"over by {number(-r['free'])} B"

   lines = [f"GBA memory by owner ({result['game'] or 'program'}): "
            f"IWRAM {number(iw['used'])} of {number(iw['limit'])} B ({margin(iw)}, stack reserve "
            f"{number(result['stack_reserve'])} B); EWRAM {number(ew['used'])} of {number(ew['limit'])} B "
            f"({margin(ew)})"]
   heads = ("IWRAM code", "IWRAM data", "EWRAM")
   lines.append("   " + "Owner".ljust(OWNER_WIDTH) + "".join(h.rjust(NUMBER_WIDTH) for h in heads))
   ram = [(o, e) for o, e in result["owners"].items() if e[IWRAM_CODE] or e[IWRAM_DATA] or e[EWRAM]]
   for owner, entry in ram[:listed]:
      lines.append("   " + owner[:OWNER_WIDTH - 1].ljust(OWNER_WIDTH) +
                   "".join(number(entry[c]).rjust(NUMBER_WIDTH) for c in (IWRAM_CODE, IWRAM_DATA, EWRAM)))
   if len(ram) > listed:
      lines.append(f"   ... {len(ram) - listed} more owners (the JSON report has every one)")
   for c in result["over"]:
      whose = f"{c['owner']} " if c["owner"] else ""
      lines.append(f"Over budget: {whose}{REGION_TEXT[c['region']]} {number(c['used'])} of {number(c['budget'])} B "
                   f"(over by {number(c['over'])} B). Largest:")
      for who in c["contributors"]:
         names = ", ".join(f"{n} {number(b)}" for n, b in who["largest"])
         lines.append(f"   {who['owner']}: {number(who['bytes'])} B ({names})")
   return "\n".join(lines) + "\n"
