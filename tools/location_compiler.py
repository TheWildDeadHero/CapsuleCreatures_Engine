#!/usr/bin/env python3
"""!Locations (D-17): the named places maps belong to, their name banners
(a sliding name box or frame) and location previews (a full-screen sequence on
entry), and the world's default warp transition. Called by map_compiler.py; not run on its own.

World, optional keys:

    "transition": "fade",                     // warps that name none (default fade)
    "banner_styles": {
      "box":  {"ink": "#f8f8f8", "shadow": "#303030", "fill": "#284878", "border": "#f8f8f8",
               "frames": 120, "slide": 8},
      "sign": {"frame": "banners/sign.png", "ink": "#302010", "shadow": "#a08060", "fill": "#e8c890",
               "frames": 120, "slide": 8}
    },
    "locations": {
      "plaza": {"name": "Showcase Plaza", "banner": "box",
                "preview": {"show": "first", "sequences": [
                    {"sequence": "sequences/plaza_day.json"},
                    {"when": "night", "sequence": "sequences/plaza_night.json"}]}}
    }

Map, optional key: "location": "plaza".

- A **banner** slides in at the top left when the player enters a map whose
  location differs from the last one (after the transition); it shows the
  location's name. A style without "frame" draws a plain box in its
  colors; "frame" names a 24x24 PNG (3x3 tiles: corners, edges, interior)
  whose pixels in the "fill" color become the text background (the glyphs
  are drawn in "ink" with a "shadow").
- A **preview** is a sequence (capsule.sequence/1, tools/sequence_compiler.py)
  played full screen while the map is hidden, when the player enters the
  location: "show" "always" (default) or "first" (first visit only, a
  permanent bit per location). With several, the last whose "when"
  (encounter condition names: time of day, ...) all hold is played; a
  morning / day / evening / night set of previews is four such entries.

Checks (build errors): unknown keys, styles, locations and condition names;
names that are empty, too long for a banner (28 characters) or not in the
font; frames that are not 24x24 or use more than 15 colors; sequences that
do not compile. When no preview entry's conditions hold, none shows.

Field placeholders (D-22 decision 6): when the game's scripts manifest
declares "placeholders" (tools/script_compiler.py), location names are field
text: `{player}` and the other declared names print the field resolver's
text, and a name's length counts each placeholder at the most it prints.
Otherwise braces are plain characters.
"""
from __future__ import annotations

import json
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import Any, Protocol

import bitmap
import event_compiler as evc
import sequence_compiler as seqc
import text_encoder
from asset_compiler import TILE_SIZE as TILE_PIXELS, encode_text, ManifestError as AssetError


## Codes of an escape (FC or FD and the code it carries).
ESCAPE_CODES = 2


class FieldText(Protocol):
   """!What compile_locations() needs of the game's field placeholders (script_compiler.FieldPlaceholders)."""

   ## Placeholder name -> argument byte after FD.
   codes: Mapping[str, int]
   ## Placeholder name -> most cells it prints.
   widths: Mapping[str, int]

   def encode(self, text: Any, where: str) -> list[int]:
      """!Encodes a field text.
      @param text   the text
      @param where  location for error messages
      @return the codes, ending with CODE_END
      """


def name_cells(codes: Sequence[int], field: FieldText) -> int:
   """!Most cells an encoded field text prints: glyphs, plus each placeholder at its widest.
   @param codes  the codes, ending with CODE_END
   @param field  the game's field placeholders
   @return cells
   """
   widths = {field.codes[n]: w for n, w in field.widths.items()}
   cells = 0
   i = 0
   while codes[i] != text_encoder.CODE_END:
      c = codes[i]
      if c in (text_encoder.CODE_CONTROL, text_encoder.CODE_PLACEHOLDER):
         cells += widths.get(codes[i + 1], 0) if c == text_encoder.CODE_PLACEHOLDER else 0
         i += ESCAPE_CODES
         continue
      if c <= text_encoder.MAX_GLYPH_CODE:
         cells += 1
      i += 1
   return cells


## Transition names in capsule::script::Transition order.
TRANSITIONS = evc.TRANSITIONS
## Keys per object kind.
KEYS = {
   "style": {"frame", "ink", "shadow", "fill", "border", "frames", "slide"},
   "location": {"name", "banner", "preview"},
   "preview": {"show", "sequences"},
   "variant": {"when", "sequence"},
}
## Longest banner text (the screen is 30 cells; the frame takes 2).
MAX_NAME = 28
## Locations per world (Map::location is 8-bit; 0xFF = none).
MAX_LOCATIONS = 255
## Banner styles per world.
MAX_STYLES = 254
## Default style colors (ink, shadow, fill, border) and timing.
DEFAULTS = {"ink": "#f8f8f8", "shadow": "#303030", "fill": "#284878", "border": "#f8f8f8", "frames": 120,
            "slide": 8}

