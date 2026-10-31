#!/usr/bin/env python3
"""!Tests for the ruleset pipelines and points (D-20 D): tools/ruleset_compiler.py
(capsule.ruleset/2: named formulas, helpers, points, pipelines, bind(),
run_pipeline(), C++ emission) and tools/battlec/pipelines.py (point handler
forms, the legacy lowering, point_runner()), on the Starter sample.

The proof that the Python evaluator reproduces the engine line by line over the
WP-2 damage and preview grids moved with those goldens to the reference-ruleset
repo (classic/tests/test_classic_pipelines.py, E11)."""
from __future__ import annotations

import json
import sys
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler as bc  # noqa: E402
import ruleset_compiler as rc  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## The Starter sample.
FIXTURE = ROOT / "samples/starter/battle.json"
## Its ruleset.
RULESET = ROOT / "samples/starter/ruleset.json"
## Highest damage roll of the Starter sample (its ROLL_SPAN 17, less one; C2).
ROLL_HIGH = 16
## A neutral type tier's Q8 factor (x1).
EVEN_Q8 = 256
def ruleset_data() -> JsonDict:
   """!The fixture ruleset (editor data dropped).
   @return dict
   """
   data = json.loads(RULESET.read_text(encoding="utf-8"))
   data.pop("editor", None)
   return data


