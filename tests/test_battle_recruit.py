#!/usr/bin/env python3
"""!Tests of the battle's Item action, recruitment and the storage boxes in the compilers (D-26 section 3, package G3):
the structure's recruit section (tools/battlec/recruit.py) and storage section (tools/battlec/storage.py), the AI
profiles' item-use rules and a party's battle items, the effect primitive `recruit`, the ruleset pipeline
recruit_odds, the save image's Boxes section (tools/save_layout.py), the script compiler's box statements and the
widened hook mask, on the neutral fixture tests/data/recruit and the Starter sample (run by CTest, or directly)."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler as bc  # noqa: E402
import effect_compiler as ec  # noqa: E402
import ruleset_compiler as rc  # noqa: E402
import save_layout  # noqa: E402
import script_compiler as sc  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## The neutral recruit fixture.
FIXTURE = ROOT / "tests/data/recruit/battle.json"
## Its effect logic.
FIXTURE_LOGIC = ROOT / "tests/data/recruit/logic.ceffect"
## Its ruleset.
FIXTURE_RULESET = ROOT / "tests/data/recruit/ruleset.json"
## The Starter sample.
STARTER = ROOT / "samples/starter/battle.json"


def compile_with(edit: Callable[[JsonDict], None], logic: str | None = None, platform: str | None = None,
                 ruleset: Callable[[JsonDict], None] | None = None) -> JsonDict:
   """!Compiles the fixture after an edit of its merged view (and, optionally, other logic or ruleset), from memory.
   @param edit      callable(merged view)
   @param logic     replacement text of logic.ceffect, or None
   @param platform  target platform, or None (the GBA)
   @param ruleset   callable(ruleset data), or None
   @return compiled
   """
   data = bc.view_of(FIXTURE)
   edit(data)
   sources = bc.sources_of(FIXTURE, data)
   if logic is not None:
      sources[FIXTURE_LOGIC.resolve()] = logic
   if ruleset is not None:
      rs = json.loads(FIXTURE_RULESET.read_text(encoding="utf-8"))
      ruleset(rs)
      sources[FIXTURE_RULESET.resolve()] = rs
   return bc.compile_battle(FIXTURE, None, sources, platform=platform)


class RecruitTests(unittest.TestCase):
   """!The structure's recruit section."""

   def test_fixture(self) -> None:
      """!Three checks, the recruit_rate field, Sleepy's bonus, hit points and status kept, the orb_used role, the
      moments' messages (broke_free from its own list) and the emitted kRecruit."""
      c = bc.compile_battle(FIXTURE)
      r = c["recruit"]
      self.assertEqual((r["checks"], r["rate_name"], r["rate_range"]), (3, "recruit_rate", (0, 255)))
      self.assertEqual(r["status_bonus"], [10, 20])
      self.assertEqual((r["keep_names"], r["owned"], r["item_role_name"]), (["hp", "status"], "refuse", "orb_used"))
      # C6: refusal_consumes "blocked" is its RecruitFlag bit 2 (RECRUIT_CONSUME_BLOCKED)
      self.assertEqual((r["refusal_consumes"], r["flags"]), (["blocked"], 1 << 2))
      self.assertEqual(r["messages"][bc.RECRUIT_MOMENTS.index(bc.RECRUIT_BREAK_FREE_MOMENT)], bc.NONE8)
      names = bc.declared_messages(bc.view_of(FIXTURE)["messages"])
      self.assertEqual([names[k] for k in r["break_free"]], ["orb_free0", "orb_free1", "orb_free2"])
      cpp = bc.emit("battle.json", c)
      self.assertIn("const RecruitData kRecruit = {kRecruitStatusBonus, 3, 0, 0x03, 4,", cpp)
      # the rate field's table is emitted for the engine's species_field_of()
      self.assertIn("kSpeciesFields", cpp)

   def test_no_section(self) -> None:
      """!A battle without the section cannot recruit (no checks), and has no boxes."""
      def drop(d: JsonDict) -> None:
         del d["recruit"]
         del d["storage"]
         for key in [k for k in d["messages"] if k.startswith("orb_")]:
            del d["messages"][key]   # their {value} is the recruit moments' own
      c = compile_with(drop, logic=FIXTURE_LOGIC.read_text(encoding="utf-8"))
      self.assertIsNone(c["recruit"])
      self.assertEqual(c["storage"]["slots"], 0)
      self.assertIn("const RecruitData kRecruit = {nullptr, 0, 0xFF,", bc.emit("battle.json", c))

   def test_refusals(self) -> None:
      """!Checks out of range, an unknown rate field, status or message, a bad owned / keep, an item role that is
      not stored or too narrow, too many break_free messages."""
      cases: list[tuple[Callable[[JsonDict], None], str]] = [
         (lambda d: d["recruit"].update(checks=0), "checks"),
         (lambda d: d["recruit"].update(checks=bc.RECRUIT_MAX_CHECKS + 1), "checks"),
         (lambda d: d["recruit"].update(rate="catch_rate"), "not a species field"),
         (lambda d: d["recruit"]["status_bonus"].update(Frozen=20), "not a status"),
         (lambda d: d["recruit"]["status_bonus"].update(Sleepy=300), "status_bonus"),
         (lambda d: d["recruit"].update(owned="sometimes"), "owned"),
         (lambda d: d["recruit"].update(refusal_consumes=["owned"]), "refusal_consumes"),
         (lambda d: d["recruit"].update(refusal_consumes=["blocked", "blocked"]), "refusal_consumes"),
         (lambda d: d["recruit"].update(keep=["hp", "hp"]), "keep"),
         (lambda d: d["recruit"].update(item_role="ball"), "not a role the layout stores"),
         (lambda d: d["recruit"]["messages"].update(check="gotcha"), "not one of the battle's messages"),
         (lambda d: d["recruit"]["messages"].update(broke_free="orb_free0"), "broke_free"),
         (lambda d: d["recruit"].update(break_free=["orb_free0"] * (bc.RECRUIT_MAX_BREAK_FREE + 1)), "at most"),
         (lambda d: d["recruit"].update(colour=1), "colour"),
      ]
      for edit, pattern in cases:
         with self.subTest(pattern=pattern):
            with self.assertRaisesRegex(bc.ManifestError, pattern):
               compile_with(edit)

   def test_narrow_item_role(self) -> None:
      """!The item role must hold the highest item id."""
      def narrow(d: JsonDict) -> None:
         for r in d["monster_layout"]["roles"]:
            if isinstance(r, dict) and r.get("custom") == "orb_used":
               r["width"] = 2
      with self.assertRaisesRegex(bc.ManifestError, "the items need"):
         compile_with(narrow)

   def test_needs_the_pipeline(self) -> None:
      """!A recruit section draws against the ruleset's recruit_odds: a ruleset without one is refused."""
      with self.assertRaisesRegex(bc.ManifestError, "recruit_odds"):
         compile_with(lambda d: None, ruleset=lambda rs: rs["pipelines"].pop("recruit_odds"))

   def test_value_placeholder(self) -> None:
      """!The recruit messages may say {value}; another message of the battle's own may not."""
      def other(d: JsonDict) -> None:
         d["messages"]["plain_one"] = "Got {value}!"
      with self.assertRaisesRegex(bc.ManifestError, "value"):
         compile_with(other)
      c = bc.compile_battle(FIXTURE)
      self.assertIsNotNone(c["messages"][bc.declared_messages(bc.view_of(FIXTURE)["messages"]).index("orb_wobble")])


