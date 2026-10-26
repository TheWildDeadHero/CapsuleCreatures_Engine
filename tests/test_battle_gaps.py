#!/usr/bin/env python3
"""!Tests for G1 (D-26 section 1) on the compiler side: AI profiles (tools/battlec/ai.py: rules, switching rules, a
party's profile, the default one, the migration helper tools/battle_ai.py), delayed hits (tools/battlec/delayed.py),
volatiles bound to their source (`bound_to`), the ai_score pipeline and the bench's party "ai" key, on the neutral
fixture tests/data/battle_gaps and the Starter sample (run by CTest, or directly)."""
from __future__ import annotations

import contextlib
import copy
import io
import json
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_ai as migrate  # noqa: E402
import battle_compiler as bc  # noqa: E402
import bench_scenario as bs  # noqa: E402
import effect_compiler as ec  # noqa: E402
import ruleset_compiler as rc  # noqa: E402
from battlec.conditions import NO_SLOT  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## The neutral fixture of G1.
FIXTURE = ROOT / "tests/data/battle_gaps/battle.json"
## The Starter sample.
STARTER = ROOT / "samples/starter/battle.json"


def compile_with(edit: Callable[[JsonDict], None], fixture: Path = FIXTURE) -> JsonDict:
   """!Compiles a fixture after an edit of its merged view, from memory.
   @param edit     callable(battle dict)
   @param fixture  the battle manifest
   @return compiled
   """
   data = bc.view_of(fixture)
   edit(data)
   return bc.compile_battle(fixture, None, bc.sources_of(fixture, data))


class AiProfileTests(unittest.TestCase):
   """!AI profiles as manifest data."""

   def test_fixture(self) -> None:
      """!Profiles compile to rule kinds with their numbers; parties name them; a party without one has the
      default."""
      c = bc.compile_battle(FIXTURE)
      ai = c["ai"]
      self.assertEqual(ai["names"], ["basic", "hooked", "piped", "switcher", "nervous"])
      self.assertEqual(ai["default"], 0)
      hooked = ai["profiles"][1]
      self.assertEqual(hooked["rules"], [(bc.AI_RULES.index("noise"), 16, 0), (bc.AI_RULES.index("damage"), 4, 40),
                                         (bc.AI_RULES.index("hook"), 0, 0)])
      switcher = ai["profiles"][3]
      self.assertEqual(switcher["switching"], [(len(bc.AI_RULES) + bc.AI_SWITCH_RULES.index("below"), 32767, 0)])
      piped = ai["profiles"][2]
      self.assertEqual(piped["rules"][1], (bc.AI_RULES.index("pipeline"), 0, 0))
      parties = {p["ai"]: p["ai_profile"] for p in c["parties"]}
      self.assertEqual(parties["hooked"], 1)
      self.assertEqual(c["parties"][0]["ai"], "basic")   # Left names none
      text = bc.emit("battle.json", c)
      self.assertIn("kAiProfiles, kAiRules, kDelayedHits, 5, 0, 3,", text)
      self.assertIn("// nervous: hook", text)

   def test_defaults(self) -> None:
      """!A rule's numbers default to the former scorer's; a pipeline rule without veto_below never vetoes; a
      manifest without profiles gets DEFAULT_PROFILE."""
      def edit(d: JsonDict) -> None:
         d["ai_profiles"][1]["rules"] = [{"rule": r} for r in bc.AI_RULES]
      rules = compile_with(edit)["ai"]["profiles"][1]["rules"]
      self.assertEqual(rules[bc.AI_RULES.index("noise")][1:], (16, 0))
      self.assertEqual(rules[bc.AI_RULES.index("damage")][1:], (4, 40))
      self.assertEqual(rules[bc.AI_RULES.index("status")][1:], (60, 0))
      self.assertEqual(rules[bc.AI_RULES.index("status_move")][1:], (8, 0))
      self.assertEqual(rules[bc.AI_RULES.index("save_resources")][1:], (40, 0))
      self.assertEqual(rules[bc.AI_RULES.index("pipeline")][1], bc.AI_NO_VETO)

      def none(d: JsonDict) -> None:
         del d["ai_profiles"]
         for p in d["parties"]:
            p.pop("ai", None)
      ai = compile_with(none)["ai"]
      self.assertEqual(ai["names"], [bc.AI_DEFAULT_PROFILE["name"]])
      self.assertFalse(ai["given"])
      self.assertEqual(ai["profiles"][0]["rules"], [(0, 16, 0), (bc.AI_RULES.index("status_move"), 8, 0)])

   def test_refusals(self) -> None:
      """!Unknown rules, numbers and profiles, two defaults, a flag list (migration), unknown item rules and items
      (G3), a pipeline rule without the ruleset's pipeline."""
      cases: list[tuple[Callable[[JsonDict], None], str]] = [
         (lambda d: d["ai_profiles"][0]["rules"].append({"rule": "telepathy"}), "rule: one of"),
         (lambda d: d["ai_profiles"][0]["rules"].append({"rule": "noise", "max": 0}), "max"),
         (lambda d: d["ai_profiles"][0]["rules"].append({"rule": "damage", "weight": 300}), "weight"),
         (lambda d: d["ai_profiles"][0]["rules"].append({"rule": "hook", "score": 3}), "takes no numbers"),
         (lambda d: d["ai_profiles"][0].update(switching=[{"rule": "noise"}]), "one of below, hook"),
         (lambda d: d["ai_profiles"][1].update(default=True), "only one profile is the default"),
         (lambda d: d["ai_profiles"].append({"name": "basic"}), "a name of its own"),
         # G3: item rules are below_hp / hook and name an item of the battle
         (lambda d: d["ai_profiles"][0].update(items=[{"rule": "heal"}]), "one of below_hp, hook"),
         (lambda d: d["ai_profiles"][0].update(items=[{"rule": "below_hp", "item": "Nothing"}]), "an item of"),
         (lambda d: d["parties"][0].update(ai="dreamer"), "unknown AI profile"),
         (lambda d: d["parties"][0].update(ai=["damage"]), "battle_ai.py"),
      ]
      for edit, pattern in cases:
         with self.subTest(pattern=pattern):
            with self.assertRaisesRegex(bc.ManifestError, pattern):
               compile_with(edit)

   def test_pipeline_rule_needs_the_pipeline(self) -> None:
      """!A pipeline rule runs the ruleset's ai_score: the Starter's ruleset has none."""
      def edit(d: JsonDict) -> None:
         d["ai_profiles"][0]["rules"].append({"rule": "pipeline"})
      with self.assertRaisesRegex(bc.ManifestError, "ai_score pipeline"):
         compile_with(edit, STARTER)

   def test_starter_profiles(self) -> None:
      """!The Starter's profiles are the former scorer's flag sets with their numbers stated."""
      c = bc.compile_battle(STARTER)
      self.assertEqual(c["ai"]["names"], ["Basic", "Battler", "Strategist"])
      self.assertEqual(c["ai"]["names"][c["ai"]["default"]], "Basic")
      flags = {"Battler": ["damage", "avoid_useless"],
               "Strategist": ["damage", "avoid_useless", "status", "save_resources"], "Basic": []}
      for name, fl in flags.items():
         profile = c["ai"]["profiles"][c["ai"]["names"].index(name)]
         self.assertEqual([bc.AI_RULES[k] for k, _a, _b in profile["rules"]], [r["rule"] for r in bc.flag_rules(fl)])


