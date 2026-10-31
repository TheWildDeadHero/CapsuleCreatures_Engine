#!/usr/bin/env python3
"""!Map events (B-3): warps, triggers and signs of each map, the world's step
hooks and area roots. Called by map_compiler.py; not run on its own.

Map (`capsule.map/2`), optional keys:

    "area": "meadow",          // B-2: the outdoor map whose area this map is in
                               // (default: itself); leaving an area clears the
                               // area flags and variables
    "events": {
      "warps":    [{"at": [x, y], "elevation": 0, "to": "cave", "warp": 0,
                    "trigger": "step"}],            // or "press" + "direction"
      "triggers": [{"at": [x, y], "elevation": 0, "script": "rival_stops_you",
                    "var": "rival_state", "value": 0}],
      "signs":    [{"at": [x, y], "elevation": 0, "script": "town_sign",
                    "facing": "up", "flag": "found_potion"}]
    }

- **Warps** move the walker to warp number "warp" of map "to", landing on
  that warp's cell, with an optional "transition" (none, fade, white,
  mosaic, wipe_h, wipe_v, box; default: the world's, D-17). "step" warps fire when the walker arrives on the cell;
  "press" warps when it stands on the cell and presses "direction" (doors in
  a wall, arrows at a map's edge). I3: "arrow" warps are press warps that need the walker to face the direction
  already (a press while facing elsewhere turns it first); "door" warps fire when the walker stands on the
  neighbouring cell opposite "direction", faces it and presses it (it does not step: the game opens the door,
  capsule::map::kWarpFromAdjacent); "none" warps never fire (a landing other warps name, kept so warp numbers stay
  those of the source data). "to": "dynamic" leaves the destination to the game when the warp fires
  (capsule::map::kDynamicWarp; "warp" is passed on). DS1: "door": "door:NAME" binds a door animation (asset kind
  door) to the warp's cell (the door's anchor, its bottom-left cell): the warp sequence opens it, the walker walks in
  and it closes; arriving on it, it shows open and closes after the walker walks out (map_compiler.py also binds
  doors by metatile, the world's "doors").
- **Triggers** run a script when the walker arrives on the cell; with "var",
  only while that variable holds "value" (the script usually changes it, as
  the reference games' coordinate events do). C10: "run": "immediate" (default
  "step") runs the script to its end at once (script::run_now(): no lock, the
  walker keeps moving, the cell's next events are still checked; the script
  must not wait), as the reference games' run-immediately coordinate events.
- **Signs** run a script when the player presses A facing the cell; "facing"
  (default "any") is the direction the player must face. With "flag" the
  sign is gone once that flag is set (hidden items).
- "elevation" 0 matches any level; otherwise the walker's level must equal it.
- Scripts, flags and variables are names from the world's scripts manifest
  (world "scripts"), compiled with tools/script_compiler.py.

Records are bucketed per chunk (C-03c): a chunk's directory entry names its
first event and count, and an event holds its chunk-local position (x | y <<
4), so a step checks only the events of the walker's chunk.

World, optional keys:

    "scripts": "../scripts/scripts.json",
    "step_hooks": [{"name": "drain", "period": 4}, {"name": "timer", "period": 256, "phase": 7}]

Step hooks are the step dispatcher's subscribers (B-3): every period-th step
(a power of two, 1-32768) field::game::step_hook(id) runs. Phases not given
are spread so hooks of the same period fall on different steps. --world-ids
writes their IDs (STEP_HOOK_...) and the map indices (MAP_...).
"""
from __future__ import annotations

from collections.abc import Callable, Iterable, Mapping, Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

import text_encoder

