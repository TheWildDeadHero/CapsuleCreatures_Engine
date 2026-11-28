#!/usr/bin/env python3
"""!Tests for D-36 §3.1 G0: the party part of a record layout (tools/battlec/layout.py, "party": true) and the
layout-only compile (tools/layout_compiler.py): packing, box bytes, the hash, refusals, the platform's limits, and
the generated source compiling against the engine's headers. Small invented layouts."""
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

import layout_compiler as lc  # noqa: E402
from battlec.common import ManifestError  # noqa: E402
from battlec.layout import BITS_PER_BYTE, ROLE_NAMES, compile_layout  # noqa: E402

## The stats of the invented layouts.
STATS = ["hp", "attack", "defense"]
## Bytes of the invented layout's box part (species 32 + level 8 + a 24-bit custom role: 64 bits).
BOX_BYTES = 8
## Its party part (a 16-bit hp and three 16-bit stats from the next word: 64 bits).
PARTY_BYTES = 8
## The current hp role's bits.
HP_BITS = 16


def roles(party: bool = True) -> list[Any]:
   """!An invented layout's roles, with a party part or without.
   @param party  mark the party roles
   @return the role entries
   """
   return ["species_key",
           {"role": "current_hp", "party": party},
           "level",
           {"custom": "battle_stats", "width": 16, "count": len(STATS), "party": party},
           {"custom": "met", "width": 24}]


def layout(party: bool = True) -> dict[str, Any]:
   """!An invented v2 layout object.
   @param party  mark the party roles
   @return it
   """
   return {"party_size": 6, "move_slots": 4, "roles": roles(party)}


def compiled(party: bool = True) -> dict[str, Any]:
   """!The invented layout, compiled without the battle's required roles.
   @param party  mark the party roles
   @return compile_layout()'s result
   """
   return compile_layout(layout(party), "test", stat_count=len(STATS), battle=False)


class PartyPartTests(unittest.TestCase):
   """!"party": true packs roles after the box part, from the next word."""

   def test_packing(self) -> None:
      """!The box roles first in the manifest's order, the party roles from the next word, box_bytes."""
      lay = compiled()
      self.assertEqual(lay["box_bytes"], BOX_BYTES)
      self.assertEqual(lay["bytes"], BOX_BYTES + PARTY_BYTES)
      self.assertEqual(lay["party_roles"], ["current_hp", "battle_stats"])
      self.assertEqual(lay["roles"], ["species_key", "level", "met", "current_hp", "battle_stats"])
      custom = {c["name"]: c for c in lay["custom"]}
      # the party part starts where the box part's words end: hp there, the stats after it
      hp_offset = lay["offsets"][ROLE_NAMES.index("current_hp")]
      self.assertEqual(hp_offset, BOX_BYTES * BITS_PER_BYTE)
      self.assertEqual(custom["battle_stats"]["offset"], hp_offset + HP_BITS)

   def test_without_party_roles(self) -> None:
      """!A layout without party roles: box_bytes is the whole record and its hash is what it was."""
      plain = compiled(party=False)
      self.assertEqual(plain["box_bytes"], plain["bytes"])
      self.assertEqual(plain["party_roles"], [])
      old = compile_layout({"party_size": 6, "move_slots": 4,
                            "roles": ["species_key", "current_hp", "level",
                                      {"custom": "battle_stats", "width": 16, "count": len(STATS)},
                                      {"custom": "met", "width": 24}]}, "test", stat_count=len(STATS), battle=False)
      self.assertEqual(plain["hash"], old["hash"])
      # the party part changes the hash and the record's own hash
      self.assertNotEqual(compiled()["hash"], plain["hash"])
      self.assertNotEqual(compiled()["record_hash"], plain["record_hash"])

   def test_refusals(self) -> None:
      """!A party flag that is not a boolean, an unstored party role, and battle roles a battle layout needs."""
      bad = layout()
      bad["roles"][1] = {"role": "current_hp", "party": "yes"}
      with self.assertRaisesRegex(ManifestError, "party: true"):
         compile_layout(bad, "test", stat_count=len(STATS), battle=False)
      bad = layout()
      bad["roles"].append({"custom": "shiny", "width": 1, "stored": False, "party": True})
      with self.assertRaisesRegex(ManifestError, "unstored"):
         compile_layout(bad, "test", stat_count=len(STATS), battle=False)
      with self.assertRaisesRegex(ManifestError, "battles need"):
         compile_layout(layout(), "test", stat_count=len(STATS))


