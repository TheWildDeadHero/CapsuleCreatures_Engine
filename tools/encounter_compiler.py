#!/usr/bin/env python3
"""!Compile a world's encounter tables (C-03c, C-03d, C-03e, D-12).

Called by tools/map_compiler.py, which emits the result with the world
(capsule::map::kWorld.encounters). Run alone, it checks a world's
encounters and prints a summary:

    encounter_compiler.py <world.json> [--ids <out.h>]

With --ids it also writes the identifiers game code uses for methods and
conditions (ENCOUNTER_METHOD_..., ENCOUNTER_CONDITION_...); the map
compiler takes the same option.

World manifest keys (capsule.world/1):
  - battle: the capsule.battle/1 manifest whose species encounters name
    (relative to the world file; levels are checked against rules.level_max
    of the structure manifest it names). Needed once any map has encounters.
  - encounter_methods: up to 16 methods {"name", "trigger": "step" |
    "script", "vehicles": [vehicle classes]}. Step methods roll when the
    walker arrives on an encounter cell while in one of their vehicle
    classes (default: every class); the first matching step method with a
    table for the cell's slot rolls. Script methods (one per tool or
    interaction, ...) roll when game code asks. Default: one method "step"
    (trigger step, every class).
  - encounter_conditions: up to 32 condition names (time of day, weather,
    swarms, ...); game code sets their bits at run time.
  - encounter_tables: a capsule.encounters/1 file of shared tables, each
    {"name", "rate", "entries"}, that maps refer to by name.

Map manifest key (capsule.map/2):
  - encounters: the map's capsule.encounters/1 file (relative to the map
    file). Its tables are {"slot": 0-15, "method", "rate", "entries"}, or
    {"slot", "method", "table": shared name, "rate" (optional override)}.
    One table per slot and method. Encounter cells pick their slot with
    the cell's slot field (bits 23-26).

Tables:
  - rate: the odds that a trigger starts an encounter (P-5): "1/144",
    "12.5%", [n, m] (m up to 65535) or 1 (always).
  - entries: up to 64 (none: the table never starts an encounter) {"species", "levels": level or [lowest, highest],
    "weight": 1-65535, "requires": [conditions], "excludes": [conditions],
    "modifiers": [{"when": condition, "multiply": 3.0 | "1/2" | 0}]}
    (up to 8 modifiers; multipliers compile to Q8, P-4).

Identical tables are stored once (shared tables and equal inline tables
alike). Species resolve through the battle manifest's species keys (Asset
Key v1, as the battle compiler sorts them), levels against its
rules.level_max.
"""
from __future__ import annotations

import json
import re
import sys
from collections.abc import Mapping, Sequence
from fractions import Fraction
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from asset_compiler import strip  # noqa: E402
from sequence_compiler import ManifestError as SequenceManifestError  # noqa: E402
from sequence_compiler import asset_key  # noqa: E402

## Schema of encounter files.
SCHEMA = "capsule.encounters/1"
## Encounter slots per map (4-bit cell field).
MAX_SLOTS = 16
## Methods per world.
MAX_METHODS = 16
## Condition bits per world.
MAX_CONDITIONS = 32
## Entries per table.
MAX_ENTRIES = 64
## Modifiers per entry.
MAX_MODIFIERS = 8
## Largest Q8 multiplier (x127.99).
MAX_Q8 = 0x7FFF
## Largest odds denominator (P-5).
MAX_RATE_M = 65535
## Triggers, in capsule::encounter::Trigger order.
TRIGGERS = ["step", "script"]
## Largest denominator a decimal (float) number is approximated with.
FLOAT_DENOMINATOR_LIMIT = 65535
## A percentage's denominator.
PERCENT = 100
## Numbers in a rate pair [n, m] and in a level range [lowest, highest].
PAIR = 2
## Q8 fixed point: the value of x1.
Q8_ONE = 256
## Q8 fixed point: bits of the fraction.
Q8_SHIFT = 8
## Halves per unit: doubling, adding one and halving again rounds to the nearest.
HALVES = 2
## Highest level when the battle manifest's rules name none (the battle compiler's neutral default: the engine's
## 8-bit cap, D-22 decision 8).
DEFAULT_LEVEL_MAX = 255
## Lowest rules.level_max.
MIN_LEVEL_MAX = 2
## Highest rules.level_max (levels are 8-bit).
MAX_LEVEL_MAX = 255
## Largest entry weight (a u16).
MAX_WEIGHT = 65535
## A table's weights times multipliers must stay below this (they are summed in a u32).
WEIGHT_SUM_LIMIT = 1 << 32
## Distinct tables per world (16-bit indices; 0xFFFF is reserved).
MAX_TABLES = 0xFFFE
## Entries per world (16-bit indices).
MAX_TOTAL_ENTRIES = 0xFFFF
## Exit status of a command line that is not understood.
EXIT_USAGE = 2
## Command-line words of an option with a value ("--ids" and the file).
OPTION_WORDS = 2

