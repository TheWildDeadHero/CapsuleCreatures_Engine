#!/usr/bin/env python3
"""!Tests for tools/encounter_compiler.py (run by CTest, or directly), on a
copy of the test world (tests/data/maps) and its battle data."""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import encounter_compiler as ec  # noqa: E402
import map_compiler as mc  # noqa: E402
sys.path.insert(0, str(ROOT / "tests"))
from test_map_compiler import copy_world  # noqa: E402

## A parsed JSON object (a manifest, or compiled output).
JsonDict = dict[str, Any]

## The test world.
FIXTURE = ROOT / "tests/data/maps"


class EncounterCompilerTests(unittest.TestCase):
   """!Encounter rules and output."""

   def setUp(self) -> None:
      """!Copies the world and the battle data beside it."""
      ## Temporary root (maps/ and battle/).
      self.root = Path(tempfile.mkdtemp(prefix="encounter_compiler_"))
      ## The world's folder.
      self.dir = copy_world(self.root)

   def tearDown(self) -> None:
      """!Removes the copy."""
      shutil.rmtree(self.root, ignore_errors=True)

   def load(self, rel: str) -> Any:
      """!Reads a file of the copy.
      @param rel  path below the world folder
      @return JSON
      """
      return json.loads((self.dir / rel).read_text(encoding="utf-8"))

   def save(self, rel: str, data: Any) -> None:
      """!Writes a file of the copy.
      @param rel   path below the world folder
      @param data  JSON
      """
      (self.dir / rel).write_text(json.dumps(data), encoding="utf-8")

   def compile(self, sources: dict[Path, JsonDict] | None = None) -> JsonDict:
      """!Compiles the copy's encounters.
      @param sources  in-memory manifests, or None
      @return compile_encounters() result
      """
      path = (self.dir / "world.json").resolve()
      w, _, classes, _, maps = mc.load_world(path, sources)
      return ec.compile_encounters(w, path, classes, maps, [], sources)

   def broken(self, rel: str, change: Callable[[JsonDict], object], needle: str) -> None:
      """!Asserts an edit of one file makes compiling fail with a message (the file is put back after).
      @param rel     file below the world folder
      @param change  function changing its JSON in place
      @param needle  text the message must contain
      """
      original = (self.dir / rel).read_text(encoding="utf-8")
      data = json.loads(original)
      change(data)
      self.save(rel, data)
      try:
         with self.assertRaises(ec.ManifestError) as cm:
            self.compile()
         self.assertIn(needle, str(cm.exception))
      finally:
         (self.dir / rel).write_text(original, encoding="utf-8")

   def test_fixture(self) -> None:
      """!The fixture compiles: methods, conditions, refs sorted by slot, shared table used."""
      enc = self.compile()
      self.assertEqual([m["name"] for m in enc["methods"]], ["walk", "swim", "old lure"])
      self.assertEqual(enc["methods"][1]["vehicles"], 2)
      self.assertEqual(enc["methods"][2]["trigger"], 1)
      self.assertEqual(enc["conditions"], ["night", "rain"])
      self.assertEqual([(s, m) for s, m, _ in enc["refs"]["field"]], [(0, 0), (1, 0), (2, 1), (2, 2)])
      self.assertEqual(enc["refs"]["small"], [])
      self.assertEqual(len(enc["tables"]), 4)
      grass = enc["tables"][enc["refs"]["field"][0][2]]
      self.assertEqual((grass[0], grass[1]), (1, 1))
      fern = grass[2][0]
      self.assertEqual(enc["species"][fern[0]], "Sproutle")
      self.assertEqual((fern[1], fern[2], fern[3], fern[5]), (3, 5, 60, 1))   # excludes night (bit 0)
      self.assertEqual(grass[2][2][6], ((1, 0),))                             # rain x0

   def test_species_follow_battle_order(self) -> None:
      """!Species indices are the battle compiler's (sorted by key)."""
      sys.path.insert(0, str(ROOT / "tools"))
      import battle_compiler as bc
      c = bc.compile_battle(self.root / "battle/battle.json")
      self.assertEqual(self.compile()["species"], [s["name"] for s in c["species"]])

   def test_identical_tables_stored_once(self) -> None:
      """!An inline table equal to another (or to a shared one) is stored once."""
      f = self.load("encounters/field.json")
      f["tables"].append({"slot": 3, "method": "walk", "rate": "1/1",
                          "entries": [{"species": "Tidepup", "levels": [10, 12], "weight": 1}]})
      self.save("encounters/field.json", f)
      enc = self.compile()
      self.assertEqual(len(enc["tables"]), 4)
      refs = {s: t for s, m, t in enc["refs"]["field"] if m in (0, 1)}
      self.assertEqual(refs[3], refs[2])

   def test_rates(self) -> None:
      """!Rates parse as P-5 odds."""
      self.assertEqual(ec.rate("1/144", "t"), (1, 144))
      self.assertEqual(ec.rate("12.5%", "t"), (1, 8))
      self.assertEqual(ec.rate([20, 2880], "t"), (20, 2880))
      self.assertEqual(ec.rate(1, "t"), (1, 1))
      for bad in ("2/1", [3, 2], "fast", None, [1, 0]):
         with self.assertRaises(ec.ManifestError):
            ec.rate(bad, "t")
      self.assertEqual(ec.q8(3.0, "m"), 768)
      self.assertEqual(ec.q8("1/2", "m"), 128)
      with self.assertRaises(ec.ManifestError):
         ec.q8(200, "m")

   def test_in_memory_sources(self) -> None:
      """!Editors' unsaved encounter files are compiled instead of the files."""
      path = (self.dir / "encounters/field.json").resolve()
      f = self.load("encounters/field.json")
      f["tables"] = f["tables"][:1]
      enc = self.compile({path: f})
      self.assertEqual(len(enc["refs"]["field"]), 1)

   def test_level_max_from_the_battle_rules(self) -> None:
      """!Levels are checked against rules.level_max of the battle's structure manifest (E5s); without one, the
      engine's 255 (E7); the single-file form is refused with the migration command."""
      with tempfile.TemporaryDirectory() as tmp:
         path = Path(tmp) / "battle.json"
         structure = Path(tmp) / "structure.json"
         path.write_text(json.dumps({"schema": "capsule.battle/1", "structure": "structure.json",
                                     "species": [{"name": "Sproutle"}]}), encoding="utf-8")
         structure.write_text(json.dumps({"schema": "capsule.structure/1", "rules": {"level_max": 50}}),
                              encoding="utf-8")
         deps: list[Path] = []
         self.assertEqual(ec.load_species(path, deps).level_max, 50)
         self.assertEqual(deps, [path.resolve(), structure.resolve()])
         structure.write_text(json.dumps({"schema": "capsule.structure/1"}), encoding="utf-8")
         self.assertEqual(ec.load_species(path).level_max, ec.DEFAULT_LEVEL_MAX)
         path.write_text(json.dumps({"schema": "capsule.battle/1", "species": [{"name": "Sproutle"}],
                                     "rules": {"level_max": 50}}), encoding="utf-8")
         with self.assertRaisesRegex(ec.ManifestError, "battle_split.py"):
            ec.load_species(path)

   def test_errors(self) -> None:
      """!Every rule names its place."""
      field = "encounters/field.json"
      self.broken(field, lambda f: f["tables"][0]["entries"][0].update(species="Nobody"), "unknown species 'Nobody'")
      self.broken(field, lambda f: f["tables"][0].update(method="fly"), "unknown method 'fly'")
      self.broken(field, lambda f: f["tables"][0].update(slot=16), "slot")
      self.broken(field, lambda f: f["tables"][1].update(slot=0), "slot 0 already has a 'walk' table")
      self.broken(field, lambda f: f["tables"][0]["entries"][0].update(requires=["night"]),
                  "both required and excluded")
      self.broken(field, lambda f: f["tables"][0]["entries"][0].update(levels=[5, 3]), "levels")
      self.broken(field, lambda f: f["tables"][0]["entries"][0].update(levels=[1, 101]), "levels")
      self.broken(field, lambda f: f["tables"][0]["entries"][0].update(weight=0), "weight")
      self.broken(field, lambda f: f["tables"][0]["entries"][0].update(requires=["snow"]), "unknown condition 'snow'")
      self.broken(field, lambda f: f["tables"][0].pop("rate"), "needs a rate")
      self.broken(field, lambda f: f["tables"][2].update(table="nowhere"), "unknown shared table 'nowhere'")
      self.broken(field, lambda f: f["tables"][2].update(entries=[]), "not both")
      self.broken(field, lambda f: f["tables"][0].update(entries=f["tables"][0]["entries"] * 22), "up to 64 entries")
      self.broken(field, lambda f: f["tables"][0].update(colour="red"), "unknown key(s) colour")
      self.broken(field, lambda f: f.update(schema="capsule.encounters/0"), "schema")
      self.broken(field, lambda f: f["tables"][0]["entries"][0].update(
         weight=65535, modifiers=[{"when": "rain", "multiply": 127}] * 8), "P-18")
      world = "world.json"
      self.broken(world, lambda w: w.pop("battle"), "needs \"battle\"")
      self.broken(world, lambda w: w["encounter_methods"][0].update(vehicles=["boat"]), "vehicles")
      self.broken(world, lambda w: w["encounter_methods"][2].update(vehicles=["foot"]), "step methods only")
      self.broken(world, lambda w: w["encounter_methods"][1].update(name="walk"), "unique name")
      self.broken(world, lambda w: w.update(encounter_conditions=["x"] * 2), "unique")
      self.broken(world, lambda w: w.update(encounter_conditions=[f"c{i}" for i in range(33)]), "up to 32")
      self.broken("encounters/shared.json", lambda f: f["tables"][0].pop("name"), "unique name")

   def test_default_method_and_no_encounters(self) -> None:
      """!A world without encounter keys compiles to the default method and no tables."""
      w = self.load("world.json")
      for k in ("battle", "encounter_methods", "encounter_conditions", "encounter_tables"):
         w.pop(k)
      self.save("world.json", w)
      m = self.load("maps/field.json")
      m.pop("encounters")
      self.save("maps/field.json", m)
      enc = self.compile()
      self.assertEqual([x["name"] for x in enc["methods"]], ["step"])
      self.assertEqual(enc["tables"], [])

   def test_ids_and_emit(self) -> None:
      """!Identifiers and C++ output."""
      enc = self.compile()
      ids = ec.emit_ids("world.json", enc)
      self.assertIn("ENCOUNTER_METHOD_OLD_LURE = 2;", ids)
      self.assertIn("ENCOUNTER_CONDITION_RAIN = 1;", ids)
      text = "\n".join(ec.emit(enc, True))
      self.assertIn("encounter::Trigger::Script", text)
      self.assertIn('"Tidepup"', text)
      self.assertEqual(ec.emit_refs(enc, "small", 1), ([], "nullptr", 0))


if __name__ == "__main__":
   unittest.main()
