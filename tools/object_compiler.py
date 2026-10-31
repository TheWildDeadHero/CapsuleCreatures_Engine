#!/usr/bin/env python3
"""!Map objects (B-21, D-17): the NPCs, spotters and pickups of each map,
with RPG Maker XP-style event pages, triggers, movement options and move
routes. Called by map_compiler.py; not run on its own.

Map (`capsule.map/2`), optional key:

    "objects": [
      {"name": "guard", "at": [x, y], "elevation": 0,
       "sprite": "sprite:NPC_GUARD",           // the sprite asset (C-02d key)
       "facing": "down",                        // default down
       "movement": "static",                    // see below; default static
       "range": [1, 1],                         // wander / pace: cells each way (0-7)
       "route": {"steps": ["left 2", "wait 30", "right 2"], "repeat": true, "skip": false},
       "speed": 2, "frequency": 3,              // 1-4 and 1-5 (defaults 2 and 3)
       "options": {"walk_anim": true, "step_anim": false, "direction_fix": false,
                   "through": false, "on_top": false},
       "script": "guard_talk",                  // what the trigger starts
       "trigger": "action",                     // see below; default action
       "flag": "guard_gone",                    // hidden while this flag is set
       "self_switches": true,                   // give it self switches A-D (usually found by itself)
       "spotter": {"sight": 4, "until": "beat_guard"},
       "kind": "bush",                          // F2: one of the world's object_kinds (field actions)
       "pages": [{"when": {"flag": "badge_1"}, "movement": "wander", "range": [2, 2],
                  "script": "guard_after"},
                 {"when": {"self": "A"}, "hidden": true}]}
    ]

- **name**: an identifier, unique in the map. world_ids.h names the object's
  local ID OBJ_<MAP>_<NAME> for game code; scripts address objects by local
  ID (the list index, as a number or a script constant), `self` or `player`.
- **movement** (the classic handheld games' set and RPG Maker XP's): `static` (stands facing
  its direction), `look_around` (turns to a random direction now and then),
  `wander` (random steps inside its range box around its cell),
  `pace_vertical` / `pace_horizontal` (walks to and fro within its range),
  `approach` (steps towards the player), `custom` (follows its "route";
  tools/route_compiler.py lists the steps). `schedule` needs the world clock
  (C-05) and is refused until it exists.
- I3: the classic handheld games' object movement types, frame for frame (capsule/map/map.h "Movement
  arguments"; their settings go in "movement_args"): `turn` (turns after a random delay to a random entry of
  "choices": 2 or 4 directions, 4 may repeat one; "delays": medium or short; "watch_runner": true turns a spotter to
  a running player in its sight box), `roam` (likewise, and steps there when it can), `rotate` ("clockwise",
  "watch_runner"), `patrol` (back and forth along its facing's line within its range), `circuit` ("legs": four
  directions, each walked to the end of its range), `mirror` (copies the player's moves turned by the angle from the
  player's facing at spawn to its "facing"; "keep_to": a vehicle class whose terrain it keeps to), `in_place`
  ("pace": slow, walk, jog or run) and `invisible` (never drawn, still solid); `static` takes "keep_facing": true
  (no turn back after a talk). A range of 0 on an axis leaves these movements unbounded on it.
- I3: a map with an "object_view" may list up to 64 objects (they spawn near the player); "stacked_objects": true
  lets objects share a cell (a game whose flags keep at most one of them there).
- **trigger** (RPG Maker XP's): `action` (A, facing it), `player_touch` (the
  player walks into it), `event_touch` (it walks into the player, or the
  player into it), `autorun` (runs again and again while its page is
  active; the player waits), `parallel` (runs alongside play).
- **spotter**: when the player is in the object's line of sight (its facing,
  up to "sight" cells, 1-8, nothing in between) the object walks up to it
  and its script runs (an opponent who challenges the player, a guard...).
  "until" turns the sight off while set; the script (or the battle it
  starts) sets it. A spotter needs both.
- **pages** (RPG Maker XP's event pages): each has conditions ("when": a
  flag set, a variable at least a value, or equal with "equal": true, self
  switches on: "self": "A" or ["A", "B"]; at least one) and the fields it
  replaces (sprite, facing, movement, range, route, speed, frequency,
  options, script, trigger), or "hidden": true (the object is absent). The
  last page whose conditions all hold is the active one; with none, the
  object's own fields are. Pages are chosen again whenever a flag, variable
  or self switch changes.
- **kind** (F2, D-27 section 3): one of the world's `object_kinds` (a bush,
  a boulder, ...). Field actions target a kind: facing an object of it
  with A (and the object has no script of its own to run) offers them.
- **self switches** A-D are four permanent bits per object, given to objects
  whose pages test them, whose scripts (or scripts they call) use them, or
  that ask with "self_switches": true.

Checks (build errors): unknown keys, scripts, flags and variables; names
that are not identifiers or repeat; more than 15 objects (sprite slots
1-15; the walker has slot 0); cells that are not painted or outside the
map's chunks; two objects on one cell whose levels meet (equal, or either
is 0 = any or 15 = multi-level); ranges, speeds and frequencies out of
range; a custom movement without a route, or a route without it; malformed
route steps; autorun, parallel and spotter objects without a script; a
spotter without an until flag; a page without conditions or that changes
nothing; variable values outside the variable's range. Sprite assets are
not checked here (the studio warns about missing ones).
"""
from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from typing import Any