## A parsed JSON value.
Json = Any
## In-memory manifests replacing files (resolved Path -> parsed JSON), as editors pass them.
Sources = Mapping[Path, Json]
## A compiled entry: (species, lowest level, highest level, weight, required, excluded, ((condition, q8), ...)).
Entry = tuple[int, int, int, int, int, int, tuple[tuple[int, int], ...]]

## Methods of a world that names none.
DEFAULT_METHODS = [{"name": "step", "trigger": "step"}]
## Allowed keys per object.
KEYS = {
   "method": {"name", "trigger", "vehicles"},
   "file": {"schema", "id", "tables"},
   "map_table": {"slot", "method", "rate", "entries", "table"},
   "shared_table": {"name", "rate", "entries"},
   "entry": {"species", "levels", "weight", "requires", "excludes", "modifiers"},
   "modifier": {"when", "multiply"},
}


class ManifestError(Exception):
   """!An encounter rule was broken; the message says where and how."""
   pass


def check_keys(obj: Json, kind: str, where: str) -> None:
   """!Rejects unknown keys (and non-objects).
   @param obj    the object
   @param kind   key set name in KEYS
   @param where  location for error messages
   @return None
   """
   if not isinstance(obj, dict):
      raise ManifestError(f"{where}: expected an object")
   unknown = sorted(set(obj) - KEYS[kind])
   if unknown:
      raise ManifestError(f"{where}: unknown key(s) {', '.join(unknown)}")


def integer(value: Json, lo: int, hi: int, where: str) -> int:
   """!Checks an integer range.
   @param value  the value
   @param lo     minimum
   @param hi     maximum
   @param where  location
   @return the value
   """
   if not isinstance(value, int) or isinstance(value, bool) or not lo <= value <= hi:
      raise ManifestError(f"{where}: expected an integer {lo}-{hi}, got {value!r}")
   return value


def fraction(v: Json, where: str) -> Fraction:
   """!Parses a non-negative number: integer, decimal, "n/m" or "p%".
   @param v      value
   @param where  location
   @return Fraction
   """
   if isinstance(v, bool):
      raise ManifestError(f"{where}: expected a number")
   if isinstance(v, int):
      f = Fraction(v)
   elif isinstance(v, float):
      f = Fraction(v).limit_denominator(FLOAT_DENOMINATOR_LIMIT)
   elif isinstance(v, str):
      m = re.fullmatch(r"\s*(?P<n>\d+)\s*/\s*(?P<d>\d+)\s*", v)
      p = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*%\s*", v)
      if m and int(m.group("d")) > 0:
         f = Fraction(int(m.group("n")), int(m.group("d")))
      elif p:
         f = Fraction(p.group(1)) / PERCENT
      else:
         raise ManifestError(f"{where}: expected a number like 3, 0.5, \"1/2\" or \"50%\", got {v!r}")
   else:
      raise ManifestError(f"{where}: expected a number, got {v!r}")
   if f < 0:
      raise ManifestError(f"{where}: must not be negative")
   return f


