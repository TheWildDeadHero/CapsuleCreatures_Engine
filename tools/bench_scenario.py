#!/usr/bin/env python3
"""!Battle bench scenarios (D-23 section 3.3): validate a `capsule.bench/1` scenario, build the bench for a game's
battle manifest, run it and parse what it writes. The command line and the importable API the studio's logic editor
uses (its core/bench.py).

The bench (apps/battle_bench) is a compiled host program linked against one game's generated battle data, so it
behaves exactly as the game does; build() configures the engine once into a cache folder the caller gives and then
rebuilds incrementally (only the battle data regenerates after an edit, then the link).

Scenario (`capsule.bench/1`, JSON; names are the battle manifest's):
  - schema: "capsule.bench/1"; name: the transcript's title (default "scenario"); description: free text;
    expect: a transcript file the run must reproduce exactly (relative to the scenario; check);
    rng: the generator the expectations were made with (a name: "default", the engine's, or a game's own, e.g. the
    reference repo's "method1"; optional): a bench that links another one refuses the scenario, and `run --manifest`
    builds with it (a name other than "default" needs --rng-source, the generator's source file);
  - format: a format name (default the first); seed: the battle seed (required); no_escape: true forbids running;
  - field: {weather: name or null, weather_turns: n};
  - parties: 1-6 [{side, controller: "human" | "ai" (default ai), flags: ["unowned", "no_escape"], rank,
    ai: an AI profile (G1; default: the data party's, else the battle's default profile), bag: {item name: n}
    (G3: the bag its Item actions use), data: a party of the data, or members: [member, ...]}]. A member:
    {species, level, seed (default the goldens' record seed), moves: [name or null, ...] (full move pools),
    resources: {resource name: n (a battler resource) or [n per move slot] (a move resource)} (persisted resources
    only), passive_slot, trait, item, hp, status, status_counter, experience} (record values, before the battle
    starts; "pp" is refused: D-25);
  - pokes: [{party, member, hp, status, status_counter, status_aux (the status's counter and scratch), counters:
    {volatile name: n} (that volatile's counter), item, volatiles: [names], stages: {stage name: a value in
    that stage's range}, resources: {resource name: n or [n per move slot]} (clamped to the pool's maximum)}]
    (battler values, right after the battle starts; check_names() checks them against a compiled manifest, the
    bench does too);
  - choices: per turn, the last repeating: "first" | "run" | {"move": slot or name, "target": position} |
    {"switch": member} | {"item": name, "target": position, "member": n, "slot": n} (G3: the Item action on a
    battler's position, a member of the party or a move slot of it), or a list of those, one per field position;
    none: the first usable move throughout; the battles have empty storage boxes (a recruit into a full party);
  - answers: effect requests in order: {"learn_move": slot} | {"switch_target": member} | {"choice": option};
  - stop: "end" (default) | {"turns": n} | {"steps": n}; step_limit (default 20000);
  - trace: {rng, vm, events: true / false}.

Output: JSON lines with "t" = bench, line (a transcript line in the WP-2 golden format), rng, vm, event, request,
trapped (C5: {turn, battler} when a human battler's turn choice comes while try_switch hooks trap it, is_trapped()),
note, stop, error (see apps/battle_bench/output.h).

Usage:
  bench_scenario.py validate SCENARIO...                      checks the scenarios' shape (names: the bench checks)
  bench_scenario.py build MANIFEST --cache DIR [--rng NAME --rng-source FILE] [--engine DIR] [--jobs N]
                                                              configures (once) and builds; prints the bench's path
  bench_scenario.py run SCENARIO (--bench EXE | --manifest M --cache DIR [--rng R --rng-source FILE] [--jobs N])
                                                              [--trace rng,vm,events]
                                                              runs it and prints the output lines (R defaults to
                                                              the scenario's "rng", else "default")
  bench_scenario.py check SCENARIO --bench EXE                runs it and compares the transcript with "expect"
  bench_scenario.py emit-data MANIFEST OUT.cpp [--platform P] [--depfile D]
                                                              the bench tables (build step of capsule_battle_bench())
"""
from __future__ import annotations

import argparse
import json
import os
import queue
import subprocess
import sys
import threading
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import effect_compiler as ec  # noqa: E402
import ruleset_compiler as rc  # noqa: E402
from battlec.common import GENERATOR_NAME  # noqa: E402
from battlec.conditions import NO_SLOT  # noqa: E402

## The scenario format.
SCHEMA = "capsule.bench/1"
## Exit status of a usage error.
EXIT_USAGE = 2
## Exit status of a failed check or a bad scenario.
EXIT_FAILED = 1
## The engine folder (this file's parent's parent).
ENGINE_DIR = Path(__file__).resolve().parent.parent
## The bench target of a game's cache build (cmake/parts/bench.cmake).
GAME_TARGET = "capsule_bench_game"
## The engine's generator (a bench links it unless a game names its own with a source file).
DEFAULT_RNG = "default"
## A generator name (a scenario's "rng"; a bench reports the one it links; battlec.common.GENERATOR_NAME).
RNG_NAME = GENERATOR_NAME
## Schema of a generators manifest: a repository declares its random generators (name -> next16() source file,
## relative to the manifest), so tools and the studio name them without guessing from file names. The reference
## repo's is compat/generators.json.
GENERATORS_SCHEMA = "capsule.generators/1"
## The usual file name of a generators manifest.
GENERATORS_FILE = "generators.json"
## Keys of a generators manifest.
GENERATORS_KEYS = {"schema", "generators"}
## Keys of one generator of a generators manifest.
GENERATOR_KEYS = {"name", "source", "description"}
## Trace names.
TRACES = ("rng", "vm", "events")
## Largest 32-bit value (seeds, experience).
U32_MAX = 0xFFFFFFFF
## Most parties a battle has (capsule::battle::kMaxParties).
MAX_PARTIES = 6
## Values any stage can take (capsule::battle::kStageLimit; each stage's own range is the manifest's).
STAGE_RANGE = (-rc.STAGE_LIMIT, rc.STAGE_LIMIT)
## A scenario's switch for the effect logic's texts in the transcript (E13b; X1: the studio's key).
OWNER_TEXTS_KEY = "owner_texts"
## The record kind of a trapped battler's turn choice (C5: apps/battle_bench/main.cpp, is_trapped(); the studio's).
TRAPPED_RECORD = "trapped"
## Keys of a scenario.
SCENARIO_KEYS = {"schema", "name", "description", "expect", "rng", "format", "seed", "no_escape", "field", "parties",
                 "pokes", "choices", "answers", "stop", "step_limit", "trace", OWNER_TEXTS_KEY}