import event_compiler as evc
import route_compiler as rc
from sequence_compiler import asset_key, ManifestError as KeyError_

## Movement names in capsule::map::Movement order.
MOVEMENTS = ["static", "look_around", "wander", "pace_vertical", "pace_horizontal", "schedule", "approach", "custom",
             "turn", "roam", "rotate", "patrol", "circuit", "mirror", "in_place", "invisible"]
## Settings each I3 movement takes in "movement_args".
MOVEMENT_ARGS = {"static": {"keep_facing"}, "turn": {"choices", "delays", "watch_runner"},
                 "roam": {"choices", "delays"},
                 "rotate": {"clockwise", "watch_runner"}, "patrol": set(), "circuit": {"legs"},
                 "mirror": {"facing", "keep_to"}, "in_place": {"pace"}, "invisible": set()}
## Delay tables (capsule::map::kMoveDelays...).
DELAYS = ["medium", "short"]
## Walk-in-place paces (capsule::map::kInPlace...).
PACES = ["slow", "walk", "jog", "run"]
## movement_flags bits (capsule::map::kMoveTwoChoices, kMoveWatchRunner, kStaticKeepsFacing, kRotateClockwise).
MOVE_TWO_CHOICES, MOVE_WATCH_RUNNER, STATIC_KEEPS_FACING, ROTATE_CLOCKWISE = 1, 0x40, 0x80, 1
## Shift of the delay table in movement_flags (capsule::map::kMoveDelaysShift).
MOVE_DELAYS_SHIFT = 4
## Bits per choice or leg in movement_arg (capsule::map::kCircuitLegShift).
LEG_SHIFT = 2
## Entries of a choice table or a circuit.
LEGS = 4
## Choices of a two-way table.
TWO_CHOICES = 2
## Shift of a mirror's vehicle class + 1 in movement_arg (capsule::map::kMirrorClassShift).
MIRROR_CLASS_SHIFT = 2
## Most vehicle classes a mirror may keep to (3 bits: class + 1).
MAX_MIRROR_CLASS = 6
## Trigger names in capsule::map::Trigger order.
TRIGGERS = ["action", "player_touch", "event_touch", "autorun", "parallel"]
## Objects per map (capsule::map::kMaxObjects).
MAX_OBJECTS = 15
## Objects per map with an object view (I3, capsule::map::kMaxObjectTemplates).
MAX_VIEW_OBJECTS = 64
## Longest spotter sight.
MAX_SIGHT = 8
## Largest range each way (4 bits; I3: was 7).
MAX_RANGE = 15
## Parallel objects per map (overworld::kMaxParallel script contexts).
MAX_PARALLEL = 4
## Pages per map (16-bit indices) and per object (8-bit count; 255 is field::kHiddenPage).
MAX_PAGES, MAX_OBJECT_PAGES = 0xFFFF, 254
## Routes per map.
MAX_ROUTES = 0xFFFE
## No script / flag / variable.
NONE = evc.NO_CONDITION
## ObjectTemplate::kind of an object without a kind (capsule::map::kNoObjectKind; kinds are index + 1).
NO_KIND = 0
## Most object kinds of a world (an 8-bit kind, 0 = none).
MAX_KINDS = 0xFF - 1
## No route (capsule::map::kNoRoute) and no self switches (kNoSelfSwitches).
NO_ROUTE, NO_SELF = 0xFFFF, 0xFFFF
## Page field bits (capsule::map::kPage...).
PAGE_SPRITE, PAGE_SCRIPT, PAGE_MOVEMENT, PAGE_FACING, PAGE_TRIGGER, PAGE_MOTION, PAGE_OPTIONS, PAGE_HIDDEN = \
    1, 2, 4, 8, 16, 32, 64, 128
