#!/usr/bin/env python3
"""!Tests for tools/ruleset_compiler.py and tools/battle_compiler.py (run by
CTest, or directly)."""
from __future__ import annotations

import copy
import json
import re
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler as bc  # noqa: E402
import build_settings as bs  # noqa: E402
import effect_compiler as ec  # noqa: E402
import ruleset_compiler as rc  # noqa: E402

## A parsed JSON object (battle data, a ruleset, or compiled output).
JsonDict = dict[str, Any]

## The Starter sample (the engine's neutral sample game).
FIXTURE = ROOT / "samples/starter/battle.json"
## Its ruleset.
RULESET = ROOT / "samples/starter/ruleset.json"
## The step of its damage pipeline that scales by the type chart (C2: a roll follows it, so the chart's effect is
## read there, before the roll).
TYPE_SCALE_STEP = next(i for i, step in enumerate(json.loads(RULESET.read_text(encoding="utf-8"))["pipelines"]
                                                   ["damage"]) if step.get("expr") == "type_scale(d)")
## The three-stat test battle data (tests/battle_stats3_tests.cpp runs it).
STATS3 = ROOT / "tests/data/battle_stats3/battle.json"
## Stage and chart test data (E6a).
TIERS_FIXTURE = ROOT / "tests/data/battle_tiers/battle.json"
## Turn structure test data (E6b).
TURN_FIXTURE = ROOT / "tests/data/battle_turn/battle.json"
## Presenter test data (motions, a presenter sequence, a two-presenter party; E11).
PRESENTERS = ROOT / "tests/data/battle_presenters/battle.json"
## A three-position format with adjacent reach (layout of a GBA screen), added to the Starter in memory.
TRIPLE_FORMAT = {"name": "Triple", "sides": [[0, 1, 2], [3, 4, 5]], "range": "adjacent",
                 "layout": [[32, 96], [76, 90], [120, 84], [212, 56], [168, 50], [124, 44]], "party_max": 6,
                 "on_target_fainted": "fail"}


def ruleset(**formulas: str) -> JsonDict:
   """!The fixture ruleset with some formulas (slots or stat formulas) replaced.
   @param formulas  slot or stat formula name -> expression
   @return dict
   """
   data = json.loads(RULESET.read_text(encoding="utf-8"))
   data.pop("editor", None)
   for slot, expr in formulas.items():
      section = "formulas" if slot in data["formulas"] else "stat_formulas"
      data[section][slot]["expr"] = expr
   return data


def write_split(path: Path, view: JsonDict) -> None:
   """!Writes a merged view as the battle manifest and the structure manifest it names (E5s).
   @param path  the battle.json to write (the structure file goes next to it)
   @param view  a merged view (bc.view_of(), edited)
   @return None
   """
   for p, d in bc.sources_of(path, view).items():
      p.write_text(json.dumps(d), encoding="utf-8")


class RulesetTests(unittest.TestCase):
   """!Expressions, interval analysis, semantics."""

   def test_fixture_loads(self) -> None:
      """!The fixture compiles; clamps are reported."""
      c = rc.load(ruleset())
      self.assertFalse(c["stat_formulas"]["vital"]["clamp_active"])
      self.assertTrue(c["formulas"]["damage_base"]["clamp_active"])
      self.assertEqual(rc.run(c, "damage_base", level=10, power=40, attack=20, defense=15), 10)

   def test_overflow_is_refused(self) -> None:
      """!A product that can leave int32 fails the build (P-18)."""
      with self.assertRaisesRegex(rc.RulesetError, "outside int32"):
         rc.load(ruleset(damage_base="level * power * attack * defense * 1000"))

   def test_division_by_possible_zero(self) -> None:
      """!A divisor range containing 0 fails; max() guards pass."""
      with self.assertRaisesRegex(rc.RulesetError, "contains 0"):
         rc.load(ruleset(escape_odds="speed / (foe_speed - 1)"))
      rc.load(ruleset(escape_odds="speed / max(foe_speed - 1, 1)"))

   def test_isqrt_negative(self) -> None:
      """!isqrt of a possibly negative value fails."""
      with self.assertRaisesRegex(rc.RulesetError, "negative"):
         rc.load(ruleset(escape_odds="isqrt(speed - foe_speed)"))

   def test_names_and_slots(self) -> None:
      """!Unknown names, functions and missing slots fail."""
      with self.assertRaisesRegex(rc.RulesetError, "unknown name"):
         rc.load(ruleset(vital="base + bogus"))
      with self.assertRaisesRegex(rc.RulesetError, "unknown function"):
         rc.load(ruleset(vital="pow(base, 2)"))
      with self.assertRaisesRegex(rc.RulesetError, "not allowed"):
         rc.load(ruleset(vital="base + 1.5"))
      data = ruleset()
      del data["formulas"]["exp_gain"]
      with self.assertRaisesRegex(rc.RulesetError, "missing formula"):
         rc.load(data)

   def test_cpp_semantics(self) -> None:
      """!Division and remainder truncate toward zero; precedence; functions."""
      def ev(text: str, **v: int) -> int:
         return rc.evaluate(rc.parse(text, list(v), {"K": 3}, "t"), v, {"K": 3})
      self.assertEqual(ev("0 - 7 / 2"), -3)
      self.assertEqual(ev("(0 - 7) / 2"), -3)
      self.assertEqual(ev("(0 - 7) % 2"), -1)
      self.assertEqual(ev("1 + 2 * K"), 7)
      self.assertEqual(ev("if(x > 2 && x < 5, 10, 20)", x=3), 10)
      self.assertEqual(ev("clamp(x, 0, 5) + isqrt(17) + isqrt_ceil(17) + abs(0 - 4)", x=9), 5 + 4 + 5 + 4)
      self.assertEqual(ev("!x + (x == 9)", x=9), 1)

   def test_interval_is_sound(self) -> None:
      """!Every evaluated value lies inside the analysed bounds."""
      c = rc.load(ruleset())
      f = c["formulas"]["damage_base"]
      lo, hi = f["bounds"]
      for level in (1, 25, 50):            # the Starter's level input [1, 50] (C2)
         for power in (1, 1023):
            for atk in (1, 700):         # its attack / defense inputs [1, 700]
               for df in (1, 700):
                  v = rc.evaluate(f["tree"], {"level": level, "power": power, "attack": atk, "defense": df},
                                  c["constants"])
                  self.assertTrue(lo <= v <= hi)

   def test_growth_table(self) -> None:
      """!Growth tables start at 0, never decrease and are clamped."""
      t = rc.growth_table("n*n*n - 1000", {}, 20, "g", 0, 5000)
      self.assertEqual(t[1], 0)
      self.assertEqual(t[2], 0)                     # negative values clamp to 0
      self.assertEqual(t[20], 5000)                 # clamped at the top
      self.assertTrue(all(a <= b for a, b in zip(t[1:], t[2:])))

   def test_cpp_emission(self) -> None:
      """!Generated functions clamp to the declared range."""
      text = "\n".join(rc.emit_functions(rc.load(ruleset())))
      self.assertIn("int32_t stat_formula_vital(int32_t base, int32_t gene", text)
      self.assertIn("rules::clamp32(", text)
      dispatch = "\n".join(rc.emit_stat_dispatch(["vital", "core"]))
      self.assertIn("int32_t stat(int32_t index, int32_t base", dispatch)
      self.assertIn("case 1: return stat_formula_core(", dispatch)

   def test_stat_formulas(self) -> None:
      """!Stat formulas are named identifiers with the stat inputs; slots and stat formulas evaluate alike."""
      c = rc.load(ruleset())
      self.assertEqual(rc.run(c, "vital", base=39, gene=0, training=0, awaken=0, level=5, trait_pct=100), 22)
      for bad in ("Stat", "reward", "9x"):
         with self.subTest(name=bad):
            data = ruleset()
            data["stat_formulas"][bad] = data["stat_formulas"]["vital"]
            with self.assertRaisesRegex(rc.RulesetError, "lower-case identifier"):
               rc.load(data)
      data = ruleset()
      data["formulas"]["vital"] = data["stat_formulas"]["vital"]
      with self.assertRaisesRegex(rc.RulesetError, "unknown formula slot"):
         rc.load(data)