## Keys of a party.
PARTY_KEYS = {"side", "controller", "data", "members", "flags", "rank", "ai", "bag"}
## Keys of an item choice (G3).
ITEM_CHOICE_KEYS = {"item", "target", "member", "slot"}
## Keys of a record member.
MEMBER_KEYS = {"species", "level", "seed", "moves", "resources", "passive_slot", "trait", "item", "hp", "status",
               "status_counter", "experience"}
## Keys of a battler poke.
POKE_KEYS = {"party", "member", "hp", "status", "status_counter", "status_aux", "counters", "item", "volatiles",
             "stages", "resources"}
## Keys that are gone, with what replaced them (D-25: PP is a resource).
RETIRED_MEMBER_KEYS = {"pp": "PP is a resource now (D-25): \"resources\": {\"pp\": [n per move slot]}"}
## Poke keys that are gone (C1: the engine's bench keeps only the generic forms), with what replaced them.
RETIRED_POKE_KEYS = {"status_turns": "status_counter", "escalation": "status_aux"}
## A party's controllers (the first is the default).
CONTROLLERS = ("ai", "human")
## Default controller of a party.
DEFAULT_CONTROLLER = CONTROLLERS[0]
## Party flags.
PARTY_FLAGS = {"unowned", "no_escape"}
## Request kinds an answer names.
ANSWER_KINDS = {"learn_move", "switch_target", "choice"}
## How a run stops: STOP_END (the default: the battle ends) or {kind: n} for the other kinds.
STOP_KINDS = ("end", "turns", "steps")
## The default stop.
STOP_END = STOP_KINDS[0]
## Characters of a failed configure or build log the error keeps (its end).
LOG_TAIL = 4000
## Seconds a bench run may take.
RUN_TIMEOUT = 120
## Seconds between two looks at a build's cancel callback while its step runs.
CANCEL_POLL = 0.1
## Most seconds a finished or cancelled step's output reader gets to reach the end of the pipe.
READER_JOIN = 5
## The bench tables' lists: (list key, compiled names key, index of the first name).
NAME_LISTS = [("species", "species", 0), ("moves", "moves", 0), ("items", "items", 1), ("statuses", "statuses", 1),
              ("weathers", "weathers", 0), ("passives", "passives", 0), ("traits", "traits", 0),
              ("formats", "formats", 0), ("parties", "parties", 0), ("types", "types", 0),
              ("categories", "categories", 0), ("stats", "stats", 0), ("resources", "resources", 0),
              ("ai_profiles", "ai_profiles", 0)]


class BenchError(Exception):
   """!A scenario, build or run problem."""


def load_generators(path: str | Path) -> dict[str, Path]:
   """!Reads a generators manifest (GENERATORS_SCHEMA): {"schema", "generators": [{"name", "source",
   "description"}]}, each source a next16() implementation relative to the manifest (a build's --rng NAME
   --rng-source FILE, CMake capsule_rng(... SOURCE FILE)).
   @param path  the manifest
   @return {generator name: its source file (absolute)}
   @throws BenchError when the manifest is unreadable, malformed, names a generator twice (or "default", the
           engine's own) or a source that does not exist
   """
   path = Path(path)
   try:
      data = json.loads(path.read_text(encoding="utf-8"))
   except (OSError, json.JSONDecodeError) as e:
      raise BenchError(f"{path}: cannot read ({e})") from None
   if not isinstance(data, dict) or data.get("schema") != GENERATORS_SCHEMA or set(data) - GENERATORS_KEYS:
      raise BenchError(f"{path}: a {GENERATORS_SCHEMA} manifest {{schema, generators}}")
   out: dict[str, Path] = {}
   for i, g in enumerate(data.get("generators", [])):
      where = f"{path.name}: generators[{i}]"
      if not isinstance(g, dict) or set(g) - GENERATOR_KEYS or not {"name", "source"} <= set(g):
         raise BenchError(f"{where}: {{name, source, description}}")
      name = g["name"]
      if not isinstance(name, str) or not RNG_NAME.match(name) or name == DEFAULT_RNG or name in out:
         raise BenchError(f"{where}.name: a new lower-case identifier (not {DEFAULT_RNG!r}), got {name!r}")
      source = (path.parent / str(g["source"])).resolve()
      if not source.is_file():
         raise BenchError(f"{where}.source: {g['source']} does not exist")
      out[name] = source
   return out


class BenchCancelled(BenchError):
   """!build() stopped because its cancel callback said so (the step it ran was terminated)."""



# -- validation -----------------------------------------------------------------------------------------------------

