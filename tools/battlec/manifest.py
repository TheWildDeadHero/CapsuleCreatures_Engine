"""!The files of a battle manifest (D-23 section 2, package E5s).

A game's battle data is two manifests:
  - battle.json (capsule.battle/1): the content (types, moves, species,
    passives, items, traits, parties, presenters, messages, the effect
    sources and the names their logic uses: texts, cues); it names the
    ruleset ("ruleset") and the structure manifest ("structure"), both paths
    relative to it;
  - structure.json (capsule.structure/1): the game's battle structure (the
    record layout, stats, stages, rules, the type chart, categories, the
    condition records, move flags, tags, growth, formats, the turn structure,
    progression and terms).

The compiler reads both into one view (read_battle()) whose keys are the
same as before the split, so every section reads its keys unchanged. A key
in the other file is refused with a message naming the right one, and the
old single-file form (structure keys in battle.json, no "structure") is
refused with the migration command (tools/battle_split.py, which uses
split_manifest()). The schemas are unreleased and renamed in place (D-23
open question 3): there is no compatibility reader.

A later package adds keys to STRUCTURE_KEYS (D-25: "resources"); the split
tool and the refusals follow the tuple.
"""
from __future__ import annotations

import json
from collections.abc import Mapping
from pathlib import Path

from .common import Json, ManifestError, Sources, check_keys, register_keys, strip

## Schema of the content manifest (battle.json).
BATTLE_SCHEMA = "capsule.battle/1"
## Schema of the structure manifest (structure.json).
STRUCTURE_SCHEMA = "capsule.structure/1"
## battle.json key naming the structure manifest (a path relative to battle.json).
STRUCTURE_KEY = "structure"
## battle.json key naming the ruleset (a path relative to battle.json).
RULESET_KEY = "ruleset"
## File name of the structure manifest next to a battle.json.
DEFAULT_STRUCTURE_FILE = "structure.json"
## The content manifest's usual file name (its structure manifest is DEFAULT_STRUCTURE_FILE).
DEFAULT_BATTLE_FILE = "battle.json"
## Suffix of the structure manifest of a content manifest with another name (slots2.json -> slots2.structure.json).
STRUCTURE_SUFFIX = ".structure.json"
## Keys of the structure manifest besides "schema", in the order the split tool writes them when the merged
## manifest does not order them (it keeps the merged manifest's order).
STRUCTURE_KEYS = ("monster_layout", "stats", "stages", "rules", "chart", "categories", "statuses", "volatiles",
                  "weathers", "side_conditions", "field_conditions", "move_flags", "tags", "growth", "formats",
                  "turn", "learn", "exp_policy", "generation", "terms", "resources", "category_by_type",
                  "passive_slots", "species_fields", "hit_category_by_type", "pockets", "delayed_hits",
                  "recruit", "storage", "transformation", "training", "weather_suppress_tag",
                  # F1 (D-27): the lead rule and the timed field effects
                  "lead", "field_effects",
                  # F2 (D-27 section 3): the field actions
                  "field_actions",
                  # F3 (D-27 section 4): the step effects
                  "step_effects",
                  # B1 (D-28 section 1): the combination methods
                  "combine_methods",
                  # B2 (D-28 sections 2 and 3): eggs and the nursery
                  "eggs", "nursery",
                  # C9: what a build does with a transferred egg its layout has no egg role for
                  "transfer_eggs")
## Keys of the content manifest.
BATTLE_KEYS = ("schema", "id", RULESET_KEY, STRUCTURE_KEY, "types", "moves", "passives", "items", "traits",
               "species", "presenters", "parties", "messages", "effects", "texts", "cues", "shops", "ai_profiles")
## Keys refused in either file, with what replaced them.
RETIRED_KEYS = {
   "stage_names": "stage_names is gone (E6a): each stage of \"stages\" has its \"label\" (and its name, kind, range "
                  "and table)",
}
## The command that splits a single-file manifest (run from the engine's folder).
MIGRATION_COMMAND = "python3 tools/battle_split.py"

register_keys({
   "battle": set(BATTLE_KEYS),
   "structure": {"schema"} | set(STRUCTURE_KEYS),
})


