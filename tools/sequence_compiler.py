#!/usr/bin/env python3
"""Compile a sequence manifest (capsule.sequence/1) into C++ ROM tables.

    sequence_compiler.py <manifest.json> <out.inc> [--symbol kName]
                        [--max-tracks 8] [--max-sprites 16] [--max-affine 4]
                        [--code-page <page.json>]

One format for title screens, cutscenes and credits. Follows the general
manifest rule (P-1): editor data stripped, unknown keys rejected, names
resolved to numbers, record sizes locked to the C++ structs. Also rejects
zero-time loops, shadowed combos, dangling targets/labels, unbalanced loops
and $item outside for_each. The emitted .inc static_asserts the slot/track
counts it uses against the build's limits. Text comes from text assets; the
game's code page (--code-page, tools/text_encoder.py) is only checked here:
text on bg3 needs a font of at most 96 glyphs (BG3's character block shares
its upper tiles with the screen blocks).
"""
from __future__ import annotations

import copy
import json
import re
import struct
import sys
import unicodedata
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path
from typing import Any

import text_encoder

## Key names in GBA KEYINPUT bit order.
KEYS = ["A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L"]
## Blend layer names -> layer mask bits.
LAYERS = {"bg0": 1, "bg1": 2, "bg2": 4, "bg3": 8, "obj": 16, "backdrop": 32}
## Blend mode names in capsule::seq::BlendMode order.
BLEND_MODES = ["off", "alpha", "brighten", "darken"]
## Text target names in capsule::seq::TextTarget order.
TEXT_TARGETS = ["bg0", "bg1", "bg2", "bg3", "box"]
## The text target whose font size is limited (text_encoder.BG3_MAX_FONT_GLYPHS).
BG3_TARGET = "bg3"
## Alignment names in capsule::seq::TextAlign order.
ALIGN = ["left", "center", "right"]
## Stage flag names -> capsule::seq::StageFlag masks.
STAGE_FLAGS = {"reset_visuals": 1, "seed_on_press": 2, "exit_on_song_end": 4}
## wait_for 'what' names in capsule::seq::WaitKind order.
WAIT_KINDS = ["fade", "sprite", "text", "input", "song_end", "song_frame", "blend", "pal_fade",
              "window", "cond"]

## Engine op names; numbering must match capsule::seq::Op.
OPS = ["end", "wait", "wait_for", "fade", "video_mode", "bg_load", "bg_show", "bg_pos", "bg_scroll",
       "blend", "blend_ramp", "spr_load", "spr_show", "spr_pos", "spr_move", "spr_anim",
       "spr_obj_window", "spr_affine", "spr_blend", "emitter", "pal_anim", "pal_fade_color",
       "pal_fade", "scanline", "window", "window_move", "text_box", "text_style", "text",
       "text_clear", "map_show", "map_camera", "map_scroll", "song", "song_fade", "sfx", "jump",
       "if", "repeat", "for_each", "next", "goto"]
## Op name -> number.
OP = {name: i for i, name in enumerate(OPS)}

## Manifest ops -> accepted keys (some are sugar for engine ops).
OP_KEYS = {
   "end": set(), "wait": {"frames"}, "wait_for": {"what", "slot", "keys", "frame", "cond"},
   "fade": {"dir", "color", "frames"}, "video_mode": {"mode"},
   "bg_load": {"bg", "asset"}, "bg_show": {"bg"}, "bg_hide": {"bg"}, "bg_pos": {"bg", "x", "y"},
   "bg_scroll": {"bg", "vx", "vy"},
   "blend": {"mode", "src", "tgt", "src_layers", "tgt_layers"}, "blend_ramp": {"src", "tgt", "frames"},
   "spr_load": {"slot", "asset"}, "spr_show": {"slot"}, "spr_hide": {"slot"},
   "spr_pos": {"slot", "x", "y"}, "spr_move": {"slot", "vx", "vy", "frames"},
   "spr_anim": {"slot", "anim"}, "spr_obj_window": {"slot", "on"},
   "spr_affine": {"slot", "scale", "angle", "frames"}, "spr_blend": {"slot", "on"},
   "emitter": {"slot", "on", "asset"}, "pal_anim": {"slot", "on", "asset"},
   "pal_fade": {"palettes", "color", "amount", "frames"},
   "scanline": {"bg", "on", "asset"},
   "window": {"win", "flags", "x0", "y0", "x1", "y1"},
   "window_move": {"win", "x0", "y0", "x1", "y1", "frames"},
   "text_box": {"open", "x", "y", "w", "h"}, "text_style": {"style"},
   "text": {"target", "x", "y", "align", "typed", "asset"}, "text_clear": {"target", "row", "rows"},
   "map_show": {"asset"}, "map_hide": set(), "map_camera": {"x", "y"}, "map_scroll": {"vx", "vy"},
   "song": {"asset", "fade_in"}, "song_stop": set(), "song_fade": {"frames"}, "sfx": {"asset"},
   "label": {"name"}, "jump": {"to"}, "if": {"cond", "not", "to"},
   "repeat": {"count"}, "for_each": {"list", "max"}, "next": set(), "goto": {"to"},
   "use": None,   # macro expansion: any keys (macro parameters) + "macro"
}
## Allowed top-level manifest keys.
TOP_KEYS = {"schema", "id", "exits", "conditions", "lists", "macros", "start", "stages"}
## Allowed stage keys.
STAGE_KEYS = {"name", "flags", "skip", "timeout", "on", "combos", "tracks", "fast_forward"}
## Allowed keys of a stage's "on" object.
ON_KEYS = {"done", "skip", "timeout", "song_end"}
## Allowed combo keys.
COMBO_KEYS = {"held", "pressed", "to", "cond"}
## Allowed fast_forward keys.
FF_KEYS = {"keys", "speed", "cond"}

## Most stages in one sequence (stage targets are 7-bit).
MAX_STAGES = 128
## Most exit codes (0-126; 127 | 0x80 is kNoTarget).
MAX_EXITS = 127
## Nesting limit of repeat / for_each (capsule::seq::kMaxLoopDepth).
MAX_LOOP_DEPTH = 2
## Target value meaning "none" (capsule::seq::kNoTarget).
NO_TARGET = 0xFF
## Asset operand meaning "current for_each item" (capsule::seq::kItemAsset).
ITEM_ASSET = 0xFFFFFFFF
## Ops that make a track wait (needed in every loop body).
BLOCKING = {OP["wait"], OP["wait_for"]}
## Target flag marking an exit code instead of a stage index (capsule::seq::kExitBit).
EXIT_TARGET_FLAG = 0x80
## Most condition names (ids are 1-based and 8-bit).
MAX_CONDITIONS = 255
## Most list names (ids are 1-based and 8-bit).
MAX_LISTS = 255
## Most combos of a whole sequence (8-bit combo indices).
MAX_COMBOS = 255
## Most commands of a whole sequence (16-bit track table entries).
MAX_COMMANDS = 0xFFFF
## Deepest macro nesting before expansion stops (guards against recursion).
MAX_MACRO_DEPTH = 8

