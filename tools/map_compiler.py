#!/usr/bin/env python3
"""!Compile a world (capsule.world/1) with its tilesets and maps into the C++
map tables (capsule::map::kWorld).

    map_compiler.py <world.json> <out.cpp> [--depfile <out.d>] [--debug-names] [--ids <out.h>]
                    [--world-ids <world_ids.h>] [--scripts <scripts.json>] [--code-page <page.json>]

--code-page names the game's code page (tools/text_encoder.py), used for
location names, the scripts the events name and the location previews;
default the engine's ASCII page.

Follows the manifest rule (P-1): editor data stripped, unknown keys rejected,
names resolved to numbers, sizes checked against the engine (C-03a/b/c, E-7).

World (`capsule.world/1`):
  - vehicle_classes: up to 8 movement modes (A2, D-22 decision 5); class 0
    is on foot. Each is a name (on-foot defaults) or {"name", "speeds":
    {"walk", "run"} (pixels per frame: 1, 2, 4 or 8; default 1 and 2, the
    walker's capsule::field::kWalkSpeed / kRunSpeed), "can_run" (default
    true), "first_animation" (the sprite animation its set starts at, a
    multiple of 4, default 0; see capsule/field/field.h), "passable" (the
    behavior names it may enter; default every behavior that lets it),
    "dismount" (F2, default false; not class 0: stepping onto a cell only
    class 0 enters changes the walker to class 0, leaving the water),
    "turn_frames" (I3: frames a turn in place takes, default 0 = the
    field's 6), "bumps" (I3: a blocked step walks in place for a step's
    time), "keeps_moving" (I3: arriving with another direction held steps
    on without turning)}. Scripts change the class with `vehicle NAME;`.
  - behaviors: terrain classes, index 0 first ("normal"). Each may be solid,
    passable only by some vehicle classes, or move the walker: "slide"
    (keep going until blocked), "ledge": DIR (jump over it moving DIR, a wall
    otherwise), "push": DIR (forced step). "walls" (I2, optional): the
    cell's sides that are walls (directions): a step leaving it through one,
    or entering it through one, is blocked (one-way fences). "counter" (I3):
    true: pressing A facing it talks to the object beyond. A class enters a behavior when
    both the behavior's "passable" and the class's "passable" allow it.
  - elevations: the 16 levels of a cell's elevation (A1), each {"name",
    "meets": "all" | "same" | [levels, by number or name] (a level always
    meets itself; two levels meet when either names the other; default
    "same"), "on_arrive": "take" (the mover takes the level, default) |
    "keep" (it keeps its own: bridges), "priority": the sprite priority of
    a mover on it (0-3, default 2)}. Without it the default rule applies:
    0 meets all, 15 meets all and keeps, 1-14 meet themselves.
  - elevation_priority (older form, only without elevations): 16 sprite
    priorities, one per level, for the default rule.
  - tilesets / maps: name -> file (relative to the world file).
  - start: {"map", "x", "y", "facing"}.
  - map_groups (optional): how the maps are organised, a tree of groups
    {"name", "maps": [map names], "groups": [subgroups]}. Each map is in at
    most one group (maps in none are ungrouped); group names are unique among
    their siblings. Checked here, not compiled: the runtime does not use it
    yet (warps and connections will name maps).
  - battle, encounter_methods, encounter_conditions, encounter_tables
    (optional): encounters, see tools/encounter_compiler.py. --ids
    writes the method and condition identifiers (encounter_ids.h).
  - object_kinds, map_tags (F2, D-27 section 3; optional): names of the
    kinds map objects may have (a bush, a boulder: up to 254) and of the
    tags maps may carry (dark, cave, outdoors: up to 16). Field actions of
    the battle manifest's structure target behaviors, object kinds and map
    tags by name (the script compiler resolves them).
  - flight_points (F2, optional): up to 32 {"name", "label", "map", "x",
    "y", "facing"} places a flight menu lists once recorded (the script
    compiler gives each an implicit permanent flag flight_<name>; scripts:
    `flight record NAME;`, `flight visit NAME;`, `flight menu -> var(v);`).
  - draw_order (DS1, D-30 section 4, optional): the rule giving every object's
    sprite its sub-priority each frame from its bottom edge on screen and its
    elevation: {"elevation_base": 16 values, "bias", "row_pixels" (a power of
    two), "rows", "row_step", "add", "wrap" (a power of two up to 256)}:
    base[elevation] + (rows - ((bottom + bias) & (wrap - 1)) / row_pixels) *
    row_step + add; lower draws in front (sprites sort by priority, then
    sub-priority, the lower edge first on a tie). Missing keys take the
    defaults (base 0, bias 8, row_pixels 16, rows 16, row_step 2, add 1,
    wrap 256).
  - doors (DS1, D-30 section 1, optional): door bindings by metatile,
    [{"tileset": NAME or "*" (any of a map's tilesets), "metatile": NAME,
    "door": "door:KEY"}]: every cell showing that metatile (on any layer) is
    that door's anchor (its bottom-left cell). A warp's "door": "door:KEY"
    (tools/event_compiler.py) binds its own cell and wins over a metatile.
  - interactions (C13b, optional): what A facing a cell does when no object
    and no sign answers, in order (the first rule that holds starts its
    script): [{"behavior": NAME, "script": SCRIPT, "facing": DIRECTION
    (optional: any), "vehicle": CLASS (optional: any), "flag": FLAG (optional:
    it must be set), "same_level": true (optional: the faced level must be
    the cell's), "after_actions": true (optional: tried only after the field
    actions had nothing)}] (capsule::map::Interaction).

Tileset (`capsule.tileset/2`):
  - sheets: 8x8-tile sources, each drawn from shapes ("size" + "shapes",
    explicit palette) or read from a PNG ("image"), with one palette: a
    list of up to 15 colors, "auto" (from the images), or "@sheet" to share
    another sheet's. An image sheet may name a "transparent" color
    ("#rrggbb", matched exactly in the source image): those pixels become
    transparent and take no palette entry, like pixels with low alpha. Tiles
    are numbered row by row.
  - metatiles: 16x16 graphics, one layer of 2x2 tiles: {"name", "tiles":
    [top-left, top-right, bottom-left, bottom-right]}. Tile references are
    "sheet:index" with an optional "h", "v" or "hv" flip suffix; null is
    transparent. Metatiles carry no behavior: that is painted on the map.
  - animations: replace "count" tiles starting at a tile reference with
    successive frames from another sheet (frame k = its tiles k*count ...),
    every "period" display frames. "phase" (optional, 0 to period - 1) is
    the frames into the period at map entry (the first frame change comes
    period - phase frames later); without it the compiler spreads the map's
    animations over their periods. I3: "from_sheet": true uploads no frame
    at entry (the tileset's own tiles show until the first change);
    "on_shift": keep (default) | reload (a seamless shift reloads its
    tiles: its own show until the next change) | restart (reloaded and
    started over); kept ones follow the map's animation clock.
    DS1 (D-30 section 5): "targets": [{"tiles" ("at" in a fixed tileset),
    "offset": 0 to frames - 1, "phase"}, ...] in place of "tiles" / "at":
    the same frames shown on several tile ranges, target k showing frame
    (t + phase_k) / period + offset_k; the frames are stored once. The
    compiler checks that no two targets of a tileset replace the same tile.
  - place (I2, optional): a fixed-placement tileset, for imported indexed
    graphics whose tilemap entries already name VRAM tiles and palette
    banks. {"sheet": the sheet loaded at "tile" (first VRAM tile), "tiles":
    VRAM tiles it owns, "palette": first palette bank, "palettes": banks it
    owns}. Its sheets are indexed images ({"name", "image", "indexed":
    true}: the PNG's palette indices as they are, 0-15); "palettes" lists
    its banks' colors (16 "#rrggbb" each, entry 0 included; the runtime
    keeps entries 1-15); metatiles give raw tilemap entries ({"name",
    "entries": [4 x (VRAM tile | h flip 0x400 | v flip 0x800 | bank << 12)]},
    any tile of any fixed tileset the map loads); animations name "at" (the
    first tile, counted from "tile") instead of "tiles". Maps drawing from
    fixed tilesets load each of their tilesets at its place, without
    renumbering or deduplication; their tilesets' places must not overlap.

Map (`capsule.map/2`, C-01b):
  - tilesets: the world tilesets the map draws from (optional; editors offer
    their metatiles). Every dictionary entry must come from one of them.
  - layers: up to four tile layers, each {"bg": 0-3, "priority": 0-3,
    "content": "tilemap", "purpose": ...}; one BG per layer. Draw priority
    is independent of the BG number (0 = front; default: the BG number).
  - metatiles: the map's dictionary, "tileset/metatile" names; layer cells
    index it (up to MAX_METATILES entries). "all" (I2) is every metatile of
    the map's tilesets, in their order (tileset by tileset).
  - tilesets also says what a map of fixed tilesets loads: all of them, at
    their places, whether its dictionary names their metatiles or not.
  - behaviors: the map's behavior dictionary, world behavior names
    (default: the world's behavior 0); attribute cells index it.
  - chunks: sparse 8x8-metatile chunks {"at": [cx, cy], "cells": 8 rows of
    8 attribute cells, "layers": per tile layer 8 rows of 8 dictionary
    indices or null (nothing on that layer)}. An attribute cell is null
    (unpainted: the border shows, impassable) or behavior index |
    env << 8 | collision << 16 | elevation << 18 | encounter << 22 |
    encounter slot << 23 (the runtime layout, with the map's behavior index
    in place of the world's). The slot (0-15) picks which of the map's
    encounter tables a cell uses.
  - border: {"size": [w, h], "layers": per tile layer w*h dictionary
    indices or null, "phase": [x, y]} fills everything outside the chunks
    and every unpainted cell; it is always impassable.
  - A map may be blank (no metatiles, chunks or layers); the compiler then
    emits one unpainted chunk at 0,0.
  - backdrop: color behind every layer.
  - can_run (optional, default true): false forbids running on the map.
  - darkness (F2, optional, 0 to the world's darkest level, default 0):
    how dark the map is on entering (a lit box around the walker, the rest
    darkened; a script's `darkness N;` changes it until the next map, kept
    across a save: C8). The world's darkness_levels (C8, optional, at most
    MAX_DARKNESS) say what level n looks like: {"lit": half size of the
    lit box in pixels, "brightness": 0-16 of the rest}; without it the
    default levels DEFAULT_DARKNESS_LEVELS apply.
  - tags (F2, optional): world map_tags the map carries (field actions'
    menu targets: a light action on dark maps, an escape action in caves).
  - escape_point (F2, optional, default false): the map is outdoors: a warp
    from it to a map without escape_point records the escape point (where
    the walker stood before the warp; scripts' `escape;` returns there).
  - object_view (I3, optional): [left, right, up, down] cells around the
    walker its objects spawn in (they go when both their cell and their
    template cell are outside); with it a map may list up to 64 objects,
    without it at most 15, all spawned at entry. stacked_objects (I3,
    optional): objects may share a cell (data whose flags keep one there).
    unpainted_events (I3, optional, default false): its warps, triggers and
    objects may stand on unpainted cells of its chunks (objects outside its
    chunks too), and warps may land on them (imported data kept as its
    source has it; the walker never steps onto such a cell by itself).
  - encounters (optional): the map's capsule.encounters/1 file, see
    tools/encounter_compiler.py.
  - area, events (optional): the map's area root (B-2) and its warps,
    triggers and signs (B-3), see tools/event_compiler.py.
  - override_sets (DS1, D-30 section 2, optional): named cell overrides
    scripts apply and clear (`override apply NAME;` / `override clear NAME;`):
    [{"name" (unique in the world), "persist": {"flag": NAME} | {"var":
    NAME, "value": N} (absent: until the map is left), "cells": [{"cell":
    [x, y], "layer": n, "metatile": "tileset/name" | null, or "entries":
    [4 map entries]}]}]; a persisted set is applied again whenever the map is
    entered while its flag is set (its variable holds the value).

World, also: scripts (the scripts manifest events name scripts, flags and
variables from) and step_hooks (the step dispatcher), see
tools/event_compiler.py. --world-ids writes world_ids.h (MAP_..., STEP_HOOK_...).
"""
from __future__ import annotations

import json
import sys
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import asset_compiler as ac  # noqa: E402
import bitmap  # noqa: E402
import manifest_paths  # noqa: E402
import encounter_compiler as ec  # noqa: E402
import event_compiler as evc  # noqa: E402
import object_compiler as obc  # noqa: E402
import location_compiler as lc  # noqa: E402
import text_encoder  # noqa: E402
from sequence_compiler import ManifestError as SequenceManifestError  # noqa: E402
from sequence_compiler import asset_key  # noqa: E402
from sequence_compiler import BLUE_SHIFT as SEQUENCE_BLUE_SHIFT, GREEN_SHIFT as SEQUENCE_GREEN_SHIFT  # noqa: E402

## Metatiles per chunk side (C-03a).
CHUNK = 8
## Highest metatile id; the value itself marks an unpainted cell.
UNDEFINED = 0x3FF
## Most entries of a map's metatile dictionary (X1: UNDEFINED, 1,023; I2: 4,095, for imported maps whose two
## tilesets offer 1,024 two-layer metatiles, each one engine metatile per layer).
MAX_METATILES = 0xFFF
## 4bpp tiles the map's character blocks hold (gfx plan: blocks 0-1).
MAX_TILES = 1024
## Background palette banks a map may use (13-15 are the UI's).
MAX_PALETTES = 13
## Most tile animations per map.
MAX_ANIMATIONS = 32
## Directions, in capsule::field::Direction order.
DIRECTIONS = ["down", "up", "left", "right"]
## Behavior kinds, in capsule::map::BehaviorKind order.
KINDS = ["none", "slide", "ledge", "push", "counter"]
## Tile layers per map (capsule::map::kMaxLayers).
MAX_LAYERS = 4
## Layer content types (C-01b); only tilemaps are built so far.
LAYER_CONTENT = ["tilemap"]
## Layer purposes (C-01b), for tools and scripts.
LAYER_PURPOSE = ["unused", "terrain", "geometry", "canopy", "infrastructure", "backdrop", "environment_effect"]
## No metatile on a layer (capsule::map::kNoMetatile).
NO_METATILE = 0xFFFF
## An unpainted attribute cell (capsule::map::kCellUndefined).
CELL_UNDEFINED = 0xFFFFFFFF
## What border cells read as (capsule::map::kCellBorder): impassable, behavior 0.
CELL_BORDER = 0x00030000
## Attribute cell fields: (mask, shift).
CELL_FIELDS = {"behavior": (0xFF, 0), "env": (0xFF, 8), "collision": (0x3, 16), "elevation": (0xF, 18),
               "encounter": (0x1, 22), "slot": (0xF, 23)}
## Bits an attribute cell may use.
CELL_BITS = 0x07FFFFFF
## Tileset schema this compiler reads.
TILESET_SCHEMA = "capsule.tileset/2"
## Map schema this compiler reads.
MAP_SCHEMA = "capsule.map/2"

## Deepest map group nesting.
MAX_GROUP_DEPTH = 8

