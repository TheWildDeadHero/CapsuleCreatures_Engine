#!/usr/bin/env python3
"""!The Starter sample's contract (samples/starter, D-25, E9): the engine's neutral sample game and the studio's
"Starter" template. These checks keep the sample what new users copy from: nine stats (HP, MP and EP pools with
their own bases and growth), HP / MP / EP resources (MP and EP persist like HP; EP regenerates, MP does not),
physical moves paying EP, elemental and status moves paying MP, the free fallback Strike, Intelligence as the
effect odds stat, Agility as the order stat, nine types in a multiply chart, 8 move slots, 2 passive slots per
species, neutral texts, shape art for every creature, and a build on every platform (ctest starter_sample_tests).
"""
from __future__ import annotations

import json
import re
import sys
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import asset_compiler  # noqa: E402
import battle_compiler as bc  # noqa: E402
import effect_compiler as ec  # noqa: E402
import manifest_paths  # noqa: E402

## The sample's folder.
SAMPLE = ROOT / "samples" / "starter"
## The sample's playable app.
APP = ROOT / "apps" / "starter_battle"
## The stats, in order: (name, label).
STATS = [("hp", "HP"), ("mp", "MP"), ("ep", "EP"), ("attack", "Attack"), ("magic", "Magic"),
         ("defence", "Defence"), ("protection", "Protection"), ("intelligence", "Intelligence"),
         ("agility", "Agility")]
## The types, in order.
TYPES = ["Fire", "Wind", "Lightning", "Water", "Earth", "Darkness", "Light", "Cosmic", "Bio"]
## What each category pays with: physical EP, elemental and status MP.
CATEGORY_COSTS = {"physical": "ep", "elemental": "mp", "status": "mp"}
## The fallback move (free, weak).
FALLBACK = "Strike"
## Move slots and passive slots per species.
MOVE_SLOTS = 8
PASSIVE_SLOTS = 2
## B2: the species with the hidden talent Nurture (its third passive slot, not rolled).
NURTURE_LINE = {"Sproutle", "Sprouthorn", "Sprigshield"}
## Words the neutral sample never uses (franchise terms; the engine's check_principles.sh keeps a longer list).
FRANCHISE_WORDS = re.compile(r"\b(trainer|pok[eé]mon|pok[eé]ball|nature|ability|abilities|prize money|PP|wild)\b",
                             re.IGNORECASE)

JsonDict = dict[str, Any]


def load(name: str) -> JsonDict:
   """!One of the sample's JSON files.
   @param name  file name under samples/starter
   @return its data
   """
   return json.loads((SAMPLE / name).read_text(encoding="utf-8"))


class StructureTests(unittest.TestCase):
   """!Stats, resources, categories, chart and slots."""

   def setUp(self) -> None:
      """!Reads the structure manifest.
      @return None
      """
      self.structure = load("structure.json")

   def test_stats(self) -> None:
      """!Nine stats; HP the pool stat, Agility the order stat, Intelligence the odds stat."""
      stats = self.structure["stats"]
      self.assertEqual([(s["name"], s["label"]) for s in stats], STATS)
      flags = {key: [s["name"] for s in stats if s.get(key)] for key in ("pool", "order", "odds")}
      self.assertEqual(flags, {"pool": ["hp"], "order": ["agility"], "odds": ["intelligence"]})

   def test_resources(self) -> None:
      """!HP faints; MP and EP persist like HP; only EP regenerates; each pool is its own stat."""
      res = {r["name"]: r for r in self.structure["resources"]}
      self.assertEqual(list(res), ["hp", "mp", "ep"])
      self.assertEqual({n: r["stat"] for n, r in res.items()}, {"hp": "hp", "mp": "mp", "ep": "ep"})
      self.assertEqual({n: r.get("empty") for n, r in res.items()}, {"hp": "faint", "mp": None, "ep": None})
      self.assertTrue(all(r["scope"] == "battler" and r.get("persist") for r in res.values()))
      self.assertEqual({n: "regen" in r for n, r in res.items()}, {"hp": False, "mp": False, "ep": True})
      phases = [p["run"] for p in self.structure["turn"]["end_of_turn"]]
      self.assertIn("regen", phases)

   def test_categories_and_chart(self) -> None:
      """!Physical: Attack vs Defence; elemental: Magic vs Protection; status: no damage; a multiply chart."""
      cats = {c["name"]: (c.get("offense"), c.get("defense")) for c in self.structure["categories"]}
      self.assertEqual(cats, {"physical": ("attack", "defence"), "elemental": ("magic", "protection"),
                              "status": (None, None)})
      chart = self.structure["chart"]
      self.assertEqual(chart["combine"], "multiply")
      self.assertEqual(set(chart["matchups"]), set(TYPES))
      tiers = {t["name"] for t in chart["tiers"]}
      for row in chart["matchups"].values():
         self.assertTrue(set(row) <= set(TYPES) and set(row.values()) <= tiers)

   def test_slots_and_rules(self) -> None:
      """!8 move slots, the fallback Strike."""
      self.assertEqual(self.structure["monster_layout"]["move_slots"], MOVE_SLOTS)
      self.assertEqual(self.structure["rules"]["fallback_move"], FALLBACK)


