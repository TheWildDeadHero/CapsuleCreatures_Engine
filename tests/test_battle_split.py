#!/usr/bin/env python3
"""!Tests for the split battle manifest (E5s): tools/battlec/manifest.py (content
in battle.json, structure in the structure manifest it names, the old single
file refused) and the migration tool tools/battle_split.py (run by CTest, or
directly)."""
from __future__ import annotations

import contextlib
import io
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler as bc  # noqa: E402
import battle_split as split  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## A data set the split tool wrote (the three-stat set; before E11 the Gen-3 sample, now in the reference repo).
FIXTURE = ROOT / "tests/data/battle_stats3/battle.json"
## Every engine battle manifest (samples and tests), all split.
MANIFESTS = [ROOT / "samples/starter/battle.json", ROOT / "tests/data/battle_stats3/battle.json",
             ROOT / "tests/data/battle_stats3/slots2.json", ROOT / "tests/data/effects/battle/battle.json",
             ROOT / "tests/data/effects/conditions/battle.json", ROOT / "tests/data/battle_tiers/battle.json",
             ROOT / "tests/data/battle_turn/battle.json", ROOT / "tests/data/battle_progress/battle.json",
             ROOT / "tests/data/battle_resources/battle.json", ROOT / "tests/data/battle_presenters/battle.json"]
## The manifests written by hand since the split (their own editor notes per file, or another folder's structure).
HAND_WRITTEN = [ROOT / "samples/starter/battle.json", ROOT / "tests/data/battle_presenters/battle.json"]


def single_file(path: Path) -> JsonDict:
   """!The single-file form (before E5s) of a split manifest: its merged view without "structure".
   @param path  battle.json
   @return the old manifest
   """
   merged = bc.view_of(path)
   del merged[bc.STRUCTURE_KEY]
   return merged


def copy_sample(folder: Path) -> Path:
   """!Copies the sample's ruleset and sources into a folder and writes its single-file manifest there.
   @param folder  an empty folder
   @return the written battle.json
   """
   shutil.copy(FIXTURE.parent / "ruleset.json", folder / "ruleset.json")
   for sub in ("effects", "sequences"):
      if (FIXTURE.parent / sub).is_dir():
         shutil.copytree(FIXTURE.parent / sub, folder / sub)
   path = folder / "battle.json"
   path.write_text(split.dump(single_file(FIXTURE)), encoding="utf-8")
   return path


def run_tool(*args: str) -> tuple[int, str, str]:
   """!Runs battle_split.main() with arguments.
   @param args  command-line arguments
   @return (exit status, stdout, stderr)
   """
   out, err = io.StringIO(), io.StringIO()
   with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
      try:
         status = split.main(["battle_split.py", *args])
      except SystemExit as e:
         status = int(e.code)
   return status, out.getvalue(), err.getvalue()