## Page condition bit: the variable must equal the value.
PAGE_VAR_EQUAL = 1
## Defaults: speed, frequency.
DEFAULT_SPEED, DEFAULT_FREQUENCY = 2, 3
## Numbers in a range box ([x, y]).
RANGE_AXES = 2
## Bit position of y in a packed range box (x | y << 4).
RANGE_Y_SHIFT = 4
## Range box of an object that gives none: one cell each way.
DEFAULT_RANGE = 1 | (1 << RANGE_Y_SHIFT)
## Bit position of the frequency in a packed motion byte (speed | frequency << 4).
FREQUENCY_SHIFT = 4
## Motion byte of the default speed and frequency.
DEFAULT_MOTION = DEFAULT_SPEED | (DEFAULT_FREQUENCY << FREQUENCY_SHIFT)
## Elevation of an object on every level (bridges); it meets every other level, as 0 (any) does.
ELEVATION_MULTI = 15
## Self switches per object (A-D), as bits of the world's self switch bit set.
SELF_SWITCHES = 4
## Most self switch bits in a world (0xFFFF is NO_SELF).
MAX_SELF_BITS = 0xFFFE
## Bytes per line of an emitted route code array.
BYTES_PER_LINE = 16

## A parsed JSON object.
JsonDict = dict[str, Any]
## (movement, range, route index, movement_arg, movement_flags) of an object or page.
MovementFields = tuple[int, int, int, int, int]

## Keys per object kind.
KEYS = {
   "object": {"name", "at", "elevation", "sprite", "facing", "movement", "range", "route", "speed", "frequency",
              "options", "script", "trigger", "flag", "self_switches", "spotter", "pages", "kind", "movement_args"},
   "spotter": {"sight", "until"},
   "page": {"when", "sprite", "facing", "movement", "range", "route", "speed", "frequency", "options", "script",
            "trigger", "hidden", "movement_args"},
   "when": {"flag", "var", "value", "equal", "self"},
   "route": {"steps", "repeat", "skip"},
}


def _keys(obj: Any, kind: str, where: str) -> None:
   """!Rejects unknown keys.
   @param obj    the object
   @param kind   key set in KEYS
   @param where  location for messages
   @return None
   """
   if not isinstance(obj, dict):
      raise evc.EventError(f"{where}: expected an object")
   unknown = sorted(set(obj) - KEYS[kind])
   if unknown:
      raise evc.EventError(f"{where}: unknown key(s) {', '.join(unknown)}")


def _movement(value: Any, where: str) -> int:
   """!A movement type.
   @param value  its name
   @param where  location for messages
   @return its index
   """
   if value == "schedule":
      raise evc.EventError(f"{where}: movement 'schedule' needs the world clock (C-05), which is not built yet")
   if value not in MOVEMENTS:
      names = [m for m in MOVEMENTS if m != "schedule"]
      raise evc.EventError(f"{where}: movement must be one of {', '.join(names)}, got {value!r}")
   return MOVEMENTS.index(value)


def _trigger(value: Any, where: str) -> int:
   """!A trigger.
   @param value  its name
   @param where  location for messages
   @return its index
   """
   if value not in TRIGGERS:
      raise evc.EventError(f"{where}: trigger must be one of {', '.join(TRIGGERS)}, got {value!r}")
   return TRIGGERS.index(value)


def _range(value: Any, where: str) -> int:
   """!A range box.
   @param value  [x, y]
   @param where  location for messages
   @return x | y << 4
   """
   if not isinstance(value, list) or len(value) != RANGE_AXES:
      raise evc.EventError(f"{where}: range must be [x, y]")
   x, y = value
   rx = evc._int(x, 0, MAX_RANGE, f"{where} range")
   ry = evc._int(y, 0, MAX_RANGE, f"{where} range")
   return rx | (ry << RANGE_Y_SHIFT)


def _facing(value: Any, where: str) -> int:
   """!A facing.
   @param value  direction name
   @param where  location for messages
   @return field::Direction
   """
   if value not in evc.DIRECTIONS:
      raise evc.EventError(f"{where}: facing must be one of {', '.join(evc.DIRECTIONS)}, got {value!r}")
   return evc.DIRECTIONS.index(value)


def _sprite(value: Any, where: str) -> int:
   """!A sprite reference's key.
   @param value  "sprite:NAME"
   @param where  location for messages
   @return the asset key
   """
   if not isinstance(value, str) or not value.startswith("sprite:"):
      raise evc.EventError(f"{where}: sprite must be a sprite asset reference, 'sprite:NAME'")
   try:
      return asset_key(value, f"{where} sprite")
   except KeyError_ as e:
      raise evc.EventError(str(e)) from None


def _options(value: Any, base: int, where: str) -> int:
   """!Movement options.
   @param value  {option: bool}
   @param base   bits they start from
   @param where  location for messages
   @return route::kOption... bits
   """
   if not isinstance(value, dict):
      raise evc.EventError(f"{where}: options must be an object")
   bits = base
   for k, v in value.items():
      if k not in rc.OPTIONS:
         raise evc.EventError(f"{where} options: unknown option {k!r} ({', '.join(rc.OPTIONS)})")
      if not isinstance(v, bool):
         raise evc.EventError(f"{where} options: {k} must be true or false")
      bits = (bits | rc.OPTIONS[k]) if v else (bits & ~rc.OPTIONS[k])
   return bits