def rate(v: Json, where: str) -> tuple[int, int]:
   """!Trigger odds (P-5).
   @param v      "1/144", "12.5%", [n, m], 0.5 or 1
   @param where  location
   @return (n, m) with 0 <= n <= m <= MAX_RATE_M
   """
   if v is None:
      raise ManifestError(f"{where}: needs a rate (the odds a trigger starts an encounter, e.g. \"1/10\")")
   if isinstance(v, list):
      if len(v) != PAIR:
         raise ManifestError(f"{where}: rate [n, m] has two numbers")
      vn, vm = v
      n, m = integer(vn, 0, MAX_RATE_M, f"{where} rate"), integer(vm, 1, MAX_RATE_M, f"{where} rate")
   else:
      f = fraction(v, f"{where} rate")
      if f.denominator > MAX_RATE_M:
         f = f.limit_denominator(MAX_RATE_M)
      n, m = f.numerator, f.denominator
   if n > m:
      raise ManifestError(f"{where}: rate {v!r} is above 1")
   return n, m


def q8(v: Json, where: str) -> int:
   """!A multiplier as Q8 (rounded to nearest, P-4).
   @param v      value
   @param where  location
   @return 0 to MAX_Q8
   """
   f = fraction(v, where)

   # scale to Q8 and round half up: (2x + 1) // 2
   r = int((f * Q8_ONE * HALVES + 1) // HALVES)
   if r > MAX_Q8:
      raise ManifestError(f"{where}: multiplier {v!r} is above {MAX_Q8 / Q8_ONE:.2f}")
   return r


def load_json(path: str | Path, where: str, sources: Sources | None = None) -> Json:
   """!Reads and strips a manifest file.
   @param path     file
   @param where    location
   @param sources  resolved Path -> parsed JSON replacing the file (editors), or None
   @return parsed JSON without editor data
   """
   path = Path(path).resolve()
   if sources is not None and path in sources:
      return strip(sources[path])
   try:
      return strip(json.loads(path.read_text(encoding="utf-8")))
   except OSError as e:
      raise ManifestError(f"{where}: cannot read {path}: {e.strerror}") from None
   except json.JSONDecodeError as e:
      raise ManifestError(f"{where}: {path}: {e}") from None


def ident(name: str) -> str:
   """!A C++ identifier tail for a name.
   @param name  any name
   @return UPPER_CASE with underscores
   """
   s = re.sub(r"[^0-9A-Za-z]+", "_", name).strip("_").upper()
   return s if s and not s[0].isdigit() else f"N{s}"


def key_of(ns: str, name: str, where: str) -> int:
   """!Asset Key v1 of a name.
   @param ns     namespace
   @param name   name
   @param where  location
   @return key
   """
   try:
      return asset_key(f"{ns}:{name}", where)
   except SequenceManifestError as e:
      raise ManifestError(str(e)) from None


# -- species ----------------------------------------------------------------------------------

class Species:
   """!The species of a battle manifest, indexed as the battle compiler does."""

   def __init__(self, names: Sequence[str], level_max: int, path: Path | None) -> None:
      """!Wraps the species list.
      @param names      species names (manifest order)
      @param level_max  the ruleset's highest level
      @param path       battle manifest, or None
      """
      keyed = sorted((key_of("species", n, f"battle species {n!r}"), n) for n in names)
      ## Species names in index order (sorted by key, like battle_compiler.py).
      self.names = [n for _, n in keyed]
      ## Key -> index.
      self.by_key = {k: i for i, (k, _) in enumerate(keyed)}
      ## Highest level.
      self.level_max = level_max
      ## Battle manifest path, or None.
      self.path = path

   def index(self, name: Json, where: str) -> int:
      """!A species index.
      @param name   species name (any case and spacing, as Asset Key v1 canonicalises)
      @param where  location
      @return index
      """
      if not isinstance(name, str) or not name.strip():
         raise ManifestError(f"{where}: species needs a name")
      k = key_of("species", name, where)
      if k not in self.by_key:
         raise ManifestError(f"{where}: unknown species {name!r}" + (
            f" (not in {self.path.name})" if self.path else ""))
      return self.by_key[k]


def load_species(path: str | Path, deps: list[Path] | None = None, sources: Sources | None = None) -> Species:
   """!Reads the species and level cap of a battle manifest (no battle compile).
   @param path     capsule.battle/1 file
   @param deps     list read files are appended to, or None
   @param sources  in-memory manifests, or None
   @return Species
   """
   from battlec.common import ManifestError as BattleManifestError
   from battlec.manifest import BATTLE_SCHEMA, read_battle
   path = Path(path).resolve()
   b = load_json(path, "world battle", sources)
   if not isinstance(b, dict) or b.get("schema") != BATTLE_SCHEMA:
      raise ManifestError(f"world battle: {path.name} is not a {BATTLE_SCHEMA} manifest")
   # the species are content (battle.json), rules.level_max is structure (the structure manifest it names, E5s)
   try:
      b, _st_path = read_battle(path, sources, deps)
   except BattleManifestError as e:
      raise ManifestError(f"world battle: {e}") from None
   names = [s.get("name") for s in b.get("species", []) if isinstance(s, dict)]
   if not all(isinstance(n, str) and n for n in names):
      raise ManifestError(f"world battle: {path.name}: every species needs a name")
   # the battle manifest's rules.level_max (the battle compiler reports a bad one; levels then check against the
   # default)
   level_max = DEFAULT_LEVEL_MAX
   rules = b.get("rules")
   lm = rules.get("level_max", DEFAULT_LEVEL_MAX) if isinstance(rules, dict) else DEFAULT_LEVEL_MAX
   if isinstance(lm, int) and not isinstance(lm, bool) and MIN_LEVEL_MAX <= lm <= MAX_LEVEL_MAX:
      level_max = lm
   return Species(names, level_max, path)


# -- world settings -----------------------------------------------------------------------------

def compile_methods(raw: Json, classes: Sequence[str]) -> list[dict[str, Any]]:
   """!The world's encounter methods.
   @param raw      the "encounter_methods" value, or None
   @param classes  vehicle class names
   @return list of {"name", "trigger" (index), "vehicles" (mask)}
   """
   raw = DEFAULT_METHODS if raw is None else raw
   if not isinstance(raw, list) or not 1 <= len(raw) <= MAX_METHODS:
      raise ManifestError(f"world encounter_methods: 1-{MAX_METHODS} methods")
   out, names = [], set()
   for i, md in enumerate(raw):
      w = f"world encounter_methods {i}"
      check_keys(md, "method", w)
      n = md.get("name")
      if not isinstance(n, str) or not n.strip() or n in names:
         raise ManifestError(f"{w}: needs a unique name")
      names.add(n)
      trig = md.get("trigger", "step")
      if trig not in TRIGGERS:
         raise ManifestError(f"{w} ({n}): trigger must be one of {', '.join(TRIGGERS)}")
      vs = md.get("vehicles", classes)
      if not isinstance(vs, list) or any(v not in classes for v in vs):
         raise ManifestError(f"{w} ({n}): vehicles lists vehicle classes ({', '.join(classes)})")
      if trig == "script" and "vehicles" in md:
         raise ManifestError(f"{w} ({n}): vehicles apply to step methods only")
      # vehicles: one bit per vehicle class, in the world's order
      out.append({"name": n, "trigger": TRIGGERS.index(trig),
                  "vehicles": sum(1 << classes.index(v) for v in vs) if trig == "step" else 0})
   return out


def compile_conditions(raw: Json) -> list[str]:
   """!The world's condition names.
   @param raw  the "encounter_conditions" value, or None
   @return list of names (bit order)
   """
   raw = [] if raw is None else raw
   if not isinstance(raw, list) or len(raw) > MAX_CONDITIONS:
      raise ManifestError(f"world encounter_conditions: up to {MAX_CONDITIONS} names")
   for i, n in enumerate(raw):
      if not isinstance(n, str) or not n.strip():
         raise ManifestError(f"world encounter_conditions {i}: needs a name")
   if len(set(raw)) != len(raw):
      raise ManifestError("world encounter_conditions: names must be unique")
   return list(raw)


# -- tables -----------------------------------------------------------------------------------

def compile_entries(raw: Json, species: Species, conditions: Sequence[str], where: str) -> tuple[Entry, ...]:
   """!A table's entries.
   @param raw         the "entries" value
   @param species     Species
   @param conditions  condition names
   @param where       location
   @return tuple of entries (hashable): (species, lo, hi, weight, required, excluded, ((cond, q8), ...))
   """
   if not isinstance(raw, list) or len(raw) > MAX_ENTRIES:
      raise ManifestError(f"{where}: entries lists up to {MAX_ENTRIES} entries (an empty table rolls nothing)")

   def bits(names: Json, w: str) -> int:
      """!A mask of condition bits (one per condition, in the world's order)."""
      if names is None:
         return 0
      if not isinstance(names, list):
         raise ManifestError(f"{w}: expected a list of condition names")
      mask = 0
      for n in names:
         if n not in conditions:
            raise ManifestError(f"{w}: unknown condition {n!r}" + (
               f" (the world's: {', '.join(conditions)})" if conditions else
               " (the world lists no encounter_conditions)"))
         mask |= 1 << conditions.index(n)
      return mask

   out = []
   for i, e in enumerate(raw):
      w = f"{where} entry {i}"
      check_keys(e, "entry", w)
      sp = species.index(e.get("species"), w)
      lv = e.get("levels")
      if isinstance(lv, int) and not isinstance(lv, bool):
         lv = [lv, lv]
      if not (isinstance(lv, list) and len(lv) == PAIR):
         raise ManifestError(f"{w}: levels is a level or [lowest, highest]")
      lv_lo, lv_hi = lv
      lo = integer(lv_lo, 1, species.level_max, f"{w} levels")
      hi = integer(lv_hi, lo, species.level_max, f"{w} levels")
      weight = integer(e.get("weight"), 1, MAX_WEIGHT, f"{w} weight")
      req, exc = bits(e.get("requires"), f"{w} requires"), bits(e.get("excludes"), f"{w} excludes")
      if req & exc:
         raise ManifestError(f"{w}: a condition is both required and excluded")
      mods = e.get("modifiers", [])
      if not isinstance(mods, list) or len(mods) > MAX_MODIFIERS:
         raise ManifestError(f"{w}: modifiers lists up to {MAX_MODIFIERS} modifiers")
      ms = []
      for j, md in enumerate(mods):
         mw = f"{w} modifier {j}"
         check_keys(md, "modifier", mw)
         c = md.get("when")
         if c not in conditions:
            raise ManifestError(f"{mw}: unknown condition {c!r}")
         ms.append((conditions.index(c), q8(md.get("multiply"), f"{mw} multiply")))
      out.append((sp, lo, hi, weight, req, exc, tuple(ms)))
   worst = sum(max_weight(e) for e in out)
   if worst >= WEIGHT_SUM_LIMIT:
      raise ManifestError(f"{where}: weights times multipliers can reach {worst}; at most {WEIGHT_SUM_LIMIT - 1} "
                          "(P-18)")
   return tuple(out)


def max_weight(entry: Entry) -> int:
   """!The largest weight an entry can reach (every multiplier above x1 active).
   @param entry  compiled entry
   @return weight
   """
   _sp, _lo, _hi, w, _req, _exc, mods = entry

   # a multiplier is Q8: above x1 it raises the weight
   for _, m in mods:
      if m > Q8_ONE:
         w = (w * m) >> Q8_SHIFT
   return w


def load_file(path: Path, where: str, sources: Sources | None, deps: list[Path] | None) -> list[Json]:
   """!Reads an encounter file.
   @param path     file
   @param where    location
   @param sources  in-memory manifests, or None
   @param deps     read files list, or None
   @return its tables list
   """
   data = load_json(path, where, sources)
   if deps is not None:
      deps.append(Path(path).resolve())
   check_keys(data, "file", where)
   if data.get("schema") != SCHEMA:
      raise ManifestError(f"{where}: schema must be '{SCHEMA}'")
   tables = data.get("tables", [])
   if not isinstance(tables, list):
      raise ManifestError(f"{where}: tables must be a list")
   return tables


class Bank:
   """!Tables, entries and modifiers being collected, deduplicated."""

   def __init__(self) -> None:
      """!Starts empty."""
      ## (rate, entries) -> table index.
      self.index: dict[tuple[tuple[int, int], tuple[Entry, ...]], int] = {}
      ## Compiled tables: (rate_n, rate_m, entries).
      self.tables: list[tuple[int, int, tuple[Entry, ...]]] = []

   def add(self, rate_nm: tuple[int, int], entries: tuple[Entry, ...]) -> int:
      """!Stores a table once.
      @param rate_nm  (n, m)
      @param entries  compiled entries
      @return table index
      """
      key = (rate_nm, entries)
      if key not in self.index:
         self.index[key] = len(self.tables)
         n, m = rate_nm
         self.tables.append((n, m, entries))
      return self.index[key]


def compile_encounters(world: dict[str, Any], world_path: Path, classes: Sequence[str], map_paths: Mapping[str, Path],
                       deps: list[Path] | None = None, sources: Sources | None = None,
                       only_maps: Sequence[str] | set[str] | None = None) -> dict[str, Any]:
   """!Compiles every map's encounter tables.
   @param world       stripped world manifest
   @param world_path  world file (resolved)
   @param classes     vehicle class names
   @param map_paths   map name -> resolved manifest Path
   @param deps        read files list, or None
   @param sources     in-memory manifests, or None
   @param only_maps   map names to compile, or None for all
   @return dict: "methods", "conditions", "tables", "refs" (map name -> [(slot, method, table)]),
           "species" (names by index, or []), "shared" (names)
   """
   base = world_path.parent
   methods = compile_methods(world.get("encounter_methods"), classes)
   method_names = [m["name"] for m in methods]
   conditions = compile_conditions(world.get("encounter_conditions"))
   species = None

   def need_species(where: str) -> Species:
      """!The battle manifest's species, read the first time a table needs them."""
      nonlocal species
      if species is None:
         rel = world.get("battle")
         if not isinstance(rel, str):
            raise ManifestError(f"{where}: encounters name species, so the world needs \"battle\" "
                                "(its capsule.battle/1 manifest)")
         species = load_species(base / rel, deps, sources)
      return species

   if world.get("battle") is not None:
      if not isinstance(world.get("battle"), str):
         raise ManifestError("world battle: expected a file name")
      need_species("world")

   bank = Bank()
   shared = {}
   shared_rel = world.get("encounter_tables")
   if shared_rel is not None:
      if not isinstance(shared_rel, str):
         raise ManifestError("world encounter_tables: expected a file name")
      where = f"shared encounter tables {shared_rel}"
      for i, t in enumerate(load_file(base / shared_rel, where, sources, deps)):
         tw = f"{where} table {i}"
         check_keys(t, "shared_table", tw)
         n = t.get("name")
         if not isinstance(n, str) or not n.strip() or n in shared:
            raise ManifestError(f"{tw}: needs a unique name")
         tw = f"{where} table {n!r}"
         shared[n] = (rate(t.get("rate"), tw), compile_entries(t.get("entries"), need_species(tw), conditions, tw))

   refs = {}
   for name, mpath in map_paths.items():
      if only_maps is not None and name not in only_maps:
         continue
      m = load_json(mpath, f"map {name!r}", sources)
      rel = m.get("encounters") if isinstance(m, dict) else None
      if rel is None:
         refs[name] = []
         continue
      if not isinstance(rel, str):
         raise ManifestError(f"map {name!r} encounters: expected a file name")
      where = f"map {name!r} encounters {rel}"
      out, seen = [], {}
      for i, t in enumerate(load_file(mpath.parent / rel, where, sources, deps)):
         tw = f"{where} table {i}"
         check_keys(t, "map_table", tw)
         slot = integer(t.get("slot"), 0, MAX_SLOTS - 1, f"{tw} slot")
         md = t.get("method", method_names[0])
         if md not in method_names:
            raise ManifestError(f"{tw}: unknown method {md!r} (the world's: {', '.join(method_names)})")
         mi = method_names.index(md)
         if (slot, mi) in seen:
            raise ManifestError(f"{tw}: slot {slot} already has a {md!r} table (table {seen[(slot, mi)]})")
         seen[(slot, mi)] = i
         tw = f"{tw} (slot {slot}, {md})"
         if t.get("table") is not None:
            if t.get("entries") is not None:
               raise ManifestError(f"{tw}: give entries or a shared table, not both")
            if t["table"] not in shared:
               raise ManifestError(f"{tw}: unknown shared table {t['table']!r}" + (
                  "" if shared_rel else " (the world names no encounter_tables file)"))
            r, entries = shared[t["table"]]
            if t.get("rate") is not None:
               r = rate(t["rate"], tw)
         else:
            r = rate(t.get("rate"), tw)
            entries = compile_entries(t.get("entries"), need_species(tw), conditions, tw)
         out.append((slot, mi, bank.add(r, entries)))
      refs[name] = sorted(out)
   if len(bank.tables) > MAX_TABLES:
      raise ManifestError(f"world: {len(bank.tables)} distinct encounter tables; at most {MAX_TABLES}")
   entry_count = sum(len(es) for _n, _m, es in bank.tables)
   if entry_count > MAX_TOTAL_ENTRIES:
      raise ManifestError(f"world: {entry_count} encounter entries; at most 65535")
   return {"methods": methods, "conditions": conditions, "tables": bank.tables, "refs": refs,
           "species": species.names if species else [], "shared": list(shared)}


# -- output -----------------------------------------------------------------------------------

def emit(enc: dict[str, Any], debug_names: bool = False) -> list[str]:
   """!C++ definitions of the bank (inside namespace capsule::map's anonymous namespace).
   @param enc          compile_encounters() result
   @param debug_names  also emit method, condition and species names
   @return (lines, per-map ref array name or "nullptr", bank symbol)
   """
   L = []
   entries, mods, tables = [], [], []
   for n, m, es in enc["tables"]:
      tables.append((len(entries), len(es), n, m))
      for sp, lo, hi, wt, req, exc, ms in es:
         entries.append((req, exc, sp, wt, len(mods), len(ms), lo, hi))
         mods.extend(ms)
   L.append("const encounter::Method kEncMethods[] = {")
   L.extend(f"    {{encounter::Trigger::{['Step', 'Script'][md['trigger']]}, 0x{md['vehicles']:02X}, {{0, 0}}}},"
            f"   // {md['name']}" for md in enc["methods"])
   L.append("};")

   # C++ arrays cannot be empty: an empty bank emits one placeholder row per array
   L.append("const encounter::Table kEncTables[] = {")
   L.extend(f"    {{{a}, {b}, {n}, {m}}}," for a, b, n, m in tables or [(0, 0, 0, 1)])
   L.append("};")
   L.append("const encounter::Entry kEncEntries[] = {")
   L.extend(f"    {{0x{r:08X}u, 0x{x:08X}u, {sp}, {wt}, {mf}, {mc}, {lo}, {hi}, {{0, 0, 0}}}},"
            + (f"   // {enc['species'][sp]}" if enc["species"] else "")
            for r, x, sp, wt, mf, mc, lo, hi in entries or [(0, 0, 0, 0, 0, 0, 0, 0)])
   L.append("};")
   L.append("const encounter::WeightModifier kEncModifiers[] = {")
   L.extend(f"    {{{c}, 0, {q}}}," for c, q in mods or [(0, 0)])
   L.append("};")
   names = ("nullptr", "nullptr", "nullptr", 0)
   if debug_names:
      def strs(sym: str, items: Sequence[str]) -> None:
         """!A string array (one empty string when there are none)."""
         L.append(f"const char* const {sym}[] = {{")
         L.extend("    " + json.dumps(s) + "," for s in items or [""])
         L.append("};")
      strs("kEncMethodNames", [m["name"] for m in enc["methods"]])
      strs("kEncConditionNames", enc["conditions"])
      strs("kEncSpeciesNames", enc["species"])
      names = ("kEncMethodNames", "kEncConditionNames", "kEncSpeciesNames", len(enc["species"]))
   method_names, condition_names, species_names, species_count = names
   L.append(f"const encounter::Bank kEncounters = {{kEncMethods, kEncTables, kEncEntries, kEncModifiers, "
            f"{method_names}, {condition_names}, {species_names}, {len(tables)}, {len(entries)}, {len(mods)}, "
            f"{species_count}, "
            f"{len(enc['methods'])}, {len(enc['conditions'])}}};")
   return L


def emit_refs(enc: dict[str, Any], map_name: str, index: int) -> tuple[list[str], str, int]:
   """!A map's ref array.
   @param enc       compile_encounters() result
   @param map_name  map name
   @param index     map index (array suffix)
   @return (lines, symbol or "nullptr", count)
   """
   refs = enc["refs"].get(map_name, [])
   if not refs:
      return [], "nullptr", 0
   L = [f"const encounter::Ref kEncRefs{index}[] = {{"]
   L.extend(f"    {{{s}, {m}, {t}}},   // slot {s}, {enc['methods'][m]['name']}" for s, m, t in refs)
   L.append("};")
   return L, f"kEncRefs{index}", len(refs)


def emit_ids(src_name: str, enc: dict[str, Any]) -> str:
   """!The identifiers header.
   @param src_name  world file name
   @param enc       compile_encounters() result
   @return header text
   """
   L = [f"// Generated from {src_name} by tools/map_compiler.py (encounters). Do not edit.", "#pragma once", "",
        "#include <cstdint>", "", "/// Encounter methods and condition bits of the build's world (D-12).",
        "namespace encounter_ids {", ""]
   for prefix, names in (("ENCOUNTER_METHOD", [m["name"] for m in enc["methods"]]),
                         ("ENCOUNTER_CONDITION", enc["conditions"])):
      seen = set()
      for i, n in enumerate(names):
         idn = f"{prefix}_{ident(n)}"
         if idn in seen:
            raise ManifestError(f"{src_name}: two names give the identifier {idn}")
         seen.add(idn)
         L.append(f"inline constexpr uint8_t {idn} = {i};")
      L.append(f"inline constexpr int NUM_{prefix}S = {len(names)};")
      L.append("")
   L += ["} // namespace encounter_ids", ""]
   return "\n".join(L)


def summary(enc: dict[str, Any]) -> str:
   """!One line describing the result.
   @param enc  compile_encounters() result
   @return text
   """
   n_entries = sum(len(es) for _n, _m, es in enc["tables"])
   n_refs = sum(len(r) for r in enc["refs"].values())
   return (f"{len(enc['methods'])} methods, {len(enc['conditions'])} conditions, {n_refs} map tables, "
           f"{len(enc['tables'])} distinct tables, {n_entries} entries")


def main(argv: Sequence[str]) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return exit status
   """
   import map_compiler as mc
   args = list(argv[1:])
   ids = None
   if "--ids" in args:
      i = args.index("--ids")
      if i + 1 >= len(args):
         print(__doc__)
         return EXIT_USAGE
      ids = Path(args[i + 1])
      del args[i:i + OPTION_WORDS]
   if len(args) != 1:
      print(__doc__)
      return EXIT_USAGE
   src = Path(args[0]).resolve()
   try:
      w, _, classes, _, maps = mc.load_world(src)
      enc = compile_encounters(w, src, classes, maps)
      if ids is not None:
         ids.parent.mkdir(parents=True, exist_ok=True)
         ids.write_text(emit_ids(src.name, enc), encoding="utf-8")
   except (ManifestError, mc.ManifestError) as e:
      print(f"{src}: error: {e}", file=sys.stderr)
      return 1
   print(f"{src}: {summary(enc)}")
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv))
