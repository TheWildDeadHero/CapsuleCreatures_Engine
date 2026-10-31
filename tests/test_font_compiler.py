#!/usr/bin/env python3
"""Tests for tools/font_compiler.py: glyphs chosen and laid out by a code page (C-1, C-2)."""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import font_compiler as fc  # noqa: E402
import text_encoder as te  # noqa: E402

## The engine's font.
DEFAULT_FONT = ROOT / "assets" / "fonts" / "default_8x8.txt"
## The test page (ASCII, 0x80 = é, 0x81 = x with a combining acute) and its extra glyphs.
ACCENTS = ROOT / "tests" / "data" / "text" / "accents.codepage.json"
## Glyphs of the test page's two extra characters.
ACCENT_GLYPHS = ROOT / "tests" / "data" / "text" / "accents_8x8.txt"


def run(*args: str) -> subprocess.CompletedProcess[str]:
   """Runs the font compiler.
   @param args  its arguments
   @return the finished process
   """
   return subprocess.run([sys.executable, str(ROOT / "tools" / "font_compiler.py"), *args],
                         capture_output=True, text=True)


class FontCompiler(unittest.TestCase):
   """Default output, code pages, extra glyph files and errors."""

   def test_default_page(self) -> None:
      """The ASCII page lays the default font out from 0x20, 95 glyphs."""
      lowest, laid = fc.layout(fc.parse_characters(DEFAULT_FONT), te.DEFAULT)
      self.assertEqual((lowest, len(laid)), (0x20, 95))
      self.assertEqual(laid[0x41 - 0x20][0], "A")
      self.assertEqual(laid[0x5C - 0x20][0], "backslash")
      self.assertEqual(len(fc.parse(DEFAULT_FONT)), 95)   # the studio's preview reads code points

   def test_code_page_layout(self) -> None:
      """Glyphs go to their page codes; gaps are blank; several code points make one character."""
      glyphs = fc.merge(fc.parse_characters(DEFAULT_FONT), fc.parse_characters(ACCENT_GLYPHS), ACCENT_GLYPHS)
      lowest, laid = fc.layout(glyphs, te.CodePage.load(ACCENTS))
      self.assertEqual((lowest, len(laid)), (0x20, 0x81 - 0x20 + 1))
      self.assertEqual(laid[0x7F - 0x20], ("blank", None))
      self.assertEqual(laid[0x80 - 0x20][0], "U+00E9")
      self.assertEqual(laid[0x81 - 0x20][0], "U+0078 U+0301")
      with tempfile.TemporaryDirectory() as tmp:
         out = Path(tmp) / "font.cpp"
         r = run(str(DEFAULT_FONT), str(out), "kFont", "--glyphs", str(ACCENT_GLYPHS), "--code-page", str(ACCENTS))
         self.assertEqual(r.returncode, 0, r.stderr)
         text = out.read_text(encoding="utf-8")
         self.assertIn("const FontInfo kFont = {0x20, 98, 8, 8};", text)
         self.assertIn("0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, "
                       "0x00,  // 0x7F blank", text)

   def test_errors(self) -> None:
      """A mapped character without a glyph, a glyph defined twice and a bad code are errors."""
      with tempfile.TemporaryDirectory() as tmp:
         out = str(Path(tmp) / "font.cpp")
         r = run(str(DEFAULT_FONT), out, "kFont", "--code-page", str(ACCENTS))
         self.assertNotEqual(r.returncode, 0)
         self.assertIn("missing glyphs for code page 'xx': 0x80 (U+00E9), 0x81 (U+0078 U+0301)", r.stderr)
         r = run(str(DEFAULT_FONT), out, "kFont", "--glyphs", str(DEFAULT_FONT))
         self.assertIn("defines characters the font already has", r.stderr)
         bad = Path(tmp) / "bad.txt"
         bad.write_text("A ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. ..#..\n", encoding="utf-8")
         r = run(str(bad), out, "kFont")
         self.assertIn("expected a code point", r.stderr)
      self.assertEqual(fc.parse_character("U+00E9"), "é")
      self.assertEqual(fc.parse_character("0x78+0x301"), "x́")
      self.assertIsNone(fc.parse_character("0xZZ"))


if __name__ == "__main__":
   unittest.main()