class MigrationTests(unittest.TestCase):
   """!tools/battle_ai.py: party flags -> profiles."""

   def test_migrate(self) -> None:
      """!Each flag set becomes a profile of its rules; no flags is the default; a migrated battle is kept."""
      battle = {"schema": "capsule.battle/1", "parties": [{"name": "A", "ai": ["avoid_useless", "damage"]},
                                                          {"name": "B", "ai": []}, {"name": "C"}],
                "messages": {}}
      out, notes = migrate.migrate_battle(battle)
      self.assertEqual(list(out), ["schema", "ai_profiles", "parties", "messages"])
      self.assertEqual([p["name"] for p in out["ai_profiles"]], ["basic", "damage_avoid_useless"])
      self.assertTrue(out["ai_profiles"][0]["default"])
      self.assertEqual([r["rule"] for r in out["ai_profiles"][1]["rules"]],
                       ["noise", "damage", "avoid_useless", "status_move"])
      self.assertEqual([p.get("ai") for p in out["parties"]], ["damage_avoid_useless", None, None])
      self.assertTrue(notes)
      self.assertEqual(migrate.migrate_battle(out), (out, []))
      with self.assertRaisesRegex(bc.ManifestError, "unknown AI flag"):
         migrate.migrate_battle({"parties": [{"name": "X", "ai": ["psychic"]}]})

   def test_file(self) -> None:
      """!The command line writes the file the studio's way; --check writes nothing."""
      with tempfile.TemporaryDirectory() as tmp:
         path = Path(tmp) / "battle.json"
         path.write_text(json.dumps({"parties": [{"name": "A", "ai": ["damage"]}]}), encoding="utf-8")
         out = io.StringIO()
         with contextlib.redirect_stdout(out):
            self.assertEqual(migrate.main([str(path), "--check"]), migrate.EXIT_OK)
            self.assertNotIn("ai_profiles", json.loads(path.read_text(encoding="utf-8")))
            self.assertEqual(migrate.main([str(path)]), migrate.EXIT_OK)
         self.assertIn("-> profile 'damage'", out.getvalue())
         data = json.loads(path.read_text(encoding="utf-8"))
         self.assertEqual(data["parties"][0]["ai"], "damage")
         self.assertTrue(path.read_text(encoding="utf-8").endswith("\n"))