class LayoutCompilerTests(unittest.TestCase):
   """!tools/layout_compiler.py: the file, the platform's limits, the generated source."""

   def write(self, folder: Path, data: dict[str, Any]) -> Path:
      """!Writes a record layout file.
      @param folder  where
      @param data    its contents
      @return its path
      """
      path = folder / "layout.json"
      path.write_text(json.dumps(data), encoding="utf-8")
      return path

   def test_file(self) -> None:
      """!The schema, the stats and unknown keys are checked; a good file compiles."""
      with tempfile.TemporaryDirectory() as tmp:
         good = {"schema": lc.SCHEMA, "stats": STATS, "monster_layout": layout()}
         self.assertEqual(lc.compile_file(self.write(Path(tmp), good))["box_bytes"], BOX_BYTES)
         for bad, message in (({**good, "schema": "x"}, "schema"), ({**good, "stats": []}, "stats"),
                              ({**good, "extra": 1}, "unknown key")):
            with self.assertRaisesRegex(ManifestError, message):
               lc.compile_file(self.write(Path(tmp), bad))

   def test_platform_limit(self) -> None:
      """!A record over the GBA's limit is refused for the GBA (the default platform), not for the desktop."""
      wide = layout()
      wide["roles"].append({"custom": "padding", "width": 32, "count": 22})
      with tempfile.TemporaryDirectory() as tmp:
         path = self.write(Path(tmp), {"schema": lc.SCHEMA, "stats": STATS, "monster_layout": wide})
         lc.compile_file(path, "desktop")
         with self.assertRaisesRegex(ManifestError, "at most 100"):
            lc.compile_file(path)
         with redirect_stderr(io.StringIO()):
            self.assertEqual(lc.main([str(path), str(Path(tmp) / "out.cpp"), "--platform", "gba"]), lc.EXIT_ERROR)

   def test_source(self) -> None:
      """!The source defines kLayout with box_bytes, and compiles against the engine's headers."""
      with tempfile.TemporaryDirectory() as tmp:
         path = self.write(Path(tmp), {"schema": lc.SCHEMA, "stats": STATS, "monster_layout": layout()})
         out = Path(tmp) / "layout.cpp"
         self.assertEqual(lc.main([str(path), str(out)]), lc.EXIT_OK)
         text = out.read_text(encoding="utf-8")
         self.assertIn(f", {BOX_BYTES + PARTY_BYTES}, {BOX_BYTES}, {len(STATS)}, 4, 6, 2, 0, {{0}}, ", text)
         compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
         if compiler is None:
            self.skipTest("no C++ compiler")
         done = subprocess.run([compiler, "-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
                                f"-I{ROOT / 'include'}", str(out)], capture_output=True, text=True, check=False)
         self.assertEqual(done.returncode, 0, done.stderr)


## G4a: the invented storage (boxes, slots per box).
STORAGE_BOXES = 2
## Its slots per box.
STORAGE_SLOTS = 3
## 14 boxes of 30 slots and a box part of 80 bytes: 33,600 bytes, within the GBA's 36 KB.
GBA_FIT = (14, 30)
## 16 boxes of 30 such slots: 38,400 bytes, past it.
GBA_PAST = (16, 30)
## A custom role that makes the box part 80 bytes (8 + 18 words).
PADDING_WORDS = 18
## A party part byte the storage must not keep (G4a: read() leaves the party part 0).
PARTY_MARK = 0x5A
## The program that puts a record with a party part and reads it back (G4a: only the box part is kept).
STORAGE_PROGRAM = """
#include "capsule/storage/storage.h"
#include "storage_ids.h"
#include <cstdio>
using namespace capsule;
storage::Buffer<storage_ids::STORAGE_BYTES, storage_ids::STORAGE_USED_BYTES> g_buffer;
int main()
{
   storage::Storage st = storage::view(g_buffer);
   monster::Record rec{};
   for (int b = 0; b < monster::kLayout.record_bytes; ++b)
   {
      rec.bytes[b] = static_cast<uint8_t>(b < monster::kLayout.box_bytes ? b + 1 : %d);
   }
   const int last = storage::slot_count() - 1;
   if (!storage::fits(st) || !storage::put(st, last, rec))
   {
      return 1;
   }
   monster::Record out{};
   if (!storage::read(st, last, out))
   {
      return 2;
   }
   for (int b = 0; b < monster::kLayout.record_bytes; ++b)
   {
      if (out.bytes[b] != (b < monster::kLayout.box_bytes ? b + 1 : 0))
      {
         return 3;
      }
   }
   std::printf("%%u %%d\\n", static_cast<unsigned>(st.capacity), storage::slot_count());
   return 0;
}
"""