def _route(value: Any, names: evc.Names, where: str) -> bytes:
   """!Compiles a move route.
   @param value  {"steps": [...], "repeat": bool, "skip": bool}
   @param names  event_compiler.Names
   @param where  location for messages
   @return route bytes
   """
   _keys(value, "route", f"{where} route")
   steps = value.get("steps")
   if not isinstance(steps, list) or not steps:
      raise evc.EventError(f"{where} route: steps must be a non-empty list")
   for k in ("repeat", "skip"):
      if not isinstance(value.get(k, False), bool):
         raise evc.EventError(f"{where} route: {k} must be true or false")
   try:
      words = [rc.parse_text(t) for t in steps]
      return rc.compile_route(words, value.get("repeat", True), value.get("skip", False),
                              lambda n: names.flag(n, f"{where} route"),
                              lambda r: _sprite(r, f"{where} route"))
   except rc.RouteError as e:
      raise evc.EventError(f"{where} route: {e}") from None


def parse_objects(name: str, m: JsonDict) -> list[JsonDict]:
   """!Reads a map's objects (names not resolved yet).
   @param name  map name
   @param m     the map manifest
   @return list of object dicts (raw fields plus x, y, where)
   """
   where = f"map {name!r}"
   items = m.get("objects", [])
   if not isinstance(items, list):
      raise evc.EventError(f"{where}: objects must be a list")
   viewed = m.get("object_view") is not None   # I3: objects spawn near the player, up to 64
   stacked = m.get("stacked_objects", False)
   if not isinstance(stacked, bool):
      raise evc.EventError(f"{where}: stacked_objects must be true or false")
   most = MAX_VIEW_OBJECTS if viewed else MAX_OBJECTS
   if len(items) > most:
      raise evc.EventError(f"{where}: {len(items)} objects; at most {most}"
                           + ("" if viewed else " (sprite slots 1-15; an object_view allows 64)"))
   out, seen = [], set()
   for i, o in enumerate(items):
      w = f"{where} object {i}"
      _keys(o, "object", w)
      n = o.get("name")
      if not isinstance(n, str) or not n.isidentifier() or not n.isascii():
         raise evc.EventError(f"{w}: needs a name that is an identifier")
      if n in seen:
         raise evc.EventError(f"{w}: another object is named {n!r}")
      seen.add(n)
      w = f"{where} object {n!r}"
      x, y = evc._at(o, w)
      out.append(dict(o, x=x, y=y, where=w, stacked=stacked))
   return out


class _Routes:
   """!A map's route table, identical routes stored once."""

   def __init__(self) -> None:
      """!Empty."""
      ## Route bytes in index order.
      self.codes = []
      ## Bytes -> index.
      self.index = {}

   def add(self, code: bytes) -> int:
      """!Adds a route.
      @param code  route bytes
      @return its index
      """
      if code not in self.index:
         if len(self.codes) >= MAX_ROUTES:
            raise evc.EventError(f"more than {MAX_ROUTES} move routes in one map")
         self.index[code] = len(self.codes)
         self.codes.append(code)
      return self.index[code]


