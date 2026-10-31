#!/usr/bin/env python3
"""Compile a text font asset into a packed C++ glyph table (P-1, C-1, C-2).

Input: a font file in the format described at the top of
assets/fonts/default_8x8.txt. Output: a .cpp file defining a FontInfo record
and the 2bpp glyph data (16 bytes per 8x8 glyph) that the engine expands to
4bpp tiles at load time.

The first token of each glyph line is the character, as a Unicode code point
in hex ("0x41" or "U+0041"; code points joined by "+" for a character of
several, such as a letter with a combining mark). The code page (--code-page,
tools/text_encoder.py; default the engine's ASCII page) chooses the glyphs and
their codes: the font keeps the glyphs the page maps, laid out by code from
the lowest mapped code to the highest, gaps filled with blank glyphs. A mapped
character without a glyph is an error. --glyphs adds the glyphs of another
font file (repeatable; a character defined in two files is an error), so a
language's few extra characters can sit beside the engine's font.

Usage: font_compiler.py <font.txt> <out.cpp> <symbol> [--code-page <page.json>] [--glyphs <more.txt>]...
  e.g. font_compiler.py assets/fonts/default_8x8.txt build/default_font.cpp kDefaultFont

I4 (D-29): proportional fonts (capsule.font/2, `*.font.json`), the C-1 variable-width fonts, are font assets
(tools/asset_compiler.py kind "font"; compile_proportional() here):

    {"schema": "capsule.font/2", "name": "normal", "cell": [16, 16], "line_height": 16, "letter_spacing": 0,
     "glyphs": {"A": {"width": 6, "rows": ["0011110000000000", ...]}, "#heart": {...}}}

Each glyph is keyed by its character (or `#name`, a named glyph of the code page with one code); its cell rows are
pixel values (0 transparent, 1 ink, 2 shadow, 3 fill), its width the pen's advance in pixels. The font covers the
codes its glyphs have; the codes between them are blank with width 0.
"""
from __future__ import annotations

import sys
from pathlib import Path
from typing import NoReturn

sys.path.insert(0, str(Path(__file__).resolve().parent))
import text_encoder  # noqa: E402

## Cell size: glyphs are 8x8 pixels.
CELL = 8
## Authored glyph width.
GLYPH_W = 5
## Glyph columns 1-5 within the cell.
COL_OFFSET = 1
## Fewest rows an authored glyph may have.
MIN_ROWS = 7
## Most rows an authored glyph may have.
MAX_ROWS = 8
## Pixel value: empty (transparent).
EMPTY = 0
## Pixel value: foreground (palette entry 1).
FG = 1
## Pixel value: shadow (palette entry 2).
SHADOW = 2
## First character labelled by itself in the output (space).
FIRST_LABELLED = 0x20
## Last character labelled by itself (tilde); others are labelled U+XXXX.
LAST_LABELLED = 0x7E
## Character labelled by name in the output (a backslash ending a C++ comment would continue it).
BACKSLASH = "\\"
## Prefixes of a code point in the font file.
CODE_POINT_PREFIXES = ("0x", "U+")
## Separator of the code points of one character.
CODE_POINT_SEPARATOR = "+"
## Base of the hex character codes in the font file.
HEX_BASE = 16
## Bytes of one packed 2bpp glyph (two bytes per row of an 8x8 cell).
GLYPH_BYTES_2BPP = 16
## Bits per pixel of the packed glyphs.
BITS_PER_PIXEL = 2
## Pixels packed into one byte at 2bpp.
PIXELS_PER_BYTE = 4

## Option adding the glyphs of another font file.
GLYPHS_OPTION = "--glyphs"
## Words of an option with its value.
OPTION_WORDS = 2
## Number of positional command-line arguments, including the program name.
ARGUMENT_COUNT = 4
## Position of the font file argument.
SOURCE_ARGUMENT = 1
## Position of the output file argument.
OUTPUT_ARGUMENT = 2
## Position of the symbol argument.
SYMBOL_ARGUMENT = 3


def fail(path: Path, line_no: int, message: str) -> NoReturn:
   """!Stops with a compiler-style error for one line of the font file.
   @param path     font file
   @param line_no  1-based line
   @param message  what is wrong
   @return never: exits the program
   """
   sys.exit(f"{path}:{line_no}: error: {message}")


