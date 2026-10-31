#!/usr/bin/env python3
"""!Tests for eggs and the nursery (D-28 sections 2 and 3, package B2: tools/battlec/nursery.py): the sections'
checks and emitted tables, the refusal flags of the systems that use records, the hatch hook and the nursery
pipelines, the Python mirrors (hatch_steps(), egg_record(), nursery_fee(), nursery_gain()), the save image's Nursery
section (tools/save_layout.py), the script compiler's nursery statements and implicit egg_member, and the Starter's
Grove (run by CTest, or directly)."""
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
import save_layout as sl  # noqa: E402
import script_compiler as sc  # noqa: E402
from battlec import nursery as bn  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## The neutral combination fixture (its nest method, eggs and nursery).
FIXTURE = ROOT / "tests/data/combine/battle.json"
## Its scripts manifest.
FIXTURE_SCRIPTS = ROOT / "tests/data/combine/scripts.json"
## The Starter sample.
STARTER = ROOT / "samples/starter/battle.json"
## A battle without eggs or a nursery.
STATS3 = ROOT / "tests/data/battle_stats3/battle.json"
## The fixture's egg cycle (nest's cycle_steps).
CYCLE = 8


def compile_with(edit: Callable[[JsonDict], None] | None = None, path: Path = FIXTURE,
                 platform: str = "headless") -> JsonDict:
   """!Compiles a battle manifest after an edit of its merged view, from memory.
   @param edit      callable(merged view), or None
   @param path      the manifest
   @param platform  the platform
   @return compiled
   """
   data = bc.view_of(path)
   if edit is not None:
      edit(data)
   return bc.compile_battle(path, None, bc.sources_of(path, data), platform=platform)


def method(data: JsonDict, name: str) -> JsonDict:
   """!A combination method of a merged view.
   @param data  the view
   @param name  its name
   @return the entry
   """
   return next(m for m in data["combine_methods"] if m["name"] == name)


