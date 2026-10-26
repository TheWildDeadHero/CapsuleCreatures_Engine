#!/usr/bin/env python3
"""!Tests for locations, banner styles, previews and the default warp
transition (tools/location_compiler.py through tools/map_compiler.py,
D-17), on a copy of the map test world (maps plaza and garden)."""
from __future__ import annotations

import sys
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import bitmap  # noqa: E402
import location_compiler as lc  # noqa: E402
import map_compiler as mc  # noqa: E402
from test_map_compiler import Scratch  # noqa: E402

## A parsed JSON object (a manifest).
JsonDict = dict[str, Any]


class LocationTests(unittest.TestCase):
   """!Locations compile and are checked."""

   def setUp(self) -> None:
      """!A fresh copy of the world."""
      ## The copy.
      self.s = Scratch()

   def tearDown(self) -> None:
      """!Removes it."""
      self.s.close()

   def world(self, fn: Callable[[JsonDict], object]) -> None:
      """!Changes the world manifest.
      @param fn  callable(manifest)
      """
      w = self.s.load("world.json")
      fn(w)
      self.s.save("world.json", w)

   def fails(self, text: str) -> None:
      """!Compiling must fail with a message containing text.
      @param text  expected part of the message
      """
      with self.assertRaises(mc.ManifestError) as cm:
         self.s.compile()
      self.assertIn(text, str(cm.exception))

   def test_fixture(self) -> None:
      """!The fixture: two locations, a style, a preview, maps pointing at them."""
      w = self.s.compile()
      locs = w["locations"]
      self.assertEqual([loc["name"] for loc in locs["locations"]], ["plaza", "garden"])
      self.assertEqual(locs["map_location"], {"plaza": 0, "garden": 1})
      self.assertEqual(locs["locations"][1]["rule"], 1)        # first visit
      self.assertEqual(locs["locations"][1]["count"], 1)
      self.assertEqual(locs["default_transition"], lc.TRANSITIONS.index("fade"))
      text = mc.emit("world.json", w)
      self.assertIn("const Locations kLocations = {kLocationTable, kBannerStyles, kPreviews, g_visited, 2, 1, 1, 1",
                    text)
      self.assertIn("kSequence", text.replace("kPreview0", "kSequence"))
      self.assertIn("kNoLocation}", text)   # maps without a location

   def test_frames(self) -> None:
      """!A frame image becomes 9 tiles; ink, shadow and fill take entries 1-3."""
      ink, fill, edge = (248, 248, 248, 255), (40, 72, 120, 255), (200, 40, 40, 255)
      rgba = bytearray()
      for y in range(24):
         for x in range(24):
            border = x in (0, 23) or y in (0, 23)
            rgba += bytes(edge if border else (ink if (x, y) == (12, 12) else fill))
      bitmap.write_png(self.s.dir / "frame.png", 24, 24, rgba)
      self.world(lambda w: w["banner_styles"]["box"].update(frame="frame.png", ink="#f8f8f8", fill="#284878"))
      w = self.s.compile()
      st = w["locations"]["styles"][0]
      self.assertEqual(len(st["tiles"]), 9)
      # Indexed as tiles[tile][row][column]; tile 4 is the centre of the 3x3 frame.
      self.assertEqual(st["tiles"][4][4][4], 1)    # the ink pixel at the centre
      self.assertEqual(st["tiles"][4][0][0], 3)    # fill
      self.assertEqual(st["tiles"][0][0][0], 4)    # the first other color
      bitmap.write_png(self.s.dir / "frame.png", 16, 16, bytearray(16 * 16 * 4))
      self.fails("a banner frame is 24x24")

   def test_checks(self) -> None:
      """!Unknown styles, locations and conditions; long names; bad sequences; transitions."""
      self.world(lambda w: w["locations"]["plaza"].update(banner="fancy"))
      self.fails("unknown banner style 'fancy'")
      self.world(lambda w: w["locations"]["plaza"].update(banner="box", name="x" * 29))
      self.fails("a banner fits 28")
      self.world(lambda w: w["locations"]["plaza"].update(name="Plaza"))
      self.world(lambda w: w["locations"]["garden"]["preview"]["sequences"][0].update(when="noon"))
      self.fails("unknown condition 'noon'")
      self.world(lambda w: w["locations"]["garden"]["preview"]["sequences"][0].update(when="night",
                                                                                     sequence="nope.json"))
      self.fails("cannot read")
      self.world(lambda w: w["locations"]["garden"]["preview"]["sequences"][0].update(
         sequence="sequences/preview.json"))
      w = self.s.compile()
      self.assertEqual(w["locations"]["previews"][0]["mask"], 1)   # night is condition 0
      self.world(lambda w: w.update(transition="spin"))
      self.fails("world transition")
      self.world(lambda w: w.update(transition="box"))
      m = self.s.load("maps/plaza.json")
      m["location"] = "moon"
      self.s.save("maps/plaza.json", m)
      self.fails("unknown location 'moon'")


   def test_field_placeholders(self) -> None:
      """!With placeholders declared in the scripts manifest, names are field text; without, braces are kept."""
      self.world(lambda w: w["locations"]["plaza"].update(name="{player}'s Room"))
      scripts = self.s.load("scripts/scripts.json")
      del scripts["placeholders"]   # the fixture's {hero}
      self.s.save("scripts/scripts.json", scripts)
      w = self.s.compile()
      braces = [ord(c) for c in "{player}'s Room"] + [lc.text_encoder.CODE_END]
      self.assertEqual(w["locations"]["locations"][0]["codes"], braces)   # no placeholders: plain characters
      scripts = self.s.load("scripts/scripts.json")
      scripts["placeholders"] = {"hero": {"name": 6}, "player": {"name": 7}}
      self.s.save("scripts/scripts.json", scripts)
      w = self.s.compile()
      self.assertEqual(w["locations"]["locations"][0]["codes"],
                       [lc.text_encoder.CODE_PLACEHOLDER, lc.text_encoder.FIRST_FIELD_ARG + 1]
                       + [ord(c) for c in "'s Room"] + [lc.text_encoder.CODE_END])
      # The banner fits 28 cells: the name slot counts as its 7.
      self.world(lambda w: w["locations"]["plaza"].update(name="{player}" + "x" * 21))
      self.s.compile()
      self.world(lambda w: w["locations"]["plaza"].update(name="{player}" + "x" * 22))
      self.fails("is 29 characters; a banner fits 28")
      self.world(lambda w: w["locations"]["plaza"].update(name="{rival}"))
      self.fails("unknown placeholder {rival}")


if __name__ == "__main__":
   unittest.main()