## Cells per side of a chunk, as a shift (CHUNK = 1 << CHUNK_SHIFT); the events' chunks are the same.
CHUNK_SHIFT = evc.CHUNK_SHIFT
## Mask of a cell's position within its chunk.
CHUNK_MASK = CHUNK - 1
## Cells per chunk.
CHUNK_CELLS = CHUNK * CHUNK
## Lowest chunk coordinate.
MIN_CHUNK_COORD = -2048
## Highest chunk coordinate.
MAX_CHUNK_COORD = 2047
## Chunk positions a map's grid may span (64x64).
MAX_CHUNK_POSITIONS = 4096
## Grid entry of a position without a chunk.
NO_CHUNK = 0xFFFF
## Chunk tuple field: attribute cells (after cx, cy).
CHUNK_CELLS_FIELD = 2
## Chunk tuple field: per-layer metatile grids.
CHUNK_GRIDS_FIELD = 3
## Behavior index bits of an attribute cell.
BEHAVIOR_MASK = CELL_FIELDS["behavior"][0]
## Numbers in an [x, y] or [w, h] pair.
AXES = 2
## Pixels per side of a tile.
TILE_SIZE = ac.TILE_SIZE
## Widest or tallest shape sheet, in pixels.
MAX_SHEET_SIZE = 1024
## Tilemap entry bit: horizontal flip.
FLIP_H = ac.H_FLIP
## Tilemap entry bit: vertical flip.
FLIP_V = ac.V_FLIP
## Bit position of the palette bank in a tilemap entry.
PALETTE_SHIFT = 12
## Bit position of green in a BGR555 color.
GREEN_SHIFT = SEQUENCE_GREEN_SHIFT
## Bit position of blue in a BGR555 color.
BLUE_SHIFT = SEQUENCE_BLUE_SHIFT
## Mask of one 5-bit color channel.
CHANNEL_MASK = ac.CHANNEL_MASK
## Tiles per metatile (2x2: top-left, top-right, bottom-left, bottom-right).
METATILE_TILES = 4
## Largest raw tilemap entry (16 bits: tile, flips, palette bank).
MAX_ENTRY = 0xFFFF
## Colors of one palette bank, entry 0 included (a fixed tileset's "palettes").
BANK_COLORS = 16
## Largest pixel value of a 4bpp tile (an indexed sheet's palette indices).
MAX_PIXEL_4BPP = BANK_COLORS - 1
## Most tiles one animation replaces.
MAX_ANIMATION_TILES = 32
## Fewest frames of an animation.
MIN_ANIMATION_FRAMES = 2
## Most frames of an animation (an 8-bit count).
MAX_ANIMATION_FRAMES = 255
## Display frames between animation frames when none are given.
DEFAULT_ANIMATION_PERIOD = 16
## Longest animation period (an 8-bit count).
MAX_ANIMATION_PERIOD = 255
## Highest BG number (BG0-3).
MAX_BG = 3
## Highest GBA display priority (BG and sprite priorities are 0-3, 0 = front).
MAX_PRIORITY = 3
## Behaviors per world and per map dictionary (8-bit indices).
MAX_BEHAVIORS = 256
## Movement classes per world (an 8-bit passable mask).
MAX_VEHICLE_CLASSES = 8
## Cell elevations (4 bits), one sprite priority each (capsule::map::kElevationLevels).
ELEVATIONS = 16
## Sprite priority of an elevation when the world gives none (capsule::map::kWalkerPriority).
DEFAULT_SPRITE_PRIORITY = 2
## Elevation meet mask of every level (capsule::map::kMeetsAll).
MEETS_ALL = (1 << ELEVATIONS) - 1
## Default rule: the level that meets every level (capsule::map::kElevationAny, stairs).
ELEVATION_ANY = 0
## Default rule: the level that meets every level and keeps the mover's (capsule::map::kElevationMulti, bridges).
ELEVATION_MULTI = ELEVATIONS - 1
## Elevation flag: arriving keeps the mover's level (capsule::map::kElevationKeep).
ELEVATION_KEEP = 1
## Elevation "meets" words.
MEETS_WORDS = ("all", "same")
## Elevation "on_arrive" words, in flag order (take = 0, keep = ELEVATION_KEEP).
ON_ARRIVE = ("take", "keep")
## Longest turn in place of a vehicle class, frames (I3; an 8-bit count).
MAX_TURN_FRAMES = 255
## Animation flag bits (capsule::map::kAnimFromSheet, kAnimReloadOnShift, kAnimRestartOnShift).
ANIM_FROM_SHEET, ANIM_RELOAD_ON_SHIFT, ANIM_RESTART_ON_SHIFT = 1, 2, 4
## An animation's "on_shift" values and their flag bits.
ON_SHIFT = {"keep": 0, "reload": ANIM_RELOAD_ON_SHIFT, "restart": ANIM_RELOAD_ON_SHIFT | ANIM_RESTART_ON_SHIFT}
## Sides of an object view (left, right, up, down).
VIEW_SIDES = 4
## Most cells an object view reaches on a side (an int8 count).
MAX_VIEW_CELLS = 127
## Pixels per frame a vehicle class walks at by default (capsule::map::kDefaultWalkSpeed).
DEFAULT_WALK_SPEED = 1
## Pixels per frame a vehicle class runs at by default (capsule::map::kDefaultRunSpeed).
DEFAULT_RUN_SPEED = 2
## Speeds a vehicle class may use, pixels per frame (each divides a 16-pixel cell).
VEHICLE_SPEEDS = (1, 2, 4, 8)
## Vehicle class flag: it may run (capsule::map::kVehicleCanRun).
VEHICLE_CAN_RUN = 1
## Sprite animations per direction set (capsule/field/field.h: standing 0-3, walking 4-7, running 8-11).
ANIMATION_SET = 4
## Highest first animation of a vehicle class (an 8-bit animation index).
MAX_FIRST_ANIMATION = 255
## Map flag: running is not allowed (capsule::map::kMapNoRun).
MAP_NO_RUN = 1
## Map flag: warps leaving the map record the escape point (capsule::map::kMapEscapePoint; F2).
MAP_ESCAPE_POINT = 2
## Most darkness levels a world defines (capsule::map::kMaxDarkness; F2, C8: the levels are the world's data).
MAX_DARKNESS = 15
## Widest lit box half size of a darkness level, pixels (capsule::map::DarknessLevel::lit_half, a u8).
MAX_LIT_HALF = 255
## Darkest brightness of a darkness level (the Darken blend's coefficient, 0-16; capsule::map::DarknessLevel).
MAX_DARK_BRIGHTNESS = 16
## The darkness levels of a world that states none (C8: F2's table, the Starter's and the sandbox's): level n
## (1 up) is entry n - 1: the lit box's half size around the walker (pixels) and how dark the rest goes.
DEFAULT_DARKNESS_LEVELS = [{"lit": 72, "brightness": 6}, {"lit": 56, "brightness": 10},
                           {"lit": 40, "brightness": 13}, {"lit": 24, "brightness": 16}]
## Map tags of a world (capsule::map::kMaxMapTags: a 16-bit mask; F2).
MAX_MAP_TAGS = 16
## Object kinds of a world (an 8-bit kind, 0 = none; F2).
MAX_OBJECT_KINDS = obc.MAX_KINDS
## Flight points of a world (capsule::script::kMaxFlightPoints; F2).
MAX_FLIGHT_POINTS = 32
## Longest flight point label (letters).
MAX_FLIGHT_LABEL = 16
## Vehicle class flag: stepping onto a cell only class 0 enters makes the walker class 0 (kVehicleDismount; F2).
VEHICLE_DISMOUNT = 2
## Largest border pattern side, in metatiles.
MAX_BORDER_SIZE = 8
## Lowest start position coordinate (an int16).
MIN_COORD = -32768
## Highest start position coordinate (an int16).
MAX_COORD = 32767
## DS1: most more targets of a map's animations (capsule::map::kMaxAnimTargets).
MAX_ANIM_TARGETS = 64
## DS1: bytes of one TileAnimation record on the GBA (a pointer and 8 bytes).
TILE_ANIMATION_BYTES = 12
## DS1: bytes of one AnimTarget record.
ANIM_TARGET_BYTES = 4
## DS1: bytes of one 4bpp tile.
TILE_BYTES = 32
## DS1: override sets per map (capsule::map::kMaxMapOverrideSets: a 32-bit mask).
MAX_MAP_OVERRIDE_SETS = 32
## DS1: override cells, sets and entry groups of a world (16-bit indices).
MAX_OVERRIDE_ITEMS = 0xFFFF
## DS1: OverrideCell::flags bit: the value indexes the world's override entries (capsule::map::kOverrideEntries).
OVERRIDE_ENTRIES = 1
## DS1: capsule::map::OverridePersist names, in order.
OVERRIDE_PERSISTS = ("Map", "Flag", "Var")
## DS1: lowest and highest value of an override set's variable (an int16).
MIN_OVERRIDE_VALUE, MAX_OVERRIDE_VALUE = -32768, 32767
## DS1: door bindings per map (an 8-bit count).
MAX_DOOR_BINDINGS = 255
## C13b: most cell interaction rules (16-bit count).
MAX_INTERACTIONS = 0xFFFF
## C13b: capsule::map::kAnyDirection, kAnyVehicle, kNoEventCondition.
ANY_DIRECTION, ANY_VEHICLE, NO_CONDITION = 4, 0xFF, 0xFFFF
## C13b: capsule::map::kInteractSameLevel, kInteractAfterActions.
INTERACT_SAME_LEVEL, INTERACT_AFTER_ACTIONS = 1, 2
## DS1: DoorBinding::x of a metatile binding (capsule::map::kDoorAnyCell).
DOOR_ANY_CELL = -32768
## DS1: a door binding's tileset that matches any of the map's tilesets.
ANY_TILESET = "*"
## DS1: the draw-order rule of a world without one (capsule::map::kDefaultDrawOrder).
DEFAULT_DRAW_ORDER = {"elevation_base": [0] * ELEVATIONS, "bias": 8, "row_pixels": 16, "rows": 16, "row_step": 2,
                      "add": 1, "wrap": 256}
## DS1: a draw-order rule's 8-bit fields and bias range.
MAX_U8, MIN_I8, MAX_I8 = 255, -128, 127
## DS1: largest wrap of a draw-order rule (an 8-bit mask: 256 rows of pixels).
MAX_DRAW_WRAP = 256
## Values per line of an emitted 32-bit array.
WORDS_PER_LINE = 8
## Values per line of an emitted 16-bit array.
HALFWORDS_PER_LINE = 16
## Bits per byte of the self switch bit set.
BITS_PER_BYTE = 8
## Exit status of a command line that is not understood.
EXIT_USAGE = 2
## Command-line words of an option with a value (the flag and its file).
OPTION_WORDS = 2
## Positional command-line arguments (world.json and out.cpp).
POSITIONAL_ARGS = 2

## A parsed JSON value.
Json = Any
## In-memory manifests replacing files (resolved Path -> parsed JSON), as editors pass them.
Sources = Mapping[Path, Json]
## An image: (width, height, RGBA bytes).
Image = tuple[int, int, Any]
## One tile: 8 rows of 8 palette indices.
Tile = tuple[tuple[int, ...], ...]
## A resolved tile reference: (sheet, index, flip bits).
TileRef = tuple[str, int, int]

## Allowed keys per manifest object.
KEYS = {
   "world": {"schema", "id", "vehicle_classes", "behaviors", "elevations", "elevation_priority", "tilesets", "maps",
             "start",
             "map_groups", "battle", "encounter_methods", "encounter_conditions", "encounter_tables", "scripts",
             "step_hooks", "transition", "banner_styles", "locations", "object_kinds", "map_tags", "flight_points",
             "darkness_levels", "draw_order", "doors", "interactions"},
   "interaction": {"behavior", "script", "facing", "vehicle", "flag", "same_level", "after_actions"},
   "draw_order": {"elevation_base", "bias", "row_pixels", "rows", "row_step", "add", "wrap"},
   "door_binding": {"tileset", "metatile", "door"},
   "darkness_level": {"lit", "brightness"},
   "map_group": {"name", "maps", "groups"},
   "elevation": {"name", "meets", "on_arrive", "priority"},
   "vehicle_class": {"name", "speeds", "can_run", "first_animation", "passable", "dismount", "turn_frames", "bumps",
                     "keeps_moving"},
   "speeds": {"walk", "run"},
   "behavior": {"name", "solid", "passable", "slide", "ledge", "push", "walls", "counter"},
   "start": {"map", "x", "y", "facing"},
   "tileset": {"schema", "id", "sheets", "metatiles", "animations", "place", "palettes"},
   "place": {"sheet", "tile", "tiles", "palette", "palettes"},
   "sheet": {"name", "palette", "image", "size", "shapes", "transparent", "indexed"},
   "fixed_sheet": {"name", "image", "indexed"},
   "metatile": {"name", "tiles"},
   "fixed_metatile": {"name", "entries"},
   "animation": {"name", "tiles", "count", "frames", "period", "phase", "from_sheet", "on_shift", "targets"},
   "fixed_animation": {"name", "at", "count", "frames", "period", "phase", "from_sheet", "on_shift", "targets"},
   "anim_target": {"tiles", "offset", "phase"},
   "fixed_anim_target": {"at", "offset", "phase"},
   "map": {"schema", "id", "tilesets", "layers", "metatiles", "behaviors", "chunks", "border", "backdrop",
           "encounters", "area", "events", "objects", "location", "can_run", "darkness", "tags", "escape_point",
           "object_view", "stacked_objects", "unpainted_events", "override_sets"},
   "override_set": {"name", "persist", "cells"},
   "persist": {"flag", "var", "value"},
   "override_cell": {"cell", "layer", "metatile", "entries"},
   "layer": {"bg", "priority", "content", "purpose"},
   "chunk": {"at", "cells", "layers"},
   "border": {"size", "layers", "phase"},
   "flight_point": {"name", "label", "map", "x", "y", "facing"},
}


class ManifestError(Exception):
   """!A manifest rule was broken; the message says where and how."""
   pass


def check_keys(obj: Json, kind: str, where: str) -> None:
   """!Rejects unknown keys (and non-objects).
   @param obj    the object
   @param kind   key set name in KEYS
   @param where  location for error messages
   @return None
   """
   if not isinstance(obj, dict):
      raise ManifestError(f"{where}: expected an object")
   unknown = sorted(set(obj) - KEYS[kind])
   if unknown:
      raise ManifestError(f"{where}: unknown key(s) {', '.join(unknown)}")


def integer(value: Json, lo: int, hi: int, where: str) -> int:
   """!Checks an integer range.
   @param value  the value
   @param lo     minimum
   @param hi     maximum
   @param where  location for the error message
   @return the value
   """
   if not isinstance(value, int) or isinstance(value, bool) or not lo <= value <= hi:
      raise ManifestError(f"{where}: expected an integer {lo}-{hi}, got {value!r}")
   return value


def load_json(path: str | Path, where: str, sources: Sources | None = None) -> Json:
   """!Reads and strips a manifest file.
   @param path     file
   @param where    location for error messages
   @param sources  resolved Path -> already parsed JSON to use instead of the
                   file (an editor's unsaved documents), or None
   @return the parsed, stripped JSON
   """
   if sources is not None and Path(path).resolve() in sources:
      return ac.strip(sources[Path(path).resolve()])
   try:
      return ac.strip(json.loads(Path(path).read_text(encoding="utf-8")))
   except OSError as e:
      raise ManifestError(f"{where}: cannot read {path}: {e.strerror}") from None
   except json.JSONDecodeError as e:
      raise ManifestError(f"{where}: {path}: {e}") from None


# -- tilesets -------------------------------------------------------------------------

class Sheet:
   """!One tile sheet after drawing: indexed pixels plus its palette group."""

   def __init__(self, name: str, group: str, px: list[list[int]]) -> None:
      """!Wraps drawn pixels.
      @param name   sheet name
      @param group  palette group key
      @param px     rows of indices
      """
      ## Sheet name within its tileset.
      self.name = name
      ## Palette group key ("tileset/sheet" of the sheet owning the palette).
      self.group = group
      ## Rows of palette indices (0 = transparent).
      self.px = px
      ## Width, tiles.
      self.w = len(px[0]) // TILE_SIZE if px else 0
      ## Height, tiles.
      self.h = len(px) // TILE_SIZE
      ## C10 (S1): tiles cut out so far, by number (a sheet's tile is the same for every map that loads it).
      self.cut: dict[int, Tile] = {}

   def tile(self, index: int, where: str) -> Tile:
      """!One tile.
      @param index  row-major tile number
      @param where  location for error messages
      @return 8 rows of 8 indices (tuples)
      """
      if not 0 <= index < self.w * self.h:
         raise ManifestError(f"{where}: sheet {self.name!r} has tiles 0-{self.w * self.h - 1}, not {index}")
      cut = self.cut   # a local name: Doxygen 1.12 reads `x in self.attr` as an undocumented member
      if index not in cut:
         tx, ty = index % self.w, index // self.w
         cut[index] = tuple(tuple(self.px[ty * TILE_SIZE + y][tx * TILE_SIZE:(tx + 1) * TILE_SIZE])
                            for y in range(TILE_SIZE))
      return cut[index]


class Tileset:
   """!A compiled tileset: drawn sheets, palette groups, metatiles, animations."""

   def __init__(self, name: str) -> None:
      """!An empty tileset.
      @param name  tileset name
      """
      ## Tileset name in the world.
      self.name = name
      ## name -> Sheet.
      self.sheets: dict[str, Sheet] = {}
      ## Palette group key -> (15 BGR555 colors, colors used).
      self.palettes: dict[str, tuple[list[int], int]] = {}
      ## name -> metatile dict (resolved refs).
      self.metatiles: dict[str, dict[str, Any]] = {}
      ## Animation dicts.
      self.animations: list[dict[str, Any]] = []
      ## I2: the fixed placement ({"sheet", "tile", "tiles", "palette", "palettes"}), or None.
      self.place: dict[str, Any] | None = None
      ## I2: a fixed tileset's banks, 16 BGR555 colors each (entry 0 included).
      self.bank_colors: list[list[int]] = []
      ## DS1: ROM its animations' more targets save (frames stored once instead of once per target).
      self.target_saved = 0


def parse_ref(ref: Json, ts: Tileset, where: str) -> TileRef | None:
   """!Resolves a tile reference.
   @param ref    "sheet:index" with optional h / v / hv suffix, or None
   @param ts     the Tileset
   @param where  location for error messages
   @return None for transparent, else (sheet, index, flip bits 0x400 / 0x800)
   """
   if ref is None:
      return None
   if not isinstance(ref, str) or ":" not in ref:
      raise ManifestError(f"{where}: tile references look like 'sheet:12' or 'sheet:12hv', got {ref!r}")
   sheet, rest = ref.split(":", 1)
   flip = 0
   while rest and rest[-1] in "hv":
      flip |= FLIP_H if rest[-1] == "h" else FLIP_V
      rest = rest[:-1]
   if sheet not in ts.sheets:
      raise ManifestError(f"{where}: unknown sheet {sheet!r} in {ref!r}")
   if not rest.isdigit():
      raise ManifestError(f"{where}: bad tile number in {ref!r}")
   index = int(rest)
   ts.sheets[sheet].tile(index, where)
   return sheet, index, flip


