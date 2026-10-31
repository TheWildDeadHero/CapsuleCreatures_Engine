#!/usr/bin/env python3
"""!Compile an asset manifest (capsule.assets/1) into the C++ asset table.

    asset_compiler.py <manifest.json> <out.cpp> [--depfile <out.d>] [--code-page <page.json>]
                      [--scripts <scripts.json>] [--fit-report <fit.json>] [--language <code>]

Assets are drawn from simple shapes (rectangles, circles, rings, ellipses,
polygons, gradients, stripes, checkers, dots); sprite frames can also be PNG
images (see tools/bitmap.py). Follows the manifest rule (P-1): editor data
stripped, unknown keys rejected, names resolved to numbers. With --depfile,
the image files read are listed in a make-style dependency file so the build
reruns the compiler when one changes.

Kinds:
  - background: a 256x256 image for a regular layer (4bpp, tiles deduplicated
    with flips), or an affine image of 128 or 256 pixels square (8bpp), or
    both forms ("affine": "both") so one asset works on either kind of layer.
  - sprite: equally sized frames (a GBA OBJ size) plus animations. A frame is
    either {"shapes": [...]} or {"image": "path.png"}, the path relative to
    the manifest and the image exactly the sprite's size. An animation plays
    its frames for "period" display frames each, or for per-frame
    "durations" (one 1-255 value per entry of "frames"). I3: a frame may
    also take frame n of a sheet ({"image": "sheet.png", "frame": n}: frames
    of the sprite's size in reading order) and use the image's palette
    indices as they are ("indexed": true, a palette PNG); an animation's
    "flips" give each entry's mirroring (0 none, 1 horizontal, 2 vertical,
    3 both); "stream": true keeps only the shown frame in VRAM (frames x
    tiles may exceed a slot, one frame may not); "steps": [first stand,
    second stand, after second, after first] is the walk animations' step
    table (asset::Sprite::steps).
    DS1 (D-30 section 3): "layout": "direction_grid" gives the frames as a
    grid instead: "grid" (an indexed sheet {"image", "indexed": true} of
    frames of the sprite's size, "columns" across, one row per row name; or
    rows of frame objects), "columns" 3 (down, up, left: right is left
    mirrored horizontally) or 4 (down, up, left, right), "rows" (the rows'
    names); each animation {"rows": [row names], "period" | "durations",
    "loop"} becomes the four directions' animations (down, up, left, right),
    or one with "direction". Identical frames are stored once.
  - door (DS1, D-30 section 1): a door animation (asset::Door, played by
    capsule/field/doors.h): "cells": [width, height] metatiles (the anchor a
    map binds is the bottom-left cell); "show": "tiles" (the frames' tiles
    load into the map's tile area at "load_at", "end" by default, and
    "entries" draw the cells: per cell, top row first, per layer four map
    entries, a frame tile as {"tile": k, "palette": bank, "flip"} or a raw
    entry) with "sheet" (an indexed PNG or shapes) and "frame_tiles", or
    "override" ("frames": per frame the cells' raw entries); "layers" (the
    map layers it draws, default 1, the others keep the cell's own);
    "open": [[frame or "closed", ticks], ...] (a frame shows for its ticks
    display frames), "close": the same or "reverse" (default); "sounds":
    {"open", "close"} sound keys; "exit": the direction a walker arriving on
    it walks out (default down).
  - text: a string in the game's code page (tools/text_encoder.py; --code-page,
    default the engine's ASCII page) and newlines; braces are plain characters,
    unless --scripts names the game's scripts manifest and it declares field
    placeholders (D-22 decision 6): then a text is field text like a script
    message ({player}, terms, {prompt}, {page}; script_compiler.load_placeholders()).
  - list: an ordered list of asset keys (e.g. what a showcase loops over).
  - font (I4, D-29): a proportional font (C-1), {"key": "font:NAME", "kind": "font", "font": "x.font.json"}
    (capsule.font/2, tools/font_compiler.py compile_proportional()), for asset::font() and the window printer
    (capsule/text/window.h).
  - tiles / tilemap / palette (UC, D-33): a full-screen scene's graphics as it loads them (asset::Tiles,
    asset::Tilemap, asset::Palette); see tools/screen_assets.py for their keys.
  - text_box (C10): a text box style (asset::TextBox, asset::text_box()): a game's message or menu box as data:
    {"key": "text_box:NAME", "kind": "text_box", "font": "font:NAME", "fonts": {"7": "font:OTHER"},
     "window": {...}, "frame": {...}}. "font" is the window's font, "fonts" the fonts its Font control n selects
    (n as a string). "window" holds text::window::Config's settings: bg, char_block (left out: the layer's own,
    asset::kLayerCharBlock), first_tile, palette_bank, x, y, width, height (cells), left, top (pixels), lines,
    ink, shadow, background (palette entries), frames_per_char, scroll_speed, arrow (a code; left out: none),
    typing ("interval" | "counter"), char_delay, speedup ("rest" | "per_frame" | "none"), lead_frames; "frame"
    holds text::window::Frame's: first_tile, palette_bank, pad_left, pad_top, pad_right, pad_bottom. A setting
    left out takes the struct's default.
I4: "includes" lists other capsule.assets/1 manifests whose palettes and assets join this one (their paths
relative to them); "text_tables" lists text tables (capsule.texts/1, D-8 C-3), each text a text asset:

    {"schema": "capsule.texts/1", "language": "en",
     "box": {"font": "normal", "width": 216, "lines": 2}, "fonts": {"normal": "fonts/normal.font.json"},
     "texts": {"text:Map.Label": {"text": "...", "wrap": "original"}}}

C10: a manifest may name tables of several languages (each table's "language"; the same keys in each): the build
takes one, --language, else the manifest's "language", else the tables' only language ("fallback_language": a key
the build's language lacks takes that language's text); a table without "language" is in every build.

A text with "wrap": "original" keeps its authored breaks (newlines, {scroll}, {page}); the compiler checks each
line fits the box's width in the box's font (or the text's "font": a name of "fonts"), with placeholders measured
by their terms (other placeholders count nothing: such lines are listed as "with placeholders"), and reports what
does not fit (--fit-report <out.json>; one summary line otherwise). "wrap": "auto" (the default) breaks lines at
spaces to the width and pages after "lines" lines. Other keys of a text ("braille_format", ...) are data for the
game's printer.

Colors are palette indices 1-15 into the asset's own palette (hex color
strings such as "#336699"); 0 is transparent. Image pixels are mapped to the
nearest palette color; a sprite whose frames are all images may use
"palette": "auto" to get a palette chosen from the images (up to 15 colors,
see bitmap.make_palette()). The generated file static_asserts tile counts
against the loaders' VRAM plan (gfx::background, gfx::sprites).
"""
from __future__ import annotations

import json
import sys
from collections.abc import Iterable, Mapping, Sequence
from os import PathLike
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bitmap  # noqa: E402
import manifest_paths  # noqa: E402
import screen_assets  # noqa: E402
import text_encoder  # noqa: E402
from sequence_compiler import BLUE_SHIFT, GREEN_SHIFT, U32_MASK  # noqa: E402
from sequence_compiler import ManifestError as SequenceManifestError  # noqa: E402
from sequence_compiler import asset_key  # noqa: E402

## Allowed top-level keys (I4: includes, text_tables; C10: language, fallback_language).
TOP_KEYS = {"schema", "id", "palettes", "assets", "includes", "text_tables", "language", "fallback_language"}
## Schema of a text table (I4).
TEXT_TABLE_SCHEMA = "capsule.texts/1"
## Keys of a text table.
TEXT_TABLE_KEYS = {"schema", "language", "box", "fonts", "texts", "editor"}
## Keys of a text table's box.
BOX_KEYS = {"font", "width", "lines"}
## Wrap modes of a table text: keep the authored breaks, or break at spaces.
WRAP_MODES = ("original", "auto")
## Lines of a box without "lines".
DEFAULT_BOX_LINES = 2
## Most lines one fit report lists.
MAX_FIT_LISTED = 500
## Allowed keys per asset kind.
KIND_KEYS = {
   "background": {"key", "kind", "palette", "affine", "size", "fill", "priority", "shapes"},
   "sprite": {"key", "kind", "palette", "width", "height", "frames", "animations", "stream", "steps", "layout",
              "grid", "rows", "columns"},
   "text": {"key", "kind", "text"},
   "list": {"key", "kind", "items"},
   "font": {"key", "kind", "font"},
   "door": {"key", "kind", "cells", "show", "layers", "sheet", "frame_tiles", "load_at", "entries", "frames",
            "open", "close", "sounds", "exit"},
   "text_box": {"key", "kind", "font", "fonts", "window", "frame", "menu", "palette"},
   **screen_assets.KIND_KEYS,   # UC (D-33): tiles, tilemap, palette (tools/screen_assets.py)
}
## DS1: keys of a door's sheet (an indexed image, or shapes drawn with palette indices).
DOOR_SHEET_KEYS = {"image", "indexed", "size", "shapes"}
## DS1: keys of a door's tile entry (a frame tile drawn in a palette bank).
DOOR_ENTRY_KEYS = {"tile", "palette", "flip"}
## DS1: keys of a door's sounds.
DOOR_SOUND_KEYS = {"open", "close"}
## DS1: how a door's frames show, in asset::DoorShow order.
DOOR_SHOWS = ("tiles", "override")
## DS1: DoorStep::frame of the closed door (asset::kDoorClosed), and its manifest word.
DOOR_CLOSED, DOOR_CLOSED_WORD = 0xFF, "closed"
## DS1: a door's close steps given as its open steps reversed.
DOOR_REVERSE = "reverse"
## DS1: the default first map tile of a tiles door ("end": the last tiles of the map's tile area).
DOOR_LOAD_END = "end"
## DS1: map tiles a map's tile area holds (capsule::map::kMaxTiles).
MAP_TILES = 1024
## DS1: most cells across or down of a door.
MAX_DOOR_CELLS = 4
## DS1: tile layers of a map (capsule::map::kMaxLayers).
MAX_MAP_LAYERS = 4
## DS1: map entries per metatile layer of a cell (2 x 2).
CELL_ENTRIES = 4
## DS1: pixels per metatile side.
METATILE_PIXELS = 16
## DS1: largest map entry, and a map entry's tile, flip and palette fields.
MAX_MAP_ENTRY = 0xFFFF
## DS1: map entry bits of the tile index.
ENTRY_TILE_MASK = 0x3FF
## DS1: map entry shift of the palette bank.
ENTRY_PALETTE_SHIFT = 12
## DS1: most palette banks a map entry names.
MAX_ENTRY_PALETTE = 15
## DS1: directions in capsule::field::Direction order (a door's exit; a direction grid's columns).
DIRECTIONS = ("down", "up", "left", "right")
## DS1: a direction grid's columns without a right column (right mirrors left).
GRID_MIRRORED_COLUMNS = 3
## DS1: the sprite layout whose frames are a grid of directions (columns) and named rows.
DIRECTION_GRID = "direction_grid"
## DS1: the direction-grid column a missing right column mirrors (left).
MIRRORED_FROM = DIRECTIONS.index("left")
## DS1: keys of a direction-grid animation.
GRID_ANIM_KEYS = {"rows", "direction", "period", "durations", "loop"}
## Allowed keys per shape, besides "shape".
SHAPE_KEYS = {
   "rect": {"x", "y", "w", "h", "color"},
   "frame": {"x", "y", "w", "h", "color", "width"},
   "circle": {"cx", "cy", "r", "color"},
   "ring": {"cx", "cy", "r", "width", "color"},
   "ellipse": {"cx", "cy", "rx", "ry", "color"},
   "polygon": {"points", "color"},
   "vgradient": {"x", "y", "w", "h", "colors"},
   "hgradient": {"x", "y", "w", "h", "colors"},
   "stripes": {"x", "y", "w", "h", "colors", "period", "dir"},
   "checker": {"x", "y", "w", "h", "colors", "cell"},
   "dots": {"x", "y", "w", "h", "color", "count", "seed", "size"},
}
## OBJ (width, height) -> (shape, size), as hal::video::ObjShape and size.
OBJ_SIZES = {
   (8, 8): (0, 0), (16, 16): (0, 1), (32, 32): (0, 2), (64, 64): (0, 3),
   (16, 8): (1, 0), (32, 8): (1, 1), (32, 16): (1, 2), (64, 32): (1, 3),
   (8, 16): (2, 0), (8, 32): (2, 1), (16, 32): (2, 2), (32, 64): (2, 3),
}
## hal::video::ObjShape enumerator names by value.
OBJ_SHAPE_NAMES = ["OBJ_SQUARE", "OBJ_WIDE", "OBJ_TALL"]

## Highest Animation::flips value (horizontal | vertical).
MAX_FLIP = 3
## Entries of a sprite's step table (asset::kStepEntries).
STEP_ENTRIES = 4
## Entries of a 4bpp palette (entry 0 is transparent).
PALETTE_ENTRIES = 16
## Mask of one 5-bit channel of a BGR555 color.
CHANNEL_MASK = 31

## Pixels per side of a tile.
TILE_SIZE = 8
## Bits per pixel of a 4bpp tile.
BITS_4BPP = 4
## Bits per pixel of an 8bpp tile.
BITS_8BPP = 8
## 32-bit words of a 4bpp tile (one per row).
WORDS_4BPP = 8
## 32-bit words of an 8bpp tile.
WORDS_8BPP = 16
## Pixels in one 32-bit word of an 8bpp tile.
PIXELS_PER_WORD_8BPP = 4
## Map cell bit: tile flipped horizontally.
H_FLIP = 0x400
## Map cell bit: tile flipped vertically.
V_FLIP = 0x800
## DS1: map entry flip bits by flip word.
ENTRY_FLIPS = {"": 0, "h": H_FLIP, "v": V_FLIP, "hv": H_FLIP | V_FLIP}

## Pixels per side of a regular background.
REGULAR_BG_SIZE = 256
## Pixels per side an affine background may have.
AFFINE_BG_SIZES = (128, 256)
## Pixels per side of an affine background without a "size".
DEFAULT_AFFINE_BG_SIZE = 128
## Most unique tiles of an affine background (its map entries are 8-bit).
MAX_AFFINE_TILES = 256
## Back-most BG priority (0 is the front).
BACK_PRIORITY = 3
## Forms of an asset with "affine": "both" (regular, then affine).
BOTH_FORMS = 2

## Shapes use doubled coordinates, so pixel centres (x + 0.5) are integers.
SUBPIXEL_SCALE = 2
## The doubled coordinates' scale, squared (for squared distances).
SUBPIXEL_SCALE_SQUARED = SUBPIXEL_SCALE * SUBPIXEL_SCALE
## Pixels scanned beyond a curved shape's radius on each side.
BOUNDS_MARGIN = 1
## Fewest colors of a gradient, stripes or checker.
MIN_PATTERN_COLORS = 2
## Fewest points of a polygon.
MIN_POLYGON_POINTS = 3
## Stripe width when the shape gives none, pixels.
DEFAULT_STRIPE_PERIOD = 8
## Checker cell size when the shape gives none, pixels.
DEFAULT_CHECKER_CELL = 8
## Dots drawn when the shape gives no count.
DEFAULT_DOT_COUNT = 32
## Multiplier of the dots' linear congruential generator (A-4: the same dots every build; a neutral full-period
## 32-bit LCG step, the state step of the engine's default generator, E11).
LCG_MULTIPLIER = 747796405
## Increment of that generator.
LCG_INCREMENT = 2891336453
## Shift taking the generator's high bits (its low bits repeat quickly).
LCG_OUTPUT_SHIFT = 16