class BattleTests(unittest.TestCase):
   """!Battle data compilation."""

   def compile_with(self, edit: Callable[[JsonDict], None], fixture: Path = FIXTURE) -> JsonDict:
      """!Compiles a fixture after an edit, from memory.
      @param edit     callable(battle dict)
      @param fixture  the battle manifest (default: the Starter sample)
      @return compiled
      """
      data = bc.view_of(fixture)
      edit(data)
      return bc.compile_battle(fixture, None, bc.sources_of(fixture, data))

   def test_fixture(self) -> None:
      """!The fixture compiles and emits."""
      c = bc.compile_battle(FIXTURE)
      self.assertEqual(len(c["types"]), 9)
      self.assertEqual(len(c["statuses"]), 5)   # + none
      # + none (G2: the bag's supplies, the orb, the key item; G4: 3 charms; F1: the Ward charm)
      self.assertEqual(len(c["items"]), 14)
      self.assertEqual(c["layout"]["bytes"], FIXTURE_RECORD_BYTES)
      keys = [s["key"] for s in c["species"]]
      self.assertEqual(keys, sorted(keys))
      nt = len(c["types"])
      fire, bio, lightning, earth = (c["names"]["types"].index(t) for t in ("Fire", "Bio", "Lightning", "Earth"))
      tiers = [t["name"] for t in c["chart"]["tiers"]]
      self.assertEqual(c["chart"]["matchups"][fire * nt + bio], tiers.index("strong"))
      self.assertEqual(c["chart"]["matchups"][lightning * nt + earth], tiers.index("none"))
      cpp = bc.emit("battle.json", c)
      self.assertIn("const BattleData kData", cpp)
      self.assertIn("const Layout kLayout", cpp)
      ids = bc.emit_ids("battle.json", c)
      self.assertIn("SPECIES_EMBERKIT", ids)
      self.assertIn("NUM_PASSIVES = 10", ids)   # B2: Nurture
      self.assertIn("PRESENTER_COACH = 0", ids)
      self.assertIn("CATEGORY_STATUS = 2", ids)
      self.assertEqual(bc.preview_stats(c, "Emberkit", 5), [25, 12, 14, 14, 14, 11, 10, 12, 14])

   def test_triple_adjacency(self) -> None:
      """!Adjacent range: the flanks don't reach the far foe."""
      c = self.compile_with(lambda d: d["formats"].append(copy.deepcopy(TRIPLE_FORMAT)))
      triple = c["formats"][c["names"]["formats"].index("Triple")]
      # Reach is a bit mask of the battle positions a slot can target.
      self.assertEqual(triple["reach"][0], (1 << 1) | (1 << 4) | (1 << 5))
      self.assertEqual(triple["reach"][3], (1 << 4) | (1 << 1) | (1 << 2))

   def test_unknown_key(self) -> None:
      """!Unknown keys are build errors (P-1)."""
      def edit(d: JsonDict) -> None:
         d["moves"][0]["speed"] = 3
      with self.assertRaisesRegex(bc.ManifestError, "unknown key"):
         self.compile_with(edit)

   def test_messages_required(self) -> None:
      """!Every engine message needs a text."""
      def edit(d: JsonDict) -> None:
         del d["messages"]["fainted"]
      with self.assertRaisesRegex(bc.ManifestError, "missing message"):
         self.compile_with(edit)

   def test_placeholders(self) -> None:
      """!Unknown placeholders and placeholders in names fail."""
      def edit(d: JsonDict) -> None:
         d["messages"]["fainted"] = "{who} fainted!"
      with self.assertRaisesRegex(bc.ManifestError, "unknown placeholder"):
         self.compile_with(edit)

      def edit2(d: JsonDict) -> None:
         d["moves"][1]["name"] = "{user}"   # moves[0] is the fallback move the rules name
      with self.assertRaisesRegex(bc.ManifestError, "no placeholders here"):
         self.compile_with(edit2)

   def test_placeholder_sets(self) -> None:
      """!Each text takes only the placeholders the engine fills for it."""
      def edit(d: JsonDict) -> None:
         d["messages"]["used_move"] = "{user} used {item}!"
      with self.assertRaisesRegex(bc.ManifestError, r"messages.used_move: placeholder \{item\}"):
         self.compile_with(edit)

      def edit2(d: JsonDict) -> None:
         d["messages"]["fainted"] = "{user} has {status}"
      with self.assertRaisesRegex(bc.ManifestError, "not available here"):
         self.compile_with(edit2)

      def edit3(d: JsonDict) -> None:
         d["statuses"][0]["texts"]["inflicted"] = "{user} was hit by {move}!"
      with self.assertRaisesRegex(bc.ManifestError, "not available here"):
         self.compile_with(edit3)

   def test_status_turns_take_no_odds(self) -> None:
      """!A status with turns takes no skip or cure odds."""
      def edit(d: JsonDict) -> None:
         timed = next(x for x in d["statuses"] if x.get("turns"))
         timed["skip"] = "1/2"
      with self.assertRaisesRegex(bc.ManifestError, "no skip or cure odds"):
         self.compile_with(edit)

   def test_unit_limits(self) -> None:
      """!Learnsets stay within one step's event budget (handlers: the effect compiler's budgets)."""
      def edit2(d: JsonDict) -> None:
         sp = d["species"][0]
         sp["learnset"] += [[7, m["name"]] for m in d["moves"][:bc.MAX_LEARN_PER_LEVEL + 1]]
      with self.assertRaisesRegex(bc.ManifestError, "moves at one level"):
         self.compile_with(edit2)

   def test_legacy_forms_refused(self) -> None:
      """!The legacy forms are gone (D-23 Q8, E4c): their keys and handler shapes are errors."""
      def move_list(d: JsonDict) -> None:
         d["moves"][0]["effects"] = [{"do": "damage"}]
      with self.assertRaisesRegex(bc.ManifestError, r"moves\[0\].*unknown key.*effects"):
         self.compile_with(move_list)

      def hook(d: JsonDict) -> None:
         d["passives"][0]["handlers"] = [{"hook": "OnResidual", "effects": [{"do": "heal", "fraction": "1/4"}]}]
      with self.assertRaisesRegex(bc.ManifestError, "a manifest handler is a point handler"):
         self.compile_with(hook)

      def modify_hook(d: JsonDict) -> None:
         d["passives"][0]["handlers"] = [{"hook": "OnModifyAtk", "effects": [{"do": "modify", "by": 1.5}]}]
      with self.assertRaisesRegex(bc.ManifestError, "a manifest handler is a point handler"):
         self.compile_with(modify_hook)

      def point_list(d: JsonDict) -> None:
         d["items"][0]["handlers"] = [{"point": "power", "effects": [{"do": "modify", "by": 2}]}]
      with self.assertRaisesRegex(bc.ManifestError, "unknown key.*effects"):
         self.compile_with(point_list)

      for key, value in (("physical", 0.5), ("speed", 0.5)):
         def status_field(d: JsonDict) -> None:
            d["statuses"][0][key] = value
         with self.assertRaisesRegex(bc.ManifestError, f"statuses\\[0\\].*unknown key.*{key}"):
            self.compile_with(status_field)
      for key in ("boost", "weaken"):
         def weather_field(d: JsonDict) -> None:
            d["weathers"][0][key] = ["Water", 1.5]
         with self.assertRaisesRegex(bc.ManifestError, f"weathers\\[0\\].*unknown key.*{key}"):
            self.compile_with(weather_field)

   def test_manifest_point_code(self) -> None:
      """!A manifest point handler is effect-language text of the pure subset."""
      def code(d: JsonDict) -> None:
         d["items"][0]["handlers"] = [{"point": "power", "code": "when move.type == Fire { modify by 2; }"}]
      c = self.compile_with(code)
      self.assertEqual(c["items"][1]["count"], 1)

      def impure(d: JsonDict) -> None:
         d["items"][0]["handlers"] = [{"point": "power", "code": "{ heal holder 1; }"}]
      with self.assertRaisesRegex(bc.ManifestError, "pure"):
         self.compile_with(impure)

   def test_move_rules(self) -> None:
      """!Only damaging categories take power; a damaging move without logic needs power."""
      def edit(d: JsonDict) -> None:
         d["moves"][0]["category"] = "status"
      with self.assertRaisesRegex(bc.ManifestError, "have no power"):
         self.compile_with(edit)

      def edit2(d: JsonDict) -> None:
         m = next(m for m in d["moves"] if m["name"] == "Strike")   # a move without .ceffect logic
         m["power"] = 0
      with self.assertRaisesRegex(bc.ManifestError, "without .ceffect logic hits with its power"):
         self.compile_with(edit2)

   def test_layout(self) -> None:
      """!Required roles; size limit; byte alignment."""
      def edit(d: JsonDict) -> None:
         d["monster_layout"]["roles"].remove("current_hp")
      with self.assertRaisesRegex(bc.ManifestError, "current_hp"):
         self.compile_with(edit)

      roles = bc.view_of(FIXTURE)["monster_layout"]["roles"]
      with self.assertRaisesRegex(bc.ManifestError, "at most 64"):
         bc.compile_layout(roles + ["stat_awaken", "owner_name", "capture_device"], "l", 64)
      lay = bc.compile_layout(["species_key", "nicknamed", "level", "experience", "moves", "pp", "current_hp",
                               "primary_status"], "l")
      names = [r[0] for r in bc.ROLES]
      self.assertEqual(lay["custom"][0]["offset"], 32)   # the retired role, as a custom role of its shape
      self.assertEqual(lay["offsets"][names.index("nicknamed")], bc.ROLE_ABSENT)
      self.assertEqual(lay["offsets"][names.index("level")], 40)   # byte-aligned after the 1-bit role

   def compile_with_ruleset(self, edit: Callable[[JsonDict], None]) -> JsonDict:
      """!Compiles the fixture with an edited ruleset, from memory.
      @param edit  callable(ruleset dict)
      @return compiled
      """
      data = json.loads(RULESET.read_text(encoding="utf-8"))
      edit(data)
      return bc.compile_battle(FIXTURE, None, {RULESET.resolve(): data})

   def test_ruleset_inputs_cover_engine(self) -> None:
      """!Every formula input must accept every value the engine passes."""
      def edit(d: JsonDict) -> None:
         d["inputs"]["attack"] = [1, 700]
      # the staged attack (up to STAGED_CAP: the stat cap x the top stage's x2, C4) reaches beyond damage_base's
      # declared attack
      with self.assertRaisesRegex(bc.ManifestError, "argument attack can reach .* outside its declared range"):
         self.compile_with_ruleset(edit)
      for key, rng in (("gene", [1, 31]), ("speed", [1, 500]), ("attempts", [0, 10]), ("participants", [1, 6]),
                       ("foe_rank", [0, 0]), ("reward_base", [0, 20])):
         with self.subTest(key=key):
            # Default arguments bind the loop values now, not when the edit runs.
            def edit_rs(d: JsonDict, key: str = key, rng: list[int] = rng) -> None:
               d["inputs"][key] = rng
            with self.assertRaisesRegex(bc.ManifestError, f"input '{key}' must cover"):
               self.compile_with_ruleset(edit_rs)

   def test_ruleset_outputs_fit(self) -> None:
      """!Stat and damage formulas must declare ranges the engine can store."""
      for slot, rng in (("vital", [0, 999]), ("core", [1, 70000]), ("reward", [-1, 100])):
         with self.subTest(slot=slot):
            def edit_rs(d: JsonDict, slot: str = slot, rng: list[int] = rng) -> None:
               d["formulas" if slot in d["formulas"] else "stat_formulas"][slot]["range"] = rng
            with self.assertRaisesRegex(bc.ManifestError, f"{slot}.range must lie within"):
               self.compile_with_ruleset(edit_rs)

   def test_party_references(self) -> None:
      """!Party members resolve species, moves and items; parties name at most two known presenters."""
      def edit(d: JsonDict) -> None:
         d["parties"][0]["members"][0]["species"] = "Nobody"
      with self.assertRaisesRegex(bc.ManifestError, "unknown species"):
         self.compile_with(edit)

      def edit2(d: JsonDict) -> None:
         d["parties"][0]["presenters"] = ["Nobody"]
      with self.assertRaisesRegex(bc.ManifestError, "unknown presenter"):
         self.compile_with(edit2)

      def edit3(d: JsonDict) -> None:
         d["parties"][0]["presenters"] = ["Herald", "Sage", "Guide"]
      with self.assertRaisesRegex(bc.ManifestError, "at most 2"):
         self.compile_with(edit3, PRESENTERS)

      def edit4(d: JsonDict) -> None:
         d["parties"][0]["rank"] = 2
      with self.assertRaisesRegex(bc.ManifestError, "rank"):
         self.compile_with(edit4)

   def test_parties(self) -> None:
      """!Parties compile their reward base, rank, escape flag and presenters; a party may have no members."""
      c = bc.compile_battle(PRESENTERS)
      pair = c["parties"][c["names"]["parties"].index("Pair Herald & Mime")]
      self.assertEqual(pair["presenters"], [c["names"]["presenters"].index("Herald"),
                                            c["names"]["presenters"].index("Mime")])
      self.assertEqual((pair["reward"], pair["rank"], pair["flags"]), (25, 1, bc.PARTY_NO_ESCAPE))

      def edit(d: JsonDict) -> None:
         d["parties"].append({"name": "Host", "presenters": ["Sage"]})
      host = self.compile_with(edit, PRESENTERS)["parties"][-1]
      self.assertEqual((host["members"], host["flags"], host["rank"]), ([], 0, bc.DEFAULT_RANK))
      self.assertIn("{kT", bc.emit("battle.json", self.compile_with(edit, PRESENTERS)))

   def test_categories(self) -> None:
      """!Categories name their stats; move_is() and moves resolve them; damage needs a damaging one."""
      c = bc.compile_battle(FIXTURE)
      # physical: attack against defence
      self.assertEqual(c["categories"][0], {"offense": 3, "defense": 5, "flags": bc.CATEGORY_DAMAGING})
      self.assertEqual(c["categories"][1]["flags"], bc.CATEGORY_DAMAGING)
      self.assertEqual(c["categories"][2]["flags"], 0)
      self.assertEqual(c["rules"]["self_hit_category"], 0)

      def edit(d: JsonDict) -> None:
         d["categories"][0]["offense"] = "hp"
      with self.assertRaisesRegex(bc.ManifestError, "other than the pool stat"):
         self.compile_with(edit)

      def edit2(d: JsonDict) -> None:
         del d["categories"][1]["defense"]
      with self.assertRaisesRegex(bc.ManifestError, "both offense and defense"):
         self.compile_with(edit2)

      def edit3(d: JsonDict) -> None:
         d["rules"]["self_hit"]["category"] = "status"
      with self.assertRaisesRegex(bc.ManifestError, "damaging category"):
         self.compile_with(edit3)

      def edit4(d: JsonDict) -> None:
         d["categories"].append({"name": "x"})
         d["categories"].append({"name": "y"})
      with self.assertRaisesRegex(bc.ManifestError, "at most 4"):
         self.compile_with(edit4)

   def test_presenters(self) -> None:
      """!Presenters: motions, flags, texts with {party} only, sequences checked and embedded."""
      c = bc.compile_battle(PRESENTERS)
      pr = {p["name"]: p for p in c["presenters"]}
      self.assertEqual(pr["Sage"]["flags"], bc.PRESENTER_STAYS)
      self.assertEqual(pr["Bard"]["flags"], bc.PRESENTER_RETURNS)
      self.assertIsNone(pr["Mime"]["texts"]["intro"])
      self.assertIn("kBattlePresenter3Enter", pr["Bard"]["enter"]["seq"])
      self.assertIn("&::kBattlePresenter3Enter", bc.emit("battle.json", c))
      deps: list[Path] = []
      bc.compile_battle(PRESENTERS, deps)
      self.assertIn((PRESENTERS.parent / "sequences/bard_enter.json").resolve(), deps)
      bad = {
         "frames": ({"enter": {"style": "slide", "frames": 0}}, "frames"),
         "stay": ({"enter": {"style": "stay"}}, "style"),
         "when": ({"enter": {"style": "none", "when": "after_intro"}}, "unknown key"),
         "sequence": ({"enter": {"style": "sequence"}}, "needs a sequence"),
         "sprite": ({"sprite": "image:X"}, "sprite asset key"),
         "at": ({"at": [200, 0]}, "at"),
         "text": ({"intro": "{user} is here"}, "not available here"),
      }
      for key, (change, message) in bad.items():
         with self.subTest(key=key):
            def edit(d: JsonDict, change: JsonDict = change) -> None:
               d["presenters"][0].update(change)
            with self.assertRaisesRegex(bc.ManifestError, message):
               self.compile_with(edit, PRESENTERS)

   def test_presenter_sequences(self) -> None:
      """!A presenter sequence may only use its sprite slots and the allowed ops."""
      seq = json.loads((PRESENTERS.parent / "sequences/bard_enter.json").read_text(encoding="utf-8"))
      folder = Path(tempfile.mkdtemp(prefix="presenter_seq_"))
      for name, edit, message in (("slot", lambda t: t[0].update(slot=3), "sprite slot 3"),
                                  ("op", lambda t: t.insert(0, {"op": "fade", "dir": "in", "color": "black",
                                                                "frames": 4}), "not allowed")):
         with self.subTest(name=name):
            data = copy.deepcopy(seq)
            edit(data["stages"][0]["tracks"][0])
            path = folder / f"{name}.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(bc.ManifestError, message):
               bc.compile_presenter_sequence(path.name, "p", folder, None, bc.text_encoder.DEFAULT, "kX")

   def test_terms(self) -> None:
      """!Terms merge over the defaults, expand in in-game texts and are checked."""
      t = bc.compile_terms({"party": "Trainer", "passive": {"one": "Ability", "many": "Abilities"}}, "t")
      self.assertEqual(t["party"], {"one": "Trainer", "many": "Trainers"})
      self.assertEqual(t["passive"]["many"], "Abilities")
      self.assertEqual(t["species"], {"one": "Species", "many": "Species"})
      self.assertEqual(t["item"]["one"], "Held item")
      with self.assertRaisesRegex(bc.ManifestError, "unknown concept"):
         bc.compile_terms({"not_a_concept": "x"}, "t")
      with self.assertRaisesRegex(bc.ManifestError, "1-24 characters"):
         bc.compile_terms({"party": ""}, "t")

      def edit(d: JsonDict) -> None:
         d["messages"]["reward"] = "{@reward}: {value} for {@party:many}!"
      c = self.compile_with(edit)
      want = bc.encode("Coins: {value} for Rivals!", "w", {"value"})   # the Starter's terms
      self.assertEqual(c["messages"][bc.MESSAGES.index("reward")], want)

      def edit2(d: JsonDict) -> None:
         d["moves"][1]["name"] = "{@move}"   # moves[0] is the fallback move
      with self.assertRaisesRegex(bc.ManifestError, "no terms here"):
         self.compile_with(edit2)

   def test_cli(self) -> None:
      """!The command line writes the source, ids and depfile."""
      out = Path(tempfile.mkdtemp(prefix="battle_compiler_"))
      status = bc.main(["battle_compiler.py", str(FIXTURE), str(out / "b.cpp"), "--ids", str(out / "ids.h"),
                        "--depfile", str(out / "b.d")])
      self.assertEqual(status, 0)
      self.assertIn("ruleset.json", (out / "b.d").read_text(encoding="utf-8"))
      self.assertIn("MOVE_STRIKE", (out / "ids.h").read_text(encoding="utf-8"))



## Layout hash and record size of the fixture (the Starter sample: nine stats, 4-bit genes; recorded in E9; G4: 6-bit
## training values and a 1-bit form role; F3: an 8-bit bond role).
FIXTURE_HASH = 0x2DDFCB04
## Record bytes of the fixture.
FIXTURE_RECORD_BYTES = 64
## Stats of the fixture.
FIXTURE_STATS = 9
## Statuses of the fixture (status_bits "auto" fits their ids).
FIXTURE_STATUSES = 4


def more_stats(d: JsonDict, count: int) -> None:
   """!Grows the fixture to count stats (extra ones use the core formula; every species gets a base for each).
   @param d      battle dict (edited)
   @param count  stats wanted (>= 6)
   @return None
   """
   for k in range(len(d["stats"]), count):
      d["stats"].append({"name": f"extra{k}", "label": f"Extra {k}", "formula": "core"})
      d["stages"].append({**d["stages"][-1], "stat": f"extra{k}"})   # the same table as the other stats
   for sp in d["species"]:
      sp["base"] += [50] * (count - len(sp["base"]))
      for form in sp.get("forms", []):   # G4: a form's own base stats
         if "base" in form:
            form["base"] += [50] * (count - len(form["base"]))


