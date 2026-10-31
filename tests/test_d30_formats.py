#!/usr/bin/env python3
"""!Tests of the D-30 data formats (DS1) in the compilers: tile animation targets, override sets, door bindings and
the draw-order rule (tools/map_compiler.py, tools/event_compiler.py), door assets and direction-grid sprites
(tools/asset_compiler.py), the override statement (tools/script_compiler.py). Fixture: tests/data/d30."""
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
import asset_compiler as ac  # noqa: E402
import map_compiler as mc  # noqa: E402
import script_compiler as sc  # noqa: E402

## The fixture.
FIXTURE = ROOT / "tests/data/d30"
## A parsed JSON object.
JsonDict = dict[str, Any]


class Scratch:
   """!A copy of the fixture in a temporary folder, to change on purpose.

   @var dir
   The copy's folder (Path).
   """

   def __init__(self, tmp: str) -> None:
      """!Copies the fixture.
      @param tmp  an empty temporary folder
      """
      self.dir = Path(tmp) / "d30"
      shutil.copytree(FIXTURE, self.dir)

   def load(self, rel: str) -> Any:
      """!A manifest of the copy.
      @param rel  its path in the fixture
      @return the parsed JSON
      """
      return json.loads((self.dir / rel).read_text(encoding="utf-8"))

   def save(self, rel: str, data: Any) -> None:
      """!Writes a manifest of the copy.
      @param rel   its path
      @param data  the JSON
      @return None
      """
      (self.dir / rel).write_text(json.dumps(data), encoding="utf-8")

   def world(self) -> JsonDict:
      """!Compiles the copy's world.
      @return compile_world()'s result
      """
      return mc.compile_world(self.dir / "world.json", scripts_path=self.dir / "scripts/scripts.json")


def compile_fixture() -> JsonDict:
   """!The fixture's world, compiled.
   @return compile_world()'s result
   """
   return mc.compile_world(FIXTURE / "world.json", scripts_path=FIXTURE / "scripts/scripts.json")


class AnimationTargetTests(unittest.TestCase):
   """!Tile animation targets (D-30 section 5)."""

   def test_fixture(self) -> None:
      """!Three targets: the first is the record's own, two more with offsets 2 and 3 (the animation's phase, or
      their own)."""
      world = compile_fixture()
      yard = world["maps"][0]
      an = yard["anims"][0]
      self.assertEqual(len(an["frames"]), 4)
      self.assertEqual([(off, ph) for _first, off, ph in an["targets"]], [(2, 0), (3, 1)])
      self.assertEqual(an["phase"], 0)
      self.assertGreater(world["target_saved"], 0)
      text = mc.emit("world.json", world)
      self.assertIn("{kAnimFrames0_0, 1, 1, 4, 4, 0, 0, 2},", text)
      self.assertIn("const AnimTarget kAnimTargets0[] = {", text)

   def test_first_offset_rotates_frames(self) -> None:
      """!A first target with an offset: the frames are stored from it on, the others' offsets made relative."""
      with tempfile.TemporaryDirectory() as tmp:
         s = Scratch(tmp)
         ts = s.load("tilesets/yard.json")
         ts["animations"][0]["targets"][0]["offset"] = 1
         s.save("tilesets/yard.json", ts)
         base = compile_fixture()["maps"][0]["anims"][0]
         an = s.world()["maps"][0]["anims"][0]
         self.assertEqual(an["frames"], base["frames"][1:] + base["frames"][:1])
         self.assertEqual([off for _f, off, _p in an["targets"]], [1, 2])

   def test_single_target_form_unchanged(self) -> None:
      """!An animation without targets keeps "tiles" (and emits the initializer of before)."""
      with tempfile.TemporaryDirectory() as tmp:
         s = Scratch(tmp)
         ts = s.load("tilesets/yard.json")
         an = ts["animations"][0]
         an["tiles"] = an.pop("targets")[0]["tiles"]
         s.save("tilesets/yard.json", ts)
         world = s.world()
         self.assertEqual(world["maps"][0]["anims"][0]["targets"], [])
         self.assertIn("{kAnimFrames0_0, 1, 1, 4, 4, 0, 0},", mc.emit("world.json", world))

   def test_errors(self) -> None:
      """!Offsets past the frames, overlapping targets, tiles past the sheet, targets with tiles."""
      cases = [
         (lambda an: an["targets"][1].update(offset=4), "frame index"),
         (lambda an: an["targets"][2].update(tiles="tiles:5"), "both replace tile 5"),
         (lambda an: an["targets"][2].update(tiles="tiles:8"), "8"),
         (lambda an: an.update(tiles="tiles:4"), "names its tiles in them"),
         (lambda an: an["targets"][1].update(phase=4), "phase"),
         (lambda an: an.update(targets=[]), "targets lists"),
      ]
      for change, message in cases:
         with self.subTest(message=message), tempfile.TemporaryDirectory() as tmp:
            s = Scratch(tmp)
            ts = s.load("tilesets/yard.json")
            change(ts["animations"][0])
            s.save("tilesets/yard.json", ts)
            with self.assertRaises(mc.ManifestError) as e:
               s.world()
            self.assertIn(message, str(e.exception))


