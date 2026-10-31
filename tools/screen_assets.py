"""!UC (D-33): the screen graphics kinds of capsule.assets/1 (asset::Tiles, asset::Tilemap, asset::Palette), compiled
for tools/asset_compiler.py.

A full-screen scene (capsule/screen/screen.h) loads tiles, tilemaps and palettes where its own plan says, so the
three are separate assets:

  - tiles: {"key": "tiles:NAME", "kind": "tiles", "data": "x.4bpp", "bpp": 4}: raw tile bytes as video memory
    holds them (4bpp: 32 bytes a tile, pixel 0 in the low nibble; 8bpp: 64 bytes, pixel 0 in the low byte), or
    "shapes" drawn with palette indices on a "width" x "height" canvas (pixels, multiples of 8; "fill" its first
    index), cut into tiles row by row, or by blocks of "block": [columns, rows] tiles (sprite frames: each block's
    tiles row by row, the blocks in reading order). "columns" (optional) records the source image's tiles per row.
  - tilemap: {"key": "tilemap:NAME", "kind": "tilemap", "data": "x.bin"}: raw little-endian 16-bit text-mode map
    entries as stored (tile, flips, palette bank), or "cells": [entries]; "affine": true for one byte a cell of an
    affine layer's map; "columns": entries per stored row (default 32: screen-block order).
  - palette: {"key": "palette:NAME", "kind": "palette", "colors": [...]}: colours bank by bank (16 a bank), each a
    "#rrggbb" string (8-bit channels, the low three bits dropped) or a raw BGR555 integer 0-32767; entry 0 of each
    bank is kept as given.
"""
from __future__ import annotations

from collections.abc import Callable, Sequence
from os import PathLike
from pathlib import Path
from typing import Any

import manifest_paths

## A manifest object.
JsonDict = dict[str, Any]

## Keys of each kind (asset_compiler.KIND_KEYS merges them in).
KIND_KEYS = {
   "tiles": {"key", "kind", "data", "bpp", "shapes", "width", "height", "fill", "block", "columns"},
   "tilemap": {"key", "kind", "data", "cells", "affine", "columns"},
   "palette": {"key", "kind", "colors"},
}
## The kinds whose "data" is a file path (asset_compiler's includes rewrite it).
DATA_KINDS = ("tiles", "tilemap")
## asset::Kind numbers of the three (after asset_compiler's KIND_TEXT_BOX).
KIND_TILES = 7
## asset::Kind::Tilemap.
KIND_TILEMAP = 8
## asset::Kind::Palette.
KIND_PALETTE = 9
## The kinds' C++ names, in that order.
KIND_NAMES = ("Kind::Tiles", "Kind::Tilemap", "Kind::Palette")

## Bits per pixel of a 4bpp tile.
BPP4 = 4
## Bits per pixel of an 8bpp tile.
BPP8 = 8
## Pixels per byte of a 4bpp tile.
PIXELS_PER_BYTE_4BPP = 2
## Values of a [columns, rows] pair.
PAIR = 2
## Pixels per tile side.
TILE_SIZE = 8
## Bytes of one 4bpp tile.
TILE_BYTES_4BPP = 32
## Bytes of one 8bpp tile.
TILE_BYTES_8BPP = 64
## Bytes per 32-bit word.
WORD_BYTES = 4
## Highest palette index of a 4bpp pixel.
MAX_INDEX_4BPP = 15
## Highest palette index of an 8bpp pixel.
MAX_INDEX_8BPP = 255
## Most tiles in one block (a character block holds 512 4bpp tiles; sprite memory 1024): asset::Tiles::count is 16 bits.
MAX_TILES = 0xFFFF
## Most entries of one tilemap (asset::Tilemap::count is 16 bits).
MAX_TILEMAP_ENTRIES = 0xFFFF
## Highest 16-bit map entry.
MAX_ENTRY = 0xFFFF
## Highest affine map entry (a tile byte).
MAX_AFFINE_ENTRY = 0xFF
## Bytes of one text-mode map entry.
ENTRY_BYTES = 2
## Default entries per stored row (one screen block's width).
DEFAULT_COLUMNS = 32
## Highest columns value (asset::Tilemap::columns and asset::Tiles::columns are 8 bits).
MAX_COLUMNS = 255
## Most colours of one palette (BG + OBJ memory).
MAX_COLORS = 512
## Highest raw BGR555 colour.
MAX_BGR555 = 0x7FFF
## Values per generated source line.
VALUES_PER_LINE = 8
## Narrow values per generated source line.
NARROW_PER_LINE = 16