def structure_file_for(battle_name: str) -> str:
   """!The structure manifest's file name the split tool gives a content manifest.
   @param battle_name  the content manifest's file name
   @return structure.json for battle.json, otherwise STEM.structure.json
   """
   if battle_name == DEFAULT_BATTLE_FILE:
      return DEFAULT_STRUCTURE_FILE
   return Path(battle_name).stem + STRUCTURE_SUFFIX


def read_json(path: Path, sources: Sources | None) -> Json:
   """!A manifest from the editors' sources or from its file, editor data stripped.
   @param path     resolved path
   @param sources  {Path: data} replacing files, or None
   @return the data
   @throws ManifestError when the file cannot be read or is not JSON
   """
   if sources is not None and path in sources:
      return strip(sources[path])
   try:
      return strip(json.loads(path.read_text(encoding="utf-8")))
   except OSError as e:
      raise ManifestError(f"{path}: cannot read ({e.strerror})") from None
   except json.JSONDecodeError as e:
      raise ManifestError(f"{path}: invalid JSON ({e})") from None


def _check_retired(data: Mapping[str, Json], where: str) -> None:
   """!Refuses keys that are gone.
   @param data   a manifest
   @param where  its file name
   @return None
   """
   for key, why in RETIRED_KEYS.items():
      if key in data:
         raise ManifestError(f"{where}: {why}")


def structure_path_of(battle: Mapping[str, Json], battle_path: Path) -> Path:
   """!The structure manifest a content manifest names, refusing the single-file form.
   @param battle       the content manifest (schema checked)
   @param battle_path  its resolved path
   @return the structure manifest's resolved path
   @throws ManifestError when it names none (with the migration command when structure keys are inline)
   """
   where = battle_path.name
   name = battle.get(STRUCTURE_KEY)
   if name is None:
      inline = [k for k in STRUCTURE_KEYS if k in battle]
      if inline:
         raise ManifestError(
            f"{where}: the battle structure ({', '.join(inline)}) moved to its own manifest, "
            f"{STRUCTURE_SCHEMA} (E5s); split this file with `{MIGRATION_COMMAND} {battle_path}` (it writes "
            f"{structure_file_for(where)} and names it under \"{STRUCTURE_KEY}\")")
      raise ManifestError(f"{where}: {STRUCTURE_KEY} must name a {STRUCTURE_SCHEMA} file")
   if not isinstance(name, str) or not name:
      raise ManifestError(f"{where}: {STRUCTURE_KEY} must name a {STRUCTURE_SCHEMA} file")
   return (battle_path.parent / name).resolve()


def read_parts(path: str | Path, sources: Sources | None = None) -> tuple[Json, Json, Path]:
   """!Reads a content manifest and the structure manifest it names, each checked on its own (schema, keys in
   the right file, no unknown keys); nothing else is compiled.
   @param path     battle.json
   @param sources  {Path: data} replacing files (editors' unsaved documents), or None
   @return (content manifest, structure manifest, structure path)
   @throws ManifestError
   """
   path = Path(path).resolve()
   where = path.name
   battle = read_json(path, sources)
   if not isinstance(battle, dict) or battle.get("schema") != BATTLE_SCHEMA:
      raise ManifestError(f"{where}: schema must be {BATTLE_SCHEMA}")
   _check_retired(battle, where)
   st_path = structure_path_of(battle, path)
   moved = [k for k in STRUCTURE_KEYS if k in battle]
   if moved:
      raise ManifestError(f"{where}: {', '.join(moved)} belong(s) in the structure manifest "
                          f"{Path(battle[STRUCTURE_KEY]).name} ({STRUCTURE_SCHEMA})")
   check_keys(battle, "battle", where)
   swhere = st_path.name
   structure = read_json(st_path, sources)
   if not isinstance(structure, dict) or structure.get("schema") != STRUCTURE_SCHEMA:
      raise ManifestError(f"{swhere}: schema must be {STRUCTURE_SCHEMA}")
   _check_retired(structure, swhere)
   content = [k for k in BATTLE_KEYS if k in structure and k != "schema"]
   if content:
      raise ManifestError(f"{swhere}: {', '.join(content)} belong(s) in the battle manifest {where} "
                          f"({BATTLE_SCHEMA})")
   check_keys(structure, "structure", swhere)
   return battle, structure, st_path