## Default build limit CAPSULE_SEQ_MAX_TRACKS.
DEFAULT_MAX_TRACKS = 8
## Default build limit CAPSULE_SEQ_MAX_SPRITES.
DEFAULT_MAX_SPRITES = 16
## Default build limit CAPSULE_SEQ_MAX_AFFINE.
DEFAULT_MAX_AFFINE = 4

## Width of an 8-bit operand field.
U8_BITS = 8
## Width of a 16-bit operand field.
U16_BITS = 16
## Width of a BG number (BG0-BG3).
BG_BITS = 2
## Width of a blend weight or palette fade amount field (0-16 needs five bits).
WEIGHT_BITS = 5
## Largest blend weight (16/16 is full strength).
MAX_BLEND_WEIGHT = 16
## Largest palette fade amount (16/16 is the full fade color).
MAX_FADE_AMOUNT = 16
## Slowest fast-forward speed (frames run per frame).
MIN_FF_SPEED = 2
## Fastest fast-forward speed.
MAX_FF_SPEED = 16
## Fast-forward speed when the manifest gives none.
DEFAULT_FF_SPEED = 2

## Fade operand bit: fading in (else out).
FADE_IN_FLAG = 1
## Fade operand bit: fading to or from white (else black).
FADE_WHITE_FLAG = 2
## Video modes a sequence may use (C-01d).
VIDEO_MODES = (0, 1, 2)
## Shift of the alignment in a text command's first operand (the target takes the low bits).
TEXT_ALIGN_SHIFT = 3
## Shift of the "typed" flag in a text command's first operand.
TEXT_TYPED_SHIFT = 5

## Q8.8 fixed point: the value of 1.0.
Q8_ONE = 256
## Smallest signed 16-bit value.
INT16_MIN = -32768
## Largest signed 16-bit value.
INT16_MAX = 32767
## Mask to the 16-bit pattern of a value.
U16_MASK = 0xFFFF
## Largest unsigned 16-bit value.
U16_MAX = 0xFFFF
## Degrees in a full turn.
DEGREES_PER_TURN = 360.0
## Engine angle units in a full turn (16-bit binary angle).
ANGLE_UNITS_PER_TURN = 65536

## FNV-1a 32-bit offset basis.
FNV_OFFSET_BASIS = 0x811C9DC5
## FNV-1a 32-bit prime.
FNV_PRIME = 0x01000193
## Mask to 32 bits.
U32_MASK = 0xFFFFFFFF

## Base of hex digits.
HEX_BASE = 16
## Hex digits per channel of a "#rrggbb" color.
HEX_DIGITS_PER_CHANNEL = 2
## Where each channel's digits start in a "#rrggbb" color.
HEX_CHANNEL_OFFSETS = (1, 3, 5)
## Bits dropped reducing an 8-bit channel to the GBA's 5 bits.
CHANNEL_DROP_BITS = 3
## Shift of green in a BGR555 color.
GREEN_SHIFT = 5
## Shift of blue in a BGR555 color.
BLUE_SHIFT = 10
## First bit of the OBJ palettes in a palette mask (BG palettes take bits 0-15).
OBJ_PALETTE_BIT = 16
## Highest palette number of each kind.
LAST_PALETTE = 15
## Regex group of a palette spec's range ("all", "lo-hi" or one number).
SPEC_RANGE_GROUP = 2

## Bytes of a capsule::seq::Cmd record.
CMD_BYTES = 8
## Bytes of a capsule::seq::Stage record.
STAGE_BYTES = 20
## Bytes of a capsule::seq::Combo record.
COMBO_BYTES = 8
## Bytes of one track table entry (uint16_t).
TRACK_ENTRY_BYTES = 2

## Fewest command-line arguments, including the program name.
MIN_ARGUMENTS = 3
## Position of the output file argument.
OUTPUT_ARGUMENT = 2
## Exit status for a bad command line.
EXIT_USAGE = 2

## A manifest object (parsed JSON).
JsonDict = dict[str, Any]
## A compiled command: (op, a, b, c).
Command = tuple[int, int, int, int]
## A compiled combo: (held, pressed, target, cond).
Combo = tuple[int, int, int, int]


class ManifestError(Exception):
   """!A manifest rule was broken; the message says where and how."""
   pass


def strip_editor(node: Any) -> Any:
   """!Drops editor-only data everywhere in a manifest (P-1 rule 2).
   @param node  any JSON value
   @return a copy without "editor" and "_"-prefixed keys
   """
   if isinstance(node, dict):
      return {k: strip_editor(v) for k, v in node.items() if k != "editor" and not k.startswith("_")}
   if isinstance(node, list):
      return [strip_editor(v) for v in node]
   return node


def asset_key(ref: object, where: str = "asset") -> int:
   """!Asset Key v1 (C-02d): FNV-1a 32 over 'namespace:CANONICAL NAME'.
   @param ref    'namespace:Name', or '$item' for the current list item
   @param where  location for error messages
   @return the 32-bit key (ITEM_ASSET for '$item')
   @throws ManifestError for a malformed reference or a hash equal to ITEM_ASSET
   """
   if ref == "$item":
      return ITEM_ASSET
   if not isinstance(ref, str) or ":" not in ref:
      raise ManifestError(f"{where}: asset reference must be 'namespace:NAME', got {ref!r}")
   ns, name = ref.split(":", 1)
   canon = " ".join(unicodedata.normalize("NFC", name).upper().split())
   h = FNV_OFFSET_BASIS
   for b in f"{ns}:{canon}".encode("utf-8"):
      h = ((h ^ b) * FNV_PRIME) & U32_MASK
   if h == ITEM_ASSET:
      raise ManifestError(f"{where}: {ref!r} hashes to the reserved $item key; rename the asset")
   return h