class ScreenAssetError(Exception):
   """!A screen graphics asset that cannot be compiled (asset_compiler turns it into its ManifestError)."""


def read_data(path: object, base: str | PathLike[str] | None, deps: list[Path] | None, where: str) -> bytes:
   """!The bytes of a data file named by the manifest.
   @param path   relative path with '/' separators
   @param base   the manifest's directory
   @param deps   list the resolved path is appended to, or None
   @param where  location for errors
   @return the file's bytes
   @throws ScreenAssetError on a bad path or a missing file
   """
   if not isinstance(path, str) or not path or manifest_paths.is_absolute_text(path) or "\\" in path:
      raise ScreenAssetError(f"{where}: data paths are relative, with '/' separators: {path!r}")
   if base is None:
      raise ScreenAssetError(f"{where}: data files need the manifest's directory")
   full = (Path(base) / path).resolve()
   if deps is not None:
      deps.append(full)
   try:
      return full.read_bytes()
   except OSError as e:
      raise ScreenAssetError(f"{where}: {path}: {e.strerror}") from None


def words_of(blob: bytes) -> list[int]:
   """!Little-endian 32-bit words of a byte string (its length a multiple of 4).
   @param blob  the bytes
   @return the words
   """
   return [int.from_bytes(blob[i:i + WORD_BYTES], "little") for i in range(0, len(blob), WORD_BYTES)]


def pack_tile(rows: Sequence[Sequence[int]], bpp: int) -> bytes:
   """!One tile's bytes as video memory holds them.
   @param rows  8 rows of 8 palette indices
   @param bpp   4 or 8
   @return 32 or 64 bytes
   """
   out = bytearray()
   for row in rows:
      if bpp == BPP8:
         out += bytes(row)
      else:
         out += bytes(row[i] | row[i + 1] << BPP4 for i in range(0, TILE_SIZE, PIXELS_PER_BYTE_4BPP))
   return bytes(out)


def int_value(a: JsonDict, key: str, low: int, high: int, default: int, where: str) -> int:
   """!An optional integer key.
   @param a        the asset
   @param key      its key
   @param low      lowest value
   @param high     highest value
   @param default  value when left out
   @param where    location for errors
   @return the value
   """
   v = a.get(key, default)
   if not isinstance(v, int) or isinstance(v, bool) or not low <= v <= high:
      raise ScreenAssetError(f"{where}: {key} must be {low}-{high}")
   return v