def merge(battle: Mapping[str, Json], structure: Mapping[str, Json]) -> Json:
   """!The one view the compiler's sections read: the content manifest's keys and the structure's (without its
   schema). The two key sets are disjoint (read_parts()).
   @param battle     content manifest
   @param structure  structure manifest
   @return the merged dict
   """
   return {**battle, **{k: v for k, v in structure.items() if k != "schema"}}


def read_battle(path: str | Path, sources: Sources | None = None,
                deps: list[Path] | None = None) -> tuple[Json, Path]:
   """!The merged view of a battle manifest and its structure (read_parts() + merge()).
   @param path     battle.json
   @param sources  {Path: data} replacing files, or None
   @param deps     list collecting the files read (both), or None
   @return (merged view, structure path)
   @throws ManifestError
   """
   battle, structure, st_path = read_parts(path, sources)
   if deps is not None:
      deps += [Path(path).resolve(), st_path]
   return merge(battle, structure), st_path


def split_manifest(merged: Mapping[str, Json], structure_name: str) -> tuple[Json, Json]:
   """!Splits a single-file manifest (the form before E5s) into the content manifest and the structure manifest.
   Keys keep their order within each file; the content manifest names the structure right after its ruleset
   (or after its id). Editor data ("editor") stays with the content manifest; keys neither file knows stay with
   the content manifest, where the compiler reports them.
   @param merged          the old battle.json
   @param structure_name  the structure manifest's path relative to the content manifest
   @return (content manifest, structure manifest)
   @throws ManifestError when the manifest is not capsule.battle/1 or already names a structure
   """
   if not isinstance(merged, Mapping) or merged.get("schema") != BATTLE_SCHEMA:
      raise ManifestError(f"schema must be {BATTLE_SCHEMA}")
   if STRUCTURE_KEY in merged:
      raise ManifestError(f"already split: it names the structure manifest {merged[STRUCTURE_KEY]!r}")
   battle: dict[str, Json] = {}
   structure: dict[str, Json] = {"schema": STRUCTURE_SCHEMA}
   for key, value in merged.items():
      if key in STRUCTURE_KEYS:
         structure[key] = value
         continue
      battle[key] = value
      # the structure is named next to the ruleset, the other file the manifest names
      if key == RULESET_KEY:
         battle[STRUCTURE_KEY] = structure_name
   if STRUCTURE_KEY not in battle:
      ordered: dict[str, Json] = {}
      for key, value in battle.items():
         ordered[key] = value
         if key == "id":
            ordered[STRUCTURE_KEY] = structure_name
      if STRUCTURE_KEY not in ordered:
         ordered[STRUCTURE_KEY] = structure_name
      battle = ordered
   return battle, structure


def view_of(path: str | Path) -> Json:
   """!The merged view of a split manifest on disk as written (editor data kept, nothing checked): what tools and
   tests edit before handing the result back through sources_of().
   @param path  battle.json
   @return the merged dict (it keeps "structure")
   @throws ManifestError when a file cannot be read or names no structure manifest
   """
   path = Path(path).resolve()
   try:
      battle = json.loads(path.read_text(encoding="utf-8"))
      structure = json.loads(structure_path_of(battle, path).read_text(encoding="utf-8"))
   except OSError as e:
      raise ManifestError(f"{path}: cannot read ({e.strerror})") from None
   except json.JSONDecodeError as e:
      raise ManifestError(f"{path}: invalid JSON ({e})") from None
   return merge(battle, structure)


def sources_of(path: str | Path, view: Mapping[str, Json]) -> dict[Path, Json]:
   """!The in-memory sources (compile_battle()'s `sources`) of an edited merged view: the structure keys go to the
   structure manifest the view names (its schema kept), everything else to the content manifest.
   @param path  battle.json (the structure path is relative to it)
   @param view  a merged view (view_of(), edited)
   @return {battle path: content manifest, structure path: structure manifest}
   @throws ManifestError when the view names no structure manifest
   """
   path = Path(path).resolve()
   battle = {k: v for k, v in view.items() if k not in STRUCTURE_KEYS}
   structure = {"schema": STRUCTURE_SCHEMA, **{k: v for k, v in view.items() if k in STRUCTURE_KEYS}}
   return {path: battle, structure_path_of(battle, path): structure}
