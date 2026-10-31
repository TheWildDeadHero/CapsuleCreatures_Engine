#!/usr/bin/env python3
"""Tests for the I4 (D-29) additions to the text and route tools: generic controls with arguments and named glyphs
(tools/text_encoder.py), proportional fonts (tools/font_compiler.py, capsule.font/2), text tables, font assets and
manifest includes (tools/asset_compiler.py), and the route steps imported movements need (tools/route_compiler.py)."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import asset_compiler as ac  # noqa: E402
import font_compiler as fc  # noqa: E402
import route_compiler as rc  # noqa: E402
import text_encoder as te  # noqa: E402

## A proportional font of two glyphs (A: 3 pixels ink, B: width 2), a 4x2 cell.
FONT = {"schema": "capsule.font/2", "name": "tiny", "cell": [4, 2], "line_height": 3, "letter_spacing": 1,
        "glyphs": {"A": {"width": 3, "rows": ["1110", "2200"]}, "B": {"width": 2, "rows": ["0100", "0000"]}}}
## The packed 2bpp bytes of A's rows (pixel 0 in bits 1:0).
A_ROWS = [0b00010101, 0b00001010]


def write(path: Path, data: Any) -> Path:
   """Writes JSON.
   @param path  the file
   @param data  its content
   @return the path
   """
   path.parent.mkdir(parents=True, exist_ok=True)
   path.write_text(json.dumps(data), encoding="utf-8")
   return path


class Controls(unittest.TestCase):
   """Generic controls with argument bytes, their lengths, and markup names."""

   def test_encode(self) -> None:
      """{pause:15} is FC 08 0F; {colors:1,2,3} has three argument bytes; a two-byte argument is little-endian."""
      codes = te.encode("a{pause:15}b", "t", controls=True)
      self.assertEqual(codes[:5], [ord("a"), te.CODE_CONTROL, 0x08, 15, ord("b")])
      codes = te.encode("{colors:1,2,3}", "t", controls=True)
      self.assertEqual(codes[:5], [te.CODE_CONTROL, 0x04, 1, 2, 3])
      codes = te.encode("{se:258}", "t", controls=True)
      self.assertEqual(codes[:4], [te.CODE_CONTROL, 0x10, 2, 1])

   def test_lengths(self) -> None:
      """control_length() counts FC, the control and its arguments; an unknown control is two codes."""
      self.assertEqual(te.control_length([te.CODE_CONTROL, 0x04, 1, 2, 3, te.CODE_END], 0), te.CONTROL_HEAD + 3)
      self.assertEqual(te.control_length([te.CODE_CONTROL, 0x7E, te.CODE_END], 0), te.CONTROL_HEAD)

   def test_placeholder_names(self) -> None:
      """A placeholder name may have one ':' part; a control's name is no placeholder name."""
      self.assertTrue(te.NAME_PATTERN.fullmatch("str:1"))
      self.assertFalse(te.NAME_PATTERN.fullmatch("a:b:c"))


class ProportionalFonts(unittest.TestCase):
   """capsule.font/2 compiled for a code page."""

   def test_compile(self) -> None:
      """Widths and packed glyphs per code, the metrics, the codes covered."""
      with tempfile.TemporaryDirectory() as tmp:
         path = write(Path(tmp) / "tiny.font.json", FONT)
         font = fc.compile_proportional(path, te.DEFAULT)
      self.assertEqual((font["first"], font["count"]), (ord("A"), 2))
      self.assertEqual(font["widths"], [3, 2])
      self.assertEqual(font["glyphs"][0], A_ROWS)
      self.assertEqual((font["line_height"], font["letter_spacing"]), (3, 1))
      self.assertEqual(font["codes"], {ord("A"), ord("B")})
      self.assertEqual(font["glyph_height"], 0)   # C13a: the whole cell by default

   def test_glyph_height(self) -> None:
      """C13a: glyph_height (the rows a window draws) is kept, and must fit the cell."""
      with tempfile.TemporaryDirectory() as tmp:
         font = fc.compile_proportional(write(Path(tmp) / "a.font.json", dict(FONT, glyph_height=1)), te.DEFAULT)
         self.assertEqual(font["glyph_height"], 1)
         for bad in (3, -1, True):
            with self.assertRaises(te.TextError):
               fc.compile_proportional(write(Path(tmp) / "b.font.json", dict(FONT, glyph_height=bad)), te.DEFAULT)

   def test_errors(self) -> None:
      """A cell width that is no multiple of 4, rows of the wrong length, a character the page lacks."""
      bad = [dict(FONT, cell=[3, 2]), dict(FONT, glyphs={"A": {"rows": ["11", "00"]}}),
             dict(FONT, glyphs={"é": {"rows": ["0000", "0000"]}})]
      with tempfile.TemporaryDirectory() as tmp:
         for k, data in enumerate(bad):
            with self.subTest(k), self.assertRaises(te.TextError):
               fc.compile_proportional(write(Path(tmp) / f"bad{k}.font.json", data), te.DEFAULT)