class RulesetPipelineTests(unittest.TestCase):
   """!capsule.ruleset/2: loading, analysis, evaluation, emission."""

   def test_schema_and_sections(self) -> None:
      """!The fixture loads and binds; the schema is /2; every pipeline is required."""
      c = rc.load(ruleset_data())
      self.assertEqual(list(c["pipelines"]), list(rc.PIPELINES))
      self.assertEqual([p["id"] for p in c["points"].values()], list(range(len(c["points"]))))
      self.assertIn("damage_base", rc.named_formulas(c))
      old = ruleset_data()
      old["schema"] = "capsule.ruleset/1"
      with self.assertRaisesRegex(rc.RulesetError, "capsule.ruleset/2"):
         rc.load(old)
      d = ruleset_data()
      del d["pipelines"]["crit"]
      with self.assertRaisesRegex(rc.RulesetError, "missing pipeline"):
         rc.load(d)
      d = ruleset_data()
      d["constants"]["STAT_CAP"] = 5
      with self.assertRaisesRegex(rc.RulesetError, "battle rules"):
         rc.load(d)

   def test_steps_are_checked(self) -> None:
      """!Steps: one kind each, known points, variables before use, outputs set."""
      def broken(edit: Callable[[JsonDict], None], pattern: str) -> None:
         d = ruleset_data()
         edit(d)
         with self.assertRaisesRegex(rc.RulesetError, pattern):
            rc.bind(rc.load(d))
      broken(lambda d: d["pipelines"]["speed"].append({"point": "nowhere", "on": "speed"}), "unknown point")
      broken(lambda d: d["pipelines"]["speed"].insert(0, {"point": "speed", "on": "speed_raw"}), "not an input")
      broken(lambda d: d["pipelines"]["speed"].insert(0, {"let": "x", "expr": "later + 1"}), "unknown name")
      broken(lambda d: d["pipelines"]["speed"].clear(), "output 'speed'")
      broken(lambda d: d["pipelines"]["speed"].append({"let": "speed", "roll": "x", "expr": "1"}), "a step is")
      broken(lambda d: d["pipelines"]["crit"].append({"let": "chance", "expr": "0 - 1"}), "output chance")
      broken(lambda d: d["pipelines"]["damage"].append({"roll": "r", "span": "100000"}), "span can reach")

   def test_named_formula_arguments(self) -> None:
      """!A call's arguments must lie inside the formula's declared input ranges (P-18)."""
      d = ruleset_data()
      d["inputs"]["power"] = [1, 50]
      with self.assertRaisesRegex(rc.RulesetError, "argument power can reach .* outside its declared range"):
         rc.bind(rc.load(d))
      d = ruleset_data()
      d["formulas"]["twice"] = {"args": ["level"], "expr": "level * 2", "range": [0, 200]}
      d["pipelines"]["speed"].insert(0, {"let": "t", "expr": "twice(1)"})
      b = rc.bind(rc.load(d))
      self.assertEqual(b["pipelines"]["speed"]["steps"][0]["bounds"], (2, 2))
      with self.assertRaisesRegex(rc.RulesetError, "not a function name"):
         d["formulas"]["min"] = d["formulas"].pop("twice")
         rc.load(d)

   def test_helpers(self) -> None:
      """!Helpers read the battle's tables; mulq8 / pow2 need non-negative values."""
      stat = {"kind": "stat", "range": (-6, 6), "table": [(2, 8)] * 6 + [(2, 2)] + [(4, 2)] * 6, "odds": None,
              "formula": None}
      crit = {"kind": "crit", "range": (0, 4), "table": None, "formula": None,
              "odds": [(1, 16), (1, 8), (1, 4), (1, 3), (1, 2)]}
      battle = {**rc.DEFAULT_BATTLE, "stages": [stat, crit], "stat_stage": 0, "accuracy_stage": None,
                "crit_stage": 1}
      env = {"battle": battle, "formulas": {}}

      def ev(text: str, **v: int) -> int:
         return rc.evaluate(rc.parse(text, list(v), {}, "t", rc.HELPERS), v, {}, env)
      self.assertEqual(ev("staged(100, 0 - 9)"), 25)      # stage clamped to -6
      self.assertEqual(ev("staged(100, 3)"), 200)
      self.assertEqual(ev("crit_chance(9) + crit_out_of(9)"), 3)   # stage clamped to 4: 1/2
      self.assertEqual(ev("mulq8(100, 384) + pow2(10, 0 - 1) + pow2(1, 20)"), 150 + 5 + 256)
      tree = rc.parse("mulq8(x, 384)", ["x"], {}, "t", rc.HELPERS)
      with self.assertRaisesRegex(rc.RulesetError, "can be negative"):
         rc.interval(tree, {"x": (-1, 5)}, {}, "t", [], env)
      tree = rc.parse("staged(x, s)", ["x", "s"], {}, "t", rc.HELPERS)
      self.assertEqual(rc.interval(tree, {"x": (10, 100), "s": (-1, 1)}, {}, "t", [], env), (2, 200))

   def test_run_pipeline(self) -> None:
      """!Rolls: a fixed roll is clamped below the span, a callable draws; points see the value and are clamped."""
      b = rc.bind(rc.load(ruleset_data()))
      inputs = {"level": 50, "move_power": 80, "offense_raw": 100, "offense_stage": 0, "defense_raw": 90,
                "defense_stage": 0, "crit": 0, "spread": 0, "same_type": 0, "type_exp": 0}
      low = rc.run_pipeline(b, "damage", inputs, roll=0)["outputs"]["damage"]
      high = rc.run_pipeline(b, "damage", inputs, roll=ROLL_HIGH)["outputs"]["damage"]
      self.assertEqual(rc.run_pipeline(b, "damage", inputs, roll=99)["outputs"]["damage"], high)
      spans = []
      rc.run_pipeline(b, "damage", inputs, roll=lambda span: spans.append(span) or 0)
      self.assertEqual(spans, [ROLL_HIGH + 1])
      self.assertLess(low, high)
      seen = []

      def point(name: str, v: int) -> int | None:
         seen.append(name)
         return 10 ** 6 if name == "power" else None
      out = rc.run_pipeline(b, "damage", inputs, point=point)
      self.assertEqual(seen, ["offense", "power", "weather", "damage", "guard"])
      self.assertEqual(out["vars"]["power"], 1023)   # the point's range clamps what the handlers give
      self.assertEqual(out["trace"][0], (0, "let", "attack", 100))

   def test_staged_stats_pass_the_stat_cap(self) -> None:
      """!The Starter's staged stats run up to STAGED_CAP (the stat cap x the top stage's x2, C4), not the stat cap:
      at the top end +3 and +4 still differ, for the damage pipeline's attack and the speed pipeline."""
      c = bc.compile_battle(FIXTURE)
      bound = c["pipelines"]
      cap = c["rules"]["stat_cap"]
      staged = rc.load(ruleset_data())["constants"]["STAGED_CAP"]
      stage_id = next(st["stage"] for st in c["stats"] if st["name"] == "attack")
      speed_id = next(st["stage"] for st in c["stats"] if st["order"])
      attack = []
      speed = []
      for stage in (3, 4):
         inputs = {"level": 50, "move_power": 80, "offense_raw": cap, "offense_stage": stage, "defense_raw": cap,
                   "defense_stage": 0, "crit": 0, "spread": 0, "same_type": 0, "type_exp": 0,
                   "offense_stage_id": stage_id, "defense_stage_id": stage_id}
         attack.append(rc.run_pipeline(bound, "damage", inputs, roll=0, types=[EVEN_Q8])["vars"]["attack"])
         speed.append(rc.run_pipeline(bound, "speed", {"speed_raw": cap, "speed_stage": stage,
                                                       "speed_stage_id": speed_id})["outputs"]["speed"])
      self.assertGreater(attack[0], cap)
      self.assertLess(attack[0], attack[1])
      self.assertEqual(attack[1], staged)
      self.assertLess(speed[0], speed[1])
      self.assertEqual(speed[1], staged)

   def test_cpp_emission(self) -> None:
      """!Pipelines are straight-line C++ with the point ids; the named formula is file-local."""
      b = rc.bind(rc.load(ruleset_data()))
      text = "\n".join(rc.emit_functions(b) + rc.emit_pipelines(b))
      self.assertIn("int32_t damage(battle::PipelineCtx& ctx, const battle::DamageInputs& in) {", text)
      self.assertIn("battle::ChanceResult crit(battle::PipelineCtx& ctx, const battle::CritInputs& in) {", text)
      self.assertIn("l_attack = battle::run_point(ctx, kPointOffense, l_attack);", text)
      self.assertIn("int32_t l_r = battle::pipeline_roll(ctx, int32_t{17});", text)
      self.assertIn("[[maybe_unused]] int32_t formula_damage_base(int32_t level", text)
      self.assertIn("constexpr uint8_t kPointSpeed = 5;", text)
      self.assertNotIn("STAT_CAP", "\n".join(x for x in text.splitlines() if not x.lstrip().startswith("//")))


