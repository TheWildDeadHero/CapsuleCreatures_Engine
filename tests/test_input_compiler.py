#!/usr/bin/env python3
"""Validation tests for tools/input_compiler.py: bad manifests must fail the build."""
from __future__ import annotations

import copy
import json
import sys
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import input_compiler as ic  # noqa: E402

## A parsed JSON object (an input manifest).
JsonDict = dict[str, Any]

## The default input manifest every test edits a copy of.
BASE = json.loads((ROOT / "assets/input/default_input.json").read_text(encoding="utf-8"))


def variant(edit: Callable[[JsonDict], object]) -> JsonDict:
   """!A copy of the default manifest with one edit.
   @param edit  changes the copy in place
   @return the edited copy
   """
   m = copy.deepcopy(BASE)
   edit(m)
   return m


class InputCompilerTests(unittest.TestCase):
   """!Bad input manifests are refused with a useful message."""

   def assert_rejected(self, manifest: JsonDict, target: str, fragment: str) -> None:
      """!Asserts that compiling @p manifest for @p target fails mentioning @p fragment.
      @param manifest  input manifest
      @param target    target name
      @param fragment  expected text
      """
      with self.assertRaises(ic.ManifestError) as ctx:
         ic.compile_map(manifest, target)
      self.assertIn(fragment, str(ctx.exception))

   def test_default_manifest_compiles_for_every_target(self) -> None:
      for target in ic.TARGETS:
         bindings, wake, frames = ic.compile_map(BASE, target)
         self.assertEqual(len(bindings), len(ic.ACTIONS))

   def test_gba_has_no_x_or_y(self) -> None:
      m = variant(lambda m: m["targets"]["gba"].update(MAIN_MENU=["X"]))
      self.assert_rejected(m, "gba", "does not exist on target gba")

   def test_x_is_fine_on_snes_layout(self) -> None:
      ic.compile_map(BASE, "snes_layout")

   def test_missing_action(self) -> None:
      m = variant(lambda m: m["targets"]["gba"].pop("HELP"))
      self.assert_rejected(m, "gba", "HELP: missing")

   def test_only_menu_capable_actions_may_be_null(self) -> None:
      m = variant(lambda m: m["targets"]["gba"].update(ACCEPT=None))
      self.assert_rejected(m, "gba", "must have a button")

   def test_button_bound_twice(self) -> None:
      m = variant(lambda m: m["targets"]["gba"].update(BACK=["A"]))
      self.assert_rejected(m, "gba", "already bound to ACCEPT")

   def test_wheel_must_be_a_shoulder(self) -> None:
      m = variant(lambda m: m["targets"]["snes_layout"].update(WHEEL_1=["X"], MAIN_MENU=["START"],
                                                                TOGGLE_RUN=None))
      self.assert_rejected(m, "snes_layout", "exactly one of L or R")

   def test_unknown_button_and_key(self) -> None:
      self.assert_rejected(variant(lambda m: m["targets"]["gba"].update(ACCEPT=["C"])), "gba", "unknown button")
      self.assert_rejected(variant(lambda m: m["targets"]["gba"].update(JUMP=["A"])), "gba", "unknown key(s): JUMP")

   def test_frame_counts_in_range(self) -> None:
      self.assert_rejected(variant(lambda m: m["config"].update(chord_window=0)), "gba", "1-255")
      self.assert_rejected(variant(lambda m: m["config"].update(sleep_hold_frames=300)), "gba", "1-255")

   def test_dreamcast_has_no_select(self) -> None:
      m = variant(lambda m: m["targets"]["dreamcast"].update(HELP=["SELECT"]))
      self.assert_rejected(m, "dreamcast", "does not exist on target dreamcast")

   def test_wake_keys_per_target(self) -> None:
      m = variant(lambda m: m["config"].update(wake_keys={"gba": ["START"], "snes_layout": ["START"]}))
      self.assert_rejected(m, "dreamcast", "no entry for dreamcast")
      m = variant(lambda m: m["config"].update(wake_keys={"gba": ["START"], "snes_layout": ["START"],
                                                          "dreamcast": ["SELECT"]}))
      self.assert_rejected(m, "dreamcast", "does not exist on target dreamcast")
      m = variant(lambda m: m["config"].update(wake_keys={"gba": ["START"], "snes_layout": ["START"],
                                                          "dreamcast": ["START"], "n64": ["A"]}))
      self.assert_rejected(m, "gba", "unknown key")

   def test_wake_keys_exist_on_target(self) -> None:
      m = variant(lambda m: m["config"].update(wake_keys=["X"]))
      self.assert_rejected(m, "gba", "does not exist on target gba")

   def test_editor_data_is_stripped(self) -> None:
      m = variant(lambda m: m["targets"]["gba"].update(_note="ignored"))
      ic.compile_map(m, "gba")

   def test_unknown_target(self) -> None:
      self.assert_rejected(BASE, "n64", "unknown target")


if __name__ == "__main__":
   unittest.main()