def compile_tileset(name: str, path: str | Path, behaviors: Mapping[str, int], deps: list[Path],
                    sources: Sources | None = None, images: Mapping[Path, Image] | None = None) -> Tileset:
   """!Draws a tileset's sheets and resolves its metatiles and animations.
   @param name       tileset name
   @param path       manifest file
   @param behaviors  behavior name -> index
   @param deps       list image paths are appended to
   @param sources    in-memory manifests (see load_json()), or None
   @param images     resolved Path -> (w, h, RGBA) to use instead of image files, or None
   @return Tileset
   """
   where = f"tileset {name!r}"
   m = load_json(path, where, sources)
   check_keys(m, "tileset", where)
   if m.get("schema") != TILESET_SCHEMA:
      raise ManifestError(f"{where}: schema must be '{TILESET_SCHEMA}'")
   ts = Tileset(name)
   base = Path(path).resolve().parent
   if "place" in m:
      return compile_fixed_tileset(ts, m, base, where, deps)
   if "palettes" in m:
      raise ManifestError(f"{where}: palettes belong to a fixed tileset (one with \"place\")")
   sheets = m.get("sheets", [])
   if not isinstance(sheets, list):   # an empty tileset is fine: it offers no metatiles yet
      raise ManifestError(f"{where}: sheets must be a list")
   specs = {}
   for i, s in enumerate(sheets):
      sw = f"{where} sheet {i}"
      check_keys(s, "sheet", sw)
      n = s.get("name")
      if not isinstance(n, str) or not n or ":" in n or "/" in n:
         raise ManifestError(f"{sw}: needs a name without ':' or '/'")
      if n in specs:
         raise ManifestError(f"{sw}: duplicate sheet {n!r}")
      specs[n] = s
   # Palette groups: a sheet owns one (list or "auto") or shares one ("@other").
   owner = {}
   for n, s in specs.items():
      p = s.get("palette")
      if isinstance(p, str) and p.startswith("@"):
         target = p[1:]
         if target not in specs or isinstance(specs[target].get("palette"), str) and \
                 specs[target]["palette"].startswith("@"):
            raise ManifestError(f"{where} sheet {n!r}: '@{target}' must name a sheet with its own palette")
         owner[n] = target
      else:
         owner[n] = n
   loaded = {}
   for n, s in specs.items():
      sw = f"{where} sheet {n!r}"
      if "image" in s:
         if "shapes" in s or "size" in s:
            raise ManifestError(f"{sw}: an image sheet has no size or shapes")
         p = s["image"]
         if not isinstance(p, str) or manifest_paths.is_absolute_text(p):
            raise ManifestError(f"{sw}: image paths are relative to the tileset")
         full = (base / p).resolve()
         deps.append(full)
         if images is not None and full in images:
            img = images[full]
         else:
            try:
               img = bitmap.read_png(full)
            except bitmap.BitmapError as e:
               raise ManifestError(f"{sw}: {e}") from None
         img_w, img_h, _ = img
         if img_w % TILE_SIZE or img_h % TILE_SIZE or not img_w or not img_h:
            raise ManifestError(f"{sw}: {p} is {img_w}x{img_h}; sizes are multiples of 8")
         if s.get("transparent") is not None:
            key = bitmap.parse_rgb(s["transparent"])
            if key is None:
               raise ManifestError(f"{sw}: transparent is a color like \"#ff00ff\", got {s['transparent']!r}")
            img = bitmap.key_out(img, key)
         loaded[n] = img
      elif s.get("transparent") is not None:
         raise ManifestError(f"{sw}: only image sheets take a transparent color (shape sheets use entry 0)")
   for n in specs:
      o = owner[n]
      p = specs[o].get("palette")
      gw = f"{where} sheet {o!r}"
      if p == "auto":
         members = [loaded[k] for k in specs if owner[k] == o and k in loaded]
         if len(members) != sum(1 for k in specs if owner[k] == o):
            raise ManifestError(f"{gw}: palette 'auto' needs every sheet sharing it to be an image")
         five = bitmap.make_palette(members)
         pal = [r | g << GREEN_SHIFT | b << BLUE_SHIFT for r, g, b in five]
      else:
         try:
            pal = ac.palette(p, {}, gw)[1:]
         except ac.ManifestError as e:
            raise ManifestError(str(e)) from None
      if f"{name}/{o}" not in ts.palettes:
         count = len(five) if p == "auto" else len(p) if isinstance(p, list) else 0

         # always a full 15-color palette (entry 0, transparent, is not stored)
         ts.palettes[f"{name}/{o}"] = (pal + [0] * bitmap.MAX_COLORS)[:bitmap.MAX_COLORS], count
   for n, s in specs.items():
      sw = f"{where} sheet {n!r}"
      group = f"{name}/{owner[n]}"
      colors, count = ts.palettes[group]
      if n in loaded:
         w, h, _ = loaded[n]
         # the palette back as 5-bit (r, g, b) to index the image against
         five = [(c & CHANNEL_MASK, c >> GREEN_SHIFT & CHANNEL_MASK, c >> BLUE_SHIFT & CHANNEL_MASK)
                 for c in colors[:count]]
         idx = bitmap.index_image(loaded[n], five)
         px = [list(idx[y * w:(y + 1) * w]) for y in range(h)]
      else:
         size = s.get("size")
         if not (isinstance(size, list) and len(size) == AXES):
            raise ManifestError(f"{sw}: a shape sheet needs \"size\": [w, h] in pixels")
         size_w, size_h = size
         w, h = integer(size_w, TILE_SIZE, MAX_SHEET_SIZE, sw), integer(size_h, TILE_SIZE, MAX_SHEET_SIZE, sw)
         if w % TILE_SIZE or h % TILE_SIZE:
            raise ManifestError(f"{sw}: size must be multiples of 8")
         if specs[owner[n]].get("palette") == "auto":
            raise ManifestError(f"{sw}: shape sheets use palette indices, so they need an explicit palette")
         canvas = ac.Canvas(w, h)
         for i, sh in enumerate(s.get("shapes", [])):
            try:
               ac.draw(canvas, sh, count, f"{sw} shape {i}")
            except ac.ManifestError as e:
               raise ManifestError(str(e)) from None
         px = canvas.px
      ts.sheets[n] = Sheet(n, group, px)
   for s in specs.values():
      if "indexed" in s:
         raise ManifestError(f"{where} sheet {s['name']!r}: indexed sheets belong to a fixed tileset (\"place\")")
   for i, mt in enumerate(m.get("metatiles", [])):
      mw = f"{where} metatile {i}"
      check_keys(mt, "metatile", mw)
      n = mt.get("name")
      if not isinstance(n, str) or not n or "/" in n:
         raise ManifestError(f"{mw}: needs a name without '/'")
      if n in ts.metatiles:
         raise ManifestError(f"{mw}: duplicate metatile {n!r}")
      refs = mt.get("tiles", [None] * METATILE_TILES)
      if not isinstance(refs, list) or len(refs) != METATILE_TILES:
         raise ManifestError(f"{mw}: tiles needs 4 tile references (top-left, top-right, bottom-left, "
                             "bottom-right)")
      ts.metatiles[n] = {"tiles": [parse_ref(r, ts, f"{mw} tiles") for r in refs]}
   spans: list[tuple[Any, int, int, str]] = []
   for i, an in enumerate(m.get("animations", [])):
      aw = f"{where} animation {i}"
      check_keys(an, "animation", aw)
      count = integer(an.get("count", 1), 1, MAX_ANIMATION_TILES, f"{aw} count")
      frames = an.get("frames")
      if frames not in ts.sheets:
         raise ManifestError(f"{aw}: frames must name a sheet of this tileset")
      fs = ts.sheets[frames]
      n_frames = fs.w * fs.h // count
      if n_frames < MIN_ANIMATION_FRAMES or n_frames > MAX_ANIMATION_FRAMES:
         raise ManifestError(f"{aw}: frame sheet holds {fs.w * fs.h} tiles, so {n_frames} frames; needs 2-255")

      def target_of(ref: Json, tw: str, count: int = count, fs: Sheet = fs, frames: str = frames) -> tuple[str, int]:
         """!A target's sheet and first tile, checked: unflipped, inside its sheet, sharing the frames' palette."""
         target = parse_ref(ref, ts, f"{tw} tiles")
         if target is None or target[-1]:   # the flip bits
            raise ManifestError(f"{tw}: tiles must be an unflipped 'sheet:index'")
         sheet = ts.sheets[target[0]]
         if target[1] + count > sheet.w * sheet.h:
            raise ManifestError(f"{tw}: {count} tiles from {ref} run past the sheet (the frames' tiles must fit)")
         if fs.group != sheet.group:
            raise ManifestError(f"{tw}: frame sheet {frames!r} must share the palette of {target[0]!r} "
                                f"(\"palette\": \"@{sheet.group.split('/', 1)[1]}\")")
         return target[0], target[1]

      # frame k is the frame sheet's tiles k * count ... k * count + count - 1
      period = integer(an.get("period", DEFAULT_ANIMATION_PERIOD), 1, MAX_ANIMATION_PERIOD, f"{aw} period")
      tiles = [[fs.tile(k * count + j, aw) for j in range(count)] for k in range(n_frames)]
      first, phase, more, tiles = animation_targets(an, tiles, period, aw, "tiles", "anim_target", target_of)
      name = an.get("name", f"animation {i}")
      for k, (target, _offset, _phase) in enumerate([(first, 0, phase)] + more):
         spans.append((target[0], target[1], count, f"{name!r} target {k}"))
      ts.target_saved += target_savings(tiles, count, len(more))
      ts.animations.append({"sheet": first[0], "first": first[1], "count": count, "frames": tiles,
                            "period": period, "phase": phase, "flags": animation_flags(an, aw), "name": name,
                            "targets": more})
   check_target_overlap(spans, where)
   return ts


def animation_flags(an: Mapping[str, Any], where: str) -> int:
   """!I3: an animation's flags (capsule::map::kAnim...): "from_sheet" (no frame at entry: the map's own tiles show
   until the first change) and "on_shift" ("keep": runs on; "reload": its tiles are reloaded, the map's own show until
   the next change; "restart": reloaded and started over).
   @param an     the animation manifest
   @param where  location for error messages
   @return the bits
   """
   sheet = an.get("from_sheet", False)
   if not isinstance(sheet, bool):
      raise ManifestError(f"{where}: from_sheet must be true or false")
   shift = an.get("on_shift", "keep")
   if shift not in ON_SHIFT:
      raise ManifestError(f"{where}: on_shift must be one of {', '.join(ON_SHIFT)}")
   return (ANIM_FROM_SHEET if sheet else 0) | ON_SHIFT[shift]


def animation_phase(an: Mapping[str, Any], period: int, where: str) -> int | None:
   """!An animation's "phase": frames into its period at map entry.
   @param an      the animation manifest
   @param period  its period
   @param where   location for error messages
   @return 0 to period - 1, or None (the compiler spreads the map's animations)
   """
   if an.get("phase") is None:
      return None
   return integer(an["phase"], 0, period - 1, f"{where} phase")


def animation_targets(an: Mapping[str, Any], frames: list[Any], period: int, where: str, ref_key: str,
                      target_kind: str, resolve: Any) -> tuple[Any, int | None, list[tuple[Any, int, int | None]],
                                                               list[Any]]:
   """!DS1 (D-30 section 5): an animation's tile targets. Without "targets" its "tiles" / "at" (and "phase") is its one
   target; with "targets" ([{"tiles" | "at", "offset", "phase"}, ...]) the first target becomes the record's own (the
   frames rotated so its offset is 0) and the others follow as more targets, their offsets relative to it.
   @param an           the animation manifest
   @param frames       its frames (each a list of tiles)
   @param period       its period
   @param where        location for error messages
   @param ref_key      "tiles" (a tileset) or "at" (a fixed tileset)
   @param target_kind  KEYS entry of a target ("anim_target" / "fixed_anim_target")
   @param resolve      callable(value, where) -> the target's first tile (whatever the tileset form uses)
   @return (first target, its phase or None, more targets [(first, offset 0 to frames - 1, phase or None)],
           the frames, rotated)
   """
   targets = an.get("targets")
   if targets is None:
      return resolve(an.get(ref_key), where), animation_phase(an, period, where), [], frames
   if ref_key in an:
      raise ManifestError(f"{where}: an animation with targets names its tiles in them (no {ref_key!r})")
   if not isinstance(targets, list) or not targets or len(targets) > MAX_ANIM_TARGETS + 1:
      raise ManifestError(f"{where}: targets lists 1-{MAX_ANIM_TARGETS + 1} targets")
   parsed = []
   for k, tg in enumerate(targets):
      tw = f"{where} target {k}"
      check_keys(tg, target_kind, tw)
      first = resolve(tg.get(ref_key), tw)
      offset = integer(tg.get("offset", 0), 0, len(frames) - 1, f"{tw} offset (a frame index)")
      phase = animation_phase(tg, period, tw) if "phase" in tg else animation_phase(an, period, where)
      parsed.append((first, offset, phase))
   base, rot, base_phase = parsed[0]
   # target k shows frame (t + offset_k) of the stored frames: storing them from the first target's offset on
   # makes its offset 0 and the others' (offset_k - offset_0) mod frames
   more = [(first, (offset - rot) % len(frames), phase) for first, offset, phase in parsed[1:]]
   return base, base_phase, more, frames[rot:] + frames[:rot]


def check_target_overlap(spans: list[tuple[Any, int, int, str]], where: str) -> None:
   """!DS1: no two animation targets of a tileset replace the same tile.
   @param spans  (space, first, count, name): a sheet name (or the fixed place) and the tiles replaced
   @param where  location for error messages
   @return None
   """
   seen = sorted(spans, key=lambda s: (str(s[0]), s[1]))
   for (space_a, first_a, count_a, name_a), (space_b, first_b, _count_b, name_b) in zip(seen, seen[1:]):
      if space_a == space_b and first_b < first_a + count_a:
         raise ManifestError(f"{where}: animation targets {name_a} and {name_b} both replace tile {first_b}")


def target_savings(frames: Sequence[Any], count: int, more: int) -> int:
   """!DS1: ROM the more targets of an animation save against one animation (with its own frames) per target.
   @param frames  the frames
   @param count   tiles per frame
   @param more    more targets
   @return bytes
   """
   return more * (len(frames) * count * TILE_BYTES + TILE_ANIMATION_BYTES - ANIM_TARGET_BYTES)


def compile_fixed_tileset(ts: Tileset, m: Mapping[str, Any], base: Path, where: str, deps: list[Path]) -> Tileset:
   """!The fixed-placement form of a tileset (I2, see the module notes: "place").
   @param ts     the tileset being compiled (name set)
   @param m      its manifest (keys checked)
   @param base   its folder
   @param where  location for error messages
   @param deps   list image paths are appended to
   @return ts, with place, bank_colors, sheets (group "NAME/fixed"), metatiles (raw entries) and animations
   """
   place = m["place"]
   pw = f"{where} place"
   check_keys(place, "place", pw)
   tile = integer(place.get("tile"), 0, MAX_TILES - 1, f"{pw} tile")
   tiles = integer(place.get("tiles"), 1, MAX_TILES - tile, f"{pw} tiles")
   bank = integer(place.get("palette"), 0, MAX_PALETTES - 1, f"{pw} palette")
   banks = integer(place.get("palettes"), 0, MAX_PALETTES - bank, f"{pw} palettes")
   group = f"{ts.name}/fixed"
   for i, sh in enumerate(m.get("sheets", [])):
      sw = f"{where} sheet {i}"
      check_keys(sh, "fixed_sheet", sw)
      n = sh.get("name")
      if not isinstance(n, str) or not n or ":" in n or "/" in n or n in ts.sheets:
         raise ManifestError(f"{sw}: needs a unique name without ':' or '/'")
      image = sh.get("image")
      if sh.get("indexed") is not True or not isinstance(image, str) or manifest_paths.is_absolute_text(image):
         raise ManifestError(f"{sw}: a fixed tileset's sheets are indexed images (\"indexed\": true, \"image\": "
                             "a path relative to the tileset)")
      full = (base / sh["image"]).resolve()
      deps.append(full)
      try:
         img_w, img_h, idx = bitmap.read_png_indices(full)
      except bitmap.BitmapError as e:
         raise ManifestError(f"{sw}: {e}") from None
      if img_w % TILE_SIZE or img_h % TILE_SIZE or not img_w or not img_h:
         raise ManifestError(f"{sw}: {sh['image']} is {img_w}x{img_h}; sizes are multiples of 8")
      if idx and max(idx) > MAX_PIXEL_4BPP:
         raise ManifestError(f"{sw}: {sh['image']} uses palette index {max(idx)}; 4bpp tiles use 0-{MAX_PIXEL_4BPP}")
      ts.sheets[n] = Sheet(n, group, [list(idx[y * img_w:(y + 1) * img_w]) for y in range(img_h)])
   sheet = place.get("sheet")
   if sheet not in ts.sheets:
      raise ManifestError(f"{pw}: sheet must name one of the tileset's sheets")
   if ts.sheets[sheet].w * ts.sheets[sheet].h > tiles:
      raise ManifestError(f"{pw}: sheet {sheet!r} holds {ts.sheets[sheet].w * ts.sheets[sheet].h} tiles; the place "
                          f"owns {tiles}")
   ts.place = {"sheet": sheet, "tile": tile, "tiles": tiles, "palette": bank, "palettes": banks}
   colors = m.get("palettes", [])
   if not isinstance(colors, list) or len(colors) > banks:
      raise ManifestError(f"{where}: palettes lists up to {banks} banks (the place's)")
   for i, row in enumerate(colors):
      if not isinstance(row, list) or len(row) != BANK_COLORS:
         raise ManifestError(f"{where} palettes {i}: needs {BANK_COLORS} colors (entry 0 included)")
      ts.bank_colors.append([ac.bgr555(c, f"{where} palettes {i}") for c in row])
   for i, mt in enumerate(m.get("metatiles", [])):
      mw = f"{where} metatile {i}"
      check_keys(mt, "fixed_metatile", mw)
      n = mt.get("name")
      if not isinstance(n, str) or not n or "/" in n or n in ts.metatiles:
         raise ManifestError(f"{mw}: needs a unique name without '/'")
      entries = mt.get("entries")
      if not isinstance(entries, list) or len(entries) != METATILE_TILES:
         raise ManifestError(f"{mw}: entries needs 4 tilemap entries (top-left, top-right, bottom-left, "
                             "bottom-right)")
      ts.metatiles[n] = {"entries": [integer(v, 0, MAX_ENTRY, f"{mw} entries") for v in entries]}
   spans: list[tuple[Any, int, int, str]] = []
   for i, an in enumerate(m.get("animations", [])):
      aw = f"{where} animation {i}"
      check_keys(an, "fixed_animation", aw)
      count = integer(an.get("count", 1), 1, MAX_ANIMATION_TILES, f"{aw} count")
      frames = an.get("frames")
      if frames not in ts.sheets:
         raise ManifestError(f"{aw}: frames must name a sheet of this tileset")
      fs = ts.sheets[frames]
      n_frames = fs.w * fs.h // count
      if n_frames < MIN_ANIMATION_FRAMES or n_frames > MAX_ANIMATION_FRAMES or fs.w * fs.h % count:
         raise ManifestError(f"{aw}: frame sheet holds {fs.w * fs.h} tiles, so {fs.w * fs.h / count:g} frames of "
                             f"{count}; needs a whole 2-255")
      period = integer(an.get("period", DEFAULT_ANIMATION_PERIOD), 1, MAX_ANIMATION_PERIOD, f"{aw} period")

      def at_of(value: Json, tw: str, count: int = count) -> int:
         """!A target's first tile, counted from the place's: the frames' tiles must fit the place."""
         return integer(value, 0, tiles - count, f"{tw} at (the place owns {tiles} tiles)")

      frame_tiles = [[fs.tile(k * count + j, aw) for j in range(count)] for k in range(n_frames)]
      at, phase, more, frame_tiles = animation_targets(an, frame_tiles, period, aw, "at", "fixed_anim_target", at_of)
      name = an.get("name", f"animation {i}")
      for k, (first, _offset, _phase) in enumerate([(at, 0, phase)] + more):
         spans.append(("place", first, count, f"{name!r} target {k}"))
      ts.target_saved += target_savings(frame_tiles, count, len(more))
      ts.animations.append({"at": at, "count": count, "frames": frame_tiles, "period": period, "phase": phase,
                            "flags": animation_flags(an, aw), "name": name, "targets": more})
   check_target_overlap(spans, where)
   return ts