class StorageTests(unittest.TestCase):
   """!G4a: a project's storage boxes through tools/layout_compiler.py (--storage, --ids)."""

   def write(self, folder: Path, lay: dict[str, Any]) -> Path:
      """!Writes a record layout file.
      @param folder  where
      @param lay     its v2 layout object
      @return its path
      """
      path = folder / "layout.json"
      path.write_text(json.dumps({"schema": lc.SCHEMA, "stats": STATS, "monster_layout": lay}), encoding="utf-8")
      return path

   def test_sizes(self) -> None:
      """!Each slot keeps the box part; the sizes header and kStorage say so."""
      lay = compiled()
      st = lc.storage_of(lay, STORAGE_BOXES, STORAGE_SLOTS)
      self.assertEqual(st["record_bytes"], BOX_BYTES)
      self.assertEqual(st["bytes"], STORAGE_BOXES * STORAGE_SLOTS * BOX_BYTES)
      self.assertIn("const StorageData kStorage = {kBoxNames, 3, 2, 0};", lc.emit_cpp("layout.json", lay, st))
      self.assertNotIn("kStorage", lc.emit_cpp("layout.json", lay))
      ids = lc.emit_ids("layout.json", st)
      self.assertIn(f"inline constexpr uint32_t STORAGE_BYTES = {STORAGE_BOXES * STORAGE_SLOTS * BOX_BYTES};", ids)
      self.assertIn("inline constexpr int NUM_BOXES = 2;", ids)

   def test_gba_limit(self) -> None:
      """!420 slots of an 80-byte box part fit the GBA's 36 KB; 480 do not (the desktop takes them)."""
      wide = layout()
      wide["roles"].append({"custom": "padding", "width": 32, "count": PADDING_WORDS})
      with tempfile.TemporaryDirectory() as tmp:
         lay = lc.compile_file(self.write(Path(tmp), wide))
      self.assertEqual(lay["box_bytes"], 80)
      lc.storage_of(lay, *GBA_FIT)
      lc.storage_of(lay, *GBA_PAST, "desktop")
      with self.assertRaisesRegex(ManifestError, "max_storage_bytes"):
         lc.storage_of(lay, *GBA_PAST)

   def test_flags_together(self) -> None:
      """!--storage without --ids (or the reverse) is refused."""
      with tempfile.TemporaryDirectory() as tmp:
         path = self.write(Path(tmp), layout())
         out = str(Path(tmp) / "layout.cpp")
         with redirect_stderr(io.StringIO()):
            self.assertEqual(lc.main([str(path), out, "--storage", "2", "3"]), lc.EXIT_ERROR)
            self.assertEqual(lc.main([str(path), out, "--ids", str(Path(tmp) / "ids.h")]), lc.EXIT_ERROR)

   def test_program(self) -> None:
      """!Built with the engine's storage: a record put and read back keeps its box part, its party part is 0, and
      the buffer is every slot's box part."""
      compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
      if compiler is None:
         self.skipTest("no C++ compiler")
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         path = self.write(folder, layout())
         self.assertEqual(lc.main([str(path), str(folder / "layout.cpp"), "--storage", str(STORAGE_BOXES),
                                   str(STORAGE_SLOTS), "--ids", str(folder / "storage_ids.h")]), lc.EXIT_OK)
         (folder / "main.cpp").write_text(STORAGE_PROGRAM % PARTY_MARK, encoding="utf-8")
         exe = folder / "storage_check"
         built = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'include'}",
                                 f"-I{folder}", str(folder / "main.cpp"), str(folder / "layout.cpp"),
                                 str(ROOT / "src" / "storage" / "storage.cpp"), "-o", str(exe)],
                                capture_output=True, text=True, check=False)
         self.assertEqual(built.returncode, 0, built.stderr)
         ran = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
         self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
         slots = STORAGE_BOXES * STORAGE_SLOTS
         self.assertEqual(ran.stdout.split(), [str(slots * BOX_BYTES), str(slots)])


if __name__ == "__main__":
   unittest.main()
