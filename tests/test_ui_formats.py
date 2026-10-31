#!/usr/bin/env python3
"""!Tests of the N3 data formats in tools/asset_compiler.py: a text box style's "menu" object (ui::MenuStyle: positions,
cursor, wrap rule, delay, keys, sounds, a list's scroll arrows) and its window's "arrow_image" (the arrow image a wait
shows: rows, height, steps, delay). Fixture: tests/data/ui."""
from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import asset_compiler as ac  # noqa: E402

## The fixture.
FIXTURE = ROOT / "tests/data/ui"
## A parsed JSON object.
JsonDict = dict[str, Any]


def manifest() -> JsonDict:
   """!The fixture's manifest.
   @return it, parsed
   """
   return json.loads((FIXTURE / "assets.json").read_text(encoding="utf-8"))


def style(m: JsonDict, key: str) -> JsonDict:
   """!A text box style of a manifest.
   @param m    the manifest
   @param key  its key
   @return the asset object (in m)
   """
   return next(a for a in m["assets"] if a["key"] == key)


def compile_text(m: JsonDict) -> str:
   """!Compiles a manifest to its C++ table.
   @param m  the manifest
   @return the generated source
   """
   entries, bgs, sprites, texts, lists = ac.compile_manifest(m, FIXTURE, [])
   return ac.emit("assets.json", entries, bgs, sprites, texts, lists, ac.compile_manifest.fonts,
                  ac.compile_manifest.doors, ac.compile_manifest.text_boxes)


class MenuStyle(unittest.TestCase):
   """!The "menu" object."""

   def test_fixture(self) -> None:
      """!Every style compiles; words become the ui constants, keys their bits, the sprite its key."""
      text = compile_text(manifest())
      self.assertIn(".wrap = ui::kWrapAbove, .wrap_above = 3, .input_delay = 2", text)
      self.assertIn(".order = ui::kEveryKey, .cancel_keys = 0x000Au", text)   # B and START
      self.assertIn(f".arrow_sprite = AssetKey{{0x{ac.asset_key('sprite:UI_ARROWS', 'x'):08X}u}}", text)
      self.assertIn("&kBoxMenu0,", text)
      self.assertIn("nullptr},   // text_box:UI_MESSAGE", text)

   def test_members_in_order(self) -> None:
      """!The generated initializer names the members in ui::MenuStyle's declaration order (C++20 designated)."""
      m = manifest()
      menu = style(m, "text_box:UI_LIST")["menu"]
      out = ac.compile_menu_style(dict(reversed(list(menu.items()))), "x")
      line = ", ".join(f".{k} = {v}" for k, v in sorted(out.items(), key=lambda kv: ac.MENU_ORDER.index(kv[0])))
      names = [part.split(" = ")[0][1:] for part in line.split(", ")]
      self.assertEqual(names, [n for n in ac.MENU_ORDER if n in names])

   def test_errors(self) -> None:
      """!Unknown settings, bad words, ranges, keys and sprites are refused."""
      bad = [{"colour": 1}, {"wrap": "sometimes"}, {"row_height": 256}, {"cancel_keys": ["Z"]},
             {"arrow_sprite": 3}, {"order": "all"}, {"cursor": 255}]
      for menu in bad:
         with self.subTest(menu=menu):
            m = manifest()
            style(m, "text_box:UI_COLUMN")["menu"] = menu
            with self.assertRaises(ac.ManifestError):
               compile_text(m)


class ArrowImage(unittest.TestCase):
   """!The window's "arrow_image"."""

   def test_rows(self) -> None:
      """!The PNG's rows become 4bpp words (pixel x at bits 4x), the steps and the settings the Config's."""
      text = compile_text(manifest())
      self.assertIn("kBoxArrowRows3[] = {0x00000000u, 0x00000000u, 0x00222220u, 0x00022200u, 0x00002000u, "
                    "0x00000000u}", text)
      self.assertIn("kBoxArrowSteps3[] = {0, 1, 2, 1}", text)
      self.assertIn(".arrow_kind = 1, .arrow_rows = kBoxArrowRows3, .arrow_image_rows = 6, .arrow_height = 4", text)
      self.assertIn(".arrow_steps = 4, .arrow_delay = 8}", text)

   def test_errors(self) -> None:
      """!A missing image, a row past the image, a height past it, unknown settings are refused."""
      base = style(manifest(), "text_box:UI_MESSAGE")["window"]["arrow_image"]
      bad = [dict(base, image="missing.png"), dict(base, steps=[6]), dict(base, height=7), dict(base, speed=1),
             dict(base, delay=256), "arrow.png"]
      for arrow in bad:
         with self.subTest(arrow=arrow):
            m = manifest()
            style(m, "text_box:UI_MESSAGE")["window"]["arrow_image"] = copy.deepcopy(arrow)
            with self.assertRaises(ac.ManifestError):
               compile_text(m)


class FrameArt(unittest.TestCase):
   """!The style's "palette" and the frame's "image" / "palette"."""

   def test_tables(self) -> None:
      """!The colors as BGR555 (entry 0 is 0), the frame's nine tiles in reading order, the TextBox's pointers."""
      text = compile_text(manifest())
      self.assertIn("const Color kBoxPalette3[] = {0x0000, 0x7FFF, 0x318C, 0x675A, 0x043C, 0x0000", text)
      self.assertIn("const Color kBoxFramePalette0[] = {0x0000, 0x7FFF, 0x3084, 0x7290, 0x0000", text)
      self.assertIn("alignas(4) const uint32_t kBoxFrameTiles0[] = {0x22222222u, 0x33333332u, 0x11111132u", text)
      self.assertIn("&kBoxMenu0, nullptr, kBoxFrameTiles0, kBoxFramePalette0},   // text_box:UI_COLUMN", text)
      self.assertIn("nullptr, kBoxPalette3, nullptr, nullptr},   // text_box:UI_MESSAGE", text)
      tiles = ac.frame_image_tiles("frame.png", FIXTURE, None, "x")
      self.assertEqual(len(tiles), 9 * 8)

   def test_errors(self) -> None:
      """!A frame image that is not 24 x 24, a bad color, too many colors are refused."""
      for change in ({"image": "arrow.png"}, {"palette": ["#xyz"]}, {"palette": ["#000000"] * 16}):
         with self.subTest(change=change):
            m = manifest()
            style(m, "text_box:UI_COLUMN")["frame"].update(change)
            with self.assertRaises(ac.ManifestError):
               compile_text(m)


if __name__ == "__main__":
   unittest.main()