# -- maps ---------------------------------------------------------------------------------

def compile_map(name: str, path: str | Path, tilesets: Mapping[str, Tileset], behaviors: Sequence[str] | None = None,
                where_prefix: str = "map", sources: Sources | None = None,
                tags: Sequence[str] = (), darkest: int = MAX_DARKNESS) -> dict[str, Any]:
   """!Compiles one map against the world's tilesets.
   @param name          map name
   @param path          manifest file
   @param tilesets      name -> Tileset
   @param behaviors     the world's behavior names, in order (default: just "normal")
   @param where_prefix  label for error messages
   @param sources       in-memory manifests (see load_json()), or None
   @param tags          the world's map tags (F2: the map's "tags" name them)
   @param darkest       the world's darkest level (C8: len(darkness_levels()); the map's "darkness" goes up to it)
   @return dict of everything emit() needs
   """
   where = f"{where_prefix} {name!r}"
   behaviors = behaviors or ["normal"]
   m = load_json(path, where, sources)
   check_keys(m, "map", where)
   if m.get("schema") != MAP_SCHEMA:
      raise ManifestError(f"{where}: schema must be '{MAP_SCHEMA}'")
   uses = m.get("tilesets")
   if uses is not None:
      if not isinstance(uses, list) or not all(isinstance(t, str) for t in uses) or len(set(uses)) != len(uses):
         raise ManifestError(f"{where}: tilesets lists tileset names, each once")
      for t in uses:
         if t not in tilesets:
            raise ManifestError(f"{where}: tilesets: unknown tileset {t!r}")

   # Tile layers (C-01b).
   raw_layers = m.get("layers", [])
   if not isinstance(raw_layers, list) or len(raw_layers) > MAX_LAYERS:
      raise ManifestError(f"{where}: layers lists up to {MAX_LAYERS} tile layers")
   layers = []
   for i, ly in enumerate(raw_layers):
      lw = f"{where} layer {i}"
      check_keys(ly, "layer", lw)
      bg = integer(ly.get("bg"), 0, MAX_BG, f"{lw} bg")
      if any(bg == other[0] for other in layers):
         raise ManifestError(f"{lw}: BG{bg} already holds another layer")
      if ly.get("content", "tilemap") not in LAYER_CONTENT:
         raise ManifestError(f"{lw}: content must be one of {', '.join(LAYER_CONTENT)} (other kinds come later)")
      if ly.get("purpose", "terrain") not in LAYER_PURPOSE:
         raise ManifestError(f"{lw}: purpose must be one of {', '.join(LAYER_PURPOSE)}")
      layers.append((bg, integer(ly.get("priority", bg), 0, MAX_PRIORITY, f"{lw} priority")))

   names = m.get("metatiles", [])
   if names == "all":   # I2: every metatile of the map's tilesets, tileset by tileset
      if uses is None:
         raise ManifestError(f"{where}: metatiles \"all\" needs the map's tilesets")
      names = [f"{t}/{n}" for t in uses for n in tilesets[t].metatiles]
   if not isinstance(names, list) or len(names) > MAX_METATILES:
      raise ManifestError(f"{where}: metatiles needs up to {MAX_METATILES} \"tileset/metatile\" names")
   metas = []
   for i, n in enumerate(names):
      if not isinstance(n, str) or n.count("/") != 1:
         raise ManifestError(f"{where} metatiles {i}: expected \"tileset/metatile\", got {n!r}")
      t, mt = n.split("/")
      if t not in tilesets:
         raise ManifestError(f"{where} metatiles {i}: unknown tileset {t!r}")
      if uses is not None and t not in uses:
         raise ManifestError(f"{where} metatiles {i}: {n!r} is from tileset {t!r}, which the map's tilesets "
                             f"({', '.join(uses) or 'none'}) leave out")
      if mt not in tilesets[t].metatiles:
         raise ManifestError(f"{where} metatiles {i}: tileset {t!r} has no metatile {mt!r}")
      metas.append((tilesets[t], tilesets[t].metatiles[mt]))
   bnames = m.get("behaviors", [behaviors[0]])
   if not isinstance(bnames, list) or not bnames or len(bnames) > MAX_BEHAVIORS:
      raise ManifestError(f"{where}: behaviors lists 1-256 world behavior names")
   bmap = []
   for i, b in enumerate(bnames):
      if b not in behaviors:
         raise ManifestError(f"{where} behaviors {i}: unknown behavior {b!r} (the world's: {', '.join(behaviors)})")
      bmap.append(behaviors.index(b))

   fixed = [t for t in (uses or []) if tilesets[t].place is not None] + \
      [ts.name for ts, _ in metas if ts.place is not None]
   if fixed:   # I2: fixed tilesets load at their places (fixed_vram()); no renumbering, no deduplication
      tiles, pals, metatiles, anims = fixed_vram(where, uses, tilesets, metas)
      banks = {}
   else:
      tiles, pals, metatiles, anims, banks = None, None, None, None, {}

   # Palette banks in order of first use; tiles deduplicated with flips.

   def bank_of(ts: Tileset, sheet: str) -> int:
      """!The palette bank of a sheet's palette group, given the next bank on first use."""
      group = ts.sheets[sheet].group
      if group not in banks:
         banks[group] = len(banks)
         if len(banks) > MAX_PALETTES:
            raise ManifestError(f"{where}: uses {len(banks)} palettes; a map has {MAX_PALETTES} "
                                "(fixed palettes for now; per-chunk palette streaming comes later)")
      return banks[group]

   lookup = {}
   animated = {}   # (tileset, sheet, index) -> vram index, for animation targets
   if tiles is None:
      tiles = [tuple(tuple(0 for _ in range(TILE_SIZE)) for _ in range(TILE_SIZE))]   # tile 0: transparent
      for flip, t in ac.flips(tiles[0]):
         lookup.setdefault(t, (0, flip))
      anims = []
      used_ts = []
      for ts, _ in metas:
         if ts not in used_ts:
            used_ts.append(ts)
      for ts in used_ts:
         for an in ts.animations:
            firsts = []
            # DS1: each target's tiles get map tiles of their own (the frames stay one array)
            for sheet_name, sheet_first in [(an["sheet"], an["first"])] + [tg[0] for tg in an.get("targets", [])]:
               sheet = ts.sheets[sheet_name]
               firsts.append(len(tiles))
               for j in range(an["count"]):
                  animated[(ts.name, sheet_name, sheet_first + j)] = len(tiles)
                  tiles.append(sheet.tile(sheet_first + j, where))
               bank_of(ts, sheet_name)
            more = [(f, off, ph) for f, (_ref, off, ph) in zip(firsts[1:], an.get("targets", []))]
            anims.append({"first": firsts[0], "count": an["count"], "frames": an["frames"], "period": an["period"],
                          "phase": an["phase"], "flags": an.get("flags", 0), "name": f"{ts.name}/{an['name']}",
                          "targets": more})
   if len(anims) > MAX_ANIMATIONS:
      raise ManifestError(f"{where}: {len(anims)} tile animations; at most {MAX_ANIMATIONS}")
   if sum(len(an.get("targets", [])) for an in anims) > MAX_ANIM_TARGETS:
      raise ManifestError(f"{where}: its animations have {sum(len(an.get('targets', [])) for an in anims)} more "
                          f"targets; at most {MAX_ANIM_TARGETS}")

   def entry(ts: Tileset, ref: TileRef | None) -> int:
      """!A tilemap entry: VRAM tile | flip bits | palette bank << 12 (transparent: tile 0)."""
      if ref is None:
         return 0
      sheet, index, flip = ref
      bank = bank_of(ts, sheet)
      key = (ts.name, sheet, index)
      if key in animated:
         return animated[key] | flip | bank << PALETTE_SHIFT
      pixels = ts.sheets[sheet].tile(index, where)
      if pixels not in lookup:
         lookup_index = len(tiles)
         tiles.append(pixels)
         for f, t in ac.flips(pixels):
            lookup.setdefault(t, (lookup_index, f))

      # the stored tile may be a flipped copy: combining both flips gives the one to draw
      vi, f = lookup[pixels]
      return vi | (f ^ flip) | bank << PALETTE_SHIFT

   if metatiles is None:
      metatiles = [[entry(ts, r) for r in mt["tiles"]] for ts, mt in metas]
   if len(tiles) > MAX_TILES:
      raise ManifestError(f"{where}: {len(tiles)} unique tiles; the map's VRAM holds {MAX_TILES}")

   def attr(v: Json, cw: str) -> int:
      """!An attribute cell with the map's behavior index replaced by the world's."""
      if v is None:
         return CELL_UNDEFINED
      integer(v, 0, CELL_BITS, cw)
      b = v & BEHAVIOR_MASK
      if b >= len(bmap):
         raise ManifestError(f"{cw}: behavior index {b} is past the map's behaviors ({len(bmap)})")
      return (v & ~BEHAVIOR_MASK) | bmap[b]

   def tile_rows(rows: Json, lw: str) -> list[int]:
      """!One layer's grid of a chunk, row by row (no metatile everywhere when null)."""
      if rows is None:
         return [NO_METATILE] * CHUNK_CELLS
      if not (isinstance(rows, list) and len(rows) == CHUNK and
              all(isinstance(r, list) and len(r) == CHUNK for r in rows)):
         raise ManifestError(f"{lw}: needs {CHUNK} rows of {CHUNK} dictionary indices or null")
      out = []
      for y, row in enumerate(rows):
         for x, v in enumerate(row):
            out.append(NO_METATILE if v is None else integer(v, 0, len(names) - 1, f"{lw} cell {x},{y}")
                       if names else integer(v, 0, -1, f"{lw} cell {x},{y} (the map has no metatiles)"))
      return out

   chunks, seen = [], {}
   raw = m.get("chunks", [])
   if not isinstance(raw, list):
      raise ManifestError(f"{where}: chunks must be a list")
   for i, c in enumerate(raw):
      cw = f"{where} chunk {i}"
      check_keys(c, "chunk", cw)
      at = c.get("at")
      if not (isinstance(at, list) and len(at) == AXES):
         raise ManifestError(f"{cw}: at must be [chunk x, chunk y]")
      at_x, at_y = at
      cx = integer(at_x, MIN_CHUNK_COORD, MAX_CHUNK_COORD, cw)
      cy = integer(at_y, MIN_CHUNK_COORD, MAX_CHUNK_COORD, cw)
      if (cx, cy) in seen:
         raise ManifestError(f"{cw}: chunk {cx},{cy} appears twice (also chunk {seen[(cx, cy)]})")
      seen[(cx, cy)] = i
      rows = c.get("cells")
      if not (isinstance(rows, list) and len(rows) == CHUNK and all(isinstance(r, list) and len(r) == CHUNK
                                                                   for r in rows)):
         raise ManifestError(f"{cw}: cells must be {CHUNK} rows of {CHUNK} attribute cells or null")
      cells = [attr(v, f"{cw} cell {x},{y}") for y, r in enumerate(rows) for x, v in enumerate(r)]
      cl = c.get("layers", [])
      if not isinstance(cl, list) or len(cl) > len(layers):
         raise ManifestError(f"{cw}: layers holds one grid per tile layer ({len(layers)})")
      grids = [tile_rows(cl[k] if k < len(cl) else None, f"{cw} layer {k}") for k in range(len(layers))]
      chunks.append((cx, cy, cells, grids))
   if not chunks:   # a blank map: one unpainted chunk
      chunks.append((0, 0, [CELL_UNDEFINED] * CHUNK_CELLS, [[NO_METATILE] * CHUNK_CELLS] * len(layers)))
   chunks.sort(key=lambda c: (c[1], c[0]))

   # the grid: one entry per chunk position of the bounding box, row-major (NO_CHUNK where there is none)
   x0, y0 = min(c[0] for c in chunks), min(c[1] for c in chunks)
   gw, gh = max(c[0] for c in chunks) - x0 + 1, max(c[1] for c in chunks) - y0 + 1
   if gw * gh > MAX_CHUNK_POSITIONS:
      raise ManifestError(f"{where}: chunks span {gw}x{gh}; at most {MAX_CHUNK_POSITIONS} chunk positions (64x64)")
   grid = [NO_CHUNK] * (gw * gh)
   for i, c in enumerate(chunks):
      grid[(c[1] - y0) * gw + (c[0] - x0)] = i

   b = m.get("border", {})
   bw = f"{where} border"
   check_keys(b, "border", bw)
   size = b.get("size", [1, 1])
   bwid, bhei = integer(size[0], 1, MAX_BORDER_SIZE, bw), integer(size[1], 1, MAX_BORDER_SIZE, bw)
   bl = b.get("layers", [])
   if not isinstance(bl, list) or len(bl) > len(layers):
      raise ManifestError(f"{bw}: layers holds one pattern per tile layer ({len(layers)})")
   border = []
   for k in range(len(layers)):
      cells = bl[k] if k < len(bl) else [None] * (bwid * bhei)
      if not isinstance(cells, list) or len(cells) != bwid * bhei:
         raise ManifestError(f"{bw} layer {k}: needs {bwid * bhei} dictionary indices or null")
      border.append([NO_METATILE if v is None else integer(v, 0, len(names) - 1, f"{bw} layer {k}")
                     for v in cells])
   phase = b.get("phase", [0, 0])
   px_, py_ = integer(phase[0], 0, bwid - 1, bw), integer(phase[1], 0, bhei - 1, bw)
   backdrop = ac.bgr555(m.get("backdrop", "#000000"), f"{where} backdrop")
   can_run = m.get("can_run", True)
   if not isinstance(can_run, bool):
      raise ManifestError(f"{where}: can_run must be true or false")
   darkness, tag_mask, escape = map_field_rules(m, tags, where, darkest)
   view = object_view(m, where)
   sets = compile_override_sets(m, names, len(layers), {(c[0], c[1]) for c in chunks}, where)
   if pals is None:
      pals = [None] * len(banks)
      for group, n in banks.items():
         t = group.split("/", 1)[0]
         pals[n] = tilesets[t].palettes[group][0]
   return {"name": name, "key": asset_key(f"map:{name}", where), "chunks": chunks, "grid": grid,
           "grid_x0": x0, "grid_y0": y0, "grid_w": gw, "grid_h": gh, "metatiles": metatiles,
           "tiles": tiles, "palettes": pals, "border": border, "layers": layers,
           "border_w": bwid, "border_h": bhei, "phase": (px_, py_), "anims": anims, "backdrop": backdrop,
           "dictionary": names, "behavior_dictionary": bnames,
           "flags": (0 if can_run else MAP_NO_RUN) | (MAP_ESCAPE_POINT if escape else 0),
           "darkness": darkness, "tags": tag_mask, "object_view": view, "override_sets": sets}