class StageChartTests(unittest.TestCase):
   """!Stages (D-22 A7) and the type chart's tiers and combine modes (A6), E6a."""

   def compile_with(self, edit: Callable[[JsonDict, JsonDict], None]) -> JsonDict:
      """!Compiles the fixture after an edit of the battle and ruleset data, from memory.
      @param edit  callable(battle dict, ruleset dict)
      @return compiled
      """
      data = bc.view_of(FIXTURE)
      rs = json.loads(RULESET.read_text(encoding="utf-8"))
      edit(data, rs)
      return bc.compile_battle(FIXTURE, None, {**bc.sources_of(FIXTURE, data), RULESET.resolve(): rs})

   def refused(self, edit: Callable[[JsonDict, JsonDict], None], pattern: str) -> None:
      """!Asserts that an edit fails to compile.
      @param edit     callable(battle dict, ruleset dict)
      @param pattern  expected message (regex)
      @return None
      """
      with self.assertRaisesRegex(bc.ManifestError, pattern):
         self.compile_with(edit)

   @staticmethod
   def own_stages(r: JsonDict) -> None:
      """!Points every staged() of the ruleset's pipelines at the stat's own stage (stage_scale()).
      @param r  ruleset dict (edited)
      @return None
      """
      for pl in r["pipelines"].values():
         for step in pl:
            if "expr" in step:
               step["expr"] = re.sub(r"staged\((offense|defense|speed)_raw, ", r"stage_scale(\1_stage_id, \1_raw, ",
                                     step["expr"])

   @staticmethod
   def shared_stages(r: JsonDict) -> None:
      """!Points every stage_scale() of the ruleset's pipelines back at the stats' shared table (staged(); the
      sample names each stat's own stage since E7).
      @param r  ruleset dict (edited)
      @return None
      """
      for pl in r["pipelines"].values():
         for step in pl:
            if "expr" in step:
               step["expr"] = re.sub(r"stage_scale\((offense|defense|speed)_stage_id, \1_raw, ", r"staged(\1_raw, ",
                                     step["expr"])

   def test_legacy_keys_are_refused(self) -> None:
      """!stage_names, stats[].stage, the rules' stage tables and the flat chart name their replacement."""
      self.refused(lambda d, r: d.update(stage_names={"accuracy": "a", "evasion": "e", "critical": "c"}),
                   "stage_names is gone")
      self.refused(lambda d, r: d["stats"][3].update(stage=True), r"stats\[3\] \(attack\)\.stage is gone")
      for key in ("stat_stages", "accuracy_stages", "crit_odds"):
         with self.subTest(key):
            self.refused(lambda d, r: d["rules"].update({key: []}), f"rules.{key} is gone")
      self.refused(lambda d, r: d.update(chart={"Fire": {"Bio": 2}}), "declare chart.tiers")

   def test_stage_records(self) -> None:
      """!Kinds, ranges, tables, one stage per stat and per single kind, no pool stat stage."""
      def stage(i: int, **v: Any) -> Callable[[JsonDict, JsonDict], None]:
         return lambda d, r: d["stages"][i].update(v)
      self.refused(stage(3, range=[1, 6]), r"range \(attack\)")
      self.refused(stage(3, range=[-6, 16]), r"range \(attack\)")
      self.refused(stage(3, table=[[2, 2]]), "9 \\[num, den\\] pairs")   # the Starter's range -4..4 (C2)
      self.refused(stage(2, table=[[1, 2]] * 3), "4 odds")   # the crit stage's range is 0..3
      self.refused(stage(2, table=[[3, 2]] * 4), "at most 1")
      self.refused(stage(4, stat="attack"), "attack already has a stage")
      self.refused(stage(4, stat="hp"), "other than the pool stat")
      self.refused(stage(1, kind="accuracy"), "a second accuracy stage")
      self.refused(stage(0, kind="luck"), "kind: one of stat")
      self.refused(stage(0, stat="attack"), "only a stage of kind stat")
      def formula(i: int, name: str) -> Callable[[JsonDict, JsonDict], None]:
         return lambda d, r: (d["stages"][i].pop("table"), d["stages"][i].update(formula=name))
      self.refused(formula(2, "damage_base"), "the crit stage takes odds")
      self.refused(formula(3, "damage_base"), "with two args")
      self.refused(stage(3, formula="x"), "not both")

      # a narrower range: change_stage clamps to it, its table has one entry per stage
      narrow = stage(5, range=[-2, 3], table=[[1, 2], [3, 4], [1, 1], [5, 4], [3, 2], [2, 1]])
      c = self.compile_with(lambda d, r: (narrow(d, r), self.own_stages(r)))
      defence = c["stages"][5]
      self.assertEqual((defence["range"], len(defence["table"])), ((-2, 3), 6))
      self.assertEqual(bc.shared_stat_stage(c["stages"]), bc.NONE8)   # the stat stages differ now
      text = bc.emit("battle.json", c)
      self.assertIn("{1, 2}, {3, 4}, {1, 1}, {5, 4}, {3, 2}, {2, 1}", text)
      self.assertIn("inline constexpr uint8_t STAGE_DEFENCE = 5;", bc.emit_ids("battle.json", c))

   def test_staged_needs_a_shared_table(self) -> None:
      """!staged() is refused when the stat stages differ; stage_scale() names the stat's own stage."""
      def differ(d: JsonDict, r: JsonDict) -> None:
         d["stages"][8]["table"] = [[1, 1]] * 9   # agility: no multiplier (the Starter's range -4..4)
      self.refused(lambda d, r: (differ(d, r), self.shared_stages(r)),
                   "staged\\(\\) scales by the table every stat stage shares")

      def own(d: JsonDict, r: JsonDict) -> None:
         differ(d, r)
         self.own_stages(r)
      c = self.compile_with(own)
      pl = c["pipelines"]
      spe, atk = c["stats"][8]["stage"], c["stats"][3]["stage"]   # agility, attack
      self.assertEqual(rc.run_pipeline(pl, "speed", {"speed_raw": 100, "speed_stage": 2,
                                                     "speed_stage_id": spe})["outputs"]["speed"], 100)
      self.assertEqual(rc.run_pipeline(pl, "speed", {"speed_raw": 100, "speed_stage": 2,
                                                     "speed_stage_id": atk})["outputs"]["speed"], 150)
      self.assertEqual(rc.run_pipeline(pl, "speed", {"speed_raw": 100, "speed_stage": 2})["outputs"]["speed"], 100)
      self.assertIn("battle::pipeline_stage_scale(in.speed_stage_id, in.speed_raw, in.speed_stage)",
                    bc.emit("battle.json", c))

   def test_formula_stage(self) -> None:
      """!A formula stage runs its named formula (value, stage clamped to its range) in Python and C++."""
      def formula(d: JsonDict, r: JsonDict) -> None:
         r["inputs"].update(v=[1, 999], st=[-6, 6])
         r["formulas"]["speed_curve"] = {"args": ["v", "st"], "expr": "v + v * st / 4", "range": [1, 999]}
         d["stages"][8] = {"stat": "agility", "range": [-2, 2], "formula": "speed_curve"}
         r["pipelines"]["speed"][0]["expr"] = "stage_scale(speed_stage_id, speed_raw, speed_stage)"
      self.refused(lambda d, r: (formula(d, r), self.shared_stages(r),
                                 r["pipelines"]["speed"][0].update(
                                    expr="stage_scale(speed_stage_id, speed_raw, speed_stage)")),
                   "staged\\(\\) scales")   # damage still uses the shared table

      def everywhere(d: JsonDict, r: JsonDict) -> None:
         formula(d, r)
         self.own_stages(r)
      c = self.compile_with(everywhere)
      spe = c["stats"][8]["stage"]   # agility
      run = [rc.run_pipeline(c["pipelines"], "speed", {"speed_raw": 100, "speed_stage": x,
                                                       "speed_stage_id": spe})["outputs"]["speed"] for x in (-6, 1, 6)]
      self.assertEqual(run, [50, 125, 150])   # stages clamped to [-2, 2]
      text = bc.emit("battle.json", c)
      self.assertIn("case 0: return formula_speed_curve(v, stage);", text)
      self.assertEqual(c["stages"][8]["formula_id"], 0)

      def narrow(d: JsonDict, r: JsonDict) -> None:
         everywhere(d, r)
         r["inputs"]["st"] = [-1, 1]
      self.refused(narrow, "argument st can reach \\[-2, 2\\]")

   def test_tiers(self) -> None:
      """!Tiers: names or multipliers in matchups, decisive tiers with messages, the default, the limits."""
      c = bc.compile_battle(FIXTURE)
      ch = c["chart"]
      self.assertEqual((ch["combine"], ch["types_per_species"], ch["added_types"]), ("multiply", 2, 1))
      self.assertEqual([t["name"] for t in ch["tiers"]], ["none", "weak", "normal", "strong"])
      self.assertEqual(ch["tiers"][0]["message"], bc.MESSAGES.index("no_effect"))
      self.assertEqual((ch["above"], ch["below"]), (bc.MESSAGES.index("super_effective"),
                                                    bc.MESSAGES.index("not_very_effective")))

      def tiers(**v: Any) -> Callable[[JsonDict, JsonDict], None]:
         return lambda d, r: d["chart"].update(v)
      self.refused(lambda d, r: (d["chart"].update(combine="exponent"),
                                 d["chart"]["tiers"].append({"name": "odd", "q8": 384})), "power of two")
      self.refused(lambda d, r: d["chart"]["tiers"][2].update(message="missed"), "only a decisive tier")
      self.refused(lambda d, r: d["chart"]["tiers"][0].update(message="nope"), "an engine message key")
      self.refused(lambda d, r: d["chart"]["tiers"][0].update(q8=0), "without q8")
      self.refused(tiers(tiers=[{"name": f"t{i}", "q8": 256} for i in range(9)]), "1 to 8 tiers")
      self.refused(tiers(combine="max"), "chart.combine is one of")
      self.refused(tiers(types_per_species=3), "types_per_species \\+ added_types")
      self.refused(lambda d, r: d["chart"]["matchups"]["Fire"].update(Bio=3), "multiplier of exactly one tier")
      self.refused(lambda d, r: d["chart"]["matchups"]["Fire"].update(Bio="triple"), "a tier")
      self.refused(lambda d, r: (d["chart"].pop("default"), d["chart"]["tiers"].pop(2)), "chart.default")
      self.refused(lambda d, r: d["species"][0].update(types=["Fire", "Water", "Bio"]), "1 to 2 types")
      c = self.compile_with(lambda d, r: d["chart"]["matchups"]["Fire"].update(Bio=1.75, Water=0))
      nt, names = len(c["types"]), c["names"]["types"]
      fire = names.index("Fire")
      self.assertEqual(c["chart"]["matchups"][fire * nt + names.index("Bio")], 3)     # strong (x1.75, C2)
      self.assertEqual(c["chart"]["matchups"][fire * nt + names.index("Water")], 0)   # none (decisive)

   def test_combine_modes(self) -> None:
      """!Non-power-of-two tiers combine by multiply (one factor) or sequential (each in turn), in type_effect()
      and type_scale()."""
      def modes(combine: str) -> Callable[[JsonDict, JsonDict], None]:
         def edit(d: JsonDict, r: JsonDict) -> None:
            d["chart"]["combine"] = combine
            d["chart"]["tiers"][1] = {"name": "weak", "q8": 160}
            d["chart"]["tiers"][3] = {"name": "strong", "q8": 448}
            d["chart"]["matchups"]["Fire"].update(Bio="strong", Wind="weak")
            for step in r["pipelines"]["damage"]:
               if step.get("expr") == "pow2(d, type_exp)":
                  step["expr"] = "type_scale(d)"
         return edit
      results = {}
      for combine in ("multiply", "sequential"):
         c = self.compile_with(modes(combine))
         nt, names = len(c["types"]), c["names"]["types"]
         fx = bc.type_effect(c["chart"], nt, names.index("Fire"), [names.index("Bio"), names.index("Wind")])
         self.assertEqual((fx["decisive"], fx["exp"], fx["q8"], fx["factors"]), (None, 0, 280, [448, 160]))
         inputs = {"level": 50, "move_power": 80, "offense_raw": 101, "offense_stage": 0, "defense_raw": 90,
                   "defense_stage": 0, "crit": 0, "spread": 0, "same_type": 0, "type_exp": 0}
         out = rc.run_pipeline(c["pipelines"], "damage", inputs, roll=0, types=fx["factors"])
         results[combine] = out["trace"][TYPE_SCALE_STEP][3]
         self.assertIn("battle::pipeline_type_scale(ctx, l_d)", bc.emit("battle.json", c))
         with self.assertRaisesRegex(rc.RulesetError, "needs the target's tiers"):
            rc.run_pipeline(c["pipelines"], "damage", inputs, roll=0)
      # one factor of 280/256 against x1.75 then x0.625 truncating after each
      base = rc.run_pipeline(self.compile_with(modes("multiply"))["pipelines"], "damage",
                             {"level": 50, "move_power": 80, "offense_raw": 101, "offense_stage": 0, "defense_raw": 90,
                              "defense_stage": 0, "crit": 0, "spread": 0, "same_type": 0, "type_exp": 0}, roll=0,
                             types=[])["trace"][TYPE_SCALE_STEP][3]
      self.assertEqual(results["multiply"], (base * 280) >> rc.Q8_SHIFT)
      self.assertEqual(results["sequential"], (((base * 448) >> rc.Q8_SHIFT) * 160) >> rc.Q8_SHIFT)


   def test_tiers_fixture(self) -> None:
      """!tests/data/battle_tiers (battle_tiers_tests.cpp): the Python side gives the C++ test's numbers."""
      c = bc.compile_battle(TIERS_FIXTURE)
      nt, names = len(c["types"]), c["names"]["types"]
      slush = [names.index("Cold"), names.index("Hot")]
      fx = bc.type_effect(c["chart"], nt, names.index("Hot"), slush)
      self.assertEqual((fx["q8"], fx["factors"]), (280, [448, 160]))
      self.assertEqual(rc.type_scale({"battle": c["pipelines"]["battle"], "types": fx["factors"]},
                                     {"type_exp": 0}, 1003), 1096)
      void = bc.type_effect(c["chart"], nt, names.index("Hot"), [names.index("Void"), names.index("Cold")])
      self.assertEqual(void["decisive"], "ko")
      pace = c["stats"][2]["stage"]
      speed = rc.run_pipeline(c["pipelines"], "speed", {"speed_raw": 100, "speed_stage": 2, "speed_stage_id": pace})
      self.assertEqual(speed["outputs"]["speed"], 150)
      self.assertIn("formula_pace_curve(v, stage)", bc.emit("battle.json", c))


class StatTests(unittest.TestCase):
   """!Stats from the manifest (D-20 C): limits, flags, stages, traits, record roles, the layout hash."""

   def compile_with(self, edit: Callable[[JsonDict], None], platform: str | None = None) -> JsonDict:
      """!Compiles the fixture after an edit, from memory.
      @param edit      callable(battle dict)
      @param platform  target platform, or None
      @return compiled
      """
      data = bc.view_of(FIXTURE)
      edit(data)
      return bc.compile_battle(FIXTURE, None, bc.sources_of(FIXTURE, data), platform=platform)

   def test_fixture_stats(self) -> None:
      """!The fixture's nine stats: pool, order, stages after accuracy, evasion and critical (none for the HP, MP
      and EP pools), the stage names."""
      c = bc.compile_battle(FIXTURE)
      self.assertEqual([st["name"] for st in c["stats"]], ["hp", "mp", "ep", "attack", "magic", "defence",
                                                           "protection", "intelligence", "agility"])
      self.assertEqual([st["stage"] for st in c["stats"]], [bc.NONE8] * 3 + [3, 4, 5, 6, 7, 8])
      self.assertEqual([st["flags"] for st in c["stats"]], [0] * 3 + [bc.STATINFO_TRAIT] * 6)
      self.assertTrue(c["stats"][0]["pool"] and c["stats"][8]["order"])
      names = [bytes(x[:-1]).decode() for x in c["stage_names"]]
      self.assertEqual(names, ["accuracy", "evasion", "focus", "Attack", "Magic", "Defence", "Protection",
                               "Intelligence", "Agility"])
      self.assertEqual(c["traits"][c["names"]["traits"].index("Fierce")]["pct"],
                       [100, 100, 100, 115, 100, 100, 88, 100, 100])
      self.assertIn("inline constexpr uint8_t STAT_AGILITY = 8;", bc.emit_ids("battle.json", c))
      self.assertIn("inline constexpr int NUM_STATS = 9;", bc.emit_ids("battle.json", c))
      lo, hi = c["stats"][0]["bounds"]
      self.assertTrue(1 <= lo <= hi <= 700)

   def test_three_and_ten_stats(self) -> None:
      """!A three-stat manifest and a ten-stat one compile (ten is the GBA's limit)."""
      c = bc.compile_battle(STATS3, platform="gba")
      self.assertEqual(len(c["stats"]), 3)
      self.assertIn("kStatCount = 3;", bc.emit("battle.json", c))
      c = self.compile_with(lambda d: more_stats(d, 10), "gba")
      self.assertEqual(len(c["stats"]), 10)
      self.assertEqual(len(c["stage_names"]), 3 + 7)   # six stat stages and the extra stat's
      self.assertEqual(c["layout"]["stat_count"], 10)

   def test_stat_limits(self) -> None:
      """!Eleven stats are refused for the GBA (and by default), 33 everywhere."""
      for platform in ("gba", None):
         with self.subTest(platform=platform):
            with self.assertRaisesRegex(bc.ManifestError, "at most 10 stats"):
               self.compile_with(lambda d: more_stats(d, 11), platform)
      self.assertEqual(len(self.compile_with(lambda d: more_stats(d, 11), "desktop")["stats"]), 11)
      self.assertEqual(len(self.compile_with(lambda d: more_stats(d, 32), "headless")["stats"]), 32)
      for platform in ("desktop", "headless", "dreamcast", "default"):
         with self.subTest(platform=platform):
            with self.assertRaisesRegex(bc.ManifestError, "at most 32 stats"):
               self.compile_with(lambda d: more_stats(d, 33), platform)
      with self.assertRaisesRegex(bc.ManifestError, "unknown platform"):
         self.compile_with(lambda d: None, "n64")

   def test_record_limits(self) -> None:
      """!The record limit is the platform's: 100 bytes on the GBA, 256 elsewhere."""
      # G4: the fixture holds stat_training and a custom form role itself
      extras = ["stat_awaken", "owner_name", "capture_device", "text_language", {"custom": "pad", "width": 32,
                                                                                  "count": 4}]
      roles = bc.view_of(FIXTURE)["monster_layout"]["roles"] + extras
      gba, other = bs.platform_limits("gba")["max_record_bytes"], bs.platform_limits("desktop")["max_record_bytes"]
      self.assertEqual((gba, other), (100, 256))
      with self.assertRaisesRegex(bc.ManifestError, "at most 100"):
         bc.compile_layout(roles, "l", gba, 16)
      self.assertLessEqual(bc.compile_layout(roles, "l", other, 16)["bytes"], other)

      def grow(d: JsonDict) -> None:
         more_stats(d, 32)
         d["monster_layout"]["roles"] += extras
      self.assertGreater(self.compile_with(grow, "desktop")["layout"]["bytes"], gba)

   def test_one_pool_and_one_order(self) -> None:
      """!Exactly one pool stat and one order stat."""
      edits = {"no pool": lambda d: d["stats"][0].pop("pool"),
               "two pools": lambda d: d["stats"][1].update(pool=True),
               "no order": lambda d: d["stats"][8].pop("order"),
               "two orders": lambda d: d["stats"][4].update(order=True)}
      for name, edit in edits.items():
         with self.subTest(name):
            with self.assertRaisesRegex(bc.ManifestError, "exactly one stat is the (pool|order) stat"):
               self.compile_with(edit)

   def test_stat_entries(self) -> None:
      """!Names, formulas and transfer keys are checked; base stats are one per stat."""
      def bad_formula(d: JsonDict) -> None:
         d["stats"][1]["formula"] = "stat_nope"
      with self.assertRaisesRegex(bc.ManifestError, "names a stat formula"):
         self.compile_with(bad_formula)

      def bad_name(d: JsonDict) -> None:
         d["stats"][1]["name"] = "Attack"
      with self.assertRaisesRegex(bc.ManifestError, "lower-case identifier"):
         self.compile_with(bad_name)

      def stage_clash(d: JsonDict) -> None:
         d["stats"][3]["name"] = "accuracy"   # its stage would be named accuracy too
         d["stages"][3]["stat"] = "accuracy"
      with self.assertRaisesRegex(bc.ManifestError, "no other stage uses"):
         self.compile_with(stage_clash)

      def same_transfer(d: JsonDict) -> None:
         d["stats"][4]["transfer"] = "attack"
      with self.assertRaisesRegex(bc.ManifestError, "transfer"):
         self.compile_with(same_transfer)

      def short_base(d: JsonDict) -> None:
         d["species"][0]["base"].pop()
      with self.assertRaisesRegex(bc.ManifestError, "9 base stats"):
         self.compile_with(short_base)

   def test_traits_name_trait_stats(self) -> None:
      """!A trait naming a stat without the trait flag is refused; the pool stat has none by default."""
      def no_trait(d: JsonDict) -> None:
         d["stats"][5]["trait"] = False   # Sturdy raises defence
      with self.assertRaisesRegex(bc.ManifestError, "with the trait flag"):
         self.compile_with(no_trait)

      def pool_trait(d: JsonDict) -> None:
         d["traits"][0].update(up="hp", pct=[115, 88])
      with self.assertRaisesRegex(bc.ManifestError, "with the trait flag"):
         self.compile_with(pool_trait)

      def no_pct(d: JsonDict) -> None:
         del d["traits"][1]["pct"]   # Fierce raises attack: no default percentages (D-22 decision 8)
      with self.assertRaisesRegex(bc.ManifestError, r"traits\[1\]\.pct: \[raised %, lowered %\]"):
         self.compile_with(no_pct)

   def test_stages(self) -> None:
      """!Stages are the manifest's records in order: effect code names them; a stat without one has none."""
      c = bc.compile_battle(FIXTURE)
      lookup = bc.stage_lookup(c["stages"], "s")
      self.assertEqual(lookup("critical", "s"), 2)
      self.assertEqual(lookup("attack", "s"), 3)
      self.assertNotIn("hp", lookup.index)
      self.assertEqual([st["kind"] for st in c["stages"]], ["accuracy", "evasion", "crit"] + ["stat"] * 6)
      self.assertEqual(c["stages"][2]["odds"], [(1, 20), (1, 10), (1, 5), (3, 10)])
      self.assertEqual(bc.shared_stat_stage(c["stages"]), 3)

      def unstaged(d: JsonDict) -> None:
         del d["stages"][4]   # magic: no effect names its stage
      c = self.compile_with(unstaged)
      self.assertEqual([st["stage"] for st in c["stats"]], [bc.NONE8] * 3 + [3, bc.NONE8, 4, 5, 6, 7])
      self.assertEqual(len(c["stage_names"]), 3 + 5)

   def test_layout_hash(self) -> None:
      """!The fixture's stat count and widths give its layout hash and record size; another stat count or gene
      width changes the hash."""
      layout = bc.view_of(FIXTURE)["monster_layout"]
      c = bc.compile_battle(FIXTURE)
      statuses = len(c["statuses"]) - 1   # without "none"
      self.assertEqual((c["layout"]["hash"], c["layout"]["bytes"]), (FIXTURE_HASH, FIXTURE_RECORD_BYTES))

      def hash_of(roles: list[Any], stats: int = FIXTURE_STATS) -> int:
         """!The layout hash with other roles or another stat count."""
         return bc.compile_layout({**layout, "roles": roles}, "l", stat_count=stats, status_count=statuses)["hash"]
      self.assertEqual(hash_of(layout["roles"]), FIXTURE_HASH)
      self.assertNotEqual(hash_of(layout["roles"], FIXTURE_STATS + 1), FIXTURE_HASH)

      def genes(width: int) -> list[Any]:
         """!The fixture's roles with another gene width."""
         return [{"role": "stat_genes", "width": width} if isinstance(r, dict) and r.get("role") == "stat_genes"
                 else r for r in layout["roles"]]
      self.assertEqual(hash_of(genes(4)), FIXTURE_HASH)
      self.assertNotEqual(hash_of(genes(6)), FIXTURE_HASH)

   def test_per_stat_role_sizes(self) -> None:
      """!Per-stat roles hold width x stat_count bits; widths up to the engine maximum, only where choosable."""
      names = [r[0] for r in bc.ROLES]
      roles = ["species_key", {"role": "stat_genes", "width": 4}, "stat_training"] + bc.REQUIRED_ROLES[1:]
      lay = bc.compile_layout(roles, "l", stat_count=3)
      genes, training = lay["offsets"][names.index("stat_genes")], lay["offsets"][names.index("stat_training")]
      self.assertEqual(genes, 32)
      self.assertEqual(training, 48)   # 32 + 4 x 3 = 44, byte-aligned
      self.assertEqual(lay["offsets"][names.index("level")], training + 8 * 3)
      self.assertEqual(lay["widths"][names.index("stat_genes")], 4)
      self.assertEqual(bc.role_max_value(lay, "stat_genes"), 15)
      with self.assertRaisesRegex(bc.ManifestError, "width"):
         bc.compile_layout(["species_key", {"role": "stat_genes", "width": 9}] + bc.REQUIRED_ROLES[1:], "l")
      with self.assertRaisesRegex(bc.ManifestError, "is fixed"):
         bc.compile_layout([{"role": "species_key", "width": 16}] + bc.REQUIRED_ROLES[1:], "l")
      c = bc.compile_battle(STATS3)
      self.assertEqual(c["layout"]["bytes"], 56)   # layout v2: six move slots, a nickname, three custom roles
      with self.assertRaisesRegex(bc.ManifestError, "gene"):
         data = bc.view_of(STATS3)
         data["parties"][0]["members"][0]["genes"] = 16   # 4-bit genes
         bc.compile_battle(STATS3, None, bc.sources_of(STATS3, data))

   def test_genes_follow_the_layout(self) -> None:
      """!The ruleset's gene input must cover the layout's genes (0-15 for 4 bits), not a fixed 0-31."""
      rs_path = STATS3.parent / "ruleset.json"
      data = json.loads(rs_path.read_text(encoding="utf-8"))
      data["inputs"]["gene"] = [0, 14]
      with self.assertRaisesRegex(bc.ManifestError, "input 'gene' must cover \\[0, 15\\]"):
         bc.compile_battle(STATS3, None, {rs_path.resolve(): data})

   def test_preview_matches_engine(self) -> None:
      """!preview_stats() runs each stat's formula; tests/battle_stats3_tests.cpp expects the same numbers."""
      c = bc.compile_battle(STATS3)
      self.assertEqual(bc.preview_stats(c, "Blob", 10, genes=[15, 3, 7], trait="Quick"), [45, 14, 16])

   def test_engine_registry(self) -> None:
      """!capsule/monster/monster.h's kRoleChoices are the per-stat flags and widest widths of ROLES."""
      header = (ROOT / "include/capsule/monster/monster.h").read_text(encoding="utf-8")
      body = re.search(r"kRoleChoices\[[^]]*\] = \{(.*?)\};", header, re.S).group(1)
      consts = {k: int(v) for k, v in re.findall(r"constexpr \w+ (k\w+) = (\d+);", header)}
      rules = {"One": "one", "PerStat": "stat", "PerMoveSlot": "move", "Chosen": "chosen", "Retired": "retired"}

      def value(v: str) -> int:
         """!A number or a constant of the header."""
         return consts[v] if v in consts else int(v)
      choices = [(rules[a], value(b), value(c)) for a, b, c in
                 re.findall(r"\{RoleCount::(\w+),\s*(\w+),\s*(\w+),\s*0\}", body)]
      self.assertEqual(choices, [(r[3], bc.role_width_max(r[0]), bc.role_count_max(r[0])) for r in bc.ROLES])
      enum = re.search(r"enum class RoleCount : uint8_t\s*\{(.*?)\};", header, re.S).group(1)
      self.assertEqual([rules[n] for n in re.findall(r"^\s*(\w+),", enum, re.M) if n in rules], bc.COUNT_RULES)
      self.assertIn(f"kDefaultStatCount = {bc.DEFAULT_STAT_COUNT};", header)
      self.assertEqual((consts["kMaxMoveSlots"], consts["kDefaultMoveSlots"]), (bc.MAX_MOVE_SLOTS,
                                                                              bc.DEFAULT_MOVE_SLOTS))
      self.assertEqual((consts["kMaxCustomRoles"], consts["kDefaultPartySize"]), (bc.MAX_CUSTOM_ROLES,
                                                                                bc.DEFAULT_PARTY_SIZE))
      self.assertIn(f"kFirstCustomRole = 0x{bc.CUSTOM_ROLE_BASE:02X};", header)
      self.assertIn(f"kMaxPartySize = {bs.platform_limits('gba')['max_party_size']};", header)
      self.assertIn(f"kMaxPartySize = {bs.platform_limits('desktop')['max_party_size']};", header)

   def test_cli_platform(self) -> None:
      """!--platform picks the limits; an unknown platform fails."""
      out = Path(tempfile.mkdtemp(prefix="battle_compiler_"))
      self.assertEqual(bc.main(["battle_compiler.py", str(STATS3), str(out / "b.cpp"), "--platform", "gba"]), 0)
      self.assertIn("kStatCount = 3;", (out / "b.cpp").read_text(encoding="utf-8"))
      self.assertEqual(bc.main(["battle_compiler.py", str(STATS3), str(out / "b.cpp"), "--platform", "n64"]), 1)


