#!/usr/bin/env python3
"""!The sample projects' contract (D-40 RP, docs/APPS.md "Sample projects"): an app with a sample.json
(capsule.sample/1) is one a user may copy as a game of their own (the toolkit's `capsule-studio sample copy`, the
Studio's Open a sample...). The engine keeps one copy of each sample, the app itself, so these checks are what keeps
a copy building outside the engine (ctest sample_project_tests):

  - the manifest: schema, id (the app folder's name), name, one-line description, version, "shared" folders that are
    battle data templates inside the engine (template.json), each copied to a folder the app does not use;
  - every path the app's JSON manifests write that leads out of the app folder leads to a file a shared folder's
    template lists (the copy points it at that file's copy), and no #include of its sources leads out;
  - its "libraries" are ones capsule_game() links for any game: CMakeLists.txt defines them for every platform
    with games, outside the engine's own test switches;
  - the engine builds it (capsule_game() of the app, a target the release's sample downloads name).
"""
from __future__ import annotations

import json
import re
import unittest
from pathlib import Path, PurePosixPath
from typing import Any

## The engine checkout.
ROOT = Path(__file__).resolve().parent.parent
## The engine's apps (each sample is one of them).
APPS = ROOT / "apps"
## A sample's manifest in its app folder.
SAMPLE_FILE = "sample.json"
## The sample manifest's schema and keys.
SCHEMA = "capsule.sample/1"
KEYS = {"schema", "id", "name", "description", "version", "shared", "editor"}
## A shared entry's keys.
SHARED_KEYS = {"from", "to"}
## A battle data template's manifest (the toolkit's core.templates) and its parts.
TEMPLATE_FILE = "template.json"
TEMPLATE_PARTS = ("structure", "logic", "content")
## A game's project manifest (its paths are relative to the game folder; other manifests' to their own folder).
PROJECT_FILE = Path("reference") / "project.json"
## The engine libraries every game may link (apps/common), as the toolkit's core.samples.GAME_LIBRARIES lists them.
GAME_LIBRARIES = {"test_kit", "battle_kit"}
## The samples the release ships (release.yml SAMPLE_TARGETS: capsule_<id>).
RELEASE_SAMPLES = {"starter_battle", "sandbox_map"}
## Lowest sample version.
FIRST_VERSION = 1
## Longest JSON text read as a path (the toolkit's core.samples.PATH_MAX_CHARS).
PATH_MAX_CHARS = 260
## A quoted #include line.
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)
## Suffixes of C and C++ files.
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc"}
## CMake lines that open and close a block (if / foreach / function / macro).
BLOCK_OPEN = re.compile(r"^\s*(if|foreach|function|macro|while)\s*\(", re.IGNORECASE)
BLOCK_CLOSE = re.compile(r"^\s*(endif|endforeach|endfunction|endmacro|endwhile)\s*\(", re.IGNORECASE)
## Switches of the engine's own apps and tests: a library defined only under one is not there for a user's game.
TEST_SWITCHES = ("CAPSULE_ENGINE_TESTS", "CAPSULE_TEST_APPS")

JsonDict = dict[str, Any]


def samples() -> list[Path]:
   """!The app folders that are samples.
   @return Paths, sorted
   """
   return sorted(p.parent for p in APPS.glob(f"*/{SAMPLE_FILE}"))


def load(path: Path) -> Any:
   """!A JSON file.
   @param path  the file
   @return its data
   """
   return json.loads(path.read_text(encoding="utf-8"))


def texts(value: Any) -> list[str]:
   """!Every text in a JSON value (keys left out).
   @param value  parsed JSON
   @return the texts, in order
   """
   if isinstance(value, dict):
      return [t for item in value.values() for t in texts(item)]
   if isinstance(value, list):
      return [t for item in value for t in texts(item)]
   return [value] if isinstance(value, str) else []


def relative_path(text: str) -> bool:
   """!Whether a text is a relative path as manifests write them.
   @param text  a JSON text
   @return True for a relative POSIX path that climbs ("..")
   """
   if not text or len(text) > PATH_MAX_CHARS or "\\" in text or "\n" in text or ":" in text:
      return False
   pure = PurePosixPath(text)
   return not pure.is_absolute() and ".." in pure.parts


def template_files(folder: Path) -> set[Path]:
   """!The files a battle data template lists.
   @param folder  the template's folder
   @return resolved Paths
   """
   data = load(folder / TEMPLATE_FILE)
   parts = data.get("parts", {})
   return {(folder / f).resolve() for part in TEMPLATE_PARTS for f in parts.get(part, [])}


