#!/usr/bin/env python3
"""Compile an input manifest into one target's InputMap table (B-11, P-1).

Usage: input_compiler.py <manifest.json> <target> <out.cpp> <symbol>
  target: gba | snes_layout | dreamcast

Rules (any violation is a build error):
  - every engine action is listed for the target, as a list of buttons or null;
  - buttons must exist on the target (no X/Y on GBA);
  - a button drives at most one action per target;
  - null (no button, offered as a main-menu entry) only for menu-capable actions;
  - each wheel action is bound to exactly one shoulder button (L or R), the two
    wheels to different shoulders, because the shoulder filter opens them;
  - wake keys exist on the target (one list for every target, or a list per
    target); frame counts are 1-255;
  - unknown keys are errors; "editor" and "_"-prefixed keys are stripped.
"""
from __future__ import annotations

import json
import sys
from collections.abc import Iterable
from pathlib import Path
from typing import Any

## A manifest object (parsed JSON).
JsonDict = dict[str, Any]

## Button names by bit position; must match capsule::hal::input::Key.
KEYS = ["A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"]
## Targets: generated InputTarget id and the buttons that exist there.
TARGETS = {
   "gba":         {"id": "INPUT_TARGET_GBA",         "keys": KEYS[:10]},
   "snes_layout": {"id": "INPUT_TARGET_SNES_LAYOUT", "keys": KEYS},
   # Dreamcast pad: A B X Y, Start, D-pad, analog triggers reported as L/R; no Select.
   "dreamcast":   {"id": "INPUT_TARGET_DREAMCAST",   "keys": [k for k in KEYS if k != "SELECT"]},
}
## Action names; order must match capsule::input::Action (checked by
## generated static_asserts).
ACTIONS = ["UP", "DOWN", "LEFT", "RIGHT", "ACCEPT", "BACK", "MAIN_MENU", "DEVICE_MENU",
           "TOGGLE_RUN", "HELP", "WHEEL_1", "WHEEL_2"]
## Actions that may be null (offered as a main-menu entry instead).
MENU_CAPABLE = {"TOGGLE_RUN", "HELP"}
## Actions driven by the shoulder filter.
WHEELS = {"WHEEL_1", "WHEEL_2"}
## Required keys of the manifest's "config" object.
CONFIG_KEYS = {"chord_window", "sleep_hold_frames", "reset_hold_frames", "wake_keys"}
## Largest frame count of the config (stored in one byte).
MAX_FRAMES = 255

## Number of command-line arguments, including the program name.
ARGUMENT_COUNT = 5
## Position of the manifest argument.
SOURCE_ARGUMENT = 1
## Position of the target argument.
TARGET_ARGUMENT = 2
## Position of the output file argument.
OUTPUT_ARGUMENT = 3
## Position of the symbol argument.
SYMBOL_ARGUMENT = 4


class ManifestError(Exception):
   """!A manifest rule was broken; the message says where and how."""
   pass


def strip(obj: JsonDict) -> JsonDict:
   """!Drops editor-only data (P-1 rule 2).
   @param obj  a manifest object (dict)
   @return a copy without "editor" and "_"-prefixed keys
   """
   return {k: v for k, v in obj.items() if k != "editor" and not k.startswith("_")}


def check_unknown(obj: JsonDict, allowed: Iterable[str], where: str) -> None:
   """!Rejects keys that are not allowed.
   @param obj      manifest object (dict)
   @param allowed  allowed keys
   @param where    location for the error message
   @throws ManifestError on an unknown key
   @return None
   """
   unknown = sorted(set(obj) - set(allowed))
   if unknown:
      raise ManifestError(f"{where}: unknown key(s): {', '.join(unknown)}")


def mask(buttons: Any, target: str, where: str) -> int:
   """!Button list to a KeyBits mask, checked against the target.
   @param buttons  list of button names
   @param target   target name (key of TARGETS)
   @param where    location for error messages
   @return the mask
   @throws ManifestError for a bad list or a button the target lacks
   """
   if not isinstance(buttons, list) or not buttons:
      raise ManifestError(f"{where}: expected a non-empty list of buttons or null")
   bits = 0
   for b in buttons:
      if b not in KEYS:
         raise ManifestError(f"{where}: unknown button {b!r}")
      if b not in TARGETS[target]["keys"]:
         raise ManifestError(f"{where}: button {b} does not exist on target {target}")
      bits |= 1 << KEYS.index(b)
   return bits