class SplitFormTests(unittest.TestCase):
   """!The compiler reads the split form and refuses the rest."""

   def compile_parts(self, battle: JsonDict, structure: JsonDict) -> JsonDict:
      """!Compiles the sample with its two manifests replaced in memory.
      @param battle     content manifest
      @param structure  structure manifest
      @return compiled
      """
      st = FIXTURE.parent / bc.DEFAULT_STRUCTURE_FILE
      return bc.compile_battle(FIXTURE, None, {FIXTURE.resolve(): battle, st.resolve(): structure})

   def parts(self) -> tuple[JsonDict, JsonDict]:
      """!The sample's two manifests as on disk.
      @return (battle.json, structure.json)
      """
      battle = json.loads(FIXTURE.read_text(encoding="utf-8"))
      structure = json.loads((FIXTURE.parent / battle[bc.STRUCTURE_KEY]).read_text(encoding="utf-8"))
      return battle, structure

   def test_every_data_set_is_split(self) -> None:
      """!Every engine manifest names a structure manifest; each file holds only its own keys.
      @return None
      """
      for path in MANIFESTS:
         with self.subTest(path=path.relative_to(ROOT).as_posix()):
            battle, structure, st_path = bc.read_parts(path)
            self.assertEqual(st_path.name, bc.structure_file_for(path.name))
            self.assertFalse(set(battle) & set(bc.STRUCTURE_KEYS))
            self.assertEqual(set(structure) - {"schema"} - set(bc.STRUCTURE_KEYS), set())
            self.assertEqual(structure["schema"], bc.STRUCTURE_SCHEMA)

   def test_files_are_the_tool_output(self) -> None:
      """!Re-splitting each data set's single-file form gives exactly the files on disk (the tool wrote them).
      @return None
      """
      for path in [m for m in MANIFESTS if m not in HAND_WRITTEN]:
         with self.subTest(path=path.relative_to(ROOT).as_posix()):
            battle, structure = bc.split_manifest(single_file(path), bc.structure_file_for(path.name))
            self.assertEqual(split.dump(battle), path.read_text(encoding="utf-8"))
            self.assertEqual(split.dump(structure),
                             (path.parent / bc.structure_file_for(path.name)).read_text(encoding="utf-8"))

   def test_deps_name_both_files(self) -> None:
      """!The depfile list holds the battle manifest, its structure manifest and the ruleset.
      @return None
      """
      deps: list[Path] = []
      bc.compile_battle(FIXTURE, deps)
      for name in ("battle.json", "structure.json", "ruleset.json"):
         self.assertIn((FIXTURE.parent / name).resolve(), deps)

   def test_single_file_refused(self) -> None:
      """!The old form (structure keys in battle.json, no "structure") is refused with the migration command.
      @return None
      """
      with self.assertRaisesRegex(bc.ManifestError, r"battle\.json: the battle structure \(monster_layout, stats"
                                                    r".*battle_split\.py .*structure\.json"):
         bc.compile_battle(FIXTURE, None, {FIXTURE.resolve(): single_file(FIXTURE)})

   def test_keys_in_the_wrong_file(self) -> None:
      """!A structure key in battle.json, a content key in structure.json, unknown and retired keys, schemas.
      @return None
      """
      battle, structure = self.parts()
      cases = [
         ({**battle, "stats": structure["stats"]}, structure,
          r"battle\.json: stats belong\(s\) in the structure manifest structure\.json"),
         (battle, {**structure, "moves": battle["moves"]},
          r"structure\.json: moves belong\(s\) in the battle manifest battle\.json"),
         (battle, {**structure, "colour": 1}, r"structure\.json: unknown key\(s\) colour"),
         ({**battle, "colour": 1}, structure, r"battle\.json: unknown key\(s\) colour"),
         (battle, {**structure, "stage_names": []}, r"structure\.json: stage_names is gone"),
         (battle, {**structure, "schema": "capsule.battle/1"}, r"structure\.json: schema must be capsule\.structure/1"),
         ({**battle, "structure": 3}, structure, r"battle\.json: structure must name a capsule\.structure/1 file"),
         ({k: v for k, v in battle.items() if k != "structure"}, structure,
          r"battle\.json: structure must name a capsule\.structure/1 file"),
      ]
      for b, s, pattern in cases:
         with self.subTest(pattern=pattern), self.assertRaisesRegex(bc.ManifestError, pattern):
            self.compile_parts(b, s)

   def test_missing_structure_file(self) -> None:
      """!A structure manifest that does not exist cannot be read.
      @return None
      """
      battle, _structure = self.parts()
      with self.assertRaisesRegex(bc.ManifestError, r"nowhere\.json: cannot read"):
         bc.compile_battle(FIXTURE, None, {FIXTURE.resolve(): {**battle, "structure": "nowhere.json"}})

   def test_structure_errors_name_the_structure_file(self) -> None:
      """!A section of the structure reports structure.json, the content battle.json.
      @return None
      """
      battle, structure = self.parts()
      with self.assertRaisesRegex(bc.ManifestError, r"^structure\.json: needs at least one format"):
         self.compile_parts(battle, {**structure, "formats": []})
      with self.assertRaisesRegex(bc.ManifestError, r"^structure\.json: monster_layout"):
         self.compile_parts(battle, {**structure, "monster_layout": {"roles": []}})
      with self.assertRaisesRegex(bc.ManifestError, r"^battle\.json: needs at least one type"):
         self.compile_parts({**battle, "types": []}, structure)

   def test_view_round_trip(self) -> None:
      """!sources_of() hands an edited merged view back as the two manifests.
      @return None
      """
      view = bc.view_of(FIXTURE)
      sources = bc.sources_of(FIXTURE, view)
      battle, structure = self.parts()
      self.assertEqual(sources, {FIXTURE.resolve(): battle,
                                 (FIXTURE.parent / "structure.json").resolve(): structure})