## Warp transitions in capsule::script::Transition order (D-17); absent = the default.
TRANSITIONS = ["none", "fade", "white", "mosaic", "wipe_h", "wipe_v", "box"]
## Transition byte of "the default" (capsule::script::kTransitionDefault).
TRANSITION_DEFAULT = 0xFF
## Event kinds, in capsule::map::EventKind order.
KINDS = ["warp", "trigger", "sign"]
## EventKind::Warp.
KIND_WARP = KINDS.index("warp")
## EventKind::Trigger.
KIND_TRIGGER = KINDS.index("trigger")
## EventKind::Sign.
KIND_SIGN = KINDS.index("sign")
## Directions, in capsule::field::Direction order ("any" is NUM_DIRECTIONS).
DIRECTIONS = ["down", "up", "left", "right"]
## No direction / any facing (capsule::field::kNoDirection).
ANY = 4
## No condition (capsule::map::kNoEventCondition).
NO_CONDITION = 0xFFFF
## Warp triggers (I3: arrow, door and none).
WARP_TRIGGERS = ("step", "press", "arrow", "door", "none")
## Event::direction bit: the walker must face the direction (capsule::map::kWarpNeedsFacing).
WARP_NEEDS_FACING = 0x10
## Event::direction bit: a door, fired from the neighbouring cell (capsule::map::kWarpFromAdjacent).
WARP_FROM_ADJACENT = 0x20
## C10: Event::direction bit of a trigger that runs immediately (capsule::map::kTriggerImmediate).
TRIGGER_IMMEDIATE = 0x40
## C10: a trigger's "run" values: a field request (the walker stops, the field locks) or at once.
TRIGGER_RUNS = ("step", "immediate")
## Warp::map of a warp whose destination the game decides (capsule::map::kDynamicWarp).
DYNAMIC_WARP = 0xFFFF
## The "to" of such a warp.
DYNAMIC_TO = "dynamic"
## Warps per map (Map::warp_count is 8-bit).
MAX_WARPS = 255
## Events per chunk (ChunkDirectoryEntry::event_count is 8-bit).
MAX_CHUNK_EVENTS = 255
## Events per map (16-bit indices).
MAX_EVENTS = 0xFFFF
## Step hooks per world.
MAX_STEP_HOOKS = 32
## Numbers in a cell position ([x, y]).
CELL_COORDS = 2
## Lowest cell coordinate.
MIN_CELL = -16384
## Highest cell coordinate.
MAX_CELL = 16383
## Highest elevation (0 matches any level).
MAX_ELEVATION = 15
## Lowest trigger value (an int32).
MIN_VALUE = -2**31
## Highest trigger value (an int32).
MAX_VALUE = 2**31 - 1
## Longest step hook period, in steps.
MAX_HOOK_PERIOD = 32768
## Cells per side of a chunk, as a shift (8 cells).
CHUNK_SHIFT = 3
## Cells per side of a chunk.
CHUNK_CELLS = 1 << CHUNK_SHIFT
## Mask of a cell's position within its chunk.
CHUNK_MASK = CHUNK_CELLS - 1
## Bit position of y in an event's chunk-local position (x | y << 4).
LOCAL_Y_SHIFT = 4
## Bucketed record field: chunk index.
BUCKET_CHUNK = 0
## Bucketed record field: row-major order within the chunk.
BUCKET_ORDER = 1
## Bucketed record field: chunk-local position, the first of the emitted fields.
BUCKET_LOCAL = 2
## Bucketed record field: event kind.
BUCKET_KIND = 3

## A parsed JSON object.
JsonDict = dict[str, Any]
## A step hook: name, period, phase.
StepHook = tuple[str, int, int]

## Keys per object.
KEYS = {
   "events": {"warps", "triggers", "signs"},
   "warp": {"at", "elevation", "to", "warp", "trigger", "direction", "transition", "door"},
   "trigger": {"at", "elevation", "script", "var", "value", "run"},
   "sign": {"at", "elevation", "script", "facing", "flag"},
   "hook": {"name", "period", "phase"},
}


class EventError(Exception):
   """!An event rule was broken; the message says where."""
   pass


def _keys(obj: Any, kind: str, where: str) -> None:
   """!Rejects unknown keys.
   @param obj    the object
   @param kind   key set in KEYS
   @param where  location for messages
   @return None
   """
   if not isinstance(obj, dict):
      raise EventError(f"{where}: expected an object")
   unknown = sorted(set(obj) - KEYS[kind])
   if unknown:
      raise EventError(f"{where}: unknown key(s) {', '.join(unknown)}")


