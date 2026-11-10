"""M6a K1: the world's clock (world.json "clock", tools/map_compiler.py compile_clock() / emit_clock()) and the save
image's Clock section (tools/save_layout.py), on a scratch copy of the map compiler's test world."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
import map_compiler as mc  # noqa: E402
import save_layout as sl  # noqa: E402
from test_map_compiler import Scratch  # noqa: E402

## A clock with every part set.
CLOCK = {"periods": [4, 10, 20], "start": {"hour": 9, "weekday": 3},
         "wait": {"cooldown": "2h", "max_hours_per_use": 12},
         "seasons": {"names": ["spring", "summer", "autumn", "winter"], "mode": "calendar_month", "length_days": 30}}
## The hours "2h" is.
TWO_HOURS = 2 * 3600


class ClockManifestTests(unittest.TestCase):
   """The clock block: compiled, its defaults, its refusals, its generated source."""

   def setUp(self) -> None:
      """Fresh scratch copy."""
      ## The scratch copy.
      self.s = Scratch()

   def tearDown(self) -> None:
      """Removes it."""
      self.s.close()

   def with_clock(self, clock: object) -> None:
      """Gives the scratch world a clock block.
      @param clock  the block
      """
      raw = self.s.load("world.json")
      raw["clock"] = clock
      self.s.save("world.json", raw)

   def fails(self, fragment: str) -> None:
      """Asserts that compiling fails mentioning fragment.
      @param fragment  expected text
      """
      with self.assertRaises(mc.ManifestError) as ctx:
         self.s.compile()
      self.assertIn(fragment, str(ctx.exception))

   def test_none(self) -> None:
      """No clock: compiled as None, settings() answers nullptr and no time of day is defined."""
      w = self.s.compile()
      self.assertIsNone(w["clock"])
      src = mc.emit("world.json", w)
      self.assertIn("return nullptr;   // the world declares no clock", src)
      self.assertNotIn("time_of_day", src)

   def test_full(self) -> None:
      """Every part as written; the generated Settings and the weak time of day and minutes."""
      self.with_clock(CLOCK)
      c = self.s.compile()["clock"]
      self.assertEqual(c, {"periods": [4, 10, 20], "start_hour": 9, "start_weekday": 3, "cooldown": TWO_HOURS,
                           "max_wait": 12, "seasons": CLOCK["seasons"]["names"],
                           "mode": mc.CLOCK_SEASON_MODES.index("calendar_month"), "length_days": 30})
      src = mc.emit("world.json", self.s.compile())
      self.assertIn("constexpr uint8_t kPeriodStarts[] = {4, 10, 20};", src)
      self.assertIn(f"const Settings kWorldClock = {{kPeriodStarts, 3, 9, 3, 12, {TWO_HOURS}u, 4, "
                    "SeasonMode::CalendarMonth, 30};", src)
      self.assertIn("CAPSULE_WEAK uint8_t time_of_day()", src)
      self.assertIn("CAPSULE_WEAK uint32_t minutes()", src)

   def test_defaults(self) -> None:
      """An empty block: one period, Sunday 10:00, a 30-minute cooldown, 23 hours a Wait, one season of a week."""
      self.with_clock({})
      c = self.s.compile()["clock"]
      self.assertEqual((c["periods"], c["start_hour"], c["start_weekday"], c["cooldown"], c["max_wait"]),
                       ([0], 10, 0, 30 * 60, 23))
      self.assertEqual((c["seasons"], c["mode"], c["length_days"]), (["season"], 0, mc.CLOCK_DEFAULT_SEASON_DAYS))

   def test_durations(self) -> None:
      """Seconds, or a number with s / m / h / d."""
      for value, seconds in ((90, 90), ("45s", 45), ("15m", 900), ("3h", 10800), ("1d", 86400)):
         with self.subTest(value=value):
            self.assertEqual(mc.clock_seconds(value, "here"), seconds)
      for value in ("5w", "m", "-3m", True, 1.5):
         with self.subTest(value=value), self.assertRaises(mc.ManifestError):
            mc.clock_seconds(value, "here")

   def test_refusals(self) -> None:
      """Out-of-range or malformed parts name what is wrong."""
      for clock, fragment in (({"periods": [10, 4]}, "ascending"), ({"periods": [24]}, "start hours"),
                              ({"periods": list(range(9))}, "periods"), ({"start": {"hour": 24}}, "start hour"),
                              ({"start": {"weekday": 7}}, "start weekday"),
                              ({"wait": {"max_hours_per_use": mc.CLOCK_WAIT_WRAP}}, "max_hours_per_use"),
                              ({"wait": {"cooldown": "soon"}}, "duration"),
                              ({"seasons": {"names": []}}, "names"),
                              ({"seasons": {"names": ["a", "a"]}}, "distinct"),
                              ({"seasons": {"mode": "lunar"}}, "mode"),
                              ({"seasons": {"length_days": 0}}, "length_days"),
                              ({"tick": 1}, "unknown key"), ([], "an object")):
         self.with_clock(clock)
         with self.subTest(fragment=fragment):
            self.fails(fragment)


class ClockSaveLayoutTests(unittest.TestCase):
   """The save image's Clock section in tools/save_layout.py."""

   def test_section(self) -> None:
      """The section's bytes only with a clock; the section list keeps the engine's id order."""
      self.assertEqual(sl.SECTIONS[-2:], ("clock", "plots"))
      self.assertEqual(sl.CLOCK_BYTES, 14)
      with tempfile.TemporaryDirectory() as tmp:
         with_clock = Path(tmp) / "with.json"
         without = Path(tmp) / "without.json"
         with_clock.write_text(json.dumps({"clock": {}}), encoding="utf-8")
         without.write_text(json.dumps({}), encoding="utf-8")
         self.assertTrue(sl.world_has_clock(with_clock))
         self.assertFalse(sl.world_has_clock(without))
      self.assertFalse(sl.world_has_clock(None))


if __name__ == "__main__":
   unittest.main()