def _is_int(v: Any) -> bool:
   """!Whether a JSON value is an integer (not a boolean).
   @param v  the value
   @return true for an int
   """
   return isinstance(v, int) and not isinstance(v, bool)


def _int(problems: list[str], where: str, v: Any, lo: int, hi: int) -> None:
   """!Checks an integer in a range.
   @param problems  collected problems
   @param where     path
   @param v         the value
   @param lo        lowest
   @param hi        highest
   @return None
   """
   if not _is_int(v) or not lo <= v <= hi:
      problems.append(f"{where}: an integer {lo}..{hi}")


def _keys(problems: list[str], where: str, v: Any, allowed: set[str]) -> bool:
   """!Checks that a value is an object with known keys.
   @param problems  collected problems
   @param where     path
   @param v         the value
   @param allowed   known keys
   @return false when it is not an object
   """
   if not isinstance(v, dict):
      problems.append(f"{where or 'scenario'}: an object")
      return False
   for k in v:
      if k not in allowed:
         problems.append(f"{where + '.' if where else ''}{k}: unknown member")
   return True


def _name(problems: list[str], where: str, v: Any, nullable: bool = False) -> None:
   """!Checks a name (the bench resolves it).
   @param problems  collected problems
   @param where     path
   @param v         the value
   @param nullable  null allowed ("none")
   @return None
   """
   if not (isinstance(v, str) or (nullable and v is None)):
      problems.append(f"{where}: a name" + (" or null" if nullable else ""))


def _choice(problems: list[str], where: str, v: Any) -> None:
   """!Checks one choice.
   @param problems  collected problems
   @param where     path
   @param v         the choice
   @return None
   """
   if v in ("first", "run"):
      return
   if isinstance(v, dict) and "item" in v:
      # G3: the Item action
      if set(v) - ITEM_CHOICE_KEYS:
         problems.append(f'{where}: an item choice has {", ".join(sorted(ITEM_CHOICE_KEYS))}')
      _name(problems, f"{where}.item", v["item"])
      for k in ("target", "member", "slot"):
         if k in v:
            _int(problems, f"{where}.{k}", v[k], 0, ec.U8_MAX)
      return
   if not isinstance(v, dict) or set(v) - {"move", "target", "switch"} or ("move" in v) == ("switch" in v):
      problems.append(f'{where}: "first", "run", {{"move": slot or name, "target": n}} or {{"switch": member}}')
      return
   if "move" in v and not (isinstance(v["move"], str) or _is_int(v["move"])):
      problems.append(f"{where}.move: a slot or a move name")
   for k in ("target", "switch"):
      if k in v:
         _int(problems, f"{where}.{k}", v[k], 0, ec.U8_MAX)


def _member(problems: list[str], where: str, m: Any) -> None:
   """!Checks a record member.
   @param problems  collected problems
   @param where     path
   @param m         the member
   @return None
   """
   for k, why in RETIRED_MEMBER_KEYS.items():
      if isinstance(m, dict) and k in m:
         problems.append(f"{where}.{k}: {why}")
   if not _keys(problems, where, {k: v for k, v in m.items() if k not in RETIRED_MEMBER_KEYS}
                if isinstance(m, dict) else m, MEMBER_KEYS):
      return
   if "species" not in m or "level" not in m:
      problems.append(f'{where}: a member needs "species" and "level"')
   if "species" in m:
      _name(problems, f"{where}.species", m["species"])
   if "level" in m:
      _int(problems, f"{where}.level", m["level"], 1, ec.U8_MAX)
   for k in ("trait",):
      if k in m:
         _name(problems, f"{where}.{k}", m[k])
   for k in ("item", "status"):
      if k in m:
         _name(problems, f"{where}.{k}", m[k], nullable=True)
   for k, hi in (("seed", U32_MAX), ("passive_slot", ec.U8_MAX), ("hp", ec.U16_MAX),
                 ("status_counter", ec.U8_MAX), ("experience", U32_MAX)):
      if k in m:
         _int(problems, f"{where}.{k}", m[k], 0, hi)
   if "moves" in m:
      if not isinstance(m["moves"], list):
         problems.append(f"{where}.moves: a list of move names")
      else:
         for i, mv in enumerate(m["moves"]):
            _name(problems, f"{where}.moves[{i}]", mv, nullable=True)
   if "resources" in m:
      _resources(problems, f"{where}.resources", m["resources"])


def _resources(problems: list[str], where: str, v: Any) -> None:
   """!Checks a "resources" object's shape: {name: n or [n, ...]} (check_names() checks the names and scopes).
   @param problems  collected problems
   @param where     path
   @param v         the object
   @return None
   """
   if not isinstance(v, dict):
      problems.append(f"{where}: an object of resource name: value (or a list, one per move slot)")
      return
   for name, x in v.items():
      for i, n in enumerate(x if isinstance(x, list) else [x]):
         _int(problems, f"{where}.{name}" + (f"[{i}]" if isinstance(x, list) else ""), n, 0, ec.U16_MAX)