def _int(value: Any, lo: int, hi: int, where: str) -> int:
   """!Checks an integer.
   @param value  the value
   @param lo     minimum
   @param hi     maximum
   @param where  location for messages
   @return the value
   """
   if not isinstance(value, int) or isinstance(value, bool) or not lo <= value <= hi:
      raise EventError(f"{where}: expected an integer {lo}-{hi}, got {value!r}")
   return value


def _at(e: JsonDict, where: str) -> tuple[int, int]:
   """!An event's cell.
   @param e      event object
   @param where  location for messages
   @return (x, y)
   """
   at = e.get("at")
   if not isinstance(at, list) or len(at) != CELL_COORDS:
      raise EventError(f"{where}: at must be [x, y]")
   x, y = at
   return _int(x, MIN_CELL, MAX_CELL, f"{where} at"), _int(y, MIN_CELL, MAX_CELL, f"{where} at")


class Names:
   """!Script, flag and variable names of the world's scripts manifest."""

   def __init__(self, scripts: dict[str, int] | None = None, flags: dict[str, int] | None = None,
                variables: dict[str, tuple[int, int, int]] | None = None, source: str | None = None,
                self_scripts: Iterable[str] | None = None, waiting: Iterable[str] | None = None) -> None:
      """!Wraps the name tables.
      @param scripts    {name: script id}, or None when the world names no scripts manifest
      @param flags      {name: flag id}
      @param variables  {name: (var id, lo, hi)}
      @param source     the manifest's path, for messages
      @param self_scripts  names of the scripts that use self switches (D-17)
      @param waiting    C10: names of the scripts that may wait (directly or through calls)
      """
      ## C10: scripts that may wait (an immediate trigger's may not).
      self.waiting = set(waiting or ())
      ## Scripts that use self switches (directly or through calls).
      self.self_scripts = set(self_scripts or ())
      ## Script name -> ID, or None (no scripts manifest).
      self.scripts = scripts
      ## Flag name -> ID.
      self.flags = flags or {}
      ## Variable name -> (ID, lowest, highest).
      self.variables = variables or {}
      ## Scripts manifest, for messages.
      self.source = source

   def script(self, name: Any, where: str) -> int:
      """!A script's ID.
      @param name   script name
      @param where  location for messages
      @return ID
      """
      if self.scripts is None:
         raise EventError(f"{where}: events run scripts; name the scripts manifest in the world (\"scripts\")")
      if not isinstance(name, str) or name not in self.scripts:
         raise EventError(f"{where}: unknown script {name!r} (in {self.source})")
      return self.scripts[name]

   def flag(self, name: Any, where: str) -> int:
      """!A flag's ID.
      @param name   flag name
      @param where  location for messages
      @return ID
      """
      if self.scripts is None:
         raise EventError(f"{where}: flags come from the scripts manifest; name it in the world (\"scripts\")")
      if not isinstance(name, str) or name not in self.flags:
         raise EventError(f"{where}: unknown flag {name!r} (in {self.source})")
      return self.flags[name]

   def var(self, name: Any, where: str) -> tuple[int, int, int]:
      """!A variable's ID and range.
      @param name   variable name
      @param where  location for messages
      @return (ID, lowest, highest)
      """
      if self.scripts is None:
         raise EventError(f"{where}: variables come from the scripts manifest; name it in the world (\"scripts\")")
      if not isinstance(name, str) or name not in self.variables:
         raise EventError(f"{where}: unknown variable {name!r} (in {self.source})")
      return self.variables[name]


def load_names(path: str | Path | None, deps: list[Path], sc: ModuleType,
               codepage: text_encoder.CodePage = text_encoder.DEFAULT) -> Names:
   """!Compiles the world's scripts manifest for its names.
   @param path      scripts manifest, or None
   @param deps      list the manifest and its sources are appended to
   @param sc        the script_compiler module
   @param codepage  the game's code page (the scripts' inline texts)
   @return Names
   """
   if path is None:
      return Names()
   path = Path(path).resolve()
   deps.append(path)
   try:
      comp = sc.compile_file(path, codepage=codepage)
   except (OSError, ValueError) as e:
      raise EventError(f"world scripts: cannot read {path}: {e}") from None
   deps.extend(Path(p) for p in comp.deps)
   errs = comp.errors()
   if errs:
      raise EventError(f"world scripts: {path.name} does not compile ({errs[0]}); fix the scripts first")
   variables = {n: (comp.var_ids[n], comp.vars[n]["lo"], comp.vars[n]["hi"]) for n in comp.order["var"]}
   return Names(dict(comp.script_ids), dict(comp.flag_ids), variables, path.name,
                comp.result.get("uses_self", set()),
                {n for n, w in comp.result.get("waits", {}).items() if w})


