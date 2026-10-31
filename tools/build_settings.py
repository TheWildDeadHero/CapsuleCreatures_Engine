#!/usr/bin/env python3
"""Per-game runtime build settings (A-2, A-5): the "build" object of a game's
reference/project.json (capsule.project/1).

    build_settings.py <project.json> [--set key=value ...] [--cmake]

    "build": {"vram_must_land_slots": 32, "vram_streaming_slots": 64,
              "vram_budget_cycles": 67392, "gba_waitcnt": "0x4014",
              "screen_arena_bytes": 32768}

Every key is optional; an absent key keeps the engine's default. This module
is the one authority on the keys, their ranges and the C++ definitions they
become: CMake runs it while configuring each game (capsule_game()), and the
studio's build settings panel imports it.

--set gives a value from the command line (CMake passes non-empty cache
variables this way, so a cache variable overrides the manifest). --cmake
prints the compile definitions as a CMake list (NAME=value;...). Without
--cmake it prints the effective settings. Exit status 1 with a message on
stderr when a value is invalid.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Mapping
from os import PathLike
from pathlib import Path
from typing import Any

## Cycles in one VBlank (68 lines of 1,232 cycles); vram_queue.h kVBlankCycles.
VBLANK_CYCLES = 68 * 1232
## Cost of the largest streaming piece (4 KB from ROM, 32-bit DMA: 64 + 1024 x 6);
## vram_queue.h requires it to be under a quarter of the budget.
PIECE_CYCLES = 64 + (4096 // 4) * 6
## Largest ring: each slot is a 12-byte request in IWRAM (32 KB, shared with the stack),
## so two rings of 128 take 3 KB.
MAX_SLOTS = 128
## Wait-state choices per WAITCNT field: (name, bit shift, bit width, cycles per value).
WAITCNT_FIELDS = [
   ("sram", 0, 2, [4, 3, 2, 8]),
   ("ws0_first", 2, 2, [4, 3, 2, 8]),
   ("ws0_second", 4, 1, [2, 1]),
   ("ws1_first", 5, 2, [4, 3, 2, 8]),
   ("ws1_second", 7, 1, [4, 1]),
   ("ws2_first", 8, 2, [4, 3, 2, 8]),
   ("ws2_second", 10, 1, [8, 1]),
   ("phi", 11, 2, ["off", "4.19 MHz", "8.38 MHz", "16.78 MHz"]),
   ("prefetch", 14, 1, ["off", "on"]),
]
## WAITCNT bits that must be 0: bit 13 (unused) and bit 15 (game pak type, read-only).
WAITCNT_RESERVED_BITS = 0xA000
## Prefix of a hex value in a string.
HEX_PREFIX = "0x"
## Base of a hex value.
HEX_BASE = 16
## Base of a decimal value.
DECIMAL_BASE = 10
## UM (D-33): the screen arena's blocks start on a word (capsule::screen::arena::kAlign), so its size is a multiple.
ARENA_ALIGN = 4
## UM: the smallest screen arena (1 KB).
ARENA_MIN_BYTES = 1024
## UM: the largest screen arena (192 KB of the GBA's 256 KB EWRAM; the field, the save and the game's other state need
## the rest).
ARENA_MAX_BYTES = 192 * 1024
## UM: the screen arena's default (32 KB): an engine game's few screens; a game with deep screen chains sets its own.
ARENA_DEFAULT_BYTES = 32 * 1024


class SettingsError(Exception):
   """!A setting is unknown or out of range; the message says why."""
   pass


class Setting:
   """!One build setting."""

   def __init__(self, key: str, define: str, default: int, lo: int, hi: int, text: str, power_of_two: bool = False,
                hex_value: bool = False, multiple: int = 1) -> None:
      """!Describes a setting.
      @param key           key in the "build" object
      @param define        C++ definition it becomes
      @param default       engine default
      @param lo            lowest value
      @param hi            highest value
      @param text          one line for editors and errors
      @param power_of_two  value must be a power of two
      @param hex_value     shown and stored as a 0x string
      @param multiple      value must be a multiple of this (UM: 1 for any value)
      """
      ## Key in the "build" object.
      self.key = key
      ## C++ definition it becomes.
      self.define = define
      ## Engine default.
      self.default = default
      ## Lowest value.
      self.lo = lo
      ## Highest value.
      self.hi = hi
      ## One line for editors and errors.
      self.text = text
      ## Value must be a power of two.
      self.power_of_two = power_of_two
      ## Shown and stored as a 0x string.
      self.hex_value = hex_value
      ## Value must be a multiple of this.
      self.multiple = multiple

   def parse(self, raw: object) -> int:
      """!Checks a value.
      @param raw  int, or a decimal or 0x string
      @return the int value
      @throws SettingsError when it is not valid
      """
      if isinstance(raw, bool) or not isinstance(raw, (int, str)):
         raise SettingsError(f"{self.key}: expected a number, got {raw!r}")
      if isinstance(raw, str):
         text = raw.strip()
         try:
            is_hex = text[:len(HEX_PREFIX)].lower() == HEX_PREFIX
            value = int(text, HEX_BASE) if is_hex else int(text, DECIMAL_BASE)
         except ValueError:
            raise SettingsError(f"{self.key}: {raw!r} is not a number") from None
      else:
         value = raw
      if not self.lo <= value <= self.hi:
         raise SettingsError(f"{self.key}: {self.show(value)} is outside {self.show(self.lo)}..{self.show(self.hi)}")
      # A power of two has one bit set, so clearing its lowest set bit leaves 0.
      if self.power_of_two and value & (value - 1):
         raise SettingsError(f"{self.key}: {value} is not a power of two")
      if value % self.multiple:
         raise SettingsError(f"{self.key}: {value} is not a multiple of {self.multiple}")
      if self.key == "gba_waitcnt" and value & WAITCNT_RESERVED_BITS:
         raise SettingsError(f"{self.key}: bits 13 and 15 of WAITCNT must be 0 ({self.show(value)})")
      return value

   def show(self, value: int) -> str:
      """!A value as written in manifests.
      @param value  int
      @return text
      """
      return f"0x{value:04X}" if self.hex_value else str(value)

   def stored(self, value: int) -> int | str:
      """!A value as stored in project.json.
      @param value  int
      @return int, or a 0x string for hex settings
      """
      return self.show(value) if self.hex_value else value


## Every setting, in display order.
SETTINGS = [
   Setting("vram_must_land_slots", "CAPSULE_VRAM_MUST_LAND_SLOTS", 32, 4, MAX_SLOTS,
           "VRAM queue: must-land ring slots (palettes, tilemap rows; all land at the next VBlank)",
           power_of_two=True),
   Setting("vram_streaming_slots", "CAPSULE_VRAM_STREAMING_SLOTS", 64, 4, MAX_SLOTS,
           "VRAM queue: streaming ring slots (tiles and large loads, 4 KB pieces while the budget lasts)",
           power_of_two=True),
   Setting("vram_budget_cycles", "CAPSULE_VRAM_BUDGET_CYCLES", VBLANK_CYCLES - 16384, 4 * (PIECE_CYCLES + 1),
           VBLANK_CYCLES,
           "VRAM queue: cycles of each VBlank for uploads (the rest is register and OAM copies and, later, "
           "the audio mix)"),
   Setting("gba_waitcnt", "CAPSULE_GBA_WAITCNT", 0x4014, 0, 0xFFFF,
           "GBA: WAITCNT at start-up (ROM and SRAM wait states, prefetch); the queue's cost table assumes "
           "the default ROM 3/1", hex_value=True),
   Setting("screen_arena_bytes", "CAPSULE_SCREEN_ARENA_BYTES", ARENA_DEFAULT_BYTES, ARENA_MIN_BYTES, ARENA_MAX_BYTES,
           "Screens: bytes of the screen arena (EWRAM on the GBA) the open screens borrow their buffers from; it "
           "holds the deepest chain of screens open at once (capsule/screen/arena.h)", multiple=ARENA_ALIGN),
]
## Settings by key.
BY_KEY = {s.key: s for s in SETTINGS}

## Engine limits per target platform (D-20 C, D-23 section 2): most stats per creature
## (capsule::battle::kMaxStats), the largest creature record (capsule::monster::kMaxRecordBytes), the most
## members of a party (capsule::monster::kMaxPartySize) and the most battler / move resources (D-25;
## capsule::battle::kMaxBattlerResources / kMaxMoveResources). "default" covers every platform but the GBA.
PLATFORM_LIMITS = {"gba": {"max_stats": 10, "max_record_bytes": 100, "max_party_size": 6, "max_battler_resources": 4,
                           "max_move_resources": 2, "max_passive_slots": 4,
                           # G2: the bag (capsule::inventory kMaxPockets / kMaxBagSlots) and the save image budget
                           # (the cartridge's 64 KB flash; capsule::save kSaveBudget)
                           "max_pockets": 8, "max_bag_slots": 256, "max_save_bytes": 65536,
                           # G3: the storage boxes (capsule::storage kMaxBoxes / kMaxBoxSlots / kMaxStorageBytes:
                           # 32 KB of EWRAM for the boxed records)
                           "max_boxes": 16, "max_box_slots": 512, "max_storage_bytes": 32768,
                           # B1 (D-28): the records one combination takes (capsule::combine::kMaxInputs)
                           "max_combine_inputs": 4,
                           # B2 (D-28 section 2): the nursery's slots (capsule::combine::kMaxNurserySlots)
                           "max_nursery_slots": 4},
                   "default": {"max_stats": 32, "max_record_bytes": 256, "max_party_size": 8,
                               "max_battler_resources": 8, "max_move_resources": 4, "max_passive_slots": 8,
                               "max_pockets": 16, "max_bag_slots": 1024, "max_save_bytes": 262144,
                               "max_boxes": 32, "max_box_slots": 2048, "max_storage_bytes": 131072,
                               "max_combine_inputs": 8, "max_nursery_slots": 8}}
## Platform names a build may give (CMake's CAPSULE_PLATFORMS, plus "default").
PLATFORMS = ["desktop", "headless", "gba", "dreamcast", "default"]
## Platform whose limits apply when none is named (a project's targets default to the GBA, the strictest).
DEFAULT_PLATFORM = "gba"


def platform_limits(platform: str | None = None) -> dict[str, int]:
   """!The engine limits of one target platform.
   @param platform  a PLATFORMS name, or None for DEFAULT_PLATFORM
   @return {"max_stats": n, "max_record_bytes": n, "max_party_size": n, "max_battler_resources": n (hit points
           included; capsule::battle::kMaxBattlerResources), "max_move_resources": n (kMaxMoveResources),
           "max_passive_slots": n (passive slots per species; kMaxPassiveSlots), "max_pockets" / "max_bag_slots"
           (the bag, G2: capsule::inventory::kMaxPockets / kMaxBagSlots), "max_save_bytes" (the save image budget,
           G2: capsule::save::kSaveBudget), "max_boxes" / "max_box_slots" / "max_storage_bytes" (the storage boxes,
           G3: capsule::storage::kMaxBoxes / kMaxBoxSlots / kMaxStorageBytes), "max_combine_inputs" (the records one
           combination takes, B1: capsule::combine::kMaxInputs), "max_nursery_slots" (the nursery's slots, B2:
           capsule::combine::kMaxNurserySlots)}
   @throws SettingsError for an unknown platform
   """
   name = DEFAULT_PLATFORM if platform is None else platform
   if name not in PLATFORMS:
      raise SettingsError(f"unknown platform {name!r} (platforms: {', '.join(PLATFORMS)})")
   return dict(PLATFORM_LIMITS[name if name in PLATFORM_LIMITS else "default"])


def decode_waitcnt(value: int) -> dict[str, int | str]:
   """!The fields of a WAITCNT value.
   @param value  int
   @return {field name: choice}
   """
   out = {}
   for name, shift, width, choices in WAITCNT_FIELDS:
      # The field's bits, shifted down and masked to its width, index its choices.
      out[name] = choices[(value >> shift) & ((1 << width) - 1)]
   return out


def encode_waitcnt(fields: Mapping[str, int | str]) -> int:
   """!A WAITCNT value from field choices.
   @param fields  {field name: choice} (missing fields are 0)
   @return int
   @throws SettingsError for an unknown field or choice
   """
   value = 0
   known = {f[0]: f for f in WAITCNT_FIELDS}
   for name, choice in fields.items():
      if name not in known:
         raise SettingsError(f"unknown WAITCNT field {name!r}")
      _, shift, _, choices = known[name]
      if choice not in choices:
         raise SettingsError(f"WAITCNT {name}: {choice!r} is not one of {choices}")
      value |= choices.index(choice) << shift
   return value


def parse(build: Any, overrides: Mapping[str, object] | None = None) -> dict[str, int]:
   """!Checks a "build" object.
   @param build      the object from project.json, or None
   @param overrides  {key: raw value} that win over it, or None
   @return {key: int} of the settings given (absent keys keep the default)
   @throws SettingsError for an unknown key or a bad value
   """
   if build is None:
      build = {}
   if not isinstance(build, dict):
      raise SettingsError('"build" must be an object')
   merged = dict(build)
   merged.update(overrides or {})
   out = {}
   for key, raw in merged.items():
      s = BY_KEY.get(key)
      if s is None:
         raise SettingsError(f"unknown build setting {key!r} (known: {', '.join(BY_KEY)})")
      out[key] = s.parse(raw)
   return out


def effective(values: Mapping[str, int]) -> dict[str, int]:
   """!Every setting with its value (given or default).
   @param values  result of parse()
   @return {key: int}
   """
   return {s.key: values.get(s.key, s.default) for s in SETTINGS}


def definitions(values: Mapping[str, int]) -> list[str]:
   """!C++ definitions for the given settings.
   @param values  result of parse()
   @return list of "NAME=value"
   """
   return [f"{BY_KEY[k].define}={BY_KEY[k].show(v)}" for k, v in values.items()]


def load(project_path: str | PathLike[str], overrides: Mapping[str, object] | None = None) -> dict[str, int]:
   """!Reads and checks a project manifest's build settings.
   @param project_path  reference/project.json
   @param overrides     {key: raw value}, or None
   @return {key: int}
   @throws SettingsError when the manifest or a value is bad
   """
   try:
      data = json.loads(Path(project_path).read_text(encoding="utf-8"))
   except (OSError, ValueError) as e:
      raise SettingsError(f"cannot read {project_path}: {e}") from e
   if not isinstance(data, dict):
      raise SettingsError(f"{project_path} is not a JSON object")
   return parse(data.get("build"), overrides)


def main(argv: list[str] | None = None) -> int:
   """!Command line.
   @param argv  arguments, or None for sys.argv
   @return exit status
   """
   p = argparse.ArgumentParser(description="Check a game's build settings.")
   p.add_argument("project", help="reference/project.json")
   p.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="override a setting")
   p.add_argument("--cmake", action="store_true", help="print compile definitions as a CMake list")
   args = p.parse_args(argv)
   overrides = {}
   for item in args.set:
      key, sep, value = item.partition("=")
      if not sep:
         print(f"{args.project}: --set needs KEY=VALUE, got {item!r}", file=sys.stderr)
         return 1
      overrides[key] = value
   try:
      values = load(args.project, overrides)
   except SettingsError as e:
      print(f"{args.project}: build settings: {e}", file=sys.stderr)
      return 1
   if args.cmake:
      sys.stdout.write(";".join(definitions(values)))
   else:
      for key, value in effective(values).items():
         mark = "" if key in values else "  (default)"
         print(f"{key} = {BY_KEY[key].show(value)}{mark}")
   return 0


if __name__ == "__main__":
   sys.exit(main())