def compile_override_sets(m: Mapping[str, Any], dictionary: Sequence[str], layer_count: int,
                          chunk_positions: set[tuple[int, int]], where: str) -> list[dict[str, Any]]:
   """!DS1 (D-30 section 2): a map's "override_sets": [{"name", "persist": {"flag": NAME} | {"var": NAME,
   "value": N} (absent: until the map is left), "cells": [{"cell": [x, y], "layer": n, "metatile": "tileset/name" |
   null, or "entries": [4 map entries]}]}], checked: cells inside the map's chunks, layers the map has, metatiles of
   its dictionary. Flag and variable names are resolved by compile_world() (the scripts' names).
   @param m                the map manifest
   @param dictionary       its metatile dictionary ("tileset/name" entries)
   @param layer_count      its tile layers
   @param chunk_positions  its chunks' positions
   @param where            location for messages
   @return [{"name", "persist": ("map" | "flag" | "var", name, value), "cells": [(x, y, layer, entries or None,
           metatile id)]}]
   """
   raw = m.get("override_sets", [])
   if not isinstance(raw, list) or len(raw) > MAX_MAP_OVERRIDE_SETS:
      raise ManifestError(f"{where}: override_sets lists up to {MAX_MAP_OVERRIDE_SETS} sets")
   out: list[dict[str, Any]] = []
   for i, s in enumerate(raw):
      sw = f"{where} override set {i}"
      check_keys(s, "override_set", sw)
      name = s.get("name")
      if not isinstance(name, str) or not name.isidentifier() or any(o["name"] == name for o in out):
         raise ManifestError(f"{sw}: needs a unique name (letters, digits, '_')")
      persist = ("map", None, 0)
      if "persist" in s:
         ps = s["persist"]
         check_keys(ps, "persist", f"{sw} persist")
         if ("flag" in ps) == ("var" in ps) or ("value" in ps) != ("var" in ps):
            raise ManifestError(f"{sw} persist: needs a flag, or a var and a value")
         persist = ("flag", ps["flag"], 0) if "flag" in ps else \
            ("var", ps["var"], integer(ps["value"], MIN_OVERRIDE_VALUE, MAX_OVERRIDE_VALUE, f"{sw} persist value"))
         if persist[0] == "var" and not persist[-1]:
            raise ManifestError(f"{sw} persist: the value must not be 0 (clearing the set writes 0)")
      cells = s.get("cells")
      if not isinstance(cells, list) or not cells:
         raise ManifestError(f"{sw}: cells lists the cells it changes")
      seen: set[tuple[int, int, int]] = set()
      parsed = []
      for k, c in enumerate(cells):
         cw = f"{sw} cell {k}"
         check_keys(c, "override_cell", cw)
         at = c.get("cell")
         if not isinstance(at, list) or len(at) != AXES:
            raise ManifestError(f"{cw}: cell must be [x, y]")
         x, y = integer(at[0], MIN_COORD, MAX_COORD, cw), integer(at[1], MIN_COORD, MAX_COORD, cw)
         if (x >> CHUNK_SHIFT, y >> CHUNK_SHIFT) not in chunk_positions:
            raise ManifestError(f"{cw}: {x},{y} is outside the map's chunks")
         layer = integer(c.get("layer", 0), 0, layer_count - 1, f"{cw} layer (the map has {layer_count} layers)") \
            if layer_count else integer(c.get("layer"), 0, -1, f"{cw} layer (the map has no layers)")
         if (x, y, layer) in seen:
            raise ManifestError(f"{cw}: the set changes {x},{y} layer {layer} twice")
         seen.add((x, y, layer))
         if ("metatile" in c) == ("entries" in c):
            raise ManifestError(f"{cw}: needs a metatile (or null) or four entries")
         if "entries" in c:
            e = c["entries"]
            if not isinstance(e, list) or len(e) != METATILE_TILES:
               raise ManifestError(f"{cw}: entries are {METATILE_TILES} map entries")
            parsed.append((x, y, layer, tuple(integer(v, 0, MAX_ENTRY, f"{cw} entries") for v in e), 0))
            continue
         mt = c["metatile"]
         if mt is not None and mt not in dictionary:
            raise ManifestError(f"{cw}: metatile {mt!r} is not in the map's metatiles")
         parsed.append((x, y, layer, None, NO_METATILE if mt is None else list(dictionary).index(mt)))
      out.append({"name": name, "persist": persist, "cells": parsed, "where": sw})
   return out


def compile_draw_order(w: Mapping[str, Any]) -> dict[str, Any] | None:
   """!DS1 (D-30 section 4): the world's "draw_order": the rule turning a sprite's bottom edge on screen and its
   elevation into a sub-priority (capsule::map::DrawOrder): {"elevation_base": 16 values, "bias", "row_pixels" (a
   power of two), "rows", "row_step", "add", "wrap" (a power of two up to 256: the edge's pixels kept)}; missing
   keys take DEFAULT_DRAW_ORDER's.
   @param w  the world manifest
   @return the rule ({"base", "bias", "mask", "row_shift", "rows", "row_step", "add"}), or None (the default)
   """
   if "draw_order" not in w:
      return None
   d = w["draw_order"]
   where = "world draw_order"
   check_keys(d, "draw_order", where)
   r = dict(DEFAULT_DRAW_ORDER, **d)
   base = r["elevation_base"]
   if not isinstance(base, list) or len(base) != ELEVATIONS:
      raise ManifestError(f"{where}: elevation_base needs {ELEVATIONS} values (one per elevation)")
   base = [integer(v, 0, MAX_U8, f"{where} elevation_base") for v in base]
   rows_px = integer(r["row_pixels"], 1, MAX_DRAW_WRAP, f"{where} row_pixels")
   wrap = integer(r["wrap"], 1, MAX_DRAW_WRAP, f"{where} wrap")
   if rows_px & (rows_px - 1) or wrap & (wrap - 1):
      raise ManifestError(f"{where}: row_pixels and wrap are powers of two")
   return {"base": base, "bias": integer(r["bias"], MIN_I8, MAX_I8, f"{where} bias"), "mask": wrap - 1,
           "row_shift": rows_px.bit_length() - 1, "rows": integer(r["rows"], 0, MAX_U8, f"{where} rows"),
           "row_step": integer(r["row_step"], 0, MAX_U8, f"{where} row_step"),
           "add": integer(r["add"], 0, MAX_U8, f"{where} add")}


def compile_door_bindings(w: Mapping[str, Any], maps: Sequence[dict[str, Any]],
                          events: Mapping[str, Any]) -> dict[str, list[tuple[int, int, int, int, str]]]:
   """!DS1 (D-30 section 1): each map's door bindings: its warps' "door" (a cell binding at the warp's cell) first,
   then the world's "doors": [{"tileset": T (or "*": any of the map's tilesets), "metatile": NAME, "door":
   "door:KEY"}], one binding per entry of the map's dictionary showing that metatile.
   @param w       the world manifest
   @param maps    compile_map() results
   @param events  compile_world_events()["events"]
   @return {map name: [(key, metatile id or NO_METATILE, x or DOOR_ANY_CELL, y, key text)]}
   """
   raw = w.get("doors", [])
   if not isinstance(raw, list):
      raise ManifestError("world: doors lists door bindings")
   rules = []
   for i, b in enumerate(raw):
      bw = f"world door binding {i}"
      check_keys(b, "door_binding", bw)
      key = b.get("door")
      if not isinstance(key, str) or not key.startswith("door:"):
         raise ManifestError(f"{bw}: door names a door asset (\"door:NAME\")")
      if not isinstance(b.get("tileset"), str) or not isinstance(b.get("metatile"), str):
         raise ManifestError(f"{bw}: needs a tileset (or \"*\") and a metatile name")
      rules.append((b["tileset"], b["metatile"], asset_key(key, bw), key))
   out = {}
   for mp in maps:
      found = [(k, NO_METATILE, x, y, text) for x, y, k, text in events.get(mp["name"], {}).get("doors", [])]
      # C10 (S1): the dictionary's entries by metatile name, split once per map (not once per rule)
      by_name: dict[str, list[tuple[int, str]]] = {}
      for idx, entry in enumerate(mp["dictionary"]):
         t_name, m_name = entry.split("/", 1)
         by_name.setdefault(m_name, []).append((idx, t_name))
      for ts, mt, k, text in rules:
         for idx, t_name in by_name.get(mt, []):
            if ts == ANY_TILESET or ts == t_name:
               found.append((k, idx, DOOR_ANY_CELL, 0, text))
      if len(found) > MAX_DOOR_BINDINGS:
         raise ManifestError(f"map {mp['name']!r}: {len(found)} door bindings; at most {MAX_DOOR_BINDINGS}")
      out[mp["name"]] = found
   return out


def fixed_vram(where: str, uses: Sequence[str] | None, tilesets: Mapping[str, Tileset],
               metas: Sequence[tuple[Tileset, Mapping[str, Any]]]) -> tuple[list[Tile], list[list[int]],
                                                                          list[list[int]], list[dict[str, Any]]]:
   """!What a map of fixed tilesets loads (I2): each of its tilesets at its place.
   @param where     location for error messages
   @param uses      the map's tilesets (all of them fixed, none overlapping)
   @param tilesets  the world's tilesets
   @param metas     the map's dictionary as (tileset, metatile) pairs
   @return (tiles: VRAM tile 0 up to the last placed tile, unplaced tiles blank; palettes: banks 0 up to the last
           placed bank, 15 colors each; metatiles: raw entries; animations)
   """
   if uses is None or any(tilesets[t].place is None for t in uses):
      raise ManifestError(f"{where}: a map drawing from fixed tilesets lists its tilesets, all of them fixed")
   for ts, _ in metas:
      if ts.name not in uses:
         raise ManifestError(f"{where}: dictionary tileset {ts.name!r} is not one of the map's tilesets")
   blank = tuple(tuple(0 for _ in range(TILE_SIZE)) for _ in range(TILE_SIZE))
   places = [tilesets[t].place for t in uses]
   tile_end = max(pl["tile"] + pl["tiles"] for pl in places)
   bank_end = max(pl["palette"] + pl["palettes"] for pl in places)
   tiles: list[Tile] = [blank] * tile_end
   pals: list[list[int]] = [[0] * bitmap.MAX_COLORS for _ in range(bank_end)]
   owner_tile: list[str | None] = [None] * tile_end
   owner_bank: list[str | None] = [None] * bank_end
   anims = []
   for t in uses:
      ts = tilesets[t]
      pl = ts.place
      sheet = ts.sheets[pl["sheet"]]
      for k in range(pl["tile"], pl["tile"] + pl["tiles"]):
         if owner_tile[k] is not None:
            raise ManifestError(f"{where}: tilesets {owner_tile[k]!r} and {t!r} both place VRAM tile {k}")
         owner_tile[k] = t
      for k in range(pl["palette"], pl["palette"] + pl["palettes"]):
         if owner_bank[k] is not None:
            raise ManifestError(f"{where}: tilesets {owner_bank[k]!r} and {t!r} both place palette bank {k}")
         owner_bank[k] = t
      for k in range(sheet.w * sheet.h):
         tiles[pl["tile"] + k] = sheet.tile(k, where)
      for k, colors in enumerate(ts.bank_colors):
         pals[pl["palette"] + k] = colors[1:]   # entry 0 is transparent (bank 0's is the map's backdrop)
      for an in ts.animations:
         anims.append({"first": pl["tile"] + an["at"], "count": an["count"], "frames": an["frames"],
                       "period": an["period"], "phase": an["phase"], "flags": an.get("flags", 0),
                       "name": f"{t}/{an['name']}",
                       "targets": [(pl["tile"] + at, off, ph) for at, off, ph in an.get("targets", [])]})
   # entries may name tiles no tileset of this map places: they show what VRAM holds there (blank)
   metatiles = [list(mt["entries"]) for _, mt in metas]
   return tiles, pals, metatiles, anims


def object_view(m: Mapping[str, Any], where: str) -> tuple[int, int, int, int]:
   """!I3: a map's "object_view": [left, right, up, down], the cells around the walker its objects spawn in (more than
   15 objects need one); absent: every object spawns at entry.
   @param m      the map manifest
   @param where  location for messages
   @return the four counts (all 0 without one)
   """
   v = m.get("object_view")
   if v is None:
      return (0, 0, 0, 0)
   if not isinstance(v, list) or len(v) != VIEW_SIDES:
      raise ManifestError(f"{where}: object_view must be [left, right, up, down]")
   counts = tuple(integer(n, 0, MAX_VIEW_CELLS, f"{where} object_view") for n in v)
   if not any(counts):
      raise ManifestError(f"{where}: object_view needs a cell on some side (leave it out to spawn every object)")
   left, right, up, down = counts
   return left, right, up, down


def map_field_rules(m: Mapping[str, Any], tags: Sequence[str], where: str,
                    darkest: int = MAX_DARKNESS) -> tuple[int, int, bool]:
   """!A map's F2 properties (darkness, tags, escape_point), checked; studios call it in-process.
   @param m        the map manifest
   @param tags     the world's map tags
   @param where    location for messages
   @param darkest  the world's darkest level (C8: len(darkness_levels(world)))
   @return (darkness level, tag mask, escape point)
   """
   darkness = integer(m.get("darkness", 0), 0, darkest, f"{where} darkness")
   raw = m.get("tags", [])
   if not isinstance(raw, list) or any(t not in tags for t in raw) or len(set(raw)) != len(raw):
      raise ManifestError(f"{where}: tags lists the world's map_tags ({', '.join(tags) or 'none declared'}), "
                          f"each once")
   escape = m.get("escape_point", False)
   if not isinstance(escape, bool):
      raise ManifestError(f"{where}: escape_point must be true or false")
   return darkness, sum(1 << list(tags).index(t) for t in raw), escape


def _chunk_of(mp: Mapping[str, Any], x: int, y: int) -> tuple[Any, ...] | None:
   """!The compiled chunk holding a position.
   @param mp  compile_map() result
   @param x   metatile column
   @param y   metatile row
   @return the chunk tuple, or None
   """
   cx, cy = (x >> CHUNK_SHIFT) - mp["grid_x0"], (y >> CHUNK_SHIFT) - mp["grid_y0"]
   if 0 <= cx < mp["grid_w"] and 0 <= cy < mp["grid_h"]:
      i = mp["grid"][cy * mp["grid_w"] + cx]
      if i != NO_CHUNK:
         return mp["chunks"][i]
   return None


def cell_at(mp: Mapping[str, Any], x: int, y: int) -> int:
   """!A compiled map's attribute cell, as the runtime finds it (border-aware).
   @param mp  compile_map() result
   @param x   metatile column
   @param y   metatile row
   @return the cell (world behavior index), CELL_BORDER outside
   """
   c = _chunk_of(mp, x, y)
   i = (y & CHUNK_MASK) * CHUNK + (x & CHUNK_MASK)   # the cell within its chunk, row-major
   if c is not None and c[CHUNK_CELLS_FIELD][i] != CELL_UNDEFINED:
      return c[CHUNK_CELLS_FIELD][i]
   return CELL_BORDER


def metatile_at(mp: Mapping[str, Any], layer: int, x: int, y: int) -> int:
   """!The metatile id a compiled map's tile layer shows at a position.
   @param mp     compile_map() result
   @param layer  tile layer index
   @param x      metatile column
   @param y      metatile row
   @return metatile id, or NO_METATILE
   """
   c = _chunk_of(mp, x, y)
   i = (y & CHUNK_MASK) * CHUNK + (x & CHUNK_MASK)
   if c is not None and c[CHUNK_CELLS_FIELD][i] != CELL_UNDEFINED:
      return c[CHUNK_GRIDS_FIELD][layer][i]

   # elsewhere the border pattern repeats, shifted by its phase
   bx = (x + mp["phase"][0]) % mp["border_w"]
   by = (y + mp["phase"][1]) % mp["border_h"]
   return mp["border"][layer][by * mp["border_w"] + bx]


# -- world --------------------------------------------------------------------------------

def load_world(path: str | Path,
               sources: Sources | None = None) -> tuple[Json, list[Any], Json, dict[str, Path], dict[str, Path]]:
   """!Reads a world manifest and resolves what it lists (no compiling).
   @param path     world manifest file
   @param sources  in-memory manifests (see load_json()), or None
   @return (stripped world JSON, behavior names in order, vehicle classes,
           {tileset name: Path}, {map name: Path})
   """
   path = Path(path).resolve()
   w = load_json(path, "world", sources)
   check_keys(w, "world", "world")
   base = path.parent
   tilesets = {n: (base / rel).resolve() for n, rel in (w.get("tilesets") or {}).items()}
   maps = {n: (base / rel).resolve() for n, rel in (w.get("maps") or {}).items()}
   behaviors = [b.get("name") for b in w.get("behaviors", [{"name": "normal"}]) if isinstance(b, dict)]
   return w, behaviors, vehicle_class_names(w), tilesets, maps


def vehicle_class_names(w: Mapping[str, Any]) -> list[Any]:
   """!The vehicle class names of a world manifest, in class order (no checks:
   compile_vehicle_classes() reports bad entries).
   @param w  the world manifest
   @return names (an entry without one gives None)
   """
   raw = w.get("vehicle_classes", ["foot"])
   if not isinstance(raw, list):
      return []
   return [c.get("name") if isinstance(c, dict) else c for c in raw]