def _party(problems: list[str], where: str, p: Any) -> None:
   """!Checks a party.
   @param problems  collected problems
   @param where     path
   @param p         the party
   @return None
   """
   if not _keys(problems, where, p, PARTY_KEYS):
      return
   if "side" not in p:
      problems.append(f'{where}: a party needs "side"')
   else:
      _int(problems, f"{where}.side", p["side"], 0, ec.U8_MAX)
   if p.get("controller", DEFAULT_CONTROLLER) not in CONTROLLERS:
      problems.append(f'{where}.controller: one of {", ".join(CONTROLLERS)}')
   if "rank" in p:
      _int(problems, f"{where}.rank", p["rank"], 0, ec.U8_MAX)
   if "ai" in p:
      _name(problems, f"{where}.ai", p["ai"])   # G1: an AI profile (the bench checks the name)
   if "bag" in p:
      # G3: {item name: count}
      if not isinstance(p["bag"], dict):
         problems.append(f"{where}.bag: an object of item name -> count")
      else:
         for name, n in p["bag"].items():
            _int(problems, f"{where}.bag.{name}", n, 1, ec.U16_MAX)
   flags = p.get("flags", [])
   if not isinstance(flags, list) or any(f not in PARTY_FLAGS for f in flags):
      problems.append(f"{where}.flags: a list of {', '.join(sorted(PARTY_FLAGS))}")
   if ("data" in p) == ("members" in p):
      problems.append(f'{where}: either "data" (a party of the data) or "members" (records)')
   elif "data" in p:
      _name(problems, f"{where}.data", p["data"])
   elif not isinstance(p["members"], list) or not p["members"]:
      problems.append(f"{where}.members: a non-empty list")
   else:
      for i, m in enumerate(p["members"]):
         _member(problems, f"{where}.members[{i}]", m)


def _poke(problems: list[str], where: str, k: Any) -> None:
   """!Checks a battler poke.
   @param problems  collected problems
   @param where     path
   @param k         the poke
   @return None
   """
   for old, new in RETIRED_POKE_KEYS.items():
      if isinstance(k, Mapping) and old in k:
         problems.append(f"{where}.{old}: the poke key is {new!r} now (the engine's generic name)")
   if not _keys(problems, where, k, POKE_KEYS):
      return
   if "party" not in k or "member" not in k:
      problems.append(f'{where}: a poke needs "party" and "member"')
   for key, hi in (("party", MAX_PARTIES - 1), ("member", ec.U8_MAX), ("hp", ec.U16_MAX),
                   ("status_counter", ec.U8_MAX), ("status_aux", ec.U8_MAX)):
      if key in k:
         _int(problems, f"{where}.{key}", k[key], 0, hi)
   for key in ("status", "item"):
      if key in k:
         _name(problems, f"{where}.{key}", k[key], nullable=True)
   if "counters" in k:
      if not isinstance(k["counters"], dict):
         problems.append(f"{where}.counters: an object of volatile name: counter value")
      else:
         for name, v in k["counters"].items():
            _int(problems, f"{where}.counters.{name}", v, 0, ec.U8_MAX)
   if "volatiles" in k:
      if not isinstance(k["volatiles"], list):
         problems.append(f"{where}.volatiles: a list of volatile names")
      else:
         for i, v in enumerate(k["volatiles"]):
            _name(problems, f"{where}.volatiles[{i}]", v)
   if "resources" in k:
      _resources(problems, f"{where}.resources", k["resources"])
   if "stages" in k:
      if not isinstance(k["stages"], dict):
         problems.append(f"{where}.stages: an object of stage name: value")
      else:
         for name, v in k["stages"].items():
            _int(problems, f"{where}.stages.{name}", v, *STAGE_RANGE)


def check_names(scenario: Mapping[str, Any], compiled: Mapping[str, Any]) -> list[str]:
   """!Checks a well-formed scenario's pokes against a compiled battle manifest (battle_compiler.compile_battle()):
   volatile names, counters (a volatile with a counter), stage names and each stage's own range, status and item
   names. The bench checks the same (and every other name) when it reads the scenario.
   @param scenario  a scenario validate() accepts
   @param compiled  the game's compiled manifest
   @return problems ("path: message")
   """
   problems: list[str] = []
   names = compiled["names"]
   volatiles = {v["source_name"]: v for v in compiled["volatiles"]}
   stages = {st["name"]: st for st in compiled["stages"]}
   resources = {r["name"]: r for r in compiled.get("resources", [])}

   def check_resources(where: str, values: Mapping[str, Any], record: bool) -> None:
      """!A "resources" object against the manifest: known names, a list for a move resource (a number for a
      battler resource), values the pool can hold; a record member names persisted resources only."""
      for name, x in values.items():
         r = resources.get(name)
         if r is None:
            problems.append(f"{where}.{name}: unknown resource {name!r} ({', '.join(resources) or 'none'})")
            continue
         is_move = r["scope_name"] == "move"
         if is_move != isinstance(x, list):
            problems.append(f"{where}.{name}: " + ("a list, one value per move slot" if is_move else "a number"))
         elif is_move and any(_is_int(n) and n > ec.U8_MAX for n in x):
            problems.append(f"{where}.{name}: 0 to {ec.U8_MAX} (a move pool is 8 bits)")
         if record and r["persist_name"] is None:
            problems.append(f"{where}.{name}: refilled every battle (no persist role); poke it with "
                            f"pokes[].resources")
   for p, party in enumerate(scenario.get("parties", [])):
      for name in (party.get("bag") or {}) if isinstance(party, Mapping) else {}:
         if name not in names["items"]:
            problems.append(f"parties[{p}].bag.{name}: unknown item {name!r}")
      for m, member in enumerate(party.get("members", []) if isinstance(party, Mapping) else []):
         if isinstance(member, Mapping) and isinstance(member.get("resources"), Mapping):
            check_resources(f"parties[{p}].members[{m}].resources", member["resources"], True)
   for i, k in enumerate(scenario.get("pokes", [])):
      where = f"pokes[{i}]"
      if isinstance(k.get("resources"), Mapping):
         check_resources(f"{where}.resources", k["resources"], False)
      for key, kind in (("status", "statuses"), ("item", "items")):
         if k.get(key) is not None and k[key] not in names[kind]:
            problems.append(f"{where}.{key}: unknown {key} {k[key]!r}")
      for j, v in enumerate(k.get("volatiles", [])):
         if v not in volatiles:
            problems.append(f"{where}.volatiles[{j}]: unknown volatile {v!r}")
      counters = {f"counters.{n}": n for n in k.get("counters", {})}
      for path, v in counters.items():
         if v not in volatiles:
            problems.append(f"{where}.{path}: unknown volatile {v!r}")
         elif volatiles[v]["counter"] == NO_SLOT:
            problems.append(f"{where}.{path}: the volatile {v} has no counter")
      for name, v in k.get("stages", {}).items():
         st = stages.get(name)
         if st is None:
            problems.append(f"{where}.stages.{name}: unknown stage {name!r} ({', '.join(stages) or 'none'})")
         elif _is_int(v) and not st["range"][0] <= v <= st["range"][1]:
            problems.append(f"{where}.stages.{name}: {st['range'][0]} to {st['range'][1]} (the stage's range)")
   return problems