class PipelineTests(unittest.TestCase):
   """!The ruleset pipeline recruit_odds and the widened roll span."""

   def test_contract(self) -> None:
      """!Optional (a ruleset without it never recruits), its inputs and outputs, a ChanceResult of sure / chance /
      out_of with out_of up to 65536."""
      self.assertIn("recruit_odds", rc.OPTIONAL_PIPELINES)
      spec = rc.PIPELINES["recruit_odds"]
      self.assertEqual(spec["struct"], "RecruitOddsInputs")
      self.assertEqual(list(spec["outputs"]), ["sure", "chance", "out_of"])
      self.assertEqual(spec["outputs"]["out_of"], (1, rc.DRAW_SPAN))
      self.assertEqual(rc.DRAW_SPAN, 65536)

   def test_fixture_odds(self) -> None:
      """!The fixture's formula at full HP without a status: rate x HP share x bonus x status bonus, in 256."""
      c = bc.compile_battle(FIXTURE)
      pl = c["pipelines"]
      inputs = {"target_hp": 40, "target_max_hp": 40, "recruit_rate": 200, "bonus": 10, "status_bonus": 10,
                "target_level": 5, "user_level": 20, "turn": 1}
      out = rc.run_pipeline(pl, "recruit_odds", inputs)["outputs"]
      self.assertEqual(out, {"sure": 0, "chance": 100, "out_of": 256})
      self.assertEqual(rc.run_pipeline(pl, "recruit_odds", {**inputs, "bonus": 255})["outputs"]["sure"], 1)

   def test_roll_span(self) -> None:
      """!A roll may span 65536 (a 16-bit draw as it is), not more."""
      base = json.loads(FIXTURE_RULESET.read_text(encoding="utf-8"))
      base.pop("editor")
      base["pipelines"]["recruit_odds"] = [{"roll": "r", "span": "65536"}, {"let": "sure", "expr": "0"},
                                           {"let": "chance", "expr": "r"}, {"let": "out_of", "expr": "65536"}]
      rc.bind(rc.load(base, "r.json"))
      base["pipelines"]["recruit_odds"][0]["span"] = "65537"
      with self.assertRaisesRegex(rc.RulesetError, "at most 65536"):
         rc.bind(rc.load(base, "r.json"))


