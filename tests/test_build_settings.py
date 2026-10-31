#!/usr/bin/env python3
"""Tests for tools/build_settings.py: per-game build settings (A-2, A-5)."""
from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import build_settings as bs  # noqa: E402


class BuildSettings(unittest.TestCase):
   """Values, ranges, WAITCNT fields and the command line."""

   def test_defaults_match_the_headers(self) -> None:
      """The defaults and definitions are the ones the C++ headers use."""
      header = (ROOT / "include" / "capsule" / "hal" / "vram_queue.h").read_text()
      self.assertIn("#define CAPSULE_VRAM_MUST_LAND_SLOTS 32", header)
      self.assertIn("#define CAPSULE_VRAM_STREAMING_SLOTS 64", header)
      self.assertIn("#define CAPSULE_VRAM_BUDGET_CYCLES (68 * 1232 - 16384)", header)
      self.assertEqual(bs.BY_KEY["vram_budget_cycles"].default, 68 * 1232 - 16384)
      self.assertIn("#define CAPSULE_GBA_WAITCNT 0x4014", (ROOT / "src" / "hal" / "gba" / "hal_gba.cpp").read_text())
      # UM: the screen arena's size (src/screen/arena_storage.cpp), a multiple of its word alignment
      arena = (ROOT / "src" / "screen" / "arena_storage.cpp").read_text()
      self.assertIn("#define CAPSULE_SCREEN_ARENA_BYTES 32768", arena)
      self.assertEqual(bs.BY_KEY["screen_arena_bytes"].default, 32768)
      self.assertEqual(bs.parse({"screen_arena_bytes": "65536"}), {"screen_arena_bytes": 65536})
      for bad in (1026, 512, 300000):
         with self.assertRaises(bs.SettingsError):
            bs.parse({"screen_arena_bytes": bad})

   def test_cmake_lists_every_setting(self) -> None:
      """CMakeLists.txt pairs every key with its cache variable."""
      cmake = (ROOT / "CMakeLists.txt").read_text()
      block = re.search(r"set\(CAPSULE_BUILD_SETTINGS(.*?)\)", cmake, re.S).group(1).split()
      self.assertEqual(list(zip(block[0::2], block[1::2])), [(s.key, s.define) for s in bs.SETTINGS])

   def test_ranges(self) -> None:
      """Unknown keys, powers of two, budget and WAITCNT bits are checked."""
      self.assertEqual(bs.parse({"vram_must_land_slots": 16, "gba_waitcnt": "0x4317"}),
                       {"vram_must_land_slots": 16, "gba_waitcnt": 0x4317})
      for bad in ({"vram_must_land_slots": 24}, {"vram_streaming_slots": 2}, {"bogus": 1},
                  {"vram_budget_cycles": 1000}, {"vram_budget_cycles": 90000}, {"gba_waitcnt": "0x8000"},
                  {"gba_waitcnt": "fast"}, {"vram_must_land_slots": True}):
         with self.assertRaises(bs.SettingsError, msg=bad):
            bs.parse(bad)
      self.assertEqual(bs.parse({"vram_must_land_slots": 16}, {"vram_must_land_slots": "64"}),
                       {"vram_must_land_slots": 64})
      # The lowest budget is the lowest vram_queue.h's static_assert accepts
      # (a 4 KB ROM piece must cost under a quarter of it, integer division).
      lo = bs.BY_KEY["vram_budget_cycles"].lo
      self.assertTrue(bs.PIECE_CYCLES < lo // 4 and not bs.PIECE_CYCLES < (lo - 1) // 4)
      with self.assertRaises(bs.SettingsError):
         bs.parse({"vram_streaming_slots": 256})   # 12 B of IWRAM per slot: capped at 128

   def test_waitcnt_fields(self) -> None:
      """The default WAITCNT is prefetch on, ROM 3/1; fields round-trip."""
      f = bs.decode_waitcnt(0x4014)
      self.assertEqual((f["ws0_first"], f["ws0_second"], f["prefetch"], f["sram"]), (3, 1, "on", 4))
      self.assertEqual(bs.encode_waitcnt(f), 0x4014)
      with self.assertRaises(bs.SettingsError):
         bs.encode_waitcnt({"ws0_first": 5})

   def test_command_line(self) -> None:
      """--cmake prints definitions; a bad value fails with a message."""
      with tempfile.TemporaryDirectory() as d:
         p = Path(d) / "project.json"
         p.write_text(json.dumps({"schema": "capsule.project/1", "name": "x",
                                  "build": {"vram_streaming_slots": 128, "gba_waitcnt": 16404}}))
         out = subprocess.run([sys.executable, str(ROOT / "tools" / "build_settings.py"), str(p), "--cmake",
                               "--set", "vram_budget_cycles=60000"], capture_output=True, text=True)
         self.assertEqual(out.returncode, 0, out.stderr)
         self.assertEqual(sorted(out.stdout.split(";")), ["CAPSULE_GBA_WAITCNT=0x4014",
                                                          "CAPSULE_VRAM_BUDGET_CYCLES=60000",
                                                          "CAPSULE_VRAM_STREAMING_SLOTS=128"])
         out = subprocess.run([sys.executable, str(ROOT / "tools" / "build_settings.py"), str(p),
                               "--set", "vram_streaming_slots=100"], capture_output=True, text=True)
         self.assertEqual(out.returncode, 1)
         self.assertIn("not a power of two", out.stderr)


if __name__ == "__main__":
   unittest.main()