def parse_map(name: str, m: JsonDict, map_names: Iterable[str]) -> JsonDict:
   """!Reads a map's events (names not resolved yet).
   @param name       map name
   @param m          the map manifest (stripped)
   @param map_names  the world's map names
   @return dict: area (map name), warps, triggers, signs (lists of dicts with x, y and the raw fields)
   """
   where = f"map {name!r}"
   area = m.get("area", name)
   if area not in map_names:
      raise EventError(f"{where}: area {area!r} is not a map of the world")
   ev = m.get("events", {})
   _keys(ev, "events", f"{where} events")
   out = {"area": area, "warps": [], "triggers": [], "signs": []}
   for kind, key in (("warp", "warps"), ("trigger", "triggers"), ("sign", "signs")):
      items = ev.get(key, [])
      if not isinstance(items, list):
         raise EventError(f"{where} events: {key} must be a list")
      for i, e in enumerate(items):
         w = f"{where} {kind} {i}"
         _keys(e, kind, w)
         x, y = _at(e, w)
         d = dict(e, x=x, y=y, where=w, elevation=_int(e.get("elevation", 0), 0, MAX_ELEVATION, f"{w} elevation"))
         out[key].append(d)
   if len(out["warps"]) > MAX_WARPS:
      raise EventError(f"{where}: {len(out['warps'])} warps; at most {MAX_WARPS}")
   return out