def open_blocks(cmake: str, needle: str) -> list[str]:
   """!The blocks of a CMake file open where a text first appears.
   @param cmake   the file's text
   @param needle  the text, e.g. "add_library(capsule_test_kit "
   @return the opening lines, outermost first (empty: at the top level)
   @throws AssertionError when the text is not there
   """
   stack: list[str] = []
   for line in cmake.splitlines():
      if needle in line:
         return stack
      if BLOCK_CLOSE.match(line):
         stack.pop()
      elif BLOCK_OPEN.match(line):
         stack.append(line.strip())
   raise AssertionError(f"{needle!r} is not in CMakeLists.txt")


class SampleManifestTests(unittest.TestCase):
   """!sample.json of every sample."""

   def test_release_samples_are_samples(self) -> None:
      """!Both release samples have a sample.json."""
      self.assertEqual(RELEASE_SAMPLES, {p.name for p in samples()} & RELEASE_SAMPLES)

   def test_manifests(self) -> None:
      """!Schema, keys, id, name, one-line description, version, shared folders."""
      for folder in samples():
         with self.subTest(sample=folder.name):
            data = load(folder / SAMPLE_FILE)
            self.assertEqual(SCHEMA, data["schema"])
            self.assertLessEqual(set(data), KEYS)
            self.assertEqual(folder.name, data["id"])
            self.assertTrue(data["name"].strip())
            self.assertTrue(data["description"].strip())
            self.assertNotIn("\n", data["description"])
            self.assertIsInstance(data["version"], int)
            self.assertGreaterEqual(data["version"], FIRST_VERSION)
            self.assertTrue((folder / PROJECT_FILE).is_file())
            for entry in data.get("shared", []):
               self.assertEqual(SHARED_KEYS, set(entry))
               shared = (folder / entry["from"]).resolve()
               self.assertIn(ROOT, shared.parents, "a shared folder is the engine's")
               self.assertTrue((shared / TEMPLATE_FILE).is_file(), f"{shared} is a battle data template")
               to = PurePosixPath(entry["to"])
               self.assertFalse(to.is_absolute() or ".." in to.parts, f"{to} is inside the copy")
               self.assertFalse((folder / entry["to"]).exists(), f"{to} is not one of the app's own folders")


class SelfContainedTests(unittest.TestCase):
   """!What leads out of a sample's folder is only what its copy brings along."""

   def test_paths_lead_only_into_shared_files(self) -> None:
      """!Every JSON path leading out of the app reaches a file a shared template lists."""
      for folder in samples():
         data = load(folder / SAMPLE_FILE)
         shared: set[Path] = set()
         for entry in data.get("shared", []):
            shared |= template_files((folder / entry["from"]).resolve())
         app = folder.resolve()
         for manifest in sorted(folder.rglob("*.json")):
            if manifest.name == SAMPLE_FILE and manifest.parent == folder:
               continue
            base = folder if manifest == folder / PROJECT_FILE else manifest.parent
            for text in texts(load(manifest)):
               if not relative_path(text):
                  continue
               target = (base / text).resolve()
               if app in target.parents:
                  continue
               with self.subTest(sample=folder.name, file=str(manifest.relative_to(folder)), path=text):
                  self.assertIn(target, shared, "a path out of the sample must lead to a shared template file")

   def test_sources_include_nothing_outside(self) -> None:
      """!No quoted #include of a sample's sources leads out of its folder."""
      for folder in samples():
         app = folder.resolve()
         for source in sorted(p for p in folder.rglob("*") if p.suffix in SOURCE_SUFFIXES):
            for included in INCLUDE.findall(source.read_text(encoding="utf-8")):
               target = (source.parent / included).resolve()
               with self.subTest(sample=folder.name, source=source.name, include=included):
                  self.assertFalse(".." in PurePosixPath(included).parts and app not in target.parents)

   def test_libraries_are_there_for_every_game(self) -> None:
      """!A sample's libraries are GAME_LIBRARIES, which CMakeLists.txt defines outside its test switches."""
      cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
      for library in sorted(GAME_LIBRARIES):
         with self.subTest(library=library):
            blocks = open_blocks(cmake, f"add_library(capsule_{library} ")
            self.assertFalse([b for b in blocks if any(s in b for s in TEST_SWITCHES)], blocks)
      for folder in samples():
         with self.subTest(sample=folder.name):
            project = load(folder / PROJECT_FILE)
            self.assertLessEqual(set(project.get("libraries", [])), GAME_LIBRARIES)

   def test_the_engine_builds_each_sample(self) -> None:
      """!capsule_game() builds every sample app (by name or in the test apps' list)."""
      cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
      for folder in samples():
         with self.subTest(sample=folder.name):
            direct = f"capsule_game(${{CAPSULE_ROOT}}/apps/{folder.name})" in cmake
            listed = re.search(rf"foreach\(app [^)]*\b{folder.name}\b", cmake) is not None
            self.assertTrue(direct or listed)
            self.assertEqual(folder.name, load(folder / PROJECT_FILE)["name"], "capsule_<id> is the target")


if __name__ == "__main__":
   unittest.main()