def compile_map(manifest: JsonDict, target: str) -> tuple[list[int], int, dict[str, int]]:
   """!Validates the manifest and compiles one target's map.
   @param manifest  parsed JSON manifest
   @param target    target name (key of TARGETS)
   @return (bindings per action, wake key mask, frame counts dict)
   @throws ManifestError on any rule violation (see the module docstring)
   """
   if target not in TARGETS:
      raise ManifestError(f"unknown target {target!r} (known: {', '.join(TARGETS)})")
   manifest = strip(manifest)
   check_unknown(manifest, {"config", "targets"}, "manifest")
   config = strip(manifest.get("config", {}))
   check_unknown(config, CONFIG_KEYS, "config")
   for k in CONFIG_KEYS:
      if k not in config:
         raise ManifestError(f"config: missing {k}")
   targets = strip(manifest.get("targets", {}))
   check_unknown(targets, TARGETS, "targets")
   if target not in targets:
      raise ManifestError(f"targets: no entry for {target}")
   actions = strip(targets[target])
   check_unknown(actions, ACTIONS, f"targets.{target}")

   bindings, owner = [], {}
   for action in ACTIONS:
      where = f"targets.{target}.{action}"
      if action not in actions:
         raise ManifestError(f"{where}: missing")
      value = actions[action]
      if value is None:
         if action not in MENU_CAPABLE:
            raise ManifestError(f"{where}: must have a button (only {', '.join(sorted(MENU_CAPABLE))} may be null)")
         bindings.append(0)
         continue
      bits = mask(value, target, where)
      # A wheel's mask must be exactly the L bit or exactly the R bit.
      if action in WHEELS and bits not in (1 << KEYS.index("L"), 1 << KEYS.index("R")):
         raise ManifestError(f"{where}: a wheel must be bound to exactly one of L or R")
      for i, key in enumerate(KEYS):
         if bits & (1 << i):
            if key in owner:
               raise ManifestError(f"{where}: button {key} is already bound to {owner[key]}")
            owner[key] = action
      bindings.append(bits)
   if bindings[ACTIONS.index("WHEEL_1")] == bindings[ACTIONS.index("WHEEL_2")]:
      raise ManifestError(f"targets.{target}: WHEEL_1 and WHEEL_2 must use different shoulders")

   frames = {}
   for k in ("chord_window", "sleep_hold_frames", "reset_hold_frames"):
      v = config[k]
      if not isinstance(v, int) or isinstance(v, bool) or not 1 <= v <= MAX_FRAMES:
         raise ManifestError(f"config.{k}: must be an integer 1-{MAX_FRAMES}, got {v!r}")
      frames[k] = v
   wake_keys = config["wake_keys"]
   where = "config.wake_keys"
   if isinstance(wake_keys, dict):
      wake_keys = strip(wake_keys)
      check_unknown(wake_keys, TARGETS, where)
      if target not in wake_keys:
         raise ManifestError(f"{where}: no entry for {target}")
      wake_keys, where = wake_keys[target], f"{where}.{target}"
   wake = mask(wake_keys, target, where)
   return bindings, wake, frames


def emit(src_name: str, target: str, symbol: str, bindings: list[int], wake: int, frames: dict[str, int]) -> str:
   """!Generates the .cpp defining the InputMap.
   @param src_name  manifest file name, for the header comment
   @param target    target name
   @param symbol    C++ symbol to define
   @param bindings  KeyBits per action, in ACTIONS order
   @param wake      wake key mask
   @param frames    chord_window, sleep_hold_frames and reset_hold_frames
   @return the source text
   """
   lines = [
      f"// Generated by tools/input_compiler.py from {src_name} for target {target}. Do not edit.",
      '#include "capsule/input/input.h"',
      "",
      "namespace capsule::input {",
      "",
   ]
   lines += [f"static_assert(ACTION_{a} == {i});" for i, a in enumerate(ACTIONS)]
   lines += [f"static_assert(NUM_ACTIONS == {len(ACTIONS)});",
             f"static_assert(hal::input::NUM_KEYS == {len(KEYS)});", ""]
   lines.append(f"extern const InputMap {symbol};   // external linkage for any symbol name")
   lines.append(f"const InputMap {symbol} = {{")
   lines.append("    {")
   for a, b in zip(ACTIONS, bindings):
      names = " ".join(k for i, k in enumerate(KEYS) if b & (1 << i)) or "none: main-menu entry"
      lines.append(f"        0x{b:04X},  // {a}: {names}")
   lines.append("    },")
   lines.append(f"    0x{wake:04X},  // wake keys")
   lines.append(f"    {frames['chord_window']}, {frames['sleep_hold_frames']}, {frames['reset_hold_frames']},")
   lines.append(f"    {TARGETS[target]['id']},")
   lines += ["};", "", "} // namespace capsule::input", ""]
   return "\n".join(lines)


def main(argv: list[str]) -> None:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return None
   """
   if len(argv) != ARGUMENT_COUNT:
      sys.exit(__doc__)
   src, target = Path(argv[SOURCE_ARGUMENT]), argv[TARGET_ARGUMENT]
   dst, symbol = Path(argv[OUTPUT_ARGUMENT]), argv[SYMBOL_ARGUMENT]
   try:
      manifest = json.loads(src.read_text(encoding="utf-8"))
      bindings, wake, frames = compile_map(manifest, target)
   except (ManifestError, json.JSONDecodeError) as e:
      sys.exit(f"{src}: error: {e}")
   text = emit(src.name, target, symbol, bindings, wake, frames)
   dst.parent.mkdir(parents=True, exist_ok=True)
   if not dst.exists() or dst.read_text(encoding="utf-8") != text:
      dst.write_text(text, encoding="utf-8")


if __name__ == "__main__":
   main(sys.argv)
