#!/usr/bin/env python3
"""!Usage reports (D-36 §5; docs/USAGE.md): what a game's build uses, recorded by its compilers and merged into one
report, and the GBA link map compared against it.

Each compiler run with `--usage FILE` writes a **usage record** (capsule.usage/1): the game, the compiler and what it
emitted or referenced, as categories of named counts:

    {"schema": "capsule.usage/1", "game": "starter_battle", "source": "script",
     "used": {"script.op": {"Msg": 12, "End": 4}, "script.native": {"give_item": 1}}}

The VM bank tools' records (capsule.vm-usage/1, tools/vm_ext.py) are read too: a bank's ops become the category
`vm.<vm>` (`vm.<vm>.<table>` for another command table than the main one).

    usage.py merge RECORD... --game NAME --out REPORT.json [--platform P] [--libraries L...] [--hal SOURCE...]
    usage.py show REPORT.json [--json]
    usage.py linkmap REPORT.json MAP|NM [--json] [--out FILE]
    usage.py memory NM [--project project.json] [--stack-reserve BYTES] [--game NAME] [--out FILE] [--json] [--check]
                       [--demangler C++FILT]

`merge` writes the game's **usage report** (capsule.usage-report/1): every category summed over the records, the
records it read, the engine libraries the project links, the platform and its HAL sources, and per engine module
(the folders of src/) whether anything in the report uses it. `show` prints a report. `linkmap` reads a GNU ld map
(the GBA build's capsule_<game>.map) or a symbol listing (its capsule_<game>.nm, `nm -S --defined-only`: exact under
link-time optimization, where the map loses most symbols), sums the bytes each engine module links (by the symbols'
mangled namespaces) and lists the modules that link code while the report says nothing uses them
(capsule.usage-linkmap/1). `memory` reads a GBA program's listing and prints what each owner (its first two
namespaces) holds in IWRAM and EWRAM, checked against the regions and the game's budgets (tools/usage_memory.py,
capsule.usage-memory/1); with --check it fails when one is passed, naming the largest contributors. Generated files
are written with LF line ends and only when their bytes change.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections.abc import Iterable, Iterator, Mapping, Sequence
from contextlib import contextmanager
from pathlib import Path, PurePosixPath
from typing import Any

## The usage record schema.
RECORD_SCHEMA = "capsule.usage/1"
## The VM bank tools' usage record schema (tools/vm_ext.py USAGE_SCHEMA).
VM_RECORD_SCHEMA = "capsule.vm-usage/1"
## The merged report schema.
REPORT_SCHEMA = "capsule.usage-report/1"
## The link map comparison schema.
LINKMAP_SCHEMA = "capsule.usage-linkmap/1"
## A VM's main command table (tools/vm_ext.py MAIN_TABLE).
VM_MAIN_TABLE = "main"
## The prefix of the categories made from VM bank records.
VM_CATEGORY = "vm"
## The tools' folder (memory_command() imports its siblings from it; the compilers that import this module need not).
TOOLS = Path(__file__).resolve().parent
## The memory command's default stack reserve (tools/usage_memory.py DEFAULT_STACK_RESERVE; CMake's
## CAPSULE_GBA_STACK_RESERVE).
DEFAULT_STACK_RESERVE = 2048
## The engine's source folder (each sub folder is one engine module).
ENGINE_SOURCES = Path(__file__).resolve().parent.parent / "src"
## Category prefix -> the engine module that runs what the category lists (the longest matching prefix wins).
CATEGORY_MODULES = {
   "script": "script", "asset": "asset", "map.map": "map", "map.event": "field", "map.object": "overworld",
   "route": "overworld", "map.encounter": "encounter", "map.feature": "map", "seq": "seq", "audio": "audio",
   "effect": "battle", "battle": "battle", "vm.battle": "battle", "vm.anim": "anim", "text": "text",
   "screen": "screen",
}
## Entries another engine module runs than their category's: category -> name -> module (asset kinds another
## module draws, script ops of the gameplay modules, battle data parts the gameplay modules hold).
NAME_MODULES = {
   "asset.kind": {"font": "text", "text_box": "text", "tiles": "screen", "tilemap": "screen", "palette": "screen",
                  "door": "field"},
   "script.op": {"ItemGive": "inventory", "ItemTake": "inventory", "ItemCount": "inventory", "BagOpen": "inventory",
                 "Shop": "inventory", "BoxCount": "storage", "BoxDeposit": "storage", "BoxWithdraw": "storage",
                 "BoxesOpen": "storage", "Combine": "combine", "Nursery": "combine", "Encounters": "encounter",
                 "RoleGet": "monster", "RoleSet": "monster", "Transform": "monster"},
   "battle.feature": {"species": "monster", "growth": "growth", "items": "inventory", "inventory": "inventory",
                      "storage": "storage", "combine": "combine"},
}
## Engine modules every game links (start-up, the platform layer, graphics, input): never reported as unused.
BASE_MODULES = ("core", "hal", "gfx", "input")
## The module of a linked object that is no engine source (the game's own code, generated tables).
GAME_MODULE = "game"
## The module of linked code from outside any object file (the toolchain's libraries, linker stubs).
TOOLCHAIN_MODULE = "toolchain"
## The module of a listed symbol outside the engine's namespace (the game's code, the toolchain's libraries).
OTHER_MODULE = "other"
## A line of `nm -S --defined-only`: address, size, kind, name (symbols without a size are skipped).
NM_LINE = re.compile(r"^(?P<addr>[0-9a-fA-F]+)\s+(?P<size>[0-9a-fA-F]+)\s+(?P<kind>\S)\s+(?P<name>\S+)$")
## nm kinds that take no memory of their own (absolute symbols, debugging, weak references to nothing).
NM_SKIPPED_KINDS = frozenset("aANnUwv-")
## Exit status for a bad command line.
EXIT_USAGE = 2
## JSON indent of the written files.
JSON_INDENT = 1
## Hex base.
HEX = 16
## Sections of a GNU ld map whose bytes count as linked code and data (ROM and RAM alike).
LINKED_SECTIONS = re.compile(r"^\.(text|rodata|data|bss|iwram|ewram|sbss|sdata)\b")
## A map line with a section, an address, a size and an input file (the section may be on the line before).
MAP_LINE = re.compile(r"^ (?P<section>\.\S+)?\s+0x(?P<addr>[0-9a-fA-F]+)\s+0x(?P<size>[0-9a-fA-F]+)\s+(?P<file>\S.*)$")
## A symbol line of a GNU ld map: an address and a name, under its input section.
MAP_SYMBOL = re.compile(r"^\s{2,}0x(?P<addr>[0-9a-fA-F]+)\s+(?P<name>[^\s0][^=]*)$")
## The engine's namespace in a mangled name: `N7capsule` and the length of the next name (`_ZN7capsule6script...`).
ENGINE_NAMESPACE = re.compile(r"N7capsule(?P<length>\d+)")
## Engine namespaces that live in a module of another name.
NAMESPACE_MODULES = {"rng": "core", "clock": "core", "rules": "battle", "video": "gfx"}
## The largest items a link map comparison lists per module.
LISTED_ITEMS = 20
## The items the text output names per module.
LISTED_IN_TEXT = 5
## A section name alone on its line (its address, size and file follow on the next).
MAP_SECTION_ONLY = re.compile(r"^ (?P<section>\.\S+)\s*$")
## The heading after which a GNU ld map lists the placed sections.
MAP_START = "Linker script and memory map"
## An archive member in a map's input file: `path/libx.a(member.o)`.
ARCHIVE_MEMBER = re.compile(r"\((?P<member>[^()]+)\)$")
## Object file suffixes CMake and compilers give (longest first).
OBJECT_SUFFIXES = (".cpp.obj", ".cpp.o", ".c.obj", ".c.o", ".s.obj", ".s.o", ".obj", ".o")
## Suffixes of the engine's source files.
SOURCE_SUFFIXES = (".cpp", ".c", ".s")
## The engine library archives' name prefix (libcapsule_engine.a and the engine's kits, libcapsule_test_kit.a, ...).
ENGINE_ARCHIVE_PREFIX = "libcapsule_"
## The engine's source folder's name in an object's path (a runtime object compiled into the game).
ENGINE_SOURCE_FOLDER = "src"


class UsageError(Exception):
   """!A record or report the tool cannot read."""


class Collector:
   """!What the compile in progress used, noted by the compilers' shared parts (routes, effect programs) that run
   inside another compiler (collecting())."""

   def __init__(self) -> None:
      """!Empty."""
      ## Category -> name -> count.
      self.used = {}   # type: dict[str, dict[str, int]]


## The collectors of the compiles in progress (the innermost last).
_ACTIVE: list[Collector] = []


@contextmanager
def collecting() -> Iterator[Collector]:
   """!Collects what note() records while the block runs (nothing is noted outside such a block).
   @return the collector, filled when the block ends
   """
   collector = Collector()
   _ACTIVE.append(collector)
   try:
      yield collector
   finally:
      _ACTIVE.remove(collector)


def note(category: str, name: str, n: int = 1) -> None:
   """!Notes one use for the compile in progress (no effect when nothing collects).
   @param category  the category
   @param name      the name
   @param n         how many
   @return None
   """
   if _ACTIVE:
      count(_ACTIVE[-1].used, category, name, n)


def write_if_changed(path: Path, text: str) -> None:
   """!Writes a file only when its bytes change (LF line ends).
   @param path  the file (its folder is created)
   @param text  the content
   @return None
   """
   path.parent.mkdir(parents=True, exist_ok=True)
   data = text.encode("utf-8")
   if not path.exists() or path.read_bytes() != data:
      path.write_bytes(data)


def as_json(obj: Any) -> str:
   """!A JSON document as the tool writes it.
   @param obj  the object
   @return its text with a final newline
   """
   return json.dumps(obj, indent=JSON_INDENT, ensure_ascii=False) + "\n"


def sorted_counts(counts: Mapping[str, int]) -> dict[str, int]:
   """!Counts with their names sorted.
   @param counts  name -> count
   @return the same, sorted by name
   """
   return {k: int(counts[k]) for k in sorted(counts)}


def record(game: str, source: str, used: Mapping[str, Mapping[str, int]]) -> dict[str, Any]:
   """!A usage record (capsule.usage/1).
   @param game    the game's name
   @param source  the compiler that writes it (script, asset, map, seq, audio, battle, ...)
   @param used    category -> name -> count (empty categories are left out)
   @return the record
   """
   return {"schema": RECORD_SCHEMA, "game": game, "source": source,
           "used": {c: sorted_counts(used[c]) for c in sorted(used) if used[c]}}


def write_record(path: str | Path, game: str, source: str, used: Mapping[str, Mapping[str, int]]) -> None:
   """!Writes a usage record (only when it changes).
   @param path    the file
   @param game    the game's name
   @param source  the compiler
   @param used    category -> name -> count
   @return None
   """
   write_if_changed(Path(path), as_json(record(game, source, used)))


def game_of(path: str | Path) -> str:
   """!The game a record belongs to, from its path (the build writes `usage/<game>/<compiler>.json`).
   @param path  the record's path
   @return the name of the record's folder
   """
   return Path(path).resolve().parent.name


def count(into: dict[str, dict[str, int]], category: str, name: str, n: int = 1) -> None:
   """!Adds to one count.
   @param into      category -> name -> count
   @param category  the category
   @param name      the name
   @param n         how much
   @return None
   """
   names = into.setdefault(category, {})
   names[name] = names.get(name, 0) + n


def vm_category(vm: str, table: str) -> str:
   """!The category of a VM bank record's ops.
   @param vm     the VM
   @param table  its command table
   @return `vm.<vm>`, or `vm.<vm>.<table>` for another table than the main one
   """
   return f"{VM_CATEGORY}.{vm}" if table == VM_MAIN_TABLE else f"{VM_CATEGORY}.{vm}.{table}"


def read_record(path: Path) -> tuple[str, dict[str, dict[str, int]]]:
   """!Reads one record of either schema.
   @param path  the file
   @return (its source, category -> name -> count)
   @throws UsageError on another schema or a malformed record
   """
   try:
      raw = json.loads(path.read_text(encoding="utf-8"))
   except (OSError, json.JSONDecodeError) as e:
      raise UsageError(f"{path}: {e}") from None
   schema = raw.get("schema") if isinstance(raw, dict) else None
   if schema == RECORD_SCHEMA:
      used = raw.get("used", {})
      if not isinstance(used, dict) or not all(isinstance(v, dict) for v in used.values()):
         raise UsageError(f"{path}: \"used\" must map categories to counts")
      return str(raw.get("source", path.stem)), {c: {k: int(n) for k, n in v.items()} for c, v in used.items()}
   if schema == VM_RECORD_SCHEMA:
      category = vm_category(str(raw.get("vm", "")), str(raw.get("table", VM_MAIN_TABLE)))
      ops = raw.get("ops", {})
      return f"bank:{raw.get('bank', path.stem)}", {category: {k: int(n) for k, n in ops.items()}}
   raise UsageError(f"{path}: schema must be {RECORD_SCHEMA} or {VM_RECORD_SCHEMA}")


def engine_modules(sources: Path = ENGINE_SOURCES) -> list[str]:
   """!The engine's modules: the folders of src/.
   @param sources  the engine's src/ folder
   @return their names, sorted
   """
   return sorted(p.name for p in sources.iterdir() if p.is_dir()) if sources.is_dir() else []


def module_of(category: str, name: str) -> str | None:
   """!The engine module that runs what one category entry names.
   @param category  the category
   @param name      the entry
   @return the module, or None when no module is known for the category
   """
   if name in NAME_MODULES.get(category, {}):
      return NAME_MODULES[category][name]
   best = ""
   for prefix in CATEGORY_MODULES:
      if (category == prefix or category.startswith(prefix + ".")) and len(prefix) > len(best):
         best = prefix
   return CATEGORY_MODULES[best] if best else None


def merge(paths: Sequence[Path], game: str, platform: str = "", libraries: Sequence[str] = (),
          hal: Sequence[str] = (), sources: Path = ENGINE_SOURCES) -> dict[str, Any]:
   """!The merged usage report of a game (capsule.usage-report/1).
   @param paths      its records
   @param game       the game
   @param platform   the build's platform
   @param libraries  the engine libraries the project links (project.json "libraries")
   @param hal        the platform's HAL sources in the engine library (relative to the engine)
   @param sources    the engine's src/ folder (its modules)
   @return the report
   @throws UsageError when a record does not read
   """
   used: dict[str, dict[str, int]] = {}
   read: list[dict[str, Any]] = []
   by_module: dict[str, set[str]] = {}
   for path in sorted(paths, key=lambda p: Path(p).as_posix()):
      source, categories = read_record(Path(path))
      read.append({"file": Path(path).name, "source": source})
      for category, names in categories.items():
         for name, n in names.items():
            count(used, category, name, n)
            module = module_of(category, name)
            if module is not None and n > 0:
               by_module.setdefault(module, set()).add(category)
   modules: dict[str, Any] = {}
   for module in engine_modules(sources):
      if module in BASE_MODULES:
         modules[module] = {"used": True, "by": ["base"]}
      else:
         by = sorted(by_module.get(module, set()))
         modules[module] = {"used": bool(by), "by": by}
   return {"schema": REPORT_SCHEMA, "game": game, "platform": platform, "records": read,
           "libraries": sorted(set(libraries)), "hal": sorted({PurePosixPath(h).as_posix() for h in hal}),
           "modules": modules, "used": {c: sorted_counts(used[c]) for c in sorted(used)}}


def read_report(path: Path) -> dict[str, Any]:
   """!Reads a merged report.
   @param path  the file
   @return the report
   @throws UsageError when it is not one
   """
   try:
      raw = json.loads(path.read_text(encoding="utf-8"))
   except (OSError, json.JSONDecodeError) as e:
      raise UsageError(f"{path}: {e}") from None
   if not isinstance(raw, dict) or raw.get("schema") != REPORT_SCHEMA:
      raise UsageError(f"{path}: schema must be {REPORT_SCHEMA}")
   return raw


def summary(report: Mapping[str, Any]) -> str:
   """!A report as text: the modules, then each category with its names.
   @param report  the report
   @return the text
   """
   lines = [f"Usage of {report.get('game', '?')} ({report.get('platform') or 'any platform'}): "
            f"{len(report.get('records', []))} records"]
   modules = report.get("modules", {})
   used = [m for m, v in modules.items() if v.get("used")]
   unused = [m for m, v in modules.items() if not v.get("used")]
   lines.append(f"Engine modules used: {', '.join(used) or 'none'}")
   lines.append(f"Engine modules nothing uses: {', '.join(unused) or 'none'}")
   if report.get("libraries"):
      lines.append(f"Libraries: {', '.join(report['libraries'])}")
   if report.get("hal"):
      lines.append(f"HAL sources: {', '.join(report['hal'])}")
   for category, names in report.get("used", {}).items():
      total = sum(names.values())
      lines.append(f"{category}: {len(names)} used, {total} uses")
      lines.append("   " + ", ".join(f"{k} {v}" for k, v in names.items()))
   return "\n".join(lines) + "\n"


def input_label(file: str) -> str:
   """!A map line's input file as the report names it: `libx.a(member.o)` for an archive's member, else the object
   file's path with `/` separators.
   @param file  the input file text
   @return the label
   """
   text = file.strip().replace("\\", "/")
   m = ARCHIVE_MEMBER.search(text)
   if m is not None:
      return f"{PurePosixPath(text[:m.start()]).name}({m['member']})"
   return text


def source_stem(obj: str) -> str:
   """!An object file's source file name (`vm.cpp.obj` -> `vm.cpp`, `vm.o` -> `vm`).
   @param obj  the object's file name
   @return the name without the object suffix
   """
   for suffix in OBJECT_SUFFIXES:
      if obj.endswith(suffix):
         return obj[:-len(suffix[suffix.rindex('.'):])]   # the object part only (`.obj` of `.cpp.obj`)
   return obj


def source_modules(sources: Path = ENGINE_SOURCES) -> dict[str, list[str]]:
   """!Each engine source file's name -> the modules that hold a file of that name.
   @param sources  the engine's src/ folder
   @return file name (with and without its suffix) -> modules
   """
   out: dict[str, list[str]] = {}
   if not sources.is_dir():
      return out
   for path in sources.rglob("*"):
      if path.suffix in SOURCE_SUFFIXES and path.is_file():
         module = path.relative_to(sources).parts[0]
         for key in (path.name, path.stem):
            if module not in out.setdefault(key, []):
               out[key].append(module)
   return out


def module_of_input(label: str, index: Mapping[str, list[str]], modules: Iterable[str]) -> str:
   """!The engine module (or GAME_MODULE / TOOLCHAIN_MODULE) a linked input belongs to.
   @param label    input_label()'s text
   @param index    source_modules()
   @param modules  the engine's modules
   @return the module; modules sharing a file name are joined with `|`
   """
   m = ARCHIVE_MEMBER.search(label)
   if m is not None:
      archive = label[:m.start()]
      if not archive.startswith(ENGINE_ARCHIVE_PREFIX):
         return TOOLCHAIN_MODULE
      owners = index.get(source_stem(m["member"])) or index.get(PurePosixPath(source_stem(m["member"])).stem) or []
      return "|".join(sorted(owners)) if owners else GAME_MODULE
   parts = PurePosixPath(label).parts
   for i, part in enumerate(parts[:-1]):   # an engine source compiled into the game (its runtime objects)
      if part == ENGINE_SOURCE_FOLDER and parts[i + 1] in modules:
         return parts[i + 1]
   return GAME_MODULE


def symbol_module(name: str, modules: Iterable[str]) -> str | None:
   """!The engine module of a linked symbol or input section, from its mangled namespace (`capsule::<module>::`).
   @param name     the symbol (or a section name such as `.text._ZN7capsule6script...`)
   @param modules  the engine's modules
   @return the module, or None when the name holds no known engine namespace
   """
   m = ENGINE_NAMESPACE.search(name)
   if m is None:
      return None
   length = int(m["length"])
   namespace = name[m.end():m.end() + length]
   namespace = NAMESPACE_MODULES.get(namespace, namespace)
   return namespace if namespace in modules else None


class LinkedSpan:
   """!One input section of a map, with the symbols placed in it."""

   def __init__(self, section: str, addr: int, size: int, label: str) -> None:
      """!An input section.
      @param section  its name
      @param addr     its address
      @param size     its bytes
      @param label    input_label() of its file
      """
      ## The section's name.
      self.section = section
      ## Its address.
      self.addr = addr
      ## Its bytes.
      self.size = size
      ## Its input file.
      self.label = label
      ## (address, name) of the symbols in it.
      self.symbols = []   # type: list[tuple[int, str]]


def map_spans(text: str) -> list[LinkedSpan]:
   """!The input sections a GNU ld map places (after MAP_START), with their symbols.
   @param text  the map
   @return the linked code and data sections (LINKED_SECTIONS) of non-zero size
   """
   spans: list[LinkedSpan] = []
   lines = text.splitlines()
   start = next((i for i, line in enumerate(lines) if line.startswith(MAP_START)), None)
   if start is None:
      return spans
   pending = ""
   current: LinkedSpan | None = None
   for line in lines[start + 1:]:
      only = MAP_SECTION_ONLY.match(line)
      if only is not None:
         pending = only["section"]
         continue
      m = MAP_LINE.match(line)
      if m is not None:
         section = m["section"] or pending
         pending = ""
         file = m["file"].strip()
         size = int(m["size"], HEX)
         current = None
         if section and LINKED_SECTIONS.match(section) and size and not file.startswith(("*", "0x")) \
               and "=" not in file:
            current = LinkedSpan(section, int(m["addr"], HEX), size, input_label(file))
            spans.append(current)
         continue
      pending = ""
      symbol = MAP_SYMBOL.match(line)
      if symbol is not None and current is not None and "=" not in symbol["name"]:
         addr = int(symbol["addr"], HEX)
         if current.addr <= addr < current.addr + current.size:
            current.symbols.append((addr, symbol["name"].strip()))
   return spans


def link_usage(text: str, sources: Path = ENGINE_SOURCES) -> dict[str, dict[str, Any]]:
   """!Bytes each engine module links, from a GNU ld map. Each symbol's bytes (up to the next symbol or its section's
   end) go to the module of its mangled namespace, else the section name's, else its input file's (an engine
   library member, a runtime object compiled from src/, the game's own objects, the toolchain's libraries); this
   works with link-time optimization too, where an input file holds code of many modules.
   @param text     the map
   @param sources  the engine's src/ folder
   @return module (GAME_MODULE, TOOLCHAIN_MODULE too) -> {"bytes", "items": symbol or input -> bytes}
   """
   index = source_modules(sources)
   names = engine_modules(sources)
   out: dict[str, dict[str, Any]] = {}

   def add(module: str, item: str, size: int) -> None:
      """!Counts bytes for a module."""
      entry = out.setdefault(module, {"bytes": 0, "items": {}})
      entry["bytes"] += size
      entry["items"][item] = entry["items"].get(item, 0) + size

   for span in map_spans(text):
      fallback = symbol_module(span.section, names) or module_of_input(span.label, index, names)
      marks = sorted(span.symbols)
      first = marks[0][0] if marks else span.addr + span.size
      if first > span.addr:   # bytes before the first symbol (a section without global symbols)
         add(fallback, f"{span.label} {span.section}", first - span.addr)
      for i, (addr, name) in enumerate(marks):
         end = marks[i + 1][0] if i + 1 < len(marks) else span.addr + span.size
         if end > addr:
            add(symbol_module(name, names) or fallback, name, end - addr)
   return out


def nm_usage(text: str, sources: Path = ENGINE_SOURCES) -> dict[str, dict[str, Any]]:
   """!Bytes each engine module links, from a symbol listing (`nm -S --defined-only`, mangled names: the GBA build
   writes capsule_<game>.nm beside the map). Every sized symbol, local ones included, goes to the module of its
   mangled namespace, else OTHER_MODULE (the game's own code, the toolchain's libraries); unlike the map, this keeps
   working under link-time optimization.
   @param text     the listing
   @param sources  the engine's src/ folder
   @return module -> {"bytes", "items": symbol -> bytes}
   """
   names = engine_modules(sources)
   out: dict[str, dict[str, Any]] = {}
   for line in text.splitlines():
      m = NM_LINE.match(line)
      if m is None or m["kind"] in NM_SKIPPED_KINDS:
         continue
      size = int(m["size"], HEX)
      module = symbol_module(m["name"], names) or OTHER_MODULE
      entry = out.setdefault(module, {"bytes": 0, "items": {}})
      entry["bytes"] += size
      entry["items"][m["name"]] = entry["items"].get(m["name"], 0) + size
   return out


def read_linked(text: str, sources: Path = ENGINE_SOURCES) -> dict[str, dict[str, Any]]:
   """!Bytes each engine module links, from either a GNU ld map or an nm listing (told apart by the map's heading).
   @param text     the file
   @param sources  the engine's src/ folder
   @return link_usage() or nm_usage()
   """
   return link_usage(text, sources) if MAP_START in text else nm_usage(text, sources)


def compare(report: Mapping[str, Any], linked: Mapping[str, Mapping[str, Any]],
            listed: int = LISTED_ITEMS) -> dict[str, Any]:
   """!A report against what the link kept (capsule.usage-linkmap/1).
   @param report  the merged report
   @param linked  link_usage()'s modules
   @param listed  the largest items listed per module
   @return per module its linked bytes, whether the report uses it and its largest items; the modules that link
           code the report says nothing uses, largest first
   """
   used = report.get("modules", {})
   modules: dict[str, dict[str, Any]] = {}
   for module, entry in linked.items():
      items = sorted(entry["items"].items(), key=lambda kv: (-kv[1], kv[0]))
      modules[module] = {"bytes": entry["bytes"],
                         "used": module in (GAME_MODULE, TOOLCHAIN_MODULE, OTHER_MODULE) or
                         used.get(module, {}).get("used", False),
                         "items": dict(items[:listed])}
   unused = sorted((m for m, e in modules.items() if not e["used"]), key=lambda m: (-modules[m]["bytes"], m))
   return {"schema": LINKMAP_SCHEMA, "game": report.get("game", ""),
           "linked_bytes": sum(e["bytes"] for e in modules.values()),
           "unused_linked_bytes": sum(modules[m]["bytes"] for m in unused),
           "unused_linked": unused, "modules": dict(sorted(modules.items()))}


def linkmap_text(result: Mapping[str, Any]) -> str:
   """!A link map comparison as text.
   @param result  compare()'s object
   @return the text
   """
   lines = [f"Link map of {result['game']}: {result['linked_bytes']} B linked, {result['unused_linked_bytes']} B in "
            f"modules the usage report says nothing uses"]
   for module in result["unused_linked"]:
      entry = result["modules"][module]
      largest = ", ".join(list(entry["items"])[:LISTED_IN_TEXT])
      lines.append(f"   {module}: {entry['bytes']} B (largest: {largest})")
   return "\n".join(lines) + "\n"


def merge_records(records: Iterable[Mapping[str, Mapping[str, int]]]) -> dict[str, dict[str, int]]:
   """!Sums several compilers' categories (a compiler that compiles parts of others, e.g. the map compiler's
   scripts, adds theirs to its own).
   @param records  category -> name -> count, each
   @return the sum
   """
   out: dict[str, dict[str, int]] = {}
   for used in records:
      for category, names in used.items():
         for name, n in names.items():
            count(out, category, name, n)
   return out


def memory_command(args: argparse.Namespace) -> int:
   """!The `memory` command (tools/usage_memory.py).
   @param args  its arguments
   @return 1 with --check when a region or a budget is passed, else 0
   @throws UsageError when the budgets are bad; OSError when a file cannot be read or written
   """
   if str(TOOLS) not in sys.path:
      sys.path.insert(0, str(TOOLS))
   import build_settings
   import usage_memory
   budgets = None
   if args.project is not None:
      try:
         budgets = build_settings.load_memory_budgets(args.project)
      except build_settings.SettingsError as e:
         raise UsageError(f"{args.project}: {e}") from None
   text = args.listing.read_text(encoding="utf-8", errors="replace")
   result = usage_memory.report(text, budgets, args.stack_reserve, args.game, args.demangler)
   if args.out is not None:
      args.out.parent.mkdir(parents=True, exist_ok=True)
      write_if_changed(args.out, as_json(result))
   sys.stdout.write(as_json(result) if args.json else usage_memory.report_text(result))
   return 1 if args.check and result["over"] else 0


def main(argv: Sequence[str]) -> int:
   """!Command line; see the module docstring.
   @param argv  the arguments (without the program)
   @return the exit status
   """
   ap = argparse.ArgumentParser(prog="usage.py", description="Usage records and reports (capsule.usage/1).")
   sub = ap.add_subparsers(dest="command", required=True)
   m = sub.add_parser("merge", help="merge a game's records into its report")
   m.add_argument("records", nargs="*", type=Path)
   m.add_argument("--game", required=True)
   m.add_argument("--out", required=True, type=Path)
   m.add_argument("--platform", default="")
   m.add_argument("--libraries", nargs="*", default=[])
   m.add_argument("--hal", nargs="*", default=[])
   s = sub.add_parser("show", help="print a report")
   s.add_argument("report", type=Path)
   s.add_argument("--json", action="store_true")
   lm = sub.add_parser("linkmap", help="compare a report with a GNU ld map")
   lm.add_argument("report", type=Path)
   lm.add_argument("map", type=Path)
   lm.add_argument("--json", action="store_true")
   lm.add_argument("--out", type=Path)
   me = sub.add_parser("memory", help="a GBA program's RAM by owner, checked against its regions and budgets")
   me.add_argument("listing", type=Path, help="the program's `nm -S --defined-only` listing (capsule_<game>.nm)")
   me.add_argument("--project", type=Path, help="the game's reference/project.json (its build memory_budgets)")
   me.add_argument("--stack-reserve", type=int, default=DEFAULT_STACK_RESERVE,
                   help="IWRAM bytes kept for the stack (CAPSULE_GBA_STACK_RESERVE)")
   me.add_argument("--game", default="")
   me.add_argument("--out", type=Path, help="write the report (capsule.usage-memory/1) here")
   me.add_argument("--json", action="store_true")
   me.add_argument("--check", action="store_true", help="exit 1 when a region or a budget is passed")
   me.add_argument("--demangler", help="a c++filt for readable names")
   try:
      args = ap.parse_args(list(argv))
   except SystemExit as e:
      return int(e.code) if isinstance(e.code, int) else EXIT_USAGE
   try:
      if args.command == "merge":
         report = merge([p for p in args.records if p.exists()], args.game, args.platform, args.libraries, args.hal)
         write_if_changed(args.out, as_json(report))
      elif args.command == "show":
         report = read_report(args.report)
         sys.stdout.write(as_json(report) if args.json else summary(report))
      elif args.command == "memory":
         return memory_command(args)
      else:
         report = read_report(args.report)
         result = compare(report, read_linked(args.map.read_text(encoding="utf-8", errors="replace")))
         if args.out is not None:
            write_if_changed(args.out, as_json(result))
         sys.stdout.write(as_json(result) if args.json else linkmap_text(result))
   except (UsageError, OSError) as e:
      print(f"error: {e}", file=sys.stderr)
      return 1
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv[1:]))