def compile_events(map_names: Sequence[str], parsed: Mapping[str, JsonDict], names: Names,
                   painted: Callable[[str, int, int], bool],
                   chunks: Callable[[str], Sequence[tuple[int, int]]]) -> dict[str, JsonDict]:
   """!Resolves every map's events and buckets them per chunk.
   @param map_names  the world's map names, in world order (map indices)
   @param parsed     {map name: parse_map() result} (maps missing have no events)
   @param names      Names
   @param painted    callable(map name, x, y) -> True on a painted cell
   @param chunks     callable(map name) -> chunk positions [(cx, cy), ...] in chunk directory order
   @return {map name: dict(area: map index, events: [(local, kind, elevation, direction, target, cond,
           value)] in bucket order, warps: [(map, x, y)], ranges: {chunk index: (first, count)})}
   """
   index = {n: i for i, n in enumerate(map_names)}
   out = {}
   for name in map_names:
      p = parsed.get(name, {"area": name, "warps": [], "triggers": [], "signs": []})
      area = p["area"]
      # An area root is an outdoor map: one whose own area is itself.
      if parsed.get(area, {"area": area})["area"] != area:
         raise EventError(f"map {name!r}: area {area!r} is itself in area {parsed[area]['area']!r}; "
                          "name the outdoor map at the root")
      records = []   # (cx, cy, local, kind, elevation, direction, target, cond, value, where)
      doors = []   # DS1: (x, y, door key, key text) of the warps that name a door
      warps = []
      transitions = []
      cells = set()
      for wp in p["warps"]:
         w = wp["where"]
         to = wp.get("to")
         n = _int(wp.get("warp"), 0, MAX_WARPS - 1, f"{w} warp")
         if to == DYNAMIC_TO:
            target = (DYNAMIC_WARP, n, 0)   # the game resolves it when it fires
         else:
            if to not in index:
               raise EventError(f"{w}: to must name a map of the world (or 'dynamic'), got {to!r}")
            dest = parsed.get(to, {"warps": []})["warps"]
            if n >= len(dest):
               raise EventError(f"{w}: map {to!r} has {len(dest)} warp(s); there is no warp {n}")
            dx, dy = dest[n]["x"], dest[n]["y"]
            if not painted(to, dx, dy):
               raise EventError(f"{w}: lands on {to} {dx},{dy}, which is not a painted cell")
            target = (index[to], dx, dy)
         trig = wp.get("trigger", "step")
         if trig not in WARP_TRIGGERS:
            raise EventError(f"{w}: trigger must be one of {', '.join(WARP_TRIGGERS)}")
         direction = ANY
         if trig in ("press", "arrow", "door"):
            if wp.get("direction") not in DIRECTIONS:
               raise EventError(f"{w}: a {trig} warp needs a direction ({', '.join(DIRECTIONS)})")
            direction = DIRECTIONS.index(wp["direction"])
            direction |= {"press": 0, "arrow": WARP_NEEDS_FACING, "door": WARP_FROM_ADJACENT}[trig]
         elif "direction" in wp:
            raise EventError(f"{w}: direction is for press, arrow and door warps")
         trans = wp.get("transition")
         if trans is not None and trans not in TRANSITIONS:
            raise EventError(f"{w}: transition must be one of {', '.join(TRANSITIONS)}")
         if "door" in wp:
            door = wp["door"]
            if not isinstance(door, str) or not door.startswith("door:"):
               raise EventError(f"{w}: door names a door asset (\"door:NAME\"), got {door!r}")
            doors.append((wp["x"], wp["y"], key_of(door, w), door))
         if trig != "none":   # a landing only: no event, its number kept
            records.append((wp["x"], wp["y"], KIND_WARP, wp["elevation"], direction, len(warps), NO_CONDITION, 0,
                            w))
         warps.append(target)
         transitions.append(TRANSITION_DEFAULT if trans is None else TRANSITIONS.index(trans))
      for tr in p["triggers"]:
         w = tr["where"]
         script = names.script(tr.get("script"), w)
         cond, value = NO_CONDITION, 0
         if "var" in tr:
            vid, lo, hi = names.var(tr["var"], w)
            value = _int(tr.get("value"), MIN_VALUE, MAX_VALUE, f"{w} value")
            if not lo <= value <= hi:
               raise EventError(f"{w}: {value} is outside {tr['var']}'s range {lo}..{hi}; it would never fire")
            cond = vid
         elif "value" in tr:
            raise EventError(f"{w}: value needs var")
         run = tr.get("run", TRIGGER_RUNS[0])
         if run not in TRIGGER_RUNS:
            raise EventError(f"{w}: run must be one of {', '.join(TRIGGER_RUNS)}")
         if run == TRIGGER_RUNS[1] and tr.get("script") in names.waiting:
            raise EventError(f"{w}: an immediate trigger runs its script to its end at once; "
                             f"{tr.get('script')} may wait")
         mode = ANY | (TRIGGER_IMMEDIATE if run == TRIGGER_RUNS[1] else 0)
         records.append((tr["x"], tr["y"], KIND_TRIGGER, tr["elevation"], mode, script, cond, value, w))
      for sg in p["signs"]:
         w = sg["where"]
         script = names.script(sg.get("script"), w)
         facing = sg.get("facing", "any")
         if facing != "any" and facing not in DIRECTIONS:
            raise EventError(f"{w}: facing must be any or one of {', '.join(DIRECTIONS)}")
         cond = names.flag(sg["flag"], w) if "flag" in sg else NO_CONDITION
         records.append((sg["x"], sg["y"], KIND_SIGN, sg["elevation"], ANY if facing == "any" else
                         DIRECTIONS.index(facing), script, cond, 0, w))
      for r in records:
         x, y, kind, elevation, _direction, _target, _cond, _value, w = r

         # a sign is read from a neighbouring cell, so its own cell may be unpainted
         if kind != KIND_SIGN and not painted(name, x, y):
            raise EventError(f"{w}: cell {x},{y} is not painted (the walker can never stand there)")
         # Two warps on one cell and level would be ambiguous; triggers and signs may
         # share a cell (the first whose condition holds fires, as the reference games scan them).
         key = (x, y, kind, elevation)
         if kind == KIND_WARP and key in cells:
            raise EventError(f"{w}: another warp on {x},{y} at the same elevation")
         cells.add(key)
      # Bucket by chunk (chunk directory order), then position.
      positions = chunks(name)
      chunk_index = {(c[0], c[1]): i for i, c in enumerate(positions)}
      bucketed = []
      for x, y, kind, elevation, direction, target, cond, value, w in records:
         ci = chunk_index.get((x >> CHUNK_SHIFT, y >> CHUNK_SHIFT))
         if ci is None:
            raise EventError(f"{w}: cell {x},{y} is outside the map's chunks")

         # the cell within its chunk: a row-major order for sorting, and x | y << 4 for the record
         lx, ly = x & CHUNK_MASK, y & CHUNK_MASK
         order = ly * CHUNK_CELLS + lx
         local = lx | (ly << LOCAL_Y_SHIFT)
         bucketed.append((ci, order, local, kind, elevation, direction, target, cond, value))
      bucketed.sort(key=lambda b: (b[BUCKET_CHUNK], b[BUCKET_ORDER], b[BUCKET_KIND]))
      if len(bucketed) > MAX_EVENTS:
         raise EventError(f"map {name!r}: {len(bucketed)} events; at most {MAX_EVENTS}")
      ranges = {}
      for i, b in enumerate(bucketed):
         first, count = ranges.get(b[BUCKET_CHUNK], (i, 0))
         ranges[b[BUCKET_CHUNK]] = (first, count + 1)
      for ci, (_first, count) in ranges.items():
         if count > MAX_CHUNK_EVENTS:
            c = positions[ci]
            raise EventError(f"map {name!r}: chunk {c[0]},{c[1]} has {count} events; at most {MAX_CHUNK_EVENTS}")
      out[name] = {"area": index[area], "events": [b[BUCKET_LOCAL:] for b in bucketed], "warps": warps,
                   "transitions": transitions, "ranges": ranges, "doors": doors}
   return out