def compile_tiles(a: JsonDict, base: str | PathLike[str] | None, deps: list[Path] | None, where: str,
                  draw_shapes: Callable[[int, int, int, list[Any], int, str], list[list[int]]]) -> JsonDict:
   """!A tiles asset.
   @param a            the asset
   @param base         the manifest's directory
   @param deps         dependency list, or None
   @param where        location for errors
   @param draw_shapes  asset_compiler's canvas: (width, height, fill, shapes, highest index, where) -> pixel rows
   @return {"words", "count", "bpp", "columns", "key"}
   """
   bpp = a.get("bpp", BPP4)
   if bpp not in (BPP4, BPP8):
      raise ScreenAssetError(f"{where}: bpp must be 4 or 8")
   tile_bytes = TILE_BYTES_8BPP if bpp == BPP8 else TILE_BYTES_4BPP
   columns = int_value(a, "columns", 0, MAX_COLUMNS, 0, where)
   if ("data" in a) == ("shapes" in a):
      raise ScreenAssetError(f"{where}: tiles take \"data\" (raw tile bytes) or \"shapes\", one of them")
   if "data" in a:
      for k in ("width", "height", "fill", "block"):
         if k in a:
            raise ScreenAssetError(f"{where}: {k} belongs to shapes")
      blob = read_data(a["data"], base, deps, where)
      if not blob or len(blob) % tile_bytes:
         raise ScreenAssetError(f"{where}: {a['data']} is {len(blob)} bytes, not whole {bpp}bpp tiles")
   else:
      w, h = a.get("width"), a.get("height")
      if (not isinstance(w, int) or not isinstance(h, int) or w <= 0 or h <= 0 or w % TILE_SIZE or
              h % TILE_SIZE):
         raise ScreenAssetError(f"{where}: width and height are positive multiples of 8 pixels")
      highest = MAX_INDEX_8BPP if bpp == BPP8 else MAX_INDEX_4BPP
      fill = int_value(a, "fill", 0, highest, 0, where)
      shapes = a.get("shapes")
      if not isinstance(shapes, list):
         raise ScreenAssetError(f"{where}: shapes is a list")
      px = draw_shapes(w, h, fill, shapes, highest, where)
      block = a.get("block", [w // TILE_SIZE, h // TILE_SIZE])
      if (not isinstance(block, list) or len(block) != PAIR or
              any(not isinstance(v, int) or isinstance(v, bool) or v <= 0 for v in block) or
              (w // TILE_SIZE) % block[0] or (h // TILE_SIZE) % block[1]):
         raise ScreenAssetError(f"{where}: block is [columns, rows] of tiles that divide the canvas")
      bw, bh = block
      out = bytearray()
      for by in range(0, h // TILE_SIZE, bh):
         for bx in range(0, w // TILE_SIZE, bw):
            for ty in range(by, by + bh):
               for tx in range(bx, bx + bw):
                  rows = [px[ty * TILE_SIZE + y][tx * TILE_SIZE:(tx + 1) * TILE_SIZE] for y in range(TILE_SIZE)]
                  out += pack_tile(rows, bpp)
      blob = bytes(out)
   count = len(blob) // tile_bytes
   if count > MAX_TILES:
      raise ScreenAssetError(f"{where}: {count} tiles (at most {MAX_TILES})")
   return {"key": a["key"], "words": words_of(blob), "count": count, "bpp": bpp, "columns": columns}


def compile_tilemap(a: JsonDict, base: str | PathLike[str] | None, deps: list[Path] | None, where: str) -> JsonDict:
   """!A tilemap asset.
   @param a      the asset
   @param base   the manifest's directory
   @param deps   dependency list, or None
   @param where  location for errors
   @return {"cells", "affine", "count", "columns", "key"}
   """
   affine = a.get("affine", False)
   if not isinstance(affine, bool):
      raise ScreenAssetError(f"{where}: affine is true or false")
   columns = int_value(a, "columns", 1, MAX_COLUMNS, DEFAULT_COLUMNS, where)
   if ("data" in a) == ("cells" in a):
      raise ScreenAssetError(f"{where}: a tilemap takes \"data\" (a raw file) or \"cells\", one of them")
   highest = MAX_AFFINE_ENTRY if affine else MAX_ENTRY
   if "data" in a:
      blob = read_data(a["data"], base, deps, where)
      if affine:
         cells = list(blob)
      else:
         if len(blob) % ENTRY_BYTES:
            raise ScreenAssetError(f"{where}: {a['data']} is {len(blob)} bytes, not whole 16-bit entries")
         cells = [int.from_bytes(blob[i:i + ENTRY_BYTES], "little") for i in range(0, len(blob), ENTRY_BYTES)]
   else:
      cells = a["cells"]
      if not isinstance(cells, list) or any(not isinstance(v, int) or isinstance(v, bool) or not 0 <= v <= highest
                                            for v in cells):
         raise ScreenAssetError(f"{where}: cells are entries 0-{highest}")
   if not cells or len(cells) > MAX_TILEMAP_ENTRIES:
      raise ScreenAssetError(f"{where}: a tilemap has 1-{MAX_TILEMAP_ENTRIES} entries")
   return {"key": a["key"], "cells": cells, "affine": affine, "count": len(cells), "columns": columns}


def compile_palette(a: JsonDict, where: str, hex_color: Callable[[object, str], int]) -> JsonDict:
   """!A palette asset.
   @param a          the asset
   @param where      location for errors
   @param hex_color  asset_compiler.bgr555 ("#rrggbb" -> BGR555)
   @return {"colors", "key"}
   """
   colors = a.get("colors")
   if not isinstance(colors, list) or not 1 <= len(colors) <= MAX_COLORS:
      raise ScreenAssetError(f"{where}: colors lists 1-{MAX_COLORS} colours")
   out: list[int] = []
   for i, c in enumerate(colors):
      if isinstance(c, int) and not isinstance(c, bool):
         if not 0 <= c <= MAX_BGR555:
            raise ScreenAssetError(f"{where}: colour {i} is a BGR555 value 0-{MAX_BGR555}")
         out.append(c)
      else:
         out.append(hex_color(c, f"{where} colour {i}"))
   return {"key": a["key"], "colors": out}


def emit_lines(tiles: Sequence[JsonDict], tilemaps: Sequence[JsonDict], palettes: Sequence[JsonDict]) -> list[str]:
   """!The C++ definitions of the three tables (kTileBlocks, kTilemaps, kPalettes; only those with entries).
   @param tiles     compiled tiles
   @param tilemaps  compiled tilemaps
   @param palettes  compiled palettes
   @return source lines
   """
   lines: list[str] = []

   def arr(ctype: str, name: str, values: Sequence[int], per_line: int, fmt: str) -> None:
      """!Appends one array.
      @param ctype     element type
      @param name      its name
      @param values    elements
      @param per_line  elements per line
      @param fmt       element format
      @return None
      """
      lines.append(f"alignas(4) const {ctype} {name}[] = {{")
      for i in range(0, len(values), per_line):
         lines.append("    " + ", ".join(fmt.format(v) for v in values[i:i + per_line]) + ",")
      lines.append("};")

   for i, t in enumerate(tiles):
      arr("uint32_t", f"kTileWords{i}", t["words"], VALUES_PER_LINE, "0x{:08X}")
   if tiles:
      lines.append("const Tiles kTileBlocks[] = {")
      lines.extend(f"    {{kTileWords{i}, {t['count']}, {t['bpp']}, {t['columns']}}},   // {t['key']}"
                   for i, t in enumerate(tiles))
      lines.append("};")
   for i, m in enumerate(tilemaps):
      if m["affine"]:
         arr("uint8_t", f"kTilemapBytes{i}", m["cells"], NARROW_PER_LINE, "0x{:02X}")
      else:
         arr("uint16_t", f"kTilemapCells{i}", m["cells"], NARROW_PER_LINE, "0x{:04X}")
   if tilemaps:
      lines.append("const Tilemap kTilemaps[] = {")
      for i, m in enumerate(tilemaps):
         cells, affine = ("nullptr", f"kTilemapBytes{i}") if m["affine"] else (f"kTilemapCells{i}", "nullptr")
         lines.append(f"    {{{cells}, {affine}, {m['count']}, {m['columns']}, 0}},   // {m['key']}")
      lines.append("};")
   for i, p in enumerate(palettes):
      arr("Color", f"kPaletteColors{i}", p["colors"], NARROW_PER_LINE, "0x{:04X}")
   if palettes:
      lines.append("const Palette kPalettes[] = {")
      lines.extend(f"    {{kPaletteColors{i}, {len(p['colors'])}, 0}},   // {p['key']}" for i, p in enumerate(palettes))
      lines.append("};")
   return lines


def rom_bytes(tiles: Sequence[JsonDict], tilemaps: Sequence[JsonDict], palettes: Sequence[JsonDict]) -> int:
   """!Bytes the three tables' data take.
   @param tiles     compiled tiles
   @param tilemaps  compiled tilemaps
   @param palettes  compiled palettes
   @return the bytes
   """
   return (sum(len(t["words"]) * WORD_BYTES for t in tiles) +
           sum(m["count"] * (1 if m["affine"] else ENTRY_BYTES) for m in tilemaps) +
           sum(len(p["colors"]) * ENTRY_BYTES for p in palettes))