class OverrideSetTests(unittest.TestCase):
   """!Cell override sets (D-30 section 2)."""

   def test_fixture(self) -> None:
      """!Two sets on the yard: one until the map is left, one through the flag gate_open; their cells."""
      world = compile_fixture()
      sets = world["override_sets"]
      self.assertEqual([s["name"] for s in sets], ["bridge", "gate_open"])
      self.assertEqual([s["persist"] for s in sets], [0, 1])
      self.assertEqual(sets[0]["cells"][0], (1, 1, 0, None, 3))   # yard/mark, the dictionary's 4th
      self.assertEqual(sets[0]["cells"][1][3], (4101, 5125, 6149, 7173))
      text = mc.emit("world.json", world)
      self.assertIn("OverridePersist::Flag", text)
      self.assertIn("kOverrideEntries, 0}", text)
      ids = mc.evc.emit_ids("world.json", world["map_names"], [], None, [s["name"] for s in sets])
      self.assertIn("OVERRIDE_GATE_OPEN = 1", ids)

   def test_errors(self) -> None:
      """!Cells outside the chunks, missing layers, metatiles outside the dictionary, names twice, a value of 0."""
      cases = [
         (lambda m: m["override_sets"][0]["cells"][0].update(cell=[9, 1]), "outside the map's chunks"),
         (lambda m: m["override_sets"][0]["cells"][0].update(layer=2), "layer"),
         (lambda m: m["override_sets"][0]["cells"][0].update(metatile="yard/nothing"), "not in the map's metatiles"),
         (lambda m: m["override_sets"][1].update(name="bridge"), "unique name"),
         (lambda m: m["override_sets"][1].update(persist={"flag": "no_such_flag"}), "unknown flag"),
         (lambda m: m["override_sets"][1].update(persist={"var": "x", "value": 0}), "must not be 0"),
         (lambda m: m["override_sets"][0]["cells"][1].update(entries=[1, 2, 3]), "4 map entries"),
      ]
      for change, message in cases:
         with self.subTest(message=message), tempfile.TemporaryDirectory() as tmp:
            s = Scratch(tmp)
            m = s.load("maps/yard.json")
            change(m)
            s.save("maps/yard.json", m)
            with self.assertRaises(mc.ManifestError) as e:
               s.world()
            self.assertIn(message, str(e.exception))

   def test_names_unique_in_the_world(self) -> None:
      """!Scripts name sets the world over: two maps may not both have a set of one name."""
      with tempfile.TemporaryDirectory() as tmp:
         s = Scratch(tmp)
         hall = s.load("maps/hall.json")
         hall["override_sets"] = [{"name": "bridge", "cells": [{"cell": [1, 1], "metatile": None}]}]
         s.save("maps/hall.json", hall)
         with self.assertRaises(mc.ManifestError) as e:
            s.world()
         self.assertIn("also one of map 'yard'", str(e.exception))

   def test_script_statement(self) -> None:
      """!`override apply|clear NAME;` is Op Override with the set's world index; unknown names are errors."""
      comp = sc.compile_file(FIXTURE / "scripts/scripts.json")
      self.assertEqual(comp.errors(), [])
      op = sc.OP["Override"]
      code = bytes(comp.result["code"])
      self.assertIn(bytes([op, 1, 0, 0]), code)   # apply bridge (set 0)
      self.assertIn(bytes([op, 0, 1, 0]), code)   # clear gate_open (set 1)
      with tempfile.TemporaryDirectory() as tmp:
         s = Scratch(tmp)
         src = s.dir / "scripts/d30.cscript"
         src.write_text(src.read_text(encoding="utf-8") + "script bad { override apply nowhere; }\n",
                        encoding="utf-8")
         errs = sc.compile_file(s.dir / "scripts/scripts.json").errors()
         self.assertTrue(any("not an override set" in str(e) for e in errs))


