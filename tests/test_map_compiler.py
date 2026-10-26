#!/usr/bin/env python3
"""!Tests for tools/map_compiler.py (run by CTest, or directly)."""
from __future__ import annotations

import copy
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import bitmap  # noqa: E402
import map_compiler as mc  # noqa: E402

## A parsed JSON object (a manifest, or compiled output).
JsonDict = dict[str, Any]

## The test world.
FIXTURE = ROOT / "tests/data/maps"
## The battle data its encounters name (the Starter sample; the world names it by relative path).
BATTLE = ROOT / "samples/starter"
## The world's "battle" key in a scratch copy, which holds that data in battle/ beside maps/.
SCRATCH_BATTLE = "../battle/battle.json"


def copy_world(root: Path) -> Path:
   """!Copies the test world to root/maps and its battle data to root/battle, and points the copy's world at it.
   @param root  an empty temporary folder
   @return the world's folder in the copy
   """
   world = root / "maps"
   shutil.copytree(FIXTURE, world)
   shutil.copytree(BATTLE, root / "battle")
   manifest = world / "world.json"
   data = json.loads(manifest.read_text(encoding="utf-8"))
   data["battle"] = SCRATCH_BATTLE
   manifest.write_text(json.dumps(data, indent=1), encoding="utf-8")
   return world


class Scratch:
   """!A copy of the test world in a temporary folder, to break on purpose.

   @var dir
   The world's folder (Path).
   @var root
   The temporary folder holding the world (maps/) and the battle data its
   encounters name (battle/).
   """

   def __init__(self) -> None:
      """!Copies the fixture (and the battle data its encounters name, beside it)."""
      self.root = Path(tempfile.mkdtemp(prefix="map_compiler_"))
      self.dir = copy_world(self.root)

   def load(self, rel: str) -> Any:
      """!Reads one of its files.
      @param rel  path below the world folder
      @return parsed JSON
      """
      return json.loads((self.dir / rel).read_text(encoding="utf-8"))

   def save(self, rel: str, data: Any) -> None:
      """!Writes one of its files.
      @param rel   path below the world folder
      @param data  JSON value
      """
      (self.dir / rel).write_text(json.dumps(data), encoding="utf-8")

   def compile(self) -> JsonDict:
      """!Compiles the copy.
      @return compile_world() result
      """
      return mc.compile_world(self.dir / "world.json")

   def close(self) -> None:
      """!Removes the folder."""
      shutil.rmtree(self.root, ignore_errors=True)