class ContentTests(unittest.TestCase):
   """!Types, moves and their costs, species, texts, art."""

   def setUp(self) -> None:
      """!Reads the content manifest.
      @return None
      """
      self.battle = load("battle.json")

   def test_types(self) -> None:
      """!The nine types, in order."""
      self.assertEqual([t["name"] for t in self.battle["types"]], TYPES)

   def test_move_costs(self) -> None:
      """!Physical moves pay EP, elemental and status moves pay MP; Strike is free; each type has all three."""
      kinds: dict[str, set[str]] = {t: set() for t in TYPES}
      for m in self.battle["moves"]:
         cost = m.get("cost", {})
         if m["name"] == FALLBACK:
            self.assertEqual(cost, {})
            continue
         with self.subTest(move=m["name"]):
            self.assertEqual(list(cost), [CATEGORY_COSTS[m["category"]]])
            self.assertGreater(cost[CATEGORY_COSTS[m["category"]]], 0)
            self.assertEqual("power" in m, m["category"] != "status")
         kinds[m["type"]].add(m["category"])
      for ty, cats in kinds.items():
         with self.subTest(type=ty):
            self.assertEqual(cats, set(CATEGORY_COSTS))

   def test_species(self) -> None:
      """!One creature per type at least, two passive slots each (B2: Sproutle's line a third, the hidden Nurture), at
      most 8 learned moves, sprites for each."""
      assets = {a["key"] for a in load("assets.json")["assets"]}
      firsts = set()
      for sp in self.battle["species"]:
         with self.subTest(species=sp["name"]):
            nurture = sp["name"] in NURTURE_LINE
            self.assertEqual(len(sp["passives"]), PASSIVE_SLOTS + (1 if nurture else 0))
            self.assertEqual(sp["passives"][-1] == "Nurture", nurture)
            self.assertEqual(len(sp["base"]), len(STATS))
            self.assertLessEqual(len(sp["learnset"]), MOVE_SLOTS)
            self.assertTrue({sp["front"], sp["back"]} <= assets)
         firsts.add(sp["types"][0])
      self.assertEqual(firsts, set(TYPES))
      self.assertTrue({p["sprite"] for p in self.battle["presenters"]} <= assets)

   def test_neutral_texts(self) -> None:
      """!No franchise words anywhere in the sample's texts, names or logic."""
      for path in sorted(SAMPLE.rglob("*")):
         if path.suffix in (".json", ".ceffect"):
            with self.subTest(file=path.name):
               self.assertIsNone(FRANCHISE_WORDS.search(path.read_text(encoding="utf-8")))

   def test_logic_uses_odds(self) -> None:
      """!Secondary effects and status moves roll odds() (the effect_odds pipeline), never chance()."""
      moves = (SAMPLE / "effects" / "moves.ceffect").read_text(encoding="utf-8")
      self.assertIn("odds(", moves)
      self.assertNotIn("chance", re.sub(r"#[^\n]*", "", moves))