class GapTests(unittest.TestCase):
   """!Delayed hits, bound volatiles, the pipeline, the bench key."""

   def test_delayed_hits(self) -> None:
      """!Records compile to their snapshot bits, flags, accuracy and miss message."""
      c = bc.compile_battle(FIXTURE)
      sight, late, shaky = c["delayed_hits"]
      self.assertEqual(sight["at_use"], (1 << len(bc.DELAYED_AT_USE)) - 1)
      self.assertEqual((sight["flags"], late["flags"], late["at_use"]), (0, 1, 0))
      self.assertEqual((shaky["accuracy"], shaky["miss_message"]), (1, "failed"))
      self.assertEqual(shaky["missed"], c["names"]["messages"].index("failed"))
      self.assertEqual(c["names"]["delayed_hits"], ["sight", "late", "shaky"])
      cases: list[tuple[Callable[[JsonDict], None], str]] = [
         (lambda d: d["delayed_hits"][0].update(at_use=["mood"]), "at_use"),
         (lambda d: d["delayed_hits"][0].update(accuracy=101), "accuracy"),
         (lambda d: d["delayed_hits"][0].update(miss_message="nope"), "miss_message"),
         (lambda d: d["delayed_hits"][0].update(typed="yes"), "typed: true or false"),
         (lambda d: d["delayed_hits"].append({"name": "sight"}), "a name of its own"),
         (lambda d: d["turn"]["end_of_turn"].pop(), "delay_hit"),
      ]
      for edit, pattern in cases:
         with self.subTest(pattern=pattern):
            with self.assertRaisesRegex(bc.ManifestError, pattern):
               compile_with(edit)

   def test_bound_to(self) -> None:
      """!A bound volatile gets a source slot and VOLATILE_BOUND; others none."""
      c = bc.compile_battle(FIXTURE)
      held, decoy = c["volatiles"][0], c["volatiles"][1]
      self.assertEqual((held["source"], held["flags"] & bc.VOLATILE_BOUND), (0, bc.VOLATILE_BOUND))
      self.assertEqual((decoy["source"], decoy["flags"] & bc.VOLATILE_BOUND), (NO_SLOT, 0))

      def bad(d: JsonDict) -> None:
         d["volatiles"][1]["bound_to"] = "target"
      with self.assertRaisesRegex(bc.ManifestError, "bound_to"):
         compile_with(bad)

      def many(d: JsonDict) -> None:
         d["volatiles"] += [{"name": f"b{k}", "bound_to": "source"} for k in range(bc.MAX_BOUND)]
      with self.assertRaisesRegex(bc.ManifestError, "bound volatiles"):
         compile_with(many)

   def test_ai_score_pipeline(self) -> None:
      """!ai_score is an optional pipeline: a ruleset without it adds 0; the fixture's vetoes non-damaging moves."""
      self.assertIn("ai_score", rc.OPTIONAL_PIPELINES)
      self.assertEqual(rc.PIPELINES["ai_score"]["struct"], "AiScoreInputs")
      text = bc.emit("battle.json", bc.compile_battle(FIXTURE))
      self.assertIn("int32_t ai_score(battle::PipelineCtx& ctx, const battle::AiScoreInputs& in)", text)
      starter = bc.emit("battle.json", bc.compile_battle(STARTER))
      self.assertIn("ai_score(", starter)

   def test_hook_catalog(self) -> None:
      """!The ai_score hook is pure, move-owned too, with a block verdict; the new refs and primitives."""
      spec = ec.HOOKS[ec.HOOK["ai_score"]]
      self.assertTrue(spec["pure"])
      self.assertIn("move", spec["owners"])
      self.assertEqual(spec["results"], {"block"})
      self.assertIn("ai_score", ec.MOVE_PHASES)
      self.assertIn("absorbed", ec.GLOBAL_REFS)
      self.assertEqual(ec.PRIMITIVES["delay_hit"][-1], ("hit", "id:delayed_hits"))
      self.assertEqual(ec.PRIMITIVES["use_move"][-1], ("unless", "kwid:unless:move_flags"))

   def test_bench_ai_key(self) -> None:
      """!A bench party may name its AI profile."""
      scenario = {"schema": bs.SCHEMA, "seed": 1, "parties": [
         {"side": 0, "controller": "human", "data": "Left"},
         {"side": 1, "ai": "hooked", "members": [{"species": "Blob", "level": 5}]}]}
      self.assertEqual(bs.validate(copy.deepcopy(scenario)), [])
      scenario["parties"][1]["ai"] = 3
      self.assertTrue(bs.validate(scenario))
      self.assertIn(("ai_profiles", "ai_profiles", 0), bs.NAME_LISTS)


if __name__ == "__main__":
   unittest.main()