def key_mask(names: Iterable[Any] | None, where: str) -> int:
   """!Key names to a KeyBits mask.
   @param names  list of key names (case-insensitive), or None
   @param where  location for error messages
   @return the mask
   """
   mask = 0
   for n in names or []:
      if not isinstance(n, str) or n.upper() not in KEYS:
         raise ManifestError(f"{where}: unknown key {n!r} (expected one of {KEYS})")
      mask |= 1 << KEYS.index(n.upper())
   return mask


def q8(value: float | str, where: str) -> int:
   """!Number to Q8.8, as the 16-bit pattern of the signed value.
   @param value  number
   @param where  location for error messages
   @return 0-0xFFFF
   """
   v = int(round(float(value) * Q8_ONE))
   if not INT16_MIN <= v <= INT16_MAX:
      raise ManifestError(f"{where}: {value} out of Q8.8 range")
   # Masking a negative value gives its two's complement bit pattern.
   return v & U16_MASK


def s16(value: float | str, where: str) -> int:
   """!Integer to the 16-bit pattern of a signed value.
   @param value  integer, -32768-32767
   @param where  location for error messages
   @return 0-0xFFFF
   """
   v = int(value)
   if not INT16_MIN <= v <= INT16_MAX:
      raise ManifestError(f"{where}: {value} out of int16 range")
   return v & U16_MASK


def u(value: float | str | None, bits: int, where: str, name: str = "value") -> int:
   """!Checked unsigned integer.
   @param value  integer (None is an error)
   @param bits   width in bits
   @param where  location for error messages
   @param name   field name for error messages
   @return the value
   """
   if value is None:
      raise ManifestError(f"{where}: missing {name}")
   v = int(value)
   if not 0 <= v < (1 << bits):
      raise ManifestError(f"{where}: {name} {value} out of range 0..{(1 << bits) - 1}")
   return v


def layer_mask(names: Iterable[str] | None, where: str) -> int:
   """!Layer names to a blend layer mask (see LAYERS).
   @param names  list of layer names, or None
   @param where  location for error messages
   @return the mask
   """
   m = 0
   for n in names or []:
      if n not in LAYERS:
         raise ManifestError(f"{where}: unknown layer {n!r}")
      m |= LAYERS[n]
   return m


def rgb555(value: object, where: str) -> int:
   """!Hex color to GBA BGR555.
   @param value  color string: a hash sign followed by rrggbb
   @param where  location for error messages
   @return the BGR555 value
   """
   if not isinstance(value, str) or not re.fullmatch(r"#[0-9a-fA-F]{6}", value):
      raise ManifestError(f"{where}: color must be '#rrggbb'")
   r, g, b = (int(value[i:i + HEX_DIGITS_PER_CHANNEL], HEX_BASE) >> CHANNEL_DROP_BITS for i in HEX_CHANNEL_OFFSETS)
   return r | g << GREEN_SHIFT | b << BLUE_SHIFT


def palette_mask(spec: Iterable[Any] | None, where: str) -> int:
   """!Palette spec to a 32-bit mask (bits 0-15 BG, 16-31 OBJ).
   @param spec   list such as ['bg:0-3', 'obj:5', 'bg:all']
   @param where  location for error messages
   @return the mask (never 0)
   """
   mask = 0
   for item in spec or []:
      m = re.fullmatch(r"(bg|obj):(all|\d+(?:-\d+)?)", str(item))
      if not m:
         raise ManifestError(f"{where}: palette spec {item!r} must look like 'bg:0-3', 'obj:5', 'bg:all'")
      base = 0 if m.group(1) == "bg" else OBJ_PALETTE_BIT
      rng = m.group(SPEC_RANGE_GROUP)
      # "all", "lo-hi" or a single palette number.
      lo, hi = (0, LAST_PALETTE) if rng == "all" else (map(int, rng.split("-")) if "-" in rng else (int(rng), int(rng)))
      if not 0 <= lo <= hi <= LAST_PALETTE:
         raise ManifestError(f"{where}: palette range {item!r} outside 0-15")
      for i in range(lo, hi + 1):
         mask |= 1 << (base + i)
   if mask == 0:
      raise ManifestError(f"{where}: pal_fade needs at least one palette")
   return mask


def substitute(node: Any, args: Mapping[str, Any]) -> Any:
   """!Replaces $param strings in a macro body with the call's arguments.
   @param node  any JSON value from the macro body
   @param args  parameter name -> value
   @return the substituted copy ('$item' is left alone)
   """
   if isinstance(node, str) and node.startswith("$") and node != "$item":
      name = node[1:]
      if name not in args:
         raise ManifestError(f"macro parameter {node} not supplied")
      return args[name]
   if isinstance(node, dict):
      return {k: substitute(v, args) for k, v in node.items()}
   if isinstance(node, list):
      return [substitute(v, args) for v in node]
   return node