class SectionTests(unittest.TestCase):
   """!The eggs and nursery sections compile, emit and refuse what the engine cannot do."""

   @classmethod
   def setUpClass(cls) -> None:
      """!Compiles the fixture once.
      @return None
      """
      cls.c = compile_with()

   def refused(self, pattern: str, edit: Callable[[JsonDict], None], path: Path = FIXTURE) -> None:
      """!Asserts a compile error.
      @param pattern  regex of the message
      @param edit     the edit
      @param path     the manifest
      @return None
      """
      with self.assertRaisesRegex(bc.ManifestError, pattern):
         compile_with(edit, path)

   def test_tables(self) -> None:
      """!The egg rules, the nursery and the method's cycle steps; the ids.
      @return None
      """
      eggs = self.c["combine"]["eggs"]
      self.assertEqual((eggs["role_name"], eggs["counter_name"], eggs["cycles_name"]), ("egg", "hatch_counter",
                                                                                       "hatch_cycles"))
      self.assertEqual((eggs["step_name"], eggs["hatch_script"], eggs["name"]), ("egg_cycle", "orb_hatch", "Orb"))
      self.assertEqual((eggs["flags"], eggs["field_actions"], eggs["counter_max"]), (0, 0, 0xFFFF))
      n = self.c["combine"]["nursery"]
      self.assertEqual((n["slots"], n["method_name"], n["check_name"], n["odds_scale"]), (3, "nest", "nest_check",
                                                                                          100))
      self.assertEqual((n["gain"], n["fee"], n["flags"]), (True, True, bn.NURSERY_FLAG_GAIN | bn.NURSERY_FLAG_FEE))
      _k, nest = bc.combine_method_of(self.c, "nest")
      self.assertEqual(nest["cycle_steps"], CYCLE)
      cpp = bc.emit("battle.json", self.c)
      self.assertIn("const text::Code kEggName[]", cpp)
      self.assertIn("    {100, 3, 3, 0, 3, {0, 0}}", cpp)
      self.assertIn("inline constexpr uint8_t NURSERY_SLOTS = 3;", bc.emit_ids("battle.json", self.c))
      # a battle without eggs or a nursery emits the empty tables
      self.assertIn("{nullptr, 0, 0, 0, monster::kNoRole, monster::kNoRole, kNoField, kNoField, kNoStepEffect, 0}",
                    bc.emit("battle.json", compile_with(path=STATS3)))

   def test_c9_flags(self) -> None:
      """!C9: the egg flags of the lead rules, transformations and step effects; the transfer rule for eggs (also
      without an egg section) and the egg role's transfer key.
      @return None
      """
      def flags(d: JsonDict) -> None:
         """!Allows eggs as lead, to transform, and on the nest check's handlers; transferred eggs hatch."""
         d["lead"] = {"allow_eggs": True}
         d["transformation"]["allow_eggs"] = True
         d["step_effects"][0]["allow_eggs"] = True
         d["transfer_eggs"] = "hatch"
      c = compile_with(flags)
      eggs = c["combine"]["eggs"]
      want = bn.EGG_FLAG_LEAD | bn.EGG_FLAG_TRANSFORM | bn.EGG_FLAG_TRANSFER_HATCH
      self.assertEqual((eggs["flags"], eggs["step_effects"]), (want, 1))
      self.assertIn(f"0x00000001u, {eggs['role']}, ", bc.emit("battle.json", c))
      self.assertEqual(self.c["combine"]["eggs"]["step_effects"], 0)
      # a battle without eggs still says what it does with a transferred one
      stats3 = compile_with(lambda d: d.update(transfer_eggs="hatch"), path=STATS3)
      self.assertIn(f"kNoStepEffect, {bn.EGG_FLAG_TRANSFER_HATCH}}}", bc.emit("battle.json", stats3))
      self.refused("transfer_eggs: one of refuse, hatch", lambda d: d.update(transfer_eggs="keep"))
      self.refused("lead.allow_eggs: true or false", lambda d: d.update(lead={"allow_eggs": 1}))
      # the egg role transfers under the eggs' transfer name, whatever it is called
      from battlec.layout import role_key
      egg_key = role_key(bn.EGG_TRANSFER_NAME)
      keys = {r["name"]: r["transfer_key"] for r in self.c["layout"]["custom"]}
      self.assertEqual(keys["egg"], egg_key)
      layout = {"custom": [{"name": "shell", "transfer_key": role_key("shell")},
                           {"name": "mood", "transfer_key": role_key("mood")}]}
      bn.egg_transfer_key(layout, {"role_name": "shell"}, "s")
      self.assertEqual([r["transfer_key"] for r in layout["custom"]], [egg_key, role_key("mood")])
      # another custom role named like the transfer name clashes with it
      self.refused("has the egg role's transfer key", lambda d: d["eggs"].update(role="mark"))

   def test_egg_checks(self) -> None:
      """!The egg section's refusals.
      @return None
      """
      self.refused("needs its eggs section", lambda d: d.pop("eggs"))
      self.refused("stored custom role", lambda d: d["eggs"].update(role="nothing"))
      self.refused("two roles", lambda d: d["eggs"].update(counter="egg"))
      self.refused("not a species field", lambda d: d["eggs"].update(cycles_field="nothing"))
      self.refused("not a step effect", lambda d: d["eggs"].update(step_effect="later"))
      self.refused("1 to 10 letters", lambda d: d["eggs"].update(name="An egg too long"))
      self.refused("unknown key", lambda d: d["eggs"].update(colour=1))
      self.refused("holds up to", lambda d: method(d, "nest").update(cycle_steps=0x8000))
      self.refused("only a method delivering eggs", lambda d: method(d, "echo").update(cycle_steps=4))

   def test_nursery_checks(self) -> None:
      """!The nursery's refusals: its method, its slots, its check.
      @return None
      """
      self.refused("of kind 'nursery'", lambda d: d["nursery"].update(method="echo"))
      self.refused("slots", lambda d: d["nursery"].update(slots=1))
      self.refused("slots", lambda d: d["nursery"].update(slots=9))
      self.refused("not a step effect", lambda d: d["nursery"].update(check="later"))
      self.refused("must run it", lambda d: d.pop("nursery"))
      with self.assertRaisesRegex(bc.ManifestError, "slots"):
         compile_with(lambda d: d["nursery"].update(slots=5), platform="gba")   # the GBA's four

   def test_flags(self) -> None:
      """!The refusal flags of battle, recruitment, field actions and items.
      @return None
      """
      c = compile_with(lambda d: d["rules"].update(allow_eggs=True))
      self.assertEqual(c["combine"]["eggs"]["flags"], bn.EGG_FLAG_BATTLE)
      self.refused("true or false", lambda d: d["rules"].update(allow_eggs=1))
      items = self.c["inventory"]["items"]
      names = self.c["names"]["items"]
      incubator = items[names.index("Incubator") + 1]   # items[0] is none
      self.assertTrue(incubator["allow_eggs"] and incubator["flags"] & 8)
      self.assertFalse(items[names.index("Salve") + 1]["allow_eggs"])

   def test_hook_and_pipelines(self) -> None:
      """!The hatch hook's owners and value; the nursery pipelines and their defaults.
      @return None
      """
      spec = ec.HOOKS[ec.HOOK["hatch"]]
      self.assertEqual(spec["owners"], {"global", "passive", "item"})
      self.assertEqual(spec["value"], (0, ec.MAX_HATCH_STEPS))
      self.assertEqual(ec.HOOK["hatch"], len(ec.HOOKS) - 1)
      self.assertIn("hatch", bc.GLOBAL_HOOKS)
      bank = self.c["effects"]["bank"]
      self.assertTrue(any(ec.HOOKS[h]["name"] == "hatch" for h, _f, _p in bank.handlers))
      self.assertEqual(rc.PIPELINES["nursery_gain"]["inputs"], ["level", "slot"])
      self.assertEqual(rc.PIPELINES["nursery_fee"]["inputs"], ["levels_gained", "level", "count"])
      self.assertEqual(bc.nursery_gain_of(self.c, 3, 0), 2)
      self.assertEqual(bc.nursery_fee_of(self.c, 2, 5, 2), 20)
      starter = compile_with(path=STARTER)
      self.assertEqual(bc.nursery_gain_of(starter, 3, 0), 0)   # the Grove: no gain, no fee
      self.assertEqual(bc.nursery_fee_of(starter, 9, 9, 2), 0)

   def test_mirrors(self) -> None:
      """!The hatch counter a species starts with; an egg's record dict.
      @return None
      """
      _k, nest = bc.combine_method_of(self.c, "nest")
      self.assertEqual(bc.hatch_steps(self.c, nest, "Pixlet"), 2 * CYCLE)
      self.assertEqual(bc.hatch_steps(self.c, nest, "Pupkin"), 3 * CYCLE)
      egg = bc.egg_record(self.c, {"species": "Pixlet", "level": 1}, 99999)
      self.assertEqual((egg["egg"], egg["hatch_counter"]), (1, 0xFFFF))
      # a nursery method previews (the nursery runs it); an egg input is unfit
      self.assertEqual(bc.combine_preview(self.c, "nest", [{"species": "Pupkin", "level": 3},
                                                           {"species": "Cube", "level": 3}])["odds"], 50)