def key_of(text: str, where: str) -> int:
   """!DS1: an asset key (Asset Key v1) of a name such as "door:NAME".
   @param text   the name
   @param where  location for messages
   @return the key
   """
   from sequence_compiler import ManifestError as SequenceError
   from sequence_compiler import asset_key
   try:
      return asset_key(text, where)
   except SequenceError as e:
      raise EventError(str(e)) from None


def compile_hooks(raw: Any) -> list[StepHook]:
   """!The world's step hooks, phases spread.
   @param raw  the "step_hooks" value, or None
   @return list of (name, period, phase)
   """
   if raw is None:
      return []
   if not isinstance(raw, list) or len(raw) > MAX_STEP_HOOKS:
      raise EventError(f"world step_hooks: a list of up to {MAX_STEP_HOOKS} hooks")
   hooks, seen = [], set()
   used = {}   # period -> phases taken (explicit ones first, so spread phases avoid them)
   parsed = []
   for i, h in enumerate(raw):
      w = f"world step hook {i}"
      _keys(h, "hook", w)
      n = h.get("name")
      if not isinstance(n, str) or not n.isidentifier() or n in seen:
         raise EventError(f"{w}: needs a unique identifier name")
      seen.add(n)
      period = _int(h.get("period"), 1, MAX_HOOK_PERIOD, f"{w} period")

      # a power of two has a single bit set, so clearing its lowest bit leaves 0
      if period & (period - 1):
         raise EventError(f"{w}: period {period} is not a power of two (a counter of its own is game code)")
      phase = _int(h["phase"], 0, period - 1, f"{w} phase") if "phase" in h else None
      if phase is not None:
         used.setdefault(period, set()).add(phase)
      parsed.append((n, period, phase))
   for n, period, phase in parsed:
      if phase is None:
         taken = used.setdefault(period, set())
         phase = next((p for p in range(period) if p not in taken), 0)
         taken.add(phase)
      hooks.append((n, period, phase))
   return hooks