class TextTablesAndFonts(unittest.TestCase):
   """capsule.texts/1 tables, their box check, font assets and includes."""

   def test_tables(self) -> None:
      """A table's texts become text assets; an authored line wider than the box is counted; the font is an asset;
      an included manifest's images keep working from the including one's folder."""
      with tempfile.TemporaryDirectory() as tmp:
         root = Path(tmp)
         write(root / "text" / "tiny.font.json", FONT)
         write(root / "text" / "en" / "t.json", {
            "schema": "capsule.texts/1", "language": "en", "box": {"font": "tiny", "width": 8, "lines": 1},
            "fonts": {"tiny": "../tiny.font.json"},
            "texts": {"text:T.FITS": {"text": "AB", "wrap": "original"},
                      "text:T.WIDE": {"text": "AAAA", "wrap": "original"}}})
         write(root / "text" / "text.json", {"schema": "capsule.assets/1", "assets": [
            {"key": "font:TINY", "kind": "font", "font": "tiny.font.json"}], "text_tables": ["en/t.json"]})
         top = write(root / "game" / "assets.json", {"schema": "capsule.assets/1", "assets": [],
                                                     "includes": ["../text/text.json"]})
         entries, _, _, texts, _ = ac.compile_manifest(json.loads(top.read_text()), top.parent)
         fit = ac.compile_manifest.fit
         fonts = ac.compile_manifest.fonts
      names = [e[3] for e in entries]
      self.assertIn("text:T.FITS", names)
      self.assertIn("font:TINY", names)
      self.assertEqual(len(texts), 2)
      self.assertEqual((fit["checked"], fit["over"]), (2, 1))   # AAAA: 4 x (3 + 1) > 8
      self.assertEqual(len(fonts), 1)
      text = ac.emit("x", entries, [], [], texts, [], fonts)
      self.assertIn("const text::PropFont kFonts[] = {", text)
      self.assertIn("kLists, 3, kFonts};", text)

   def test_relative_images(self) -> None:
      """Merged absolute paths become relative to the including manifest's folder, with '/' separators."""
      base = Path("/a/b")
      out = ac.relative_images([{"frames": [{"image": "/a/c/x.png"}], "font": "/a/b/f.json", "text": "/abs"}], base)
      self.assertEqual(out, [{"frames": [{"image": "../c/x.png"}], "font": "f.json", "text": "/abs"}])


class RouteSteps(unittest.TestCase):
   """The route steps imported movements need."""

   def test_steps(self) -> None:
      """in_place DIR [SPEED], face original, step NAME [ARG] (a game step's ID through the callback)."""
      steps = [w.split() for w in ("in_place left 3", "in_place up", "face original", "step spin 7")]
      code = rc.compile_route(steps, False, False, lambda n: 0, lambda r: 0, lambda name: 5)
      op = rc.OP
      self.assertEqual(code[1:], bytes([op["InPlace"], rc.DIRECTIONS.index("left"), 3,
                                        op["InPlace"], rc.DIRECTIONS.index("up"), rc.DEFAULT_SPEED,
                                        op["FaceOriginal"], op["Game"], 5, 7, op["End"]]))

   def test_errors(self) -> None:
      """A game step without the scripts' declarations; a speed out of range."""
      with self.assertRaises(rc.RouteError):
         rc.compile_route([["step", "spin"]], False, False, lambda n: 0, lambda r: 0)
      with self.assertRaises(rc.RouteError):
         rc.compile_route([["in_place", "left", "9"]], False, False, lambda n: 0, lambda r: 0, lambda name: 0)


if __name__ == "__main__":
   unittest.main()