def _movement_args(movement: int, raw: Any, classes: list[str], where: str) -> tuple[int, int]:
   """!I3: the settings of a movement ("movement_args"), as movement_arg and movement_flags.
   @param movement  its index in MOVEMENTS
   @param raw       the "movement_args" object, or None
   @param classes   the world's vehicle class names (a mirror's keep_to)
   @param where     location for messages
   @return (movement_arg, movement_flags)
   """
   name = MOVEMENTS[movement]
   allowed = MOVEMENT_ARGS.get(name)
   args = {} if raw is None else raw
   if not isinstance(args, dict):
      raise evc.EventError(f"{where}: movement_args must be an object")
   if args and allowed is None:
      raise evc.EventError(f"{where}: movement {name!r} takes no movement_args")
   unknown = sorted(set(args) - (allowed or set()))
   if unknown:
      raise evc.EventError(f"{where} movement_args: {name} takes {', '.join(sorted(allowed or ())) or 'nothing'}, "
                           f"not {', '.join(unknown)}")
   for k in ("watch_runner", "clockwise", "keep_facing"):
      if not isinstance(args.get(k, False), bool):
         raise evc.EventError(f"{where} movement_args: {k} must be true or false")
   arg, flags = 0, 0
   if name in ("turn", "roam"):
      choices = args.get("choices", list(evc.DIRECTIONS))
      if not isinstance(choices, list) or len(choices) not in (TWO_CHOICES, LEGS) or \
            any(c not in evc.DIRECTIONS for c in choices):
         raise evc.EventError(f"{where} movement_args: choices lists 2 or 4 directions ({', '.join(evc.DIRECTIONS)})")
      arg = sum(evc.DIRECTIONS.index(c) << (LEG_SHIFT * k) for k, c in enumerate(choices))
      delays = args.get("delays", "medium")
      if delays not in DELAYS:
         raise evc.EventError(f"{where} movement_args: delays must be one of {', '.join(DELAYS)}")
      flags = (MOVE_TWO_CHOICES if len(choices) == TWO_CHOICES else 0) | (DELAYS.index(delays) << MOVE_DELAYS_SHIFT)
   elif name == "circuit":
      legs = args.get("legs")
      if not isinstance(legs, list) or len(legs) != LEGS or any(c not in evc.DIRECTIONS for c in legs):
         raise evc.EventError(f"{where} movement_args: a circuit needs legs: four directions")
      arg = sum(evc.DIRECTIONS.index(c) << (LEG_SHIFT * k) for k, c in enumerate(legs))
   elif name == "mirror":
      facing = args.get("facing", "up")
      if facing not in evc.DIRECTIONS:
         raise evc.EventError(f"{where} movement_args: facing must be one of {', '.join(evc.DIRECTIONS)}")
      arg = evc.DIRECTIONS.index(facing)
      if "keep_to" in args:
         if args["keep_to"] not in classes or classes.index(args["keep_to"]) > MAX_MIRROR_CLASS:
            raise evc.EventError(f"{where} movement_args: keep_to must name one of the world's first "
                                 f"{MAX_MIRROR_CLASS + 1} vehicle classes ({', '.join(classes)})")
         arg |= (classes.index(args["keep_to"]) + 1) << MIRROR_CLASS_SHIFT
   elif name == "in_place":
      pace = args.get("pace", "walk")
      if pace not in PACES:
         raise evc.EventError(f"{where} movement_args: pace must be one of {', '.join(PACES)}")
      arg = PACES.index(pace)
   if args.get("watch_runner"):
      flags |= MOVE_WATCH_RUNNER
   if args.get("clockwise"):
      flags |= ROTATE_CLOCKWISE
   if args.get("keep_facing"):
      flags |= STATIC_KEEPS_FACING
   return arg, flags


def _movement_fields(src: JsonDict, names: evc.Names, routes: _Routes, where: str,
                     base: MovementFields | None = None, classes: list[str] | None = None) -> MovementFields:
   """!Movement, range, route and settings of an object or page.
   @param src      the object or page dict
   @param names    Names
   @param routes   _Routes
   @param where    location for messages
   @param base     what a page keeps where it gives none (its object's), or None
   @param classes  the world's vehicle class names
   @return (movement, range, route index, movement_arg, movement_flags)
   """
   base_movement, base_range, base_route, base_arg, base_flags = base if base is not None else (0, None, NO_ROUTE, 0,
                                                                                                0)
   movement = _movement(src["movement"], f"{where} movement") if "movement" in src else base_movement
   rng = _range(src["range"], where) if "range" in src else (
      base_range if base_range is not None else _range([1, 1], where))
   route = NO_ROUTE
   if "route" in src:
      if movement != MOVEMENTS.index("custom"):
         raise evc.EventError(f"{where}: a route needs movement 'custom'")
      route = routes.add(_route(src["route"], names, where))
   elif movement == MOVEMENTS.index("custom"):
      if base_route == NO_ROUTE:
         raise evc.EventError(f"{where}: movement 'custom' needs a route")
      route = base_route   # the object's own route
   if "movement" in src or "movement_args" in src:
      arg, flags = _movement_args(movement, src.get("movement_args"), list(classes or []), where)
   else:
      arg, flags = base_arg, base_flags
   return movement, rng, route, arg, flags


def _motion(src: JsonDict, base: tuple[int, int], where: str) -> int:
   """!Speed and frequency.
   @param src   the object or page dict
   @param base  (speed, frequency) when not given
   @param where  location for messages
   @return speed | frequency << 4
   """
   base_speed, base_frequency = base
   speed = evc._int(src.get("speed", base_speed), 1, rc.MAX_SPEED, f"{where} speed")
   freq = evc._int(src.get("frequency", base_frequency), 1, rc.MAX_FREQUENCY, f"{where} frequency")
   return speed | (freq << FREQUENCY_SHIFT)