class DoorTests(unittest.TestCase):
   """!Door assets and door bindings (D-30 section 1)."""

   def assets(self) -> JsonDict:
      """!The fixture's asset manifest.
      @return it
      """
      return json.loads((FIXTURE / "assets.json").read_text(encoding="utf-8"))

   def compile(self, manifest: JsonDict) -> list[JsonDict]:
      """!Compiles an asset manifest from the fixture's folder.
      @param manifest  the manifest
      @return its doors
      """
      ac.compile_manifest(manifest, FIXTURE)
      return ac.compile_manifest.doors

   def test_fixture(self) -> None:
      """!A tiles door (two frames of 8 tiles at the tile area's end, closing reversed) and an override door."""
      gate, hatch = self.compile(self.assets())
      self.assertEqual((gate["frames"], gate["frame_tiles"], gate["load_at"]), (2, 8, ac.MAP_TILES - 8))
      self.assertEqual(gate["open"], [(ac.DOOR_CLOSED, 2), (0, 3), (1, 3)])
      self.assertEqual(gate["close"], list(reversed(gate["open"])))
      self.assertEqual(gate["entries"][:4], [1016, 1017, 1018, 1019])
      self.assertEqual(len(gate["entries"]), 2 * 2 * ac.CELL_ENTRIES)
      self.assertEqual((hatch["show"], hatch["entry_frames"], hatch["tiles"]), (1, 2, []))
      self.assertEqual(hatch["close"], [(0, 2), (ac.DOOR_CLOSED, 1)])
      entries, bgs, sprites, texts, lists = ac.compile_manifest(self.assets(), FIXTURE)
      text = ac.emit("assets.json", entries, bgs, sprites, texts, lists, (), ac.compile_manifest.doors)
      self.assertIn("DoorShow::Tiles", text)
      self.assertIn("kLists, 5, nullptr, kDoors};", text)

   def test_errors(self) -> None:
      """!Frame tiles past a frame, a step's frame past the frames, cells missing, a key, the sheet's size."""
      cases = [
         (lambda d: d["entries"][0][0][0].update(tile=8), "tile is one of the frame's tiles"),
         (lambda d: d["open"].append([2, 1]), "frame is 0-1"),
         (lambda d: d["entries"].pop(), "needs 2 cells"),
         (lambda d: d.update(frame_tiles=7), "not whole frames"),
         (lambda d: d.update(exit="sideways"), "exit is one of"),
         (lambda d: d["open"].append([0, 0]), "ticks"),
         (lambda d: d.update(frames=[]), "frames are an override door's"),
      ]
      for change, message in cases:
         with self.subTest(message=message):
            m = self.assets()
            change(m["assets"][3])
            with self.assertRaises(ac.ManifestError) as e:
               self.compile(m)
            self.assertIn(message, str(e.exception))

   def test_bindings(self) -> None:
      """!A warp's door is a cell binding (first), the world's doors bind by metatile; a bad key is an error."""
      world = compile_fixture()
      yard = world["doors"]["yard"]
      gate = ac.asset_key("door:GATE", "test")
      self.assertEqual(yard[0][:4], (gate, mc.NO_METATILE, 4, 2))
      self.assertEqual(yard[1][:3], (gate, 2, mc.DOOR_ANY_CELL))
      self.assertEqual(len(world["doors"]["hall"]), 2)
      with tempfile.TemporaryDirectory() as tmp:
         s = Scratch(tmp)
         w = s.load("world.json")
         w["doors"][0]["door"] = "sprite:GATE"
         s.save("world.json", w)
         with self.assertRaises(mc.ManifestError):
            s.world()
      with tempfile.TemporaryDirectory() as tmp:
         s = Scratch(tmp)
         m = s.load("maps/yard.json")
         m["events"]["warps"][0]["door"] = "gate"
         s.save("maps/yard.json", m)
         with self.assertRaises(mc.ManifestError):
            s.world()


