#!/usr/bin/env python3
"""!Tests for the growth section of the battle compiler (D-26 section 4, package G4: tools/battlec/growth.py):
transformation rules and their triggers, forms, context values, the blocked expression, training values, the
emitted tables and ids, the effect language's `form` / `transform` / `train`, and the Python mirrors the studio's
previews use (find_change(), training_gain(), form_view()), on the neutral fixture tests/data/growth (run by CTest,
or directly)."""
from __future__ import annotations

import sys
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler as bc  # noqa: E402
import effect_compiler as ec  # noqa: E402
from battlec import growth as gr  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## The neutral growth fixture.
FIXTURE = ROOT / "tests/data/growth/battle.json"
## Its growth logic.
FIXTURE_LOGIC = ROOT / "tests/data/growth/growth.ceffect"
## The Starter sample.
STARTER = ROOT / "samples/starter/battle.json"


def compile_with(edit: Callable[[JsonDict], None] | None = None, logic: str | None = None) -> JsonDict:
   """!Compiles the fixture after an edit of its merged view (and, optionally, other growth logic), from memory.
   @param edit   callable(merged view), or None
   @param logic  replacement text of growth.ceffect, or None
   @return compiled
   """
   data = bc.view_of(FIXTURE)
   if edit is not None:
      edit(data)
   sources = bc.sources_of(FIXTURE, data)
   if logic is not None:
      sources[FIXTURE_LOGIC.resolve()] = logic
   return bc.compile_battle(FIXTURE, None, sources, platform="headless")


def species(data: JsonDict, name: str) -> JsonDict:
   """!A species of a merged view.
   @param data  the view
   @param name  its name
   @return the entry
   """
   return next(s for s in data["species"] if s["name"] == name)


def record(c: JsonDict, name: str, **roles: Any) -> JsonDict:
   """!A record dict of a species (tools/record_transfer.py unpack() form) with some roles.
   @param c      compiled
   @param name   species name
   @param roles  role values
   @return the record
   """
   key = next(s["key"] for s in c["species"] if s["name"] == name)
   return {"species_key": key, "level": 1, **roles}


class RuleTests(unittest.TestCase):
   """!Rules, triggers and their checks."""

   @classmethod
   def setUpClass(cls) -> None:
      """!Compiles the fixture once.
      @return None
      """
      ## The compiled fixture.
      cls.c = compile_with()

   def rule(self, owner: str) -> JsonDict:
      """!A rule by its owner text.
      @param owner  e.g. "Pip -> Pipster"
      @return the rule
      """
      return next(r for r in self.c["transform"]["rules"] if r["owner"] == owner)

   def test_triggers(self) -> None:
      """!`on` names a rule's triggers; without it, every trigger it can hold for (an item rule: item triggers).
      @return None
      """
      self.assertEqual(self.rule("Pip -> Pipster")["on"], ["level_up"])
      self.assertEqual(self.rule("Seedle -> Seedwood")["on"], ["trade"])
      self.assertEqual(self.rule("blocked")["on"], gr.TRIGGERS)
      # an item rule without `on`: only the triggers with an item can hold
      c = compile_with(lambda d: species(d, "Pip")["transforms"][0].pop("on"))
      rule = next(r for r in c["transform"]["rules"] if r["owner"] == "Pip -> Pipflare")
      self.assertEqual(rule["on"], list(gr.ITEM_TRIGGERS))

   def test_never_holds(self) -> None:
      """!A rule that can never hold on its triggers is an error; so is an unknown name.
      @return None
      """
      def edit(d: JsonDict) -> None:
         """!An item rule on level_up (no item there)."""
         species(d, "Pip")["transforms"][0]["on"] = ["level_up"]
      with self.assertRaisesRegex(bc.ManifestError, "can never hold on level_up"):
         compile_with(edit)
      with self.assertRaisesRegex(bc.ManifestError, "unknown name 'zzz'"):
         compile_with(lambda d: species(d, "Pip")["transforms"][1].update(when="zzz > 1"))
      with self.assertRaisesRegex(bc.ManifestError, "into: another species"):
         compile_with(lambda d: species(d, "Pip")["transforms"][1].update(into="Pip"))

   def test_section_checks(self) -> None:
      """!The structure's sections refuse what the engine cannot do.
      @return None
      """
      with self.assertRaisesRegex(bc.ManifestError, "a player cannot stop it"):
         compile_with(lambda d: d["transformation"].update(cancel=["item_use"]))
      with self.assertRaisesRegex(bc.ManifestError, "form_role: a custom role"):
         compile_with(lambda d: d["transformation"].update(form_role="level"))
      with self.assertRaisesRegex(bc.ManifestError, "the form role 'glow' must be stored"):
         compile_with(lambda d: d["transformation"].update(form_role="glow"))
      with self.assertRaisesRegex(bc.ManifestError, "gain_yield.*must cover"):
         compile_with(lambda d: d["training"]["yields"].update(mana="weight"))
      with self.assertRaisesRegex(bc.ManifestError, "not a species field"):
         compile_with(lambda d: d["training"]["yields"].update(mana="nothing"))
      with self.assertRaisesRegex(bc.ManifestError, "stat_cap"):
         compile_with(lambda d: d["training"].update(stat_cap=64))   # stat_training is 6 bits
      with self.assertRaisesRegex(bc.ManifestError, "names two values"):
         compile_with(lambda d: d["transformation"]["context_values"].append({"name": "mood",
                                                                             "values": ["night", "day"]}))

   def test_tables(self) -> None:
      """!The emitted tables and the ids.
      @return None
      """
      cpp = bc.emit(FIXTURE.name, self.c)
      self.assertIn("const GrowthData kGrowth", cpp)
      self.assertIn("bool growth_when(int32_t rule, const growth::WhenInputs& in)", cpp)
      self.assertIn("return formula_train_gain(yield, value, total, level);", cpp)
      ids = bc.emit_ids(FIXTURE.name, self.c)
      for line in ("TRIGGER_TRADE = 2", "CONTEXT_TIME_OF_DAY_NIGHT = 1", "FORM_BASE = 0", "FORM_STORM = 2"):
         self.assertIn(line, ids)
      self.assertEqual(self.c["transform"]["rederive"], ["mark"])
      # a data set without growth still defines every growth symbol (the battle modules read them)
      starter = bc.emit(STARTER.name, bc.compile_battle(STARTER, platform="headless"))
      self.assertIn("const GrowthData kGrowth", starter)
      self.assertIn("int32_t train_gain(", starter)


