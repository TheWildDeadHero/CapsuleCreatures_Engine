#!/usr/bin/env python3
"""!UC (D-33): tests of the screen graphics kinds of tools/asset_compiler.py (tools/screen_assets.py): tiles (raw tile
bytes, shapes cut row by row or by blocks, 8bpp), tilemaps (raw entries, cells, affine bytes), palettes (hex and raw
BGR555 colours), the generated tables and the refusals. Fixture: tests/data/screen."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import asset_compiler as ac  # noqa: E402
import screen_assets as sa  # noqa: E402

## The fixture.
FIXTURE = ROOT / "tests/data/screen"
## A parsed JSON object.
JsonDict = dict[str, Any]


def compile_assets(assets: list[JsonDict], base: Path | None = None) -> dict[str, list[JsonDict]]:
   """!Compiles a manifest of some assets.
   @param assets  the assets
   @param base    the manifest's folder
   @return the screen kinds compiled (compile_manifest.screen)
   """
   ac.compile_manifest({"schema": "capsule.assets/1", "id": "t", "assets": assets}, base)
   return ac.compile_manifest.screen


class ScreenFormatTests(unittest.TestCase):
   """!The tiles / tilemap / palette kinds."""

   def test_fixture(self) -> None:
      """!The fixture compiles; its tables are emitted with the table's new members.
      @return None
      """
      m = json.loads((FIXTURE / "assets.json").read_text(encoding="utf-8"))
      entries, bgs, sprites, texts, lists = ac.compile_manifest(m, FIXTURE)
      screen = ac.compile_manifest.screen
      self.assertEqual([t["count"] for t in screen["tiles"]], [2, 8])
      source = ac.emit("assets.json", entries, bgs, sprites, texts, lists, screen=screen)
      self.assertIn("const Tiles kTileBlocks[]", source)
      self.assertIn("kTileBlocks, kTilemaps, kPalettes};", source)
      self.assertIn("Kind::Tilemap", source)

   def test_blocks(self) -> None:
      """!Shapes cut by blocks: each block's tiles row by row, the blocks in reading order.
      @return None
      """
      shapes = [{"shape": "rect", "x": 8, "y": 0, "w": 8, "h": 8, "color": 5}]
      rows = compile_assets([{"key": "tiles:R", "kind": "tiles", "width": 32, "height": 8, "shapes": shapes}])
      blocks = compile_assets([{"key": "tiles:B", "kind": "tiles", "width": 16, "height": 16, "block": [1, 2],
                                "shapes": shapes}])
      self.assertEqual(rows["tiles"][0]["words"][8], 0x55555555)   # tile 1 of the row
      self.assertEqual(blocks["tiles"][0]["words"][16], 0x55555555)   # block 2's first tile (tile 2)
      self.assertEqual(blocks["tiles"][0]["words"][8], 0)   # block 1's second tile (the lower left)

   def test_data_files(self) -> None:
      """!Raw tiles, raw entries, affine bytes, 8bpp tiles.
      @return None
      """
      with tempfile.TemporaryDirectory() as d:
         base = Path(d)
         (base / "t.4bpp").write_bytes(bytes(range(32)))
         (base / "t.8bpp").write_bytes(bytes(64))
         (base / "m.bin").write_bytes(bytes([0x34, 0x12, 0xFF, 0xF0]))
         out = compile_assets([{"key": "tiles:A", "kind": "tiles", "data": "t.4bpp", "columns": 4},
                               {"key": "tiles:E", "kind": "tiles", "data": "t.8bpp", "bpp": 8},
                               {"key": "tilemap:M", "kind": "tilemap", "data": "m.bin"},
                               {"key": "tilemap:F", "kind": "tilemap", "data": "m.bin", "affine": True}], base)
         self.assertEqual(out["tiles"][0]["words"][0], 0x03020100)
         self.assertEqual((out["tiles"][0]["columns"], out["tiles"][1]["count"], out["tiles"][1]["bpp"]), (4, 1, 8))
         self.assertEqual(out["tilemap"][0]["cells"], [0x1234, 0xF0FF])
         self.assertEqual(out["tilemap"][1]["cells"], [0x34, 0x12, 0xFF, 0xF0])
         with self.assertRaises(ac.ManifestError):
            (base / "odd.4bpp").write_bytes(bytes(33))
            compile_assets([{"key": "tiles:O", "kind": "tiles", "data": "odd.4bpp"}], base)

   def test_include(self) -> None:
      """!An included manifest's data paths stay relative to it.
      @return None
      """
      with tempfile.TemporaryDirectory() as d:
         base = Path(d)
         (base / "sub").mkdir()
         (base / "sub" / "t.4bpp").write_bytes(bytes(32))
         (base / "sub" / "m.json").write_text(json.dumps(
            {"schema": "capsule.assets/1", "id": "s", "assets": [{"key": "tiles:I", "kind": "tiles",
                                                                 "data": "t.4bpp"}]}), encoding="utf-8")
         ac.compile_manifest({"schema": "capsule.assets/1", "id": "t", "assets": [], "includes": ["sub/m.json"]},
                             base)
         self.assertEqual(ac.compile_manifest.screen["tiles"][0]["count"], 1)

   def test_palette(self) -> None:
      """!Hex colours drop their low bits; raw BGR555 values stay; entry 0 is kept.
      @return None
      """
      out = compile_assets([{"key": "palette:P", "kind": "palette", "colors": ["#f8f8f8", 0x7C1F, 0]}])
      self.assertEqual(out["palette"][0]["colors"], [0x7FFF, 0x7C1F, 0])

   def test_refusals(self) -> None:
      """!Bad assets are refused with the asset named.
      @return None
      """
      bad = [{"key": "tiles:X", "kind": "tiles"},
             {"key": "tiles:X", "kind": "tiles", "bpp": 2, "width": 8, "height": 8, "shapes": []},
             {"key": "tiles:X", "kind": "tiles", "width": 12, "height": 8, "shapes": []},
             {"key": "tiles:X", "kind": "tiles", "width": 16, "height": 8, "block": [3, 1], "shapes": []},
             {"key": "tilemap:X", "kind": "tilemap", "cells": [0x10000]},
             {"key": "tilemap:X", "kind": "tilemap", "cells": [300], "affine": True},
             {"key": "tilemap:X", "kind": "tilemap", "cells": []},
             {"key": "palette:X", "kind": "palette", "colors": [0x8000]},
             {"key": "palette:X", "kind": "palette", "colors": []},
             {"key": "palette:X", "kind": "palette", "colors": ["red"]}]
      for a in bad:
         with self.subTest(asset=a), self.assertRaises(ac.ManifestError):
            compile_assets([a])

   def test_sizes(self) -> None:
      """!rom_bytes() counts the three tables' data.
      @return None
      """
      out = compile_assets([{"key": "tilemap:M", "kind": "tilemap", "cells": [1, 2, 3]},
                            {"key": "palette:P", "kind": "palette", "colors": [1, 2]}])
      self.assertEqual(sa.rom_bytes(out["tiles"], out["tilemap"], out["palette"]), 6 + 4)


if __name__ == "__main__":
   unittest.main()