def compile_objects(map_names: Sequence[str], parsed: Mapping[str, list[JsonDict]], names: evc.Names,
                    painted: Callable[[str, int, int], bool],
                    chunks: Callable[[str], Sequence[tuple[int, int]]], kinds: Sequence[str] = (),
                    classes: Sequence[str] = ()) -> dict[str, Any]:
   """!Resolves every map's objects.
   @param map_names  the world's map names, in world order
   @param parsed     {map name: parse_objects() result}
   @param names      event_compiler.Names
   @param painted    callable(map name, x, y) -> True on a painted cell
   @param chunks     callable(map name) -> chunk positions [(cx, cy), ...]
   @param kinds      the world's object_kinds (F2; an object's "kind" names one)
   @param classes    the world's vehicle class names (I3: a mirror's keep_to)
   @return {map name: dict(objects: [record dict], pages: [record dict], routes: [bytes], names: [object names])};
           the "_self_bits" entry holds the world's self switch bit count
   """
   out = {}
   self_bits = 0
   for name in map_names:
      objs, pages, cells, parallel = [], [], {}, 0
      routes = _Routes()
      inside = set(chunks(name))
      for o in parsed.get(name, []):
         w = o["where"]
         x, y = o["x"], o["y"]
         # painted() allows any cell of a map with unpainted_events (I3), its objects outside its chunks too
         if (x >> evc.CHUNK_SHIFT, y >> evc.CHUNK_SHIFT) not in inside and not painted(name, x, y):
            raise evc.EventError(f"{w}: cell {x},{y} is outside the map's chunks")
         if not painted(name, x, y):
            raise evc.EventError(f"{w}: cell {x},{y} is not painted")
         elev = evc._int(o.get("elevation", 0), 0, evc.MAX_ELEVATION, f"{w} elevation")

         # levels meet when they are equal or either is 0 (any) or multi-level
         for (cx, cy, ce), other in cells.items():
            if o["stacked"]:
               break   # I3: the map lets objects share a cell
            if (cx, cy) == (x, y) and (ce in (0, ELEVATION_MULTI) or elev in (0, ELEVATION_MULTI) or ce == elev):
               raise evc.EventError(f"{w}: object {other!r} stands on {x},{y} on a level that meets this one")
         cells[(x, y, elev)] = o["name"]
         key = _sprite(o.get("sprite"), w)
         facing = _facing(o.get("facing", "down"), f"{w} facing")
         movement, rng, route, m_arg, m_flags = _movement_fields(o, names, routes, w, classes=list(classes))
         motion = _motion(o, (DEFAULT_SPEED, DEFAULT_FREQUENCY), w)
         options = _options(o.get("options", {}), rc.DEFAULT_OPTIONS, w)
         trigger = _trigger(o.get("trigger", "action"), f"{w} trigger")
         script = names.script(o["script"], f"{w} script") if "script" in o else NONE
         if trigger in (TRIGGERS.index("autorun"), TRIGGERS.index("parallel")) and script == NONE:
            raise evc.EventError(f"{w}: {TRIGGERS[trigger]} objects need a script")
         flag = names.flag(o["flag"], f"{w} flag") if "flag" in o else NONE
         kind = NO_KIND
         if "kind" in o:
            if o["kind"] not in kinds:
               raise evc.EventError(f"{w}: kind must be one of the world's object_kinds "
                                    f"({', '.join(kinds) or 'none declared'}), got {o['kind']!r}")
            kind = list(kinds).index(o["kind"]) + 1
         sight, until = 0, NONE
         if "spotter" in o:
            t = o["spotter"]
            _keys(t, "spotter", f"{w} spotter")
            sight = evc._int(t.get("sight"), 1, MAX_SIGHT, f"{w} spotter sight")
            if "until" not in t:
               raise evc.EventError(f"{w}: a spotter needs its until flag (its sight stays on until the "
                                    "flag is set, so without one it would spot the player again and again)")
            until = names.flag(t["until"], f"{w} spotter until")
            if script == NONE:
               raise evc.EventError(f"{w}: a spotter needs a script (it runs when the spotter sees the player)")
         uses_self = o.get("self_switches", False)
         if not isinstance(uses_self, bool):
            raise evc.EventError(f"{w}: self_switches must be true or false")
         if "script" in o and o["script"] in names.self_scripts:
            uses_self = True
         raw_pages = o.get("pages", [])
         if not isinstance(raw_pages, list) or len(raw_pages) > MAX_OBJECT_PAGES:
            raise evc.EventError(f"{w}: pages must be a list of at most {MAX_OBJECT_PAGES}")
         first = len(pages)
         for j, pg in enumerate(raw_pages):
            pw = f"{w} page {j + 1}"
            _keys(pg, "page", pw)
            rec, page_self = _page(pg, names, routes, pw, (movement, rng, route, m_arg, m_flags), list(classes))
            uses_self = uses_self or page_self
            pages.append(rec)
            p_trigger = rec["trigger"] if rec["fields"] & PAGE_TRIGGER else trigger
            p_script = rec["script"] if rec["fields"] & PAGE_SCRIPT else script
            if p_trigger in (TRIGGERS.index("autorun"), TRIGGERS.index("parallel")) and p_script == NONE \
                    and not rec["fields"] & PAGE_HIDDEN:
               raise evc.EventError(f"{pw}: {TRIGGERS[p_trigger]} objects need a script")
         if trigger == TRIGGERS.index("parallel") or any(
               pg["fields"] & PAGE_TRIGGER and pg["trigger"] == TRIGGERS.index("parallel") for pg in pages[first:]):
            parallel += 1
            if parallel > MAX_PARALLEL:
               raise evc.EventError(f"{w}: map {name!r} has more than {MAX_PARALLEL} objects that can be "
                                    "parallel (overworld::kMaxParallel)")
         if len(pages) > MAX_PAGES:
            raise evc.EventError(f"map {name!r}: {len(pages)} object pages; at most {MAX_PAGES}")
         base_self = NO_SELF
         if uses_self:
            # the object's four switches are the next bits of the world's self switch bit set
            base_self = self_bits
            self_bits += SELF_SWITCHES
            if self_bits > MAX_SELF_BITS:
               raise evc.EventError("too many objects with self switches in the world")
         objs.append({"sprite": key, "x": x, "y": y, "script": script, "flag": flag, "until": until, "kind": kind,
                      "page_first": first, "route": route, "self_base": base_self, "elevation": elev,
                      "facing": facing, "movement": movement, "range": rng, "sight": sight,
                      "page_count": len(pages) - first, "trigger": trigger, "motion": motion,
                      "options": options, "sprite_ref": o["sprite"], "name": o["name"], "movement_arg": m_arg,
                      "movement_flags": m_flags})
      out[name] = {"objects": objs, "pages": pages, "routes": routes.codes, "names": [ob["name"] for ob in objs]}
   out["_self_bits"] = self_bits
   return out