class PointHandlerTests(unittest.TestCase):
   """!Point handlers (.ceffect members and manifest code) and the Python runner."""

   def compile_with(self, edit: Callable[[JsonDict], None]) -> JsonDict:
      """!Compiles the fixture after an edit, from memory.
      @param edit  callable(battle dict: the merged view of battle.json and structure.json)
      @return compiled
      """
      data = bc.view_of(FIXTURE)
      edit(data)
      return bc.compile_battle(FIXTURE, None, bc.sources_of(FIXTURE, data))

   def test_point_order(self) -> None:
      """!The sample's point handlers (its .ceffect `point` members since E4c) in table order: passives, items,
      statuses, weathers, each owner's in source order."""
      c = bc.compile_battle(FIXTURE)
      labels = [e["label"] for e in c["point_handlers"]]
      self.assertEqual(labels, ["passive Blaze Heart: point power", "passive Thick Hide: point guard",
                                "passive Insight: point odds", "passive Stubborn: point resist",
                                "passive Quick Feet: point speed", "item Focus Lens: point odds",
                                "status Scorch: point damage", "status Shock: point speed",
                                "weather Storm: point weather", "weather Storm: point weather"])
      storm = c["weathers"][c["names"]["weathers"].index("Storm")]
      self.assertEqual(storm["point_mask"], 1 << c["pipelines"]["points"]["weather"]["id"])

   def test_manifest_code(self) -> None:
      """!{"point", "code"} handlers in the manifest come before the owner's .ceffect points."""
      def edit(d: JsonDict) -> None:
         d["passives"][0]["handlers"] = [
            {"point": "power", "code": "when move.type == \"Fire\" { modify add 10; }"},
            {"point": "power", "code": "when field.weather == Storm { modify set 77; }"}]
         d["statuses"][0]["handlers"] = [{"point": "speed", "code": "{ modify by 0.25; }"}]
      c = self.compile_with(edit)
      blaze = c["passives"][0]
      hooks = [h["hook"] for h in c["handlers"][blaze["first"]:blaze["first"] + blaze["count"]]]
      power = bc.POINT_BASE + c["pipelines"]["points"]["power"]["id"]
      self.assertEqual(hooks, [power, power, power])   # two from the manifest, then Blaze Heart's own
      scorch = c["statuses"][1]   # compiled statuses are 1-based (0: none)
      self.assertEqual(scorch["handler_count"], 2)   # the manifest's speed point, then Scorch's own damage point
      user = {"hp": 10, "max_hp": 10, "passive": 0, "item": 0, "status": 1, "types": [0]}
      claw = c["names"]["moves"].index("Flare Claw")
      storm = c["names"]["weathers"].index("Storm")
      run = bc.point_runner(c, user, None, move=claw, weather=storm)
      self.assertEqual(run("power", 40), 77)       # +10, then set 77 in the storm (full HP: Blaze Heart stays)
      self.assertEqual(run("speed", 100), 25)
      self.assertIsNone(run("offense", 5))         # no handler: the value stays (None)

   def test_errors(self) -> None:
      """!Unknown points, sources a point lacks, draws and bad code are refused."""
      def refused(edit: Callable[[JsonDict], None], pattern: str) -> None:
         with self.assertRaisesRegex(bc.ManifestError, pattern):
            self.compile_with(edit)

      def handler(h: JsonDict) -> Callable[[JsonDict], None]:
         def edit(d: JsonDict) -> None:
            d["passives"][0]["handlers"] = [h]
         return edit
      refused(handler({"point": "nowhere", "code": "{ modify by 2; }"}), "unknown point")
      refused(handler({"point": "weather", "code": "{ modify by 2; }"}), "no passive source")
      refused(handler({"point": "power", "code": "{ modify by 2; inflict holder Burn; }"}), "point")
      refused(handler({"point": "power", "code": "when chance(1, 2) { modify by 2; }"}), "draws")
      refused(handler({"point": "power", "effects": [{"do": "modify", "by": 2}]}), "unknown key")
      refused(handler({"point": "power"}), "code must be effect-language text")


if __name__ == "__main__":
   unittest.main()