class SplitToolTests(unittest.TestCase):
   """!tools/battle_split.py."""

   def test_split_compiles_the_same(self) -> None:
      """!The tool splits the single-file sample; the split data compiles to the same C++ as the sample.
      @return None
      """
      with tempfile.TemporaryDirectory() as tmp:
         path = copy_sample(Path(tmp))
         status, out, _err = run_tool(str(path))
         self.assertEqual(status, split.EXIT_OK)
         self.assertIn("to structure.json", out)
         self.assertEqual(path.read_text(encoding="utf-8"), FIXTURE.read_text(encoding="utf-8"))
         compiled = bc.compile_battle(path)
      sample = bc.compile_battle(FIXTURE)
      self.assertEqual(bc.emit("battle.json", compiled), bc.emit("battle.json", sample))
      self.assertEqual(bc.emit_ids("battle.json", compiled), bc.emit_ids("battle.json", sample))

   def test_key_order_and_editor_data(self) -> None:
      """!"structure" follows "ruleset" (else "id", else last); editor data and unknown keys stay in battle.json.
      @return None
      """
      merged = {"schema": bc.BATTLE_SCHEMA, "id": "x", "editor": {"notes": "n"}, "stats": [], "ruleset": "r.json",
                "moves": [], "odd": 1, "terms": {}}
      battle, structure = bc.split_manifest(merged, "structure.json")
      self.assertEqual(list(battle), ["schema", "id", "editor", "ruleset", "structure", "moves", "odd"])
      self.assertEqual(list(structure), ["schema", "stats", "terms"])
      battle, _structure = bc.split_manifest({k: v for k, v in merged.items() if k != "ruleset"}, "s.json")
      self.assertEqual(list(battle)[:3], ["schema", "id", "structure"])
      battle, _structure = bc.split_manifest({"schema": bc.BATTLE_SCHEMA}, "s.json")
      self.assertEqual(list(battle), ["schema", "structure"])

   def test_other_names(self) -> None:
      """!slots2.json gets slots2.structure.json; --structure picks the name; --check writes nothing.
      @return None
      """
      self.assertEqual(bc.structure_file_for("battle.json"), "structure.json")
      self.assertEqual(bc.structure_file_for("slots2.json"), "slots2.structure.json")
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         path = folder / "slots2.json"
         text = split.dump(single_file(FIXTURE))
         path.write_text(text, encoding="utf-8")
         status, out, _err = run_tool(str(path), "--check")
         self.assertEqual((status, path.read_text(encoding="utf-8")), (split.EXIT_OK, text))
         self.assertIn("would move", out)
         self.assertFalse((folder / "slots2.structure.json").exists())
         self.assertEqual(run_tool(str(path))[0], split.EXIT_OK)
         self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["structure"], "slots2.structure.json")
         self.assertTrue((folder / "slots2.structure.json").is_file())
         other = folder / "other.json"
         other.write_text(text, encoding="utf-8")
         self.assertEqual(run_tool(str(other), "--structure", "shared/s.json")[0], split.EXIT_REFUSED)  # no folder
         self.assertEqual(run_tool(str(other), "--structure", "s.json")[0], split.EXIT_OK)
         self.assertEqual(json.loads(other.read_text(encoding="utf-8"))["structure"], "s.json")

   def test_refusals(self) -> None:
      """!Already split, an existing structure file, another schema, unreadable files: nothing written, status 1;
      --structure with two manifests is a usage error.
      @return None
      """
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         path = copy_sample(folder)
         (folder / "structure.json").write_text("{}", encoding="utf-8")
         before = path.read_text(encoding="utf-8")
         status, _out, err = run_tool(str(path))
         self.assertEqual((status, path.read_text(encoding="utf-8")), (split.EXIT_REFUSED, before))
         self.assertIn("exists already", err)
         (folder / "structure.json").unlink()
         self.assertEqual(run_tool(str(path))[0], split.EXIT_OK)
         status, _out, err = run_tool(str(path))
         self.assertEqual(status, split.EXIT_REFUSED)
         self.assertIn("already split", err)
         ruleset = folder / "ruleset.json"
         status, _out, err = run_tool(str(ruleset))
         self.assertEqual(status, split.EXIT_REFUSED)
         self.assertIn("schema must be capsule.battle/1", err)
         self.assertEqual(run_tool(str(folder / "gone.json"))[0], split.EXIT_REFUSED)
         self.assertEqual(run_tool(str(path), str(ruleset), "--structure", "s.json")[0], split.EXIT_USAGE)


if __name__ == "__main__":
   unittest.main()
