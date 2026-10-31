#!/usr/bin/env python3
"""!Tests for tools/bitmap.py (run by CTest, or directly)."""
from __future__ import annotations

import struct
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import bitmap  # noqa: E402


def png(width: int, height: int, color: int, depth: int, rows: list[bytes], plte: bytes | None = None,
        trns: bytes | None = None, filters: list[int] | None = None) -> bytes:
   """!Builds a PNG from raw (unfiltered) rows, applying the given filters.
   @param width    pixels
   @param height   pixels
   @param color    PNG color type
   @param depth    bit depth
   @param rows     list of row bytes, unfiltered
   @param plte     PLTE body, or None
   @param trns     tRNS body, or None
   @param filters  filter type per row (default 0)
   @return the file contents
   """
   # Channels per PNG color type; bpp is the filter's byte distance to the "left" pixel.
   channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
   bpp = max(1, channels * depth // 8)
   raw, prev = bytearray(), bytes(len(rows[0]))
   for y, row in enumerate(rows):
      kind = filters[y] if filters else 0
      out = bytearray()
      for i, v in enumerate(row):
         left = row[i - bpp] if i >= bpp else 0
         up_left = prev[i - bpp] if i >= bpp else 0
         # Predictor per filter type: none, sub, up, average, Paeth.
         pred = [0, left, prev[i], (left + prev[i]) >> 1, bitmap._paeth(left, prev[i], up_left)][kind]
         out.append((v - pred) & 255)
      raw += bytes([kind]) + out
      prev = row

   def chunk(t: bytes, b: bytes) -> bytes:
      # Length, type, body, then the CRC of type and body.
      return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))

   blob = bitmap.PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color, 0, 0, 0))
   if plte is not None:
      blob += chunk(b"PLTE", plte)
   if trns is not None:
      blob += chunk(b"tRNS", trns)
   return blob + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")


class BitmapTests(unittest.TestCase):
   """!PNG reading and writing, palettes."""

   def test_round_trip(self) -> None:
      """!encode_png() output decodes to the same pixels."""
      px = bytearray(range(256)) * 1   # 8x8 RGBA
      w, h, out = bitmap.decode_png(bitmap.encode_png(8, 8, px))
      self.assertEqual((w, h, out), (8, 8, px))

   def test_every_filter_rgba(self) -> None:
      """!All five row filters undo correctly."""
      rows = [bytes((x * 7 + y * 13) & 255 for x in range(4 * 5)) for y in range(5)]
      blob = png(5, 5, 6, 8, rows, filters=[0, 1, 2, 3, 4])
      _, _, out = bitmap.decode_png(blob)
      self.assertEqual(bytes(out), b"".join(rows))

   def test_palette_4bit_with_transparency(self) -> None:
      """!Packed palette pixels, tRNS alpha per entry."""
      plte = bytes((255, 0, 0, 0, 255, 0))
      rows = [bytes((0x01, 0x10))]   # pixels 0,1,1,0
      _, _, out = bitmap.decode_png(png(4, 1, 3, 4, rows, plte=plte, trns=bytes((0,))))
      self.assertEqual(bytes(out), bytes((255, 0, 0, 0, 0, 255, 0, 255, 0, 255, 0, 255, 255, 0, 0, 0)))

   def test_rgb_color_key_and_gray(self) -> None:
      """!RGB with a tRNS color key; gray+alpha."""
      rows = [bytes((10, 20, 30, 1, 2, 3))]
      _, _, out = bitmap.decode_png(png(2, 1, 2, 8, rows, trns=struct.pack(">HHH", 1, 2, 3)))
      self.assertEqual(out[3], 255)
      self.assertEqual(out[7], 0)
      _, _, out = bitmap.decode_png(png(1, 1, 4, 8, [bytes((99, 7))]))
      self.assertEqual(bytes(out), bytes((99, 99, 99, 7)))

   def test_rejects_unsupported(self) -> None:
      """!16-bit and non-PNG input give clear errors."""
      with self.assertRaises(bitmap.BitmapError) as ctx:
         bitmap.decode_png(png(1, 1, 0, 16, [bytes(2)]))
      self.assertIn("16-bit", str(ctx.exception))
      with self.assertRaises(bitmap.BitmapError):
         bitmap.decode_png(b"GIF89a")

   def test_exact_palette_in_appearance_order(self) -> None:
      """!Few colors: kept exactly, in order of first appearance; alpha < 128 skipped."""
      px = bytes((0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 255, 255, 9, 9, 9, 10))
      pal = bitmap.make_palette([(4, 1, px)])
      self.assertEqual(pal, [(0, 0, 31), (31, 0, 0)])
      self.assertEqual(bitmap.index_image((4, 1, px), pal), bytes((1, 2, 1, 0)))
      self.assertEqual(bitmap.hex_color(pal[1]), "#ff0000")
      self.assertEqual(bitmap.parse_hex("#ff0000"), (31, 0, 0))

   def test_reduction_to_fifteen(self) -> None:
      """!A 32-step gray ramp reduces to 15 entries that cover the range."""
      px = bytearray()
      for v in range(32):
         px += bytes((v * 8, v * 8, v * 8, 255))
      pal = bitmap.make_palette([(32, 1, px)])
      self.assertEqual(len(pal), 15)
      idx = bitmap.index_image((32, 1, px), pal)
      for v in range(32):   # every pixel lands within 2 steps of its color
         self.assertLessEqual(abs(pal[idx[v] - 1][0] - v), 2)
      self.assertEqual(pal, bitmap.make_palette([(32, 1, px)]))   # deterministic


if __name__ == "__main__":
   unittest.main()