def _page(pg: JsonDict, names: evc.Names, routes: _Routes, where: str,
          base: MovementFields, classes: list[str] | None = None) -> tuple[JsonDict, bool]:
   """!Compiles one event page.
   @param pg             the page dict
   @param names          Names
   @param routes         _Routes
   @param where          location for messages
   @param base           the object's movement fields: what a page's movement fields default to
   @param classes        the world's vehicle class names
   @return (record dict, whether it tests self switches)
   """
   when = pg.get("when")
   if when is None:
      raise evc.EventError(f"{where}: needs conditions (\"when\"); the object's own fields are the page without any")
   _keys(when, "when", f"{where} when")
   flag = names.flag(when["flag"], f"{where} when flag") if "flag" in when else NONE
   var, value, cond = NONE, 0, 0
   if "var" in when:
      vid, lo, hi = names.var(when["var"], f"{where} when var")
      value = evc._int(when.get("value"), evc.MIN_VALUE, evc.MAX_VALUE, f"{where} when value")
      equal = when.get("equal", False)
      if not isinstance(equal, bool):
         raise evc.EventError(f"{where} when: equal must be true or false")
      if (equal and not lo <= value <= hi) or (not equal and value > hi):
         raise evc.EventError(f"{where} when: {when['var']} ({lo}..{hi}) can never "
                              f"{'equal' if equal else 'reach'} {value}")
      var, cond = vid, (PAGE_VAR_EQUAL if equal else 0)
   elif "value" in when or "equal" in when:
      raise evc.EventError(f"{where} when: value and equal need var")
   letters = when.get("self", [])
   if isinstance(letters, str):
      letters = [letters]
   if not isinstance(letters, list) or any(s not in ("A", "B", "C", "D") for s in letters):
      raise evc.EventError(f"{where} when: self takes A, B, C or D (or a list of them)")
   # self switch A is bit 0, B bit 1, ...
   self_mask = 0
   for s in letters:
      self_mask |= 1 << "ABCD".index(s)
   if flag == NONE and var == NONE and not self_mask:
      raise evc.EventError(f"{where} when: give a flag, a var or self switches")
   rec = {"sprite": 0, "value": value, "flag": flag, "var": var, "script": NONE, "route": NO_ROUTE, "fields": 0,
          "self_mask": self_mask, "cond": cond, "movement": 0, "range": DEFAULT_RANGE, "facing": 0, "trigger": 0,
          "motion": DEFAULT_MOTION, "options": rc.DEFAULT_OPTIONS, "movement_arg": 0, "movement_flags": 0}
   hidden = pg.get("hidden", False)
   if not isinstance(hidden, bool):
      raise evc.EventError(f"{where}: hidden must be true or false")
   if hidden:
      extra = sorted(set(pg) - {"when", "hidden"})
      if extra:
         raise evc.EventError(f"{where}: a hidden page sets nothing else ({', '.join(extra)})")
      rec["fields"] = PAGE_HIDDEN
      return rec, bool(self_mask)
   if "sprite" in pg:
      rec["sprite"] = _sprite(pg["sprite"], where)
      rec["fields"] |= PAGE_SPRITE
   if "script" in pg:
      rec["script"] = names.script(pg["script"], f"{where} script")
      rec["fields"] |= PAGE_SCRIPT
   if any(k in pg for k in ("movement", "range", "route", "movement_args")):
      if "route" in pg and "movement" not in pg:
         pg = dict(pg, movement="custom")
      rec["movement"], rec["range"], rec["route"], rec["movement_arg"], rec["movement_flags"] = \
         _movement_fields(pg, names, routes, where, base, classes)
      rec["fields"] |= PAGE_MOVEMENT
   if "facing" in pg:
      rec["facing"] = _facing(pg["facing"], f"{where} facing")
      rec["fields"] |= PAGE_FACING
   if "trigger" in pg:
      rec["trigger"] = _trigger(pg["trigger"], f"{where} trigger")
      rec["fields"] |= PAGE_TRIGGER
   if "speed" in pg or "frequency" in pg:
      rec["motion"] = _motion(pg, (DEFAULT_SPEED, DEFAULT_FREQUENCY), where)
      rec["fields"] |= PAGE_MOTION
   if "options" in pg:
      rec["options"] = _options(pg["options"], rc.DEFAULT_OPTIONS, where)
      rec["fields"] |= PAGE_OPTIONS
   if not rec["fields"]:
      raise evc.EventError(f"{where}: changes nothing (give a field to replace, or hidden)")
   uses_self = bool(self_mask) or ("script" in pg and pg["script"] in names.self_scripts)
   return rec, uses_self