class MapCompilerTests(unittest.TestCase):
   """!Compiler rules and output."""

   def setUp(self) -> None:
      """!Fresh scratch copy."""
      ## The scratch copy.
      self.s = Scratch()

   def tearDown(self) -> None:
      """!Removes it."""
      self.s.close()

   def fails(self, fragment: str) -> None:
      """!Asserts that compiling the scratch copy fails mentioning @p fragment.
      @param fragment  expected text
      """
      with self.assertRaises(mc.ManifestError) as ctx:
         self.s.compile()
      self.assertIn(fragment, str(ctx.exception))

   def test_fixture(self) -> None:
      """!The fixture compiles: tiles deduplicated with flips, animation tile kept apart."""
      deps: list[Path] = []
      w = mc.compile_world(FIXTURE / "world.json", deps)
      field = w["maps"][0]
      self.assertEqual([m["name"] for m in w["maps"]], ["field", "small", "stack", "town", "plaza", "garden",
                                                        "fixed_a", "fixed_b", "crowd"])
      # 0 blank, 1 animated, then ground, wall, tree top, water, ice, arrow, grass (the ledge
      # and bridge reuse tiles, the bridge's flipped corner included).
      self.assertEqual(len(field["tiles"]), 9)
      self.assertEqual(field["layers"], [(3, 3), (2, 2), (1, 1)])   # back to front, priority = BG
      # Tile entries: index in bits 0-9, horizontal and vertical flip in bits 10 and 11.
      top = field["metatiles"][field["dictionary"].index("test/bridge_top")]
      self.assertEqual(top[3] & 0x3FF, top[0] & 0x3FF)           # flipped copy shares the tile
      self.assertEqual(top[3] & 0xC00, 0x400)
      self.assertEqual(mc.cell_at(field, 7, 6), mc.CELL_BORDER)  # unpainted -> border, impassable
      wall = field["dictionary"].index("test/wall")
      self.assertEqual(mc.metatile_at(field, 1, 7, 6), wall)     # border pattern [wall, tree], phase 1
      self.assertEqual(mc.metatile_at(field, 0, 7, 6), mc.NO_METATILE)
      self.assertEqual(mc.cell_at(field, 12, 0) >> 18 & 15, 4)   # elevation
      stack = w["maps"][2]
      self.assertEqual(stack["layers"], [(0, 3), (2, 0)])        # any BG, any priority
      self.assertTrue(any(p.name == "test.json" for p in deps))
      self.assertEqual(w["start"], (0, 1, 1, 0))
      src = mc.emit("world.json", w, debug_names=True)
      for needle in ("const World kWorld", "kBehaviorNames", "static_assert", "kAnims0"):
         self.assertIn(needle, src)

   def test_png_sheet_auto_palette(self) -> None:
      """!A PNG sheet gets its palette from its colors and slices into tiles."""
      px = bytearray()
      for y in range(8):
         for x in range(16):
            px += bytes((255, 0, 0, 255) if x < 8 else (0, 0, 255, 255))
      bitmap.write_png(self.s.dir / "tilesets/art.png", 16, 8, px)
      ts = self.s.load("tilesets/test.json")
      ts["sheets"].append({"name": "art", "palette": "auto", "image": "art.png"})
      ts["metatiles"].append({"name": "red", "tiles": ["art:0"] * 4})
      self.s.save("tilesets/test.json", ts)
      mp = self.s.load("maps/field.json")
      mp["metatiles"].append("test/red")
      self.s.save("maps/field.json", mp)
      field = self.s.compile()["maps"][0]
      self.assertEqual(len(field["palettes"]), 2)
      self.assertEqual(field["palettes"][1][:2], [31, 31 << 10])   # BGR555 red, blue
      self.assertEqual(field["metatiles"][-1][0] >> 12, 1)       # bank 1

   def test_transparent_color(self) -> None:
      """!An image sheet's transparent color keys its pixels out of the palette."""
      px = bytearray()
      for y in range(8):
         for x in range(16):
            px += bytes((255, 0, 255, 255) if x % 2 else (0, 128, 0, 255))   # magenta background stripes
      bitmap.write_png(self.s.dir / "tilesets/keyed.png", 16, 8, px)
      ts = self.s.load("tilesets/test.json")
      ts["sheets"].append({"name": "keyed", "palette": "auto", "image": "keyed.png", "transparent": "#ff00ff"})
      ts["metatiles"].append({"name": "keyed", "tiles": ["keyed:0"] * 4})
      self.s.save("tilesets/test.json", ts)
      mp = self.s.load("maps/field.json")
      mp["metatiles"].append("test/keyed")
      self.s.save("maps/field.json", mp)
      w = self.s.compile()
      sheet = w["tilesets"]["test"].sheets["keyed"]
      self.assertEqual(w["tilesets"]["test"].palettes["test/keyed"][1], 1)   # magenta took no entry
      tile = sheet.tile(0, "test")
      self.assertEqual([tile[0][x] for x in range(8)], [1, 0] * 4)              # every other pixel transparent
      ts["sheets"][-1]["transparent"] = "magenta"
      self.s.save("tilesets/test.json", ts)
      self.fails("transparent is a color")

   def test_map_groups(self) -> None:
      """!Map groups organise maps; each map in at most one group."""
      w = self.s.load("world.json")
      maps = list(w["maps"])
      w["map_groups"] = [{"name": "Outside", "maps": maps[:1], "groups": [{"name": "Deep", "maps": maps[1:2]}]}]
      self.s.save("world.json", w)
      self.s.compile()
      for groups, fragment in (([{"name": "A", "maps": ["nowhere"]}], "unknown map 'nowhere'"),
                               ([{"name": "A", "maps": maps[:1]}, {"name": "B", "maps": maps[:1]}],
                                "already in group 'A'"),
                               ([{"name": "A"}, {"name": "A"}], "two groups named 'A'"),
                               ([{"name": "A", "colour": 1}], "unknown key"),
                               ([{"name": ""}], "needs a name")):
         w["map_groups"] = groups
         self.s.save("world.json", w)
         self.fails(fragment)

   def test_rules(self) -> None:
      """!Typos and limits are errors with useful messages."""
      mp = self.s.load("maps/field.json")
      bad = copy.deepcopy(mp)
      bad["metatiles"][0] = "test/nope"
      self.s.save("maps/field.json", bad)
      self.fails("no metatile 'nope'")
      bad = copy.deepcopy(mp)
      bad["chunks"][1]["at"] = [0, 0]
      self.s.save("maps/field.json", bad)
      self.fails("appears twice")
      bad = copy.deepcopy(mp)
      bad["chunks"][0]["cells"][0][0] = 40
      self.s.save("maps/field.json", bad)
      self.fails("past the map's behaviors")
      bad = copy.deepcopy(mp)
      bad["schema"] = "capsule.map/1"
      self.s.save("maps/field.json", bad)
      self.fails("schema must be 'capsule.map/2'")
      bad = copy.deepcopy(mp)
      bad["colour"] = 1
      self.s.save("maps/field.json", bad)
      self.fails("unknown key")
      self.s.save("maps/field.json", mp)
      ts = self.s.load("tilesets/test.json")
      bad = copy.deepcopy(ts)
      bad["metatiles"][0]["behavior"] = "water"   # behavior is painted on maps now
      self.s.save("tilesets/test.json", bad)
      self.fails("unknown key(s) behavior")
      bad = copy.deepcopy(ts)
      bad["metatiles"][0]["tiles"][0] = "t:99"
      self.s.save("tilesets/test.json", bad)
      self.fails("tiles 0-7")
      bad = copy.deepcopy(ts)
      bad["sheets"][1]["palette"] = ["#ffffff"] * 6
      self.s.save("tilesets/test.json", bad)
      self.fails("must share the palette")
      self.s.save("tilesets/test.json", ts)
      world = self.s.load("world.json")
      world["behaviors"][0]["name"] = "plain"
      self.s.save("world.json", world)
      self.fails("behavior 0 must be")

   def test_blank_map(self) -> None:
      """!A map with no layers, metatiles, chunks or border compiles: everything is blank and blocked."""
      for n in ("field", "stack", "crowd"):   # their warps land on small's painted cells
         m = self.s.load(f"maps/{n}.json")
         m.pop("events", None)
         self.s.save(f"maps/{n}.json", m)
      self.s.save("maps/small.json", {"schema": "capsule.map/2", "id": "small", "tilesets": ["test"]})
      w = self.s.compile()
      small = w["maps"][1]
      self.assertEqual(small["metatiles"], [])
      self.assertEqual(small["layers"], [])
      self.assertEqual(len(small["chunks"]), 1)                    # one unpainted chunk
      self.assertEqual(mc.cell_at(small, 3, 3), mc.CELL_BORDER)
      self.assertEqual(mc.cell_at(small, 40, -9), mc.CELL_BORDER)
      self.assertIn("kWorld", mc.emit("world.json", w))
      # A layer with nothing painted: blank border, blank cells.
      self.s.save("maps/small.json", {"schema": "capsule.map/2", "id": "small", "layers": [{"bg": 2}]})
      small = self.s.compile()["maps"][1]
      self.assertEqual(small["border"], [[mc.NO_METATILE]])
      self.assertEqual(mc.metatile_at(small, 0, 5, 5), mc.NO_METATILE)

   def test_layers_and_attributes(self) -> None:
      """!Layer rules, attribute cells with the map's behavior dictionary."""
      mp = self.s.load("maps/field.json")
      ok = mp["layers"]
      mp["layers"] = ok + [{"bg": 2}]
      self.s.save("maps/field.json", mp)
      self.fails("BG2 already holds")
      mp["layers"] = ok + [{"bg": 0}, {"bg": 0}]
      self.s.save("maps/field.json", mp)
      self.fails("up to 4 tile layers")
      mp["layers"] = [dict(ok[0], content="parallax_image")] + ok[1:]
      self.s.save("maps/field.json", mp)
      self.fails("content must be")
      mp["layers"] = ok
      mp["behaviors"] = ["normal", "lava"]
      self.s.save("maps/field.json", mp)
      self.fails("unknown behavior 'lava'")
      # A cell's behavior index goes through the map's dictionary to the world's index.
      mp["behaviors"] = ["water", "ledge_down", "ice", "push_right", "normal"]   # index 0 = water (world 1)
      # Bit 22 is the encounter flag; it must survive the behavior remapping.
      mp["chunks"][0]["cells"][0][0] = 0 | 1 << 22
      self.s.save("maps/field.json", mp)
      field = self.s.compile()["maps"][0]
      self.assertEqual(mc.cell_at(field, 0, 0), 1 | 1 << 22)
      mp["chunks"][0]["layers"][0][0][0] = 999
      self.s.save("maps/field.json", mp)
      self.fails("expected an integer 0-")

   def test_empty_tileset(self) -> None:
      """!A new, empty tileset (no sheets or metatiles) does not stop the world compiling."""
      world = self.s.load("world.json")
      world["tilesets"]["fresh"] = "tilesets/fresh.json"
      self.s.save("world.json", world)
      self.s.save("tilesets/fresh.json", {"schema": "capsule.tileset/2", "id": "fresh"})
      w = self.s.compile()
      self.assertEqual(w["tilesets"]["fresh"].metatiles, {})

   def test_map_tilesets(self) -> None:
      """!A map's tilesets list must name world tilesets and cover its dictionary."""
      mp = self.s.load("maps/field.json")
      mp["tilesets"] = ["test"]
      self.s.save("maps/field.json", mp)
      self.s.compile()
      mp["tilesets"] = ["nope"]
      self.s.save("maps/field.json", mp)
      self.fails("unknown tileset 'nope'")
      mp["tilesets"] = []
      self.s.save("maps/field.json", mp)
      self.fails("leave out")

   def test_too_many_palettes(self) -> None:
      """!More than 13 palettes on one map is refused (until palette streaming)."""
      ts = self.s.load("tilesets/test.json")
      mp = self.s.load("maps/field.json")
      for i in range(13):
         ts["sheets"].append({"name": f"p{i}", "palette": ["#ffffff"], "size": [8, 8],
                              "shapes": [{"shape": "rect", "x": 0, "y": 0, "w": 8, "h": 8, "color": 1}]})
         ts["metatiles"].append({"name": f"m{i}", "tiles": [f"p{i}:0"] * 4})
         mp["metatiles"].append(f"test/m{i}")
      self.s.save("tilesets/test.json", ts)
      self.s.save("maps/field.json", mp)
      self.fails("uses 14 palettes")

   def test_default_elevations(self) -> None:
      """!Without elevations the default rule compiles, with elevation_priority's priorities; written out it is
      the same table."""
      w = self.s.load("world.json")
      del w["elevations"]
      self.s.save("world.json", w)
      table = self.s.compile()["elevations"]
      self.assertEqual(table[0][:3], (0xFFFF, 0, 2))
      self.assertEqual(table[7][:3], (1 << 7, 0, 2))
      self.assertEqual(table[15][:3], (0xFFFF, mc.ELEVATION_KEEP, 2))
      w["elevation_priority"] = [2] * 4 + [1] + [2] * 11
      self.s.save("world.json", w)
      table = self.s.compile()["elevations"]
      self.assertEqual(table[4][2], 1)
      w["elevations"] = mc.default_elevations(w.pop("elevation_priority"))
      self.s.save("world.json", w)
      self.assertEqual(self.s.compile()["elevations"], table)
      w["elevation_priority"] = [2] * 16
      self.s.save("world.json", w)
      self.fails("leave out elevation_priority")

   def test_custom_elevations(self) -> None:
      """!Custom levels: lists by number or name (each meets itself), keep levels, priorities; bad entries fail."""
      w = mc.compile_world(FIXTURE / "world.json")
      table = w["elevations"]
      self.assertEqual(table[12][:3], (1 << 12, 0, 2))                     # sealed: only itself
      self.assertEqual(table[0][0], 0xFFFF & ~(1 << 12))                   # any: all but 12
      self.assertEqual(table[9][:3], (1 << 9 | 1 << 5, mc.ELEVATION_KEEP, 2))   # overpass meets "ramp" and keeps
      self.assertEqual(table[4][2], 1)
      src = mc.emit("world.json", w)
      self.assertIn("{0x0220, kElevationKeep, 2},   // 9: overpass", src)
      self.assertIn("kElevations, ", src)
      raw = self.s.load("world.json")
      for change, fragment in ((lambda e: e.pop(), "needs 16 levels"),
                               (lambda e: e[3].update(name="any"), "needs a unique name"),
                               (lambda e: e[3].update(meets=[16]), "meets lists levels 0-15"),
                               (lambda e: e[3].update(meets=["nowhere"]), "meets lists levels"),
                               (lambda e: e[3].update(meets="some"), "meets is"),
                               (lambda e: e[3].update(on_arrive="stay"), "on_arrive is"),
                               (lambda e: e[3].update(priority=4), "priority"),
                               (lambda e: e[3].update(color=1), "unknown key")):
         bad = copy.deepcopy(raw)
         change(bad["elevations"])
         self.s.save("world.json", bad)
         with self.subTest(fragment=fragment):
            self.fails(fragment)

   def test_darkness_levels(self) -> None:
      """!C8: the darkness levels are the world's data: emitted as kDarknessLevels, the default levels when it states
      none, no map darker than its darkest level, each level's limits checked."""
      w = self.s.compile()
      self.assertEqual(w["darkness_levels"], [{"lit": 64, "brightness": 8}, {"lit": 32, "brightness": 12},
                                              {"lit": 16, "brightness": 16}])
      src = mc.emit("world.json", w)
      self.assertIn("{32, 12},   // level 2", src)
      raw = self.s.load("world.json")
      del raw["darkness_levels"]
      self.s.save("world.json", raw)
      self.assertEqual(self.s.compile()["darkness_levels"], mc.DEFAULT_DARKNESS_LEVELS)
      raw["darkness_levels"] = [{"lit": 10, "brightness": 4}]
      self.s.save("world.json", raw)
      self.fails("'small' darkness")   # map small is level 2
      for levels, fragment in (([{"lit": 300, "brightness": 1}], "lit"), ([{"lit": 1, "brightness": 17}], "brightness"),
                               ([{"lit": 1, "brightness": 1}] * (mc.MAX_DARKNESS + 1), "at most"),
                               ([{"lit": 1, "glow": 1}], "glow")):
         raw["darkness_levels"] = levels
         self.s.save("world.json", raw)
         with self.subTest(fragment=fragment):
            self.fails(fragment)
      # no levels at all: every map lit (map small's level 2 refused)
      raw["darkness_levels"] = []
      self.s.save("world.json", raw)
      self.fails("'small' darkness")

   def test_vehicle_classes(self) -> None:
      """!Vehicle classes: names or objects with speeds, can_run, first animation and passable behaviors."""
      w = self.s.compile()
      self.assertEqual([v["name"] for v in w["vehicles"]], ["foot", "swim", "paddle", "trek"])
      swim = w["vehicles"][1]
      self.assertEqual((swim["walk"], swim["run"], swim["can_run"], swim["first_animation"]), (2, 2, False, 12))
      src = mc.emit("world.json", w)
      self.assertIn("{1, 2, 0, kVehicleCanRun},   // foot", src)
      self.assertIn("{2, 2, 12, 0},   // swim", src)
      self.assertIn("{0, 1, 1, 0, 0}, kVehicleClasses, kDarknessLevels, 3};", src)
      # A class's passable list narrows what the behaviors allow: swim only on water.
      raw = self.s.load("world.json")
      raw["vehicle_classes"][1]["passable"] = ["water"]
      self.s.save("world.json", raw)
      masks = {n: m for m, _k, _d, n in self.s.compile()["behaviors"]}
      self.assertEqual(masks["normal"], 0b1001)   # foot and (I3) trek
      self.assertEqual(masks["water"], 0b110)   # swim and (F2) paddle
      # Names alone still work (on-foot defaults); load_world gives names either way.
      raw["vehicle_classes"] = ["foot", "swim"]
      raw["behaviors"][1]["passable"] = ["swim"]   # F2: the fixture's water also names paddle
      self.s.save("world.json", raw)
      self.assertEqual(mc.load_world(self.s.dir / "world.json")[2], ["foot", "swim"])
      self.assertEqual([(v["walk"], v["run"], v["can_run"]) for v in self.s.compile()["vehicles"]],
                       [(1, 2, True), (1, 2, True)])
      raw = self.s.load("world.json")
      # A bad swim class (class 1), then bad class lists.
      bad_swim = (({"speeds": {"walk": 3}}, "pixels per frame"),
                  ({"speeds": {"fly": 3}}, "unknown key"),
                  ({"can_run": 1}, "can_run must be"),
                  ({"first_animation": 6}, "multiple of 4"),
                  ({"passable": ["lava"]}, "unknown behavior"))
      cases = [([raw["vehicle_classes"][0], {"name": "swim", **entry}], fragment) for entry, fragment in bad_swim]
      cases += [(raw["vehicle_classes"] + [{"speeds": {}}], "distinct names"),
                (raw["vehicle_classes"] + ["foot"], "distinct names"),
                (raw["vehicle_classes"] + [f"c{i}" for i in range(8)], "1-8 distinct")]
      for classes, fragment in cases:
         bad = copy.deepcopy(raw)
         bad["vehicle_classes"] = classes
         self.s.save("world.json", bad)
         with self.subTest(fragment=fragment):
            self.fails(fragment)

   def test_can_run(self) -> None:
      """!A map's can_run: false sets kMapNoRun; it defaults to true; other values fail."""
      w = self.s.compile()
      flags = {mp["name"]: mp["flags"] for mp in w["maps"]}
      self.assertEqual(flags["small"], mc.MAP_NO_RUN)
      self.assertEqual(flags["field"], mc.MAP_ESCAPE_POINT)   # F2: field is an escape point
      src = mc.emit("world.json", w)
      # F2: small is also dark (level 2, tag dark), field an escape point
      self.assertEqual(src.count("kMapNoRun, 2, 0x0001}"), 1)
      self.assertEqual(src.count("kMapEscapePoint}"), 1)
      mp = self.s.load("maps/field.json")
      mp["can_run"] = "no"
      self.s.save("maps/field.json", mp)
      self.fails("can_run must be true or false")


