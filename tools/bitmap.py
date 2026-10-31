#!/usr/bin/env python3
"""!Bitmap support for the asset tools: PNG files and 15-color palettes.

Pure Python (zlib only), so builds need no imaging library and give the same
result everywhere. Used by tools/asset_compiler.py for image frames and by the
editor in capsule-studio for its previews, so both see the same colors.

Images are handled as (width, height, pixels) where pixels is a bytearray of
RGBA, 4 bytes per pixel, rows top to bottom.

Reading supports what editors and paint programs commonly write: non-interlaced
PNGs of bit depth 8 (gray, gray+alpha, RGB, RGBA, palette) and palette or gray
images of depth 1, 2 or 4, with tRNS transparency. 16-bit and interlaced files
are rejected with a message saying so. Palette PNGs can also be read as their
raw palette indices (read_png_indices(), I2: imported indexed graphics).

Colors are reduced to the GBA's 5 bits per channel. A pixel with alpha below
128 is transparent (palette entry 0); every other pixel is opaque.
"""
from __future__ import annotations

import struct
import zlib
from collections.abc import Iterable, Sequence
from os import PathLike

## PNG file signature.
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
## Bytes of the PNG file signature.
PNG_SIGNATURE_LENGTH = len(PNG_SIGNATURE)
## Bytes of a chunk's length and type fields, before its body.
PNG_CHUNK_HEADER = 8
## Bytes a chunk adds around its body: length, type and CRC.
PNG_CHUNK_OVERHEAD = 12
## Alpha at or above this is opaque.
ALPHA_OPAQUE = 128
## Colors available to one sprite or 4bpp tile palette (entry 0 is transparent).
MAX_COLORS = 15

## PNG row filter: none.
FILTER_NONE = 0
## PNG row filter: difference to the byte one pixel to the left.
FILTER_SUB = 1
## PNG row filter: difference to the byte above.
FILTER_UP = 2
## PNG row filter: difference to the average of left and above.
FILTER_AVERAGE = 3
## PNG row filter: difference to the Paeth predictor.
FILTER_PAETH = 4

## PNG color type: gray.
COLOR_GRAY = 0
## PNG color type: RGB.
COLOR_RGB = 2
## PNG color type: palette indices.
COLOR_PALETTE = 3
## PNG color type: gray and alpha.
COLOR_GRAY_ALPHA = 4
## PNG color type: RGBA.
COLOR_RGBA = 6
## Samples per pixel of each PNG color type.
CHANNELS_OF_COLOR = {COLOR_GRAY: 1, COLOR_RGB: 3, COLOR_PALETTE: 1, COLOR_GRAY_ALPHA: 2, COLOR_RGBA: 4}

## Bits in a byte.
BITS_PER_BYTE = 8
## The bit depth with one byte per sample (the only full depth supported).
BYTE_DEPTH = 8
## The 16-bit depth (rejected).
WIDE_DEPTH = 16
## Packed depths supported for gray and palette images.
PACKED_DEPTHS = (1, 2, 4)
## Mask keeping a sum to one byte (filter arithmetic is modulo 256).
BYTE_MASK = 255
## C10: the values of a byte (a translate table's entries).
BYTE_VALUES = 256
## Largest 8-bit channel value (also full alpha).
CHANNEL_MAX = 255
## zlib level used when writing PNGs (best compression).
ZLIB_LEVEL = 9

## Bytes per RGBA pixel.
RGBA_BYTES = 4
## Bytes per RGB pixel or palette entry.
RGB_BYTES = 3
## Bytes per gray+alpha pixel.
GRAY_ALPHA_BYTES = 2
## Index of blue in an RGB color or an RGBA pixel.
BLUE_OFFSET = 2
## Index of alpha in an RGBA pixel.
ALPHA_OFFSET = 3
## Color channels of an RGB color (red, green, blue).
RGB_CHANNELS = 3
## Bytes of a gray image's tRNS key (one 16-bit sample).
TRNS_GRAY_BYTES = 2
## Bytes of an RGB image's tRNS key (three 16-bit samples).
TRNS_RGB_BYTES = 6

## Bits dropped reducing an 8-bit channel to the GBA's 5 bits.
CHANNEL_DROP_BITS = 3
## Shift that brings a 5-bit channel's top bits down to fill the low bits when expanding to 8 bits.
CHANNEL_FILL_SHIFT = 2