## Bit position of green in a BGR555 color.
GREEN_SHIFT = seqc.GREEN_SHIFT
## Bit position of blue in a BGR555 color.
BLUE_SHIFT = seqc.BLUE_SHIFT
## Pixels per side of a tile.
TILE_SIZE = TILE_PIXELS
## Tiles per side of a banner frame (corners, edges, interior).
FRAME_TILES = 3
## Pixels per side of a banner frame image.
FRAME_SIZE = FRAME_TILES * TILE_SIZE
## Bytes per RGBA pixel.
RGBA_BYTES = 4
## Longest a banner stays on screen, in frames (a u16).
MAX_BANNER_FRAMES = 65535
## Largest banner slide speed.
MAX_BANNER_SLIDE = 64
## Location::banner of a location without a banner.
NO_BANNER = 0xFF
## Preview sequences per location.
MAX_PREVIEWS = 8
## Build limits a preview sequence is compiled with (the sequence compiler's defaults).
PREVIEW_MAX_TRACKS = 8
## Sprite slots a preview sequence may use.
PREVIEW_MAX_SPRITES = 16
## Affine slots a preview sequence may use.
PREVIEW_MAX_AFFINE = 4
## Words per line of an emitted tile array.
WORDS_PER_LINE = 8
## Bits per byte of the visited-locations bit set.
BITS_PER_BYTE = 8

## A parsed JSON object.
JsonDict = dict[str, Any]
## Tile packer: 8 rows of 8 indices -> 8 words.
TilePacker = Callable[[Sequence[Sequence[int]]], list[int]]


class LocationError(Exception):
   """!A location rule was broken; the message says where."""
   pass


def _keys(obj: Any, kind: str, where: str) -> None:
   """!Rejects unknown keys.
   @param obj    the object
   @param kind   key set in KEYS
   @param where  location for messages
   @return None
   """
   if not isinstance(obj, dict):
      raise LocationError(f"{where}: expected an object")
   unknown = sorted(set(obj) - KEYS[kind])
   if unknown:
      raise LocationError(f"{where}: unknown key(s) {', '.join(unknown)}")


def _color(value: Any, where: str) -> int:
   """!A "#rrggbb" color as BGR555.
   @param value  the text
   @param where  location for messages
   @return 15-bit color
   """
   try:
      r, g, b = bitmap.parse_hex(value)
   except (bitmap.BitmapError, TypeError, ValueError, AttributeError):
      raise LocationError(f"{where}: expected a #rrggbb color, got {value!r}") from None
   return r | (g << GREEN_SHIFT) | (b << BLUE_SHIFT)


def _frame(path: Path, colors: dict[str, int], where: str) -> tuple[list[list[list[int]]], list[int]]:
   """!A 24x24 banner frame image as 9 tiles and its palette.
   @param path    the PNG
   @param colors  {"ink", "shadow", "fill", "border"} BGR555
   @param where   location for messages
   @return (9 tiles of 8x8 indices, 15 palette colors for entries 1-15)
   """
   try:
      w, h, rgba = bitmap.read_png(path)
   except (OSError, bitmap.BitmapError) as e:
      raise LocationError(f"{where}: cannot read frame {path}: {e}") from None
   if (w, h) != (FRAME_SIZE, FRAME_SIZE):
      raise LocationError(f"{where}: a banner frame is 24x24 (3x3 tiles), not {w}x{h}")
   pal = [colors["ink"], colors["shadow"], colors["fill"]]
   index = {c: i + 1 for i, c in enumerate(pal)}
   pix = []
   for y in range(FRAME_SIZE):
      row = []
      for x in range(FRAME_SIZE):
         at = (y * FRAME_SIZE + x) * RGBA_BYTES
         r, g, b, a = rgba[at:at + RGBA_BYTES]
         if a < bitmap.ALPHA_OPAQUE:
            row.append(0)
            continue
         r5, g5, b5 = bitmap.to_rgb555(r, g, b)
         c = r5 | (g5 << GREEN_SHIFT) | (b5 << BLUE_SHIFT)

         # a new color takes the next palette entry (entry 0 is transparent)
         if c not in index:
            if len(pal) >= bitmap.MAX_COLORS:
               raise LocationError(f"{where}: frame {path} has more than 15 colors (ink, shadow and fill "
                                   "included)")
            pal.append(c)
            index[c] = len(pal)
         row.append(index[c])
      pix.append(row)

   # cut the image into its 3x3 tiles, row by row
   tiles = [[[pix[ty * TILE_SIZE + y][tx * TILE_SIZE + x] for x in range(TILE_SIZE)] for y in range(TILE_SIZE)]
            for ty in range(FRAME_TILES) for tx in range(FRAME_TILES)]
   return tiles, pal + [0] * (bitmap.MAX_COLORS - len(pal))