def compile_vehicle_classes(w: Mapping[str, Any]) -> list[dict[str, Any]]:
   """!The world's movement modes (A2), checked; studios call it in-process.
   Behavior names in "passable" are checked by compile_behaviors().
   @param w  the world manifest
   @return per class {"name", "walk", "run", "can_run", "first_animation",
           "passable" (behavior names, or None: every behavior)}
   """
   raw = w.get("vehicle_classes", ["foot"])
   names = vehicle_class_names(w)
   if not isinstance(raw, list) or not 1 <= len(raw) <= MAX_VEHICLE_CLASSES or \
         not all(isinstance(n, str) and n for n in names) or len(set(names)) != len(names):
      raise ManifestError("world: vehicle_classes needs 1-8 distinct names (class 0 is on foot)")
   out = []
   for i, c in enumerate(raw):
      cw = f"world vehicle class {i} ({names[i]})"
      c = {"name": c} if isinstance(c, str) else c
      check_keys(c, "vehicle_class", cw)
      speeds = c.get("speeds", {})
      check_keys(speeds, "speeds", f"{cw} speeds")
      walk = speeds.get("walk", DEFAULT_WALK_SPEED)
      run = speeds.get("run", DEFAULT_RUN_SPEED)
      for what, v in (("walk", walk), ("run", run)):
         if isinstance(v, bool) or v not in VEHICLE_SPEEDS:
            raise ManifestError(f"{cw}: speeds {what} is pixels per frame, one of "
                                f"{', '.join(str(s) for s in VEHICLE_SPEEDS)}")
      can_run = c.get("can_run", True)
      if not isinstance(can_run, bool):
         raise ManifestError(f"{cw}: can_run must be true or false")
      first = integer(c.get("first_animation", 0), 0, MAX_FIRST_ANIMATION, f"{cw} first_animation")
      if first % ANIMATION_SET != 0:
         raise ManifestError(f"{cw}: first_animation must be a multiple of {ANIMATION_SET} "
                             "(sets go down, up, left, right)")
      passable = c.get("passable")
      if passable is not None and (not isinstance(passable, list) or not all(isinstance(b, str) for b in passable)):
         raise ManifestError(f"{cw}: passable lists behavior names")
      dismount = c.get("dismount", False)
      if not isinstance(dismount, bool) or (dismount and i == 0):
         raise ManifestError(f"{cw}: dismount must be true or false (and class 0, on foot, has nothing to leave)")
      turn_frames = integer(c.get("turn_frames", 0), 0, MAX_TURN_FRAMES, f"{cw} turn_frames")
      for k in ("bumps", "keeps_moving"):
         if not isinstance(c.get(k, False), bool):
            raise ManifestError(f"{cw}: {k} must be true or false")
      out.append({"name": names[i], "walk": walk, "run": run, "can_run": can_run, "first_animation": first,
                  "passable": passable, "dismount": dismount, "turn_frames": turn_frames,
                  "bumps": c.get("bumps", False), "keeps_moving": c.get("keeps_moving", False)})
   return out


def name_list(w: Mapping[str, Any], key: str, most: int) -> list[str]:
   """!One of the world's name lists (F2: object_kinds, map_tags), checked.
   @param w     the world manifest
   @param key   its key
   @param most  most names
   @return the names (empty without the key)
   """
   raw = w.get(key, [])
   if not isinstance(raw, list) or len(raw) > most or not all(isinstance(n, str) and n for n in raw) or \
         len(set(raw)) != len(raw):
      raise ManifestError(f"world: {key} must be a list of at most {most} distinct names")
   return list(raw)


def object_kinds(w: Mapping[str, Any]) -> list[str]:
   """!The world's object kinds (F2; a map object's "kind" names one; ObjectTemplate::kind = index + 1).
   @param w  the world manifest
   @return names
   """
   return name_list(w, "object_kinds", MAX_OBJECT_KINDS)


def map_tags(w: Mapping[str, Any]) -> list[str]:
   """!The world's map tags (F2; Map::tags bit n = tag n).
   @param w  the world manifest
   @return names
   """
   return name_list(w, "map_tags", MAX_MAP_TAGS)


def darkness_levels(w: Mapping[str, Any]) -> list[dict[str, int]]:
   """!The world's darkness levels (C8: data, not engine constants; DEFAULT_DARKNESS_LEVELS when it states none):
   level n (1 up) is entry n - 1; a map's darkness and scripts' `darkness N;` go up to the last level, and an empty
   list makes every map lit.
   @param w  the world manifest
   @return [{"lit": half size in pixels, "brightness": 0-16}]
   """
   raw = w.get("darkness_levels", DEFAULT_DARKNESS_LEVELS)
   if not isinstance(raw, list) or len(raw) > MAX_DARKNESS:
      raise ManifestError(f"world: darkness_levels must be a list of at most {MAX_DARKNESS} levels")
   out = []
   for i, lv in enumerate(raw):
      lw = f"world darkness level {i + 1}"
      check_keys(lv, "darkness_level", lw)
      out.append({"lit": integer(lv.get("lit"), 0, MAX_LIT_HALF, f"{lw} lit"),
                  "brightness": integer(lv.get("brightness"), 0, MAX_DARK_BRIGHTNESS, f"{lw} brightness")})
   return out


def compile_flight_points(w: Mapping[str, Any], map_names: Sequence[str]) -> list[dict[str, Any]]:
   """!The world's flight points (F2), checked; the script compiler emits them (capsule::script::FlightPointInfo).
   @param w          the world manifest
   @param map_names  the world's map names
   @return [{"name", "label", "map", "x", "y", "facing" (index)}]
   """
   raw = w.get("flight_points", [])
   if not isinstance(raw, list) or len(raw) > MAX_FLIGHT_POINTS:
      raise ManifestError(f"world: flight_points must be a list of at most {MAX_FLIGHT_POINTS}")
   out, seen = [], set()
   for i, f in enumerate(raw):
      fw = f"world flight point {i}"
      check_keys(f, "flight_point", fw)
      name = f.get("name")
      if not isinstance(name, str) or not name.isidentifier() or name in seen:
         raise ManifestError(f"{fw}: needs a name of its own (an identifier)")
      seen.add(name)
      label = f.get("label", name)
      if not isinstance(label, str) or not 1 <= len(label) <= MAX_FLIGHT_LABEL:
         raise ManifestError(f"{fw} ({name}): label: 1 to {MAX_FLIGHT_LABEL} letters")
      if f.get("map") not in map_names:
         raise ManifestError(f"{fw} ({name}): unknown map {f.get('map')!r}")
      facing = f.get("facing", "down")
      if facing not in DIRECTIONS:
         raise ManifestError(f"{fw} ({name}): facing must be one of {', '.join(DIRECTIONS)}")
      out.append({"name": name, "label": label, "map": f["map"],
                  "x": integer(f.get("x"), MIN_COORD, MAX_COORD, f"{fw} x"),
                  "y": integer(f.get("y"), MIN_COORD, MAX_COORD, f"{fw} y"), "facing": DIRECTIONS.index(facing)})
   return out


def default_elevations(priorities: Sequence[int] | None = None) -> list[dict[str, Any]]:
   """!The default rule's 16 levels in manifest form (0 meets all, 15 meets
   all and keeps, 1-14 meet themselves), e.g. for a studio's new world.
   @param priorities  16 sprite priorities, or None: DEFAULT_SPRITE_PRIORITY each
   @return 16 {"name", "meets", "on_arrive", "priority"} entries
   """
   prio = list(priorities) if priorities is not None else [DEFAULT_SPRITE_PRIORITY] * ELEVATIONS
   out = []
   for level in range(ELEVATIONS):
      name, meets, arrive = f"level {level}", "same", "take"
      if level == ELEVATION_ANY:
         name, meets = "any", "all"
      elif level == ELEVATION_MULTI:
         name, meets, arrive = "bridge", "all", "keep"
      out.append({"name": name, "meets": meets, "on_arrive": arrive, "priority": prio[level]})
   return out


def compile_elevations(w: Mapping[str, Any]) -> list[tuple[int, int, int, str]]:
   """!The world's 16 levels (A1) as the runtime table; studios call it in-process.
   @param w  the world manifest
   @return per level (meet mask, flags, sprite priority, name)
   """
   raw = w.get("elevations")
   if raw is None:
      prio = w.get("elevation_priority", [DEFAULT_SPRITE_PRIORITY] * ELEVATIONS)
      if not isinstance(prio, list) or len(prio) != ELEVATIONS:
         raise ManifestError("world: elevation_priority needs 16 sprite priorities (0-3)")
      raw = default_elevations([integer(p, 0, MAX_PRIORITY, "world elevation_priority") for p in prio])
   elif "elevation_priority" in w:
      raise ManifestError("world: elevations holds the sprite priorities; leave out elevation_priority")
   if not isinstance(raw, list) or len(raw) != ELEVATIONS:
      raise ManifestError(f"world: elevations needs {ELEVATIONS} levels (a cell's 4 elevation bits)")
   names = []
   for i, e in enumerate(raw):
      check_keys(e, "elevation", f"world elevation {i}")
      n = e.get("name")
      if not isinstance(n, str) or not n.strip() or n in names or n in MEETS_WORDS:
         raise ManifestError(f"world elevation {i}: needs a unique name (not 'all' or 'same')")
      names.append(n)
   out = []
   for i, e in enumerate(raw):
      ew = f"world elevation {i} ({names[i]})"
      meets = e.get("meets", "same")
      if meets == "all":
         mask = MEETS_ALL
      elif meets == "same":
         mask = 1 << i
      elif isinstance(meets, list):
         mask = 1 << i   # a level always meets itself
         for lv in meets:
            if isinstance(lv, str) and lv in names:
               lv = names.index(lv)
            if isinstance(lv, bool) or not isinstance(lv, int) or not 0 <= lv < ELEVATIONS:
               raise ManifestError(f"{ew}: meets lists levels 0-{ELEVATIONS - 1} or level names, got {lv!r}")
            mask |= 1 << lv
      else:
         raise ManifestError(f"{ew}: meets is \"all\", \"same\" or a list of levels")
      arrive = e.get("on_arrive", "take")
      if arrive not in ON_ARRIVE:
         raise ManifestError(f"{ew}: on_arrive is \"take\" or \"keep\"")
      flags = ELEVATION_KEEP if arrive == "keep" else 0
      prio = integer(e.get("priority", DEFAULT_SPRITE_PRIORITY), 0, MAX_PRIORITY, f"{ew} priority")
      out.append((mask, flags, prio, names[i]))
   return out


def check_map_groups(groups: Json, map_names: Sequence[str]) -> None:
   """!Checks the world's map group tree (see the module docstring).
   @param groups     the "map_groups" value
   @param map_names  the world's map names
   @return None
   """
   seen = {}

   def walk(items: Json, where: str, depth: int) -> None:
      """!Checks one level of groups, then their subgroups."""
      if not isinstance(items, list):
         raise ManifestError(f"{where}: groups must be a list")
      if depth > MAX_GROUP_DEPTH:
         raise ManifestError(f"{where}: map groups nest at most {MAX_GROUP_DEPTH} deep")
      names = set()
      for i, g in enumerate(items):
         gw = f"{where} {i}"
         check_keys(g, "map_group", gw)
         n = g.get("name")
         if not isinstance(n, str) or not n.strip() or "/" in n:
            raise ManifestError(f"{gw}: needs a name without '/'")
         if n in names:
            raise ManifestError(f"{gw}: two groups named {n!r} side by side")
         names.add(n)
         gw = f"{where} {n!r}"
         ms = g.get("maps", [])
         if not isinstance(ms, list):
            raise ManifestError(f"{gw}: maps must be a list of map names")
         for m in ms:
            if m not in map_names:
               raise ManifestError(f"{gw}: unknown map {m!r}")
            if m in seen:
               raise ManifestError(f"{gw}: map {m!r} is already in group {seen[m]!r}")
            seen[m] = n
         walk(g.get("groups", []), f"{gw} group", depth + 1)
   walk(groups, "world map_groups", 1)


def raw_chunk_positions(m: Mapping[str, Any]) -> list[tuple[int, int]]:
   """!Chunk positions of a map manifest in the compiled directory's order.
   @param m  the map manifest
   @return [(cx, cy), ...]
   """
   pos = []
   for c in m.get("chunks", []) or []:
      at = c.get("at") if isinstance(c, dict) else None
      if isinstance(at, list) and len(at) == AXES and all(isinstance(v, int) for v in at):
         pos.append((at[0], at[1]))
   if not pos:
      pos = [(0, 0)]   # a blank map: one unpainted chunk
   return sorted(set(pos), key=lambda c: (c[1], c[0]))


def event_cell_ok(m: Mapping[str, Any], x: int, y: int) -> bool:
   """!Whether an event or object may stand on (or a warp land on) a cell: a painted one, or with the map's
   unpainted_events any (the caller still checks the cell is in a chunk).
   @param m  the map manifest
   @param x  metatile column
   @param y  metatile row
   @return True when allowed
   @throws ManifestError when unpainted_events is not true or false
   """
   lenient = m.get("unpainted_events", False)
   if not isinstance(lenient, bool):
      raise ManifestError("unpainted_events must be true or false")
   return lenient or raw_painted(m, x, y)


def raw_painted(m: Mapping[str, Any], x: int, y: int) -> bool:
   """!Whether a map manifest paints a cell.
   @param m  the map manifest
   @param x  metatile column
   @param y  metatile row
   @return True on a painted attribute cell
   """
   for c in m.get("chunks", []) or []:
      if isinstance(c, dict) and c.get("at") == [x >> CHUNK_SHIFT, y >> CHUNK_SHIFT]:
         rows = c.get("cells")
         try:
            return rows[y & CHUNK_MASK][x & CHUNK_MASK] is not None
         except (TypeError, IndexError):
            return False
   return False


def scripts_manifest(w: Mapping[str, Any], base: Path, scripts_path: str | Path | None) -> Path | None:
   """!The game's scripts manifest: the world's "scripts", or the one project.json names (they must agree).
   @param w             the world manifest (stripped)
   @param base          the world's folder
   @param scripts_path  the game's scripts manifest (project.json "scripts"), or None
   @return its resolved path, or None when there is none
   """
   scripts = w.get("scripts")
   if scripts is not None and (not isinstance(scripts, str) or manifest_paths.is_absolute_text(scripts)):
      raise ManifestError("world: scripts names the scripts manifest by a relative path")
   target = (base / scripts).resolve() if scripts else None
   if scripts_path is not None:
      given = Path(scripts_path).resolve()
      if target is not None and target != given:
         raise ManifestError(f"world: scripts names {scripts}, but the game's project.json names "
                             f"{Path(scripts_path).name} in another folder; they must be the same file")
      target = given
   return target


def compile_world_events(w: Mapping[str, Any], base: Path, map_paths: Mapping[str, Path], deps: list[Path],
                         sources: Sources | None = None, scripts_path: str | Path | None = None, *,
                         codepage: text_encoder.CodePage = text_encoder.DEFAULT) -> dict[str, Any]:
   """!Every map's events, area roots and the world's step hooks (tools/event_compiler.py).
   Reads the map manifests themselves, so it works without compiling tilesets
   (the studio checks events this way).
   @param w          the world manifest (stripped)
   @param base       the world's folder
   @param map_paths  {map name: Path}, world order
   @param deps       list files read are appended to
   @param sources    in-memory manifests (see load_json()), or None
   @param scripts_path  the game's scripts manifest (project.json "scripts"), or None: the world's
                        "scripts" must name the same file, or may be left out
   @param codepage   the game's code page (the scripts' inline texts)
   @return dict: events ({map name: compile_events() entry}), hooks, names (event_compiler.Names),
           objects ({map name: object_compiler.compile_objects() entry})
   """
   import script_compiler as sc
   raw = {}
   for n, p in map_paths.items():
      raw[n] = load_json(p, f"map {n!r}", sources)
   target = scripts_manifest(w, base, scripts_path)
   try:
      names = evc.load_names(target, deps, sc, codepage)
      parsed = {n: evc.parse_map(n, m, list(map_paths)) for n, m in raw.items()}
      events = evc.compile_events(list(map_paths), parsed, names, lambda n, x, y: event_cell_ok(raw[n], x, y),
                                  lambda n: raw_chunk_positions(raw[n]))
      hooks = evc.compile_hooks(w.get("step_hooks"))
      parsed_objs = {n: obc.parse_objects(n, m) for n, m in raw.items()}
      objects = obc.compile_objects(list(map_paths), parsed_objs, names,
                                    lambda n, x, y: event_cell_ok(raw[n], x, y),
                                    lambda n: raw_chunk_positions(raw[n]), object_kinds(w),
                                    vehicle_class_names(w))
      self_bits = objects.pop("_self_bits")
      evc.check_ids(list(map_paths), hooks, {n: o["names"] for n, o in objects.items()})
   except evc.EventError as e:
      raise ManifestError(str(e)) from None
   return {"events": events, "hooks": hooks, "names": names, "objects": objects, "self_bits": self_bits}