def validate(scenario: Any) -> list[str]:
   """!Checks a scenario's shape: keys, kinds and ranges (the bench resolves the names against the game's data).
   @param scenario  the parsed scenario
   @return problems ("path: message"); empty when it is well formed
   """
   problems: list[str] = []
   if not _keys(problems, "", scenario, SCENARIO_KEYS):
      return problems
   if scenario.get("schema") != SCHEMA:
      problems.append(f"schema: {SCHEMA!r}")
   for k in ("name", "description", "expect"):
      if k in scenario and not isinstance(scenario[k], str):
         problems.append(f"{k}: a string")
   if "rng" in scenario and not (isinstance(scenario["rng"], str) and RNG_NAME.match(scenario["rng"])):
      problems.append("rng: a generator name (lower-case letters, digits and _; \"default\": the engine's)")
   if "format" in scenario:
      _name(problems, "format", scenario["format"])
   if "seed" not in scenario:
      problems.append('scenario: a scenario needs "seed"')
   else:
      _int(problems, "seed", scenario["seed"], 0, U32_MAX)
   if "no_escape" in scenario and not isinstance(scenario["no_escape"], bool):
      problems.append("no_escape: true or false")
   # E13b: the transcript also shows owner texts and condition changes
   if OWNER_TEXTS_KEY in scenario and not isinstance(scenario[OWNER_TEXTS_KEY], bool):
      problems.append(f"{OWNER_TEXTS_KEY}: true or false")
   fld = scenario.get("field", {})
   if _keys(problems, "field", fld, {"weather", "weather_turns"}):
      if "weather" in fld:
         _name(problems, "field.weather", fld["weather"], nullable=True)
      if "weather_turns" in fld:
         _int(problems, "field.weather_turns", fld["weather_turns"], 0, ec.U8_MAX)
   parties = scenario.get("parties")
   if not isinstance(parties, list) or not 1 <= len(parties) <= MAX_PARTIES:
      problems.append(f"parties: a list of 1 to {MAX_PARTIES} parties")
   else:
      for i, p in enumerate(parties):
         _party(problems, f"parties[{i}]", p)
   for key, check in (("pokes", _poke), ("answers", None), ("choices", None)):
      v = scenario.get(key, [])
      if not isinstance(v, list):
         problems.append(f"{key}: a list")
         continue
      for i, entry in enumerate(v):
         where = f"{key}[{i}]"
         if key == "pokes":
            check(problems, where, entry)
         elif key == "answers":
            if not isinstance(entry, dict) or len(entry) != 1 or next(iter(entry)) not in ANSWER_KINDS:
               problems.append(f"{where}: {{kind: value}} with kind one of {', '.join(sorted(ANSWER_KINDS))}")
            else:
               _int(problems, f"{where}.{next(iter(entry))}", next(iter(entry.values())), 0, ec.U8_MAX)
         elif isinstance(entry, list):
            if not entry:
               problems.append(f"{where}: a non-empty list of choices")
            for j, c in enumerate(entry):
               _choice(problems, f"{where}[{j}]", c)
         else:
            _choice(problems, where, entry)
   stop = scenario.get("stop", STOP_END)
   if stop != STOP_END:
      if not isinstance(stop, dict) or len(stop) != 1 or next(iter(stop)) not in STOP_KINDS[1:]:
         problems.append('stop: "end", {"turns": n} or {"steps": n}')
      else:
         _int(problems, f"stop.{next(iter(stop))}", next(iter(stop.values())), 1, ec.U16_MAX)
   if "step_limit" in scenario:
      _int(problems, "step_limit", scenario["step_limit"], 1, U32_MAX >> 1)
   trace = scenario.get("trace", {})
   if _keys(problems, "trace", trace, set(TRACES)):
      for k, v in trace.items():
         if not isinstance(v, bool):
            problems.append(f"trace.{k}: true or false")
   return problems


def load(path: str | Path) -> dict[str, Any]:
   """!Reads and validates a scenario file.
   @param path  the file
   @return the scenario
   @throws BenchError when it cannot be read or is malformed
   """
   try:
      data = json.loads(Path(path).read_text(encoding="utf-8"))
   except OSError as e:
      raise BenchError(f"{path}: cannot read ({e.strerror})")
   except json.JSONDecodeError as e:
      raise BenchError(f"{path}: {e}")
   problems = validate(data)
   if problems:
      raise BenchError(f"{path}: " + "; ".join(problems))
   return data


# -- bench tables (build step) ----------------------------------------------------------------------------------------

def _cpp_string(text: str) -> str:
   """!A C++ string literal.
   @param text  the text
   @return the literal
   """
   return json.dumps(text)


