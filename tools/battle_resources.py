#!/usr/bin/env python3
"""!Migrates a battle (battle.json, the structure manifest it names, its
ruleset and `.ceffect` sources) from built-in PP to manifest resources
(D-25, E8; tools/battlec/resources.py).

    battle_resources.py <battle.json>... [--check]

What it writes, so that the compiled behaviour stays the same:

- structure "resources" (after "stats"), when the structure has none: the
  hit points {"name": "hp", "scope": "battler", "stat": the pool stat,
  "empty": "faint", "persist": "current_hp" (when the layout holds it)},
  then, when the layout holds pp or a move states "pp", the move resource
  {"name": "pp", "scope": "move", "base": "pp", "max": "pp_max" (when the
  layout holds pp_ups or the ruleset declares its pp_max slot: E7 applied it
  either way), "raises": "pp_ups" (when the layout holds it), "persist": "pp"};
- monster_layout "move_slots": 4 (the former default) when it names none;
- the ruleset's pp_max slot becomes the named formula pp_max with "args"
  ["pp_base", "pp_ups"]; a ruleset without one that a raised pp needs gets
  the former default (each raise adds a fifth of the base) and the former
  default input ranges (0-255) the ruleset does not declare;
- moves without "pp" get the former default 10;
- the message no_pp is renamed no_usable_move (a battle stating both keeps
  its no_usable_move and drops no_pp, noting it);
- `lock WHO no_pp` in the sources becomes `lock WHO free`.

Files are written as the studio writes them (JSON, one-space indents, UTF-8,
a final newline), all of a battle's or none (battle_split.write_all()); a file
that needs no change is not written. --check only
reports what would change. A ruleset shared by two battles is migrated once
(the second run finds it migrated).

Exit status: 0 when every battle was migrated (or would be, or needed
nothing), 1 when one was refused, 2 for a command line that is not understood.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from battle_split import dump, write_all  # noqa: E402
from battlec.common import ManifestError  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## Exit status: every battle migrated.
EXIT_OK = 0
## Exit status: a battle was refused.
EXIT_REFUSED = 1
## Move slots of a layout that named none before E8 (capsule::monster::kDefaultMoveSlots).
FORMER_MOVE_SLOTS = 4
## A move's PP before E8 when it named none.
FORMER_DEFAULT_PP = 10
## The former default pp_max slot: each raise adds a fifth of the base.
FORMER_PP_MAX = {"args": ["pp_base", "pp_ups"], "expr": "pp_base + pp_base * pp_ups / 5", "range": [0, 255]}
## The former default slot inputs' ranges (8-bit PP and raises).
FORMER_PP_INPUTS = {"pp_base": [0, 255], "pp_ups": [0, 255]}
## The former name of the "no usable move" message and its name now.
RENAMED_MESSAGES = {"no_pp": "no_usable_move"}
## `lock WHO no_pp` -> `lock WHO free`.
LOCK_NO_PP = re.compile(r"\block(\s+[A-Za-z_][A-Za-z0-9_]*\s+)no_pp\b")


def read(path: Path) -> JsonDict:
   """!Reads a JSON file.
   @param path  file
   @return its object
   @throws ManifestError when it cannot be read
   """
   try:
      return json.loads(path.read_text(encoding="utf-8"))
   except OSError as e:
      raise ManifestError(f"{path}: cannot read ({e.strerror})") from None
   except json.JSONDecodeError as e:
      raise ManifestError(f"{path}: invalid JSON ({e})") from None


def layout_roles(structure: JsonDict) -> list[str]:
   """!The role names a structure's monster_layout lists (engine and custom).
   @param structure  the structure manifest
   @return names
   """
   lay = structure.get("monster_layout", [])
   roles = lay.get("roles", []) if isinstance(lay, dict) else lay
   out = []
   for e in roles:
      out.append(e if isinstance(e, str) else e.get("role", e.get("custom")))
   return out


def with_key_after(obj: JsonDict, after: str, key: str, value: Any) -> JsonDict:
   """!A copy of an object with a key placed right after another (at the end when that one is missing).
   @param obj    the object
   @param after  the key to follow
   @param key    the new key
   @param value  its value
   @return the new object
   """
   out: JsonDict = {}
   for k, v in obj.items():
      out[k] = v
      if k == after:
         out[key] = value
   if key not in out:
      out[key] = value
   return out


def migrate_structure(structure: JsonDict, moves: Sequence[JsonDict],
                      ruleset_max: bool = False) -> tuple[JsonDict, list[str], bool]:
   """!The structure manifest with resources and move_slots.
   @param structure    the structure manifest
   @param moves        the content's moves (a move stating pp makes the pp resource)
   @param ruleset_max  the ruleset declares its pp_max slot (E7 applied it with or without raises)
   @return (new structure, what changed, whether the pp resource's max names pp_max)
   @throws ManifestError when the structure has no stats (it predates D-20) or no pool stat
   """
   notes: list[str] = []
   roles = layout_roles(structure)
   raises = "pp_ups" in roles
   lay = structure.get("monster_layout")
   if isinstance(lay, dict) and "move_slots" not in lay:
      structure = dict(structure)
      structure["monster_layout"] = with_key_after(lay, "party_size", "move_slots", FORMER_MOVE_SLOTS)
      notes.append(f"monster_layout.move_slots {FORMER_MOVE_SLOTS}")
   if "resources" in structure:
      return structure, notes, False
   if not isinstance(structure.get("stats"), list) or not structure["stats"]:
      raise ManifestError("it has no \"stats\": it predates the manifest stats (D-20); give it its stats first")
   pool = next((st.get("name") for st in structure["stats"] if st.get("pool")), None)
   if pool is None:
      raise ManifestError("its stats have no pool stat (hit points)")
   hp: JsonDict = {"name": "hp", "label": "HP", "scope": "battler", "stat": pool, "empty": "faint"}
   if "current_hp" in roles:
      hp["persist"] = "current_hp"
   resources = [hp]
   has_max = False
   if "pp" in roles or any("pp" in m for m in moves):
      pp: JsonDict = {"name": "pp", "label": "PP", "scope": "move", "base": "pp"}
      has_max = raises or ruleset_max
      if has_max:
         pp["max"] = "pp_max"   # E7 applied pp_max(base, ups) whenever the ruleset had it (ups 0 without the role)
      if raises:
         pp["raises"] = "pp_ups"
      if "pp" in roles:
         pp["persist"] = "pp"
      resources.append(pp)
   structure = with_key_after(structure, "stats", "resources", resources)
   notes.append(f"resources {', '.join(r['name'] for r in resources)}")
   return structure, notes, has_max


def migrate_ruleset(ruleset: JsonDict, needs_max: bool) -> tuple[JsonDict, list[str]]:
   """!The ruleset with pp_max as a named formula (when it had the slot, or a raised pp needs one).
   @param ruleset    the ruleset
   @param needs_max  the pp resource names the max formula pp_max
   @return (new ruleset, what changed)
   """
   formulas = dict(ruleset.get("formulas", {}))
   old = formulas.get("pp_max")
   if isinstance(old, dict) and "args" in old:
      return ruleset, []
   if old is None and not needs_max:
      return ruleset, []
   ruleset = dict(ruleset)
   if old is None:
      formulas["pp_max"] = dict(FORMER_PP_MAX)
      note = "pp_max: the former default as a named formula"
   else:
      formulas["pp_max"] = {"args": list(FORMER_PP_MAX["args"]), **old}
      note = "pp_max: the slot as a named formula (args pp_base, pp_ups)"
   ruleset["formulas"] = formulas
   inputs = dict(ruleset.get("inputs", {}))
   added = [k for k in FORMER_PP_INPUTS if k not in inputs]
   for k in added:
      inputs[k] = list(FORMER_PP_INPUTS[k])
   if added:
      ruleset["inputs"] = inputs
      note += f" (inputs {', '.join(added)} 0-255)"
   return ruleset, [note]


def migrate_content(battle: JsonDict, has_pp: bool) -> tuple[JsonDict, list[str]]:
   """!The content manifest with renamed messages and every move's pp stated.
   @param battle  battle.json
   @param has_pp  the structure has the pp resource (every move states its base)
   @return (new battle.json, what changed)
   """
   notes: list[str] = []
   battle = dict(battle)
   msgs = battle.get("messages")
   if isinstance(msgs, dict) and any(k in msgs for k in RENAMED_MESSAGES):
      renamed: JsonDict = {}
      for k, v in msgs.items():
         new = RENAMED_MESSAGES.get(k)
         if new is None:
            renamed[k] = v
         elif new in msgs:
            notes.append(f"message {k} dropped (the battle states {new} already)")
         else:
            renamed[new] = v
            notes.append(f"message {k} -> {new}")
      battle["messages"] = renamed
   moves = battle.get("moves", [])
   if has_pp and any("pp" not in m for m in moves):
      battle["moves"] = [m if "pp" in m else with_key_after(m, "accuracy", "pp", FORMER_DEFAULT_PP) for m in moves]
      notes.append(f"pp {FORMER_DEFAULT_PP} on the moves that named none")
   return battle, notes


def migrate_file(path: str | Path, write: bool = True) -> list[str]:
   """!Migrates one battle on disk.
   @param path   its battle.json
   @param write  false: only report
   @return what changed ("file: change")
   @throws ManifestError when a file cannot be read or the battle is not split (tools/battle_split.py first)
   """
   path = Path(path)
   battle = read(path)
   if not isinstance(battle.get("structure"), str):
      raise ManifestError(f"{path}: names no structure manifest; split it first (tools/battle_split.py)")
   if not isinstance(battle.get("ruleset"), str) or not battle["ruleset"]:
      raise ManifestError(f"{path}: names no ruleset (\"ruleset\": its ruleset file)")
   st_path = path.parent / battle["structure"]
   rs_path = path.parent / battle["ruleset"]
   structure = read(st_path)
   ruleset = read(rs_path)
   ruleset_max = "pp_max" in ruleset.get("formulas", {}) if isinstance(ruleset.get("formulas"), dict) else False
   try:
      new_structure, st_notes, needs_max = migrate_structure(structure, battle.get("moves", []), ruleset_max)
   except ManifestError as e:
      raise ManifestError(f"{st_path}: {e}") from None
   has_pp = any(r.get("name") == "pp" for r in new_structure.get("resources", []))
   new_ruleset, rs_notes = migrate_ruleset(ruleset, needs_max)
   new_battle, b_notes = migrate_content(battle, has_pp)
   writes: list[tuple[Path, str]] = []
   notes: list[str] = []
   for p, old, new, what in ((st_path, structure, new_structure, st_notes), (rs_path, ruleset, new_ruleset, rs_notes),
                             (path, battle, new_battle, b_notes)):
      if new != old:
         writes.append((p, dump(new)))
         notes += [f"{p.name}: {n}" for n in what]
   effects = battle.get("effects", [])
   sources = effects if isinstance(effects, list) else []
   for src in sources:
      sp = path.parent / src
      try:
         text = sp.read_text(encoding="utf-8")
      except OSError as e:
         raise ManifestError(f"{sp}: cannot read ({e.strerror})") from None
      new_text = LOCK_NO_PP.sub(r"lock\1free", text)
      if new_text != text:
         writes.append((sp, new_text))
         notes.append(f"{sp.name}: lock ... no_pp -> lock ... free")
   if write and writes:
      write_all(writes)   # all of the battle's files or none
   return notes


def main(argv: Sequence[str]) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return process exit status
   """
   parser = argparse.ArgumentParser(prog="battle_resources.py", description=__doc__.splitlines()[0])
   parser.add_argument("manifests", nargs="+", type=Path, help="battle.json files to migrate")
   parser.add_argument("--check", action="store_true", help="report only, write nothing")
   args = parser.parse_args(argv[1:])
   status = EXIT_OK
   for path in args.manifests:
      try:
         notes = migrate_file(path, write=not args.check)
      except ManifestError as e:
         print(f"error: {e}", file=sys.stderr)
         status = EXIT_REFUSED
         continue
      verb = "would change" if args.check else "changed"
      print(f"{path}: {verb} " + ("; ".join(notes) if notes else "nothing (already migrated)"))
   return status


if __name__ == "__main__":
   sys.exit(main(sys.argv))
