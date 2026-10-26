#!/usr/bin/env python3
"""Validation tests for tools/sequence_compiler.py: bad manifests must fail the build."""
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
import sequence_compiler as sc  # noqa: E402

## A parsed JSON object (a sequence manifest, or part of one).
JsonDict = dict[str, Any]

## Folder of the test sequences.
DATA = ROOT / "tests/data/sequences"
## Every test sequence by file stem.
FIXTURES = {p.stem: json.loads(p.read_text(encoding="utf-8")) for p in DATA.glob("*.json")}


def variant(name: str, edit: Callable[[JsonDict], object]) -> JsonDict:
   """!A copy of one test sequence with one edit.
   @param name  fixture name (file stem)
   @param edit  changes the copy in place
   @return the edited copy
   """
   m = copy.deepcopy(FIXTURES[name])
   edit(m)
   return m


def stage(m: JsonDict, name: str) -> JsonDict:
   """!A stage of a manifest by name.
   @param m     sequence manifest
   @param name  stage name
   @return the stage object
   """
   return next(s for s in m["stages"] if s["name"] == name)


class SequenceCompilerTests(unittest.TestCase):
   """!The fixtures compile; bad sequences are refused with a useful message."""

   def assert_rejected(self, manifest: JsonDict, fragment: str) -> None:
      """!Asserts that compiling @p manifest fails mentioning @p fragment.
      @param manifest  sequence manifest
      @param fragment  expected text
      """
      with self.assertRaises(sc.ManifestError) as ctx:
         sc.Compiler(manifest).run()
      self.assertIn(fragment, str(ctx.exception))

   def test_every_fixture_compiles(self) -> None:
      self.assertEqual(len(FIXTURES), 5)
      for name, m in FIXTURES.items():
         with self.subTest(name):
            comp = sc.Compiler(m).run()
            self.assertGreater(comp.rom_bytes(), 0)
            self.assertIn("inline constexpr capsule::seq::Sequence", comp.emit_inc("kX"))

   def test_debug_names(self) -> None:
      inc = sc.Compiler(FIXTURES["title_song_end"]).run().emit_inc("kX")
      self.assertIn('inline constexpr const char* kXStageNames[] = {"copyright", "intro"', inc)
      self.assertIn('kXExitNames[] = {"main_menu", "clear_save", "reset_rtc", nullptr};', inc)
      self.assertIn('kXCondNames[] = {"rtc_resettable", nullptr};', inc)
      self.assertIn("kXListNames[] = {nullptr};", inc)

   def test_record_sizes_match_the_structs(self) -> None:
      # Command records are 8 bytes, stage records 20, combo records 8.
      cmd, stage_blob, combo = sc.Compiler(FIXTURES["title_song_end"]).run().blobs()
      self.assertEqual(len(cmd) % 8, 0)
      self.assertEqual(len(stage_blob) % 20, 0)
      self.assertEqual(len(combo) % 8, 0)

   def test_asset_key_matches_engine_hash(self) -> None:
      # FNV-1a 32 over "namespace:CANONICAL NAME" (C-02d); canonicalization uppercases
      # and collapses whitespace.
      self.assertEqual(sc.asset_key("sprite:a  b"), sc.asset_key("sprite:A B"))
      self.assertNotEqual(sc.asset_key("sprite:A"), sc.asset_key("image:A"))

   def test_editor_data_is_stripped(self) -> None:
      m = variant("cutscene", lambda m: m["stages"][0].update(editor={"anything": 1}, _draft=True))
      sc.Compiler(m).run()

   def test_unknown_keys(self) -> None:
      m = variant("cutscene", lambda m: m["stages"][0]["tracks"][0][0].update(colour=1))
      self.assert_rejected(m, "unknown keys")

   def test_loop_back_without_wait(self) -> None:
      def edit(m: JsonDict) -> None:
         track = stage(m, "title_idle")["tracks"][0]
         stage(m, "title_idle")["tracks"][0] = [c for c in track if c["op"] != "wait"]
      self.assert_rejected(variant("title_song_end", edit), "has no wait")

   def test_loop_body_without_wait(self) -> None:
      def edit(m: JsonDict) -> None:
         track = stage(m, "showcase")["tracks"][0]
         stage(m, "showcase")["tracks"][0] = [c for c in track if c["op"] not in ("wait", "wait_for")]
      self.assert_rejected(variant("credits_showcase", edit), "zero time")

   def test_unbalanced_loop(self) -> None:
      m = variant("credits_showcase", lambda m: stage(m, "showcase")["tracks"][0].remove({"op": "next"}))
      self.assert_rejected(m, "without next")

   def test_loops_nested_too_deep(self) -> None:
      def edit(m: JsonDict) -> None:
         t = m["stages"][0]["tracks"][0]
         for _ in range(3):
            t.insert(0, {"op": "repeat", "count": 2})
      self.assert_rejected(variant("cutscene", edit), "nested deeper")

   def test_item_outside_for_each(self) -> None:
      m = variant("credits_showcase",
                  lambda m: stage(m, "showcase")["tracks"][0].insert(
                     0, {"op": "spr_load", "slot": 1, "asset": "$item"}))
      self.assert_rejected(m, "$item used outside")

   def test_undeclared_names(self) -> None:
      m = variant("credits_showcase",
                  lambda m: stage(m, "showcase")["tracks"][0].insert(0, {"op": "for_each", "list": "nope"}))
      self.assert_rejected(m, "unknown list")
      m = variant("cutscene",
                  lambda m: m["stages"][0]["tracks"][0].insert(1, {"op": "if", "cond": "nope", "to": "x"}))
      self.assert_rejected(m, "unknown condition")
      m = variant("cutscene", lambda m: m["stages"][0]["tracks"][0].insert(1, {"op": "jump", "to": "nowhere"}))
      self.assert_rejected(m, "unknown label")
      m = variant("cutscene", lambda m: m["stages"][0]["on"].update(done="stage:nowhere"))
      self.assert_rejected(m, "unknown stage")

   def test_macro_arguments(self) -> None:
      m = variant("credits_showcase", lambda m: m["stages"][0]["tracks"][1].append(
         {"op": "use", "macro": "page", "heading": "text:X"}))
      self.assert_rejected(m, "takes")

   def test_shadowed_combo(self) -> None:
      m = variant("title_song_end", lambda m: stage(m, "title_idle")["combos"].insert(
         0, {"held": ["B", "SELECT"], "to": "exit:clear_save"}))
      self.assert_rejected(m, "can never fire")

   def test_ranges(self) -> None:
      m = variant("credits_showcase", lambda m: stage(m, "showcase")["tracks"][0].insert(
         0, {"op": "spr_affine", "slot": 5, "scale": 2}))
      self.assert_rejected(m, "affine")
      m = variant("cutscene", lambda m: m["stages"][0]["tracks"][0].insert(0, {"op": "video_mode", "mode": 3}))
      self.assert_rejected(m, "video mode")
      m = variant("credits_showcase", lambda m: m["stages"][0].update(fast_forward={"keys": ["B"], "speed": 1}))
      self.assert_rejected(m, "speed")
      m = variant("credits_showcase", lambda m: m["stages"][0]["tracks"][1][0].update(tint="green"))
      self.assert_rejected(m, "#rrggbb")

   def test_too_many_tracks(self) -> None:
      m = variant("cutscene", lambda m: m["stages"][0].update(tracks=[[{"op": "wait", "frames": 1}]] * 9))
      self.assert_rejected(m, "tracks required")

   def test_exits_need_targets(self) -> None:
      m = variant("cutscene", lambda m: m["stages"][0]["on"].pop("skip"))
      self.assert_rejected(m, "no on.skip target")


if __name__ == "__main__":
   unittest.main()