class MirrorTests(unittest.TestCase):
   """!The Python mirrors (the studio's previews)."""

   @classmethod
   def setUpClass(cls) -> None:
      """!Compiles the fixture once.
      @return None
      """
      ## The compiled fixture.
      cls.c = compile_with()

   def test_find_change(self) -> None:
      """!find_change() as growth::find(): the blocked expression, the rules in order, forms.
      @return None
      """
      c = self.c
      pip = record(c, "Pip", level=5)
      self.assertEqual(bc.find_change(c, pip, "level_up")["into"], "Pipster")
      self.assertIsNone(bc.find_change(c, pip, "after_battle"))
      self.assertEqual(bc.find_change(c, pip, "item_use", "Ember Stone")["into"], "Pipflare")
      self.assertEqual(bc.find_change(c, {**pip, "held_item": 10}, "level_up")["kind"], "blocked")
      moss = record(c, "Moss", bond=220)
      self.assertEqual(bc.find_change(c, moss, "level_up", context={"time_of_day": "night"})["into"], "Mossnight")
      self.assertEqual(bc.find_change(c, moss, "level_up", context={"time_of_day": "day"})["into"], "Mossday")
      wiggle = record(c, "Wiggle", level=7)
      self.assertEqual(bc.find_change(c, {**wiggle, "seed": 0x10002}, "level_up")["into"], "Wigglea")
      self.assertEqual(bc.find_change(c, {**wiggle, "seed": 0x10003}, "level_up")["into"], "Wiggleb")
      scrap = record(c, "Scrap", level=8, stat_genes={s["name"]: 7 for s in c["stats"]})
      self.assertEqual(bc.find_change(c, scrap, "level_up")["into"], "Scrapmid")
      self.assertEqual(bc.find_change(c, {**scrap, "stat_training": {"power": 20}}, "level_up")["into"], "Scrapper")
      seedle = record(c, "Seedle")
      found = bc.find_change(c, seedle, "trade", "Link Cord")
      self.assertTrue(found["takes_item"] and found["into"] == "Seedlord")
      husk = record(c, "Husk", level=6)
      self.assertEqual(bc.find_change(c, husk, "level_up")["also"], "Huskshell")
      shifter = record(c, "Shifter")
      self.assertEqual(bc.find_change(c, shifter, "after_battle", context={"region": 1})["name"], "Bright")
      self.assertIsNone(bc.find_change(c, shifter, "after_battle", context={"region": 0}))
      self.assertIsNone(bc.find_change(c, {**shifter, "form": 1}, "after_battle", context={"region": 1}))
      back = bc.find_change(c, {**shifter, "form": 1}, "after_battle", context={"time_of_day": "night"})
      self.assertEqual((back["kind"], back["form"]), ("form", 0))

   def test_forms_and_training(self) -> None:
      """!form_view() / record_stats() and training_gain() (the engine's caps and order).
      @return None
      """
      c = self.c
      shifter = next(i for i, s in enumerate(c["species"]) if s["name"] == "Shifter")
      self.assertEqual(bc.form_view(c, shifter, 1)["base"][3], 80)
      self.assertEqual(bc.form_view(c, shifter, 3), c["species"][shifter])
      plain = bc.growth_record_stats(c, record(c, "Shifter", level=5))
      bright = bc.growth_record_stats(c, record(c, "Shifter", level=5, form=1))
      self.assertGreater(bright[3], plain[3])
      self.assertEqual(bc.training_gain(c, [0] * 5, "Blob", 5), [6, 0, 0, 2, 0])
      self.assertEqual(bc.training_gain(c, [29, 0, 0, 0, 0], "Blob", 5), [30, 0, 0, 2, 0])
      self.assertEqual(bc.training_gain(c, [30, 0, 0, 9, 0], "Blob", 5), [30, 0, 0, 10, 0])   # the total cap
      self.assertEqual(bc.training_gain(c, [0] * 5, "Pip", 5), [0, 0, 0, 4, 0])


class LogicTests(unittest.TestCase):
   """!The effect language's growth primitives."""

   def test_primitives(self) -> None:
      """!form names a form name (base is 0), train a stat, transform only a subject.
      @return None
      """
      c = compile_with()
      self.assertEqual(c["names"]["forms"], ["base", "Bright", "Storm"])
      names = {name for _at, name, _v in ec.instructions(c["effects"]["bank"].code)}
      self.assertTrue({"Form", "Transform", "Train"} <= names)
      with self.assertRaises(bc.ManifestError):
         compile_with(logic=FIXTURE_LOGIC.read_text(encoding="utf-8").replace("form holder Storm",
                                                                             "form holder Cloudy"))


if __name__ == "__main__":
   unittest.main()