def used_tilesets(w: Mapping[str, Any], base: Path, only_maps: Iterable[str], sources: Sources | None) -> set[str]:
   """!C10 (S1): the tilesets some maps draw from (their manifests' "tilesets"; a map without the key may use any).
   @param w          the world manifest
   @param base       its folder
   @param only_maps  the maps
   @param sources    in-memory manifests, or None
   @return tileset names (every tileset when a map names none, or cannot be read: its compile reports it)
   """
   every = set((w.get("tilesets") or {}).keys())
   out: set[str] = set()
   maps = w.get("maps") or {}
   for n in only_maps:
      if n not in maps:
         continue
      try:
         m = load_json((base / maps[n]).resolve(), f"map {n!r}", sources)
      except ManifestError:
         return every
      named = m.get("tilesets") if isinstance(m, dict) else None
      if not isinstance(named, list):
         return every
      out |= {str(x) for x in named}
   return out


def compile_world(path: str | Path, deps: list[Path] | None = None, sources: Sources | None = None,
                  images: Mapping[Path, Image] | None = None, only_maps: Iterable[str] | None = None,
                  scripts_path: str | Path | None = None, *,
                  codepage: text_encoder.CodePage = text_encoder.DEFAULT) -> dict[str, Any]:
   """!Compiles a world and everything it lists.
   @param path       world manifest file
   @param deps       list every file read is appended to, or None
   @param sources    in-memory manifests by resolved Path (an editor's unsaved
                     documents), or None
   @param images     in-memory sheet images by resolved Path, or None
   @param only_maps  compile only these map names (editor previews), or None for all
   @param scripts_path  the game's scripts manifest, or None (see compile_world_events())
   @param codepage   the game's code page (location names, scripts, previews)
   @return dict for emit()
   """
   deps = [] if deps is None else deps
   path = Path(path).resolve()
   w = load_json(path, "world", sources)
   check_keys(w, "world", "world")
   if w.get("schema") != "capsule.world/1":
      raise ManifestError("world: schema must be 'capsule.world/1'")
   base = path.parent
   vehicles = compile_vehicle_classes(w)
   classes = [v["name"] for v in vehicles]
   behs, by_name, walls = [], {}, []
   raw = w.get("behaviors", [{"name": "normal"}])
   if not isinstance(raw, list) or not raw or len(raw) > MAX_BEHAVIORS:
      raise ManifestError("world: behaviors needs 1-256 entries")
   for i, b in enumerate(raw):
      bw = f"world behavior {i}"
      check_keys(b, "behavior", bw)
      n = b.get("name")
      if not isinstance(n, str) or not n or n in by_name:
         raise ManifestError(f"{bw}: needs a unique name")
      if i == 0 and n != "normal":
         raise ManifestError("world: behavior 0 must be \"normal\" (cells without a behavior use it)")
      moves = [k for k in ("slide", "ledge", "push", "counter") if b.get(k)]
      if len(moves) > 1:
         raise ManifestError(f"{bw}: slide, ledge, push and counter exclude each other")
      passable = b.get("passable", classes)
      if not isinstance(passable, list) or any(c not in classes for c in passable):
         raise ManifestError(f"{bw}: passable lists vehicle classes ({', '.join(classes)})")

      # one bit per vehicle class that may enter (none when solid)
      mask = 0 if b.get("solid") else sum(1 << classes.index(c) for c in passable)
      kind, direction = 0, 0
      if moves:
         kind = KINDS.index(moves[0])
         if moves[0] not in ("slide", "counter"):
            d = b[moves[0]]
            if d not in DIRECTIONS:
               raise ManifestError(f"{bw}: {moves[0]} needs a direction ({', '.join(DIRECTIONS)})")
            direction = DIRECTIONS.index(d)
      sides = b.get("walls", [])
      if not isinstance(sides, list) or any(s not in DIRECTIONS for s in sides) or len(set(sides)) != len(sides):
         raise ManifestError(f"{bw}: walls lists directions ({', '.join(DIRECTIONS)}), each once")
      walls.append(sum(1 << DIRECTIONS.index(s) for s in sides))
      by_name[n] = i
      behs.append((mask, kind, direction, n))

   # A class with its own passable list enters only those behaviors (and only where they let it).
   for k, v in enumerate(vehicles):
      if v["passable"] is None:
         continue
      for bn in v["passable"]:
         if bn not in by_name:
            raise ManifestError(f"world vehicle class {k} ({v['name']}): passable: unknown behavior {bn!r}")
      behs = [(mask if n in v["passable"] else mask & ~(1 << k), kind, d, n) for mask, kind, d, n in behs]
   elevations = compile_elevations(w)
   darkness = darkness_levels(w)
   tilesets = {}
   wanted = None if only_maps is None else used_tilesets(w, base, only_maps, sources)
   for n, rel in (w.get("tilesets") or {}).items():
      p = (base / rel).resolve()
      deps.append(p)
      if wanted is not None and n not in wanted:
         continue   # C10 (S1): a preview of some maps compiles only the tilesets they draw from
      tilesets[n] = compile_tileset(n, p, by_name, deps, sources, images)
   maps = []
   for n, rel in (w.get("maps") or {}).items():
      if only_maps is not None and n not in only_maps:
         continue
      p = (base / rel).resolve()
      deps.append(p)
      maps.append(compile_map(n, p, tilesets, [bname for _mask, _kind, _dir, bname in behs], sources=sources,
                              tags=map_tags(w), darkest=len(darkness)))
   if not maps:
      raise ManifestError("world: needs at least one map")
   if len({mp["key"] for mp in maps}) != len(maps):
      raise ManifestError("world: two map names give the same key")
   check_map_groups(w.get("map_groups", []), list((w.get("maps") or {}).keys()))
   s = w.get("start")
   check_keys(s, "start", "world start")
   names = [mp["name"] for mp in maps]
   if only_maps is not None:
      names = list((w.get("maps") or {}).keys())
   if s.get("map") not in names:
      raise ManifestError(f"world start: unknown map {s.get('map')!r}")
   facing = s.get("facing", "down")
   if facing not in DIRECTIONS:
      raise ManifestError(f"world start: facing must be one of {', '.join(DIRECTIONS)}")
   start = (names.index(s["map"]), integer(s.get("x"), MIN_COORD, MAX_COORD, "world start x"),
            integer(s.get("y"), MIN_COORD, MAX_COORD, "world start y"), DIRECTIONS.index(facing))
   # F2: the flight points (the script compiler emits them; checked here too) and the object kinds
   compile_flight_points(w, list((w.get("maps") or {}).keys()))
   object_kinds(w)
   map_paths = {n: (base / rel).resolve() for n, rel in (w.get("maps") or {}).items()}
   try:
      encounters = ec.compile_encounters(w, path, classes, map_paths, deps, sources, only_maps)
   except ec.ManifestError as e:
      raise ManifestError(str(e)) from None
   world_events = compile_world_events(w, base, map_paths, deps, sources, scripts_path, codepage=codepage)
   conditions = w.get("encounter_conditions") or []
   import script_compiler as sc

   # Location names are field text when the game's scripts declare field placeholders (D-22 decision 6).
   try:
      field = sc.load_placeholders(scripts_manifest(w, base, scripts_path), codepage)
   except sc.ScriptError as e:
      raise ManifestError(f"world scripts: {e.message}") from None
   try:
      locations = lc.compile_locations(w, base, {n: load_json(p, f"map {n!r}", sources)
                                                 for n, p in map_paths.items()}, deps,
                                       [c if isinstance(c, str) else c.get("name") for c in conditions],
                                       codepage=codepage, field=field)
   except lc.LocationError as e:
      raise ManifestError(str(e)) from None
   for mp in maps:
      loc = locations["map_location"].get(mp["name"])
      mp["_location"] = loc if loc is not None else "kNoLocation"
   for mp in maps:
      if raw_chunk_positions(load_json(map_paths[mp["name"]], "map", sources)) != \
              [(c[0], c[1]) for c in mp["chunks"]]:
         raise ManifestError(f"map {mp['name']!r}: chunk order differs from the event buckets (compiler bug)")
   draw_order = compile_draw_order(w)
   doors = compile_door_bindings(w, maps, world_events["events"])
   interactions = compile_interactions(w, by_name, classes, world_events["names"])
   sets = resolve_override_sets(maps, world_events["names"])
   return {"behaviors": behs, "behavior_walls": walls, "classes": classes, "vehicles": vehicles,
           "elevations": elevations, "maps": maps,
           "start": start, "darkness_levels": darkness,
           "tilesets": tilesets, "encounters": encounters, "events": world_events,
           "map_names": list(map_paths), "locations": locations, "draw_order": draw_order, "doors": doors,
           "override_sets": sets, "interactions": interactions,
           "target_saved": sum(ts.target_saved for ts in tilesets.values())}


def compile_interactions(w: Mapping[str, Any], behaviors: Mapping[str, int], classes: Sequence[str],
                         names: Any) -> list[dict[str, Any]]:
   """!C13b: the world's "interactions" (cell interaction rules, in order; see the module notes).
   @param w          the world manifest
   @param behaviors  behavior name -> index
   @param classes    vehicle class names, in order
   @param names      event_compiler.Names (the scripts' script and flag names)
   @return [{"script", "flag", "behavior", "facing", "vehicle", "flags", "where"}] in the world's order
   """
   raw = w.get("interactions", [])
   if not isinstance(raw, list) or len(raw) > MAX_INTERACTIONS:
      raise ManifestError(f"world: interactions lists up to {MAX_INTERACTIONS} rules")
   out: list[dict[str, Any]] = []
   for i, r in enumerate(raw):
      where = f"world interaction {i}"
      check_keys(r, "interaction", where)
      if r.get("behavior") not in behaviors:
         raise ManifestError(f"{where}: unknown behavior {r.get('behavior')!r}")
      facing = r.get("facing")
      if facing is not None and facing not in DIRECTIONS:
         raise ManifestError(f"{where}: facing must be one of {', '.join(DIRECTIONS)}")
      vehicle = r.get("vehicle")
      if vehicle is not None and vehicle not in classes:
         raise ManifestError(f"{where}: unknown vehicle class {vehicle!r}")
      for key in ("same_level", "after_actions"):
         if not isinstance(r.get(key, False), bool):
            raise ManifestError(f"{where}: {key} must be true or false")
      try:
         script = names.script(r.get("script"), where)
         flag = NO_CONDITION if r.get("flag") is None else names.flag(r["flag"], where)
      except evc.EventError as e:
         raise ManifestError(str(e)) from None
      flags = (INTERACT_SAME_LEVEL if r.get("same_level", False) else 0) | \
              (INTERACT_AFTER_ACTIONS if r.get("after_actions", False) else 0)
      out.append({"script": script, "flag": flag, "behavior": behaviors[r["behavior"]],
                  "facing": ANY_DIRECTION if facing is None else DIRECTIONS.index(facing),
                  "vehicle": ANY_VEHICLE if vehicle is None else classes.index(vehicle), "flags": flags,
                  "where": f"{r['behavior']} -> {r['script']}"})
   return out


def resolve_override_sets(maps: Sequence[dict[str, Any]], names: Any) -> list[dict[str, Any]]:
   """!DS1: every map's override sets as the world's list (map order, then the map's order), their names unique the
   world over (scripts name them) and their flags and variables resolved.
   @param maps   compile_map() results, in the world's order
   @param names  event_compiler.Names (the scripts' flags and variables)
   @return [{"name", "map" (index into maps), "persist" (OVERRIDE_PERSISTS index), "id", "value", "cells"}]
   """
   out: list[dict[str, Any]] = []
   seen: dict[str, str] = {}
   for i, mp in enumerate(maps):
      for s in mp.get("override_sets", []):
         if s["name"] in seen:
            raise ManifestError(f"{s['where']}: override set {s['name']!r} is also one of map {seen[s['name']]!r} "
                                "(scripts name sets the world over)")
         seen[s["name"]] = mp["name"]
         kind, name, value = s["persist"]
         ident = 0
         try:
            if kind == "flag":
               ident = names.flag(name, s["where"])
            elif kind == "var":
               ident, lo, hi = names.var(name, s["where"])
               if not lo <= value <= hi:
                  raise ManifestError(f"{s['where']}: {value} is outside {name}'s range {lo}..{hi}")
         except evc.EventError as e:
            raise ManifestError(str(e)) from None
         out.append({"name": s["name"], "map": i, "persist": ("map", "flag", "var").index(kind), "id": ident,
                     "value": value, "cells": s["cells"]})
   if len(out) > MAX_OVERRIDE_ITEMS or sum(len(s["cells"]) for s in out) > MAX_OVERRIDE_ITEMS:
      raise ManifestError("world: too many override sets or cells (16-bit indices)")
   return out