class SaveTests(unittest.TestCase):
   """!The save image's Nursery section (tools/save_layout.py, capsule/save/save.h)."""

   def test_sizes_and_payload(self) -> None:
      """!Its most bytes, the payload's head and slots; absent without a nursery.
      @return None
      """
      c = compile_with()
      size = c["layout"]["bytes"]
      self.assertEqual(sl.sections_of(c)["nursery"], sl.NURSERY_HEAD_BYTES + 3 * (sl.NURSERY_SLOT_BYTES + size) + size)
      payload = sl.nursery_payload(c, {2: (4, 300, b"\x01" * size)}, b"\x02" * size)
      self.assertEqual(payload[:4], sl.u32(c["layout"]["record_hash"]))
      self.assertEqual(payload[6:9], bytes([3, 0b100, 1]))
      self.assertEqual(payload[9:14], bytes([4]) + sl.u32(300))
      self.assertEqual(len(payload), sl.NURSERY_HEAD_BYTES + sl.NURSERY_SLOT_BYTES + 2 * size)
      with self.assertRaises(bc.ManifestError):
         sl.nursery_payload(c, {3: (1, 0, b"")})
      plain = compile_with(path=STATS3)
      self.assertEqual(sl.sections_of(plain)["nursery"], 0)
      self.assertEqual(sl.nursery_payload(plain, {}), b"")
      image = sl.image(c, {}, nursery={0: (1, 0, b"")})
      self.assertIn(bytes([sl.SECTIONS.index("nursery"), 1]), image[sl.HEADER_BYTES:sl.HEADER_BYTES + 80])

   def test_starter_fits(self) -> None:
      """!The Starter's image with the Grove fits the GBA's flash twice.
      @return None
      """
      r = sl.layout(STARTER, platform="gba")
      self.assertTrue(r["ok"])
      self.assertGreater(r["sections"]["nursery"], 0)