class Compiler:
   """!Compiles one manifest: construct, call run(), then emit_inc().

   Attributes are documented where __init__ sets them. The three below are
   repeated here because Doxygen's Python parser otherwise reports their use
   in `for ... in self.x` / `in self.x` as undocumented members.

   @var stage_index
   Stage name -> index.
   @var stages
   Stage objects from the manifest.
   @var combos
   Compiled combos: (held, pressed, target, cond).
   @var codepage
   The game's code page (its font size limits text on bg3).
   """

   def __init__(self, manifest: JsonDict, max_tracks: int = DEFAULT_MAX_TRACKS, max_sprites: int = DEFAULT_MAX_SPRITES,
                max_affine: int = DEFAULT_MAX_AFFINE, *,
                codepage: text_encoder.CodePage = text_encoder.DEFAULT) -> None:
      """!Validates the top level and indexes the stages.
      @param manifest     parsed JSON manifest
      @param max_tracks   build limit CAPSULE_SEQ_MAX_TRACKS
      @param max_sprites  build limit CAPSULE_SEQ_MAX_SPRITES
      @param max_affine   build limit CAPSULE_SEQ_MAX_AFFINE
      @param codepage     the game's code page (its font size limits text on bg3)
      """
      m = strip_editor(manifest)
      ## The game's code page.
      self.codepage = codepage
      ## Build limit: tracks per stage.
      self.max_tracks = max_tracks
      ## Build limit: sprite slots.
      self.max_sprites = max_sprites
      ## Build limit: affine sprite slots.
      self.max_affine = max_affine
      ## Non-fatal findings, printed by main().
      self.warnings = []
      unknown = set(m) - TOP_KEYS
      if unknown:
         raise ManifestError(f"unknown top-level keys {sorted(unknown)}")
      if m.get("schema") != "capsule.sequence/1":
         raise ManifestError("schema must be 'capsule.sequence/1'")
      ## Manifest id, for messages and the generated header.
      self.id = m.get("id", "sequence")
      ## Exit names; the index is the exit code.
      self.exits = m.get("exits", [])
      ## Condition names; index + 1 is the condition id.
      self.conditions = m.get("conditions", [])
      ## List names; index + 1 is the list id.
      self.lists = m.get("lists", [])
      for name, items, cap in (("exits", self.exits, MAX_EXITS), ("conditions", self.conditions, MAX_CONDITIONS),
                               ("lists", self.lists, MAX_LISTS)):
         if len(items) > cap or len(set(items)) != len(items):
            raise ManifestError(f"{name}: at most {cap} unique names")
      ## Macro name -> {"params": [...], "body": [...]}.
      self.macros = m.get("macros", {})
      for name, mac in self.macros.items():
         if set(mac) - {"params", "body"} or not isinstance(mac.get("body"), list):
            raise ManifestError(f"macro {name!r}: needs 'body' (list) and optional 'params'")
      ## Stage objects from the manifest.
      self.stages = m.get("stages", [])
      if not 1 <= len(self.stages) <= MAX_STAGES:
         raise ManifestError(f"stages: 1..{MAX_STAGES} required")
      ## Stage name -> index.
      self.stage_index = {}
      for i, s in enumerate(self.stages):
         if s.get("name") in self.stage_index:
            raise ManifestError(f"stage {s.get('name')!r} defined twice")
         self.stage_index[s.get("name")] = i
      start = m.get("start", self.stages[0]["name"])
      if start not in self.stage_index:
         raise ManifestError(f"start: unknown stage {start!r}")
      ## Index of the start stage.
      self.start = self.stage_index[start]
      ## Stage indices some target refers to (for the unreachable warning).
      self.referenced = {self.start}
      ## Compiled commands: (op, a, b, c).
      self.cmds = []
      ## First command index of each track.
      self.tracks = []
      ## Compiled stage records, in Stage field order.
      self.stage_recs = []
      ## Compiled combos: (held, pressed, target, cond).
      self.combos = []
      ## Most tracks any stage uses.
      self.used_tracks = 0
      ## Highest sprite slot used + 1.
      self.used_sprite = 0
      ## Highest affine slot used + 1.
      self.used_affine = 0
      ## Macro expansions so far (makes labels unique).
      self.expansions = 0

   # ---- names ----
   def target(self, ref: object, where: str) -> int:
      """!'stage:NAME' / 'exit:NAME' to a Target value.
      @param ref    the reference, or None
      @param where  location for error messages
      @return stage index, 0x80 | exit code, or NO_TARGET
      """
      if ref is None:
         return NO_TARGET
      if not isinstance(ref, str) or ":" not in ref:
         raise ManifestError(f"{where}: target must be 'stage:<name>' or 'exit:<name>'")
      kind, name = ref.split(":", 1)
      if kind == "stage":
         if name not in self.stage_index:
            raise ManifestError(f"{where}: unknown stage {name!r}")
         self.referenced.add(self.stage_index[name])
         return self.stage_index[name]
      if kind == "exit":
         if name not in self.exits:
            raise ManifestError(f"{where}: unknown exit {name!r} (declare it in 'exits')")
         return EXIT_TARGET_FLAG | self.exits.index(name)
      raise ManifestError(f"{where}: bad target kind {kind!r}")

   def named(self, table: list[str], kind: str, name: object, where: str) -> int:
      """!1-based id of a declared condition or list.
      @param table  self.conditions or self.lists
      @param kind   'condition' or 'list', for error messages
      @param name   the name
      @param where  location for error messages
      @return the id
      """
      if name not in table:
         raise ManifestError(f"{where}: unknown {kind} {name!r} (declare it in '{kind}s')")
      return table.index(name) + 1

   # ---- macros ----
   def expand(self, track: list[Any], where: str, depth: int = 0) -> list[Any]:
      """!Expands 'use' commands; labels inside a macro are made local to
      each expansion.
      @param track  list of command objects
      @param where  location for error messages
      @param depth  nesting level (more than 8 is an error)
      @return the expanded command list
      """
      if depth > MAX_MACRO_DEPTH:
         raise ManifestError(f"{where}: macros nested more than {MAX_MACRO_DEPTH} deep (recursive?)")
      out = []
      for n, raw in enumerate(track):
         if isinstance(raw, dict) and raw.get("op") == "use":
            name = raw.get("macro")
            if name not in self.macros:
               raise ManifestError(f"{where} cmd {n}: unknown macro {name!r}")
            mac = self.macros[name]
            args = {k: v for k, v in raw.items() if k not in ("op", "macro")}
            params = set(mac.get("params", []))
            if set(args) != params:
               raise ManifestError(f"{where} cmd {n}: macro {name!r} takes {sorted(params)}, "
                                   f"got {sorted(args)}")
            self.expansions += 1
            tag = f"#{self.expansions}"
            body = substitute(copy.deepcopy(mac["body"]), args)
            for cmd in body:   # labels are local to each expansion
               if isinstance(cmd, dict) and cmd.get("op") == "label":
                  cmd["name"] = str(cmd["name"]) + tag
               # A jump to one of the macro's own labels follows the label's new name.
               if isinstance(cmd, dict) and cmd.get("op") in ("jump", "if") and \
                       any(isinstance(c, dict) and c.get("op") == "label" and
                           c.get("name") == cmd.get("to") for c in mac["body"]):
                  cmd["to"] = str(cmd["to"]) + tag
            out.extend(self.expand(body, f"{where} > macro {name}", depth + 1))
         else:
            out.append(raw)
      return out

   # ---- tracks ----
   def compile_track(self, track: list[Any], where: str) -> list[Command]:
      """!Compiles one track: loops, labels and zero-time checks.
      @param track  list of command objects
      @param where  location for error messages
      @return list of (op, a, b, c) tuples ending with end, jump or goto
      """
      track = self.expand(track, where)
      labels, out, fixups, loop_stack = {}, [], [], []
      for n, raw in enumerate(track):
         w = f"{where} cmd {n}"
         if not isinstance(raw, dict) or "op" not in raw:
            raise ManifestError(f"{w}: command must be an object with 'op'")
         op = raw["op"]
         args = {k: v for k, v in raw.items() if k != "op"}
         if op not in OP_KEYS or op == "use":
            raise ManifestError(f"{w}: unknown op {op!r}")
         unknown = set(args) - OP_KEYS[op]
         if unknown:
            raise ManifestError(f"{w}: unknown keys {sorted(unknown)} for {op}")
         if op == "label":
            if args.get("name") in labels:
               raise ManifestError(f"{w}: label {args.get('name')!r} defined twice")
            labels[args.get("name")] = len(out)
            continue
         if op in ("repeat", "for_each"):
            if len(loop_stack) >= MAX_LOOP_DEPTH:
               raise ManifestError(f"{w}: loops nested deeper than {MAX_LOOP_DEPTH}")
            loop_stack.append((len(out), op))
         if op == "next":
            if not loop_stack:
               raise ManifestError(f"{w}: next without repeat/for_each")
            start, kind = loop_stack.pop()
            body = out[start + 1:]
            # Blocking: a wait_for, or a wait of at least one frame.
            if not any(o in BLOCKING and (o != OP["wait"] or b > 0) for o, _, b, _ in body):
               raise ManifestError(f"{w}: {kind} body has no wait (would run in zero time)")
            o, a, b, _ = out[start]
            out[start] = (o, a, b, len(out) + 1)   # exit index = after next
            out.append((OP["next"], 0, 0, 0))
            continue
         in_foreach = any(k == "for_each" for _, k in loop_stack)
         for key in ("asset",):
            if args.get(key) == "$item" and not in_foreach:
               raise ManifestError(f"{w}: $item used outside for_each")
         out.extend(self.encode(op, args, w, fixups, len(out)))
      if loop_stack:
         raise ManifestError(f"{where}: {loop_stack[-1][1]} without next")
      if not out or out[-1][0] not in (OP["end"], OP["jump"], OP["goto"]):
         out.append((OP["end"], 0, 0, 0))
      for idx, label, w in fixups:
         if label not in labels:
            raise ManifestError(f"{w}: unknown label {label!r}")
         dest = labels[label]
         o, a, _, c = out[idx]
         out[idx] = (o, a, dest, c)
         # A backward jump needs a blocking command between the label and the jump.
         if dest <= idx and not any(xo in BLOCKING and (xo != OP["wait"] or xb > 0)
                                    for xo, _, xb, _ in out[dest:idx]):
            raise ManifestError(f"{w}: loop back to {label!r} has no wait (would spin forever)")
      return out

   def sprite_slot(self, value: float | str | None, w: str, affine: bool = False) -> int:
      """!Checks a sprite slot against the build limits and records its use.
      @param value   slot number
      @param w       location for error messages
      @param affine  the command uses scale/rotation
      @return the slot
      """
      slot = u(value, U8_BITS, w, "slot")
      limit = self.max_affine if affine else self.max_sprites
      if slot >= limit:
         raise ManifestError(f"{w}: sprite slot {slot} >= {limit}" +
                             (" (affine ramps use the first slots)" if affine else ""))
      self.used_sprite = max(self.used_sprite, slot + 1)
      if affine:
         self.used_affine = max(self.used_affine, slot + 1)
      return slot

   def rect(self, a: JsonDict, w: str, keys: Sequence[str] = ("x0", "y0", "x1", "y1")) -> int:
      """!Packs four 8-bit fields into one operand, first key lowest.
      @param a     command arguments
      @param w     location for error messages
      @param keys  the four field names
      @return the packed value
      """
      vals = [u(a.get(k), U8_BITS, w, k) for k in keys]
      packed = 0
      for i, v in enumerate(vals):
         packed |= v << (U8_BITS * i)   # one byte per field, first key in the lowest byte
      return packed

   def encode(self, op: str, a: JsonDict, w: str, fixups: list[tuple[int, Any, str]], pos: int) -> list[Command]:
      """!Encodes one manifest command as engine commands.
      @param op      manifest op name
      @param a       its arguments
      @param w       location for error messages
      @param fixups  list receiving (index, label, where) for label operands
      @param pos     index of the first emitted command in the track
      @return list of (op, a, b, c) tuples
      """
      g = a.get
      if op == "end":
         return [(OP["end"], 0, 0, 0)]
      if op == "wait":
         return [(OP["wait"], 0, u(g("frames"), U16_BITS, w, "frames"), 0)]
      if op == "wait_for":
         what = g("what")
         if what not in WAIT_KINDS:
            raise ManifestError(f"{w}: wait_for 'what' must be one of {WAIT_KINDS}")
         param = 0
         if what == "sprite":
            param = self.sprite_slot(g("slot"), w)
         elif what == "input":
            param = key_mask(g("keys"), w)
            if not param:
               raise ManifestError(f"{w}: wait_for input needs keys")
         elif what == "song_frame":
            param = u(g("frame"), U16_BITS, w, "frame")
         elif what == "cond":
            param = self.named(self.conditions, "condition", g("cond"), w)
         return [(OP["wait_for"], WAIT_KINDS.index(what), param, 0)]
      if op == "fade":
         d, col = g("dir"), g("color", "black")
         if d not in ("in", "out") or col not in ("black", "white"):
            raise ManifestError(f"{w}: fade needs dir in|out and color black|white")
         return [(OP["fade"], (FADE_IN_FLAG if d == "in" else 0) | (FADE_WHITE_FLAG if col == "white" else 0),
                  u(g("frames", 0), U16_BITS, w, "frames"), 0)]
      if op == "video_mode":
         if g("mode") not in VIDEO_MODES:
            raise ManifestError(f"{w}: video mode must be 0, 1 or 2 (C-01d)")
         return [(OP["video_mode"], g("mode"), 0, 0)]
      if op == "bg_load":
         return [(OP["bg_load"], u(g("bg"), BG_BITS, w, "bg"), 0, asset_key(g("asset"), w))]
      if op in ("bg_show", "bg_hide"):
         return [(OP["bg_show"], u(g("bg"), BG_BITS, w, "bg"), 1 if op == "bg_show" else 0, 0)]
      if op == "bg_pos":
         return [(OP["bg_pos"], u(g("bg"), BG_BITS, w, "bg"), s16(g("x", 0), w), s16(g("y", 0), w))]
      if op == "bg_scroll":
         return [(OP["bg_scroll"], u(g("bg"), BG_BITS, w, "bg"), q8(g("vx", 0), w), q8(g("vy", 0), w))]
      if op == "blend":
         if g("mode") not in BLEND_MODES:
            raise ManifestError(f"{w}: blend mode must be one of {BLEND_MODES}")
         src, tgt = u(g("src", 0), WEIGHT_BITS, w, "src"), u(g("tgt", 0), WEIGHT_BITS, w, "tgt")
         if src > MAX_BLEND_WEIGHT or tgt > MAX_BLEND_WEIGHT:
            raise ManifestError(f"{w}: blend weights are 0..{MAX_BLEND_WEIGHT}")
         # Source in the low byte, target in the next one (weights and layer masks alike).
         masks = layer_mask(g("src_layers"), w) | (layer_mask(g("tgt_layers"), w) << U8_BITS)
         return [(OP["blend"], BLEND_MODES.index(g("mode")), src | (tgt << U8_BITS), masks)]
      if op == "blend_ramp":
         src, tgt = u(g("src"), WEIGHT_BITS, w, "src"), u(g("tgt"), WEIGHT_BITS, w, "tgt")
         if src > MAX_BLEND_WEIGHT or tgt > MAX_BLEND_WEIGHT:
            raise ManifestError(f"{w}: blend weights are 0..{MAX_BLEND_WEIGHT}")
         return [(OP["blend_ramp"], 0, src | (tgt << U8_BITS), u(g("frames"), U16_BITS, w, "frames"))]
      if op in ("spr_load", "spr_show", "spr_hide", "spr_pos", "spr_move", "spr_anim",
                "spr_obj_window", "spr_blend"):
         slot = self.sprite_slot(g("slot"), w)
         if op == "spr_load":
            return [(OP["spr_load"], slot, 0, asset_key(g("asset"), w))]
         if op in ("spr_show", "spr_hide"):
            return [(OP["spr_show"], slot, 1 if op == "spr_show" else 0, 0)]
         if op == "spr_pos":
            return [(OP["spr_pos"], slot, s16(g("x", 0), w), s16(g("y", 0), w))]
         if op == "spr_move":
            return [(OP["spr_move"], slot, q8(g("vx", 0), w),
                     q8(g("vy", 0), w) | (u(g("frames", 0), U16_BITS, w, "frames") << U16_BITS))]
         if op == "spr_anim":
            return [(OP["spr_anim"], slot, u(g("anim"), U16_BITS, w, "anim"), 0)]
         return [(OP[op], slot, 1 if g("on", True) else 0, 0)]
      if op == "spr_affine":
         slot = self.sprite_slot(g("slot"), w, affine=True)
         # Scale as Q8.8; angle in 16-bit binary units (65536 per turn); frames above the angle.
         scale = int(round(float(g("scale", 1.0)) * Q8_ONE))
         if not 0 <= scale <= U16_MAX:
            raise ManifestError(f"{w}: scale out of range")
         angle = int(round(float(g("angle", 0)) / DEGREES_PER_TURN * ANGLE_UNITS_PER_TURN))
         if not 0 <= angle <= U16_MAX:
            raise ManifestError(f"{w}: angle must be 0 <= degrees < 360")
         return [(OP["spr_affine"], slot, scale, angle | u(g("frames", 0), U16_BITS, w, "frames") << U16_BITS)]
      if op in ("emitter", "pal_anim", "scanline"):
         on = bool(g("on", True))
         if on and "asset" not in a:
            raise ManifestError(f"{w}: {op} start needs an asset")
         first = u(g("bg"), BG_BITS, w, "bg") if op == "scanline" else u(g("slot"), U8_BITS, w, "slot")
         return [(OP[op], first, 1 if on else 0, asset_key(g("asset"), w) if on else 0)]
      if op == "pal_fade":
         amount = u(g("amount"), WEIGHT_BITS, w, "amount")
         if amount > MAX_FADE_AMOUNT:
            raise ManifestError(f"{w}: amount is 0..{MAX_FADE_AMOUNT} (0 = original colors)")
         return [(OP["pal_fade_color"], 0, rgb555(g("color", "#000000"), w), 0),
                 (OP["pal_fade"], amount, u(g("frames", 0), U16_BITS, w, "frames"), palette_mask(g("palettes"), w))]
      if op == "window":
         return [(OP["window"], u(g("win"), 1, w, "win"), u(g("flags", 0), U16_BITS, w, "flags"), self.rect(a, w))]
      if op == "window_move":
         return [(OP["window_move"], u(g("win"), 1, w, "win"), u(g("frames"), U16_BITS, w, "frames"), self.rect(a, w))]
      if op == "text_box":
         opn = bool(g("open", True))
         r = self.rect(a, w, ("x", "y", "w", "h")) if opn else 0
         return [(OP["text_box"], 1 if opn else 0, 0, r)]
      if op == "text_style":
         return [(OP["text_style"], u(g("style"), U8_BITS, w, "style"), 0, 0)]
      if op == "text":
         tgt, align = g("target", "box"), g("align", "left")
         if tgt not in TEXT_TARGETS or align not in ALIGN:
            raise ManifestError(f"{w}: text target must be {TEXT_TARGETS}, align {ALIGN}")
         page = self.codepage
         glyphs = page.font_span()[1]
         if tgt == BG3_TARGET and glyphs > text_encoder.BG3_MAX_FONT_GLYPHS:
            raise ManifestError(f"{w}: text on bg3 needs a font of at most {text_encoder.BG3_MAX_FONT_GLYPHS} "
                                f"glyphs; code page {page.id!r} has {glyphs} (use bg0-bg2 or the box)")
         av = (TEXT_TARGETS.index(tgt) | ALIGN.index(align) << TEXT_ALIGN_SHIFT |
               (1 if g("typed", False) else 0) << TEXT_TYPED_SHIFT)
         return [(OP["text"], av, u(g("x", 0), U8_BITS, w, "x") | u(g("y", 0), U8_BITS, w, "y") << U8_BITS,
                  asset_key(g("asset"), w))]
      if op == "text_clear":
         tgt = g("target", "box")
         if tgt not in TEXT_TARGETS:
            raise ManifestError(f"{w}: text target must be one of {TEXT_TARGETS}")
         return [(OP["text_clear"], TEXT_TARGETS.index(tgt),
                  u(g("row", 0), U8_BITS, w, "row") | u(g("rows", 0), U8_BITS, w, "rows") << U8_BITS, 0)]
      if op == "map_show":
         return [(OP["map_show"], 0, 0, asset_key(g("asset"), w))]
      if op == "map_hide":
         return [(OP["map_show"], 0, 0, 0)]
      if op == "map_camera":
         return [(OP["map_camera"], 0, s16(g("x", 0), w), s16(g("y", 0), w))]
      if op == "map_scroll":
         return [(OP["map_scroll"], 0, q8(g("vx", 0), w), q8(g("vy", 0), w))]
      if op == "song":
         return [(OP["song"], 0, u(g("fade_in", 0), U16_BITS, w, "fade_in"), asset_key(g("asset"), w))]
      if op == "song_stop":
         return [(OP["song"], 0, 0, 0)]
      if op == "song_fade":
         return [(OP["song_fade"], 0, u(g("frames"), U16_BITS, w, "frames"), 0)]
      if op == "sfx":
         return [(OP["sfx"], 0, 0, asset_key(g("asset"), w))]
      if op == "jump":
         fixups.append((pos, g("to"), w))
         return [(OP["jump"], 0, 0, 0)]
      if op == "if":
         fixups.append((pos, g("to"), w))
         cid = self.named(self.conditions, "condition", g("cond"), w)
         return [(OP["if"], cid, 0, 1 if g("not", False) else 0)]
      if op == "repeat":
         return [(OP["repeat"], 0, u(g("count"), U16_BITS, w, "count"), 0)]
      if op == "for_each":
         lid = self.named(self.lists, "list", g("list"), w)
         return [(OP["for_each"], lid, u(g("max", 0), U16_BITS, w, "max"), 0)]
      if op == "goto":
         return [(OP["goto"], self.target(g("to"), w), 0, 0)]
      raise ManifestError(f"{w}: unhandled op {op}")

   # ---- combos ----
   def compile_combo(self, c: JsonDict, w: str) -> Combo:
      """!Compiles one combo.
      @param c  combo object
      @param w  location for error messages
      @return (held, pressed, target, cond)
      """
      unknown = set(c) - COMBO_KEYS
      if unknown:
         raise ManifestError(f"{w}: unknown keys {sorted(unknown)}")
      held, pressed = key_mask(c.get("held"), w), key_mask(c.get("pressed"), w)
      if held == 0 and pressed == 0:
         raise ManifestError(f"{w}: combo needs 'held' and/or 'pressed' keys")
      cond = self.named(self.conditions, "condition", c["cond"], w) if c.get("cond") is not None else 0
      return (held, pressed, self.target(c.get("to"), w), cond)

   @staticmethod
   def shadows(earlier: Combo, later: Combo) -> bool:
      """!Whether `earlier` matches whenever `later` does (so `later` never fires).
      @param earlier  compiled combo listed first
      @param later    compiled combo listed after it
      @return True if `later` is shadowed
      """
      eh, ep, _, ec = earlier
      lh, lp, _, _ = later
      # An unconditional combo shadows when its held keys are a subset of the later one's and its
      # pressed keys are none, or a superset of the later one's (non-empty) pressed keys.
      if ec != 0 or eh & ~lh:
         return False
      return ep == 0 or (lp != 0 and (lp & ~ep) == 0)

   def run(self) -> Compiler:
      """!Compiles every stage; fills cmds, tracks, stage_recs, combos and
      warnings.
      @return self
      """
      for s in self.stages:
         w = f"stage {s.get('name')!r}"
         unknown = set(s) - STAGE_KEYS
         if unknown:
            raise ManifestError(f"{w}: unknown keys {sorted(unknown)}")
         on = s.get("on", {})
         if set(on) - ON_KEYS:
            raise ManifestError(f"{w}: unknown 'on' keys {sorted(set(on) - ON_KEYS)}")
         flags = 0
         for f in s.get("flags", []):
            if f not in STAGE_FLAGS:
               raise ManifestError(f"{w}: unknown flag {f!r}")
            flags |= STAGE_FLAGS[f]
         tracks = s.get("tracks", [])
         if not 1 <= len(tracks) <= self.max_tracks:
            raise ManifestError(f"{w}: 1..{self.max_tracks} tracks required")
         self.used_tracks = max(self.used_tracks, len(tracks))
         track_first = len(self.tracks)
         for ti, t in enumerate(tracks):
            self.tracks.append(len(self.cmds))
            self.cmds.extend(self.compile_track(t, f"{w} track {ti}"))
         skip = key_mask(s.get("skip"), w)
         timeout = u(s.get("timeout", 0), U16_BITS, w, "timeout")
         if skip and "skip" not in on:
            raise ManifestError(f"{w}: 'skip' keys set but no on.skip target")
         if timeout and "timeout" not in on:
            raise ManifestError(f"{w}: 'timeout' set but no on.timeout target")
         if (flags & STAGE_FLAGS["exit_on_song_end"]) and "song_end" not in on:
            raise ManifestError(f"{w}: exit_on_song_end needs an on.song_end target")
         if "song_end" in on and not flags & STAGE_FLAGS["exit_on_song_end"]:
            self.warnings.append(f"{w}: on.song_end ignored without the exit_on_song_end flag")
         ff = s.get("fast_forward")
         ff_mask = ff_speed = ff_cond = 0
         if ff is not None:
            if set(ff) - FF_KEYS:
               raise ManifestError(f"{w}: unknown fast_forward keys {sorted(set(ff) - FF_KEYS)}")
            ff_mask = key_mask(ff.get("keys"), w)
            ff_speed = u(ff.get("speed", DEFAULT_FF_SPEED), U8_BITS, w, "speed")
            if not ff_mask or not MIN_FF_SPEED <= ff_speed <= MAX_FF_SPEED:
               raise ManifestError(f"{w}: fast_forward needs keys and a speed of {MIN_FF_SPEED}..{MAX_FF_SPEED}")
            if ff.get("cond") is not None:
               ff_cond = self.named(self.conditions, "condition", ff["cond"], w)
         combos = [self.compile_combo(c, f"{w} combo {ci}") for ci, c in enumerate(s.get("combos", []))]
         for j, later in enumerate(combos):
            for i in range(j):
               if self.shadows(combos[i], later):
                  raise ManifestError(f"{w}: combo {j} can never fire (shadowed by combo {i}; "
                                      f"list more specific combos first)")
         combo_first = len(self.combos)
         if combo_first + len(combos) > MAX_COMBOS:
            raise ManifestError(f"more than {MAX_COMBOS} combos in total")
         self.combos.extend(combos)
         targets = tuple(self.target(on.get(k), w) for k in ("done", "skip", "timeout", "song_end"))
         if not combos and all(x == NO_TARGET for x in targets):
            self.warnings.append(f"{w}: stage has no exit (relies on goto commands)")
         self.stage_recs.append((track_first, len(tracks), flags, skip, timeout, *targets,
                                 combo_first, len(combos), ff_mask, ff_speed, ff_cond))
      for i, s in enumerate(self.stages):
         if i not in self.referenced:
            self.warnings.append(f"stage {s['name']!r} is unreachable")
      if len(self.cmds) > MAX_COMMANDS:
         raise ManifestError("too many commands")
      return self

   def blobs(self) -> tuple[bytes, bytes, bytes]:
      """!Packs the tables as the engine structs lay them out (size lock).
      @return (command bytes, stage bytes, combo bytes)
      """
      cmd = b"".join(struct.pack("<BBHI", *c) for c in self.cmds)
      stage = b"".join(struct.pack("<HBBHHBBBBBBHBBxx", *s) for s in self.stage_recs)
      combo = b"".join(struct.pack("<HHBBxx", *c) for c in self.combos)
      assert len(cmd) == CMD_BYTES * len(self.cmds)
      assert len(stage) == STAGE_BYTES * len(self.stage_recs)
      assert len(combo) == COMBO_BYTES * len(self.combos)
      return cmd, stage, combo

   def rom_bytes(self) -> int:
      """!ROM the compiled tables take.
      @return bytes
      """
      cmd, stage, combo = self.blobs()
      return len(cmd) + len(stage) + len(combo) + TRACK_ENTRY_BYTES * len(self.tracks)

   def emit_inc(self, symbol: str) -> str:
      """!Generates the .inc with the tables, exit/condition/list ids and
      debug names.
      @param symbol  C++ symbol prefix (e.g. kSeqTitle)
      @return the source text
      """
      cmd, stage, combo = self.blobs()
      ns = "capsule::seq"
      L = [f"// Generated by sequence_compiler.py from '{self.id}'. Do not edit.",
           f"// ROM: {len(cmd)} B commands, {len(stage)} B stages, {len(combo)} B combos, "
           f"{TRACK_ENTRY_BYTES * len(self.tracks)} B track table.",
           "#pragma once", '#include "capsule/seq/sequence.h"', "",
           f"static_assert({ns}::kMaxTracks >= {self.used_tracks}, \"'{self.id}' needs more sequence tracks\");",
           f"static_assert({ns}::kMaxSprites >= {self.used_sprite}, \"'{self.id}' needs more sequence sprite slots\");",
           f"static_assert({ns}::kMaxAffine >= {self.used_affine}, \"'{self.id}' needs more affine sprite slots\");",
           ""]
      L.append(f"inline constexpr {ns}::Cmd {symbol}Cmds[] = {{")
      for op, a, b, c in self.cmds:
         L.append(f"    {{static_cast<{ns}::Op>({op}), {a}, {b}u, {c}u}},  // {OPS[op]}")
      L.append("};")
      L.append(f"inline constexpr uint16_t {symbol}Tracks[] = {{{', '.join(map(str, self.tracks))}}};")
      L.append(f"inline constexpr {ns}::Stage {symbol}Stages[] = {{")
      for s, src in zip(self.stage_recs, self.stages):
         L.append("    {" + ", ".join(str(v) for v in s) + ", 0},  // " + src["name"])
      L.append("};")
      combos_ref = "nullptr"
      if self.combos:
         L.append(f"inline constexpr {ns}::Combo {symbol}Combos[] = {{")
         for held, pressed, target, cond in self.combos:
            L.append(f"    {{{held}, {pressed}, {target}, {cond}, 0}},")
         L.append("};")
         combos_ref = f"{symbol}Combos"
      L.append(f"inline constexpr {ns}::Sequence {symbol} = {{")
      L.append(f"    {symbol}Stages, {symbol}Tracks, {symbol}Cmds, {combos_ref}, "
               f"{len(self.stage_recs)}, {self.start}}};")
      L.append("")
      ident = lambda s: re.sub(r"\W", "_", s).upper()
      for i, name in enumerate(self.exits):
         L.append(f"inline constexpr uint8_t {symbol}Exit_{ident(name)} = {i};")
      for i, name in enumerate(self.conditions):
         L.append(f"inline constexpr uint8_t {symbol}Cond_{ident(name)} = {i + 1};")
      for i, name in enumerate(self.lists):
         L.append(f"inline constexpr uint8_t {symbol}List_{ident(name)} = {i + 1};")
      # Names for test apps and debuggers. inline constexpr: nothing reaches the
      # ROM unless a build references them.
      cstr = lambda s: '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'
      L.append("")
      L.append("// Debug names (test apps); not in the ROM unless referenced. "
               "Exit, condition and list tables end with nullptr.")
      L.append(f"inline constexpr const char* {symbol}StageNames[] = "
               f"{{{', '.join(cstr(st['name']) for st in self.stages)}}};")
      for label, names in (("Exit", self.exits), ("Cond", self.conditions), ("List", self.lists)):
         L.append(f"inline constexpr const char* {symbol}{label}Names[] = "
                  f"{{{', '.join([cstr(n) for n in names] + ['nullptr'])}}};")
      return "\n".join(L) + "\n"