class FixedPlacementTests(unittest.TestCase):
   """!I2: fixed-placement tilesets, the "all" dictionary, animation phases, behavior walls, shared arrays."""

   def setUp(self) -> None:
      """!Fresh scratch copy."""
      ## The scratch copy.
      self.s = Scratch()

   def tearDown(self) -> None:
      """!Removes it."""
      self.s.close()

   def fails(self, fragment: str) -> None:
      """!Asserts that compiling the scratch copy fails mentioning @p fragment.
      @param fragment  expected text
      """
      with self.assertRaises(mc.ManifestError) as ctx:
         self.s.compile()
      self.assertIn(fragment, str(ctx.exception))

   def test_fixed_map(self) -> None:
      """!fixed_a loads the tileset at its place: tile k is the sheet's tile k (no dedup, nothing renumbered),
      banks as placed (entries 1-15), raw entries, the animation at its place with its phase."""
      w = mc.compile_world(FIXTURE / "world.json")
      fixed = next(m for m in w["maps"] if m["name"] == "fixed_a")
      self.assertEqual(len(fixed["tiles"]), 8)
      self.assertEqual(fixed["tiles"][2], tuple(tuple([2] * 8) for _ in range(8)))
      self.assertEqual(fixed["tiles"][5], tuple(tuple([0] * 8) for _ in range(8)))   # placed, no sheet tile: blank
      self.assertEqual(len(fixed["palettes"]), 2)
      self.assertEqual(fixed["palettes"][1][0], 0x7FFF)   # bank 1, entry 1: white
      self.assertEqual(fixed["dictionary"], ["fixed/floor", "fixed/wall", "fixed/glow", "fixed/white"])
      self.assertEqual(fixed["metatiles"][1], [2, 0x402, 0x802, 0xC02])
      self.assertEqual([(a["first"], a["count"], a["period"], a["phase"]) for a in fixed["anims"]], [(3, 1, 4, 1)])
      self.assertEqual(w["behavior_walls"][-2], 1 << mc.DIRECTIONS.index("right"))   # fence_east (I3: then counter_top)
      src = mc.emit("world.json", w)
      self.assertIn("{0x0B, BehaviorKind::None, 0, 8},   // fence_east", src)   # I3: trek walks it too
      self.assertIn("{kAnimFrames6_0, 3, 1, 2, 4, 1, 0},   // fixed/spark", src)
      # fixed_b's tiles, palettes, metatiles and animation frames are fixed_a's arrays
      self.assertNotIn("kTiles7[]", src)
      self.assertIn("kTiles6, kPalettes6, kBorder", src.split("// fixed_b")[0].rsplit("{kChunks7", 1)[1])

   def test_fixed_rules(self) -> None:
      """!Fixed tilesets' rules: indexed sheets only there, entries 16-bit, animations inside the place, phases
      below the period, places that do not overlap, no mixing with other tilesets."""
      ts = self.s.load("tilesets/fixed.json")
      bad = dict(ts, animations=[dict(ts["animations"][0], at=8)])
      self.s.save("tilesets/fixed.json", bad)
      self.fails("at (the place owns 8 tiles)")
      self.s.save("tilesets/fixed.json", dict(ts, animations=[dict(ts["animations"][0], phase=4)]))
      self.fails("phase")
      self.s.save("tilesets/fixed.json", dict(ts, metatiles=[{"name": "x", "entries": [0x10000, 0, 0, 0]}]))
      self.fails("entries")
      self.s.save("tilesets/fixed.json", dict(ts, palettes=[["#000000"] * 15]))
      self.fails("16 colors")
      self.s.save("tilesets/fixed.json", ts)
      other = dict(ts, place=dict(ts["place"], tile=4))
      self.s.save("tilesets/other.json", other)
      w = self.s.load("world.json")
      w["tilesets"]["other"] = "tilesets/other.json"
      self.s.save("world.json", w)
      fa = self.s.load("maps/fixed_a.json")
      self.s.save("maps/fixed_a.json", dict(fa, tilesets=["fixed", "other"]))
      self.fails("both place VRAM tile 4")
      self.s.save("maps/fixed_a.json", dict(fa, tilesets=["fixed", "test"]))
      self.fails("all of them fixed")
      self.s.save("maps/fixed_a.json", dict(fa, tilesets=None))
      self.fails("metatiles \"all\" needs the map's tilesets")
      test = self.s.load("tilesets/test.json")
      test["sheets"][0]["indexed"] = True
      self.s.save("maps/fixed_a.json", fa)
      self.s.save("tilesets/test.json", test)
      self.fails("indexed sheets belong to a fixed tileset")

   def test_walls_rule(self) -> None:
      """!walls lists directions, each once."""
      w = self.s.load("world.json")
      w["behaviors"][-1]["walls"] = ["east"]
      self.s.save("world.json", w)
      self.fails("walls lists directions")


