#!/usr/bin/env python3
"""!Tests for D-36 §3.1 G1: a pocket's rules (tools/battlec/inventory.py: stacks, close_up, store and their flags),
the inventory-only compile (tools/inventory_compiler.py: the file's checks, the platform's limits, the generated
source compiling against the engine's headers, the pockets' ids) and the save image's Bag version 2 in
tools/save_layout.py (kept empty slots, version 1 for older images). The bag rules fixture (tests/data/bag_rules)
and small invented files."""
from __future__ import annotations

import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from typing import Any

## The engine's root.
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import inventory_compiler as ic  # noqa: E402
import save_layout as sl  # noqa: E402
from battlec.common import ManifestError  # noqa: E402
from battlec.inventory import POCKET_FLAG_BITS  # noqa: E402

## The bag rules fixture.
FIXTURE = ROOT / "tests" / "data" / "bag_rules" / "inventory.json"
## Its pockets' flags, in its order: loose (many stacks, keeps empty), single (keeps empty), sorted (many stacks),
## plain (none), chest (a store keeping empty slots, many stacks).
FIXTURE_FLAGS = [POCKET_FLAG_BITS["many_stacks"] | POCKET_FLAG_BITS["keeps_empty"], POCKET_FLAG_BITS["keeps_empty"],
                 POCKET_FLAG_BITS["many_stacks"], 0,
                 POCKET_FLAG_BITS["many_stacks"] | POCKET_FLAG_BITS["keeps_empty"] | POCKET_FLAG_BITS["store"]]


def fixture() -> dict[str, Any]:
   """!The fixture's file.
   @return its contents
   """
   return json.loads(FIXTURE.read_text(encoding="utf-8"))


class CompileTests(unittest.TestCase):
   """!The pockets' rules and the file's checks."""

   def compile(self, data: dict[str, Any], platform: str | None = "desktop") -> dict[str, Any]:
      """!Compiles a file's contents.
      @param data      the file
      @param platform  the platform
      @return compile_file()'s result
      """
      with tempfile.TemporaryDirectory() as tmp:
         path = Path(tmp) / "inventory.json"
         path.write_text(json.dumps(data), encoding="utf-8")
         return ic.compile_file(path, platform)

   def test_fixture(self) -> None:
      """!The fixture's flags, items and slots."""
      inv = self.compile(fixture())
      self.assertEqual([p["flags"] for p in inv["pockets"]], FIXTURE_FLAGS)
      self.assertEqual(len(inv["items"]), len(fixture()["items"]) + 1)
      self.assertEqual(inv["slot_count"], sum(p["capacity"] for p in fixture()["pockets"]))
      self.assertEqual(inv["items"][-1]["pocket"], 0xFF)   # the note has no pocket

   def test_refusals(self) -> None:
      """!An item naming a store, an unknown stacks rule, a non-boolean rule, an item use, a repeated name, the
      schema and an unknown key."""
      def edited(change: Any) -> dict[str, Any]:
         """!The fixture with one change.
         @param change  callable(file contents)
         @return the changed contents
         """
         data = fixture()
         change(data)
         return data
      cases = [
         (lambda d: d["items"][0].update(pocket="chest"), "is a store"),
         (lambda d: d["pockets"][0].update(stacks="few"), "stacks"),
         (lambda d: d["pockets"][0].update(close_up="no"), "close_up"),
         (lambda d: d["items"][0].update(use="field"), "unknown key"),
         (lambda d: d["items"].append({"name": "Pebble", "pocket": "loose"}), "given twice"),
         (lambda d: d.update(schema="x"), "schema"),
         (lambda d: d.update(extra=1), "unknown key"),
      ]
      for change, message in cases:
         with self.subTest(message=message), self.assertRaisesRegex(ManifestError, message):
            self.compile(edited(change))

   def test_platform(self) -> None:
      """!More slots than the GBA's bag holds: refused for the GBA, not the desktop."""
      data = fixture()
      data["pockets"][0]["capacity"] = 300
      self.compile(data)
      with self.assertRaisesRegex(ManifestError, "capacity"):
         self.compile(data, "gba")

   def test_source(self) -> None:
      """!The command line writes the tables and the pockets' ids; the source compiles with the engine's headers."""
      with tempfile.TemporaryDirectory() as tmp:
         out, ids = Path(tmp) / "inventory.cpp", Path(tmp) / "inventory_ids.h"
         self.assertEqual(ic.main([str(FIXTURE), str(out), "--ids", str(ids), "--platform", "desktop"]), ic.EXIT_OK)
         self.assertIn("static_cast<SortRule>(0), 3},   // loose", out.read_text(encoding="utf-8"))
         self.assertIn("inline constexpr uint8_t POCKET_CHEST = 4;", ids.read_text(encoding="utf-8"))
         with redirect_stderr(io.StringIO()):
            self.assertEqual(ic.main([str(Path(tmp) / "missing.json"), str(out)]), ic.EXIT_ERROR)
         compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
         if compiler is None:
            self.skipTest("no C++ compiler")
         done = subprocess.run([compiler, "-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
                                f"-I{ROOT / 'include'}", str(out)], capture_output=True, text=True, check=False)
         self.assertEqual(done.returncode, 0, done.stderr)


class BagSectionTests(unittest.TestCase):
   """!tools/save_layout.py's Bag version 2 and its sizes."""

   def setUp(self) -> None:
      """!The fixture compiled, in the shape save_layout reads (its inventory)."""
      with tempfile.TemporaryDirectory() as tmp:
         path = Path(tmp) / "inventory.json"
         path.write_text(json.dumps(fixture()), encoding="utf-8")
         inv = ic.compile_file(path, "desktop")
      self.compiled = {"inventory": inv}

   def test_payload(self) -> None:
      """!A pocket keeping empty slots keeps its holes; trailing empty slots are left out; keys and counts."""
      inv = self.compiled["inventory"]
      bag = {0: [(0, 0), (1, 3), (0, 0)], 3: [(7, 1)]}
      data = sl.bag_payload(self.compiled, bag)
      key = inv["items"][1]["save_key"]
      expected = (bytes([len(inv["pockets"])]) + sl.u16(2) + sl.u32(0) + sl.u16(0) + sl.u32(key) + sl.u16(3) +
                  sl.u16(0) + sl.u16(0) + sl.u16(1) + sl.u32(inv["items"][7]["save_key"]) + sl.u16(1) + sl.u16(0))
      self.assertEqual(data, expected)
      self.assertLessEqual(len(data), sl.BAG_HEAD_BYTES + len(inv["pockets"]) * sl.BAG_POCKET_BYTES +
                           inv["slot_count"] * sl.BAG_ENTRY_BYTES)

   def test_versions(self) -> None:
      """!assemble() writes version 2 by default and an older image's version 1 when asked."""
      v2 = sl.assemble({"bag": sl.bag_payload(self.compiled, {})})
      v1 = sl.assemble({"bag": sl.bag_entries_payload(self.compiled, {})}, versions={"bag": sl.BAG_VERSION_ENTRIES})
      table = sl.HEADER_BYTES
      self.assertEqual(v2[table + 1], sl.SECTION_VERSIONS["bag"])
      self.assertEqual(v1[table + 1], sl.BAG_VERSION_ENTRIES)


if __name__ == "__main__":
   unittest.main()