class BuildTests(unittest.TestCase):
   """!The sample compiles for every platform, with its assets, and the app uses it."""

   def test_compiles_everywhere(self) -> None:
      """!The battle data on the GBA's limits and the host's (3 battler resources fit the GBA's 4)."""
      for platform in ("gba", "headless"):
         with self.subTest(platform=platform):
            c = bc.compile_battle(SAMPLE / "battle.json", platform=platform)
            self.assertEqual(len(c["stats"]), len(STATS))
            self.assertEqual([r["name"] for r in c["resources"]], ["hp", "mp", "ep"])
            odds = c["ruleset"]["pipelines"]["effect_odds"]
            self.assertTrue(any(s["kind"] == "point" for s in odds["steps"]))
            self.assertEqual(c["layout"]["move_slots"], MOVE_SLOTS)

   def test_effect_odds_pipeline(self) -> None:
      """!The ruleset's effect_odds pipeline: equal Intelligence keeps the odds, more raises them, less lowers them."""
      c = bc.compile_battle(SAMPLE / "battle.json", platform="headless")
      bound = c["pipelines"]

      def odds(user: int, target: int) -> int:
         """!The odds of 10 in 100 between two Intelligence values (no handlers).
         @param user    the user's Intelligence
         @param target  the target's Intelligence
         @return the chance
         """
         run = bc.rc.run_pipeline(bound, "effect_odds", {"effect_chance": 10, "effect_out_of": 100,
                                                         "user_stat": user, "target_stat": target,
                                                         "user_level": 1, "target_level": 1})
         return run["outputs"]["chance"]

      self.assertEqual(odds(100, 100), 10)
      self.assertGreater(odds(200, 100), 10)
      self.assertLess(odds(100, 200), 10)

   def test_assets(self) -> None:
      """!The shape art compiles."""
      manifest = json.loads((SAMPLE / "assets.json").read_text(encoding="utf-8"))
      self.assertTrue(asset_compiler.compile_manifest(manifest, SAMPLE))

   def test_app_uses_the_sample(self) -> None:
      """!apps/starter_battle names the sample's files by relative paths."""
      project = json.loads((APP / "reference" / "project.json").read_text(encoding="utf-8"))
      for key in ("battle", "assets"):
         with self.subTest(key=key):
            self.assertFalse(manifest_paths.is_absolute_text(project[key]))   # absolute on no system (D-40 RW3)
            self.assertEqual((APP / project[key]).resolve().parent, SAMPLE.resolve())

   def test_template(self) -> None:
      """!template.json (capsule.template/1, D-23 section 3.5): its parts name every file of the sample (but its
      README), once."""
      template = load("template.json")
      self.assertEqual(template["schema"], "capsule.template/1")
      self.assertEqual(set(template["parts"]), {"structure", "logic", "content"})
      named = [f for files in template["parts"].values() for f in files]
      self.assertEqual(len(named), len(set(named)))
      # README.md documents the sample (its rules table, C2); it is not a part a game's template copies
      present = {p.relative_to(SAMPLE).as_posix() for p in SAMPLE.rglob("*") if p.is_file()} - {"template.json",
                                                                                                 "README.md"}
      self.assertEqual(set(named), present)
      battle = load("battle.json")
      self.assertEqual({battle["structure"], battle["ruleset"], *battle["effects"]} - set(named), set())

   def test_effect_logic_checks(self) -> None:
      """!The logic files format canonically (the studio's formatter leaves them as they are)."""
      for path in sorted((SAMPLE / "effects").glob("*.ceffect")):
         with self.subTest(file=path.name):
            text = path.read_text(encoding="utf-8")
            self.assertEqual(ec.format(ec.parse(text, path.name)), text)


if __name__ == "__main__":
   unittest.main()