## Length of a "#rrggbb" color string.
HEX_COLOR_LENGTH = 7
## Base of hex digits.
HEX_BASE = 16
## Hex digits per channel of a "#rrggbb" color.
HEX_DIGITS_PER_CHANNEL = 2
## Where each channel's digits start in a "#rrggbb" color.
HEX_CHANNEL_OFFSETS = (1, 3, 5)

## A box needs this many colors before it can be split.
MIN_SPLIT_COLORS = 2
## Divisor for half (the median cut and rounded averages).
HALF = 2
## Exponent of a squared distance.
SQUARE = 2

## An 8-bit (r, g, b) color.
Rgb = tuple[int, int, int]
## A 5-bit (r5, g5, b5) color.
Color5 = tuple[int, int, int]
## A decoded image: (width, height, RGBA pixels).
Image = tuple[int, int, bytes | bytearray]
## A 5-bit color and how many pixels use it.
WeightedColor = tuple[Color5, int]


class BitmapError(Exception):
   """!An image could not be read or does not fit the rules."""
   pass


def _paeth(a: int, b: int, c: int) -> int:
   """!PNG Paeth predictor.
   @param a  left
   @param b  above
   @param c  upper left
   @return the predicted byte
   """
   p = a + b - c
   pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
   if pa <= pb and pa <= pc:
      return a
   return b if pb <= pc else c


def _unfilter(data: bytes, height: int, bpp: int, row_bytes: int) -> list[bytearray]:
   """!Undoes PNG row filters.
   @param data       decompressed IDAT stream
   @param height     rows
   @param bpp        bytes per complete pixel, at least 1
   @param row_bytes  bytes per row, without the filter byte
   @return list of row bytearrays
   """
   rows, prev, pos = [], bytearray(row_bytes), 0
   if len(data) < height * (row_bytes + 1):
      raise BitmapError("image data is truncated")
   for _ in range(height):
      # Each row starts with its filter type byte.
      kind = data[pos]
      row = bytearray(data[pos + 1:pos + 1 + row_bytes])
      pos += row_bytes + 1
      if kind == FILTER_SUB:
         for i in range(bpp, row_bytes):
            row[i] = (row[i] + row[i - bpp]) & BYTE_MASK
      elif kind == FILTER_UP:
         for i in range(row_bytes):
            row[i] = (row[i] + prev[i]) & BYTE_MASK
      elif kind == FILTER_AVERAGE:
         for i in range(row_bytes):
            left = row[i - bpp] if i >= bpp else 0
            row[i] = (row[i] + ((left + prev[i]) >> 1)) & BYTE_MASK
      elif kind == FILTER_PAETH:
         for i in range(row_bytes):
            left = row[i - bpp] if i >= bpp else 0
            up_left = prev[i - bpp] if i >= bpp else 0
            row[i] = (row[i] + _paeth(left, prev[i], up_left)) & BYTE_MASK
      elif kind != FILTER_NONE:
         raise BitmapError(f"unknown PNG filter {kind}")
      rows.append(row)
      prev = row
   return rows


def _chunks(blob: bytes, name: str) -> tuple[tuple[int, ...] | None, bytes | None, bytes | None, bytearray]:
   """!Walks a PNG file's chunks: length, type, body, CRC (the CRC is not checked).
   @param blob  file contents
   @param name  file name for error messages
   @return (IHDR fields or None, PLTE body or None, tRNS body or None, the joined IDAT bodies)
   @throws BitmapError when the signature is wrong
   """
   if blob[:PNG_SIGNATURE_LENGTH] != PNG_SIGNATURE:
      raise BitmapError(f"{name}: not a PNG file")
   pos, ihdr, plte, trns, idat = PNG_SIGNATURE_LENGTH, None, None, None, bytearray()
   while pos + PNG_CHUNK_HEADER <= len(blob):
      length, ctype = struct.unpack(">I4s", blob[pos:pos + PNG_CHUNK_HEADER])
      body = blob[pos + PNG_CHUNK_HEADER:pos + PNG_CHUNK_HEADER + length]
      pos += PNG_CHUNK_OVERHEAD + length
      if ctype == b"IHDR":
         ihdr = struct.unpack(">IIBBBBB", body)
      elif ctype == b"PLTE":
         plte = body
      elif ctype == b"tRNS":
         trns = body
      elif ctype == b"IDAT":
         idat += body
      elif ctype == b"IEND":
         break
   return ihdr, plte, trns, idat