def compile_locations(w: JsonDict, base: Path, maps_raw: dict[str, JsonDict], deps: list[Path],
                      conditions: Sequence[str], *,
                      codepage: text_encoder.CodePage = text_encoder.DEFAULT,
                      field: FieldText | None = None) -> JsonDict:
   """!The world's locations, banner styles, previews and default transition.
   @param w           world manifest
   @param base        its folder
   @param maps_raw    {map name: manifest}
   @param deps        list files read are appended to
   @param conditions  encounter condition names (bit order)
   @param codepage    the game's code page (location names; the previews' text layers)
   @param field       the game's field placeholders (names are then field text), or None
   @return dict: styles, locations, previews, map_location {map: index}, default_transition
   """
   trans = w.get("transition", "fade")
   if trans not in TRANSITIONS:
      raise LocationError(f"world transition: one of {', '.join(TRANSITIONS)}")
   raw_styles = w.get("banner_styles", {})
   if not isinstance(raw_styles, dict) or len(raw_styles) > MAX_STYLES:
      raise LocationError(f"world banner_styles: an object of up to {MAX_STYLES} styles")
   styles, style_index = [], {}
   for sname, st in raw_styles.items():
      sw = f"banner style {sname!r}"
      _keys(st, "style", sw)
      colors = {k: _color(st.get(k, DEFAULTS[k]), f"{sw} {k}") for k in ("ink", "shadow", "fill", "border")}
      frames = evc._int(st.get("frames", DEFAULTS["frames"]), 1, MAX_BANNER_FRAMES, f"{sw} frames")
      slide = evc._int(st.get("slide", DEFAULTS["slide"]), 1, MAX_BANNER_SLIDE, f"{sw} slide")

      # a plain box: its four colors, the rest of the palette unused
      plain = [colors["ink"], colors["shadow"], colors["fill"], colors["border"]]
      tiles, pal = None, plain + [0] * (bitmap.MAX_COLORS - len(plain))
      if "frame" in st:
         path = (base / str(st["frame"])).resolve()
         deps.append(path)
         tiles, pal = _frame(path, colors, sw)
      style_index[sname] = len(styles)
      styles.append({"name": sname, "tiles": tiles, "palette": pal, "frames": frames, "slide": slide})
   raw_locs = w.get("locations", {})
   if not isinstance(raw_locs, dict) or len(raw_locs) > MAX_LOCATIONS:
      raise LocationError(f"world locations: an object of up to {MAX_LOCATIONS} locations")
   locations, loc_index, previews = [], {}, []
   for lname, loc in raw_locs.items():
      lw = f"location {lname!r}"
      _keys(loc, "location", lw)
      name = loc.get("name")
      if not isinstance(name, str) or not name.strip():
         raise LocationError(f"{lw}: needs a name")
      try:
         codes = encode_text(name, lw, codepage) if field is None else field.encode(name, lw)
      except (AssetError, text_encoder.TextError) as e:
         raise LocationError(str(e)) from None
      # the banner fits MAX_NAME glyphs (the codes without the end code; placeholders at their widest)
      cells = len(codes) - 1 if field is None else name_cells(codes, field)
      if cells > MAX_NAME:
         raise LocationError(f"{lw}: name {name!r} is {cells} characters; a banner fits {MAX_NAME}")
      banner = NO_BANNER
      if "banner" in loc:
         if loc["banner"] not in style_index:
            raise LocationError(f"{lw}: unknown banner style {loc['banner']!r}")
         banner = style_index[loc["banner"]]
      rule, first, count = 0, len(previews), 0
      if "preview" in loc:
         pv = loc["preview"]
         pw = f"{lw} preview"
         _keys(pv, "preview", pw)
         show = pv.get("show", "always")
         if show not in ("always", "first"):
            raise LocationError(f"{pw}: show is always or first")
         rule = 0 if show == "always" else 1
         seqs = pv.get("sequences")
         if not isinstance(seqs, list) or not 1 <= len(seqs) <= MAX_PREVIEWS:
            raise LocationError(f"{pw}: sequences must list 1-8 entries")
         for j, v in enumerate(seqs):
            vw = f"{pw} {j + 1}"
            _keys(v, "variant", vw)
            when = v.get("when", [])
            if isinstance(when, str):
               when = [when]
            mask = 0
            for c in when:
               if c not in conditions:
                  raise LocationError(f"{vw}: unknown condition {c!r} (world encounter_conditions)")
               # one bit per encounter condition, in the world's order
               mask |= 1 << conditions.index(c)
            path = (base / str(v.get("sequence", ""))).resolve()
            deps.append(path)
            try:
               comp = seqc.Compiler(json.loads(path.read_text(encoding="utf-8")), PREVIEW_MAX_TRACKS,
                                    PREVIEW_MAX_SPRITES, PREVIEW_MAX_AFFINE, codepage=codepage).run()
               text = comp.emit_inc(f"kPreview{len(previews)}")
               # Embedded in world.cpp: the include guard of a stand-alone .inc does not belong.
               text = "\n".join(ln for ln in text.splitlines() if ln.strip() != "#pragma once")
            except OSError as e:
               raise LocationError(f"{vw}: cannot read {path}: {e}") from None
            except (seqc.ManifestError, KeyError, TypeError, ValueError, AttributeError) as e:
               raise LocationError(f"{vw}: {path.name} does not compile: {e}") from None
            previews.append({"mask": mask, "inc": text, "symbol": f"kPreview{len(previews)}"})
            count += 1
      loc_index[lname] = len(locations)
      locations.append({"name": lname, "codes": codes, "banner": banner, "rule": rule, "first": first,
                        "count": count})
   map_location = {}
   for mname, m in maps_raw.items():
      if "location" in m:
         if m["location"] not in loc_index:
            raise LocationError(f"map {mname!r}: unknown location {m['location']!r}")
         map_location[mname] = loc_index[m["location"]]
   return {"styles": styles, "locations": locations, "previews": previews, "map_location": map_location,
           "default_transition": TRANSITIONS.index(trans)}