def main(argv: list[str]) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return process exit status
   """
   argv = list(argv)
   try:
      page = text_encoder.option(argv)
   except text_encoder.TextError as e:
      print(f"error: {e}", file=sys.stderr)
      return 1
   if len(argv) < MIN_ARGUMENTS:
      print(__doc__)
      return EXIT_USAGE
   opts = {"--symbol": "kSequence", "--max-tracks": str(DEFAULT_MAX_TRACKS), "--max-sprites": str(DEFAULT_MAX_SPRITES),
           "--max-affine": str(DEFAULT_MAX_AFFINE)}
   for k in opts:
      if k in argv:
         opts[k] = argv[argv.index(k) + 1]
   try:
      with open(argv[1], encoding="utf-8") as f:
         comp = Compiler(json.load(f), int(opts["--max-tracks"]), int(opts["--max-sprites"]),
                         int(opts["--max-affine"]), codepage=page).run()
      out = comp.emit_inc(opts["--symbol"])
   except (ManifestError, KeyError, TypeError, ValueError, AttributeError) as e:
      print(f"{argv[1]}: error: {e}", file=sys.stderr)
      return 1
   for wmsg in comp.warnings:
      print(f"{argv[1]}: warning: {wmsg}", file=sys.stderr)
   dst = Path(argv[OUTPUT_ARGUMENT])
   dst.parent.mkdir(parents=True, exist_ok=True)
   dst.write_text(out, encoding="utf-8")
   print(f"{argv[1]}: {len(comp.stage_recs)} stages, {len(comp.cmds)} cmds, {comp.rom_bytes()} B ROM")
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv))