def decode_png_indices(blob: bytes, name: str = "image") -> tuple[int, int, bytearray]:
   """!Decodes a palette PNG (color type 3, depth 1, 2, 4 or 8) to its palette indices, as they are: no color is
   matched, so imported indexed graphics keep their exact pixel values (I2: fixed-placement tilesets).
   @param blob  file contents
   @param name  file name for error messages
   @return (width, height, one index per pixel, rows top to bottom)
   @throws BitmapError when the file is not a non-interlaced palette PNG
   """
   ihdr, plte, _, idat = _chunks(blob, name)
   if ihdr is None:
      raise BitmapError(f"{name}: missing IHDR")
   width, height, depth, color, _, _, interlace = ihdr
   if color != COLOR_PALETTE or plte is None:
      raise BitmapError(f"{name}: an indexed sheet needs a palette PNG (color type {COLOR_PALETTE})")
   if interlace:
      raise BitmapError(f"{name}: interlaced PNGs are not supported; save without interlacing")
   if depth != BYTE_DEPTH and depth not in PACKED_DEPTHS:
      raise BitmapError(f"{name}: unsupported bit depth {depth} for a palette PNG")
   try:
      data = zlib.decompress(bytes(idat))
   except zlib.error as e:
      raise BitmapError(f"{name}: damaged image data ({e})") from None
   row_bytes = (width * depth + BITS_PER_BYTE - 1) // BITS_PER_BYTE
   rows = _unfilter(data, height, 1, row_bytes)
   per = BITS_PER_BYTE // depth
   if per == 1:
      return width, height, bytearray(b"".join(bytes(row[:width]) for row in rows))
   # C10 (S1): packed depths unpacked a byte table at a time: the k-th pixel of every byte (the leftmost in the high
   # bits) through bytes.translate(), interleaved by slice assignment, each row cut to the image's width
   tables = _unpack_tables(depth)
   out = bytearray(width * height)
   wide = bytearray(row_bytes * per)
   for y, row in enumerate(rows):
      for k, table in enumerate(tables):
         wide[k::per] = bytes(row).translate(table)
      out[y * width:(y + 1) * width] = wide[:width]
   return width, height, out


def _unpack_tables(depth: int) -> list[bytes]:
   """!C10: for a packed depth, per pixel position in a byte (leftmost first) the 256-byte table that maps a byte to
   that pixel's index.
   @param depth  bits per pixel (1, 2 or 4)
   @return the tables
   """
   per = BITS_PER_BYTE // depth
   mask = (1 << depth) - 1
   return [bytes((b >> (BITS_PER_BYTE - depth * (k + 1))) & mask for b in range(BYTE_VALUES)) for k in range(per)]


def read_png_indices(path: str | PathLike[str]) -> tuple[int, int, bytearray]:
   """!Reads a palette PNG's indices (see decode_png_indices()).
   @param path  file path
   @return (width, height, indices)
   @throws BitmapError on an unsupported or damaged file
   """
   try:
      with open(path, "rb") as f:
         blob = f.read()
   except OSError as e:
      raise BitmapError(f"{path}: {e.strerror}") from None
   return decode_png_indices(blob, str(path))