class StorageTests(unittest.TestCase):
   """!The structure's storage section."""

   def test_fixture(self) -> None:
      """!Two boxes of three: 6 slots of 48-byte records, the names, kStorage and the ids."""
      c = bc.compile_battle(FIXTURE)
      st = c["storage"]
      self.assertEqual((st["box_count"], st["box_size"], st["slots"], st["bytes"], st["names"]),
                       (2, 3, 6, 6 * c["layout"]["bytes"], ["Pen", "Barn"]))
      self.assertIn("const StorageData kStorage = {kBoxNames, 3, 2, 0};", bc.emit("battle.json", c))
      ids = bc.emit_ids("battle.json", c)
      self.assertIn("inline constexpr int NUM_BOXES = 2;", ids)
      self.assertIn("inline constexpr int BOX_SIZE = 3;", ids)

   def test_names_pattern(self) -> None:
      """!One name with {n} names every box."""
      c = compile_with(lambda d: d["storage"].update(names="Box {n}"))
      self.assertEqual(c["storage"]["names"], ["Box 1", "Box 2"])

   def test_refusals(self) -> None:
      """!Too many boxes or slots or record bytes for the platform, a name list of the wrong length."""
      cases: list[tuple[Callable[[JsonDict], None], str]] = [
         (lambda d: d["storage"].update(box_count=17), "box_count"),
         (lambda d: d["storage"].update(box_count=16, box_size=40, names="B{n}"), "max_box_slots"),
         (lambda d: d["storage"].update(names=["Pen"]), "names"),
         (lambda d: d["storage"].update(names=["Pen", ""]), "letters"),
         (lambda d: d["storage"].update(shelves=1), "shelves"),
      ]
      for edit, pattern in cases:
         with self.subTest(pattern=pattern):
            with self.assertRaisesRegex(bc.ManifestError, pattern):
               compile_with(edit)

   def test_bytes_budget(self) -> None:
      """!The GBA holds 32 KB of boxed record bytes: 16 boxes of 30 records of 100 bytes do not fit."""
      def big(d: JsonDict) -> None:
         d["storage"].update(box_count=16, box_size=30, names="B{n}")
         d["monster_layout"]["roles"].append({"custom": "padding", "width": 32, "count": 13})
      with self.assertRaisesRegex(bc.ManifestError, "max_storage_bytes"):
         compile_with(big)