def parse_character(token: str) -> str | None:
   """!The character a glyph line's first token names.
   @param token  "0x41", "U+0041" or code points joined by "+" ("0x65+0x301")
   @return the character, or None if the token is not one
   """
   parts = []
   rest = token
   while rest:
      prefix = next((p for p in CODE_POINT_PREFIXES if rest.upper().startswith(p.upper())), None)
      if prefix is None:
         return None
      rest = rest[len(prefix):]
      digits, _, rest = rest.partition(CODE_POINT_SEPARATOR)
      try:
         parts.append(chr(int(digits, HEX_BASE)))
      except ValueError:
         return None
   return "".join(parts) or None


def label(ch: str) -> str:
   """!How the output comments a glyph.
   @param ch  the character
   @return the character itself when printable ASCII, else U+XXXX
   """
   if ch == BACKSLASH:
      return "backslash"
   if len(ch) == 1 and FIRST_LABELLED <= ord(ch) <= LAST_LABELLED:
      return ch
   return " ".join(f"U+{ord(c):04X}" for c in ch)


def parse(path: Path) -> dict[int, list[str]]:
   """!Reads and validates a font file's single-code-point glyphs.
   @param path  font file (Path)
   @return dict of code point -> list of row strings ('.' / '#')
   """
   return {ord(ch): rows for ch, rows in parse_characters(path).items() if len(ch) == 1}


def parse_characters(path: Path) -> dict[str, list[str]]:
   """!Reads and validates a font file.
   @param path  font file (Path)
   @return dict of character -> list of row strings ('.' / '#')
   """
   glyphs = {}
   for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
      tokens = raw.split()
      if not tokens or tokens[0].startswith("#"):   # blank line or comment
         continue
      ch = parse_character(tokens[0])
      if ch is None:
         fail(path, line_no, f"expected a code point (0x41 or U+0041), got {tokens[0]!r}")
      if ch in glyphs:
         fail(path, line_no, f"character {label(ch)} is defined twice")
      rows = tokens[1:]
      if not MIN_ROWS <= len(rows) <= MAX_ROWS:
         fail(path, line_no, f"glyph {label(ch)} has {len(rows)} rows, expected {MIN_ROWS}-{MAX_ROWS}")
      for row in rows:
         if len(row) != GLYPH_W or set(row) - {".", "#"}:
            fail(path, line_no, f"glyph {label(ch)}: row {row!r} must be {GLYPH_W} of '.' or '#'")
      glyphs[ch] = rows
   return glyphs


## One laid-out glyph: its label and rows (None: a blank gap).
LaidGlyph = tuple[str, list[str] | None]


def merge(base: dict[str, list[str]], more: dict[str, list[str]], more_path: Path) -> dict[str, list[str]]:
   """!Adds the glyphs of another font file.
   @param base       character -> rows so far
   @param more       character -> rows of the other file
   @param more_path  that file, for the error message
   @return the merged glyphs
   """
   twice = [label(ch) for ch in more if ch in base]
   if twice:
      raise text_encoder.TextError(f"{more_path.name} defines characters the font already has: {', '.join(twice)}")
   return {**base, **more}


def layout(glyphs: dict[str, list[str]], page: text_encoder.CodePage) -> tuple[int, list[LaidGlyph]]:
   """!Lays the glyphs the page maps out by code.
   @param glyphs  character -> rows, from parse_characters()
   @param page    the code page
   @return (lowest code, [(label, rows or None for a blank gap)] per code from the lowest)
   """
   missing = [c for c in page.glyph_codes() if page.chars[c] not in glyphs]
   if missing:
      raise text_encoder.TextError("missing glyphs for code page " + repr(page.id) + ": " +
                                   ", ".join(f"0x{c:02X} ({label(page.chars[c])})" for c in missing))
   lowest, count = page.font_span()
   out = []
   for code in range(lowest, lowest + count):
      ch = page.chars.get(code)
      out.append((label(ch), glyphs[ch]) if ch is not None else ("blank", None))
   return lowest, out


def render(rows: list[str]) -> list[list[int]]:
   """!Places a glyph in an 8x8 cell and adds the down-right shadow.
   @param rows  the glyph's row strings
   @return 8x8 list of pixel values (EMPTY, FG, SHADOW)
   """
   cell = [[EMPTY] * CELL for _ in range(CELL)]
   for y, row in enumerate(rows):
      for x, ch in enumerate(row):
         if ch == "#":
            cell[y][x + COL_OFFSET] = FG
   # A foreground pixel casts a shadow one pixel down and right, onto empty pixels only.
   for y in range(CELL - 1):
      for x in range(CELL - 1):
         if cell[y][x] == FG and cell[y + 1][x + 1] == EMPTY:
            cell[y + 1][x + 1] = SHADOW
   return cell