## Most frames of one animation (an 8-bit count).
MAX_ANIM_FRAMES = 255
## Longest display time of one animation frame, in display frames (8-bit).
MAX_FRAME_DURATION = 255
## Display frames per animation frame when an animation gives no period.
DEFAULT_PERIOD = 8

## Code page: newline.
CODE_NEWLINE = text_encoder.CODE_NEWLINE
## Code page: end of text.
CODE_END = text_encoder.CODE_END
## Most keys of a list asset (16-bit count).
MAX_LIST_ITEMS = 0xFFFF

## asset::Kind::Background (index into the generated kinds table).
KIND_BACKGROUND = 0
## asset::Kind::Sprite.
KIND_SPRITE = 1
## asset::Kind::Text.
KIND_TEXT = 2
## asset::Kind::List.
KIND_LIST = 3
## asset::Kind::Font (I4).
KIND_FONT = 4
## asset::Kind::Door (DS1).
KIND_DOOR = 5
## asset::Kind::TextBox (C10).
KIND_TEXT_BOX = 6
## C10: a text box style's window settings (text::window::Config members) -> (lowest, highest) values; typing and
## speedup are words (TEXT_BOX_TYPING, TEXT_BOX_SPEEDUP), arrow a code (or left out: no arrow).
TEXT_BOX_WINDOW = {"bg": (0, 3), "char_block": (0, 3), "first_tile": (0, 1023), "palette_bank": (0, 15),
                   "x": (0, 63), "y": (0, 63), "width": (1, 32), "height": (1, 32), "left": (0, 255),
                   "top": (0, 255), "lines": (1, 255), "ink": (0, 15), "shadow": (0, 15), "background": (0, 15),
                   "frames_per_char": (0, 255), "scroll_speed": (0, 255), "arrow": (0, 254),
                   "char_delay": (0, 255), "lead_frames": (0, 255)}
## C10: the window settings given as words.
TEXT_BOX_WORDS = {"typing", "speedup"}
## C10: text::window::Config::typing values (kTypingInterval, kTypingCounter).
TEXT_BOX_TYPING = ("interval", "counter")
## C10: text::window::Config::speedup values (kSpeedupRest, kSpeedupPerFrame, kSpeedupNone).
TEXT_BOX_SPEEDUP = ("rest", "per_frame", "none")
## C10: a text box style's frame settings (text::window::Frame members) -> (lowest, highest).
TEXT_BOX_FRAME = {"first_tile": (0, 1023), "palette_bank": (0, 15), "pad_left": (0, 31), "pad_top": (0, 31),
                  "pad_right": (0, 31), "pad_bottom": (0, 31)}
## C10: the window's char_block when the style leaves it out (asset::kLayerCharBlock: the layer's own block).
LAYER_CHAR_BLOCK = 0xFF
## C10: most fonts a text box's Font control selects (text::Control::Font's argument is one byte).
MAX_TEXT_BOX_FONTS = 255
## N3: a text box window's arrow image (text::window::Config arrow_kind kArrowImage): its settings besides the image.
TEXT_BOX_ARROW_KEYS = {"image", "height", "steps", "delay"}
## N3: text::window::kArrowImageWidth: pixels across an arrow image (one 4bpp word per row).
ARROW_IMAGE_WIDTH = 8
## N3: a frame image's size: three by three tiles, the nine parts in reading order (text::window::Frame).
FRAME_IMAGE_PIXELS = 24
## N3: a frame's keys besides the numbers: its art and its bank's colors.
TEXT_BOX_FRAME_ART = {"image", "palette"}
## N3: bits of one 4bpp pixel in an arrow image's row word (pixel x at bits 4x).
NIBBLE_BITS = 4
## N3: text::window::kArrowImage (Config::arrow_kind).
ARROW_KIND_IMAGE = 1
## N3: most rows of an arrow image (Config::arrow_image_rows is one byte).
MAX_ARROW_ROWS = 255
## N3: a text box style's menu (ui::MenuStyle members) -> (lowest, highest) values.
TEXT_BOX_MENU = {"option_x": (0, 255), "cursor_x": (0, 255), "top": (0, 255), "row_height": (0, 255),
                 "cursor_width": (0, 255), "cursor_height": (0, 255), "cursor": (0, 254), "wrap_above": (0, 255),
                 "input_delay": (0, 255), "sound_move": (0, 65535), "sound_choose": (0, 65535),
                 "sound_cancel": (0, 65535), "sound_open": (0, 65535), "arrow_up": (0, 255), "arrow_down": (0, 255),
                 "arrow_amplitude": (0, 255), "arrow_speed": (0, 255)}
## N3: the menu settings given as words (or lists, or asset keys) and their C++ values.
TEXT_BOX_MENU_WORDS = {"wrap": ("kWrapNever", "kWrapAlways", "kWrapAbove"),
                       "move_keys": ("kMovePressed", "kMoveRepeated"),
                       "move_sound": ("kSoundWhenMoved", "kSoundAlways"),
                       "order": ("kOneAction", "kEveryKey")}
## N3: the words of TEXT_BOX_MENU_WORDS in the manifest (same order).
TEXT_BOX_MENU_NAMES = {"wrap": ("never", "always", "above"), "move_keys": ("pressed", "repeated"),
                       "move_sound": ("when_moved", "always"), "order": ("one_action", "every_key")}
## N3: the pad's keys by name (hal::input::Key) for a menu's cancel_keys.
MENU_KEY_NAMES = ("A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y")
## N3: ui::MenuStyle's members in declaration order (designated initializers keep it).
MENU_ORDER = ("option_x", "cursor_x", "top", "row_height", "cursor_width", "cursor_height", "cursor", "wrap",
              "wrap_above", "input_delay", "move_keys", "move_sound", "order", "cancel_keys", "sound_move",
              "sound_choose", "sound_cancel", "sound_open", "arrow_sprite", "arrow_up", "arrow_down",
              "arrow_amplitude", "arrow_speed")

## C10: the window's text::window::Config defaults the generated initializer repeats for left-out settings.
TEXT_BOX_DEFAULTS = {"bg": 0, "first_tile": 0, "palette_bank": 0, "x": 0, "y": 0, "width": 0, "height": 0,
                     "left": 0, "top": 0, "lines": 1, "ink": 1, "shadow": 2, "background": 0,
                     "frames_per_char": 1, "scroll_speed": 1, "char_delay": 0, "lead_frames": 0}

## Generated array values per line for 32-bit words.
WIDE_VALUES_PER_LINE = 8
## Generated array values per line for 8- and 16-bit values.
NARROW_VALUES_PER_LINE = 16
## Bytes of a 32-bit tile word (for the ROM estimate).
WORD_BYTES = 4
## Bytes of a 16-color palette (for the ROM estimate).
PALETTE_BYTES = 32

## Positional command-line arguments (manifest and output).
POSITIONAL_ARGUMENTS = 2
## Arguments of an option with a value (the option and the value).
OPTION_WITH_VALUE = 2
## Exit status for a bad command line.
EXIT_USAGE = 2
## Leading fields of an image tuple that give its size (width, height).
IMAGE_SIZE_FIELDS = 2

## A manifest object (parsed JSON).
JsonDict = dict[str, Any]
## An 8x8 tile: rows of color indices.
Tile = tuple[tuple[int, ...], ...]
## An asset table entry: (key, kind, index, name).
Entry = tuple[int, int, int, str]


## C10: the JSON value types strip() walks into (json.loads() makes plain dicts and lists).
CONTAINERS = (dict, list)


class ManifestError(Exception):
   """!A manifest rule was broken; the message says where and how."""
   pass


def strip(node: Any) -> Any:
   """!Drops editor-only data everywhere (P-1 rule 2).
   @param node  any JSON value
   @return a copy without "editor" and "_"-prefixed keys
   """
   if isinstance(node, dict):
      return {k: (strip(v) if isinstance(v, CONTAINERS) else v) for k, v in node.items()
              if k != "editor" and not k.startswith("_")}
   if isinstance(node, list):
      # C10 (S1): scalars are kept as they are without a call each (maps hold long lists of numbers)
      return [strip(v) if isinstance(v, CONTAINERS) else v for v in node]
   return node


def check_keys(obj: Iterable[str], allowed: Iterable[str], where: str) -> None:
   """!Rejects unknown keys.
   @param obj      dict
   @param allowed  allowed keys
   @param where    location for the error message
   @throws ManifestError on an unknown key
   @return None
   """
   unknown = sorted(set(obj) - set(allowed))
   if unknown:
      raise ManifestError(f"{where}: unknown key(s) {', '.join(unknown)}")


def bgr555(value: object, where: str) -> int:
   """!Hex color string to BGR555.
   @param value  a hash sign followed by six hex digits
   @param where  location for the error message
   @return the color
   """
   if not isinstance(value, str) or len(value) != bitmap.HEX_COLOR_LENGTH or value[0] != "#":
      raise ManifestError(f"{where}: colors are strings like '#336699', got {value!r}")
   try:
      r, g, b = (int(value[i:i + bitmap.HEX_DIGITS_PER_CHANNEL], bitmap.HEX_BASE) >> bitmap.CHANNEL_DROP_BITS
                 for i in bitmap.HEX_CHANNEL_OFFSETS)
   except ValueError:
      raise ManifestError(f"{where}: bad color {value!r}") from None
   return r | g << GREEN_SHIFT | b << BLUE_SHIFT


def palette(spec: Any, named: Mapping[str, Any], where: str) -> list[int]:
   """!Resolves an asset's palette: a list of up to 15 colors or a name.
   @param spec   list of color strings, or a key of the manifest's "palettes"
   @param named  the manifest's named palettes
   @param where  location for error messages
   @return 16 BGR555 colors; entry 0 (transparent) is 0
   """
   if isinstance(spec, str):
      if spec not in named:
         raise ManifestError(f"{where}: unknown palette {spec!r}")
      spec = named[spec]
   if not isinstance(spec, list) or not 1 <= len(spec) <= bitmap.MAX_COLORS:
      raise ManifestError(f"{where}: palette needs 1-15 colors (entries 1-15)")
   colors = [0] + [bgr555(c, where) for c in spec]
   return colors + [0] * (PALETTE_ENTRIES - len(colors))


class Canvas:
   """!An indexed image that shapes are drawn into (painter's order)."""

   def __init__(self, w: int, h: int, fill: int = 0) -> None:
      """!Creates a canvas.
      @param w     width, pixels
      @param h     height, pixels
      @param fill  initial color index
      """
      ## Width, pixels.
      self.w = w
      ## Height, pixels.
      self.h = h
      ## Rows of color indices.
      self.px = [[fill] * w for _ in range(h)]

   def put(self, x: int, y: int, c: int) -> None:
      """!Sets one pixel, ignoring pixels off the canvas.
      @param x  column
      @param y  row
      @param c  color index
      @return None
      """
      if 0 <= x < self.w and 0 <= y < self.h:
         self.px[y][x] = c


def color_index(value: object, ncolors: int, where: str) -> int:
   """!Checks a color index against the palette.
   @param value    the index
   @param ncolors  colors in the palette (entries 1..ncolors)
   @param where    location for the error message
   @return the index
   """
   if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= ncolors:
      raise ManifestError(f"{where}: color must be 0 (transparent) or 1-{ncolors}, got {value!r}")
   return value