def decode_png(blob: bytes, name: str = "image") -> tuple[int, int, bytearray]:
   """!Decodes a PNG file.
   @param blob  file contents
   @param name  file name for error messages
   @return (width, height, RGBA bytearray)
   @throws BitmapError on an unsupported or damaged file
   """
   ihdr, plte, trns, idat = _chunks(blob, name)
   if ihdr is None:
      raise BitmapError(f"{name}: missing IHDR")
   width, height, depth, color, _, _, interlace = ihdr
   if interlace:
      raise BitmapError(f"{name}: interlaced PNGs are not supported; save without interlacing")
   channels = CHANNELS_OF_COLOR.get(color)
   if channels is None:
      raise BitmapError(f"{name}: unknown PNG color type {color}")
   if depth == WIDE_DEPTH:
      raise BitmapError(f"{name}: 16-bit PNGs are not supported; save as 8-bit")
   if depth != BYTE_DEPTH and not (color in (COLOR_GRAY, COLOR_PALETTE) and depth in PACKED_DEPTHS):
      raise BitmapError(f"{name}: unsupported bit depth {depth} for color type {color}")
   if color == COLOR_PALETTE and plte is None:
      raise BitmapError(f"{name}: palette image without a palette")
   try:
      data = zlib.decompress(bytes(idat))
   except zlib.error as e:
      raise BitmapError(f"{name}: damaged image data ({e})") from None
   # Bits per row, rounded up to whole bytes.
   row_bytes = (width * channels * depth + BITS_PER_BYTE - 1) // BITS_PER_BYTE
   rows = _unfilter(data, height, max(1, channels * depth // BITS_PER_BYTE), row_bytes)
   out = bytearray(width * height * RGBA_BYTES)
   # tRNS of gray and RGB images names one color (16-bit samples) that is transparent.
   gray_key = struct.unpack(">H", trns[:TRNS_GRAY_BYTES])[0] \
      if color == COLOR_GRAY and trns and len(trns) >= TRNS_GRAY_BYTES else None
   rgb_key = struct.unpack(">HHH", trns[:TRNS_RGB_BYTES]) \
      if color == COLOR_RGB and trns and len(trns) >= TRNS_RGB_BYTES else None
   for y, row in enumerate(rows):
      for x in range(width):
         o = (y * width + x) * RGBA_BYTES
         if depth < BYTE_DEPTH:   # gray or palette, packed high bits first
            per = BITS_PER_BYTE // depth
            v = (row[x // per] >> (BITS_PER_BYTE - depth * (x % per + 1))) & ((1 << depth) - 1)
            if color == COLOR_PALETTE:
               r, g, b = plte[v * RGB_BYTES:v * RGB_BYTES + RGB_BYTES]
               a = trns[v] if trns and v < len(trns) else CHANNEL_MAX
            else:
               a = 0 if gray_key == v else CHANNEL_MAX
               # Scale the packed gray level to the full 8-bit range.
               v = v * CHANNEL_MAX // ((1 << depth) - 1)
               r = g = b = v
         elif color == COLOR_GRAY:
            r = g = b = row[x]
            a = 0 if gray_key == row[x] else CHANNEL_MAX
         elif color == COLOR_RGB:
            r, g, b = row[x * RGB_BYTES:x * RGB_BYTES + RGB_BYTES]
            a = 0 if rgb_key == (r, g, b) else CHANNEL_MAX
         elif color == COLOR_PALETTE:
            v = row[x]
            if v * RGB_BYTES + RGB_BYTES > len(plte):
               raise BitmapError(f"{name}: palette index {v} out of range")
            r, g, b = plte[v * RGB_BYTES:v * RGB_BYTES + RGB_BYTES]
            a = trns[v] if trns and v < len(trns) else CHANNEL_MAX
         elif color == COLOR_GRAY_ALPHA:
            r = g = b = row[x * GRAY_ALPHA_BYTES]
            a = row[x * GRAY_ALPHA_BYTES + 1]
         else:
            r, g, b, a = row[x * RGBA_BYTES:x * RGBA_BYTES + RGBA_BYTES]
         out[o:o + RGBA_BYTES] = bytes((r, g, b, a))
   return width, height, out


def read_png(path: str | PathLike[str]) -> tuple[int, int, bytearray]:
   """!Reads a PNG file.
   @param path  file path
   @return (width, height, RGBA bytearray)
   @throws BitmapError on an unsupported or damaged file
   """
   try:
      with open(path, "rb") as f:
         blob = f.read()
   except OSError as e:
      raise BitmapError(f"{path}: {e.strerror}") from None
   return decode_png(blob, str(path))


def encode_png(width: int, height: int, rgba: bytes | bytearray) -> bytes:
   """!Encodes an RGBA image as a PNG (8-bit RGBA, no filters). The output
   depends only on the pixels, so saved frames don't change between runs.
   @param width   pixels
   @param height  pixels
   @param rgba    width * height * 4 bytes
   @return the file contents
   """
   if len(rgba) != width * height * RGBA_BYTES:
      raise BitmapError("pixel data does not match the size")

   def chunk(ctype: bytes, body: bytes) -> bytes:
      """!One PNG chunk: length, type, body and the CRC of type and body.
      @param ctype  four-letter chunk type
      @param body   chunk data
      @return the chunk's bytes
      """
      return struct.pack(">I", len(body)) + ctype + body + struct.pack(">I", zlib.crc32(ctype + body))

   raw = bytearray()
   for y in range(height):
      raw.append(FILTER_NONE)
      raw += rgba[y * width * RGBA_BYTES:(y + 1) * width * RGBA_BYTES]
   # IHDR: size, depth, color type, then compression, filter and interlace methods (all 0).
   return (PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, BYTE_DEPTH, COLOR_RGBA, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(raw), ZLIB_LEVEL)) + chunk(b"IEND", b""))


def write_png(path: str | PathLike[str], width: int, height: int, rgba: bytes | bytearray) -> None:
   """!Writes an RGBA image as a PNG file (see encode_png()).
   @param path    file path
   @param width   pixels
   @param height  pixels
   @param rgba    width * height * 4 bytes
   @return None
   """
   with open(path, "wb") as f:
      f.write(encode_png(width, height, rgba))


def to_rgb555(r: int, g: int, b: int) -> Color5:
   """!Reduces an 8-bit color to 5 bits per channel.
   @param r  red, 0-255
   @param g  green, 0-255
   @param b  blue, 0-255
   @return (r5, g5, b5)
   """
   return r >> CHANNEL_DROP_BITS, g >> CHANNEL_DROP_BITS, b >> CHANNEL_DROP_BITS


def expand5(c: int) -> int:
   """!Expands a 5-bit channel to 8 bits so that reducing it again gives @p c.
   @param c  0-31
   @return 0-255
   """
   # Repeat the top bits in the low bits, so 31 becomes 255 and 0 stays 0.
   return (c << CHANNEL_DROP_BITS) | (c >> CHANNEL_FILL_SHIFT)


def hex_color(c5: Sequence[int]) -> str:
   """!A 5-bit color as a manifest color string.
   @param c5  (r5, g5, b5)
   @return a hash sign and six hex digits
   """
   return "#{:02x}{:02x}{:02x}".format(*(expand5(c) for c in c5))


def parse_hex(value: object) -> Color5:
   """!A manifest color string as a 5-bit color.
   @param value  a hash sign and six hex digits
   @return (r5, g5, b5)
   @throws BitmapError on a malformed string
   """
   if not isinstance(value, str) or len(value) != HEX_COLOR_LENGTH or value[0] != "#":
      raise BitmapError(f"colors are strings like '#336699', got {value!r}")
   try:
      return to_rgb555(*(int(value[i:i + HEX_DIGITS_PER_CHANNEL], HEX_BASE) for i in HEX_CHANNEL_OFFSETS))
   except ValueError:
      raise BitmapError(f"bad color {value!r}") from None


def opaque_colors(images: Iterable[Image]) -> list[WeightedColor]:
   """!Counts the opaque 5-bit colors of some images.
   @param images  list of (width, height, RGBA)
   @return list of (color, count) in order of first appearance
   """
   counts, order = {}, []
   for _, _, px in images:
      for o in range(0, len(px), RGBA_BYTES):
         if px[o + ALPHA_OFFSET] < ALPHA_OPAQUE:
            continue
         c = to_rgb555(px[o], px[o + 1], px[o + BLUE_OFFSET])
         if c not in counts:
            counts[c] = 0
            order.append(c)
         counts[c] += 1
   return [(c, counts[c]) for c in order]


def _median_cut(colors: Iterable[WeightedColor], n: int) -> list[Color5]:
   """!Splits weighted colors into at most @p n boxes and averages each.
   @param colors  list of (color, count), at least one
   @param n       boxes wanted
   @return list of 5-bit colors
   """
   boxes = [list(colors)]
   while len(boxes) < n:
      # Split the box whose widest channel range, weighted by pixel count, is largest.
      best, best_score, axis = None, 0, 0
      for i, box in enumerate(boxes):
         if len(box) < MIN_SPLIT_COLORS:
            continue
         for ax in range(RGB_CHANNELS):
            lo, hi = min(c[0][ax] for c in box), max(c[0][ax] for c in box)
            score = (hi - lo) * sum(c[1] for c in box)
            if score > best_score:
               best, best_score, axis = i, score, ax
      if best is None:
         break
      # Cut along that channel where half the pixel weight has been passed.
      box = sorted(boxes[best], key=lambda c: (c[0][axis], c[0]))
      total, run, cut = sum(c[1] for c in box), 0, 1
      for j, c in enumerate(box[:-1]):
         run += c[1]
         cut = j + 1
         if run * HALF >= total:
            break
      boxes[best:best + 1] = [box[:cut], box[cut:]]
   out = []
   for box in boxes:
      # Weighted average per channel, rounded to nearest.
      total = sum(c[1] for c in box)
      out.append(tuple((sum(c[0][ax] * c[1] for c in box) + total // HALF) // total for ax in range(RGB_CHANNELS)))
   return out


def key_out(image: Image, rgb: Sequence[int]) -> tuple[int, int, bytes]:
   """!Makes every pixel of one color transparent (a color-key image).
   @param image  (width, height, RGBA)
   @param rgb    (r, g, b), 8 bits per channel; matched exactly
   @return (width, height, RGBA) with those pixels' alpha 0
   """
   w, h, px = image
   out = bytearray(px)
   r, g, b = rgb
   for i in range(0, len(out), RGBA_BYTES):
      if out[i] == r and out[i + 1] == g and out[i + BLUE_OFFSET] == b:
         out[i + ALPHA_OFFSET] = 0
   return w, h, bytes(out)


def parse_rgb(value: object) -> Rgb | None:
   """!Reads a "#rrggbb" color.
   @param value  the text
   @return (r, g, b), or None if it is not "#rrggbb"
   """
   if not isinstance(value, str) or len(value) != HEX_COLOR_LENGTH or value[0] != "#":
      return None
   try:
      red, green, blue = (int(value[i:i + HEX_DIGITS_PER_CHANNEL], HEX_BASE) for i in HEX_CHANNEL_OFFSETS)
      return red, green, blue
   except ValueError:
      return None


def make_palette(images: Iterable[Image], max_colors: int = MAX_COLORS) -> list[Color5]:
   """!Chooses a palette for some images: their exact colors when there are
   few enough, otherwise a median-cut reduction weighted by pixel count.
   @param images      list of (width, height, RGBA)
   @param max_colors  1-15
   @return list of 5-bit colors (palette entries 1..n), at least one
   """
   colors = opaque_colors(images)
   if not colors:
      return [(0, 0, 0)]
   if len(colors) <= max_colors:
      return [c for c, _ in colors]
   return _median_cut(colors, max_colors)


def nearest(c: Sequence[int], palette: Sequence[Sequence[int]]) -> int:
   """!The palette entry closest to a color (ties go to the lower entry).
   @param c        5-bit color
   @param palette  list of 5-bit colors (entries 1..n)
   @return entry number, 1-based
   """
   best, best_d = 1, None
   for i, p in enumerate(palette):
      # Squared distance in 5-bit RGB space.
      d = (c[0] - p[0]) ** SQUARE + (c[1] - p[1]) ** SQUARE + (c[BLUE_OFFSET] - p[BLUE_OFFSET]) ** SQUARE
      if best_d is None or d < best_d:
         best, best_d = i + 1, d
         if d == 0:
            break
   return best


def index_image(image: Image, palette: Sequence[Sequence[int]]) -> bytes:
   """!Maps an image onto a palette.
   @param image    (width, height, RGBA)
   @param palette  list of 5-bit colors (entries 1..n)
   @return bytes of entry numbers, 0 for transparent pixels
   """
   _, _, px = image
   out, cache = bytearray(len(px) // RGBA_BYTES), {}
   for o in range(0, len(px), RGBA_BYTES):
      if px[o + ALPHA_OFFSET] < ALPHA_OPAQUE:
         continue
      c = to_rgb555(px[o], px[o + 1], px[o + BLUE_OFFSET])
      if c not in cache:
         cache[c] = nearest(c, palette)
      out[o // RGBA_BYTES] = cache[c]
   return bytes(out)


def indexed_to_rgba(indices: bytes | bytearray | Sequence[int], palette: Sequence[Sequence[int]]) -> bytearray:
   """!Renders indexed pixels as the GBA shows them (5-bit colors expanded).
   @param indices  entry numbers, 0 = transparent
   @param palette  list of 5-bit colors (entries 1..n)
   @return RGBA bytearray
   """
   out = bytearray(len(indices) * RGBA_BYTES)
   for i, v in enumerate(indices):
      if v:
         r, g, b = palette[v - 1]
         out[i * RGBA_BYTES:i * RGBA_BYTES + RGBA_BYTES] = bytes((expand5(r), expand5(g), expand5(b), CHANNEL_MAX))
   return out
