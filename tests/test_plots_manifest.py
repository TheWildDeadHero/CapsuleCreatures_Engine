"""M6a K2: the world's plots (world.json "plots", tools/map_compiler.py compile_plots() / emit_plots()), the save
image's Plots section (tools/save_layout.py) and the names `plot` statements use (tools/script_compiler.py
load_world_field()), on a scratch copy of the map compiler's test world."""
from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
import map_compiler as mc  # noqa: E402
import save_layout as sl  # noqa: E402
import script_compiler as sc  # noqa: E402
from test_map_compiler import Scratch  # noqa: E402

## A soil type and a tree type, a crop of each (items of the starter battle data), three places.
PLOTS = {
   "types": [{"id": "soil", "stages": 4, "water": True, "ripe_hours": 24, "regrow": 2, "sprite": "sprite:SOIL"},
             {"id": "tree", "stages": 1, "replant": True}],
   "crops": [{"id": "drop", "item": "Mana Drop", "type": "soil", "stage_hours": 3, "yield": [2, 5],
              "sprite": "sprite:BERRY_DROP"},
             {"id": "bar", "item": 2, "type": "tree", "stage_hours": 24, "sprite": "sprite:TREE_BAR"}],
   "places": [{"map": "town", "object": 0, "type": "soil", "name": "town_soil"},
              {"map": "town", "object": "rival", "type": "tree", "start": "bar"},
              {"map": "plaza", "object": "sw", "type": "soil", "name": "plaza_soil"}]}
## The local ID of the town's "rival" and the plaza's "sw" (their order in the fixture maps).
RIVAL, SW = 1, 3


class PlotsManifestTests(unittest.TestCase):
   """The plots block: compiled, its refusals, its generated source, its save section and its script names."""

   def setUp(self) -> None:
      """Fresh scratch copy."""
      ## The scratch copy.
      self.s = Scratch()

   def tearDown(self) -> None:
      """Removes it."""
      self.s.close()

   def with_plots(self, plots: Any) -> None:
      """Gives the scratch world a plots block.
      @param plots  the block
      """
      raw = self.s.load("world.json")
      raw["plots"] = plots
      self.s.save("world.json", raw)

   def fails(self, fragment: str) -> None:
      """Asserts that compiling fails mentioning fragment.
      @param fragment  expected text
      """
      with self.assertRaises(mc.ManifestError) as ctx:
         self.s.compile()
      self.assertIn(fragment, str(ctx.exception))

   def test_none(self) -> None:
      """No plots: compiled as None, data() answers nullptr and no object look is defined."""
      w = self.s.compile()
      self.assertIsNone(w["plots"])
      src = mc.emit("world.json", w)
      self.assertIn("return nullptr;   // the world declares no plots", src)
      self.assertNotIn("object_look", src)

   def test_full(self) -> None:
      """The types, crops (an item by name and by id) and places (a local ID by number and by name)."""
      self.with_plots(PLOTS)
      w = self.s.compile()
      p = w["plots"]
      self.assertTrue(p["engine"])
      self.assertEqual([t["stages"] for t in p["types"]], [4, 1])
      self.assertEqual((p["types"][0]["water"], p["types"][1]["replant"], p["types"][1]["sprite"]), (1, 1, 0))
      self.assertEqual([(c["item"], c["type"], c["yield_min"], c["yield_max"]) for c in p["crops"]],
                       [(1, 0, 2, 5), (2, 1, 1, 1)])
      names = [mp["name"] for mp in w["maps"]]
      self.assertEqual([(q["map"], q["local"], q["type"], q["start"]) for q in p["places"]],
                       [(names.index("town"), 0, 0, 0), (names.index("town"), RIVAL, 1, 2),
                        (names.index("plaza"), SW, 0, 0)])
      self.assertEqual(p["place_names"], ["town_soil", "", "plaza_soil"])
      src = mc.emit("world.json", w)
      self.assertIn("PlotRecord g_plot_records[3];", src)
      self.assertIn("const Data kPlots = {kPlotTypes, kPlotCrops, kPlotPlaces, 2, 2, 3, g_plot_records};", src)
      self.assertIn("return plots::look(map, local, sprite, anim);", src)

   def test_game_storage(self) -> None:
      """"storage": "game": no engine records, no save section."""
      plots = dict(PLOTS, storage="game")
      self.with_plots(plots)
      src = mc.emit("world.json", self.s.compile())
      self.assertIn("kPlotPlaces, 2, 2, 3, nullptr};", src)
      self.assertNotIn("g_plot_records", src)

   def test_refusals(self) -> None:
      """Out-of-range or malformed parts name what is wrong."""
      cases = [(("types", 0, "stages"), 9, "stages"), (("types", 0, "water"), 1, "true or false"),
               (("types", 1, "id"), "soil", "distinct"), (("crops", 0, "item"), "Nothing", "no item"),
               (("crops", 0, "type"), "pot", "unknown type"), (("crops", 0, "yield"), [5, 2], "maximum"),
               (("crops", 0, "sprite"), "BERRY", "sprite asset"), (("places", 0, "map"), "moon", "unknown map"),
               (("places", 0, "object"), "nobody", "no object"), (("places", 1, "start"), "drop", "another type"),
               (("places", 2, "name"), "town_soil", "distinct"), (("places", 0, "object"), RIVAL, "map order"),
               (("storage",), "cloud", "storage"), (("types", 0, "tick"), 1, "unknown key")]
      for path, value, fragment in cases:
         plots = copy.deepcopy(PLOTS)
         at = plots
         for key in path[:-1]:
            at = at[key]
         at[path[-1]] = value
         self.with_plots(plots)
         with self.subTest(fragment=fragment, path=path):
            self.fails(fragment)

   def test_script_names(self) -> None:
      """load_world_field() gives the place names and crop ids `plot` statements resolve."""
      self.with_plots(PLOTS)
      field = sc.load_world_field(self.s.dir / "world.json")
      self.assertEqual(field["plots"], {"places": ["town_soil", "None", "plaza_soil"], "crops": ["drop", "bar"]})
      self.assertEqual(field["maps"], list(json.loads((self.s.dir / "world.json").read_text())["maps"]))   # C18


class PlotsSaveLayoutTests(unittest.TestCase):
   """The save image's Plots section in tools/save_layout.py."""

   def test_section(self) -> None:
      """The section's bytes per place, only with engine records."""
      self.assertEqual((sl.PLOTS_HEAD_BYTES, sl.PLOT_RECORD_BYTES), (2, 10))
      with tempfile.TemporaryDirectory() as tmp:
         world = Path(tmp) / "world.json"
         world.write_text(json.dumps({"plots": PLOTS}), encoding="utf-8")
         self.assertEqual(sl.world_plot_places(world), 3)
         world.write_text(json.dumps({"plots": dict(PLOTS, storage="game")}), encoding="utf-8")
         self.assertEqual(sl.world_plot_places(world), 0)
      self.assertEqual(sl.world_plot_places(None), 0)
      self.assertEqual(sl.sections_of({"inventory": {"slot_count": 0}, "layout": {"party_size": 0, "bytes": 0},
                                       "storage": None, "combine": {}}, plots=3).get("plots"), 2 + 3 * 10)


if __name__ == "__main__":
   unittest.main()