def emit_map(i: int, ev: JsonDict) -> tuple[list[str], str, str]:
   """!C++ tables of one map's events.
   @param i   map index
   @param ev  compile_events() entry
   @return (lines, events symbol or 'nullptr', warps symbol or 'nullptr')
   """
   L = []
   esym = wsym = "nullptr"
   if ev["events"]:
      esym = f"kEvents{i}"
      L.append(f"const Event {esym}[] = {{")
      for local, kind, elev, d, target, cond, value in ev["events"]:
         L.append(f"    {{0x{local:02X}, EventKind::{['Warp', 'Trigger', 'Sign'][kind]}, {elev}, {d}, {target}, "
                  f"0x{cond:04X}, {value}}},")
      L.append("};")
   if ev["warps"]:
      wsym = f"kWarps{i}"
      L.append(f"const Warp {wsym}[] = {{")
      trans = ev.get("transitions") or [TRANSITION_DEFAULT] * len(ev["warps"])
      L.extend(f"    {{{m}, {x}, {y}, 0x{t:02X}, 0}}," for (m, x, y), t in zip(ev["warps"], trans))
      L.append("};")
   return L, esym, wsym


def emit_hooks(hooks: Sequence[StepHook]) -> tuple[list[str], str]:
   """!The step hook table.
   @param hooks  compile_hooks() result
   @return (lines, symbol or 'nullptr')
   """
   if not hooks:
      return [], "nullptr"
   L = ["const StepHook kStepHooks[] = {"]

   # the engine stores a period as the mask period - 1 (step & mask == phase)
   L.extend(f"    {{0x{p - 1:04X}, {ph}}},   // {n}" for n, p, ph in hooks)
   L.append("};")
   return L, "kStepHooks"


def id_name(name: object) -> str:
   """!A name as a C++ identifier suffix: upper case, other characters as _.
   @param name  map or hook name
   @return text
   """
   return "".join(c if c.isalnum() and c.isascii() else "_" for c in str(name)).upper() or "_"


def check_ids(map_names: Sequence[str], hooks: Sequence[StepHook],
              objects: Mapping[str, Sequence[str]] | None = None) -> None:
   """!Checks that world_ids.h can name every map, hook and object (the studio calls this too).
   @param map_names  map names
   @param hooks      compile_hooks() result
   @param objects    {map name: [object names in local ID order]}, or None
   @return None
   """
   emit_ids("world", map_names, hooks, objects)


def emit_ids(src_name: str, map_names: Sequence[str], hooks: Sequence[StepHook],
             objects: Mapping[str, Sequence[str]] | None = None, override_sets: Sequence[str] = ()) -> str:
   """!The world IDs header (maps by index, step hooks, object local IDs; DS1: override sets).
   @param src_name       world file name
   @param map_names      map names in world order
   @param hooks          compile_hooks() result
   @param objects        {map name: [object names in local ID order]}, or None
   @param override_sets  DS1: the world's override set names, by index (map::kWorld.override_sets)
   @return header text
   """
   L = [f"// Generated by tools/map_compiler.py from {src_name}. Do not edit.", "#pragma once", "",
        "#include <cstdint>", "",
        "/// Map indices (map::kWorld.maps), step hook IDs (field::game::step_hook()) and map object local IDs.",
        "namespace world_ids {", ""]
   seen = {}
   for i, n in enumerate(map_names):
      ident = id_name(n)
      if ident in seen:
         raise EventError(f"maps {seen[ident]!r} and {n!r} both become MAP_{ident} in world_ids.h; rename one")
      seen[ident] = n
      L.append(f"inline constexpr uint16_t MAP_{ident} = {i};   ///< {n}")
   L.append("")
   L.extend(f"inline constexpr uint16_t STEP_HOOK_{n.upper()} = {i};   ///< every {p} steps, phase {ph}"
            for i, (n, p, ph) in enumerate(hooks))
   taken = {}
   for n in map_names:
      names = (objects or {}).get(n, [])
      if names:
         L.append("")
      for i, o in enumerate(names):
         ident = f"OBJ_{id_name(n)}_{id_name(o)}"
         if ident in taken:
            raise EventError(f"map {n!r} object {o!r} and {taken[ident]} both become {ident} in world_ids.h; "
                             "rename one")
         taken[ident] = f"map {n!r} object {o!r}"
         L.append(f"inline constexpr uint8_t {ident} = {i};   ///< map {n}, object {o} (local ID)")
   if override_sets:
      L.append("")
   L.extend(f"inline constexpr uint16_t OVERRIDE_{id_name(n)} = {i};   ///< override set {n}"
            for i, n in enumerate(override_sets))
   L += ["", "} // namespace world_ids", ""]
   return "\n".join(L)