def draw(canvas: Canvas, shape: JsonDict, ncolors: int, where: str) -> None:
   """!Draws one shape. Coordinates are pixels; a pixel is inside a curved
   shape when its centre is.
   @param canvas   the Canvas
   @param shape    shape object
   @param ncolors  palette size, for color checks
   @param where    location for error messages
   @return None
   """
   kind = shape.get("shape")
   if kind not in SHAPE_KEYS:
      raise ManifestError(f"{where}: unknown shape {kind!r} (known: {', '.join(SHAPE_KEYS)})")
   check_keys(shape, SHAPE_KEYS[kind] | {"shape"}, where)
   g = shape.get

   def num(key: str, default: int | None = None) -> int:
      """!An integer field of the shape.
      @param key      field name
      @param default  value when absent (None: required)
      @return the value
      """
      v = g(key, default)
      if not isinstance(v, int) or isinstance(v, bool):
         raise ManifestError(f"{where}: {kind}.{key} must be an integer, got {v!r}")
      return v

   def colors(key: str) -> list[int]:
      """!A list field of color indices.
      @param key  field name
      @return the checked indices
      """
      v = g(key)
      if not isinstance(v, list) or len(v) < MIN_PATTERN_COLORS:
         raise ManifestError(f"{where}: {kind}.{key} needs at least 2 color indices")
      return [color_index(c, ncolors, where) for c in v]

   if kind in ("rect", "frame", "vgradient", "hgradient", "stripes", "checker", "dots"):
      x0, y0 = num("x", 0), num("y", 0)
      w, h = num("w", canvas.w), num("h", canvas.h)
   if kind == "rect":
      c = color_index(g("color"), ncolors, where)
      for y in range(y0, y0 + h):
         for x in range(x0, x0 + w):
            canvas.put(x, y, c)
   elif kind == "frame":
      c, t = color_index(g("color"), ncolors, where), num("width", 1)
      for y in range(y0, y0 + h):
         for x in range(x0, x0 + w):
            if x < x0 + t or x >= x0 + w - t or y < y0 + t or y >= y0 + h - t:
               canvas.put(x, y, c)
   elif kind in ("circle", "ring", "ellipse"):
      cx, cy = num("cx"), num("cy")
      rx = num("r") if kind != "ellipse" else num("rx")
      ry = num("r") if kind != "ellipse" else num("ry")
      inner = num("width") if kind == "ring" else None
      c = color_index(g("color"), ncolors, where)
      for y in range(cy - ry - BOUNDS_MARGIN, cy + ry + BOUNDS_MARGIN + 1):
         for x in range(cx - rx - BOUNDS_MARGIN, cx + rx + BOUNDS_MARGIN + 1):
            # Pixel centre (x + 0.5, y + 0.5) in doubled integer coordinates.
            dx, dy = SUBPIXEL_SCALE * x + 1 - SUBPIXEL_SCALE * cx, SUBPIXEL_SCALE * y + 1 - SUBPIXEL_SCALE * cy
            # Outside the ellipse (dx/2rx)^2 + (dy/2ry)^2 <= 1, multiplied out to stay in integers.
            if dx * dx * ry * ry + dy * dy * rx * rx > SUBPIXEL_SCALE_SQUARED * rx * rx * ry * ry:
               continue
            if inner is not None:
               # A ring leaves out the circle inside its width.
               ir = rx - inner
               if ir > 0 and dx * dx + dy * dy < SUBPIXEL_SCALE_SQUARED * ir * ir:
                  continue
            canvas.put(x, y, c)
   elif kind == "polygon":
      pts = g("points")
      if not isinstance(pts, list) or len(pts) < MIN_POLYGON_POINTS:
         raise ManifestError(f"{where}: polygon needs at least 3 points")
      c = color_index(g("color"), ncolors, where)
      xs, ys = [p[0] for p in pts], [p[1] for p in pts]
      for y in range(min(ys), max(ys) + 1):
         for x in range(min(xs), max(xs) + 1):
            px, py = SUBPIXEL_SCALE * x + 1, SUBPIXEL_SCALE * y + 1   # doubled pixel centre
            # Even-odd rule: count the edges a ray to the right of the centre crosses.
            inside = False
            for i in range(len(pts)):
               ax, ay = SUBPIXEL_SCALE * pts[i][0], SUBPIXEL_SCALE * pts[i][1]
               bx, by = SUBPIXEL_SCALE * pts[i - 1][0], SUBPIXEL_SCALE * pts[i - 1][1]
               if (ay > py) != (by > py):
                  # x of the edge at py, compared without division
                  lhs = (px - ax) * (by - ay)
                  rhs = (bx - ax) * (py - ay)
                  if (lhs < rhs) == (by > ay):
                     inside = not inside
            if inside:
               canvas.put(x, y, c)
   elif kind in ("vgradient", "hgradient"):
      cs = colors("colors")
      span = h if kind == "vgradient" else w
      for y in range(y0, y0 + h):
         for x in range(x0, x0 + w):
            t = (y - y0) if kind == "vgradient" else (x - x0)
            canvas.put(x, y, cs[min(len(cs) - 1, t * len(cs) // max(1, span))])
   elif kind == "stripes":
      cs, period = colors("colors"), num("period", DEFAULT_STRIPE_PERIOD)
      vertical = g("dir", "h") == "v"
      for y in range(y0, y0 + h):
         for x in range(x0, x0 + w):
            t = (x - x0) if vertical else (y - y0)
            canvas.put(x, y, cs[(t // max(1, period)) % len(cs)])
   elif kind == "checker":
      cs, cell = colors("colors"), num("cell", DEFAULT_CHECKER_CELL)
      for y in range(y0, y0 + h):
         for x in range(x0, x0 + w):
            canvas.put(x, y, cs[((x - x0) // cell + (y - y0) // cell) % len(cs)])
   elif kind == "dots":
      c, count, size = color_index(g("color"), ncolors, where), num("count", DEFAULT_DOT_COUNT), num("size", 1)
      state = num("seed", 1) & U32_MASK
      for _ in range(count):
         state = (LCG_MULTIPLIER * state + LCG_INCREMENT) & U32_MASK   # A-4 LCG: same dots every build
         x = x0 + (state >> LCG_OUTPUT_SHIFT) % max(1, w)
         state = (LCG_MULTIPLIER * state + LCG_INCREMENT) & U32_MASK
         y = y0 + (state >> LCG_OUTPUT_SHIFT) % max(1, h)
         for yy in range(y, y + size):
            for xx in range(x, x + size):
               canvas.put(xx, yy, c)


def tile_at(canvas: Canvas, tx: int, ty: int) -> Tile:
   """!One 8x8 tile of a canvas.
   @param canvas  the Canvas
   @param tx      tile column
   @param ty      tile row
   @return 8 rows of 8 color indices (tuples)
   """
   return tuple(tuple(canvas.px[ty * TILE_SIZE + y][tx * TILE_SIZE + x] for x in range(TILE_SIZE))
                for y in range(TILE_SIZE))


def words_4bpp(tile: Tile) -> list[int]:
   """!Packs a tile as 8 words, pixel 0 in the low nibble.
   @param tile  8 rows of 8 indices (0-15)
   @return list of 8 ints
   """
   return [sum(v << (BITS_4BPP * i) for i, v in enumerate(row)) for row in tile]


def words_8bpp(tile: Tile) -> list[int]:
   """!Packs a tile as 16 words, pixel 0 in the low byte.
   @param tile  8 rows of 8 indices
   @return list of 16 ints
   """
   flat = [v for row in tile for v in row]
   return [sum(flat[w * PIXELS_PER_WORD_8BPP + i] << (BITS_8BPP * i) for i in range(PIXELS_PER_WORD_8BPP))
           for w in range(WORDS_8BPP)]


def flips(tile: Tile) -> list[tuple[int, Tile]]:
   """!A tile's four orientations, as stored in map cells.
   @param tile  8 rows of 8 indices
   @return [(flip bits, tile)] for none, H, V and HV
   """
   h = tuple(tuple(reversed(r)) for r in tile)
   return [(0, tile), (H_FLIP, h), (V_FLIP, tuple(reversed(tile))), (H_FLIP | V_FLIP, tuple(reversed(h)))]


def compile_background(a: JsonDict, named: Mapping[str, Any], where: str) -> list[JsonDict]:
   """!Draws and tiles a background.
   @param a      asset object
   @param named  named palettes
   @param where  location for error messages
   @return list of one or two forms (dicts with tiles, map / affine_map,
           palette, width, height, affine); the first is the one the key names
   """
   pal = palette(a.get("palette"), named, where)
   ncolors = len([c for c in a.get("palette") if c]) if isinstance(a.get("palette"), list) else bitmap.MAX_COLORS
   form = a.get("affine", False)
   if form not in (False, True, "both"):
      raise ManifestError(f"{where}: affine must be false, true or \"both\"")
   if form == "both":
      regular = dict(a, affine=False)
      regular.pop("size", None)
      return [compile_background(regular, named, where)[0], compile_background(dict(a, affine=True), named, where)[0]]
   affine = form is True
   priority = a.get("priority", 0)
   if not isinstance(priority, int) or not 0 <= priority <= BACK_PRIORITY:
      raise ManifestError(f"{where}: priority must be 0 (front) to 3 (back)")
   size = a.get("size", DEFAULT_AFFINE_BG_SIZE if affine else REGULAR_BG_SIZE)
   if affine and size not in AFFINE_BG_SIZES:
      raise ManifestError(f"{where}: affine size must be 128 or 256")
   if not affine and size != REGULAR_BG_SIZE:
      raise ManifestError(f"{where}: regular backgrounds are 256x256 (omit 'size')")
   canvas = Canvas(size, size, color_index(a.get("fill", 0), bitmap.MAX_COLORS, where))
   for i, shape in enumerate(a.get("shapes", [])):
      draw(canvas, shape, bitmap.MAX_COLORS if ncolors > bitmap.MAX_COLORS else max(ncolors, 1), f"{where} shape {i}")
   cells = size // TILE_SIZE
   tiles, index, map_cells = [], {}, []
   if not affine:
      # Regular layer: tile 0 is blank; a tile matching an earlier one in any flip reuses it.
      blank = tuple(tuple([0] * TILE_SIZE) for _ in range(TILE_SIZE))
      index[blank] = (0, 0)
      tiles.append(blank)
      for ty in range(cells):
         for tx in range(cells):
            t = tile_at(canvas, tx, ty)
            if t not in index:
               for bits, variant in flips(t):
                  index.setdefault(variant, (len(tiles), bits))
               tiles.append(t)
            n, bits = index[t]
            map_cells.append(n | bits)
      return [{"tiles": [w for t in tiles for w in words_4bpp(t)], "count": len(tiles), "map": map_cells,
               "affine_map": None, "palette": pal, "width": cells, "height": cells, "affine": 0,
               "priority": priority}]
   # Affine layer: no flips in its map, so only identical tiles are shared.
   blank = tuple(tuple([0] * TILE_SIZE) for _ in range(TILE_SIZE))
   index[blank] = 0
   tiles.append(blank)
   for ty in range(cells):
      for tx in range(cells):
         t = tile_at(canvas, tx, ty)
         if t not in index:
            index[t] = len(tiles)
            tiles.append(t)
         map_cells.append(index[t])
   if len(tiles) > MAX_AFFINE_TILES:
      raise ManifestError(f"{where}: {len(tiles)} unique tiles; an affine layer holds at most 256")
   return [{"tiles": [w for t in tiles for w in words_8bpp(t)], "count": len(tiles), "map": None,
            "affine_map": map_cells, "palette": pal, "width": cells, "height": cells, "affine": 1,
            "priority": priority}]


def load_image(path: object, base: str | PathLike[str] | None, w: int, h: int, deps: list[Path] | None, where: str,
               images: Mapping[str, bitmap.Image] | None = None) -> bitmap.Image:
   """!Reads an image frame.
   @param path    path from the manifest, relative to @p base
   @param base    manifest directory, or None when compiling without one
   @param w       required width
   @param h       required height
   @param deps    list the resolved path is appended to, or None
   @param where   location for error messages
   @param images  images already in memory by manifest path (e.g. an editor's
                  unsaved frames), used instead of the files; or None
   @return (width, height, RGBA)
   """
   if not isinstance(path, str) or not path:
      raise ManifestError(f"{where}: image must be a file path")
   if manifest_paths.is_absolute_text(path) or "\\" in path:
      raise ManifestError(f"{where}: image paths are relative, with '/' separators: {path!r}")
   if images is not None and path in images:
      img = images[path]
      if img[:IMAGE_SIZE_FIELDS] != (w, h):
         raise ManifestError(f"{where}: {path} is {img[0]}x{img[1]}, the sprite is {w}x{h}")
      return img
   if base is None:
      raise ManifestError(f"{where}: image frames need the manifest's directory")
   full = (Path(base) / path).resolve()
   if deps is not None:
      deps.append(full)
   try:
      img = bitmap.read_png(full)
   except bitmap.BitmapError as e:
      raise ManifestError(f"{where}: {e}") from None
   if img[:IMAGE_SIZE_FIELDS] != (w, h):
      raise ManifestError(f"{where}: {path} is {img[0]}x{img[1]}, the sprite is {w}x{h}")
   return img


def load_indexed_frame(frame: JsonDict, base: str | PathLike[str] | None, w: int, h: int, deps: list[Path] | None,
                       where: str, cache: dict[Path, tuple[int, int, bytearray]]) -> list[list[int]]:
   """!I3: one frame of a palette PNG, its pixels' palette indices kept.
   @param frame  the frame object ("image", optional "frame": the frame of a sheet in reading order)
   @param base   manifest directory
   @param w      frame width
   @param h      frame height
   @param deps   list the resolved path is appended to, or None
   @param where  location for error messages
   @param cache  sheets read so far, by path
   @return h rows of w indices
   """
   path = frame.get("image")
   if not isinstance(path, str) or not path or manifest_paths.is_absolute_text(path) or "\\" in path:
      raise ManifestError(f"{where}: image paths are relative, with '/' separators: {path!r}")
   if base is None:
      raise ManifestError(f"{where}: image frames need the manifest's directory")
   full = (Path(base) / path).resolve()
   if full not in cache:
      if deps is not None:
         deps.append(full)
      try:
         cache[full] = bitmap.read_png_indices(full)
      except bitmap.BitmapError as e:
         raise ManifestError(f"{where}: {e}") from None
   iw, ih, px = cache[full]
   n = frame.get("frame", 0)
   if iw % w or ih % h:
      raise ManifestError(f"{where}: {path} is {iw}x{ih}, not a sheet of {w}x{h} frames")
   per_row = iw // w
   if not isinstance(n, int) or isinstance(n, bool) or not 0 <= n < per_row * (ih // h):
      raise ManifestError(f"{where}: frame must be 0-{per_row * (ih // h) - 1} of {path}")
   x0, y0 = (n % per_row) * w, (n // per_row) * h
   rows = [list(px[(y0 + y) * iw + x0:(y0 + y) * iw + x0 + w]) for y in range(h)]
   if any(v >= PALETTE_ENTRIES for r in rows for v in r):
      raise ManifestError(f"{where}: {path} uses palette indices past 15 (a 4bpp sprite has 16)")
   return rows


def compile_sprite(a: JsonDict, named: Mapping[str, Any], where: str, base: str | PathLike[str] | None = None,
                   deps: list[Path] | None = None, images: Mapping[str, bitmap.Image] | None = None) -> JsonDict:
   """!Draws a sprite's frames and checks its animations.
   @param a       asset object
   @param named   named palettes
   @param where   location for error messages
   @param base    manifest directory, for image frames
   @param deps    list image paths are appended to, or None
   @param images  in-memory images by manifest path (see load_image()), or None
   @return dict with tiles (words), palette, shape, size, frame count, animations
   """
   w, h = a.get("width"), a.get("height")
   if (w, h) not in OBJ_SIZES:
      raise ManifestError(f"{where}: {w}x{h} is not a sprite size "
                          f"({', '.join(f'{x}x{y}' for x, y in OBJ_SIZES)})")
   shape, size = OBJ_SIZES[(w, h)]
   frames = a.get("frames", [])
   if not isinstance(frames, list) or not frames:
      raise ManifestError(f"{where}: needs at least one frame")
   loaded = {}
   indexed: dict[int, list[list[int]]] = {}
   sheets: dict[Path, tuple[int, int, bytearray]] = {}
   for f, frame in enumerate(frames):
      check_keys(frame, {"shapes", "image", "frame", "indexed"}, f"{where} frame {f}")
      if "image" in frame:
         if "shapes" in frame:
            raise ManifestError(f"{where} frame {f}: a frame has shapes or an image, not both")
         if frame.get("indexed", False) is True:
            indexed[f] = load_indexed_frame(frame, base, w, h, deps, f"{where} frame {f}", sheets)
            continue
         if "frame" in frame or "indexed" in frame:
            raise ManifestError(f"{where} frame {f}: frame (a sheet's frame) needs indexed: true")
         loaded[f] = load_image(frame["image"], base, w, h, deps, f"{where} frame {f}", images)
   if a.get("palette") == "auto":
      if len(loaded) != len(frames) or indexed:
         raise ManifestError(f"{where}: palette 'auto' needs every frame to be an image")
      five = bitmap.make_palette(list(loaded.values()))
      pal = [0] + [r | g << GREEN_SHIFT | b << BLUE_SHIFT for r, g, b in five]
      pal += [0] * (PALETTE_ENTRIES - len(pal))
   else:
      pal = palette(a.get("palette"), named, where)
      # BGR555 back to 5-bit channels, for mapping image pixels onto the palette.
      five = [(c & CHANNEL_MASK, c >> GREEN_SHIFT & CHANNEL_MASK, c >> BLUE_SHIFT & CHANNEL_MASK) for c in pal[1:]]
   words = []
   for f, frame in enumerate(frames):
      canvas = Canvas(w, h)
      if f in indexed:
         canvas.px = indexed[f]
      if f in loaded:
         idx = bitmap.index_image(loaded[f], five)
         canvas.px = [list(idx[y * w:(y + 1) * w]) for y in range(h)]
      for i, s in enumerate(frame.get("shapes", [])):
         draw(canvas, s, bitmap.MAX_COLORS, f"{where} frame {f} shape {i}")
      for ty in range(h // TILE_SIZE):           # 1D mapping: row-major tiles
         for tx in range(w // TILE_SIZE):
            words.extend(words_4bpp(tile_at(canvas, tx, ty)))
   anims = a.get("animations", [{"frames": [0], "period": 1}])
   out = []
   for i, an in enumerate(anims):
      check_keys(an, {"frames", "period", "durations", "loop", "flips"}, f"{where} animation {i}")
      fr = an.get("frames", [])
      if not fr or any(not isinstance(x, int) or not 0 <= x < len(frames) for x in fr) or len(fr) > MAX_ANIM_FRAMES:
         raise ManifestError(f"{where} animation {i}: frames must be 1-255 indices below {len(frames)}")
      durations = an.get("durations")
      if durations is not None:
         if "period" in an:
            raise ManifestError(f"{where} animation {i}: use period or durations, not both")
         if (not isinstance(durations, list) or len(durations) != len(fr) or
                 any(not isinstance(d, int) or isinstance(d, bool) or not 1 <= d <= MAX_FRAME_DURATION
                     for d in durations)):
            raise ManifestError(f"{where} animation {i}: durations must be one 1-255 value per frame")
         period = 0
      else:
         period = an.get("period", DEFAULT_PERIOD)
         if not isinstance(period, int) or not 1 <= period <= MAX_FRAME_DURATION:
            raise ManifestError(f"{where} animation {i}: period must be 1-255")
      flips = an.get("flips")
      if flips is not None and (not isinstance(flips, list) or len(flips) != len(fr) or any(
            not isinstance(v, int) or isinstance(v, bool) or not 0 <= v <= MAX_FLIP for v in flips)):
         raise ManifestError(f"{where} animation {i}: flips must be one 0-3 value per frame")
      out.append((fr, period, 1 if an.get("loop", True) else 0, durations, flips))
   stream = a.get("stream", False)
   if not isinstance(stream, bool):
      raise ManifestError(f"{where}: stream must be true or false")
   tiles_frame = (w // TILE_SIZE) * (h // TILE_SIZE)
   steps = a.get("steps")
   if steps is not None and (not isinstance(steps, list) or len(steps) != STEP_ENTRIES or any(
         not isinstance(v, int) or isinstance(v, bool) or not 0 <= v <= MAX_ANIM_FRAMES for v in steps)):
      raise ManifestError(f"{where}: steps must be {STEP_ENTRIES} animation entries")
   return {"tiles": words, "palette": pal, "shape": shape, "size": size, "frames": len(frames),
           "tiles_total": tiles_frame if stream else len(words) // WORDS_4BPP, "animations": out,
           "stream": stream, "steps": steps}


def encode_text(value: object, where: str, codepage: text_encoder.CodePage = text_encoder.DEFAULT,
                field: Any = None) -> list[int]:
   """!Encodes a string into a code page (its characters, newline); braces are plain characters, or with the game's
   field placeholders a field text (FD 80-FE, terms, FA / FB).
   @param value     the string
   @param where     location for the error message
   @param codepage  the game's code page
   @param field     the game's field placeholders (script_compiler.FieldPlaceholders), or None
   @return list of codes ending with 0xFF
   """
   try:
      if field is not None:
         return field.encode(value, where)
      return text_encoder.encode(value, where, codepage=codepage, markup=False)
   except text_encoder.TextError as e:
      raise ManifestError(str(e)) from None


def absolute_images(node: Any, base: Path) -> Any:
   """!A copy of an included manifest's assets with their image paths made absolute (they are relative to it).
   @param node  the assets (recursively)
   @param base  the included manifest's folder
   @return the copy
   """
   if isinstance(node, dict):
      paths = path_keys(node)
      return {k: (str((base / v).resolve()) if k in paths and isinstance(v, str) else absolute_images(v, base))
              for k, v in node.items()}
   if isinstance(node, list):
      return [absolute_images(v, base) for v in node]
   return node


def path_keys(node: Mapping[str, Any]) -> tuple[str, ...]:
   """!The keys of an asset object that hold file paths: "image", and "font" except in a text box style, where it
   names a font asset (C10); UC: "data" of tiles and tilemaps (their raw files).
   @param node  the object
   @return the keys
   """
   if node.get("kind") in screen_assets.DATA_KINDS:
      return ("image", "data")
   return ("image",) if node.get("kind") == "text_box" else ("image", "font")


def relative_image(path: str, base: Path) -> str:
   """!One merged absolute image path made relative to the including manifest's folder, with '/' separators (I4;
   D-40 RW3: the absolute check and the separators are the same on every system).
   @param path  the absolute path (absolute_images() resolved it on this system)
   @param base  the including manifest's folder
   @return the relative path
   @throws ManifestError when the image is on another drive than the manifest (Windows)
   """
   try:
      return manifest_paths.relative_to_folder(path, base)
   except manifest_paths.PathError as e:
      raise ManifestError(f"includes: {e}") from None


def relative_images(node: Any, base: Path) -> Any:
   """!A copy of merged assets with their absolute image paths made relative to the including manifest's folder
   (image paths are relative, with '/' separators; I4).
   @param node  the assets (recursively)
   @param base  the including manifest's folder
   @return the copy
   """
   if isinstance(node, dict):
      paths = path_keys(node)
      return {k: (relative_image(v, base) if k in paths and isinstance(v, str) and manifest_paths.is_absolute_text(v)
                  else relative_images(v, base))
              for k, v in node.items()}
   if isinstance(node, list):
      return [relative_images(v, base) for v in node]
   return node


def expand_manifest(manifest: JsonDict, base: Path, deps: list[Path] | None = None,
                    seen: set[Path] | None = None) -> tuple[JsonDict, list[JsonDict]]:
   """!A manifest with its includes merged in and its text tables read (I4).
   @param manifest  parsed manifest
   @param base      its folder
   @param deps      collects the files read, or None
   @param seen      manifests already merged (a cycle of includes is an error)
   @return (the merged manifest: palettes, assets; the table texts: [{"key", "text", "wrap", "font", "box",
           "fonts", "table", ...}])
   """
   top = seen is None   # the including manifest: merged image paths become relative to it again
   seen = set() if seen is None else seen
   m = strip(manifest)
   check_keys(m, TOP_KEYS, "manifest")
   palettes = dict(m.get("palettes", {}))
   assets = list(m.get("assets", []))
   included: list[JsonDict] = []   # the includes' assets, their image paths absolute
   table_texts: list[JsonDict] = []
   for rel in m.get("includes", []) or []:
      path = (base / str(rel)).resolve()
      if path in seen:
         raise ManifestError(f"includes: {rel} is included twice (or includes itself)")
      seen.add(path)
      try:
         inner = json.loads(path.read_text(encoding="utf-8"))
      except (OSError, ValueError) as e:
         raise ManifestError(f"includes: cannot read {rel} ({e})") from None
      if deps is not None:
         deps.append(path)
      merged, more = expand_manifest(inner, path.parent, deps, seen)
      for name, pal in merged.get("palettes", {}).items():
         if name in palettes:
            raise ManifestError(f"includes: {rel}: palette {name!r} is defined twice")
         palettes[name] = pal
      included += absolute_images(merged.get("assets", []), path.parent)
      table_texts += more
   for rel in m.get("text_tables", []) or []:
      path = (base / str(rel)).resolve()
      table_texts += load_text_table(path, deps)
   out = {k: v for k, v in m.items() if k not in ("includes", "text_tables")}
   # only the merged assets change: the manifest's own image paths stay as written (and are checked as written)
   out["palettes"] = palettes
   out["assets"] = assets + (relative_images(included, base) if top else included)
   return out, table_texts


def load_text_table(path: Path, deps: list[Path] | None = None) -> list[JsonDict]:
   """!One text table's texts (capsule.texts/1, I4).
   @param path  the table
   @param deps  collects the files read, or None
   @return [{"key", "text", "wrap", "font", "box", "fonts", "table"}]
   """
   where = path.name
   try:
      t = json.loads(path.read_text(encoding="utf-8"))
   except (OSError, ValueError) as e:
      raise ManifestError(f"text_tables: cannot read {where} ({e})") from None
   if deps is not None:
      deps.append(path)
   if not isinstance(t, dict) or t.get("schema") != TEXT_TABLE_SCHEMA:
      raise ManifestError(f"{where}: schema must be {TEXT_TABLE_SCHEMA!r}")
   check_keys(t, TEXT_TABLE_KEYS, where)
   box = t.get("box")
   if box is not None:
      if not isinstance(box, dict):
         raise ManifestError(f"{where}: box is {{\"font\", \"width\", \"lines\"}}")
      check_keys(box, BOX_KEYS, f"{where}: box")
   fonts = {n: str((path.parent / str(f)).resolve()) for n, f in (t.get("fonts") or {}).items()}
   texts = t.get("texts")
   if not isinstance(texts, dict):
      raise ManifestError(f"{where}: texts is an object of key -> text")
   out = []
   for key, entry in texts.items():
      if isinstance(entry, str):
         entry = {"text": entry}
      if not isinstance(entry, dict) or not isinstance(entry.get("text"), str):
         raise ManifestError(f"{where}: {key}: a text or {{\"text\": ...}}")
      wrap = entry.get("wrap", WRAP_MODES[1])
      if wrap not in WRAP_MODES:
         raise ManifestError(f"{where}: {key}: wrap is one of {', '.join(WRAP_MODES)}")
      font = entry.get("font", (box or {}).get("font"))
      if font is not None and font not in fonts:
         raise ManifestError(f"{where}: {key}: font {font!r} is not one of the table's fonts")
      out.append({"key": key, "text": entry["text"], "wrap": wrap, "font": font, "box": box, "fonts": fonts,
                  "table": where, "language": t.get("language")})
   return out


def select_language(texts: list[JsonDict], manifest: Mapping[str, Any], language: str | None) -> list[JsonDict]:
   """!C10: the table texts of the build's language. A manifest may name text tables of several languages (the same
   keys in each); the build takes one: `language` (the command line's --language), else the manifest's "language",
   else the tables' only language. A table without "language" belongs to every language. With "fallback_language", a
   key the build's language lacks takes that language's text.
   @param texts     every table text (load_text_table(): "language" per text)
   @param manifest  the merged manifest (its "language", "fallback_language")
   @param language  the language asked for, or None
   @return the texts the build compiles
   @throws ManifestError when the language cannot be decided or names no table
   """
   languages = sorted({t["language"] for t in texts if t.get("language") is not None})
   chosen = language or manifest.get("language")
   if chosen is None and len(languages) > 1:
      raise ManifestError("text_tables of several languages (" + ", ".join(languages) + "): name the build's "
                          "language (the manifest's \"language\" or --language)")
   chosen = chosen or (languages[0] if languages else None)
   if chosen is not None and languages and chosen not in languages:
      raise ManifestError(f"no text table is in language {chosen!r} (" + ", ".join(languages) + ")")
   keep = [t for t in texts if t.get("language") in (None, chosen)]
   fallback = manifest.get("fallback_language")
   if fallback is not None and fallback != chosen:
      if fallback not in languages:
         raise ManifestError(f"fallback_language {fallback!r} names no text table's language")
      have = {t["key"] for t in keep}
      keep += [t for t in texts if t.get("language") == fallback and t["key"] not in have]
   return keep


def measure_lines(codes: Sequence[int], font: Mapping[str, Any], field: Any) -> list[tuple[int, bool]]:
   """!The pixel width of each line of encoded text in a proportional font (I4): lines end at newlines, page breaks,
   prompts and the scroll control; a placeholder counts its term's text, or nothing (the line is marked).
   @param codes  the codes (ending with CODE_END)
   @param font   compile_proportional() result
   @param field  the field placeholders (script_compiler.FieldPlaceholders), or None
   @return [(width, has a placeholder that counted nothing)] per line
   """
   first, widths, spacing = font["first"], font["widths"], font["letter_spacing"]
   terms = {}
   if field is not None:
      for name, (kind, value, extra) in field.bindings.items():
         if kind == "term":
            terms[field.codes[name]] = field.term_codes(name, value, extra.get("form", "one"))[:-1]
   scroll = text_encoder.CONTROL_CODES["scroll"][0]
   out = []
   width, unmeasured = 0, False
   i = 0

   def glyph(c: int) -> int:
      return (widths[c - first] if first <= c < first + len(widths) else 0) + spacing

   while i < len(codes) and codes[i] != CODE_END:
      c = codes[i]
      if c in (CODE_NEWLINE, text_encoder.CODE_PAGE_BREAK, text_encoder.CODE_PROMPT) or (
            c == text_encoder.CODE_CONTROL and i + 1 < len(codes) and codes[i + 1] == scroll):
         out.append((width, unmeasured))
         width, unmeasured = 0, False
         i += text_encoder.control_length(list(codes), i) if c == text_encoder.CODE_CONTROL else 1
         continue
      if c == text_encoder.CODE_PLACEHOLDER and i + 1 < len(codes):
         arg = codes[i + 1]
         if arg in terms:
            width += sum(glyph(t) for t in terms[arg])
         else:
            unmeasured = True
         i += text_encoder.PLACEHOLDER_CODES
         continue
      if c == text_encoder.CODE_CONTROL:
         i += text_encoder.control_length(list(codes), i)
         continue
      width += glyph(c)
      i += 1
   out.append((width, unmeasured))
   return out


def wrap_codes(codes: list[int], font: Mapping[str, Any], width: int, lines: int) -> list[int]:
   """!Breaks encoded text at spaces so each line fits a width, a page break after every `lines` lines (I4,
   "wrap": "auto"; authored breaks stay and start a new line).
   @param codes  the codes (ending with CODE_END)
   @param font   compile_proportional() result
   @param width  the box width, pixels
   @param lines  lines per page
   @return the wrapped codes
   """
   first, widths, spacing = font["first"], font["widths"], font["letter_spacing"]
   out = list(codes)
   line_w, line_no, last_space = 0, 1, -1
   i = 0
   while i < len(out) and out[i] != CODE_END:
      c = out[i]
      if c in (CODE_NEWLINE, text_encoder.CODE_PAGE_BREAK):
         line_w, last_space = 0, -1
         line_no = 1 if c == text_encoder.CODE_PAGE_BREAK else line_no + 1
         i += 1
         continue
      if c in (text_encoder.CODE_CONTROL, text_encoder.CODE_PLACEHOLDER):
         i += text_encoder.control_length(out, i) if c == text_encoder.CODE_CONTROL else text_encoder.PLACEHOLDER_CODES
         continue
      if c == text_encoder.CODE_SPACE:
         last_space = i
      line_w += (widths[c - first] if first <= c < first + len(widths) else 0) + spacing
      if line_w > width and last_space >= 0:
         line_no += 1
         out[last_space] = CODE_NEWLINE if line_no <= lines else text_encoder.CODE_PAGE_BREAK
         line_no = line_no if line_no <= lines else 1
         i = last_space + 1
         line_w, last_space = 0, -1
         continue
      i += 1
   return out


def glyphs_of(codes: Sequence[int]) -> list[int]:
   """!The glyph codes of encoded text (escapes and their arguments skipped; I4).
   @param codes  the codes
   @return the glyph codes, in order
   """
   out = []
   i = 0
   while i < len(codes) and codes[i] != CODE_END:
      c = codes[i]
      if c == text_encoder.CODE_CONTROL:
         i += text_encoder.control_length(list(codes), i)
         continue
      if c == text_encoder.CODE_PLACEHOLDER:
         i += text_encoder.PLACEHOLDER_CODES
         continue
      if c <= text_encoder.MAX_GLYPH_CODE:
         out.append(c)
      i += 1
   return out


def compile_table_texts(table_texts: Sequence[JsonDict], codepage: text_encoder.CodePage, field: Any,
                        fit: JsonDict, deps: list[Path] | None) -> list[tuple[str, list[int]]]:
   """!The text tables' texts encoded (I4): authored ones checked against their box, automatic ones wrapped.
   @param table_texts  expand_manifest()'s table texts
   @param codepage     the game's code page
   @param field        the field placeholders, or None
   @param fit          the fit report, filled in ("checked", "over", "with_placeholders", "lines")
   @param deps         collects the fonts read, or None
   @return [(key, codes)]
   """
   import font_compiler
   fonts: dict[str, Any] = {}
   out = []
   fit.setdefault("checked", 0)
   fit.setdefault("over", 0)
   fit.setdefault("with_placeholders", 0)
   fit.setdefault("lines", [])
   for t in table_texts:
      where = f"{t['table']}: {t['key']}"
      codes = encode_text(t["text"], where, codepage, field)
      box = t["box"]
      if box is not None and t["font"] is not None:
         path = t["fonts"][t["font"]]
         if path not in fonts:
            try:
               fonts[path] = font_compiler.compile_proportional(Path(path), codepage)
            except text_encoder.TextError as e:
               raise ManifestError(f"{where}: {e}") from None
            if deps is not None:
               deps.append(Path(path))
         font = fonts[path]
         missing = sorted(set(glyphs_of(codes)) - font["codes"])
         if missing:
            raise ManifestError(f"{where}: the font {t['font']!r} has no glyph for "
                                f"{', '.join(codepage.decode([c]) or f'0x{c:02X}' for c in missing)}")
         width = int(box.get("width", 0))
         if t["wrap"] == "auto" and width > 0:
            codes = wrap_codes(codes, font, width, int(box.get("lines", DEFAULT_BOX_LINES)))
         elif width > 0:
            fit["checked"] += 1
            for n, (w, unmeasured) in enumerate(measure_lines(codes, font, field)):
               if unmeasured:
                  fit["with_placeholders"] += 1
               if w > width:
                  fit["over"] += 1
                  if len(fit["lines"]) < MAX_FIT_LISTED:
                     fit["lines"].append({"key": t["key"], "line": n + 1, "width": w, "box": width,
                                          "with_placeholders": unmeasured})
      out.append((t["key"], codes))
   return out


def compile_arrow_image(arrow: Any, base: str | PathLike[str] | None, deps: list[Path] | None,
                        where: str) -> JsonDict:
   """!N3: a text box window's arrow image ("arrow_image": {"image", "height", "steps", "delay"}): an indexed palette
   PNG 8 pixels wide (its indices kept, 0 shows the window's background), the rows shown at once, the first row
   shown per step and the frames between two steps.
   @param arrow  the object
   @param base   manifest directory
   @param deps   list the image's path is appended to, or None
   @param where  label for errors
   @return {"rows": [32-bit row words], "height", "steps": [...], "delay"}
   @throws ManifestError on a bad setting
   """
   if not isinstance(arrow, dict):
      raise ManifestError(f"{where}: arrow_image is an object ({', '.join(sorted(TEXT_BOX_ARROW_KEYS))})")
   check_keys(arrow, TEXT_BOX_ARROW_KEYS, f"{where}: arrow_image")
   path = arrow.get("image")
   if not isinstance(path, str) or not path or manifest_paths.is_absolute_text(path) or "\\" in path:
      raise ManifestError(f"{where}: arrow_image image is a relative path with '/' separators")
   if base is None:
      raise ManifestError(f"{where}: arrow_image needs the manifest's directory")
   full = (Path(base) / path).resolve()
   if deps is not None:
      deps.append(full)
   try:
      iw, ih, px = bitmap.read_png_indices(full)
   except bitmap.BitmapError as e:
      raise ManifestError(f"{where}: {e}") from None
   if iw != ARROW_IMAGE_WIDTH or not 0 < ih <= MAX_ARROW_ROWS or any(v >= PALETTE_ENTRIES for v in px):
      raise ManifestError(f"{where}: arrow_image {path} must be {ARROW_IMAGE_WIDTH} pixels wide, 1-{MAX_ARROW_ROWS} "
                          f"rows, indices 0-{PALETTE_ENTRIES - 1}")
   rows = []
   for y in range(ih):
      word = 0
      for x in range(iw):
         word |= px[y * iw + x] << (x * NIBBLE_BITS)
      rows.append(word)
   height, steps, delay = arrow.get("height", ih), arrow.get("steps", [0]), arrow.get("delay", 0)
   if isinstance(height, bool) or not isinstance(height, int) or not 0 < height <= ih:
      raise ManifestError(f"{where}: arrow_image height must be 1-{ih}")
   if (not isinstance(steps, list) or not steps or len(steps) > MAX_ARROW_ROWS
         or any(isinstance(v, bool) or not isinstance(v, int) or not 0 <= v < ih for v in steps)):
      raise ManifestError(f"{where}: arrow_image steps are 1-{MAX_ARROW_ROWS} rows of the image (0-{ih - 1})")
   if isinstance(delay, bool) or not isinstance(delay, int) or not 0 <= delay <= MAX_ARROW_ROWS:
      raise ManifestError(f"{where}: arrow_image delay must be 0-{MAX_ARROW_ROWS}")
   return {"rows": rows, "height": height, "steps": steps, "delay": delay}


def frame_image_tiles(path: Any, base: str | PathLike[str] | None, deps: list[Path] | None, where: str) -> list[int]:
   """!N3: a frame's art: an indexed palette PNG of 3 x 3 tiles (24 x 24 pixels, indices kept), the nine parts in
   reading order (text::window::Frame: top-left, top, top-right, left, inside, right, bottom-left, bottom,
   bottom-right).
   @param path   the image, relative to the manifest
   @param base   manifest directory
   @param deps   list the image's path is appended to, or None
   @param where  label for errors
   @return the nine tiles' 4bpp words (72)
   @throws ManifestError on a bad image
   """
   if not isinstance(path, str) or not path or manifest_paths.is_absolute_text(path) or "\\" in path:
      raise ManifestError(f"{where}: frame image is a relative path with '/' separators")
   if base is None:
      raise ManifestError(f"{where}: frame image needs the manifest's directory")
   full = (Path(base) / path).resolve()
   if deps is not None:
      deps.append(full)
   try:
      iw, ih, px = bitmap.read_png_indices(full)
   except bitmap.BitmapError as e:
      raise ManifestError(f"{where}: {e}") from None
   if (iw, ih) != (FRAME_IMAGE_PIXELS, FRAME_IMAGE_PIXELS) or any(v >= PALETTE_ENTRIES for v in px):
      raise ManifestError(f"{where}: frame image {path} must be {FRAME_IMAGE_PIXELS}x{FRAME_IMAGE_PIXELS} pixels, "
                          f"indices 0-{PALETTE_ENTRIES - 1}")
   words: list[int] = []
   parts = FRAME_IMAGE_PIXELS // TILE_SIZE
   for ty in range(parts):
      for tx in range(parts):
         canvas = Canvas(TILE_SIZE, TILE_SIZE)
         canvas.px = [list(px[(ty * TILE_SIZE + y) * iw + tx * TILE_SIZE:(ty * TILE_SIZE + y) * iw + tx * TILE_SIZE +
                              TILE_SIZE]) for y in range(TILE_SIZE)]
         words.extend(words_4bpp(tile_at(canvas, 0, 0)))
   return words


def compile_menu_style(menu: Any, where: str) -> JsonDict:
   """!N3: a text box style's menu (ui::MenuStyle): positions, cursor, wrap rule, input delay and keys, sounds, a
   list's scroll arrows.
   @param menu   the object
   @param where  label for errors
   @return the members as C++ values ({"name": "value"})
   @throws ManifestError on a bad setting
   """
   if not isinstance(menu, dict):
      raise ManifestError(f"{where}: menu is an object of settings")
   check_keys(menu, set(TEXT_BOX_MENU) | set(TEXT_BOX_MENU_WORDS) | {"cancel_keys", "arrow_sprite"},
              f"{where}: menu")
   out: dict[str, str] = {}
   for k, (lo, hi) in TEXT_BOX_MENU.items():
      if k in menu:
         v = menu[k]
         if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
            raise ManifestError(f"{where}: menu {k} must be {lo}-{hi}")
         out[k] = str(v)
   for k, values in TEXT_BOX_MENU_WORDS.items():
      if k in menu:
         names = TEXT_BOX_MENU_NAMES[k]
         if menu[k] not in names:
            raise ManifestError(f"{where}: menu {k} is one of {', '.join(names)}")
         out[k] = f"ui::{values[names.index(menu[k])]}"
   if "cancel_keys" in menu:
      keys = menu["cancel_keys"]
      if not isinstance(keys, list) or any(k not in MENU_KEY_NAMES for k in keys):
         raise ManifestError(f"{where}: menu cancel_keys lists keys of {', '.join(MENU_KEY_NAMES)}")
      bits = 0
      for k in keys:
         bits |= 1 << MENU_KEY_NAMES.index(k)
      out["cancel_keys"] = f"0x{bits:04X}u"
   if "arrow_sprite" in menu:
      if not isinstance(menu["arrow_sprite"], str):
         raise ManifestError(f"{where}: menu arrow_sprite names a sprite asset (\"sprite:NAME\")")
      try:
         out["arrow_sprite"] = f"AssetKey{{0x{asset_key(menu['arrow_sprite'], where):08X}u}}"
      except SequenceManifestError as e:
         raise ManifestError(str(e)) from None
   return out


def compile_text_box(a: JsonDict, where: str, base: str | PathLike[str] | None = None,
                     deps: list[Path] | None = None) -> JsonDict:
   """!C10: a text box style (kind "text_box", asset::TextBox): its window's settings, frame and fonts by key. N3: the
   window's arrow image ("arrow_image") and the style's menu ("menu", ui::MenuStyle).
   @param a      the asset
   @param where  label for errors
   @param base   manifest directory (an arrow image's path is relative to it)
   @param deps   list an arrow image's path is appended to, or None
   @return {"key", "font": key, "fonts": {n: key}, "window": {...}, "frame": {...}, "arrow": {...} or None,
           "menu": {...} or None} (numbers resolved, fonts not yet)
   @throws ManifestError on a bad setting
   """
   def font_key(v: Any, what: str) -> int:
      if not isinstance(v, str):
         raise ManifestError(f"{where}: {what} names a font asset (\"font:NAME\")")
      try:
         return asset_key(v, f"{where} {what}")
      except SequenceManifestError as e:
         raise ManifestError(str(e)) from None

   def numbers(obj: Any, ranges: Mapping[str, tuple[int, int]], words: Iterable[str], what: str) -> JsonDict:
      if not isinstance(obj, dict):
         raise ManifestError(f"{where}: {what} is an object of settings")
      check_keys(obj, set(ranges) | set(words), f"{where}: {what}")
      out: JsonDict = {}
      for k, v in obj.items():
         if k in ranges:
            lo, hi = ranges[k]
            if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
               raise ManifestError(f"{where}: {what} {k} must be {lo}-{hi}")
         out[k] = v
      return out

   window_in = dict(a.get("window", {})) if isinstance(a.get("window", {}), dict) else a.get("window")
   arrow_in = window_in.pop("arrow_image", None) if isinstance(window_in, dict) else None
   window = numbers(window_in, TEXT_BOX_WINDOW, TEXT_BOX_WORDS, "window")
   typing = window.get("typing", TEXT_BOX_TYPING[0])
   if typing not in TEXT_BOX_TYPING:
      raise ManifestError(f"{where}: window typing is one of {', '.join(TEXT_BOX_TYPING)}")
   speedup = window.get("speedup", TEXT_BOX_SPEEDUP[0])
   if speedup not in TEXT_BOX_SPEEDUP:
      raise ManifestError(f"{where}: window speedup is one of {', '.join(TEXT_BOX_SPEEDUP)}")
   window["typing"] = TEXT_BOX_TYPING.index(typing)
   window["speedup"] = TEXT_BOX_SPEEDUP.index(speedup)
   frame_in = dict(a.get("frame", {})) if isinstance(a.get("frame", {}), dict) else a.get("frame")
   art = {k: frame_in.pop(k) for k in TEXT_BOX_FRAME_ART if isinstance(frame_in, dict) and k in frame_in}
   frame = numbers(frame_in, TEXT_BOX_FRAME, (), "frame")
   fonts = a.get("fonts", {})
   if not isinstance(fonts, dict):
      raise ManifestError(f"{where}: fonts maps a Font control's number (\"0\"-\"254\") to a font asset")
   by_number: dict[int, int] = {}
   for n, v in fonts.items():
      if not str(n).isdigit() or not 0 <= int(n) < MAX_TEXT_BOX_FONTS:
         raise ManifestError(f"{where}: fonts: {n!r} is not a Font control number (0-{MAX_TEXT_BOX_FONTS - 1})")
      by_number[int(n)] = font_key(v, f"fonts {n}")
   arrow = compile_arrow_image(arrow_in, base, deps, where) if arrow_in is not None else None
   menu = compile_menu_style(a["menu"], where) if "menu" in a else None
   colors = palette(a["palette"], {}, f"{where} palette") if "palette" in a else None
   frame_colors = palette(art["palette"], {}, f"{where} frame palette") if "palette" in art else None
   frame_tiles = frame_image_tiles(art["image"], base, deps, where) if "image" in art else None
   return {"key": a["key"], "font": font_key(a.get("font"), "font"), "fonts": by_number, "window": window,
           "frame": frame, "arrow": arrow, "menu": menu, "palette": colors, "frame_palette": frame_colors,
           "frame_tiles": frame_tiles}


def text_box_lines(boxes: Sequence[JsonDict], font_index: Mapping[int, int]) -> list[str]:
   """!C10: the generated text box styles (asset::TextBox) and the font tables of their Font controls.
   @param boxes       compile_text_box() results
   @param font_index  font asset key -> index into kFonts
   @return the C++ lines (empty without styles)
   @throws ManifestError when a style names a font the table does not have
   """
   if not boxes:
      return []

   def font_ref(key: int, where: str) -> str:
      if key not in font_index:
         raise ManifestError(f"{where}: names a font that is no font asset of this manifest")
      return f"&kFonts[{font_index[key]}]"

   lines = []
   for i, b in enumerate(boxes):
      if b.get("arrow") is not None:   # N3: the arrow image's rows and steps
         rows = ", ".join(f"0x{v:08X}u" for v in b["arrow"]["rows"])
         lines.append(f"alignas(4) const uint32_t kBoxArrowRows{i}[] = {{{rows}}};")
         lines.append(f"const uint8_t kBoxArrowSteps{i}[] = {{{', '.join(str(v) for v in b['arrow']['steps'])}}};")
      for name, colors in (("kBoxPalette", b.get("palette")), ("kBoxFramePalette", b.get("frame_palette"))):
         if colors is not None:    # N3: the window's and the frame's bank colors
            lines.append(f"const Color {name}{i}[] = {{{', '.join(f'0x{c:04X}' for c in colors)}}};")
      if b.get("frame_tiles") is not None:   # N3: the frame's art
         lines.append(f"alignas(4) const uint32_t kBoxFrameTiles{i}[] = {{"
                      f"{', '.join(f'0x{v:08X}u' for v in b['frame_tiles'])}}};")
      if b.get("menu") is not None:    # N3: the style's menu
         menu = ", ".join(f".{k} = {v}" for k, v in sorted(b["menu"].items(), key=lambda kv: MENU_ORDER.index(kv[0])))
         lines.append(f"const ui::MenuStyle kBoxMenu{i} = {{{menu}}};")
      if b["fonts"]:
         refs = [font_ref(b["fonts"][n], f"{b['key']} fonts {n}") if n in b["fonts"] else "nullptr"
                 for n in range(max(b["fonts"]) + 1)]
         lines.append(f"const text::PropFont* const kBoxFonts{i}[] = {{{', '.join(refs)}}};")
   lines.append("const TextBox kTextBoxes[] = {")
   for i, b in enumerate(boxes):
      w, f = b["window"], b["frame"]
      fields = [f".font = {font_ref(b['font'], b['key'])}"]
      fields += [f".{k} = {w.get(k, TEXT_BOX_DEFAULTS[k])}" for k in ("bg",)]
      fields.append(f".char_block = {w.get('char_block', LAYER_CHAR_BLOCK)}")
      fields += [f".{k} = {w.get(k, TEXT_BOX_DEFAULTS[k])}" for k in (
         "first_tile", "palette_bank", "x", "y", "width", "height", "left", "top", "lines", "ink", "shadow",
         "background", "frames_per_char", "scroll_speed")]
      fields.append(f".arrow = {w['arrow']}" if "arrow" in w else ".arrow = text::window::kNoArrow")
      fields += [f".typing = {w['typing']}", f".char_delay = {w.get('char_delay', 0)}",
                 f".speedup = {w['speedup']}", f".lead_frames = {w.get('lead_frames', 0)}"]
      if b["fonts"]:
         fields += [f".font_count = {max(b['fonts']) + 1}", f".fonts = kBoxFonts{i}"]
      arrow = b.get("arrow")
      if arrow is not None:
         fields += [f".arrow_kind = {ARROW_KIND_IMAGE}", f".arrow_rows = kBoxArrowRows{i}",
                    f".arrow_image_rows = {len(arrow['rows'])}", f".arrow_height = {arrow['height']}",
                    f".arrow_offsets = kBoxArrowSteps{i}", f".arrow_steps = {len(arrow['steps'])}",
                    f".arrow_delay = {arrow['delay']}"]
      frame = ", ".join(f".{k} = {f.get(k, 0)}" for k in TEXT_BOX_FRAME)
      menu_ref = f"&kBoxMenu{i}" if b.get("menu") is not None else "nullptr"
      art = [f"kBoxPalette{i}" if b.get("palette") is not None else "nullptr",
             f"kBoxFrameTiles{i}" if b.get("frame_tiles") is not None else "nullptr",
             f"kBoxFramePalette{i}" if b.get("frame_palette") is not None else "nullptr"]
      lines.append(f"    {{{{{', '.join(fields)}}}, {{{frame}}}, {menu_ref}, {', '.join(art)}}},   // {b['key']}")
   lines.append("};")
   return lines


def compile_font_asset(a: JsonDict, base: str | PathLike[str] | None, codepage: text_encoder.CodePage,
                       deps: list[Path] | None, where: str) -> JsonDict:
   """!A font asset (I4): a proportional font for the game's code page.
   @param a         the asset
   @param base      the manifest's folder
   @param codepage  the game's code page
   @param deps      collects the font file, or None
   @param where     location for error messages
   @return compile_proportional() result
   """
   import font_compiler
   if not isinstance(a.get("font"), str):
      raise ManifestError(f"{where}: a font asset names its capsule.font/2 file")
   path = Path(base or ".") / a["font"]
   try:
      font = font_compiler.compile_proportional(path, codepage)
   except text_encoder.TextError as e:
      raise ManifestError(f"{where}: {e}") from None
   if deps is not None:
      deps.append(path.resolve())
   return font


def door_sheet_tiles(sheet: Any, base: str | PathLike[str] | None, deps: list[Path] | None,
                     where: str) -> list[Tile]:
   """!DS1: a door's sheet as tiles in reading order: an indexed palette PNG ({"image", "indexed": true}: its palette
   indices as they are) or shapes drawn with palette indices 1-15 of the map bank the entries name ({"size": [w, h],
   "shapes": [...]}).
   @param sheet  the sheet object
   @param base   manifest directory
   @param deps   list the image path is appended to, or None
   @param where  location for error messages
   @return its tiles
   """
   if not isinstance(sheet, dict):
      raise ManifestError(f"{where}: sheet is an indexed image or shapes")
   check_keys(sheet, DOOR_SHEET_KEYS, where)
   if "image" in sheet:
      if sheet.get("indexed") is not True or "shapes" in sheet or "size" in sheet:
         raise ManifestError(f"{where}: an image sheet is indexed (\"indexed\": true) and has no size or shapes")
      path = sheet["image"]
      if not isinstance(path, str) or manifest_paths.is_absolute_text(path) or "\\" in path or base is None:
         raise ManifestError(f"{where}: image paths are relative, with '/' separators: {path!r}")
      full = (Path(base) / path).resolve()
      if deps is not None:
         deps.append(full)
      try:
         w, h, px = bitmap.read_png_indices(full)
      except bitmap.BitmapError as e:
         raise ManifestError(f"{where}: {e}") from None
      if w % TILE_SIZE or h % TILE_SIZE or (px and max(px) >= PALETTE_ENTRIES):
         raise ManifestError(f"{where}: {path} must be multiples of 8 pixels with palette indices 0-15")
      canvas = Canvas(w, h)
      canvas.px = [list(px[y * w:(y + 1) * w]) for y in range(h)]
   else:
      size = sheet.get("size")
      if not isinstance(size, list) or len(size) != IMAGE_SIZE_FIELDS or any(
            not isinstance(v, int) or v <= 0 or v % TILE_SIZE for v in size):
         raise ManifestError(f"{where}: a shapes sheet needs \"size\": [w, h], multiples of 8 pixels")
      canvas = Canvas(size[0], size[1])
      for i, s in enumerate(sheet.get("shapes", [])):
         draw(canvas, s, bitmap.MAX_COLORS, f"{where} shape {i}")
   return [tile_at(canvas, tx, ty) for ty in range(canvas.h // TILE_SIZE) for tx in range(canvas.w // TILE_SIZE)]


def door_entry(value: Any, load_at: int | None, frame_tiles: int, where: str) -> int:
   """!DS1: one map entry of a door's cell: a raw map entry (an integer) or, for a tiles door, a frame tile
   ({"tile": k, "palette": bank, "flip": "h" | "v" | "hv"}: the frame's tile k, loaded at load_at + k).
   @param value        the entry
   @param load_at      a tiles door's first map tile, or None (an override door: raw entries only)
   @param frame_tiles  tiles per frame
   @param where        location for error messages
   @return the 16-bit map entry
   """
   if isinstance(value, int) and not isinstance(value, bool):
      if not 0 <= value <= MAX_MAP_ENTRY:
         raise ManifestError(f"{where}: a map entry is 0-{MAX_MAP_ENTRY}")
      return value
   if load_at is None or not isinstance(value, dict):
      raise ManifestError(f"{where}: an entry is a map entry" + (" or {\"tile\", \"palette\", \"flip\"}"
                                                                 if load_at is not None else ""))
   check_keys(value, DOOR_ENTRY_KEYS, where)
   k = value.get("tile")
   if not isinstance(k, int) or isinstance(k, bool) or not 0 <= k < frame_tiles:
      raise ManifestError(f"{where}: tile is one of the frame's tiles, 0-{frame_tiles - 1}")
   bank = value.get("palette", 0)
   if not isinstance(bank, int) or isinstance(bank, bool) or not 0 <= bank <= MAX_ENTRY_PALETTE:
      raise ManifestError(f"{where}: palette is a map palette bank 0-{MAX_ENTRY_PALETTE}")
   flip = value.get("flip", "")
   if flip not in ENTRY_FLIPS:
      raise ManifestError(f"{where}: flip is h, v or hv")
   return (load_at + k) | ENTRY_FLIPS[flip] | bank << ENTRY_PALETTE_SHIFT


def door_cells(raw: Any, cells: int, layers: int, load_at: int | None, frame_tiles: int, where: str) -> list[int]:
   """!DS1: one entry set of a door: per cell (top row first, left to right) per layer four map entries.
   @param raw          [cells: [layers: [4 entries]]]
   @param cells        width x height
   @param layers       tile layers per cell
   @param load_at      a tiles door's first map tile, or None
   @param frame_tiles  tiles per frame
   @param where        location for error messages
   @return the entries, flat
   """
   if not isinstance(raw, list) or len(raw) != cells:
      raise ManifestError(f"{where}: needs {cells} cells (top row first, left to right)")
   out = []
   for c, cell in enumerate(raw):
      if not isinstance(cell, list) or len(cell) != layers:
         raise ManifestError(f"{where} cell {c}: needs {layers} layers of {CELL_ENTRIES} entries")
      for k, layer in enumerate(cell):
         if not isinstance(layer, list) or len(layer) != CELL_ENTRIES:
            raise ManifestError(f"{where} cell {c} layer {k}: needs {CELL_ENTRIES} entries (top-left, top-right, "
                                "bottom-left, bottom-right)")
         out += [door_entry(v, load_at, frame_tiles, f"{where} cell {c} layer {k}") for v in layer]
   return out


def door_steps(raw: Any, frames: int, where: str) -> list[tuple[int, int]]:
   """!DS1: a door's opening or closing: [[frame or "closed", ticks], ...] (ticks: display frames it shows).
   @param raw     the steps
   @param frames  the door's frames
   @param where   location for error messages
   @return [(frame or DOOR_CLOSED, ticks)]
   """
   if not isinstance(raw, list) or not raw or len(raw) > MAX_ANIM_FRAMES:
      raise ManifestError(f"{where}: needs 1-{MAX_ANIM_FRAMES} steps [frame or \"closed\", ticks]")
   out = []
   for i, s in enumerate(raw):
      if not isinstance(s, list) or len(s) != IMAGE_SIZE_FIELDS:
         raise ManifestError(f"{where} step {i}: a step is [frame or \"closed\", ticks]")
      frame, ticks = s
      if frame == DOOR_CLOSED_WORD:
         frame = DOOR_CLOSED
      elif not isinstance(frame, int) or isinstance(frame, bool) or not 0 <= frame < frames:
         raise ManifestError(f"{where} step {i}: frame is 0-{frames - 1} or \"closed\"")
      if not isinstance(ticks, int) or isinstance(ticks, bool) or not 1 <= ticks <= MAX_FRAME_DURATION:
         raise ManifestError(f"{where} step {i}: ticks is 1-{MAX_FRAME_DURATION} display frames")
      out.append((frame, ticks))
   return out


def compile_door(a: JsonDict, base: str | PathLike[str] | None, deps: list[Path] | None, where: str) -> JsonDict:
   """!DS1 (D-30 section 1): a door animation (asset::Door). Keys: "cells": [width, height] in metatiles (its anchor,
   the cell a map binds it to, is the bottom-left one); "show": "tiles" (the frames' tiles load into the map's tile
   area at "load_at", "end" by default: the area's last tiles) or "override" (each frame is map entries over the
   cells); "layers": tile layers per cell (default 1; a map's other layers keep the cell's own metatiles); tiles:
   "sheet" (an indexed PNG or shapes) and "frame_tiles" (frame k is the sheet's tiles k * frame_tiles ...),
   "entries" (the cells' entries, frame tiles by index); override: "frames" (per frame the cells' raw entries);
   "open": [[frame or "closed", ticks], ...], "close": the same or "reverse" (the open steps backwards); "sounds":
   {"open", "close"} sound asset names; "exit": the direction a walker arriving on it walks out (default down).
   @param a      the asset
   @param base   manifest directory
   @param deps   list image paths are appended to, or None
   @param where  location for error messages
   @return the compiled door
   """
   cells = a.get("cells", [1, 1])
   if not isinstance(cells, list) or len(cells) != IMAGE_SIZE_FIELDS or any(
         not isinstance(v, int) or isinstance(v, bool) or not 1 <= v <= MAX_DOOR_CELLS for v in cells):
      raise ManifestError(f"{where}: cells is [width, height], 1-{MAX_DOOR_CELLS} metatiles each")
   width, height = cells
   show = a.get("show", DOOR_SHOWS[0])
   if show not in DOOR_SHOWS:
      raise ManifestError(f"{where}: show is tiles or override")
   layers = a.get("layers", 1)
   if not isinstance(layers, int) or isinstance(layers, bool) or not 1 <= layers <= MAX_MAP_LAYERS:
      raise ManifestError(f"{where}: layers is 1-{MAX_MAP_LAYERS}")
   tiles: list[int] = []
   if show == "tiles":
      if "frames" in a:
         raise ManifestError(f"{where}: a tiles door gives a sheet and entries (frames are an override door's)")
      sheet = door_sheet_tiles(a.get("sheet"), base, deps, f"{where} sheet")
      frame_tiles = a.get("frame_tiles")
      if not isinstance(frame_tiles, int) or isinstance(frame_tiles, bool) or not 1 <= frame_tiles <= MAP_TILES:
         raise ManifestError(f"{where}: frame_tiles is the tiles of one frame")
      if len(sheet) % frame_tiles or len(sheet) // frame_tiles > MAX_ANIM_FRAMES:
         raise ManifestError(f"{where}: the sheet's {len(sheet)} tiles are not whole frames of {frame_tiles}")
      frames = len(sheet) // frame_tiles
      load = a.get("load_at", DOOR_LOAD_END)
      load_at = MAP_TILES - frame_tiles if load == DOOR_LOAD_END else load
      if not isinstance(load_at, int) or isinstance(load_at, bool) or not 0 <= load_at <= MAP_TILES - frame_tiles:
         raise ManifestError(f"{where}: load_at is \"end\" or a map tile 0-{MAP_TILES - frame_tiles}")
      entries = door_cells(a.get("entries"), width * height, layers, load_at, frame_tiles, f"{where} entries")
      tiles = [w for tile in sheet for w in words_4bpp(tile)]
      entry_frames = 1
   else:
      for k in ("sheet", "frame_tiles", "load_at", "entries"):
         if k in a:
            raise ManifestError(f"{where}: {k} is a tiles door's (an override door lists frames)")
      raw = a.get("frames")
      if not isinstance(raw, list) or not raw or len(raw) > MAX_ANIM_FRAMES:
         raise ManifestError(f"{where}: frames lists 1-{MAX_ANIM_FRAMES} frames of the cells' map entries")
      frames, frame_tiles, load_at = len(raw), 0, 0
      entries = [e for i, f in enumerate(raw) for e in door_cells(f, width * height, layers, None, 0,
                                                                  f"{where} frame {i}")]
      entry_frames = frames
   opening = door_steps(a.get("open"), frames, f"{where} open")
   closing = list(reversed(opening)) if a.get("close", DOOR_REVERSE) == DOOR_REVERSE else \
      door_steps(a.get("close"), frames, f"{where} close")
   sounds = a.get("sounds", {})
   if not isinstance(sounds, dict):
      raise ManifestError(f"{where}: sounds is {{\"open\", \"close\"}}")
   check_keys(sounds, DOOR_SOUND_KEYS, f"{where} sounds")
   keys = []
   for k in ("open", "close"):
      name = sounds.get(k)
      try:
         keys.append((asset_key(name, f"{where} sounds {k}"), name) if name is not None else (0, None))
      except SequenceManifestError as e:
         raise ManifestError(str(e)) from None
   exit_dir = a.get("exit", DIRECTIONS[0])
   if exit_dir not in DIRECTIONS:
      raise ManifestError(f"{where}: exit is one of {', '.join(DIRECTIONS)}")
   return {"tiles": tiles, "entries": entries, "open": opening, "close": closing, "sounds": keys,
           "load_at": load_at, "frames": frames, "frame_tiles": frame_tiles, "width": width, "height": height,
           "layers": layers, "entry_frames": entry_frames, "show": DOOR_SHOWS.index(show),
           "exit": DIRECTIONS.index(exit_dir), "key": a.get("key")}


def expand_grid(a: JsonDict, where: str) -> tuple[JsonDict, int, list[str]]:
   """!DS1 (D-30 section 3): a direction-grid sprite ("layout": "direction_grid") as a plain sprite. Its "grid" is a
   sheet ({"image", "indexed": true}: frames of the sprite's size, "columns" across, the rows down) or rows of
   frames ([[down, up, left(, right)], ...], each a frame object); columns are the directions down, up, left and the
   optional right (three columns: right is left mirrored horizontally); "rows" names the rows (every direction has
   the same rows). Each animation {"rows": [row names], "period" | "durations", "loop"} becomes four (down, up, left,
   right, the field's set order), or one with "direction".
   @param a      the asset
   @param where  location for error messages
   @return (the plain sprite asset, its columns, its row names)
   """
   for k in ("frames",):
      if k in a:
         raise ManifestError(f"{where}: a direction grid's frames are its grid")
   rows = a.get("rows")
   if not isinstance(rows, list) or not rows or not all(isinstance(r, str) and r for r in rows) or \
         len(set(rows)) != len(rows):
      raise ManifestError(f"{where}: rows names the grid's rows, each once")
   grid = a.get("grid")
   cells: list[list[JsonDict]] = []
   if isinstance(grid, dict):
      columns = a.get("columns", GRID_MIRRORED_COLUMNS)
      if columns not in (GRID_MIRRORED_COLUMNS, len(DIRECTIONS)):
         raise ManifestError(f"{where}: columns is 3 (right mirrors left) or 4")
      check_keys(grid, {"image", "indexed"}, f"{where} grid")
      if grid.get("indexed") is not True:
         raise ManifestError(f"{where} grid: a grid sheet is an indexed image (\"indexed\": true)")
      cells = [[{"image": grid.get("image"), "frame": r * columns + c, "indexed": True} for c in range(columns)]
               for r in range(len(rows))]
   elif isinstance(grid, list) and len(grid) == len(rows):
      columns = len(grid[0]) if isinstance(grid[0], list) else 0
      if columns not in (GRID_MIRRORED_COLUMNS, len(DIRECTIONS)) or a.get("columns", columns) != columns or \
            any(not isinstance(r, list) or len(r) != columns for r in grid):
         raise ManifestError(f"{where}: every grid row has 3 frames (down, up, left) or 4 (and right)")
      cells = [list(r) for r in grid]
   else:
      raise ManifestError(f"{where}: grid is an indexed sheet or one row of frames per row name")
   frames = [f for r in cells for f in r]

   def frame_of(direction: int, row: int) -> tuple[int, int]:
      """!The plain sprite's frame and flip of a grid cell (right from left, mirrored, without a right column)."""
      if direction == len(DIRECTIONS) - 1 and columns == GRID_MIRRORED_COLUMNS:
         return row * columns + MIRRORED_FROM, 1
      return row * columns + direction, 0

   anims = []
   for i, an in enumerate(a.get("animations", [{"rows": [rows[0]], "period": 1}])):
      aw = f"{where} animation {i}"
      check_keys(an, GRID_ANIM_KEYS, aw)
      names = an.get("rows")
      if not isinstance(names, list) or not names or any(n not in rows for n in names):
         raise ManifestError(f"{aw}: rows lists row names of the grid ({', '.join(rows)})")
      if "direction" in an and an["direction"] not in DIRECTIONS:
         raise ManifestError(f"{aw}: direction is one of {', '.join(DIRECTIONS)}")
      dirs = [DIRECTIONS.index(an["direction"])] if "direction" in an else range(len(DIRECTIONS))
      for d in dirs:
         cell = [frame_of(d, rows.index(n)) for n in names]
         out = {k: v for k, v in an.items() if k in ("period", "durations", "loop")}
         out["frames"] = [f for f, _ in cell]
         if any(flip for _, flip in cell):
            out["flips"] = [flip for _, flip in cell]
         anims.append(out)
   plain = {k: v for k, v in a.items() if k not in ("layout", "grid", "rows", "columns")}
   plain["frames"] = frames
   plain["animations"] = anims
   return plain, columns, rows


def dedup_frames(s: JsonDict) -> int:
   """!DS1: merges a compiled sprite's identical frames (pixel for pixel), renumbering its animations.
   @param s  compile_sprite() result (changed in place)
   @return frames dropped
   """
   per = len(s["tiles"]) // s["frames"] if s["frames"] else 0
   index: dict[tuple[int, ...], int] = {}
   remap, words = [], []
   for f in range(s["frames"]):
      key = tuple(s["tiles"][f * per:(f + 1) * per])
      if key not in index:
         index[key] = len(index)
         words.extend(key)
      remap.append(index[key])
   dropped = s["frames"] - len(index)
   s["tiles"] = words
   s["frames"] = len(index)
   s["animations"] = [([remap[f] for f in fr], period, loop, durations, flips)
                      for fr, period, loop, durations, flips in s["animations"]]
   if not s["stream"]:
      s["tiles_total"] = len(words) // WORDS_4BPP
   return dropped


def compile_grid_sprite(a: JsonDict, named: Mapping[str, Any], where: str, base: str | PathLike[str] | None = None,
                        deps: list[Path] | None = None, images: Mapping[str, bitmap.Image] | None = None) -> JsonDict:
   """!DS1: a direction-grid sprite (expand_grid()), its identical frames merged; the result also says what the
   omitted right column and the merged frames save ("grid_saved", bytes).
   @param a       the asset
   @param named   named palettes
   @param where   location for error messages
   @param base    manifest directory
   @param deps    list image paths are appended to, or None
   @param images  in-memory images by manifest path, or None
   @return compile_sprite()'s result with "grid_saved", "rows" and "columns"
   """
   plain, columns, rows = expand_grid(a, where)
   s = compile_sprite(plain, named, where, base, deps, images)
   frame_bytes = (len(s["tiles"]) // s["frames"]) * WORD_BYTES if s["frames"] else 0
   dropped = dedup_frames(s)
   mirrored = len(rows) if columns == GRID_MIRRORED_COLUMNS else 0   # the right frames never stored
   s["grid_saved"] = (dropped + mirrored) * frame_bytes
   s["rows"] = rows
   s["columns"] = columns
   return s


## UC: the screen graphics kinds' asset::Kind numbers.
SCREEN_KINDS = {"tiles": screen_assets.KIND_TILES, "tilemap": screen_assets.KIND_TILEMAP,
                "palette": screen_assets.KIND_PALETTE}


def shapes_canvas(w: int, h: int, fill: int, shapes: list[Any], highest: int, where: str) -> list[list[int]]:
   """!UC: shapes drawn with palette indices on a canvas (a tiles asset's "shapes").
   @param w        width, pixels
   @param h        height, pixels
   @param fill     the canvas's first index
   @param shapes   the shapes
   @param highest  highest palette index
   @param where    location for errors
   @return h rows of w indices
   """
   canvas = Canvas(w, h, fill)
   for i, s in enumerate(shapes):
      draw(canvas, s, highest, f"{where} shape {i}")
   return canvas.px


def compile_screen_asset(kind: str, a: JsonDict, base: str | PathLike[str] | None, deps: list[Path] | None,
                         where: str) -> JsonDict:
   """!UC: one tiles / tilemap / palette asset (tools/screen_assets.py).
   @param kind   "tiles", "tilemap" or "palette"
   @param a      the asset
   @param base   the manifest's directory
   @param deps   dependency list, or None
   @param where  location for errors
   @return the compiled asset
   """
   try:
      if kind == "tiles":
         return screen_assets.compile_tiles(a, base, deps, where, shapes_canvas)
      if kind == "tilemap":
         return screen_assets.compile_tilemap(a, base, deps, where)
      return screen_assets.compile_palette(a, where, bgr555)
   except screen_assets.ScreenAssetError as e:
      raise ManifestError(str(e)) from None


def compile_manifest(manifest: JsonDict, base: str | PathLike[str] | None = None, deps: list[Path] | None = None,
                     images: Mapping[str, bitmap.Image] | None = None, *,
                     codepage: text_encoder.CodePage = text_encoder.DEFAULT, field: Any = None,
                     language: str | None = None
                     ) -> tuple[list[Entry], list[JsonDict], list[JsonDict], list[list[int]],
                                list[tuple[list[int], list[Any]]]]:
   """!Compiles every asset.
   @param manifest  parsed JSON manifest
   @param base      directory image paths are relative to (the manifest's);
                    None when the manifest has no image frames
   @param deps      list the image files read are appended to, or None
   @param images    in-memory images by manifest path, used instead of the
                    files (an editor's unsaved frames); or None
   @param codepage  code page of the text assets (the game's language)
   @param field     the game's field placeholders (script_compiler.load_placeholders()): text assets are field
                    text; or None (braces are plain characters)
   @param language  C10: the language whose text tables the build takes (None: the manifest's "language", or the
                    one language its tables have; select_language())
   @return (sorted entries [(key, kind, index, name)], backgrounds, sprites, texts, lists); I4: the fonts and the
           fit report are in compile_manifest.fonts / compile_manifest.fit (the last call's)
   """
   if manifest.get("schema") != "capsule.assets/1":
      raise ManifestError("schema must be 'capsule.assets/1'")
   m, table_texts = expand_manifest(manifest, Path(base or "."), deps)
   table_texts = select_language(table_texts, m, language)
   named = m.get("palettes", {})
   entries, bgs, sprites, texts, lists, seen = [], [], [], [], [], {}
   fonts: list[JsonDict] = []
   fit: JsonDict = {}
   for key_text, codes in compile_table_texts(table_texts, codepage, field, fit, deps):
      try:
         key = asset_key(key_text, key_text)
      except SequenceManifestError as e:
         raise ManifestError(str(e)) from None
      if key in seen:
         raise ManifestError(f"{key_text}: key collides with {seen[key]!r}")
      seen[key] = key_text
      entries.append((key, KIND_TEXT, len(texts), key_text))
      texts.append(codes)
   compile_manifest.fit = fit
   compile_manifest.fonts = fonts
   doors: list[JsonDict] = []
   compile_manifest.doors = doors   # DS1
   text_boxes: list[JsonDict] = []
   compile_manifest.text_boxes = text_boxes   # C10
   screen: dict[str, list[JsonDict]] = {"tiles": [], "tilemap": [], "palette": []}
   compile_manifest.screen = screen   # UC: the screen graphics kinds
   for n, a in enumerate(m.get("assets", [])):
      where = f"asset {n} ({a.get('key')!r})"
      kind = a.get("kind")
      if kind not in KIND_KEYS:
         raise ManifestError(f"{where}: kind must be one of {', '.join(KIND_KEYS)}")
      check_keys(a, KIND_KEYS[kind], where)
      try:
         key = asset_key(a.get("key"), where)
      except SequenceManifestError as e:
         raise ManifestError(str(e)) from None
      if key in seen:
         raise ManifestError(f"{where}: key collides with {seen[key]!r}")
      seen[key] = a.get("key")
      if kind == "background":
         entries.append((key, KIND_BACKGROUND, len(bgs), a["key"]))
         forms = compile_background(a, named, where)
         if len(forms) == BOTH_FORMS:   # the second form follows the first; they point at each other
            forms[0]["other"], forms[1]["other"] = len(bgs) + 1, len(bgs)
         bgs.extend(forms)
      elif kind == "sprite":
         entries.append((key, KIND_SPRITE, len(sprites), a["key"]))
         layout = a.get("layout")
         if layout is not None and layout != DIRECTION_GRID:
            raise ManifestError(f"{where}: layout is {DIRECTION_GRID!r} (or left out)")
         for k in ("grid", "rows", "columns"):
            if layout is None and k in a:
               raise ManifestError(f"{where}: {k} belongs to a direction grid (\"layout\": \"{DIRECTION_GRID}\")")
         sprites.append(compile_grid_sprite(a, named, where, base, deps, images) if layout == DIRECTION_GRID else
                        compile_sprite(a, named, where, base, deps, images))
      elif kind == "door":
         entries.append((key, KIND_DOOR, len(doors), a["key"]))
         doors.append(compile_door(a, base, deps, where))
      elif kind == "text":
         entries.append((key, KIND_TEXT, len(texts), a["key"]))
         texts.append(encode_text(a.get("text"), where, codepage, field))
      elif kind == "font":
         entries.append((key, KIND_FONT, len(fonts), a["key"]))
         fonts.append(compile_font_asset(a, base, codepage, deps, where))
      elif kind == "text_box":
         entries.append((key, KIND_TEXT_BOX, len(text_boxes), a["key"]))
         text_boxes.append(compile_text_box(a, where, base, deps))
      elif kind in screen:
         entries.append((key, SCREEN_KINDS[kind], len(screen[kind]), a["key"]))
         screen[kind].append(compile_screen_asset(kind, a, base, deps, where))
      else:
         items = a.get("items")
         if not isinstance(items, list) or not items or len(items) > MAX_LIST_ITEMS:
            raise ManifestError(f"{where}: a list needs 1-65535 asset keys")
         try:
            keys = [asset_key(item, f"{where} item {i}") for i, item in enumerate(items)]
         except SequenceManifestError as e:
            raise ManifestError(str(e)) from None
         entries.append((key, KIND_LIST, len(lists), a["key"]))
         lists.append((keys, items))
   entries.sort()
   if text_boxes:
      # C10: the styles' fonts must be font assets of this table (checked now, emitted with the table)
      text_box_lines(text_boxes, {k: i for k, kind, i, _ in entries if kind == KIND_FONT})
   return entries, bgs, sprites, texts, lists


def emit(src_name: str, entries: Sequence[Entry], bgs: Sequence[JsonDict], sprites: Sequence[JsonDict],
         texts: Sequence[Sequence[int]], lists: Sequence[tuple[Sequence[int], Sequence[Any]]],
         fonts: Sequence[JsonDict] = (), doors: Sequence[JsonDict] = (),
         text_boxes: Sequence[JsonDict] = (), screen: Mapping[str, Sequence[JsonDict]] | None = None) -> str:
   """!Generates the .cpp defining asset::kTable.
   @param src_name  manifest file name, for the header comment
   @param entries   sorted (key, kind, index, name)
   @param bgs       compiled backgrounds
   @param sprites   compiled sprites
   @param texts     encoded texts
   @param lists     (keys, names) per list
   @param fonts     compiled proportional fonts (I4)
   @param doors     compiled door animations (DS1)
   @param text_boxes  compiled text box styles (C10)
   @param screen    UC: compiled screen graphics by kind ("tiles", "tilemap", "palette"), or None
   @return the source text
   """
   L = [f"// Generated by tools/asset_compiler.py from {src_name}. Do not edit.",
        '#include "capsule/asset/asset.h"',
        '#include "capsule/gfx/background.h"',
        '#include "capsule/gfx/sprites.h"',
        "",
        "namespace capsule::asset {",
        "namespace {",
        "",
        "using hal::video::Color;",
        ""]

   def arr(ctype: str, name: str, values: Sequence[int], per_line: int = WIDE_VALUES_PER_LINE,
           fmt: str = "0x{:08X}") -> None:
      """!Appends a C++ array definition.
      @param ctype     element type
      @param name      array name
      @param values    elements
      @param per_line  elements per source line
      @param fmt       format of one element
      """
      L.append(f"alignas(4) const {ctype} {name}[] = {{")
      for i in range(0, len(values), per_line):
         L.append("    " + ", ".join(fmt.format(v) for v in values[i:i + per_line]) + ",")
      L.append("};")

   for i, b in enumerate(bgs):
      arr("uint32_t", f"kBgTiles{i}", b["tiles"])
      if b["affine"]:
         arr("uint8_t", f"kBgAffineMap{i}", b["affine_map"], NARROW_VALUES_PER_LINE, "{}")
         limit = "(gfx::background::kMaxAffineTiles * 2)"
         L.append(f"static_assert({b['count']} <= {limit}, \"affine background {i}: too many tiles\");")
      else:
         arr("uint16_t", f"kBgMap{i}", b["map"], NARROW_VALUES_PER_LINE, "0x{:04X}")
         L.append(f"static_assert({b['count']} <= gfx::background::kMaxTiles, "
                  f"\"background {i}: too many unique tiles\");")
      arr("Color", f"kBgPalette{i}", b["palette"], NARROW_VALUES_PER_LINE, "0x{:04X}")
   for i, s in enumerate(sprites):
      arr("uint32_t", f"kSprTiles{i}", s["tiles"])
      arr("Color", f"kSprPalette{i}", s["palette"], NARROW_VALUES_PER_LINE, "0x{:04X}")
      for j, (fr, _, _, durations, flips) in enumerate(s["animations"]):
         arr("uint8_t", f"kSprAnimFrames{i}_{j}", fr, NARROW_VALUES_PER_LINE, "{}")
         if durations is not None:
            arr("uint8_t", f"kSprAnimDurations{i}_{j}", durations, NARROW_VALUES_PER_LINE, "{}")
         if flips is not None:
            arr("uint8_t", f"kSprAnimFlips{i}_{j}", flips, NARROW_VALUES_PER_LINE, "{}")
      if s.get("steps") is not None:
         arr("uint8_t", f"kSprSteps{i}", s["steps"], NARROW_VALUES_PER_LINE, "{}")
      L.append(f"const Animation kSprAnims{i}[] = {{")
      for j, (fr, period, loop, durations, flips) in enumerate(s["animations"]):
         dur = f"kSprAnimDurations{i}_{j}" if durations is not None else "nullptr"
         # I3: the flips only when given (the initializer of before stays valid)
         flip = f", kSprAnimFlips{i}_{j}" if flips is not None else ""
         L.append(f"    {{kSprAnimFrames{i}_{j}, {dur}, {len(fr)}, {period}, {loop}{flip}}},")
      L.append("};")
      L.append(f"static_assert({s['tiles_total']} <= gfx::sprites::kTilesPerSlot, "
               f"\"sprite {i}: frames use too many tiles\");")
   for i, t in enumerate(texts):
      arr("text::Code", f"kText{i}", t, NARROW_VALUES_PER_LINE, "0x{:02X}")
   for i, (keys, names) in enumerate(lists):
      L.append(f"const AssetKey kListItems{i}[] = {{")
      L.extend(f"    AssetKey{{0x{k:08X}u}},   // {n}" for k, n in zip(keys, names))
      L.append("};")
   L.append("")
   L.append(f"const Background kBackgrounds[{max(1, len(bgs))}] = {{")
   for i, b in enumerate(bgs):
      tmap = "nullptr" if b["affine"] else f"kBgMap{i}"
      amap = f"kBgAffineMap{i}" if b["affine"] else "nullptr"
      other = f"&kBackgrounds[{b['other']}]" if "other" in b else "nullptr"
      L.append(f"    {{{other}, kBgTiles{i}, {tmap}, {amap}, kBgPalette{i}, {b['count']}, {b['width']}, "
               f"{b['height']}, {b['affine']}, {b['priority']}}},")
   if not bgs:
      L.append("    {nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0, 0},")
   L.append("};")
   L.append("const Sprite kSprites[] = {")
   for i, s in enumerate(sprites):
      extra = ""
      if s.get("stream") or s.get("steps") is not None:   # I3: flags and steps only when set
         flags = "kSpriteStreamed" if s.get("stream") else "0"
         steps = f"kSprSteps{i}" if s.get("steps") is not None else "nullptr"
         extra = f", {flags}, {steps}"
      L.append(f"    {{kSprTiles{i}, kSprPalette{i}, kSprAnims{i}, hal::video::{OBJ_SHAPE_NAMES[s['shape']]}, "
               f"{s['size']}, {s['frames']}, {len(s['animations'])}{extra}}},")
   if not sprites:
      L.append("    {nullptr, nullptr, nullptr, hal::video::OBJ_SQUARE, 0, 0, 0},")
   L.append("};")
   L.append("const text::Code* const kTexts[] = {")
   L.extend(f"    kText{i}," for i in range(len(texts)))
   if not texts:
      L.append("    nullptr,")
   L.append("};")
   L.append("const List kLists[] = {")
   L.extend(f"    {{kListItems{i}, {len(keys)}}}," for i, (keys, _) in enumerate(lists))
   if not lists:
      L.append("    {nullptr, 0},")
   L.append("};")
   for i, f in enumerate(fonts):
      arr("uint8_t", f"kFontGlyphs{i}", [b for g in f["glyphs"] for b in g], NARROW_VALUES_PER_LINE, "0x{:02X}")
      arr("uint8_t", f"kFontWidths{i}", f["widths"], NARROW_VALUES_PER_LINE, "{}")
   if fonts:
      L.append("const text::PropFont kFonts[] = {")
      for i, f in enumerate(fonts):
         cw, ch = f["cell"]
         L.append(f"    {{{{0x{f['first']:02X}, {f['count']}, {cw}, {ch}}}, kFontGlyphs{i}, kFontWidths{i}, "
                  f"{f['line_height']}, {f['letter_spacing']}, {f.get('glyph_height', 0)}, 0}},")
      L.append("};")
   for i, d in enumerate(doors):   # DS1
      if d["tiles"]:
         arr("uint32_t", f"kDoorTiles{i}", d["tiles"])
      arr("uint16_t", f"kDoorEntries{i}", d["entries"], NARROW_VALUES_PER_LINE, "0x{:04X}")
      for part in ("open", "close"):
         L.append(f"const DoorStep kDoor{part.title()}{i}[] = {{" +
                  ", ".join(f"{{{'kDoorClosed' if f == DOOR_CLOSED else f}, {n}}}" for f, n in d[part]) + "};")
   if doors:
      L.append("const Door kDoors[] = {")
      for i, d in enumerate(doors):
         tiles = f"kDoorTiles{i}" if d["tiles"] else "nullptr"
         sounds = ", ".join(f"AssetKey{{0x{k:08X}u}}" for k, _ in d["sounds"])
         L.append(f"    {{{tiles}, kDoorEntries{i}, kDoorOpen{i}, kDoorClose{i}, {sounds}, {d['load_at']}, "
                  f"{d['frames']}, {d['frame_tiles']}, {d['width']}, {d['height']}, {d['layers']}, "
                  f"{d['entry_frames']}, {len(d['open'])}, {len(d['close'])}, "
                  f"DoorShow::{DOOR_SHOWS[d['show']].title()}, {d['exit']}}},   // {d['key']}")
      L.append("};")
   L += text_box_lines(text_boxes, {k: i for k, kind, i, _ in entries if kind == KIND_FONT})   # C10
   scr = screen or {}
   tiles, tilemaps, palettes = scr.get("tiles", ()), scr.get("tilemap", ()), scr.get("palette", ())
   L += screen_assets.emit_lines(tiles, tilemaps, palettes)   # UC
   kinds = ["Kind::Background", "Kind::Sprite", "Kind::Text", "Kind::List", "Kind::Font", "Kind::Door",
            "Kind::TextBox", *screen_assets.KIND_NAMES]
   L.append("const Entry kEntries[] = {")
   for key, kind, index, name in entries:
      L.append(f"    {{AssetKey{{0x{key:08X}u}}, {kinds[kind]}, 0, {index}}},   // {name}")
   if not entries:
      L.append("    {AssetKey{0}, Kind::Text, 0, 0},")
   L.append("};")
   font_table = ", kFonts" if fonts else ""   # I4: the fonts only when there are some (the old initializer stays)
   if doors or text_boxes or tiles or tilemaps or palettes:   # DS1: the doors likewise (C10: text box styles)
      font_table = (font_table or ", nullptr") + (", kDoors" if doors else ", nullptr")
   if text_boxes or tiles or tilemaps or palettes:
      font_table += ", kTextBoxes" if text_boxes else ", nullptr"
   if tiles or tilemaps or palettes:   # UC: the screen graphics tables only when there are some
      font_table += ", kTileBlocks" if tiles else ", nullptr"
      font_table += ", kTilemaps" if tilemaps else ", nullptr"
      font_table += ", kPalettes" if palettes else ", nullptr"
   L += ["", "} // namespace", "",
         f"const Table kTable = {{kEntries, kBackgrounds, kSprites, kTexts, kLists, {len(entries)}{font_table}}};",
         "", "} // namespace capsule::asset", ""]
   return "\n".join(L)


def main(argv: list[str]) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return process exit status
   """
   args = list(argv[1:])
   depfile = None
   if "--depfile" in args:
      i = args.index("--depfile")
      if i + 1 >= len(args):
         print(__doc__)
         return EXIT_USAGE
      depfile = Path(args[i + 1])
      del args[i:i + OPTION_WITH_VALUE]
   fit_report = None
   if "--fit-report" in args:
      i = args.index("--fit-report")
      if i + 1 >= len(args):
         print(__doc__)
         return EXIT_USAGE
      fit_report = Path(args[i + 1])
      del args[i:i + OPTION_WITH_VALUE]
   language = None
   if "--language" in args:   # C10: the build's language of the text tables
      i = args.index("--language")
      if i + 1 >= len(args):
         print(__doc__)
         return EXIT_USAGE
      language = args[i + 1]
      del args[i:i + OPTION_WITH_VALUE]
   scripts = None
   if "--scripts" in args:
      i = args.index("--scripts")
      if i + 1 >= len(args):
         print(__doc__)
         return EXIT_USAGE
      scripts = Path(args[i + 1])
      del args[i:i + OPTION_WITH_VALUE]
   try:
      codepage = text_encoder.option(args)
   except text_encoder.TextError as e:
      print(f"error: {e}", file=sys.stderr)
      return 1
   if len(args) != POSITIONAL_ARGUMENTS:
      print(__doc__)
      return EXIT_USAGE
   src, dst = Path(args[0]), Path(args[1])
   deps = []
   field = None
   if scripts is not None:
      import script_compiler as sc
      try:
         field = sc.load_placeholders(scripts, codepage)
      except sc.ScriptError as e:
         print(f"{scripts}: error: {e.message}", file=sys.stderr)
         return 1
      deps.append(scripts.resolve())
   try:
      entries, bgs, sprites, texts, lists = compile_manifest(json.loads(src.read_text(encoding="utf-8")),
                                                             src.resolve().parent, deps, codepage=codepage,
                                                             field=field, language=language)
   except (ManifestError, json.JSONDecodeError, KeyError, TypeError, ValueError) as e:
      print(f"{src}: error: {e}", file=sys.stderr)
      return 1
   fit = compile_manifest.fit
   if fit.get("checked"):
      # I4: authored text against its box: one line here, the details in the fit report
      print(f"{src}: {fit['checked']} authored texts checked against their box: {fit['over']} lines wider "
            f"({fit['with_placeholders']} lines with placeholders measured without them)")
   if fit_report is not None:
      fit_report.parent.mkdir(parents=True, exist_ok=True)
      fit_report.write_text(json.dumps(fit, indent=1) + "\n", encoding="utf-8", newline="\n")
   text = emit(src.name, entries, bgs, sprites, texts, lists, compile_manifest.fonts, compile_manifest.doors,
               compile_manifest.text_boxes, compile_manifest.screen)
   dst.parent.mkdir(parents=True, exist_ok=True)
   if not dst.exists() or dst.read_bytes() != text.encode("utf-8"):
      dst.write_text(text, encoding="utf-8", newline="\n")
   if depfile is not None:
      def esc(p: str | PathLike[str]) -> str:
         """!A path as make writes it: forward slashes, spaces escaped.
         @param p  the path
         @return the text
         """
         return str(p).replace("\\", "/").replace(" ", "\\ ")
      depfile.parent.mkdir(parents=True, exist_ok=True)
      depfile.write_text(f"{esc(dst)}: " + " ".join(esc(p) for p in sorted(set(deps))) + "\n",
                         encoding="utf-8", newline="\n")
   scr = compile_manifest.screen
   rom = (sum(len(b["tiles"]) * WORD_BYTES + PALETTE_BYTES for b in bgs) +
          sum(len(s["tiles"]) * WORD_BYTES + PALETTE_BYTES for s in sprites) +
          screen_assets.rom_bytes(scr["tiles"], scr["tilemap"], scr["palette"]))
   print(f"{src}: {len(bgs)} backgrounds, {len(sprites)} sprites, {len(texts)} texts, {len(lists)} lists, "
         f"~{rom} B of graphics")
   grids = [s for s in sprites if "grid_saved" in s]
   if grids or compile_manifest.doors:   # DS1
      saved = sum(s["grid_saved"] for s in grids)
      print(f"{src}: {len(grids)} direction-grid sprites ({saved} B saved by mirrored right columns and merged "
            f"frames), {len(compile_manifest.doors)} doors")
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv))
