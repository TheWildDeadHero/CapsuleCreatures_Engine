#!/usr/bin/env python3
"""!Tests for map objects (tools/object_compiler.py through tools/map_compiler.py,
B-21), on a copy of the map test world (map town)."""
from __future__ import annotations

import sys
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import event_compiler as evc  # noqa: E402
import map_compiler as mc  # noqa: E402
import object_compiler as obc  # noqa: E402
from sequence_compiler import asset_key  # noqa: E402
from test_map_compiler import Scratch  # noqa: E402

## A parsed JSON object (a manifest).
JsonDict = dict[str, Any]


class ObjectTests(unittest.TestCase):
   """!Objects compile and are checked."""

   def setUp(self) -> None:
      """!A fresh copy of the world."""
      ## The copy.
      self.s = Scratch()

   def tearDown(self) -> None:
      """!Removes it."""
      self.s.close()

   def edit(self, fn: Callable[[JsonDict], object]) -> None:
      """!Changes the town map.
      @param fn  callable(manifest)
      """
      m = self.s.load("maps/town.json")
      fn(m)
      self.s.save("maps/town.json", m)

   def obj(self, which: str, **fields: Any) -> None:
      """!Changes one of the town's objects.
      @param which   object name
      @param fields  keys to set (None removes a key)
      """
      def change(m: JsonDict) -> None:
         o = next(o for o in m["objects"] if o["name"] == which)
         for k, v in fields.items():
            if v is None:
               o.pop(k, None)
            else:
               o[k] = v
      self.edit(change)

   def fails(self, text: str) -> None:
      """!Compiling must fail with a message containing text.
      @param text  expected part of the message
      """
      with self.assertRaises(mc.ManifestError) as cm:
         self.s.compile()
      self.assertIn(text, str(cm.exception))

   def test_fixture(self) -> None:
      """!The town's objects: records, pages, IDs, emitted tables."""
      w = self.s.compile()
      town = w["events"]["objects"]["town"]
      self.assertEqual(town["names"], ["guard", "rival", "wanderer", "ghost", "pacer"])
      guard = town["objects"][0]
      self.assertEqual(guard["sprite"], asset_key("sprite:BLINK"))
      self.assertEqual((guard["x"], guard["y"], guard["page_first"], guard["page_count"]), (2, 2, 0, 1))
      self.assertEqual(guard["trigger"], obc.TRIGGERS.index("action"))
      # Packed nibbles: motion is speed | frequency << 4, range is x | y << 4.
      self.assertEqual(guard["motion"], 2 | (3 << 4))
      rival = town["objects"][1]
      self.assertEqual(rival["sight"], 3)
      self.assertNotEqual(rival["until"], obc.NONE)
      self.assertEqual(town["objects"][2]["movement"], obc.MOVEMENTS.index("wander"))
      self.assertEqual(town["objects"][4]["range"], 0x02)
      page = town["pages"][0]
      self.assertEqual(page["fields"], obc.PAGE_FACING | obc.PAGE_SCRIPT)
      self.assertEqual(page["facing"], evc.DIRECTIONS.index("left"))
      self.assertNotEqual(page["flag"], obc.NONE)
      self.assertEqual(w["events"]["objects"]["field"], {"objects": [], "pages": [], "routes": [], "names": []})
      ids = evc.emit_ids("world.json", w["map_names"], w["events"]["hooks"],
                         {n: o["names"] for n, o in w["events"]["objects"].items()})
      self.assertIn("OBJ_TOWN_PACER = 4", ids)
      text = mc.emit("world.json", w)
      self.assertIn("const ObjectTemplate kObjects3[]", text)
      self.assertIn("Movement::PaceHorizontal", text)
      self.assertIn("const ObjectPage kObjectPages3[]", text)
      self.assertIn("kObjects3, kObjectPages3, nullptr, nullptr, 1, 0, 5, kNoLocation}", text)

   def test_names_and_limits(self) -> None:
      """!Names are unique identifiers; at most 15 objects; cells painted and free."""
      self.obj("ghost", name="guard")
      self.fails("another object is named 'guard'")
      self.obj("guard", name="bad name")
      self.fails("needs a name that is an identifier")

      def many(m: JsonDict) -> None:
         m["objects"] = [{"name": f"o{i}", "at": [i % 8, i // 8], "sprite": "sprite:X"} for i in range(16)]
      self.edit(many)
      self.fails("16 objects; at most 15")
      self.edit(lambda m: m.update(objects=[{"name": "a", "at": [9, 0], "sprite": "sprite:X"}]))
      self.fails("outside the map's chunks")
      # I3: a map with unpainted_events keeps it there (imported data)
      self.edit(lambda m: m.update(unpainted_events=True))
      self.s.compile()
      self.edit(lambda m: m.pop("unpainted_events"))
      self.edit(lambda m: m.update(objects=[{"name": "a", "at": [1, 1], "sprite": "sprite:X"},
                                            {"name": "b", "at": [1, 1], "sprite": "sprite:X"}]))
      self.fails("object 'a' stands on 1,1")
      self.edit(lambda m: m.update(objects=[{"name": "a", "at": [1, 1], "sprite": "sprite:X", "elevation": 3},
                                            {"name": "b", "at": [1, 1], "sprite": "sprite:X"}]))
      self.fails("on a level that meets this one")
      self.edit(lambda m: m.update(objects=[{"name": "a", "at": [1, 1], "sprite": "sprite:X", "elevation": 3},
                                            {"name": "b", "at": [1, 1], "sprite": "sprite:X", "elevation": 4}]))
      self.s.compile()

   def test_fields(self) -> None:
      """!Sprites, movement, ranges, facing, scripts, flags and spotters are checked."""
      self.obj("guard", sprite="BLINK")
      self.fails("sprite must be a sprite asset reference")
      self.obj("guard", sprite="sprite:BLINK", movement="schedule")
      self.fails("needs the world clock")
      self.obj("guard", movement="fly")
      self.fails("movement must be one of")
      self.obj("guard", movement="wander", range=[16, 0])
      self.fails("expected an integer 0-15")
      self.obj("guard", range=None, facing="north")
      self.fails("facing must be one of")
      self.obj("guard", facing="up", script="nope")
      self.fails("unknown script 'nope'")
      self.obj("guard", script="talk_guard", flag="nope")
      self.fails("unknown flag 'nope'")
      self.obj("guard", flag=None, extra=1)
      self.fails("unknown key(s) extra")
      self.obj("guard", extra=None)
      self.obj("rival", spotter={"sight": 9})
      self.fails("expected an integer 1-8")
      self.obj("rival", spotter={"sight": 2})
      self.fails("needs its until flag")
      self.obj("rival", spotter={"sight": 2, "until": "beat_rival"}, script=None)
      self.fails("a spotter needs a script")
      self.obj("rival", script="rival_battle")
      self.s.compile()

   def test_pages(self) -> None:
      """!Pages need conditions and must change something; routes need custom movement; self switches."""
      self.obj("guard", pages=[{"facing": "up"}])
      self.fails("needs conditions")
      self.obj("guard", pages=[{"when": {}, "facing": "up"}])
      self.fails("give a flag, a var or self switches")
      self.obj("guard", pages=[{"when": {"flag": "guard_moved"}}])
      self.fails("changes nothing")
      self.obj("guard", pages=[{"when": {"self": "E"}, "facing": "up"}])
      self.fails("self takes A, B, C or D")
      self.obj("guard", pages=[{"when": {"var": "gate", "value": 300}, "facing": "up"}])
      self.fails("can never reach 300")
      self.obj("guard", pages=[{"when": {"flag": "guard_moved"}, "hidden": True, "facing": "up"}])
      self.fails("a hidden page sets nothing else")
      self.obj("guard", pages=[{"when": {"self": ["A", "C"]}, "hidden": True},
                               {"when": {"var": "gate", "value": 1, "equal": True}, "movement": "wander",
                                "range": [2, 0], "trigger": "player_touch", "speed": 4, "frequency": 5,
                                "options": {"through": True}}])
      w = self.s.compile()
      town = w["events"]["objects"]["town"]
      p0, p1 = town["pages"][0], town["pages"][1]
      self.assertEqual((p0["self_mask"], p0["fields"]), (0b0101, obc.PAGE_HIDDEN))
      self.assertEqual(p1["cond"], obc.PAGE_VAR_EQUAL)
      self.assertEqual(p1["fields"], obc.PAGE_MOVEMENT | obc.PAGE_TRIGGER | obc.PAGE_MOTION | obc.PAGE_OPTIONS)
      self.assertEqual((p1["motion"], p1["options"], p1["range"]), (4 | (5 << 4), 1 | 8, 0x02))
      self.assertEqual(town["objects"][0]["self_base"], 0)   # its pages test self switches
      self.assertEqual(town["objects"][1]["self_base"], obc.NO_SELF)
      self.assertEqual(w["events"]["self_bits"], 8)   # the guard and plaza's "sw" (its talk sets one)
      self.assertIn("uint8_t g_self_switches[1];", mc.emit("world.json", w))

   def test_routes_and_triggers(self) -> None:
      """!Custom movement and its route, triggers needing scripts."""
      self.obj("pacer", movement="custom")
      self.fails("movement 'custom' needs a route")
      self.obj("pacer", movement="wander", route={"steps": ["up"]})
      self.fails("a route needs movement 'custom'")
      self.obj("pacer", movement="custom", route={"steps": ["fly 2"]})
      self.fails("unknown step 'fly'")
      self.obj("pacer", route={"steps": ["face left"]})
      self.fails("a repeating route needs a step that takes time")
      self.obj("pacer", route={"steps": ["left 2", "wait 30", "right 2", "set ghost_gone"], "skip": True})
      w = self.s.compile()
      town = w["events"]["objects"]["town"]
      code = town["routes"][town["objects"][4]["route"]]
      self.assertEqual(code[0], 1 | 2)   # repeat (the default) and skip
      self.assertIn("const uint8_t kRouteCode3[]", mc.emit("world.json", w))
      self.obj("pacer", trigger="parallel")
      self.fails("parallel objects need a script")
      self.obj("pacer", trigger="hop", script="talk_guard")
      self.fails("trigger must be one of")

   def test_page_defaults_and_limits(self) -> None:
      """!A page's movement fields keep the object's range and route; script and Parallel checks per page."""
      self.obj("pacer", movement="wander", range=[3, 2], route=None,
               pages=[{"when": {"flag": "guard_moved"}, "movement": "look_around"}])
      town = self.s.compile()["events"]["objects"]["town"]
      self.assertEqual(town["pages"][-1]["range"], 0x23)   # the object's range, not [1, 1]
      self.obj("pacer", movement="custom", range=None, route={"steps": ["left 2", "right 2"]},
               pages=[{"when": {"flag": "guard_moved"}, "range": [2, 2]}])
      town = self.s.compile()["events"]["objects"]["town"]
      pacer = town["objects"][4]
      self.assertEqual(town["pages"][-1]["route"], pacer["route"])   # the object's own route
      self.obj("pacer", movement=None, route=None, script=None,
               pages=[{"when": {"flag": "guard_moved"}, "trigger": "autorun"}])
      self.fails("page 1: autorun objects need a script")

      def all_parallel(m: JsonDict) -> None:
         for o in m["objects"]:
            o["trigger"] = "parallel"
            o["script"] = "talk_guard"
            o.pop("spotter", None)
      self.obj("pacer", pages=None)
      self.edit(all_parallel)
      self.assertGreater(len(self.s.load("maps/town.json")["objects"]), obc.MAX_PARALLEL)
      self.fails("can be parallel")

   def test_world_ids_clash(self) -> None:
      """!Objects whose world_ids.h names clash are refused."""
      self.edit(lambda m: m.update(objects=[{"name": "a_b", "at": [1, 1], "sprite": "sprite:X"},
                                            {"name": "A_b", "at": [2, 1], "sprite": "sprite:X"}]))
      self.fails("both become OBJ_TOWN_A_B")


if __name__ == "__main__":
   unittest.main()