def emit_data(manifest: str | Path, platform: str | None = None, deps: list[Path] | None = None) -> str:
   """!The bench tables of a battle manifest (apps/battle_bench/bench_data.h): its names, the effect bank's owner,
   hook and program names, the opcode names and the debug pc -> source line table (effect_compiler.track_lines()).
   The manifest is compiled exactly as tools/battle_compiler.py compiles it, so the tables describe the same bank.
   @param manifest  battle.json
   @param platform  target platform (its limits)
   @param deps      files read (appended), or None
   @return C++ source
   @throws BenchError when the manifest does not compile
   """
   import battle_compiler as bc
   ec.track_lines(True)
   try:
      c = bc.compile_battle(manifest, deps, platform=platform)
      bank = c["effects"]["bank"]
      lines = ec.emit_line_table(bank)
   except (bc.ManifestError, ec.EffectError, KeyError, TypeError, ValueError) as e:
      raise BenchError(f"{manifest}: {e}")
   finally:
      ec.track_lines(False)
   catalog = c["effect_catalog"]
   names = {**c["names"], "species": [s["name"] for s in c["species"]]}
   stage_names = list(catalog.ids["stages"] or {})
   volatile_names = list(catalog.ids["volatiles"] or {})

   def table(ident: str, values: Sequence[str]) -> str:
      """!A string table."""
      return f"const char* const {ident}[] = {{{', '.join(_cpp_string(v) for v in values) or 'nullptr'}}};"

   L = [f"// Generated by tools/bench_scenario.py from {Path(manifest).name}. Do not edit.",
        '#include "bench_data.h"', "", "namespace capsule::bench", "{", "", "namespace", "{", "",
        "namespace effect = capsule::battle::effect;", ""]
   lists = [(key, names[src], first) for key, src, first in NAME_LISTS]
   lists += [("stages", stage_names, 0), ("volatiles", volatile_names, 0)]
   for key, values, _first in lists:
      L.append(table(f"k{key.title()}Names", values))
   L.append("const NameList kLists[] = {")
   for key, values, first in lists:
      L.append(f"{ec.INDENT}{{{_cpp_string(key)}, k{key.title()}Names, {len(values)}, {first}}},")
   L.append("};")
   L.append(table("kOwnerNames", bank.owner_names))
   L.append(table("kProgramHooks", bank.hooks))
   L.append(table("kProgramLabels", bank.labels))
   L.append(table("kOpNames", [op[0] for op in ec.OPS]))
   L += lines
   L += ["", "} // namespace", "",
         f"const BenchData kBenchData = {{{_cpp_string(Path(manifest).name)}, kLists, {len(lists)}, kOwnerNames, "
         f"{len(bank.owner_names)}, kProgramHooks, kProgramLabels, {len(bank.hooks)}, kOpNames, {len(ec.OPS)}, "
         "&kEffectLines};", "", "} // namespace capsule::bench", ""]
   return "\n".join(L)


def write_if_changed(path: Path, text: str) -> None:
   """!Writes a file only when its text changes (keeps incremental builds quiet).
   @param path  the file
   @param text  its text
   @return None
   """
   if path.exists() and path.read_text(encoding="utf-8") == text:
      return
   path.parent.mkdir(parents=True, exist_ok=True)
   path.write_text(text, encoding="utf-8")


# -- building ----------------------------------------------------------------------------------------------------------

def _cached(cache: Path, key: str) -> str | None:
   """!A value of a CMake cache.
   @param cache  the build folder
   @param key    the variable
   @return its value, or None
   """
   try:
      text = (cache / "CMakeCache.txt").read_text(encoding="utf-8")
   except OSError:
      return None
   for line in text.splitlines():
      name, sep, value = line.partition("=")
      if sep and name.split(":")[0] == key:
         return value
   return None