def emit(src_name: str, world: Mapping[str, Any], debug_names: bool = False) -> str:
   """!Generates the .cpp defining map::kWorld.
   @param src_name     world file name, for the header comment
   @param world        compile_world() result
   @param debug_names  also emit behavior and map names (sandboxes, debug HUDs)
   @return the source text
   """
   L = [f"// Generated by tools/map_compiler.py from {src_name}. Do not edit.",
        '#include "capsule/map/map.h"', "", "namespace capsule::map {", "namespace {", "",
        "using hal::video::Color;", ""]

   def arr(ctype: str, name: str, values: Sequence[int], per_line: int = WORDS_PER_LINE,
           fmt: str = "0x{:08X}") -> None:
      """!An aligned C++ array, per_line values a line."""
      L.append(f"alignas(4) const {ctype} {name}[] = {{")
      for i in range(0, len(values), per_line):
         L.append("    " + ", ".join(fmt.format(v) for v in values[i:i + per_line]) + ",")
      L.append("};")

   # I2: an array equal to one emitted before (maps sharing tilesets or a layout) is not emitted again; the maps
   # name the first one
   emitted: dict[tuple[str, tuple[int, ...]], str] = {}

   def shared(ctype: str, name: str, values: Sequence[int], per_line: int = WORDS_PER_LINE,
              fmt: str = "0x{:08X}") -> str:
      """!arr(), or the name of an equal array emitted before.
      @return the array's name"""
      key = (ctype, tuple(values))
      if key in emitted:
         return emitted[key]
      emitted[key] = name
      arr(ctype, name, values, per_line, fmt)
      return name

   maps = world["maps"]
   enc = world["encounters"]
   L.extend(ec.emit(enc, debug_names))
   L.append("")
   refs = []
   for i, mp in enumerate(maps):
      lines, sym, count = ec.emit_refs(enc, mp["name"], i)
      L.extend(lines)
      refs.append((sym, count))
   syms: list[dict[str, str]] = []
   metatile_syms: dict[tuple[int, ...], str] = {}
   door_syms: dict[tuple[tuple[int, int, int, int], ...], str] = {}   # C10: identical binding lists, stored once
   for i, mp in enumerate(maps):
      sym = {}
      syms.append(sym)
      sym["tiles"] = shared("uint32_t", f"kTiles{i}", [w for t in mp["tiles"] for w in ac.words_4bpp(t)])
      # A blank map uses no palette; C++ has no empty arrays, so it gets one unused black palette.
      sym["palettes"] = shared("Color", f"kPalettes{i}", [c for p in mp["palettes"] for c in p] or
                               [0] * bitmap.MAX_COLORS, bitmap.MAX_COLORS, "0x{:04X}")
      # C++ has no empty arrays: a map without metatiles gets one unused blank one.
      entries = mp["metatiles"] or [[0] * METATILE_TILES]
      flat = tuple(v for e in entries for v in e)
      if flat in metatile_syms:
         sym["metatiles"] = metatile_syms[flat]
      else:
         sym["metatiles"] = metatile_syms[flat] = f"kMetatiles{i}"
         L.append(f"const Metatile kMetatiles{i}[] = {{")
         L.extend("    {{" + ", ".join(f"0x{v:04X}" for v in e) + "}}," for e in entries)
         L.append("};")
      sym["cells"] = shared("MapCell", f"kCells{i}", [v for c in mp["chunks"] for v in c[CHUNK_CELLS_FIELD]],
                            WORDS_PER_LINE, "0x{:08X}")
      sym["layer_cells"] = shared("MetatileId", f"kLayerCells{i}",
                                  [v for c in mp["chunks"] for g in c[CHUNK_GRIDS_FIELD] for v in g] or
                                  [NO_METATILE], HALFWORDS_PER_LINE, "0x{:04X}")
      ev = world["events"]["events"][mp["name"]]

      # chunk n's cells start at n * 64, its layer cells at n * 64 * layers
      L.append(f"const ChunkDirectoryEntry kChunks{i}[] = {{")
      L.extend(f"    {{{c[0]}, {c[1]}, {n * CHUNK_CELLS}u, "
               f"{n * CHUNK_CELLS * len(mp['layers'])}u, 0xFFFF, 0xFFFF, 0, "
               f"{ev['ranges'].get(n, (0, 0))[1]}, {ev['ranges'].get(n, (0, 0))[0]}, 0}},"
               for n, c in enumerate(mp["chunks"]))
      L.append("};")
      ev_lines, ev_sym, warp_sym = evc.emit_map(i, ev)
      L.extend(ev_lines)
      mp["_event_syms"] = (ev_sym, warp_sym, len(ev["events"]), len(ev["warps"]), ev["area"])
      objs = world["events"]["objects"][mp["name"]]
      ob_lines, ob_sym, pg_sym, rc_sym, ro_sym = obc.emit_map(i, objs)
      L.extend(ob_lines)
      mp["_object_syms"] = (ob_sym, pg_sym, rc_sym, ro_sym, len(objs["pages"]), len(objs["routes"]),
                            len(objs["objects"]), mp.get("_location", "kNoLocation"))
      sym["grid"] = shared("uint16_t", f"kGrid{i}", mp["grid"], HALFWORDS_PER_LINE, "0x{:04X}")
      sym["border"] = shared("MetatileId", f"kBorder{i}", [v for pat in mp["border"] for v in pat] or [NO_METATILE],
                             HALFWORDS_PER_LINE, "0x{:04X}")
      frames = [shared("uint32_t", f"kAnimFrames{i}_{j}", [w for fr in an["frames"] for t in fr
                                                           for w in ac.words_4bpp(t)])
                for j, an in enumerate(mp["anims"])]
      more_targets = []   # DS1: (first tile, offset, phase) of the animations' more targets, in animation order
      if mp["anims"]:
         L.append(f"const TileAnimation kAnims{i}[] = {{")
         for j, an in enumerate(mp["anims"]):
            phase = an.get("phase")
            if phase is None:
               phase = (j * an["period"]) // max(1, len(mp["anims"]))   # spread uploads over the period
            # DS1: the count of more targets only when it has some (the initializer of before stays valid)
            more = an.get("targets", [])
            count = f", {len(more)}" if more else ""
            L.append(f"    {{{frames[j]}, {an['first']}, {an['count']}, {len(an['frames'])}, "
                     f"{an['period']}, {phase % an['period']}, {anim_flags_text(an)}{count}}},   // {an['name']}")
            more_targets += [(first, offset, (phase if ph is None else ph) % an["period"], an["name"])
                             for first, offset, ph in more]
         L.append("};")
      mp["_targets"] = f"kAnimTargets{i}" if more_targets else "nullptr"
      if more_targets:
         L.append(f"const AnimTarget kAnimTargets{i}[] = {{")
         L.extend(f"    {{{first}, {offset}, {phase}}},   // {name}" for first, offset, phase, name in more_targets)
         L.append("};")
      bindings = world.get("doors", {}).get(mp["name"], [])
      # C10 (DS1 note): maps of the same tilesets bind the same metatiles: a binding list another map already has
      # is that map's table (the bindings and the lookup are unchanged; only the ROM copies go)
      shape = tuple((k, mt, x, y) for k, mt, x, y, _text in bindings)
      if bindings and shape not in door_syms:   # DS1: cell bindings first, then metatile bindings
         door_syms[shape] = f"kDoorBindings{i}"
         L.append(f"const DoorBinding kDoorBindings{i}[] = {{")
         L.extend(f"    {{AssetKey{{0x{k:08X}u}}, 0x{mt:04X}, {x}, {y}, 0}},   // {text}"
                  for k, mt, x, y, text in bindings)
         L.append("};")
      mp["_doors"] = (door_syms[shape] if bindings else "nullptr", len(bindings))
      L.append(f"static_assert({len(mp['tiles'])} <= kMaxTiles, \"map {mp['name']}: too many tiles\");")
      L.append(f"static_assert({len(mp['palettes'])} <= kMaxPalettes, \"map {mp['name']}: too many palettes\");")
      L.append("")
   L.append("const Map kMaps[] = {")
   for i, mp in enumerate(maps):
      anims = f"kAnims{i}" if mp["anims"] else "nullptr"
      lays = ", ".join(f"{{{bg}, {pr}, {{0, 0}}}}" for bg, pr in mp["layers"]) or "{0, 0, {0, 0}}"
      ev_sym, warp_sym, n_events, n_warps, area = mp["_event_syms"]
      sym = syms[i]
      L.append(f"    {{kChunks{i}, {sym['grid']}, {sym['cells']}, {sym['layer_cells']}, {sym['metatiles']}, "
               f"{sym['tiles']}, {sym['palettes']}, "
               f"{sym['border']}, {anims}, {refs[i][0]}, {{{lays}}}, {mp['grid_x0']}, {mp['grid_y0']}, "
               f"{mp['grid_w']}, "
               f"{mp['grid_h']}, {len(mp['chunks'])}, {len(mp['metatiles'])}, {len(mp['tiles'])}, "
               f"0x{mp['backdrop']:04X}, {len(mp['palettes'])}, {mp['border_w']}, {mp['border_h']}, "
               f"{mp['phase'][0]}, {mp['phase'][1]}, {len(mp['anims'])}, {len(mp['layers'])}, "
               f"{refs[i][1]}, {ev_sym}, {warp_sym}, {n_events}, {n_warps}, {area}, "
               f"{', '.join(str(v) for v in mp['_object_syms'])}"
               # flags, darkness and tags only when set (they default to 0)
               f"{map_trailer(mp)}}},"
               f"   // {mp['name']}")
   L.append("};")
   order = sorted(range(len(maps)), key=lambda i: maps[i]["key"])
   L.append("const MapKey kMapKeys[] = {")
   L.extend(f"    {{AssetKey{{0x{maps[i]['key']:08X}u}}, {i}, 0}},   // map:{maps[i]['name']}" for i in order)
   L.append("};")
   L.append("const Behavior kBehaviors[] = {")
   walls = world.get("behavior_walls") or [0] * len(world["behaviors"])
   L.extend(f"    {{0x{m:02X}, BehaviorKind::{['None', 'Slide', 'Ledge', 'Push', 'Counter'][k]}, {d}, {walls[i]}}},"
            f"   // {n}"
            for i, (m, k, d, n) in enumerate(world["behaviors"]))
   L.append("};")
   L.append("const Elevation kElevations[] = {")
   L.extend(f"    {{0x{m:04X}, {'kElevationKeep' if f & ELEVATION_KEEP else '0'}, {p}}},   // {i}: {n}"
            for i, (m, f, p, n) in enumerate(world["elevations"]))
   L.append("};")
   L.append("const VehicleClass kVehicleClasses[] = {")
   # I3: the turn time only when set (the initializer of before stays valid)
   L.extend(f"    {{{v['walk']}, {v['run']}, {v['first_animation']}, {vehicle_flags(v)}"
            f"{', ' + str(v['turn_frames']) if v.get('turn_frames') else ''}}},   // {v['name']}"
            for v in world["vehicles"])
   L.append("};")
   # C8: the world's darkness levels (level n is entry n - 1)
   dark = world.get("darkness_levels", [])
   dark_sym = "kDarknessLevels" if dark else "nullptr"
   if dark:
      L.append("const DarknessLevel kDarknessLevels[] = {")
      L.extend(f"    {{{lv['lit']}, {lv['brightness']}}},   // level {i + 1}" for i, lv in enumerate(dark))
      L.append("};")
   if debug_names:
      L.append("const char* const kBehaviorNames[] = {")
      L.extend(f'    "{n}",' for _, _, _, n in world["behaviors"])
      L.append("};")
      L.append("const char* const kMapNames[] = {")
      L.extend(f'    "{mp["name"]}",' for mp in maps)
      L.append("};")
   hook_lines, hook_sym = evc.emit_hooks(world["events"]["hooks"])
   L.extend(hook_lines)
   self_bytes = (world["events"]["self_bits"] + BITS_PER_BYTE - 1) // BITS_PER_BYTE   # bits rounded up to bytes
   self_sym = "nullptr"
   if self_bytes:
      self_sym = "g_self_switches"
      L.append(f"uint8_t g_self_switches[{self_bytes}];   // map objects' self switches A-D (permanent, D-17)")
   start_map, start_x, start_y, start_facing = world["start"]
   names = ("kBehaviorNames", "kMapNames") if debug_names else ("nullptr", "nullptr")
   ds1 = emit_world_ds1(L, world)
   L += ["", "} // namespace", "",
         f"const World kWorld = {{kMaps, kMapKeys, kBehaviors, kElevations, {names[0]}, {names[1]}, "
         f"&kEncounters, {hook_sym}, {self_sym}, {len(maps)}, {len(world['behaviors'])}, {self_bytes}, "
         f"{len(world['classes'])}, {len(world['events']['hooks'])}, "
         f"{{{start_map}, {start_x}, {start_y}, {start_facing}, 0}}, kVehicleClasses, {dark_sym}, {len(dark)}"
         f"{ds1}}};",
         "", "} // namespace capsule::map", ""]
   L.extend(lc.emit(world["locations"], ac.words_4bpp))
   return "\n".join(L)


def emit_world_ds1(L: list[str], world: Mapping[str, Any]) -> str:
   """!DS1: the world's draw-order rule and override sets (arrays appended to L).
   @param L      the source lines
   @param world  compile_world() result
   @return the World initializer's last fields (empty when the world has neither, so the initializer of before stays)
   """
   rule = world.get("draw_order")
   sets = world.get("override_sets", [])
   rules = world.get("interactions", [])
   if rule is None and not sets and not rules:
      return ""
   draw = "nullptr"
   if rule is not None:
      draw = "&kDrawOrder"
      L.append(f"const DrawOrder kDrawOrder = {{{{{', '.join(str(v) for v in rule['base'])}}}, {rule['bias']}, "
               f"0x{rule['mask']:02X}, {rule['row_shift']}, {rule['rows']}, {rule['row_step']}, {rule['add']}, "
               "{0, 0}};")
   tail = emit_world_interactions(L, rules)
   if not sets:
      return f", {draw}" + (", nullptr, nullptr, nullptr, 0" + tail if tail else "")
   cells, entries, rows = [], [], []
   for s in sets:
      rows.append(f"    {{{s['map']}, {len(cells)}, {len(s['cells'])}, {s['id']}, {s['value']}, "
                  f"OverridePersist::{OVERRIDE_PERSISTS[s['persist']]}, 0}},   // {s['name']}")
      for x, y, layer, four, metatile in s["cells"]:
         if four is not None:
            cells.append(f"    {{{x}, {y}, {layer}, kOverrideEntries, {len(entries) // METATILE_TILES}}},")
            entries.extend(four)
         else:
            cells.append(f"    {{{x}, {y}, {layer}, 0, 0x{metatile:04X}}},")
   L.append("const OverrideSet kOverrideSets[] = {")
   L.extend(rows)
   L.append("};")
   L.append("const OverrideCell kOverrideCells[] = {")
   L.extend(cells)
   L.append("};")
   entry_sym = "nullptr"
   if entries:
      entry_sym = "kOverrideEntryData"
      L.append("alignas(4) const uint16_t kOverrideEntryData[] = {")
      L.extend("    " + ", ".join(f"0x{v:04X}" for v in entries[k:k + HALFWORDS_PER_LINE]) + ","
               for k in range(0, len(entries), HALFWORDS_PER_LINE))
      L.append("};")
   return f", {draw}, kOverrideSets, kOverrideCells, {entry_sym}, {len(sets)}{tail}"


def emit_world_interactions(L: list[str], rules: Sequence[Mapping[str, Any]]) -> str:
   """!C13b: the world's cell interaction rules (an array appended to L).
   @param L      the source lines
   @param rules  compile_interactions() result
   @return the World initializer's last two fields (empty without rules)
   """
   if not rules:
      return ""
   L.append("const Interaction kInteractions[] = {")
   L.extend(f"    {{{r['script']}, 0x{r['flag']:04X}, {r['behavior']}, {r['facing']}, 0x{r['vehicle']:02X}, "
            f"{r['flags']}}},   // {r['where']}" for r in rules)
   L.append("};")
   return f", kInteractions, {len(rules)}"


def anim_flags_text(an: Mapping[str, Any]) -> str:
   """!A TileAnimation's flags initializer (I3): "0", or the kAnim... names.
   @param an  compile_map() animation
   @return C++ text
   """
   f = an.get("flags", 0)
   names = [n for bit, n in ((ANIM_FROM_SHEET, "kAnimFromSheet"), (ANIM_RELOAD_ON_SHIFT, "kAnimReloadOnShift"),
                             (ANIM_RESTART_ON_SHIFT, "kAnimRestartOnShift")) if f & bit]
   return " | ".join(names) or "0"


def vehicle_flags(v: Mapping[str, Any]) -> str:
   """!A vehicle class's VehicleClass::flags initializer.
   @param v  compile_vehicle_classes() entry
   @return C++ text (kVehicleCanRun, F2's kVehicleDismount, or 0)
   """
   bits = (["kVehicleCanRun"] if v["can_run"] else []) + (["kVehicleDismount"] if v.get("dismount") else []) + \
      (["kVehicleBumps"] if v.get("bumps") else []) + (["kVehicleKeepsMoving"] if v.get("keeps_moving") else [])
   return " | ".join(bits) or "0"


def map_trailer(mp: Mapping[str, Any]) -> str:
   """!The Map initializer's optional last fields: flags, darkness and tags, as far as any is set (a map without
   them keeps the shorter initializer of before F2).
   @param mp  compile_map() result
   @return C++ text (empty, or starting with ", ")
   """
   flags = (["kMapNoRun"] if mp["flags"] & MAP_NO_RUN else []) + \
      (["kMapEscapePoint"] if mp["flags"] & MAP_ESCAPE_POINT else [])
   view = tuple(mp.get("object_view", (0, 0, 0, 0)))
   targets = mp.get("_targets", "nullptr")
   doors, door_count = mp.get("_doors", ("nullptr", 0))
   # each field with its default, then the last one that is set decides how many are written
   fields = [(" | ".join(flags) or "0", bool(flags)), (str(mp.get("darkness", 0)), bool(mp.get("darkness"))),
             (f"0x{mp.get('tags', 0):04X}", bool(mp.get("tags"))),
             ("{" + ", ".join(str(n) for n in view) + "}", any(view)),   # I3: the object view
             (targets, targets != "nullptr"),   # DS1: the animations' more targets
             (doors, door_count > 0), (str(door_count), door_count > 0)]   # DS1: the door bindings
   last = max((k for k, (_text, used) in enumerate(fields) if used), default=-1)
   return "".join(", " + text for text, _used in fields[:last + 1])


def main(argv: Sequence[str]) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return process exit status
   """
   args = list(argv[1:])
   try:
      codepage = text_encoder.option(args)
   except text_encoder.TextError as e:
      print(f"error: {e}", file=sys.stderr)
      return 1
   debug = "--debug-names" in args
   if debug:
      args.remove("--debug-names")
   ids = None
   if "--ids" in args:
      i = args.index("--ids")
      if i + 1 >= len(args):
         print(__doc__)
         return EXIT_USAGE
      ids = Path(args[i + 1])
      del args[i:i + OPTION_WORDS]
   world_ids = None
   if "--world-ids" in args:
      i = args.index("--world-ids")
      if i + 1 >= len(args):
         print(__doc__)
         return EXIT_USAGE
      world_ids = Path(args[i + 1])
      del args[i:i + OPTION_WORDS]
   scripts_path = None
   if "--scripts" in args:
      i = args.index("--scripts")
      if i + 1 >= len(args):
         print(__doc__)
         return EXIT_USAGE
      scripts_path = Path(args[i + 1])
      del args[i:i + OPTION_WORDS]
   depfile = None
   if "--depfile" in args:
      i = args.index("--depfile")
      if i + 1 >= len(args):
         print(__doc__)
         return EXIT_USAGE
      depfile = Path(args[i + 1])
      del args[i:i + OPTION_WORDS]
   if len(args) != POSITIONAL_ARGS:
      print(__doc__)
      return EXIT_USAGE
   src, dst = Path(args[0]), Path(args[1])
   deps = []
   try:
      world = compile_world(src, deps, scripts_path=scripts_path, codepage=codepage)
   except (ManifestError, ac.ManifestError, SequenceManifestError) as e:
      print(f"{src}: error: {e}", file=sys.stderr)
      return 1
   try:
      text = emit(src.name, world, debug)
      id_text = ec.emit_ids(src.name, world["encounters"]) if ids is not None else None
      world_text = evc.emit_ids(src.name, world["map_names"], world["events"]["hooks"],
                                {n: o["names"] for n, o in world["events"]["objects"].items()},
                                [s["name"] for s in world.get("override_sets", [])]) \
          if world_ids is not None else None
   except (ec.ManifestError, evc.EventError) as e:
      print(f"{src}: error: {e}", file=sys.stderr)
      return 1
   for out, content in ((dst, text), (ids, id_text), (world_ids, world_text)):
      if out is None:
         continue
      out.parent.mkdir(parents=True, exist_ok=True)
      if not out.exists() or out.read_bytes() != content.encode("utf-8"):
         out.write_text(content, encoding="utf-8", newline="\n")
   if depfile is not None:
      def esc(p: Path) -> str:
         """!A path as a Make depfile writes it (forward slashes, spaces escaped)."""
         return str(p).replace("\\", "/").replace(" ", "\\ ")
      depfile.parent.mkdir(parents=True, exist_ok=True)
      depfile.write_text(f"{esc(dst)}: " + " ".join(esc(p) for p in sorted(set(deps))) + "\n",
                         encoding="utf-8", newline="\n")
   tiles = sum(len(mp["tiles"]) for mp in world["maps"])
   chunks = sum(len(mp["chunks"]) for mp in world["maps"])
   print(f"{src}: {len(world['maps'])} maps, {chunks} chunks, {tiles} tiles, "
         f"{len(world['behaviors'])} behaviors; encounters: {ec.summary(world['encounters'])}")
   targets = sum(len(an.get("targets", [])) for mp in world["maps"] for an in mp["anims"])
   if targets:   # DS1: what the animations' more targets save (frames stored once per tileset animation)
      print(f"{src}: {targets} more animation targets on the maps; {world['target_saved']} B of ROM saved by the "
            "tilesets' targets against one animation per target")
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv))