## C13b: the cell interaction fixture (tests/c13b_tests.cpp runs its rules).
INTERACTIONS = ROOT / "tests/data/c13b/world.json"


class InteractionTests(unittest.TestCase):
   """!C13b: the world's "interactions" (capsule::map::Interaction rules)."""

   def compile_with(self, rules: list[JsonDict]) -> JsonDict:
      """!Compiles the C13b fixture with other rules (an in-memory manifest).
      @param rules  the world's "interactions"
      @return compile_world() result
      """
      w = json.loads(INTERACTIONS.read_text(encoding="utf-8"))
      w["interactions"] = rules
      return mc.compile_world(INTERACTIONS, sources={INTERACTIONS.resolve(): w})

   def test_fixture(self) -> None:
      """!The rules keep the world's order and resolve names to numbers; the source holds the World's last fields."""
      w = mc.compile_world(INTERACTIONS)
      rules = w["interactions"]
      self.assertEqual(len(rules), 7)
      self.assertEqual([r["behavior"] for r in rules], [2, 3, 4, 4, 5, 7, 7])
      self.assertEqual(rules[0]["facing"], mc.ANY_DIRECTION)
      self.assertEqual(rules[0]["vehicle"], mc.ANY_VEHICLE)
      self.assertEqual(rules[0]["flag"], mc.NO_CONDITION)
      self.assertEqual(rules[1]["facing"], mc.DIRECTIONS.index("up"))
      self.assertNotEqual(rules[2]["flag"], mc.NO_CONDITION)
      self.assertEqual(rules[4]["flags"], mc.INTERACT_SAME_LEVEL)
      self.assertEqual(rules[5]["flags"], mc.INTERACT_AFTER_ACTIONS)
      self.assertEqual(rules[5]["vehicle"], 1)
      text = mc.emit("world.json", w)
      self.assertIn("const Interaction kInteractions[] = {", text)
      self.assertIn(", kInteractions, 7};", text)

   def test_no_rules(self) -> None:
      """!A world without rules emits the World initializer of before (no interaction fields)."""
      w = self.compile_with([])
      self.assertEqual(w["interactions"], [])
      self.assertNotIn("kInteractions", mc.emit("world.json", w))

   def test_errors(self) -> None:
      """!Unknown names and bad values are errors naming the rule."""
      bad = [({"behavior": "lava", "script": "use_pc"}, "unknown behavior"),
             ({"behavior": "pc", "script": "missing"}, "unknown script"),
             ({"behavior": "pc", "script": "use_pc", "flag": "nope"}, "unknown flag"),
             ({"behavior": "pc", "script": "use_pc", "facing": "north"}, "facing must be"),
             ({"behavior": "pc", "script": "use_pc", "vehicle": "car"}, "unknown vehicle class"),
             ({"behavior": "pc", "script": "use_pc", "same_level": 1}, "same_level must be"),
             ({"behavior": "pc", "script": "use_pc", "when": "always"}, "interaction 0")]
      for rule, fragment in bad:
         with self.subTest(fragment=fragment):
            with self.assertRaises(mc.ManifestError) as ctx:
               self.compile_with([rule])
            self.assertIn(fragment, str(ctx.exception))


if __name__ == "__main__":
   unittest.main()