def pack_2bpp(cell: list[list[int]]) -> list[int]:
   """!Packs a cell to 2bpp: two bytes per row; bits 1:0 of each byte are the
   leftmost pixel.
   @param cell  8x8 pixel values
   @return 16 bytes
   """
   out = []
   for row in cell:
      for half in (row[0:PIXELS_PER_BYTE], row[PIXELS_PER_BYTE:CELL]):
         byte = 0
         for i, px in enumerate(half):
            byte |= px << (BITS_PER_PIXEL * i)
         out.append(byte)
   return out


def emit(src_name: str, symbol: str, lowest: int, laid: list[LaidGlyph]) -> str:
   """!Generates the .cpp of a font.
   @param src_name  font file name, for the header comment
   @param symbol    FontInfo symbol (the glyph array is named after it, with Glyphs appended)
   @param lowest    code of glyph 0
   @param laid      from layout()
   @return the source text
   """
   count = len(laid)
   blank = [EMPTY] * GLYPH_BYTES_2BPP
   lines = [
      f"// Generated by tools/font_compiler.py from {src_name}. Do not edit.",
      '#include "capsule/text/font.h"',
      "",
      "namespace capsule::text {",
      "",
      f"const FontInfo {symbol} = {{0x{lowest:02X}, {count}, {CELL}, {CELL}}};",
      "",
      f"const uint8_t {symbol}Glyphs[{count} * kGlyphBytes2bpp] = {{",
   ]
   for i, (name, rows) in enumerate(laid):
      chunk = pack_2bpp(render(rows)) if rows is not None else blank
      lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + f",  // 0x{lowest + i:02X} {name}")
   lines += ["};", "", f"static_assert(sizeof({symbol}Glyphs) == {count * GLYPH_BYTES_2BPP});", "",
             "} // namespace capsule::text", ""]
   return "\n".join(lines)


## Schema of a proportional font (I4).
PROPORTIONAL_SCHEMA = "capsule.font/2"
## Keys of a proportional font.
PROPORTIONAL_KEYS = {"schema", "name", "cell", "line_height", "letter_spacing", "glyph_height", "glyphs", "editor"}
## Pixel values of a proportional glyph row (0 transparent, 1 ink, 2 shadow, 3 fill).
PIXEL_DIGITS = "0123"
## Most a proportional cell's side may be (one byte; the renderer's limit, capsule::text::kMaxCellPixels).
MAX_CELL = 32
## Values of a proportional font's cell (width, height).
CELL_DIMENSIONS = 2
## A proportional cell's width is a multiple of this (whole 2bpp bytes per row).
CELL_WIDTH_STEP = PIXELS_PER_BYTE
## Largest width, line height or letter spacing (one byte each).
MAX_METRIC = 255