class ScriptTests(unittest.TestCase):
   """!The script compiler's nursery statements, egg_member and the hatch script."""

   def compile(self, body: str, battle: Path = FIXTURE, extra: str = "script orb_hatch {\n}\n") -> tuple[bool,
                                                                                                         list[str]]:
      """!Compiles one script over a battle.
      @param body    the script's statements
      @param battle  the battle manifest
      @param extra   more declarations
      @return (ok, error messages)
      """
      with tempfile.TemporaryDirectory() as tmp:
         d = Path(tmp)
         (d / "s.cscript").write_text("var v : u16;\n" + extra + "script t {\n" + body + "\n}\n", encoding="utf-8")
         (d / "scripts.json").write_text(json.dumps({"schema": "capsule.scripts/1", "sources": ["s.cscript"],
                                                     "battle": str(battle)}), encoding="utf-8")
         try:
            result = sc.compile_file(d / "scripts.json")
         except sc.ScriptError as e:
            return False, [str(e)]
         errors = [str(x) for x in result.errors()]
         return not errors, errors

   def test_fixture(self) -> None:
      """!The fixture's scripts: every statement, egg_member, the Bank's hatch members, the op bytes.
      @return None
      """
      r = sc.compile_file(FIXTURE_SCRIPTS)
      self.assertEqual([str(e) for e in r.errors()], [])
      self.assertIn(bn.EGG_MEMBER_VAR, r.var_ids)
      self.assertEqual(r.result["hatch"], (r.script_ids["orb_hatch"], r.var_ids[bn.EGG_MEMBER_VAR]))
      code = bytes(r.result["code"])
      op = sc.OP["Nursery"]
      for kind in sc.NURSERY_KINDS[:-1]:
         self.assertIn(bytes([op, sc.NURSERY_KINDS.index(kind)]), code, kind)
      self.assertEqual(sc.NURSERY_KINDS, ["pick", "deposit", "withdraw", "count", "levels", "fee", "ready", "odds",
                                          "take", "name"])

   def test_statements(self) -> None:
      """!The statements compile; a missing result, a nursery statement without a nursery and an undeclared hatch
      script are refused; the nickname prompt works anywhere.
      @return None
      """
      ok, errors = self.compile("nursery withdraw 0 pay var(v) -> var(v);\nnursery name 0 -> var(v);")
      self.assertTrue(ok, errors)
      ok, errors = self.compile("nursery count;")
      self.assertFalse(ok)
      self.assertIn("needs -> var(v)", " ".join(errors))
      ok, errors = self.compile("nursery count -> var(v);", extra="")
      self.assertFalse(ok)
      self.assertIn("the hatch script 'orb_hatch' is not declared", " ".join(errors))
      stats3 = STATS3
      ok, errors = self.compile("nursery count -> var(v);", battle=stats3, extra="")
      self.assertFalse(ok)
      self.assertIn("has no nursery", " ".join(errors))
      ok, errors = self.compile("nursery name 1 -> var(v);", battle=stats3, extra="")
      self.assertTrue(ok, errors)


class StarterTests(unittest.TestCase):
   """!The Starter's Grove: kin, the method, the eggs, the Nurture talent."""

   def test_grove(self) -> None:
      """!Compatibility by kin, one combine move per type line, 5 cycles of 256 steps, Nurture's hatch handler.
      @return None
      """
      c = compile_with(path=STARTER)
      _k, grove = bc.combine_method_of(c, "grove")
      self.assertEqual((grove["kind"], grove["delivery"], grove["cycle_steps"], grove["afterwards"]),
                       ("nursery", "egg", 256, "kept"))
      self.assertEqual(c["combine"]["nursery"]["slots"], 2)
      self.assertEqual(bc.hatch_steps(c, grove, "Breezle"), 5 * 256)
      look = bc.combine_preview(c, "grove", [{"species": "Breezle", "level": 5}, {"species": "Duskowl", "level": 5}])
      self.assertEqual((look["status"], look["species"], look["odds"]), ("ok", "Breezle", 50))
      apart = bc.combine_preview(c, "grove", [{"species": "Breezle", "level": 5}, {"species": "Tidepup", "level": 5}])
      self.assertEqual(apart["status"], "incompatible")
      bases = [sp for sp in c["species"] if c["combine"]["base"][c["species"].index(sp)] == c["species"].index(sp)]
      for sp in bases:
         self.assertEqual(len(c["combine"]["combine_moves"][c["species"].index(sp)]), 1, sp["name"])
      passives = c["names"]["passives"]
      bank = c["effects"]["bank"]
      self.assertIn("Nurture", passives)
      self.assertTrue(any(ec.HOOKS[h]["name"] == "hatch" for h, _f, _p in bank.handlers))


if __name__ == "__main__":
   unittest.main()