class GridTests(unittest.TestCase):
   """!Direction-grid sprite sheets (D-30 section 3)."""

   def sprite(self, key: str, change: Any = None) -> JsonDict:
      """!One of the fixture's sprites, compiled.
      @param key     its key
      @param change  callable(asset) changing it first, or None
      @return compile_grid_sprite()'s result
      """
      manifest = json.loads((FIXTURE / "assets.json").read_text(encoding="utf-8"))
      a = copy.deepcopy(next(x for x in manifest["assets"] if x["key"] == key))
      if change is not None:
         change(a)
      return ac.compile_grid_sprite(a, {}, key, FIXTURE)

   def test_three_columns(self) -> None:
      """!Right is left mirrored: no right frames stored; identical frames merged; four animations per set."""
      s = self.sprite("sprite:GRID3")
      self.assertEqual(s["frames"], 5)
      self.assertEqual(len(s["animations"]), 8)
      left, right = s["animations"][2], s["animations"][3]
      self.assertEqual(right[0], left[0])
      self.assertEqual(right[4], [1])
      self.assertIsNone(left[4])
      frame_bytes = 16 * 32 // 2
      self.assertEqual(s["grid_saved"], (2 + 1) * frame_bytes)   # two right frames never stored, one merged

   def test_four_columns(self) -> None:
      """!A stored right column is used as it is."""
      s = self.sprite("sprite:GRID4")
      self.assertEqual(s["frames"], 4)
      self.assertEqual(s["animations"][3][0], [3])
      self.assertIsNone(s["animations"][3][4])
      self.assertEqual(s["grid_saved"], 0)

   def test_one_direction(self) -> None:
      """!An animation with a direction is one animation."""
      s = self.sprite("sprite:GRID3", lambda a: a["animations"].append({"rows": ["step"], "direction": "right"}))
      self.assertEqual(len(s["animations"]), 9)
      self.assertEqual(s["animations"][8][4], [1])

   def test_errors(self) -> None:
      """!Rows named twice, a row with a frame missing, an unknown row, frames beside a grid."""
      cases = [
         (lambda a: a.update(rows=["stand", "stand"]), "each once"),
         (lambda a: a["grid"][1].pop(), "every grid row"),
         (lambda a: a["animations"].append({"rows": ["jump"]}), "row names"),
         (lambda a: a.update(frames=[]), "frames are its grid"),
      ]
      for change, message in cases:
         with self.subTest(message=message):
            with self.assertRaises(ac.ManifestError) as e:
               self.sprite("sprite:GRID3", change)
            self.assertIn(message, str(e.exception))


class DrawOrderTests(unittest.TestCase):
   """!The world's draw-order rule (D-30 section 4)."""

   def test_fixture(self) -> None:
      """!The rule's fields as the runtime's DrawOrder."""
      rule = compile_fixture()["draw_order"]
      self.assertEqual((rule["mask"], rule["row_shift"], rule["rows"], rule["row_step"], rule["add"]),
                       (0xFF, 4, 16, 2, 1))
      self.assertEqual(rule["base"][3], 60)

   def test_default_and_errors(self) -> None:
      """!No rule: None (the runtime's default); powers of two; one base per elevation."""
      self.assertIsNone(mc.compile_draw_order({}))
      for change in ({"row_pixels": 12}, {"wrap": 300}, {"elevation_base": [0] * 4}, {"bias": 200}, {"shade": 1}):
         with self.subTest(change=change):
            with self.assertRaises(mc.ManifestError):
               mc.compile_draw_order({"draw_order": change})


if __name__ == "__main__":
   unittest.main()