def compile_proportional(path: Path, page: text_encoder.CodePage) -> dict[str, object]:
   """!Compiles a proportional font (capsule.font/2) for a code page (I4).
   @param path  the .font.json file
   @param page  the code page (which code each glyph has)
   @return {"first": lowest code, "count": codes, "cell": (w, h), "line_height", "letter_spacing", "glyph_height"
           (C13a: the rows a window draws, 0 = the whole cell), "widths": [per code], "glyphs": [packed 2bpp bytes,
           per code], "codes": set of covered codes}
   @throws text_encoder.TextError when the font is malformed or names a glyph the page lacks
   """
   import json
   where = path.name
   try:
      data = json.loads(path.read_text(encoding="utf-8"))
   except (OSError, ValueError) as e:
      raise text_encoder.TextError(f"{where}: cannot read the font ({e})") from None
   if not isinstance(data, dict) or data.get("schema") != PROPORTIONAL_SCHEMA:
      raise text_encoder.TextError(f"{where}: schema must be {PROPORTIONAL_SCHEMA!r}")
   unknown = sorted(set(data) - PROPORTIONAL_KEYS)
   if unknown:
      raise text_encoder.TextError(f"{where}: unknown keys {unknown}")
   cell = data.get("cell")
   if not (isinstance(cell, list) and len(cell) == CELL_DIMENSIONS
           and all(isinstance(v, int) and 0 < v <= MAX_CELL for v in cell) and cell[0] % CELL_WIDTH_STEP == 0):
      raise text_encoder.TextError(f"{where}: cell is [width, height], 1-{MAX_CELL} pixels, the width a multiple "
                                   f"of {CELL_WIDTH_STEP}")
   cw, ch = cell
   metrics = {}
   for key, default in (("line_height", ch), ("letter_spacing", 0)):
      v = data.get(key, default)
      if not isinstance(v, int) or not 0 <= v <= MAX_METRIC:
         raise text_encoder.TextError(f"{where}: {key} is 0-{MAX_METRIC}")
      metrics[key] = v
   # C13a: optional; the rows of a glyph a window draws (the handheld games' glyph height), 0 for the whole cell
   glyph_height = data.get("glyph_height", 0)
   if not isinstance(glyph_height, int) or isinstance(glyph_height, bool) or not 0 <= glyph_height <= ch:
      raise text_encoder.TextError(f"{where}: glyph_height is 0-{ch} (0: the whole cell)")
   metrics["glyph_height"] = glyph_height
   glyphs = data.get("glyphs")
   if not isinstance(glyphs, dict) or not glyphs:
      raise text_encoder.TextError(f"{where}: glyphs is an object of character -> glyph")
   by_code: dict[int, tuple[int, list[int]]] = {}
   for key, g in glyphs.items():
      gw = f"{where}: glyph {key!r}"
      if key.startswith(text_encoder.GLYPH_PREFIX) and key[1:] in page.names:
         codes = page.names[key[1:]]
      else:
         codes = page.table.get(key)
      if not codes or len(codes) != 1:
         raise text_encoder.TextError(f"{gw}: not a character (or one-code named glyph) of the code page {page.id!r}")
      if not isinstance(g, dict) or not isinstance(g.get("rows"), list) or len(g["rows"]) != ch:
         raise text_encoder.TextError(f"{gw}: rows are {ch} strings of {cw} pixel digits")
      width = g.get("width", cw)
      if not isinstance(width, int) or not 0 <= width <= MAX_METRIC:
         raise text_encoder.TextError(f"{gw}: width is 0-{MAX_METRIC}")
      packed = []
      for row in g["rows"]:
         if not isinstance(row, str) or len(row) != cw or set(row) - set(PIXEL_DIGITS):
            raise text_encoder.TextError(f"{gw}: row {row!r} is {cw} of {PIXEL_DIGITS}")
         for x in range(0, cw, PIXELS_PER_BYTE):
            byte = 0
            for i, digit in enumerate(row[x:x + PIXELS_PER_BYTE]):
               byte |= int(digit) << (BITS_PER_PIXEL * i)
            packed.append(byte)
      by_code[codes[0]] = (width, packed)
   first, last = min(by_code), max(by_code)
   blank = [EMPTY] * (cw * ch // PIXELS_PER_BYTE)
   return {"first": first, "count": last - first + 1, "cell": (cw, ch), **metrics,
           "widths": [by_code.get(c, (0, blank))[0] for c in range(first, last + 1)],
           "glyphs": [by_code.get(c, (0, blank))[1] for c in range(first, last + 1)], "codes": set(by_code)}


def main(argv: list[str]) -> None:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return None
   """
   args = list(argv)
   try:
      page = text_encoder.option(args)
   except text_encoder.TextError as e:
      sys.exit(f"error: {e}")
   more = []
   while GLYPHS_OPTION in args:
      i = args.index(GLYPHS_OPTION)
      if i + 1 >= len(args):
         sys.exit(__doc__)
      more.append(Path(args[i + 1]))
      del args[i:i + OPTION_WORDS]
   if len(args) != ARGUMENT_COUNT:
      sys.exit(__doc__)
   src, dst, symbol = Path(args[SOURCE_ARGUMENT]), Path(args[OUTPUT_ARGUMENT]), args[SYMBOL_ARGUMENT]
   try:
      glyphs = parse_characters(src)
      for path in more:
         glyphs = merge(glyphs, parse_characters(path), path)
      lowest, laid = layout(glyphs, page)
   except text_encoder.TextError as e:
      sys.exit(f"{src}: error: {e}")
   text = emit(src.name, symbol, lowest, laid)
   dst.parent.mkdir(parents=True, exist_ok=True)
   if not dst.exists() or dst.read_bytes() != text.encode("utf-8"):
      dst.write_text(text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
   main(sys.argv)
