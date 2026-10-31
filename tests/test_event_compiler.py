#!/usr/bin/env python3
"""!Tests for map events (tools/event_compiler.py through tools/map_compiler.py):
warps, triggers, signs, area roots and step hooks (B-3), on a copy of the
map test world."""
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
from test_map_compiler import Scratch  # noqa: E402

## A parsed JSON object (a manifest).
JsonDict = dict[str, Any]


class EventTests(unittest.TestCase):
   """!Events compile, are bucketed per chunk and are checked."""

   def setUp(self) -> None:
      """!A fresh copy of the world."""
      ## The copy.
      self.s = Scratch()

   def tearDown(self) -> None:
      """!Removes it."""
      self.s.close()

   def edit(self, name: str, fn: Callable[[JsonDict], object]) -> None:
      """!Changes one map manifest.
      @param name  map name
      @param fn    callable(manifest)
      """
      m = self.s.load(f"maps/{name}.json")
      fn(m)
      self.s.save(f"maps/{name}.json", m)

   def fails(self, text: str) -> None:
      """!Compiling must fail with a message containing text.
      @param text  expected part of the message
      """
      with self.assertRaises(mc.ManifestError) as cm:
         self.s.compile()
      self.assertIn(text, str(cm.exception))

   def test_fixture(self) -> None:
      """!The fixture's events: buckets, landings, areas, hooks, IDs header."""
      w = self.s.compile()
      ev = w["events"]["events"]
      field = ev["field"]
      self.assertEqual(field["area"], 0)
      self.assertEqual(ev["small"]["area"], 0)
      self.assertEqual(ev["stack"]["area"], 2)
      self.assertEqual(field["warps"], [(1, -4, -4), (2, 1, 1)])
      self.assertEqual(field["ranges"], {2: (0, 4)})   # all in chunk (0,1), the third in directory order
      self.assertEqual([e[1] for e in field["events"]], [0, 1, 2, 0])   # warp, trigger, sign, warp by position
      self.assertEqual(w["events"]["hooks"], [("every_step", 1, 0), ("even", 2, 0), ("four", 4, 1)])
      ids = evc.emit_ids("world.json", w["map_names"], w["events"]["hooks"])
      self.assertIn("MAP_STACK = 2", ids)
      self.assertIn("STEP_HOOK_FOUR = 2", ids)
      text = mc.emit("world.json", w)
      self.assertIn("const Event kEvents0[]", text)
      # F2: three vehicle classes; I2: 8 maps (fixed_a, fixed_b) and 6 behaviors (fence_east); I3: 9 maps (crowd),
      # 7 behaviors (counter_top), 4 vehicle classes (trek)
      self.assertIn("kStepHooks, g_self_switches, 9, 7, 1, 4, 3", text)

   def test_warps(self) -> None:
      """!Unknown maps and warps, unpainted landings, press warps without a direction."""
      self.edit("field", lambda m: m["events"]["warps"][0].update(to="nowhere"))
      self.fails("to must name a map")
      self.edit("field", lambda m: m["events"]["warps"][0].update(to="small", warp=3))
      self.fails("has 1 warp(s); there is no warp 3")
      self.edit("field", lambda m: m["events"]["warps"][0].update(warp=0, trigger="press"))
      self.fails("needs a direction")
      self.edit("field", lambda m: m["events"]["warps"][0].update(trigger="step", at=[7, 6]))
      self.fails("is not painted")
      # I3: a map with unpainted_events keeps such events (and landings on its unpainted cells)
      self.edit("field", lambda m: m.update(unpainted_events="yes"))
      self.fails("unpainted_events must be true or false")
      self.edit("field", lambda m: m.update(unpainted_events=True))
      self.assertIn("field", self.s.compile()["events"]["events"])

   def test_triggers_and_signs(self) -> None:
      """!Scripts, variables and flags are resolved and checked against the scripts manifest."""
      self.edit("field", lambda m: m["events"]["triggers"][0].update(script="nope"))
      self.fails("unknown script 'nope'")
      self.edit("field", lambda m: m["events"]["triggers"][0].update(script="step_on", value=300))
      self.fails("outside gate's range 0..255; it would never fire")
      self.edit("field", lambda m: m["events"]["triggers"][0].update(value=0, var="missing"))
      self.fails("unknown variable 'missing'")
      self.edit("field", lambda m: m["events"]["triggers"][0].update(var="gate"))
      self.edit("field", lambda m: m["events"]["signs"][0].update(flag="nope"))
      self.fails("unknown flag 'nope'")
      self.edit("field", lambda m: m["events"]["signs"][0].update(flag="sign_gone", facing="north"))
      self.fails("facing must be any")
      self.edit("field", lambda m: m["events"]["signs"][0].update(facing="down"))
      # Triggers (and signs) may share a cell: the first whose condition holds fires.
      self.edit("field", lambda m: m["events"]["triggers"].append(dict(m["events"]["triggers"][0], value=1)))
      self.assertEqual(len(self.s.compile()["events"]["events"]["field"]["events"]), 5)
      self.edit("field", lambda m: m["events"]["warps"].append(dict(m["events"]["warps"][0])))
      self.fails("another warp on 3,9")

   def test_scripts_manifest(self) -> None:
      """!Events need the world's scripts manifest; the game's must be the same file."""
      world = self.s.load("world.json")
      del world["scripts"]
      self.s.save("world.json", world)
      self.fails('name the scripts manifest in the world ("scripts")')
      w = mc.compile_world(self.s.dir / "world.json", scripts_path=self.s.dir / "scripts" / "scripts.json")
      self.assertEqual(len(w["events"]["events"]["field"]["events"]), 4)
      world["scripts"] = "scripts/scripts.json"
      self.s.save("world.json", world)
      with self.assertRaises(mc.ManifestError) as cm:
         mc.compile_world(self.s.dir / "world.json", scripts_path=self.s.root / "other.json")
      self.assertIn("must be the same file", str(cm.exception))

   def test_areas_and_hooks(self) -> None:
      """!Area roots must be outdoor maps; step hook periods are powers of two."""
      self.edit("stack", lambda m: m.update(area="small"))
      self.fails("is itself in area 'field'")
      self.edit("stack", lambda m: m.update(area="nowhere"))
      self.fails("area 'nowhere' is not a map")
      self.edit("stack", lambda m: m.pop("area"))
      world = self.s.load("world.json")
      world["step_hooks"] = [{"name": "odd", "period": 3}]
      self.s.save("world.json", world)
      self.fails("not a power of two")
      world["step_hooks"] = [{"name": "a", "period": 8}, {"name": "b", "period": 8}, {"name": "c", "period": 8}]
      self.s.save("world.json", world)
      self.assertEqual([h[2] for h in self.s.compile()["events"]["hooks"]], [0, 1, 2])   # spread
      world["step_hooks"] = [{"name": "a", "period": 4}, {"name": "b", "period": 4, "phase": 0}]
      self.s.save("world.json", world)
      self.assertEqual([h[2] for h in self.s.compile()["events"]["hooks"]], [1, 0])   # spread around explicit ones

   def test_world_ids_names(self) -> None:
      """!Map names that are not identifiers become MAP_ names with _; clashes are errors."""
      self.assertIn("MAP_ROUTE_1 = 0", evc.emit_ids("w", ["route-1"], []))
      with self.assertRaises(evc.EventError):
         evc.emit_ids("w", ["route-1", "route_1"], [])


if __name__ == "__main__":
   unittest.main()