def emit(locs: JsonDict, words_4bpp: TilePacker) -> list[str]:
   """!The world.cpp part that defines overworld::kLocations.
   @param locs        compile_locations() result
   @param words_4bpp  tile packer (asset_compiler.words_4bpp)
   @return lines
   """
   L = ["", '#include "capsule/overworld/locations.h"', ""]
   for p in locs["previews"]:
      L.append(p["inc"])
   L += ["namespace capsule::overworld {", "namespace {", "", "using hal::video::Color;", ""]
   for i, st in enumerate(locs["styles"]):
      if st["tiles"] is not None:
         words = [v for t in st["tiles"] for v in words_4bpp(t)]
         L.append(f"alignas(4) const uint32_t kBannerFrame{i}[] = {{")
         for k in range(0, len(words), WORDS_PER_LINE):
            L.append("    " + ", ".join(f"0x{v:08X}" for v in words[k:k + WORDS_PER_LINE]) + ",")
         L.append("};")
      L.append(f"const Color kBannerColors{i}[] = {{{', '.join(f'0x{c:04X}' for c in st['palette'])}}};")
   if locs["styles"]:
      L.append("const BannerStyle kBannerStyles[] = {")
      for i, st in enumerate(locs["styles"]):
         frame = f"kBannerFrame{i}" if st["tiles"] is not None else "nullptr"
         L.append(f"    {{{frame}, kBannerColors{i}, {st['frames']}, {st['slide']}, 0}},   // {st['name']}")
      L.append("};")
   for i, loc in enumerate(locs["locations"]):
      L.append(f"const text::Code kLocationName{i}[] = {{{', '.join(str(c) for c in loc['codes'])}}};")
   if locs["previews"]:
      L.append("const Preview kPreviews[] = {")
      L.extend(f"    {{&::{p['symbol']}, 0x{p['mask']:08X}u}}," for p in locs["previews"])
      L.append("};")
   if locs["locations"]:
      L.append("const Location kLocationTable[] = {")
      for i, loc in enumerate(locs["locations"]):
         L.append(f"    {{kLocationName{i}, {loc['first']}, {loc['count']}, {loc['banner']}, "
                  f"PreviewRule::{'FirstVisit' if loc['rule'] else 'Always'}, {{0, 0, 0}}}},   // {loc['name']}")
      L.append("};")
   n = len(locs["locations"])

   # one bit per location, rounded up to whole bytes
   visited = (n + BITS_PER_BYTE - 1) // BITS_PER_BYTE
   if visited:
      L.append(f"uint8_t g_visited[{visited}];   // locations visited (permanent, D-17)")
   L += ["", "} // namespace", "",
         f"const Locations kLocations = {{{'kLocationTable' if n else 'nullptr'}, "
         f"{'kBannerStyles' if locs['styles'] else 'nullptr'}, {'kPreviews' if locs['previews'] else 'nullptr'}, "
         f"{'g_visited' if visited else 'nullptr'}, {n}, {visited}, {len(locs['styles'])}, "
         f"{locs['default_transition']}, {{0, 0}}}};",
         "", "} // namespace capsule::overworld", ""]
   return L