class AiItemTests(unittest.TestCase):
   """!The AI profiles' item-use rules and the parties' battle items."""

   def test_fixture(self) -> None:
      """!careful: below_hp Salve at 50 percent, then hook Beacon; Keeper's items; kAiProfiles counts them."""
      c = bc.compile_battle(FIXTURE)
      careful = c["ai"]["profiles"][1]
      first = len(bc.AI_RULES) + len(bc.AI_SWITCH_RULES)
      salve = bc.view_of(FIXTURE)["items"].index(next(i for i in bc.view_of(FIXTURE)["items"]
                                                      if i["name"] == "Salve")) + 1
      self.assertEqual(careful["items"][0], (first + bc.AI_ITEM_RULES.index("below_hp"), 50, salve))
      self.assertEqual(careful["items"][1][0], first + bc.AI_ITEM_RULES.index("hook"))
      keeper = c["parties"][2]
      self.assertEqual(keeper["item_names"], ["Salve", "Beacon"])
      self.assertEqual(len(keeper["items"]), bc.MAX_PARTY_ITEMS)
      self.assertIn("{2, 4, 2, 0, 2, 0},   // careful", bc.emit("battle.json", c))

   def test_refusals(self) -> None:
      """!An unknown item, a field item, a hook rule's item without its own ai_score, too many party items, an
      item named by a scoring rule."""
      profiles = lambda d: d["ai_profiles"][1]  # noqa: E731
      cases: list[tuple[Callable[[JsonDict], None], str]] = [
         (lambda d: profiles(d).update(items=[{"rule": "below_hp", "item": "Nothing"}]), "an item of the battle"),
         (lambda d: profiles(d).update(items=[{"rule": "below_hp", "item": "Ether"}]), "not used in battle"),
         (lambda d: profiles(d).update(items=[{"rule": "hook", "item": "Salve"}]), "ai_score handler"),
         (lambda d: profiles(d).update(items=[{"rule": "below_hp", "item": "Salve", "percent": 0}]), "percent"),
         (lambda d: profiles(d)["rules"].append({"rule": "noise", "item": "Salve"}), "only an item-use rule"),
         (lambda d: d["parties"][2].update(items=["Salve"] * (bc.MAX_PARTY_ITEMS + 1)), "at most"),
      ]
      for edit, pattern in cases:
         with self.subTest(pattern=pattern):
            with self.assertRaisesRegex(bc.ManifestError, pattern):
               compile_with(edit)