def _enum(name: str) -> str:
   """!A snake_case name as its C++ enumerator.
   @param name  e.g. look_around
   @return e.g. LookAround
   """
   return "".join(p.capitalize() for p in name.split("_"))


def emit_map(i: int, objs: JsonDict) -> tuple[list[str], str, str, str, str]:
   """!C++ tables of one map's objects.
   @param i     map index
   @param objs  compile_objects() entry
   @return (lines, objects symbol, pages symbol, route code symbol, route offsets symbol), 'nullptr' for empty
   """
   L = []
   osym = psym = csym = rsym = "nullptr"
   if objs["objects"]:
      osym = f"kObjects{i}"
      L.append(f"const ObjectTemplate {osym}[] = {{")
      for o in objs["objects"]:
         L.append(f"    {{AssetKey{{0x{o['sprite']:08X}u}}, {o['x']}, {o['y']}, 0x{o['script']:04X}, "
                  f"0x{o['flag']:04X}, 0x{o['until']:04X}, {o['page_first']}, 0x{o['route']:04X}, "
                  f"0x{o['self_base']:04X}, {o['elevation']}, {o['facing']}, "
                  f"Movement::{_enum(MOVEMENTS[o['movement']])}, 0x{o['range']:02X}, {o['sight']}, "
                  f"{o['page_count']}, Trigger::{_enum(TRIGGERS[o['trigger']])}, 0x{o['motion']:02X}, "
                  f"0x{o['options']:02X}, {o['kind']}, 0x{o.get('movement_arg', 0):02X}, "
                  f"0x{o.get('movement_flags', 0):02X}}},   // {o['name']} ({o['sprite_ref']})")
      L.append("};")
   if objs["pages"]:
      psym = f"kObjectPages{i}"
      L.append(f"const ObjectPage {psym}[] = {{")
      for p in objs["pages"]:
         L.append(f"    {{AssetKey{{0x{p['sprite']:08X}u}}, {p['value']}, 0x{p['flag']:04X}, 0x{p['var']:04X}, "
                  f"0x{p['script']:04X}, 0x{p['route']:04X}, 0x{p['fields']:04X}, 0x{p['self_mask']:X}, "
                  f"{p['cond']}, Movement::{_enum(MOVEMENTS[p['movement']])}, 0x{p['range']:02X}, {p['facing']}, "
                  f"Trigger::{_enum(TRIGGERS[p['trigger']])}, 0x{p['motion']:02X}, 0x{p['options']:02X}, "
                  f"0x{p.get('movement_arg', 0):02X}, 0x{p.get('movement_flags', 0):02X}}},")
      L.append("};")
   if objs["routes"]:
      csym, rsym = f"kRouteCode{i}", f"kRouteOffsets{i}"
      blob, offs = bytearray(), []
      for code in objs["routes"]:
         offs.append(len(blob))
         blob += code
      L.append(f"const uint8_t {csym}[] = {{")
      for k in range(0, len(blob), BYTES_PER_LINE):
         L.append("    " + ", ".join(f"0x{b:02X}" for b in blob[k:k + BYTES_PER_LINE]) + ",")
      L.append("};")
      L.append(f"const uint16_t {rsym}[] = {{{', '.join(str(o) for o in offs)}}};")
   return L, osym, psym, csym, rsym