## The fixture's monster_layout roles (its "roles" list).
def fixture_roles() -> list[Any]:
   """!The fixture's role entries.
   @return list (a fresh copy)
   """
   return bc.view_of(FIXTURE)["monster_layout"]["roles"]


def fixture_hash(roles: list[Any], **v2: int) -> int:
   """!The layout hash of role entries with the fixture's stat and status counts (and its move slots and party
   size unless given).
   @param roles  role entries
   @param v2     move_slots / party_size overrides
   @return the hash
   """
   layout = {**bc.view_of(FIXTURE)["monster_layout"], "roles": roles, **v2}
   return bc.compile_layout(layout, "l", 256, FIXTURE_STATS, status_count=FIXTURE_STATUSES)["hash"]


def with_role(roles: list[Any], role: str, **shape: int) -> list[Any]:
   """!Role entries with one engine role given a width or count (a plain name or an object entry).
   @param roles  role entries
   @param role   the engine role
   @param shape  width / count
   @return new entries
   """
   def named(r: Any) -> bool:
      """!Whether an entry is the role."""
      return r == role or (isinstance(r, dict) and r.get("role") == role)
   return [{**(r if isinstance(r, dict) else {"role": r}), **shape} if named(r) else r for r in roles]


class RoleLayoutTests(unittest.TestCase):
   """!Layout v2 (D-22 decision 2, D-23 section 2): custom roles, move slots, party size, chosen widths, the hash."""

   def compile_with(self, edit: Callable[[JsonDict], None], platform: str | None = None) -> JsonDict:
      """!Compiles the fixture after an edit, from memory.
      @param edit      callable(battle dict)
      @param platform  target platform, or None
      @return compiled
      """
      data = bc.view_of(FIXTURE)
      edit(data)
      return bc.compile_battle(FIXTURE, None, bc.sources_of(FIXTURE, data), platform=platform)

   def test_custom_roles(self) -> None:
      """!Custom roles: IDs 0x80 + index, packing, defaults, a 32-element ribbon role, emitted tables and ids."""
      def custom(d: JsonDict) -> None:
         d["monster_layout"]["roles"] += [{"custom": "gender", "width": 2, "default": 1},
                                 {"custom": "ribbons", "width": 1, "count": 32},
                                 {"custom": "met_location", "width": 8}]
      c = self.compile_with(custom)
      lay = c["layout"]
      cr = {x["name"]: x for x in lay["custom"]}
      # G4: after the fixture's current_mp, current_ep and form (F3: and bond; B2: egg and hatch_counter)
      self.assertEqual([cr[n]["id"] for n in ("gender", "ribbons", "met_location")], [0x86, 0x87, 0x88])
      # 32 bits: byte-aligned, right after gender's two bits (G4: the form role's bit before them)
      self.assertEqual(cr["ribbons"]["offset"], (cr["gender"]["offset"] + 2 + 7) // 8 * 8)
      self.assertEqual(cr["met_location"]["offset"], cr["ribbons"]["offset"] + 32)
      self.assertEqual((cr["gender"]["default"], cr["gender"]["name_hash"]), (1, bc.name_hash("gender")))
      self.assertEqual(bc.role_info(lay, "ribbons"), (0x87, 1, 32))
      self.assertEqual(bc.role_info(lay, "level"), (bc.ROLE_NAMES.index("level"), 8, 1))
      self.assertIsNone(bc.role_info(lay, "stat_awaken"))
      self.assertEqual(bc.role_max_value(lay, "gender"), 3)
      cpp = bc.emit("battle.json", c)
      self.assertIn("const RoleSlot kCustom[] = {", cpp)
      self.assertIn(f"{{0x{bc.name_hash('ribbons'):08X}u, 0u, {cr['ribbons']['offset']}, 1, 32}},", cpp)
      self.assertIn("kCounts, kCustom,", cpp)
      ids = bc.emit_ids("battle.json", c)
      self.assertIn("inline constexpr uint8_t ROLE_RIBBONS = 0x87;", ids)
      self.assertIn("inline constexpr uint8_t ROLE_NICKNAME = 0x14;", ids)
      self.assertIn("inline constexpr int NUM_CUSTOM_ROLES = 9;", ids)
      self.assertNotIn("ROLE_STAT_AWAKEN", ids)
      # the FNV-1a of a name is the engine's (capsule::monster::name_hash)
      self.assertEqual(bc.name_hash(""), bc.FNV_OFFSET_BASIS)
      self.assertEqual(bc.name_hash("a"), 0xE40C292C)

   def test_custom_role_refusals(self) -> None:
      """!Names, widths, counts, defaults and the 32-role cap are checked."""
      base = bc.REQUIRED_ROLES + ["species_key"]
      base = list(dict.fromkeys(base))
      bad = {"engine name": {"custom": "level", "width": 8},
             "no width": {"custom": "gender"},
             "too wide": {"custom": "gender", "width": 33},
             "zero count": {"custom": "gender", "width": 1, "count": 0},
             "count over 255": {"custom": "gender", "width": 1, "count": 256},
             "default over width": {"custom": "gender", "width": 2, "default": 4},
             "not an identifier": {"custom": "Gender", "width": 2},
             "unknown key": {"custom": "gender", "width": 2, "colour": 1},
             "retired as an engine role": {"role": "friendship"}}
      for name, entry in bad.items():
         with self.subTest(name):
            with self.assertRaises(bc.ManifestError):
               bc.compile_layout(base + [entry], "l", 256)
      with self.assertRaisesRegex(bc.ManifestError, "listed twice"):
         bc.compile_layout(base + [{"custom": "gender", "width": 2}, {"custom": "gender", "width": 2}], "l", 256)
      many = [{"custom": f"flag{i}", "width": 1} for i in range(bc.MAX_CUSTOM_ROLES)]
      self.assertEqual(len(bc.compile_layout(base + many, "l", 256)["custom"]), 32)
      with self.assertRaisesRegex(bc.ManifestError, "at most 32 custom roles"):
         bc.compile_layout(base + many + [{"custom": "one_more", "width": 1}], "l", 256)
      with self.assertRaisesRegex(bc.ManifestError, "at most 100"):
         bc.compile_layout(base + [{"custom": "big", "width": 32, "count": 24}], "l")

   def test_move_slots(self) -> None:
      """!move_slots 1-8 sets the move roles' counts; members name up to that many moves."""
      for slots in (2, 6):
         with self.subTest(slots=slots):
            def edit(d: JsonDict, n: int = slots) -> None:
               d["monster_layout"]["move_slots"] = n
               for t in d["parties"]:
                  for pm in t.get("members", []):
                     if "moves" in pm:
                        pm["moves"] = pm["moves"][:n]
            c = self.compile_with(edit)
            lay = c["layout"]
            names = bc.ROLE_NAMES
            self.assertEqual(lay["counts"][names.index("moves")], slots)
            self.assertEqual(lay["move_slots"], slots)
            self.assertNotEqual(lay["hash"], FIXTURE_HASH)
            self.assertTrue(all(len(pm["moves"]) == bc.MAX_MOVE_SLOTS for t in c["parties"] for pm in t["members"]))
            self.assertIn(f", {FIXTURE_STATS}, {slots}, 6, 6, 0, {{0}}, 0x{lay['record_hash']:08X}u}};",
                          bc.emit("battle.json", c))
      # the fixture's 8 slots written out keep the hash
      self.assertEqual(fixture_hash(fixture_roles(), move_slots=8, party_size=6), FIXTURE_HASH)

      def three_moves(d: JsonDict) -> None:
         d["monster_layout"]["move_slots"] = 2
         d["parties"][0]["members"][0]["moves"] = [m["name"] for m in d["moves"][:3]]
      with self.assertRaisesRegex(bc.ManifestError, "up to 2"):
         self.compile_with(three_moves)
      for bad in (0, 9):
         with self.subTest(move_slots=bad):
            with self.assertRaisesRegex(bc.ManifestError, "move_slots"):
               bc.compile_layout({"move_slots": bad, "roles": fixture_roles()}, "l")

   def test_party_size(self) -> None:
      """!party_size up to the platform's cap (6 GBA, 8 otherwise); formats and parties are checked against it."""
      def three(d: JsonDict) -> None:
         d["monster_layout"]["party_size"] = 3
         for f in d["formats"]:
            f.pop("party_max", None)
      c = self.compile_with(three)
      self.assertEqual(c["layout"]["party_size"], 3)
      self.assertEqual({f["party_max"] for f in c["formats"]}, {3})   # the default follows the layout
      self.assertNotEqual(c["layout"]["hash"], FIXTURE_HASH)

      def three_party_max(d: JsonDict) -> None:
         three(d)
         d["formats"][0]["party_max"] = 4
      with self.assertRaisesRegex(bc.ManifestError, "party_max"):
         self.compile_with(three_party_max)

      def three_members(d: JsonDict) -> None:
         three(d)
         d["parties"][0]["members"] = d["parties"][0]["members"] * 4
      with self.assertRaisesRegex(bc.ManifestError, "at most 3 creatures"):
         self.compile_with(three_members)

      def eight(d: JsonDict) -> None:
         d["monster_layout"]["party_size"] = 8
      with self.assertRaisesRegex(bc.ManifestError, "party_size"):
         self.compile_with(eight)   # the GBA holds 6
      self.assertEqual(self.compile_with(eight, "desktop")["layout"]["party_size"], 8)

      def nine(d: JsonDict) -> None:
         d["monster_layout"]["party_size"] = 9
      with self.assertRaisesRegex(bc.ManifestError, "party_size"):
         self.compile_with(nine, "desktop")

   def test_chosen_widths(self) -> None:
      """!Engine roles take widths up to the engine's maximum where meaningful; the data must fit them."""
      def width_of(role: str, width: int) -> Callable[[JsonDict], None]:
         """!An edit giving one role of the fixture a width."""
         def edit(d: JsonDict) -> None:
            d["monster_layout"]["roles"] = with_role(d["monster_layout"]["roles"], role, width=width)
         return edit
      names = bc.ROLE_NAMES
      for role, width in (("level", 7), ("experience", 32), ("moves", 6), ("seed", 16), ("held_item", 4),
                          ("current_hp", 12), ("trait", 4)):
         with self.subTest(role=role, width=width):
            c = self.compile_with(width_of(role, width))
            self.assertEqual(c["layout"]["widths"][names.index(role)], width)
            self.assertNotEqual(c["layout"]["hash"], FIXTURE_HASH)
      # the registry width written out keeps the hash
      self.assertEqual(self.compile_with(width_of("level", 8))["layout"]["hash"], FIXTURE_HASH)
      too_narrow = {"moves": 4,          # 28 moves: 15 is not enough (the all-ones "no move" too)
                    "level": 5,          # level_max 50 (C2)
                    "current_hp": 7,     # the hit points' persist role: the hp stat reaches 249 (C1)
                    "trait": 2}          # more than four traits
      for role, width in too_narrow.items():
         with self.subTest(narrow=role):
            with self.assertRaisesRegex(bc.ManifestError, f"role '{role}' is {width} bits wide"):
               self.compile_with(width_of(role, width))
      fixed_or_over = {"species_key": 16, "primary_status": 4, "nickname": 7, "level": 9, "moves": 17,
                       "current_hp": 17}
      for role, width in fixed_or_over.items():
         with self.subTest(refused=role):
            with self.assertRaises(bc.ManifestError):
               self.compile_with(width_of(role, width))

      def nickname(n: int) -> Callable[[JsonDict], None]:
         """!An edit choosing the nickname's length."""
         def edit(d: JsonDict) -> None:
            d["monster_layout"]["roles"] = with_role(d["monster_layout"]["roles"], "nickname", count=n)
         return edit
      c = self.compile_with(nickname(12))
      self.assertEqual(c["layout"]["counts"][names.index("nickname")], 12)
      self.assertNotEqual(c["layout"]["hash"], FIXTURE_HASH)
      self.assertEqual(self.compile_with(nickname(10))["layout"]["hash"], FIXTURE_HASH)   # the fixture's own
      with self.assertRaisesRegex(bc.ManifestError, "count"):
         self.compile_with(nickname(13))
      with self.assertRaisesRegex(bc.ManifestError, "count of role 'moves' is fixed"):
         bc.compile_layout([{"role": "moves", "count": 2}] + fixture_roles()[1:], "l")

   def test_hash_rules(self) -> None:
      """!Customs fold (name hash, width, count); defaults do not; engine shapes only where they differ."""
      h = fixture_hash
      base = fixture_roles()
      plus = base + [{"custom": "gender", "width": 2}]
      self.assertNotEqual(h(plus), FIXTURE_HASH)
      self.assertEqual(h(base + [{"custom": "gender", "width": 2, "default": 1}]), h(plus))   # defaults: no
      self.assertNotEqual(h(base + [{"custom": "gender", "width": 3}]), h(plus))
      self.assertNotEqual(h(base + [{"custom": "gender", "width": 2, "count": 2}]), h(plus))
      self.assertNotEqual(h(base + [{"custom": "sex", "width": 2}]), h(plus))
      self.assertEqual(h(base + [{"custom": "gender", "width": 2, "count": 1}]), h(plus))
      # the fold is (tag, name hash, width, count), each one FNV-1a step
      expect = FIXTURE_HASH
      for v in (bc.HASH_CUSTOM_TAG, bc.name_hash("gender"), 2, 1):
         expect = ((expect ^ v) * bc.FNV_PRIME) & bc.U32_MASK
      self.assertEqual(h(plus), expect)
      # a custom role with another shape is another role
      mp = base.index({"custom": "current_mp", "width": 10})
      wider = base[:mp] + [{"custom": "current_mp", "width": 16}] + base[mp + 1:]
      self.assertNotEqual(h(wider), FIXTURE_HASH)
      # order matters; the party size folds only where it differs
      self.assertNotEqual(h(base[1:2] + base[:1] + base[2:]), FIXTURE_HASH)
      self.assertEqual(h(base, party_size=6, move_slots=8), FIXTURE_HASH)
      self.assertNotEqual(h(base, party_size=5), FIXTURE_HASH)
      self.assertNotEqual(h(base, move_slots=3), h(base, move_slots=5))



## The effect VM battle fixture (tests/battle_vm_tests.cpp runs it): moves and a passive as .ceffect logic.
VM_FIXTURE = ROOT / "tests/data/effects/battle/battle.json"


class LogicTests(unittest.TestCase):
   """!Effect logic (E4b): the legacy lists lowered to effect code, .ceffect sources, the bank."""

   def compile_dir(self, edit_battle: Callable[[JsonDict], None], logic: str) -> JsonDict:
      """!Compiles the VM fixture from a temporary folder with an edited manifest and logic source.
      @param edit_battle  callable(battle dict)
      @param logic        the logic.ceffect text
      @return compiled
      """
      data = bc.view_of(VM_FIXTURE)
      edit_battle(data)
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         write_split(folder / "battle.json", data)
         (folder / "ruleset.json").write_text((VM_FIXTURE.parent / "ruleset.json").read_text(encoding="utf-8"),
                                              encoding="utf-8")
         (folder / "logic.ceffect").write_text(logic, encoding="utf-8")
         return bc.compile_battle(folder / "battle.json")

   def test_converted_logic(self) -> None:
      """!Every move, passive, item, status, weather, volatile, side and field condition is an owner, in that order,
      then the global owners (F1: the Starter's ward rule; F3: its bond rule; B1: its merge rule); the Starter's logic
      is its .ceffect sources and condition records (per-foe handlers and the AI's fixed-damage summary: test_per_foe
      in tests/test_effect_compiler.py, test_fixed_damage_hints below)."""
      c = bc.compile_battle(FIXTURE)
      bank = c["effects"]["bank"]
      counts = [len(c["moves"]), len(c["passives"]), len(c["items"]) - 1, len(c["statuses"]) - 1,
                len(c["weathers"]), len(c["volatiles"]), len(c["side_conditions"]), len(c["field_conditions"])]
      globals_ = 3   # F1: global "ward_rule", F3: global "bond_rule", B1: global "merge_rule"
      self.assertEqual(len(bank.owners), sum(counts) + globals_)
      self.assertEqual([m["owner"] for m in c["moves"]], list(range(counts[0])))
      self.assertEqual(c["passives"][0]["owner"], counts[0])
      self.assertEqual([v["owner"] for v in c["volatiles"]], list(range(sum(counts[:5]), sum(counts[:6]))))
      self.assertNotIn("sources", c["effects"])
      claw = c["names"]["moves"].index("Flare Claw")
      self.assertTrue(c["moves"][claw]["flags"] & 1 << c["move_flags"].index("protect"))
      # point handlers join their owner's handlers
      blaze = bank.owners[c["passives"][0]["owner"]]
      self.assertEqual(blaze[1], 1 << c["pipelines"]["points"]["power"]["id"])
      self.assertTrue(c["moves"][claw]["formula"])
      self.assertFalse(c["moves"][c["names"]["moves"].index("Smolder")]["formula"])   # a status move
      # a damaging move without logic hits with the standard hit
      strike = c["moves"][c["names"]["moves"].index("Strike")]
      self.assertTrue(strike["formula"])
      self.assertIn('move "Strike" hit', bank.labels)
      cpp = bc.emit("battle.json", c)
      self.assertIn("kEffectOwners", cpp)
      self.assertNotIn("kComponents", cpp)

   def test_fixed_damage_hints(self) -> None:
      """!The AI's fixed-damage summary of a .ceffect move: numbers, user.level and target HP shares."""
      owner = ec.parse('move "M" { hit { damage_hp target 20 typed; if target.alive { damage_hp target '
                       'user.level typed; } damage_hp target max(1, target.hp * 128 >> 8) typed; '
                       'damage_hp user 5 typed; damage_hp target 7; } }')["owners"][0]
      self.assertEqual(bc.logic_hints(owner), {"fixed_amount": 20, "fixed_levels": 1, "fixed_fraction": 128})
      self.assertEqual(bc.logic_hints(None), {"fixed_amount": 0, "fixed_levels": 0, "fixed_fraction": 0})

   def test_ceffect_sources(self) -> None:
      """!The VM fixture's .ceffect logic compiles; texts and cues are named; a damaging move hits by default."""
      c = bc.compile_battle(VM_FIXTURE)
      bank = c["effects"]["bank"]
      names = c["names"]["moves"]
      charge = bank.owners[c["moves"][names.index("Charge")]["owner"]]
      hooks = [ec.HOOKS[h]["name"] for h, _f, _p in bank.handlers[charge[3]:charge[3] + charge[5]]]
      self.assertEqual(hooks, ["use", "hit"])        # the default hit phase was added
      self.assertTrue(c["moves"][names.index("Charge")]["formula"])
      self.assertEqual(len(c["texts"]), 3)
      echo = bank.owners[c["passives"][0]["owner"]]
      self.assertEqual(echo[4], 1)                   # its tag

   def test_ceffect_refusals(self) -> None:
      """!Logic twice, unknown owners, kinds a battle does not hold yet, move points, suspending residuals."""
      def refused(edit: Callable[[JsonDict], None], logic: str, pattern: str) -> None:
         with self.assertRaisesRegex(bc.ManifestError, pattern):
            self.compile_dir(edit, logic)

      def keep(_d: JsonDict) -> None:
         pass

      refused(keep, 'move "Nowhere" { use { fail; } }\n', "unknown move")
      refused(keep, 'volatile "flinch" { }\n', "unknown volatile")
      # F1: a global owner holds the encounter hooks only (the game's encounter rules)
      refused(keep, 'global "everyone" { on switch_in { fail; } }\n',
              "encounter, encounter_made, steps, combine, hatch handlers only")
      # E13b: moves have point handlers (a point the ruleset declares, with a move source)
      refused(keep, 'move "Tap" { point power { modify by 2; } }\n', "unknown point 'power'")
      refused(keep, 'move "Tap" { }\nmove "Tap" { }\n', "logic twice")
      refused(keep, 'status "Mark" { on residual { yield; } }\n', "cannot yield")
      refused(keep, 'move "Count" { use { damage; } }\n', "damaging category")
      refused(keep, 'move "Tap" { use { damage; } }\n', "damage only in a move's hit or land phase")

      def legacy(d: JsonDict) -> None:
         d["passives"][0]["handlers"] = [{"hook": "OnResidual", "effects": [{"do": "heal", "fraction": "1/4"}]}]
      refused(legacy, 'passive "Echo" { }\n', "belongs in the owner's .ceffect")
      refused(keep, 'passive "Echo" { on damaging_hit per_foe { } }\n', "per_foe is a switch_in")

   def test_stat_rename_keeps_points(self) -> None:
      """!The Starter's point handlers name categories, not stats: renaming the attack stat (and every
      reference outside the .ceffect stage statements) leaves Blaze Heart's and Scorch's handlers in place (the S3
      hazard of the legacy forms is gone)."""
      text = json.dumps(bc.view_of(FIXTURE)).replace('"attack"', '"might"')
      data = json.loads(text)
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         write_split(folder / "battle.json", data)
         (folder / "ruleset.json").write_text(RULESET.read_text(encoding="utf-8"), encoding="utf-8")
         (folder / "effects").mkdir()
         for f in (FIXTURE.parent / "effects").iterdir():
            # stage statements name the stat's stage: renamed with it
            (folder / "effects" / f.name).write_text(re.sub(r"\battack\b", "might", f.read_text(encoding="utf-8")),
                                                     encoding="utf-8")
         renamed = bc.compile_battle(folder / "battle.json")
      c = bc.compile_battle(FIXTURE)
      self.assertEqual(ec.emit_pure_cpp(renamed["point_handlers"]), ec.emit_pure_cpp(c["point_handlers"]))
      self.assertEqual(len(renamed["point_handlers"]), len(c["point_handlers"]))

   def test_positions_and_missing_sources(self) -> None:
      """!Logic errors name the source file, line and column; a listed source missing on disk is an error.
      @return None
      """
      with self.assertRaisesRegex(bc.ManifestError, r"logic\.ceffect:3:\d+: error: .*unknown"):
         self.compile_dir(lambda _d: None, 'move "Tap" {\n   hit {\n      inflict target Nowhere;\n   }\n}\n')

      def missing(d: JsonDict) -> None:
         d["effects"] = ["logic.ceffect", "gone.ceffect"]
      with self.assertRaisesRegex(bc.ManifestError, "gone.ceffect, which does not exist"):
         self.compile_dir(missing, "")

   def test_point_errors_have_one_prefix(self) -> None:
      """!A point handler's errors name their place once (no doubled "manifest:" prefix, S7).
      @return None
      """
      data = bc.view_of(FIXTURE)
      data["passives"][0]["handlers"] = [{"point": "power", "code": "{ modify by nowhere; }"}]
      try:
         bc.compile_battle(FIXTURE, None, bc.sources_of(FIXTURE, data))
      except bc.ManifestError as e:
         self.assertNotIn("manifest: manifest", str(e))
         self.assertIn("error: ", str(e))
      else:
         self.fail("the handler compiled")


## The neutral condition fixture (tests/battle_conditions_tests.cpp runs it).
CONDITIONS = ROOT / "tests/data/effects/conditions/battle.json"


class ConditionTests(unittest.TestCase):
   """!Conditions as records (D-22 A3-A5, E5): records, templates, move flags, tags, caps."""

   def compile_with(self, edit: Callable[[JsonDict], None], path: Path = CONDITIONS) -> JsonDict:
      """!Compiles a fixture after an edit, from memory.
      @param edit  callable(battle dict)
      @param path  the fixture
      @return compiled
      """
      data = bc.view_of(path)
      edit(data)
      return bc.compile_battle(path, None, bc.sources_of(path, data))

   def test_sample_records(self) -> None:
      """!The Starter's volatiles (a turn flinch, a counted daze), move flags as manifest indices, 4-bit status ids,
      the template handlers by label.
      @return None
      """
      c = bc.compile_battle(FIXTURE)
      vols = {v["source_name"]: v for v in c["volatiles"]}
      self.assertEqual(list(vols), ["flinch", "dazed"])
      self.assertEqual(vols["dazed"]["counter"], 0)
      self.assertTrue(vols["flinch"]["flags"] & bc.VOLATILE_TURN)
      self.assertEqual(c["move_flags"], ["contact", "protect"])
      self.assertEqual(c["layout"]["status_bits"], 4)
      labels = c["effects"]["bank"].labels
      for label in ('status "Scorch" on try_start', 'status "Scorch" on residual', 'status "Doze" on start',
                    'status "Shock" on before_move', 'volatile "flinch" on before_move', 'volatile "dazed" on end'):
         self.assertIn(label, labels)
      self.assertNotIn('weather "Storm" on field_residual', labels)   # no residual: no phase

   def test_templates(self) -> None:
      """!The declarative fields' expansion (the draw order of the engine's former can_act / end of turn).
      @return None
      """
      c = bc.compile_battle(FIXTURE)
      scorch = c["statuses"][c["names"]["statuses"].index("Scorch") + 1]
      code = bc.status_template("Scorch", scorch)
      self.assertIn('on try_start when holder.has_type("Fire") { immune; }', code)
      # the Starter's 1/12 in Q8 (C2)
      self.assertIn("on residual { text residual; damage_hp holder max(1, holder.max_hp * 21 >> 8); }", code)
      shock = c["statuses"][c["names"]["statuses"].index("Shock") + 1]
      self.assertIn("on before_move { if chance(1, 5) { text skip; stop; } }", bc.status_template("Shock", shock))
      vols = {v["source_name"]: v for v in c["volatiles"]}
      code = bc.volatile_template("dazed", vols["dazed"])
      self.assertIn('between(1, 3)', code)
      self.assertIn('say "became_dazed" at holder;', code)
      self.assertIn('vol_off holder "dazed";', code)

   def test_fixture_records(self) -> None:
      """!The neutral fixture: words, counters, hud, tags, side layers and turns, field turns, own texts.
      @return None
      """
      c = bc.compile_battle(CONDITIONS)
      vols = {v["source_name"]: v for v in c["volatiles"]}
      self.assertEqual(vols["veil"]["word"], 0)
      self.assertEqual(vols["rampage"]["counter"], 0)
      self.assertTrue(vols["charging"]["flags"] & bc.VOLATILE_HUD)
      self.assertFalse(vols["charging"]["flags"] & bc.VOLATILE_FAINT)
      self.assertEqual([s["layers"] for s in c["side_conditions"]], [3, 1])
      self.assertEqual([s["turns"] for s in c["side_conditions"]], [0, 3])
      self.assertEqual(c["field_conditions"][0]["turns"], 2)
      self.assertEqual(c["layout"]["status_bits"], 4)
      self.assertEqual(len(c["move_flags"]), 6)   # beyond the sample's set
      own = [t for t in c["effect_catalog"].entries["texts"] if t.startswith("volatile veil")]
      self.assertEqual(own, ["volatile veil: start", "volatile veil: end"])
      cpp = bc.emit("battle.json", c)
      self.assertIn("const SideCondData kSideConds[]", cpp)
      ids = bc.emit_ids("battle.json", c)
      for name in ("VOLATILE_VEIL", "SIDE_COND_THORNS", "FIELD_COND_HAZE", "FLAG_PIERCE", "TAG_FLOATING"):
         self.assertIn(name, ids)

   def test_caps_refused(self) -> None:
      """!The engine's caps: 16 move flags, 32 volatiles, 8 counters, 4 words, 16 side / field conditions, 16 tags,
      63 statuses, status ids that fit their bits.
      @return None
      """
      def many(key: str, n: int, extra: JsonDict | None = None) -> Callable[[JsonDict], None]:
         """!An edit giving a list n records."""
         def edit(d: JsonDict) -> None:
            d[key] = [{"name": f"c{i}", **(extra or {})} for i in range(n)]
         return edit

      def flags(d: JsonDict) -> None:
         d["move_flags"] = [f"f{i}" for i in range(bc.MAX_MOVE_FLAGS + 1)]
      cases = [(flags, "at most 16 move_flags"),
               (many("volatiles", bc.MAX_VOLATILES + 1), "at most 32 records"),
               (many("volatiles", bc.MAX_COUNTERS + 1, {"counter": True}), "at most 8 volatile counters"),
               (many("volatiles", bc.MAX_WORDS + 1, {"word": True}), "4 words"),
               (many("side_conditions", bc.MAX_SIDE_CONDS + 1), "at most 16 records"),
               (many("field_conditions", bc.MAX_FIELD_CONDS + 1), "at most 16 records")]

      def tags(d: JsonDict) -> None:
         d["tags"] = [f"t{i}" for i in range(bc.MAX_TAGS + 1)]
      cases.append((tags, "at most 16"))

      def statuses(d: JsonDict) -> None:
         d["statuses"] = [{"name": f"S{i}", "texts": {"inflicted": "x", "cured": "x", "already": "x"}}
                          for i in range(bc.MAX_STATUSES + 1)]
      cases.append((statuses, "at most 63 statuses"))

      def narrow(d: JsonDict) -> None:
         d["monster_layout"]["status_bits"] = 1
      cases.append((narrow, "1 bits hold ids up to 1"))
      for edit, pattern in cases:
         with self.subTest(pattern=pattern):
            with self.assertRaisesRegex(bc.ManifestError, pattern):
               self.compile_with(edit)

   def test_record_refusals(self) -> None:
      """!Record fields that cannot work are refused (the old rules keys, unknown flags, chains on a volatile that
      does not stay on, texts naming no message).
      @return None
      """
      def confusion(d: JsonDict) -> None:
         d["rules"]["confusion"] = {"power": 40}
      with self.assertRaisesRegex(bc.ManifestError, "rules.confusion is gone"):
         self.compile_with(confusion)

      def chain(d: JsonDict) -> None:
         d["volatiles"].append({"name": "c", "duration": "turn", "chain": {"den": 2, "cap": 3}})
      with self.assertRaisesRegex(bc.ManifestError, "stays on"):
         self.compile_with(chain)

      def kept(d: JsonDict) -> None:
         d["volatiles"].append({"name": "c", "chain": {"den": 2, "cap": 3, "kept_by": "nowhere"}})
      with self.assertRaisesRegex(bc.ManifestError, "kept_by: a move flag"):
         self.compile_with(kept)

      def message(d: JsonDict) -> None:
         d["volatiles"][0]["texts"]["start"] = {"message": "nowhere"}
      with self.assertRaisesRegex(bc.ManifestError, "names an engine message"):
         self.compile_with(message)

      def thaw(d: JsonDict) -> None:
         d["statuses"][0]["thawed_by_move"] = True
      with self.assertRaisesRegex(bc.ManifestError, "name the move flag"):
         self.compile_with(thaw)

      def flag(d: JsonDict) -> None:
         d["moves"][0]["flags"] = ["nowhere"]
      with self.assertRaisesRegex(bc.ManifestError, "the manifest's move_flags"):
         self.compile_with(flag)

      def point(d: JsonDict) -> None:
         d["effects"] = ["conditions.ceffect"]
      # E13b: volatiles have point handlers (sources user.volatile / target.volatile); this ruleset has no points
      with self.assertRaisesRegex(bc.ManifestError, "volatile hover: .ceffect point power: unknown point 'power'"):
         data = bc.view_of(CONDITIONS)
         point(data)
         text = (CONDITIONS.parent / "conditions.ceffect").read_text(encoding="utf-8")
         unsaved = text + 'volatile "hover" {\n   point power { modify by 2; }\n}\n'
         bc.compile_battle(CONDITIONS, None, {**bc.sources_of(CONDITIONS, data),
                                              (CONDITIONS.parent / "conditions.ceffect").resolve(): unsaved})


## E13b's neutral fixture (the battle primitives).
MOVES_FIXTURE = ROOT / "tests/data/battle_moves/battle.json"


class PrimitiveTests(unittest.TestCase):
   """!E13b: move / side / volatile point handlers, the move_type point, the delayed phase, species fields read by
   logic, the studio exports."""

   def compile_with(self, edit: Callable[[JsonDict], None], logic: str | None = None) -> JsonDict:
      """!Compiles the fixture after an edit (and with other logic), from memory.
      @param edit   callable(battle dict)
      @param logic  moves.ceffect text, or None for the file's
      @return compiled
      """
      data = bc.view_of(MOVES_FIXTURE)
      edit(data)
      sources = bc.sources_of(MOVES_FIXTURE, data)
      if logic is not None:
         sources[(MOVES_FIXTURE.parent / "moves.ceffect").resolve()] = logic
      return bc.compile_battle(MOVES_FIXTURE, None, sources)

   def test_point_sources(self) -> None:
      """!The ruleset's new sources map to PointSource in order; the fixture's owners get their point handlers.
      @return None
      """
      self.assertEqual(rc.POINT_SOURCES[-5:], ["move", "user.side", "target.side", "user.volatile",
                                               "target.volatile"])
      c = bc.compile_battle(MOVES_FIXTURE)
      labels = [e["label"] for e in c["point_handlers"]]
      for label in ("move Gust: point power", "move Shift: point move_type", "side_condition Screen: point damage",
                    "volatile guarded: point damage", "move Heave: point power", "weather Dust: point damage"):
         self.assertIn(label, labels)
      gust = c["moves"][c["names"]["moves"].index("Gust")]
      self.assertEqual((gust["handler_count"], gust["point_mask"]), (1, 1 << c["pipelines"]["points"]["power"]["id"]))
      out = bc.emit("battle.json", c)
      self.assertIn("PointSource::TargetSide, PointSource::TargetVolatile, PointSource::Weather", out)
      # Rules::move_type_point: the move_type point's id after self_hit_category
      self.assertEqual(c["pipelines"]["points"][rc.MOVE_TYPE_POINT]["id"], 2)
      self.assertIn(", 0, 2,\n", out)

   def test_hit_type_category(self) -> None:
      """!C5: the hit type's category (structure hit_category_by_type) for a damaging move whose type the move_type
      point changed (Shift in the dust: Stone, blast); the move's own otherwise; the point runner reads both; the
      move_type point goes to the engine only while a handler modifies it.
      @return None
      """
      c = bc.compile_battle(MOVES_FIXTURE)
      cats, types = c["names"]["categories"], c["names"]["types"]
      self.assertEqual([cats[k] for k in c["hit_categories"]], ["strike", "blast"])
      shift, tap = c["names"]["moves"].index("Shift"), c["names"]["moves"].index("Tap")
      wait = c["names"]["moves"].index("Wait")
      stone, plain = types.index("Stone"), types.index("Plain")
      self.assertEqual(cats[bc.category_of(c, shift, stone)], "blast")
      self.assertEqual(cats[bc.category_of(c, shift, plain)], "strike")   # its own type: its own category
      self.assertEqual(cats[bc.category_of(c, wait, stone)], "support")   # not damaging
      self.assertTrue(bc.point_handled(c, rc.MOVE_TYPE_POINT))
      out = bc.emit("battle.json", c)
      self.assertIn("const uint8_t kHitCategories[] = {0, 2};", out)
      self.assertIn("    kHitCategories, kStats, kMessages,", out)
      # without the Shift's handler no hit runs the point (Rules::move_type_point 255)
      logic = (MOVES_FIXTURE.parent / "moves.ceffect").read_text(encoding="utf-8")
      start = logic.index('move "Shift"')
      bare = logic[:start] + 'move "Shift" {\n}\n' + logic[logic.index("# The weather's damage point"):]
      plain_c = self.compile_with(lambda d: None, bare)
      self.assertFalse(bc.point_handled(plain_c, rc.MOVE_TYPE_POINT))
      self.assertIn(", 0, 255,\n", bc.emit("battle.json", plain_c))
      # the record's species key through its role (no RoleId literal), unstored roles' ids apart
      self.assertNotIn("RoleId{0}", out)

   def test_point_runner(self) -> None:
      """!The Python point runner reads the new sources as the engine does (the move's handler, a target's side
      condition and volatile).
      @return None
      """
      c = bc.compile_battle(MOVES_FIXTURE)
      gust = c["names"]["moves"].index("Gust")
      user = {"hp": 10, "volatiles": 0}
      aloft = {"hp": 10, "volatiles": 1 << c["names"]["volatiles"].index("aloft")}
      self.assertEqual(bc.point_runner(c, user, aloft, gust)("power", 30), 60)
      self.assertIsNone(bc.point_runner(c, user, {"hp": 10}, c["names"]["moves"].index("Tap"))("power", 30))
      screen = {"hp": 10, "side_conditions": 1, "index": 1}
      tap = c["names"]["moves"].index("Tap")
      self.assertEqual(bc.point_runner(c, user, screen, tap)("damage", 40), 20)
      self.assertEqual(bc.point_runner(c, user, screen, tap, crit=1)("damage", 40), 40)
      guarded = {"hp": 10, "volatiles": 1 << c["names"]["volatiles"].index("guarded")}
      self.assertEqual(bc.point_runner(c, user, guarded, tap)("damage", 40), 20)
      # a gene and the side's active battlers (Knack: 10 + 4 x gene + 10 x active)
      knack = c["names"]["moves"].index("Knack")
      geneful = {"hp": 10, "genes": [0, 3, 0], "side_active": 2}
      self.assertEqual(bc.point_runner(c, geneful, guarded, knack)("power", 1), 42)

   def test_delayed_phase(self) -> None:
      """!`delay` needs the delayed end-of-turn phase; the phase appears at most once.
      @return None
      """
      def without(d: JsonDict) -> None:
         d["turn"]["end_of_turn"] = [e for e in d["turn"]["end_of_turn"] if e["run"] != "delayed"]
      with self.assertRaisesRegex(bc.ManifestError, "uses `delay`"):
         self.compile_with(without)

      def twice(d: JsonDict) -> None:
         d["turn"]["end_of_turn"].append({"run": "delayed"})
      with self.assertRaisesRegex(bc.ManifestError, "the delayed phase may appear only once"):
         self.compile_with(twice)
      logic = (MOVES_FIXTURE.parent / "moves.ceffect").read_text(encoding="utf-8").replace("delay target 25 2;", "")
      self.compile_with(without, logic)   # no `delay`: no phase needed

   def test_field_points_refused(self) -> None:
      """!Field conditions still have no point source.
      @return None
      """
      def field(d: JsonDict) -> None:
         d["field_conditions"] = [{"name": "Fog", "turns": 2}]
      logic = (MOVES_FIXTURE.parent / "moves.ceffect").read_text(encoding="utf-8") + \
         'field_condition "Fog" {\n   point damage { modify by 2; }\n}\n'
      with self.assertRaisesRegex(bc.ManifestError, "field conditions have no point source"):
         self.compile_with(field, logic)

   def test_species_fields_for_logic(self) -> None:
      """!rules::species_field_of(): the table when logic reads a field, 0 without a reader.
      @return None
      """
      out = bc.emit("battle.json", bc.compile_battle(MOVES_FIXTURE))
      self.assertIn("int32_t species_field_of(int32_t species, int32_t field) {", out)
      self.assertIn("kSpeciesFields[species][field]", out)
      logic = (MOVES_FIXTURE.parent / "moves.ceffect").read_text(encoding="utf-8")
      logic = logic.replace("modify set target.species_field(mass);", "modify set 40;")
      logic = logic.replace("value user.species_field(mass)", "value 30")
      out = bc.emit("battle.json", self.compile_with(lambda d: None, logic))
      self.assertNotIn("kSpeciesFields", out)
      self.assertIn("return 0;   // no species field is read", out)

   def test_studio_exports(self) -> None:
      """!The names the studio reads from the battle compiler (S11's list).
      @return None
      """
      self.assertEqual(bc.PRESENTER_TEXTS, ("intro", "defeat", "victory"))
      self.assertTrue(set(bc.STILL_MOTIONS) < set(bc.MOTIONS))
      self.assertEqual(set(bc.STAT_INPUT_ROLES), {"gene", "training", "awaken"})
      self.assertTrue(set(bc.STAT_INPUT_ROLES) < set(rc.STAT_INPUTS))
      self.assertTrue(bc.MESSAGE_KEY.match("decoy_made"))
      self.assertIsInstance(bc.RETIRED_MESSAGES, dict)


class TurnTests(unittest.TestCase):
   """!The turn structure from the manifest (D-22 A11, E6b)."""

   def compile_with(self, edit: Callable[[JsonDict], None], path: Path = FIXTURE) -> JsonDict:
      """!Compiles a fixture after an edit, from memory.
      @param edit  callable(battle dict)
      @param path  the fixture
      @return compiled
      """
      data = bc.view_of(path)
      edit(data)
      return bc.compile_battle(path, None, bc.sources_of(path, data))

   def test_sample_is_the_default(self) -> None:
      """!The Starter writes out the default turn structure plus its regen phase (E8).
      @return None
      """
      sample = bc.compile_battle(FIXTURE)["rules"]["turn"]

      def defaults(d: JsonDict) -> None:
         d["turn"] = {"end_of_turn": [{"run": "weather"}, {"run": "battlers"}, {"run": "sides"}, {"run": "field"},
                                      {"run": "regen"}]}
      self.assertEqual(self.compile_with(defaults)["rules"]["turn"], sample)
      self.assertEqual(sample["brackets"], {"run": 30, "switch": 20, "item": 25, "move": 0, "move_priority": 1})
      self.assertEqual(sample["effect_order"], list(range(len(bc.SOURCES))))
      self.assertEqual([bc.END_PHASES[e["phase"]] for e in sample["end"]],
                       ["weather", "battlers", "sides", "field", "regen"])
      self.assertEqual(sample["end"][1]["sources"], sample["effect_order"])
      self.assertEqual((sample["tie"], sample["send_order"], sample["start_hooks"]), (0, 0, 0))

   def test_fixture(self) -> None:
      """!The neutral fixture's turn compiles as written and emits its TurnRules.
      @return None
      """
      c = bc.compile_battle(TURN_FIXTURE)
      tr = c["rules"]["turn"]
      self.assertEqual(tr["brackets"]["switch"], -10)
      self.assertEqual(tr["tie"], bc.TIE_RULES.index("position"))
      self.assertEqual([bc.END_PHASES[e["phase"]] for e in tr["end"]], ["field", "battlers", "sides", "weather"])
      self.assertEqual([bc.SOURCES[s] for s in tr["end"][1]["sources"]], ["passive", "status"])
      self.assertEqual(tr["end"][2]["each"], bc.PHASE_EACH.index("once"))
      self.assertEqual(bc.SOURCES[tr["effect_order"][0]], "passive")
      cpp = bc.emit("battle.json", c)
      self.assertIn("{30, -10, 25, 0, 1, 1, 1, 1, {5, 6, 0, 1, 2, 3, 4}, 4, {", cpp)
      self.assertIn("{1, 0, 2, {5, 0, 0, 0, 0, 0, 0}},", cpp)

   def test_move_bracket(self) -> None:
      """!"move" may be one bracket for every move instead of their priorities.
      @return None
      """
      def one(d: JsonDict) -> None:
         d["turn"]["brackets"] = {"move": -3}
      br = self.compile_with(one)["rules"]["turn"]["brackets"]
      self.assertEqual((br["move"], br["move_priority"]), (-3, 0))

   def test_errors(self) -> None:
      """!Bad turn structures are refused with a message.
      @return None
      """
      cases = [
         ({"brackets": {"run": 64}}, "brackets.run: must be an integer -64-63"),
         ({"brackets": {"move": "speed"}}, "brackets.move"),
         ({"brackets": {"flee": 1}}, "unknown key"),
         ({"tie": "coin"}, "tie: one of random_byte, position"),
         ({"effect_order": ["status", "passive"]}, "must list every source"),
         ({"effect_order": ["status"] * len(bc.SOURCES)}, "listed twice"),
         ({"end_of_turn": [{"run": "battlers"}, {"run": "sides"}, {"run": "field"}]},
          "the weather phase must appear exactly once"),
         ({"end_of_turn": [{"run": "weather"}, {"run": "weather"}, {"run": "battlers"}, {"run": "sides"},
                           {"run": "field"}]}, "the weather phase must appear exactly once"),
         ({"end_of_turn": [{"run": "weather"}, {"run": "sides"}, {"run": "field"}]}, "needs a battlers phase"),
         ({"end_of_turn": [{"run": "weather", "sources": ["status"]}, {"run": "battlers"}, {"run": "sides"},
                           {"run": "field"}]}, "only a battlers phase lists sources"),
         ({"end_of_turn": [{"run": "weather", "each": "once"}, {"run": "battlers"}, {"run": "sides"},
                           {"run": "field"}]}, "only a sides or field phase"),
         ({"end_of_turn": [{"run": "weather"}, {"run": "battlers", "sources": ["status", "status"]},
                           {"run": "sides"}, {"run": "field"}]}, "listed twice"),
         ({"end_of_turn": [{"run": "weather"}, {"run": "battlers"}] * 3 + [{"run": "battlers"}] * 3},
          "a list of 1-8 phases"),
         ({"switching": {"send_out": "player_first"}}, "switching.send_out: one of foes_first, position"),
         ({"switching": {"switch_in": "random"}}, "switching.switch_in: one of speed, send_out"),
      ]
      for turn, message in cases:
         def edit(d: JsonDict, turn: JsonDict = turn) -> None:
            d["turn"] = turn
         with self.subTest(turn=turn):
            with self.assertRaisesRegex(bc.ManifestError, re.escape(message)):
               self.compile_with(edit)


## The progression fixture (E6c): non-default learn / exp_policy / generation and its own pp_max.
PROGRESS_FIXTURE = ROOT / "tests/data/battle_progress/battle.json"


class ProgressionTests(unittest.TestCase):
   """!Progression from the manifest (D-22 A8 / A12, E6c): learn, exp_policy, generation, pp_max."""

   def compile_with(self, edit: Callable[[JsonDict], None], path: Path = FIXTURE) -> JsonDict:
      """!Compiles a fixture after an edit, from memory.
      @param edit  callable(battle dict)
      @param path  the fixture
      @return compiled
      """
      data = bc.view_of(path)
      edit(data)
      return bc.compile_battle(path, None, bc.sources_of(path, data))

   def test_sample_states_its_rules(self) -> None:
      """!The sample writes out its rules (the engine's former fixed ones); the defaults are neutral (D-22 decision
      8): learn.default_moves and exp_policy.share are required, no generation list draws nothing.
      @return None
      """
      sample = bc.compile_battle(FIXTURE)

      def drop(d: JsonDict) -> None:
         d["learn"] = {"default_moves": "last_n_by_level"}
         d["exp_policy"] = {"share": "faced"}
         del d["generation"]
      dropped = self.compile_with(drop)
      self.assertEqual(dropped["rules"]["progress"]["learn"], sample["rules"]["progress"]["learn"])
      self.assertEqual(dropped["rules"]["progress"]["exp"], sample["rules"]["progress"]["exp"])
      self.assertEqual(dropped["rules"]["progress"]["generation"], [])
      for key in ("learn", "exp_policy"):
         with self.subTest(required=key):
            with self.assertRaisesRegex(bc.ManifestError, "required"):
               self.compile_with(lambda d, k=key: d.pop(k))
      gen = sample["rules"]["progress"]["generation"]
      self.assertEqual([(e["role"], bc.GEN_KINDS[e["kind"]]) for e in gen],
                       [("seed", "wide"), ("stat_genes", "random"), ("trait", "random"),
                        ("passive_selector", "passive_slot")])
      self.assertEqual((gen[0]["span"], gen[1]["span"], gen[1]["count"]), ((1 << 32) - 1, 16, 9))   # 4-bit genes
      self.assertEqual(sample["rules"]["progress"]["learn"], {"on_full": 0, "default_moves": 0})

   def test_pp_max_slot(self) -> None:
      """!The pp_max slot is gone (D-25): a slot-shaped pp_max is refused with the migration message; the sample's
      pp_max is a named formula the move resource pp names as its max.
      @return None
      """
      ruleset = STATS3.parent / "ruleset.json"   # a PP-shaped neutral set (pp with raises and a max formula)
      data = json.loads(ruleset.read_text(encoding="utf-8"))
      del data["formulas"]["pp_max"]["args"]
      with self.assertRaisesRegex(rc.RulesetError, "the pp_max slot is gone"):
         rc.load(bc.strip(data))
      sample = rc.load(bc.strip(json.loads(ruleset.read_text(encoding="utf-8"))))
      self.assertEqual(rc.run(sample, "pp_max", pp_base=35, pp_ups=3), 61)   # 35 + 35 * 3 / 4
      self.assertNotIn("pp_max", rc.FORMULA_SLOTS)
      c = bc.compile_battle(STATS3)
      self.assertEqual(c["resource_formulas"], ["pp_max"])
      self.assertIn("case 0: return formula_pp_max(a, b);", bc.emit("battle.json", c))

   def test_fixture(self) -> None:
      """!The neutral fixture's progression compiles as written: kinds, derived roles and their C++.
      @return None
      """
      c = bc.compile_battle(PROGRESS_FIXTURE)
      pr = c["rules"]["progress"]
      self.assertEqual(pr["learn"], {"on_full": bc.LEARN_ON_FULL.index("request"),
                                     "default_moves": bc.DEFAULT_MOVE_RULES.index("first_n")})
      self.assertEqual(pr["exp"], {"share": bc.EXP_SHARES.index("party"),
                                   "receivers": bc.EXP_RECEIVERS.index("opposing_sides"),
                                   "outsider": bc.EXP_OUTSIDERS.index("owner_id")})
      kinds = [(e["role"], bc.GEN_KINDS[e["kind"]]) for e in pr["generation"]]
      self.assertEqual(kinds, [("seed", "random"), ("stat_genes", "constant"), ("pp_ups", "constant"),
                               ("gender", "derived"), ("trait", "derived"), ("held_item", "random")])
      gender = pr["generation"][3]
      self.assertEqual(bc.run_derived(gender, {"seed_lo": 8}), 2)
      self.assertEqual(pr["generation"][4]["derived"], 1)
      cpp = bc.emit("battle.json", c)
      self.assertIn("int32_t derive(int32_t index, int32_t element, const monster::Record& rec) {", cpp)
      self.assertIn("case 1: {   // trait = seed_lo % TRAIT_COUNT", cpp)
      self.assertIn("{kGeneration, 6, 1, 1, 2, 1, 1}", cpp)

   def test_errors(self) -> None:
      """!Wrong progression entries are refused with a message naming them.
      @return None
      """
      cases = [
         ("learn", {"on_full": "forget", "default_moves": "first_n"},
          "learn.on_full: must be one of skip, request, replace_oldest"),
         ("learn", {"on_full": "skip"}, "learn.default_moves: required"),
         ("exp_policy", {"receivers": "human_side"}, "exp_policy.share: required"),
         ("learn", {"order": "first", "default_moves": "first_n"}, "unknown key"),
         ("exp_policy", {"share": "all"}, "exp_policy.share: must be one of faced, active, party"),
         ("generation", [{"role": "level", "constant": 5}], "is set by the engine"),
         ("generation", [{"role": "luck", "constant": 5}], "neither an engine role nor a custom role"),
         ("generation", [{"role": "seed", "random_range": [0, 100000]}], "must be a power of two"),
         ("generation", [{"role": "seed", "constant": 1, "random_range": [0, 1]}], "exactly one of"),
         ("generation", [{"role": "trait", "random_range": [0, 40]}], "must be an integer 0-31"),
         ("generation", [{"role": "held_item", "random_range": [0, "last"]}], "'last' is the last value of"),
         ("generation", [{"role": "trait", "derived": "level % 99", "range": [0, 40]}], "must lie within [0, 31]"),
         ("generation", [{"role": "trait", "derived": "luck"}], "luck"),
         ("generation", [{"role": "seed", "constant": 1}] * (bc.MAX_GENERATION + 1), "at most 32 entries"),
      ]
      for key, value, message in cases:
         def edit(d: JsonDict, key: str = key, value: Any = value) -> None:
            d[key] = value
         with self.subTest(key=key, value=value):
            with self.assertRaisesRegex(bc.ManifestError, re.escape(message)):
               self.compile_with(edit)

      def no_owner(d: JsonDict) -> None:
         d["exp_policy"] = {"share": "faced", "outsider": "owner_id"}
         d["monster_layout"]["roles"] = [r for r in d["monster_layout"]["roles"] if r != "owner_id"]
      with self.assertRaisesRegex(bc.ManifestError, "the layout does not hold"):
         self.compile_with(no_owner)

   def test_pp_max_inputs_cover_the_engine(self) -> None:
      """!A ruleset's own pp_max must take every base PP and every raise the layout holds.
      @return None
      """
      path = (STATS3.parent / "ruleset.json").resolve()
      data = json.loads(path.read_text(encoding="utf-8"))
      data["inputs"]["pp_ups"] = [0, 1]
      with self.assertRaisesRegex(bc.ManifestError, re.escape("input 'pp_ups' must cover [0, 7]")):   # 3-bit raises
         bc.compile_battle(STATS3, None, {path: data})


class NeutralDefaultTests(unittest.TestCase):
   """!E7 (D-22 decision 8): neutral defaults, required values, plural forms, field placeholders, transfer keys."""

   def compile_with(self, edit: Callable[[JsonDict], None], **kw: Any) -> JsonDict:
      """!Compiles the sample after an edit, from memory.
      @param edit  callable(battle dict)
      @param kw    more compile_battle() keywords
      @return compiled
      """
      data = bc.view_of(FIXTURE)
      edit(data)
      return bc.compile_battle(FIXTURE, None, bc.sources_of(FIXTURE, data), **kw)

   def test_rules_defaults_are_the_engine_limits(self) -> None:
      """!Left-out rules take the engine's limits and the lowest self-hit power, not a game's numbers."""
      def drop(d: JsonDict) -> None:
         for key in ("stat_cap", "level_max"):
            del d["rules"][key]
         del d["rules"]["self_hit"]["power"]
         for sp in d["species"]:
            sp.pop("exp_yield")
      rs = json.loads(RULESET.read_text(encoding="utf-8"))
      self.assertLess(rs["inputs"]["level"][1], bc.MAX_LEVEL_MAX)
      with self.assertRaisesRegex(bc.ManifestError, "input 'level' must cover \\[1, 255\\]"):
         self.compile_with(drop)   # the sample's ruleset is written for its own level cap
      self.assertEqual((bc.DEFAULT_STAT_CAP, bc.DEFAULT_LEVEL_MAX, bc.DEFAULT_SELF_HIT_POWER, bc.DEFAULT_EXP_YIELD),
                       (0xFFFF, 255, 1, 0))

   def test_required_values(self) -> None:
      """!A battle states its party size, and a trait that raises or lowers a stat its percentages."""
      with self.assertRaisesRegex(bc.ManifestError, "a battle states its party size"):
         self.compile_with(lambda d: d.update(monster_layout=d["monster_layout"]["roles"]))
      with self.assertRaisesRegex(bc.ManifestError, "needs party_size"):
         self.compile_with(lambda d: d["monster_layout"].pop("party_size"))
      # E8: move_slots is required as well (compile_layout() alone keeps the registry's 4 for transfer fixtures)
      with self.assertRaisesRegex(bc.ManifestError, "needs move_slots"):
         self.compile_with(lambda d: d["monster_layout"].pop("move_slots"))

   def test_escalation_cap(self) -> None:
      """!An escalating status without a cap takes the counter's byte."""
      def bare(d: JsonDict) -> None:
         next(st for st in d["statuses"] if st["name"] == "Blight")["escalating"] = True
      self.assertNotEqual(bc.emit("battle.json", self.compile_with(bare)), bc.emit("battle.json",
                                                                                   bc.compile_battle(FIXTURE)))
      import battlec.conditions as bcc
      self.assertEqual(bcc.DEFAULT_ESCALATION_CAP, bc.U8_MAX)

   def test_plurals(self) -> None:
      """!plural_of(): English endings, irregular words, abbreviations; the terms' plural rule; given forms win."""
      cases = {"Ability": "Abilities", "Day": "Days", "Box": "Boxes", "Status": "Statuses", "Held item": "Held items",
               "EXP": "EXP", "Species": "Species", "Child": "Children", "Creature": "Creatures", "": ""}
      for one, many in cases.items():
         with self.subTest(one=one):
            self.assertEqual(bc.plural_of(one), many)
      self.assertEqual(bc.plural_of("Ability", "same"), "Ability")
      terms = bc.compile_terms({"passive": "Ability", "category": "Category"}, "t")
      self.assertEqual((terms["passive"]["many"], terms["category"]["many"]), ("Abilities", "Categories"))
      self.assertEqual(terms["status"]["many"], "Statuses")   # a default word
      same = bc.compile_terms({"plural": "same", "party": "Equipe", "trait": {"one": "Trait", "many": "Traits"}}, "t")
      self.assertEqual((same["party"]["many"], same["trait"]["many"], same["category"]["many"]),
                       ("Equipe", "Traits", "Categories"))   # the defaults are English
      with self.assertRaisesRegex(bc.ManifestError, "plural: one of english, same"):
         bc.compile_terms({"plural": "latin"}, "t")
      sample = bc.compile_battle(FIXTURE)["terms"]
      self.assertEqual((sample["passive"]["many"], sample["reward"]["many"]), ("Talents", "Coins"))

   def test_field_placeholders(self) -> None:
      """!With the game's scripts manifest, battle texts may use its field placeholders (FD 80 + index)."""
      def say(d: JsonDict) -> None:
         d["messages"]["used_move"] = "{user} used {move} for {player}!"
      with self.assertRaisesRegex(bc.ManifestError, "player"):
         self.compile_with(say)   # no scripts manifest: not a placeholder here
      with tempfile.TemporaryDirectory() as tmp:
         scripts = Path(tmp) / "scripts.json"
         scripts.write_text(json.dumps({"schema": "capsule.scripts/1", "sources": [],
                                        "placeholders": {"rival": {"name": 7}, "player": {"name": 7}}}),
                            encoding="utf-8")
         c = self.compile_with(say, scripts=scripts)
         codes = c["messages"][bc.MESSAGES.index("used_move")]
         self.assertIn([0xFD, 0x81], [codes[i:i + 2] for i in range(len(codes) - 1)])   # {player}: the second
         scripts.write_text(json.dumps({"schema": "capsule.scripts/1", "sources": [],
                                        "placeholders": {"user": {"name": 7}}}), encoding="utf-8")
         with self.assertRaisesRegex(bc.ManifestError, "also battle placeholders"):
            self.compile_with(say, scripts=scripts)

   def test_transfer_keys_are_checked(self) -> None:
      """!The battle compiler checks the transfer defaults' stat names (not only the transfer table compile)."""
      with self.assertRaisesRegex(bc.ManifestError, "transfer_defaults names no stat: luck"):
         self.compile_with(lambda d: d["monster_layout"].update(transfer_defaults={"luck": 3}))


class CategoryByTypeTests(unittest.TestCase):
   """!A category rule by type (E11, structure.json "category_by_type"): a damaging move's category is its type's."""

   def compile_with(self, edit: Callable[[JsonDict], None]) -> JsonDict:
      """!Compiles the Starter after an edit, from memory.
      @param edit  callable(battle dict)
      @return compiled
      """
      data = bc.view_of(FIXTURE)
      edit(data)
      return bc.compile_battle(FIXTURE, None, bc.sources_of(FIXTURE, data))

   @staticmethod
   def by_type(d: JsonDict) -> None:
      """!Earth and Wind decide physical, every other type elemental; damaging moves state no category.
      @param d  battle dict (edited)
      @return None
      """
      d["category_by_type"] = {t["name"]: "physical" if t["name"] in ("Earth", "Wind") else "elemental"
                               for t in d["types"]}
      for m in d["moves"]:
         if m.get("category") != "status":
            m.pop("category", None)

   def test_moves_take_their_type_category(self) -> None:
      """!Damaging moves get their type's category; status moves keep theirs."""
      c = self.compile_with(self.by_type)
      cats, types = c["names"]["categories"], c["names"]["types"]
      for m, name in zip(c["moves"], c["names"]["moves"]):
         with self.subTest(move=name):
            if cats[m["category"]] != "status":
               want = "physical" if types[m["type"]] in ("Earth", "Wind") else "elemental"
               self.assertEqual(cats[m["category"]], want)
      self.assertEqual(cats[c["moves"][c["names"]["moves"].index("Smolder")]["category"]], "status")

   def test_refusals(self) -> None:
      """!A damaging move naming another category, a missing or unknown type, a non-damaging category."""
      def refused(edit: Callable[[JsonDict], None], pattern: str) -> None:
         with self.assertRaisesRegex(bc.ManifestError, pattern):
            self.compile_with(edit)

      def other(d: JsonDict) -> None:
         self.by_type(d)
         next(m for m in d["moves"] if m["name"] == "Flare Claw")["category"] = "physical"   # Fire: elemental
      refused(other, "category_by_type decides a damaging move's category")
      refused(lambda d: (self.by_type(d), d["category_by_type"].pop("Bio")), "missing Bio")
      refused(lambda d: (self.by_type(d), d["category_by_type"].update(Ice="physical")), "unknown type")
      refused(lambda d: (self.by_type(d), d["category_by_type"].update(Bio="status")), "not a damaging category")
      refused(lambda d: d.update(category_by_type=["physical"]), "an object")
      self.assertEqual(bc.view_of(FIXTURE).get("category_by_type"), None)   # the Starter states categories per move

   def test_hit_categories(self) -> None:
      """!C5: hit_category_by_type gives the hit types' categories (every type, damaging ones); category_by_type gives
      them too; both at once are refused."""
      def hit_table(d: JsonDict) -> None:
         d[bc.HIT_CATEGORY_KEY] = {t["name"]: "physical" if t["name"] == "Earth" else "elemental"
                                   for t in d["types"]}

      c = self.compile_with(hit_table)
      cats, types = c["names"]["categories"], c["names"]["types"]
      self.assertEqual(cats[c["hit_categories"][types.index("Earth")]], "physical")
      self.assertEqual(cats[c["hit_categories"][types.index("Fire")]], "elemental")
      self.assertIsNone(self.compile_with(lambda d: None)["hit_categories"])
      by_type = self.compile_with(self.by_type)
      self.assertEqual(cats[by_type["hit_categories"][types.index("Wind")]], "physical")
      with self.assertRaisesRegex(bc.ManifestError, "hit_category_by_type: the structure's category_by_type"):
         self.compile_with(lambda d: (self.by_type(d), hit_table(d)))
      with self.assertRaisesRegex(bc.ManifestError, "hit_category_by_type: every type names its category"):
         self.compile_with(lambda d: (hit_table(d), d[bc.HIT_CATEGORY_KEY].pop("Bio")))
      with self.assertRaisesRegex(bc.ManifestError, "hit_category_by_type.Bio: status is not a damaging"):
         self.compile_with(lambda d: (hit_table(d), d[bc.HIT_CATEGORY_KEY].update(Bio="status")))


class EffectOddsTests(unittest.TestCase):
   """!Effect odds (E9): the odds stat flag and the optional effect_odds pipeline; the studio's lists."""

   def test_default_pipeline(self) -> None:
      """!A ruleset without effect_odds (the three-stat set's) gets the plain odds (rng::chance()'s draw), and no
      odds stat reads 0."""
      c = bc.compile_battle(STATS3)
      steps = c["pipelines"]["pipelines"]["effect_odds"]["steps"]
      self.assertEqual([s.get("var") for s in steps], ["chance", "out_of", "r", "hit"])
      self.assertFalse(any(st["odds"] for st in c["stats"]))
      run = bc.rc.run_pipeline(c["pipelines"], "effect_odds",
                               {"effect_chance": 3, "effect_out_of": 10, "user_stat": 0, "target_stat": 0,
                                "user_level": 1, "target_level": 1}, roll=2)
      self.assertEqual(run["outputs"], {"hit": 1, "chance": 3, "out_of": 10})
      certain = bc.rc.run_pipeline(c["pipelines"], "effect_odds",
                                   {"effect_chance": 10, "effect_out_of": 10, "user_stat": 0, "target_stat": 0,
                                    "user_level": 1, "target_level": 1}, roll=lambda span: span)
      self.assertEqual(certain["outputs"]["hit"], 1)   # certain odds draw nothing (span 0)

   def test_odds_stat(self) -> None:
      """!At most one stat is the odds stat."""
      def two(d: JsonDict) -> None:
         d["stats"][1]["odds"] = True
         d["stats"][2]["odds"] = True
      with self.assertRaisesRegex(bc.ManifestError, "at most one stat is the odds stat"):
         bc.compile_battle(FIXTURE, None, bc.sources_of(FIXTURE, edited(two)))
      with self.assertRaisesRegex(bc.ManifestError, "true or false"):
         bc.compile_battle(FIXTURE, None, bc.sources_of(FIXTURE, edited(lambda d: d["stats"][1].update(odds=1))))

   def test_studio_lists(self) -> None:
      """!The lists the studio's editors read are re-exported by the battle compiler."""
      self.assertEqual(bc.DURATIONS, ("turn", "until_switch"))
      self.assertEqual(bc.COUNTS, ("before_move", "end_of_turn"))
      self.assertEqual(set(bc.CLEARS_ON), {"switch_out", "faint"})
      self.assertEqual(bc.CONDITION_TEXT_ARGS, {"user", "target"})
      self.assertEqual((bc.MIN_CHAIN_DEN, bc.MAX_CHAIN_CAP), (2, 8))
      self.assertEqual(bc.DEFAULT_ESCALATION_CAP, bc.U8_MAX)
      self.assertEqual(bc.FORMAT_RANGES, ("all", "adjacent"))
      self.assertEqual(bc.ON_TARGET_FAINTED, ("redirect", "fail"))
      self.assertEqual(bc.DERIVED_ELEMENT, "element")
      self.assertEqual(bc.VOLATILE_TEXT_SLOTS, ("start", "already", "end", "skip"))
      self.assertEqual(bc.CONDITION_TEXT_SLOTS, ("start", "already", "end"))


class ReviewTests(unittest.TestCase):
   """!The engine review's structure findings (C1): manifest passive slots (D-25 decision 6), optional engine
   messages, the exports the studio reads instead of copies."""

   def compile_with(self, edit: Callable[[JsonDict], None], platform: str | None = None) -> JsonDict:
      """!Compiles the Starter after an edit, from memory.
      @param edit      callable(view)
      @param platform  target platform, or None (the GBA's limits)
      @return compiled
      """
      view = edited(edit)
      return bc.compile_battle(FIXTURE, None, bc.sources_of(FIXTURE, view), platform=platform)

   def test_passive_slots(self) -> None:
      """!The Starter's two rolled slots and (B2) its hidden one that is not rolled (Sproutle's Nurture); the three-slot
      model on another species and member; the default one slot; the platform's cap; species, members and the
      selector role follow."""
      c = bc.compile_battle(FIXTURE)
      self.assertEqual(c["passive_slots"], [{"name": "first", "rolled": True}, {"name": "second", "rolled": True},
                                            {"name": "hidden", "rolled": False}])
      text = bc.emit("battle.json", c)
      self.assertIn("static_assert(3 <= kMaxPassiveSlots", text)
      self.assertIn("    3, 0x03, 0x03,", text)   # BattleData: passive_slots, passive_rolled, passive_selector_mask

      def three(d: JsonDict) -> None:
         d["passive_slots"] = [{"name": "first"}, {"name": "second"}, {"name": "hidden", "rolled": False}]
         d["species"][0]["passives"] = [d["passives"][0]["name"], None, d["passives"][1]["name"]]
         d["parties"][0]["members"][0]["passive"] = "hidden"
      c3 = self.compile_with(three)
      self.assertEqual(bc.passive_rolled_mask(c3["passive_slots"]), 0b011)
      self.assertEqual(bc.passive_slot_bits(c3["passive_slots"]), 2)
      self.assertEqual(c3["parties"][0]["members"][0]["passive"], 2)
      self.assertIn("    3, 0x03, 0x03,", bc.emit("battle.json", c3))

      def default(d: JsonDict) -> None:
         del d["passive_slots"]
         for sp in d["species"]:
            sp["passives"] = sp["passives"][:1]
      self.assertEqual(self.compile_with(default)["passive_slots"], bc.DEFAULT_PASSIVE_SLOTS)

      def too_many(d: JsonDict) -> None:
         d["passive_slots"] = [{"name": f"s{k}"} for k in range(5)]
      with self.assertRaisesRegex(bc.ManifestError, "1 to 4 passive slots"):
         self.compile_with(too_many)
      self.assertEqual(len(self.compile_with(too_many, platform="desktop")["passive_slots"]), 5)

      def over(d: JsonDict) -> None:
         d["species"][0]["passives"] = [d["passives"][0]["name"]] * 4
      with self.assertRaisesRegex(bc.ManifestError, "passives: up to 3, one per passive slot"):
         self.compile_with(over)

      def bad_member(d: JsonDict) -> None:
         d["parties"][0]["members"][0]["passive"] = "fourth"
      with self.assertRaisesRegex(bc.ManifestError, "one of the passive slots first, second, hidden"):
         self.compile_with(bad_member)

      def narrow_selector(d: JsonDict) -> None:
         three(d)
         d["monster_layout"]["roles"] = with_role(d["monster_layout"]["roles"], "passive_selector", width=1)
      with self.assertRaisesRegex(bc.ManifestError, "role 'passive_selector' is 1 bits wide"):
         self.compile_with(narrow_selector)

   def test_optional_messages(self) -> None:
      """!Messages the engine never says itself are optional: the Starter leaves recoil out (BattleData::messages
      holds nullptr there), `say` cannot name it, a chart naming an absent one is refused; required ones stay."""
      self.assertIn("recoil", bc.OPTIONAL_MESSAGES)
      self.assertNotIn("fainted", bc.OPTIONAL_MESSAGES)
      self.assertEqual(len(bc.REQUIRED_MESSAGES) + len(bc.OPTIONAL_MESSAGES), len(bc.MESSAGES))
      c = bc.compile_battle(FIXTURE)
      self.assertIsNone(c["messages"][bc.MESSAGES.index("recoil")])
      self.assertIsNone(c["effect_catalog"].lookup("messages", "recoil"))
      self.assertEqual(c["effect_catalog"].lookup("messages", "drained"), bc.MESSAGES.index("drained"))

      def drop_required(d: JsonDict) -> None:
         del d["messages"]["fainted"]
      with self.assertRaisesRegex(bc.ManifestError, "missing message"):
         self.compile_with(drop_required)

      def absent_chart(d: JsonDict) -> None:
         del d["messages"]["super_effective"]
      with self.assertRaisesRegex(bc.ManifestError, "an engine message key the manifest gives"):
         self.compile_with(absent_chart)

   def test_msg_enum_names_engine_messages(self) -> None:
      """!capsule::battle::Msg names exactly the messages the engine says itself (C4), each at its id in MESSAGES
      (the optional ones keep their ids, unnamed), and NUM_MSGS is the engine's id count."""
      header = (ROOT / "include/capsule/battle/data/messages.h").read_text(encoding="utf-8")
      body = re.search(r"enum class Msg : uint8_t\s*\{(.*?)\};", header, re.S).group(1)
      values = {name: int(v) for name, v in re.findall(r"^\s*(\w+)\s*=\s*(\d+)", body, re.M)}
      count = values.pop("NUM_MSGS")
      self.assertEqual(count, len(bc.MESSAGES))
      snake = {re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", n).lower(): v for n, v in values.items()}
      self.assertEqual(set(snake), set(bc.REQUIRED_MESSAGES))
      for key, v in snake.items():
         with self.subTest(key=key):
            self.assertEqual(v, bc.MESSAGES.index(key))

   def test_game_messages(self) -> None:
      """!A battle declares messages of its own (C4): the Starter's dazed / staggered keys follow the engine's ids
      in manifest order, `say` and the condition records name them; a key that is no identifier is refused."""
      c = bc.compile_battle(FIXTURE)
      own = bc.game_messages(bc.view_of(FIXTURE)["messages"])
      self.assertIn("dazed", own)
      self.assertIn("staggered", own)
      self.assertEqual(len(c["messages"]), len(bc.MESSAGES) + len(own))
      self.assertEqual(c["effect_catalog"].lookup("messages", "dazed"), len(bc.MESSAGES) + own.index("dazed"))
      self.assertIsNone(c["effect_catalog"].lookup("messages", "confused"))

      def bad_key(d: JsonDict) -> None:
         d["messages"]["Not A Key"] = "x"
      with self.assertRaisesRegex(bc.ManifestError, "needs a lower-case identifier"):
         self.compile_with(bad_key)

      def too_many(d: JsonDict) -> None:
         for k in range(bc.MAX_MESSAGES):
            d["messages"][f"extra_{k}"] = "x"
      with self.assertRaisesRegex(bc.ManifestError, "messages besides the engine's"):
         self.compile_with(too_many)

   def test_exports(self) -> None:
      """!The lists the studio reads from the compiler (studio review F8) are exported."""
      self.assertEqual(bc.TARGETS[0], "selected")
      self.assertIn("save_resources", bc.AI_FLAGS)
      self.assertEqual(list(bc.DECISIVE), ["zero", "ko"])
      self.assertEqual(bc.EACH_PHASES, ("sides", "field"))
      self.assertEqual(bc.LOGIC_KINDS["status"], "statuses")
      self.assertEqual(bc.BATTLER_SUBJECTS, ("holder", "other", "user", "target"))
      self.assertTrue(bc.RNG_NAME.match("method1"))
      self.assertEqual(bc.GENERATED_BY_ENGINE[0], "species_key")
      self.assertEqual(bc.DRAWS_PER_KIND["wide"], 2)


def edited(edit: Callable[[JsonDict], None]) -> JsonDict:
   """!The fixture's merged view (the Starter, FIXTURE) after an edit.
   @param edit  callable(view)
   @return the view
   """
   view = bc.view_of(FIXTURE)
   edit(view)
   return view


class PublicExportTests(unittest.TestCase):
   """!X1: the names the studio reads are public (battle_compiler re-exports the battlec ones; the ruleset, bench and
   map compilers define theirs), each the internal definition itself."""

   def test_battle_compiler_reexports(self) -> None:
      """!The re-exports are the battlec definitions (identity, not copies).
      @return None
      """
      from battlec import content, pipelines, progression, turn
      self.assertIs(bc.OWNER_SOURCES, pipelines.OWNER_SOURCES)
      self.assertIs(bc.OWNER_KINDS, pipelines.OWNER_KINDS)
      self.assertIs(bc.MANIFEST_HANDLER_LISTS, pipelines.MANIFEST_HANDLER_LISTS)
      self.assertIs(bc.move_type_of, pipelines.move_type_of)
      self.assertIs(bc.ONCE_PHASES, turn.ONCE_PHASES)
      self.assertIs(bc.AT_MOST_ONCE_PHASES, turn.AT_MOST_ONCE_PHASES)
      self.assertIs(bc.record_inputs, progression.record_inputs)
      self.assertIs(bc.species_inputs, progression.species_inputs)
      self.assertIs(bc.SPECIES_FIELD_NAME, content.SPECIES_FIELD_NAME)
      self.assertTrue(set(bc.ONCE_PHASES + bc.AT_MOST_ONCE_PHASES) <= set(bc.END_PHASES))

   def test_manifest_handler_lists(self) -> None:
      """!The owner lists whose records take manifest "handlers" are exactly MANIFEST_HANDLER_LISTS (passives and
      items are "effect" records; the other owners' records are named by their kind).
      @return None
      """
      self.assertTrue(set(bc.MANIFEST_HANDLER_LISTS) <= set(bc.OWNER_KINDS))
      for key, kind in bc.OWNER_KINDS.items():
         record = bc.KEYS.get(kind, bc.KEYS["effect"])
         self.assertEqual("handlers" in record, key in bc.MANIFEST_HANDLER_LISTS, key)

   def test_move_type_of(self) -> None:
      """!move_type_of() runs the move_type point as the engine does, and point_runner(move_type=) hands the hit's
      type to the handlers' move.type (Shift is Stone in the dust; Dust doubles a Stone hit's damage).
      @return None
      """
      c = bc.compile_battle(MOVES_FIXTURE)
      names = c["names"]
      shift, dust = names["moves"].index("Shift"), names["weathers"].index("Dust")
      stone = names["types"].index("Stone")
      user, target = {"hp": 10}, {"hp": 10}
      run = bc.point_runner(c, user, target, shift, weather=dust)
      self.assertEqual(bc.move_type_of(c, run, shift), stone)
      self.assertEqual(bc.move_type_of(c, bc.point_runner(c, user, target, shift), shift), c["moves"][shift]["type"])
      self.assertNotEqual(c["moves"][shift]["type"], stone)
      hit = bc.point_runner(c, user, target, shift, weather=dust, move_type=stone)
      self.assertEqual(hit("damage", 40), 80)
      self.assertEqual(run("damage", 40), 40)
      # the compiled data is not changed
      self.assertNotEqual(c["moves"][shift]["type"], stone)

   def test_other_compilers(self) -> None:
      """!The ruleset compiler's unary operators (the parser's), the bench's owner texts key and the map compiler's
      dictionary, layer and border limits and cell fields.
      @return None
      """
      self.assertEqual(tuple(rc.UNARY_OPERATORS), ("-", "!", "~"))
      for op, node in rc.UNARY_OPERATORS.items():
         self.assertEqual(rc.parse(f"{op}x", ["x"], {}, "t")[0], node)
      import bench_scenario as bsc
      import map_compiler as mc
      self.assertEqual(bsc.OWNER_TEXTS_KEY, "owner_texts")
      self.assertIn(bsc.OWNER_TEXTS_KEY, bsc.SCENARIO_KEYS)
      self.assertEqual(mc.MAX_METATILES, 0xFFF)   # I2: 4,095 entries (X1's 1,023 was UNDEFINED)
      self.assertEqual((mc.MAX_LAYERS, mc.MAX_BEHAVIORS, mc.MAX_BORDER_SIZE), (4, 256, 8))
      self.assertEqual(set(mc.CELL_FIELDS), {"behavior", "env", "collision", "elevation", "encounter", "slot"})


if __name__ == "__main__":
   unittest.main()