def _step(cmd: Sequence[str], what: str, progress: Callable[[str], None] | None,
          cancel: Callable[[], bool] | None) -> None:
   """!Runs one configure or build step, passing each output line to progress and stopping when cancel says so.
   @param cmd       the command
   @param what      the step, for messages ("configuring the bench")
   @param progress  callable(line) for every output line, or None
   @param cancel    callable() -> true to stop (looked at every CANCEL_POLL seconds), or None
   @return None
   @throws BenchCancelled when cancelled (the step is terminated)
   @throws BenchError when the step fails
   """
   try:
      proc = subprocess.Popen(list(cmd), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
   except OSError as e:
      raise BenchError(f"{what} failed: {e}")
   lines: queue.Queue[str | None] = queue.Queue()

   def pump() -> None:
      """!Moves the step's output lines into the queue, then None at its end."""
      assert proc.stdout is not None
      for line in proc.stdout:
         lines.put(line)
      lines.put(None)

   reader = threading.Thread(target=pump, daemon=True)
   reader.start()
   log: list[str] = []
   try:
      while True:
         if cancel is not None and cancel():
            proc.terminate()
            proc.wait()
            raise BenchCancelled(f"{what}: cancelled")
         try:
            line = lines.get(timeout=CANCEL_POLL)
         except queue.Empty:
            continue
         if line is None:
            break
         log.append(line)
         if progress is not None:
            progress(line.rstrip("\n"))
      if proc.wait() != 0:
         raise BenchError(f"{what} failed:\n" + "".join(log)[-LOG_TAIL:])
   finally:
      # the step's output pipe is closed once its reader is done (no unclosed file, E11)
      reader.join(READER_JOIN)
      assert proc.stdout is not None
      proc.stdout.close()


def build(manifest: str | Path, cache: str | Path, rng: str = DEFAULT_RNG, engine: str | Path | None = None,
          cmake: str = "cmake", jobs: int | None = None, progress: Callable[[str], None] | None = None,
          cancel: Callable[[], bool] | None = None, rng_source: str | Path | None = None) -> Path:
   """!Builds the bench for a battle manifest: configures the engine (headless) into the cache folder once, or
   again when the manifest or generator changed, then builds the bench target incrementally.
   @param manifest  the game's battle.json (relative to the current folder, or absolute)
   @param cache     the build folder (created when missing)
   @param rng       the generator's name: "default" (the engine's), or a game's own with rng_source
   @param engine    the engine folder (default: this tool's)
   @param cmake     the cmake program
   @param jobs      parallel build jobs (default: CMake's)
   @param progress  callable(line): each configure / build output line as it comes (a studio's log), or None
   @param cancel    callable() -> true to stop the build (a studio's Cancel button), or None
   @param rng_source  the generator's source file (defines capsule::rng::next16()) for a name other than "default"
                      (the reference repo's compat/classic/src/random_method1.cpp for "method1")
   @return the bench executable
   @throws BenchCancelled when cancel stopped it
   @throws BenchError when configuring or building fails
   """
   if not isinstance(rng, str) or not RNG_NAME.match(rng):
      raise BenchError(f"generator name {rng!r}: lower-case letters, digits and _")
   source = ""
   if rng != DEFAULT_RNG:
      if rng_source is None or not Path(rng_source).is_file():
         raise BenchError(f"the generator {rng!r} needs its source file (rng_source), got {rng_source!r}")
      source = str(Path(rng_source).resolve())
   if jobs is not None and (not _is_int(jobs) or jobs < 1):
      raise BenchError(f"jobs: a positive number, got {jobs!r}")
   manifest = Path(manifest).resolve()
   cache = Path(cache).resolve()
   engine = Path(engine).resolve() if engine is not None else ENGINE_DIR
   if not manifest.is_file():
      raise BenchError(f"{manifest}: no such manifest")
   if (_cached(cache, "CAPSULE_BENCH_MANIFEST") != str(manifest) or _cached(cache, "CAPSULE_BENCH_RNG") != rng
         or (_cached(cache, "CAPSULE_BENCH_RNG_SOURCE") or "") != source):
      _step([cmake, "-S", str(engine), "-B", str(cache), "-DCAPSULE_PLATFORM=headless", "-DCMAKE_BUILD_TYPE=Release",
             f"-DCAPSULE_BENCH_MANIFEST={manifest}", f"-DCAPSULE_BENCH_RNG={rng}",
             f"-DCAPSULE_BENCH_RNG_SOURCE={source}"], "configuring the bench", progress, cancel)
   cmd = [cmake, "--build", str(cache), "--target", GAME_TARGET]
   if jobs is not None:
      cmd += ["-j", str(jobs)]
   _step(cmd, "building the bench", progress, cancel)
   exe = cache / (GAME_TARGET + (".exe" if os.name == "nt" else ""))
   if not exe.is_file():
      raise BenchError(f"{exe}: the build made no bench")
   return exe


# -- running -----------------------------------------------------------------------------------------------------------

@dataclass
class Result:
   """!What a bench run wrote, by record type."""

   ## Exit status.
   status: int = 0
   ## Every record, in order.
   records: list[dict[str, Any]] = field(default_factory=list)
   ## stderr of the bench.
   stderr: str = ""

   def of(self, kind: str) -> list[dict[str, Any]]:
      """!The records of one type.
      @param kind  "line", "rng", "vm", "event", "request", "trapped", "note", "stop", "error"
      @return them, in order
      """
      return [r for r in self.records if r.get("t") == kind]

   def transcript(self) -> str:
      """!The transcript in the WP-2 golden format (every "line" record, one per line).
      @return its text
      """
      return "".join(r["text"] + "\n" for r in self.of("line"))

   def errors(self) -> list[str]:
      """!The error messages.
      @return "where: message" (or the message)
      """
      return [f"{r['where']}: {r['message']}" if r.get("where") else r["message"] for r in self.of("error")]


def parse_output(text: str) -> list[dict[str, Any]]:
   """!Parses the bench's output.
   @param text  JSON lines
   @return records
   @throws BenchError on a line that is not a JSON object
   """
   records = []
   for n, line in enumerate(text.splitlines(), 1):
      if not line.strip():
         continue
      try:
         rec = json.loads(line)
      except json.JSONDecodeError as e:
         raise BenchError(f"bench output line {n}: {e}")
      if not isinstance(rec, dict) or "t" not in rec:
         raise BenchError(f"bench output line {n}: not a record")
      records.append(rec)
   return records


def run(bench: str | Path, scenario: str | Path | Mapping[str, Any], trace: Sequence[str] = (),
        check: bool = True) -> Result:
   """!Runs the bench on a scenario.
   @param bench     the bench executable
   @param scenario  a scenario file, or a scenario dict (passed on stdin)
   @param trace     extra traces ("rng", "vm", "events")
   @param check     validate the scenario first (false: let the bench report the problems)
   @return what it wrote
   @throws BenchError when the scenario is malformed (check) or the bench cannot run
   """
   for t in trace:
      if t not in TRACES:
         raise BenchError(f"unknown trace {t!r} ({', '.join(TRACES)})")
   if isinstance(scenario, Mapping):
      data: Any = scenario
      text = json.dumps(scenario)
      arg = "-"
   else:
      data = None
      text = None
      arg = str(scenario)
      if check:
         data = load(scenario)
   if check and data is not None:
      problems = validate(data)
      if problems:
         raise BenchError("; ".join(problems))
   cmd = [str(bench), arg] + (["--trace", ",".join(trace)] if trace else [])
   try:
      done = subprocess.run(cmd, input=text, capture_output=True, text=True, timeout=RUN_TIMEOUT)
   except (OSError, subprocess.TimeoutExpired) as e:
      raise BenchError(f"cannot run the bench: {e}")
   return Result(done.returncode, parse_output(done.stdout), done.stderr)


def compare_transcripts(want: str, got: str, name: str = "transcript") -> list[str]:
   """!Compares an expected transcript with a run's, exactly.
   @param want  the expected text
   @param got   the run's (Result.transcript())
   @param name  what the expected one is called in the message (its file name)
   @return problems (the first differing line); empty when they match
   """
   if got == want:
      return []
   a, b = want.splitlines(), got.splitlines()
   for i in range(max(len(a), len(b))):
      la = a[i] if i < len(a) else "<end>"
      lb = b[i] if i < len(b) else "<end>"
      if la != lb:
         return [f"{name}:{i + 1}\n  expected: {la}\n  bench:    {lb}"]
   return [f"{name}: differs only in line endings"]


def check_expected(bench: str | Path | None, path: str | Path, result: Result | None = None) -> list[str]:
   """!Compares a scenario's "expect" transcript with a run of it, exactly: runs the bench, or takes a run the
   caller already made (a studio that ran the scenario for its views).
   @param bench   the bench executable (None with result)
   @param path    the scenario file
   @param result  a finished run of that scenario (run()), or None to run it now
   @return problems (the first differing line); empty when they match
   @throws BenchError when the scenario is malformed or has no "expect", or neither bench nor result is given
   """
   data = load(path)
   if "expect" not in data:
      raise BenchError(f"{path}: no \"expect\" transcript")
   expect = (Path(path).parent / data["expect"])
   want = expect.read_text(encoding="utf-8")
   if result is None:
      if bench is None:
         raise BenchError("check_expected() needs the bench or a result")
      result = run(bench, path, check=False)
   if result.status != 0 or result.errors():
      return [f"{path}: the bench failed ({result.status}): "
              f"{'; '.join(result.errors()) or result.stderr.strip()}"]
   return compare_transcripts(want, result.transcript(), expect.name)


# -- command line ------------------------------------------------------------------------------------------------------

def main(argv: Sequence[str]) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return process exit status
   """
   ap = argparse.ArgumentParser(prog="bench_scenario.py", description="Battle bench scenarios (capsule.bench/1).")
   sub = ap.add_subparsers(dest="cmd", required=True)
   p = sub.add_parser("validate")
   p.add_argument("scenarios", nargs="+")
   p = sub.add_parser("build")
   p.add_argument("manifest")
   p.add_argument("--cache", required=True)
   p.add_argument("--rng", default=DEFAULT_RNG)
   p.add_argument("--rng-source")
   p.add_argument("--engine")
   p.add_argument("--jobs", type=int)
   p = sub.add_parser("run")
   p.add_argument("scenario")
   p.add_argument("--bench")
   p.add_argument("--manifest")
   p.add_argument("--cache")
   p.add_argument("--rng")
   p.add_argument("--rng-source")
   p.add_argument("--jobs", type=int)
   p.add_argument("--trace", default="")
   p = sub.add_parser("check")
   p.add_argument("scenario")
   p.add_argument("--bench", required=True)
   p = sub.add_parser("emit-data")
   p.add_argument("manifest")
   p.add_argument("out")
   p.add_argument("--platform")
   p.add_argument("--depfile")
   args = ap.parse_args(list(argv[1:]))
   try:
      if args.cmd == "validate":
         bad = 0
         for s in args.scenarios:
            try:
               load(s)
               print(f"{s}: ok")
            except BenchError as e:
               print(f"error: {e}", file=sys.stderr)
               bad += 1
         return EXIT_FAILED if bad else 0
      if args.cmd == "build":
         print(build(args.manifest, args.cache, args.rng, args.engine, jobs=args.jobs, rng_source=args.rng_source))
         return 0
      if args.cmd == "run":
         bench = args.bench
         if bench is None:
            if args.manifest is None or args.cache is None:
               print("error: run needs --bench, or --manifest and --cache", file=sys.stderr)
               return EXIT_USAGE
            rng = args.rng or load(args.scenario).get("rng", DEFAULT_RNG)
            bench = build(args.manifest, args.cache, rng, jobs=args.jobs, rng_source=args.rng_source)
         res = run(bench, args.scenario, [t for t in args.trace.split(",") if t])
         for r in res.records:
            print(json.dumps(r))
         return res.status
      if args.cmd == "check":
         problems = check_expected(args.bench, args.scenario)
         for pr in problems:
            print(f"FAIL {pr}", file=sys.stderr)
         if not problems:
            print(f"{args.scenario}: transcript matches")
         return EXIT_FAILED if problems else 0
      deps: list[Path] = []
      out = Path(args.out)
      write_if_changed(out, emit_data(args.manifest, args.platform, deps))
      if args.depfile:
         def esc(path: Path) -> str:
            """!A path as a Make depfile writes it (forward slashes, spaces escaped)."""
            return str(path).replace("\\", "/").replace(" ", "\\ ")
         write_if_changed(Path(args.depfile), f"{esc(out)}: " + " ".join(esc(d) for d in sorted(set(deps))) + "\n")
      return 0
   except BenchError as e:
      print(f"error: {e}", file=sys.stderr)
      return EXIT_FAILED


if __name__ == "__main__":
   sys.exit(main(sys.argv))