class EffectTests(unittest.TestCase):
   """!The effect primitive `recruit` and the widened hook mask."""

   def test_recruit_context(self) -> None:
      """!`recruit` only in an item's use_battle handler; there it is Op::Recruit after its bonus."""
      logic = FIXTURE_LOGIC.read_text(encoding="utf-8")
      field = "   on use_field {\n      if holder.resource(pp)"
      bad = logic.replace(field, "   on use_field {\n      recruit holder 1;\n      if holder.resource(pp)")
      with self.assertRaisesRegex(bc.ManifestError, "recruit only in an item's use_battle"):
         compile_with(lambda d: None, logic=bad)
      c = bc.compile_battle(FIXTURE)
      names = [n for _at, n, _v in ec.instructions(c["effects"]["bank"].code)]
      self.assertIn("Recruit", names)
      self.assertEqual(ec.OPS[ec.OP["Recruit"]], ("Recruit", ["subj"], 1, 1))
      self.assertEqual(ec.OP_EVENTS["recruit"][0], ec.RECRUIT_EVENTS)

   def test_hook_mask_words(self) -> None:
      """!The hook mask is as many 32-bit words as the hooks need: a 33rd hook makes two."""
      self.assertEqual(ec.hook_mask_words(), (len(ec.HOOKS) + ec.HOOK_MASK_WORD_BITS - 1) // ec.HOOK_MASK_WORD_BITS)
      saved = list(ec.HOOKS)
      try:
         while len(ec.HOOKS) <= ec.HOOK_MASK_WORD_BITS:
            ec.HOOKS.append(dict(saved[0], name=f"extra_{len(ec.HOOKS)}"))
         self.assertEqual(ec.hook_mask_words(), 2)
         self.assertEqual(ec.hook_mask_cpp((1 << ec.HOOK_MASK_WORD_BITS) | 1), "{{0x00000001u, 0x00000001u}}")
      finally:
         ec.HOOKS[:] = saved
      self.assertIn(f"static_assert(effect::kHookMaskWords == {ec.hook_mask_words()}",
                    "\n".join(ec._enum_asserts()))


class SaveTests(unittest.TestCase):
   """!The save image's Boxes section (tools/save_layout.py)."""

   def test_sizes(self) -> None:
      """!The head, a bit per slot, every slot's record (tests/recruit_tests.cpp checks the same in C++)."""
      c = bc.compile_battle(FIXTURE, platform="headless")
      self.assertEqual(save_layout.sections_of(c)["boxes"], 10 + 1 + 6 * c["layout"]["bytes"])

   def test_image(self) -> None:
      """!A record in slot 4, box 1 current: the head with the geometry, the used bit, only the used record."""
      c = bc.compile_battle(FIXTURE, platform="headless")
      size = c["layout"]["bytes"]
      record = bytes(range(1, size + 1))
      boxed = save_layout.boxes_payload(c, {4: record}, 1)
      self.assertEqual(boxed[:4], c["layout"]["record_hash"].to_bytes(4, "little"))   # C7: no party size
      self.assertEqual(boxed[6:10], bytes([2]) + (3).to_bytes(2, "little") + bytes([1]))
      self.assertEqual(boxed[10], 1 << 4)
      self.assertEqual(boxed[11:], record)
      data = save_layout.image(c, {}, boxes={4: record}, current_box=1)
      self.assertTrue(data.endswith(boxed))
      self.assertLessEqual(len(data), save_layout.image_bytes(save_layout.sections_of(c)))
      with self.assertRaisesRegex(bc.ManifestError, "storage slot 6"):
         save_layout.image(c, {}, boxes={6: record})

   def test_starter_budget(self) -> None:
      """!The Starter's 8 boxes of 30 fit the GBA's 64 KB with two copies."""
      r = save_layout.layout(STARTER, platform="gba")
      self.assertGreater(r["sections"]["boxes"], 0)
      self.assertEqual(r["copies"], 2)


class ScriptTests(unittest.TestCase):
   """!The script compiler's box statements."""

   def compile_script(self, text: str) -> sc.Compiler:
      """!Compiles one script source.
      @param text  .cscript text
      @return the compiler
      """
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         (folder / "s.cscript").write_text(text, encoding="utf-8")
         manifest = {"schema": "capsule.scripts/1", "sources": ["s.cscript"]}
         (folder / "scripts.json").write_text(json.dumps(manifest), encoding="utf-8")
         return sc.compile_file(folder / "scripts.json")

   def test_statements(self) -> None:
      """!count_boxed, deposit_box, withdraw_box and open_boxes compile to their opcodes; open_boxes waits."""
      comp = self.compile_script('var n : u8;\nscript s {\n    count_boxed -> var(n);\n'
                                 '    deposit_box(1) -> var(n);\n    withdraw_box(var(n));\n'
                                 '    open_boxes -> var(n);\n}\n')
      self.assertEqual([d.message for d in comp.errors()], [])
      code = comp.result["code"]
      for op in ("BoxCount", "BoxDeposit", "BoxWithdraw", "BoxesOpen"):
         self.assertIn(sc.OP[op], code)
      waits = self.compile_script('var n : u8;\nscript s { open_boxes -> var(n); }\n'
                                  'map_scripts room { on_load -> s; }\n')
      self.assertTrue(any("wait" in d.message for d in waits.errors()))
      names = self.compile_script("script deposit_box { end; }\n")
      self.assertEqual([d.message for d in names.errors()], [])


if __name__ == "__main__":
   unittest.main()
