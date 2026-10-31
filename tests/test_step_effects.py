#!/usr/bin/env python3
"""Step effects (F3, D-27 section 4): the battle compiler's step_effects section (the records, their checks, the
field rules' count, the ids), the effect compiler's steps hook (its owners, field.step_effect and where it may be
read, the global rules), the script compiler's implicit counters / member / outcome variables and records, and the
save image's script section (the counters' shapes) on tests/data/steps."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import battle_compiler as bc  # noqa: E402
import effect_compiler as ec  # noqa: E402
import save_layout  # noqa: E402
import save_shape  # noqa: E402
import script_compiler as sc  # noqa: E402

## The fixture.
FIXTURE = ROOT / "tests" / "data" / "steps"
## Its battle manifest.
BATTLE = FIXTURE / "battle.json"
## Its scripts manifest.
SCRIPTS = FIXTURE / "scripts.json"
## The step effects in its structure's order.
NAMES = ["sting", "rot", "bond", "hatch"]


def structure() -> dict:
   """The fixture's structure manifest.
   @return its JSON
   """
   return json.loads((FIXTURE / "structure.json").read_text(encoding="utf-8"))


def compile_with(edit: dict, tmp: Path) -> dict:
   """Compiles a copy of the fixture with the structure's step_effects replaced.
   @param edit  the new step_effects list
   @param tmp   a folder for the copy
   @return compile_battle() result
   """
   for f in FIXTURE.iterdir():
      (tmp / f.name).write_bytes(f.read_bytes())
   st = structure()
   st["step_effects"] = edit
   (tmp / "structure.json").write_text(json.dumps(st), encoding="utf-8")
   # the logic without the steps handlers (they name the fixture's step effects): the items' use logic only
   logic = (FIXTURE / "logic.ceffect").read_text(encoding="utf-8")
   (tmp / "logic.ceffect").write_text(logic[logic.index('item "Salve"'):], encoding="utf-8")
   return bc.compile_battle(tmp / "battle.json")


class BattleSideTests(unittest.TestCase):
   """The structure's step_effects in the battle data."""

   def test_fixture(self) -> None:
      """The records, the messages' ids, the count in kFieldRules, the ids."""
      c = bc.compile_battle(BATTLE)
      steps = c["field"]["steps"]
      self.assertEqual([e["name"] for e in steps], NAMES)
      sting, rot, bond, _hatch = steps
      self.assertEqual((sting["every"], sting["damage"], sting["damage_id"]), (4, "survive", 1))
      self.assertEqual((rot["every"], rot["damage"], rot["script"]), (2, "faint", "rot_down"))
      msgs = c["names"]["messages"]
      self.assertEqual(sting["message_ids"], [bc.NONE8, msgs.index("sting_survived"), bc.NONE8])
      self.assertEqual(rot["message_ids"], [msgs.index("rot_hurt"), bc.NONE8, msgs.index("rot_fainted")])
      self.assertEqual(bond["label"], "Bond")
      cpp = bc.emit("battle.json", c)
      self.assertIn("kFieldRules = {0xFFFF, 2, 0, 0xFFFF, 4, 0, 0xFFFF, 0}", cpp)
      ids = bc.emit_ids("battle.json", c)
      self.assertIn("STEP_EFFECT_HATCH = 3;", ids)
      self.assertIn("NUM_STEP_EFFECTS = 4;", ids)

   def test_without(self) -> None:
      """A battle without step effects: count 0, no ids."""
      import tempfile
      with tempfile.TemporaryDirectory() as tmp:
         c = compile_with([], Path(tmp))
      self.assertEqual(c["field"]["steps"], [])
      self.assertNotIn("STEP_EFFECT", bc.emit_ids("battle.json", c))

   def test_refused(self) -> None:
      """Bad records are refused with the record's place."""
      import tempfile
      good = structure()["step_effects"]
      cases = [
         ([{"name": "x"}], "every"),
         ([{"name": "x", "every": 0}], "every"),
         ([{"name": "x", "every": bc.STEP_EFFECT_MAX_EVERY + 1}], "every"),
         ([{"name": "x", "every": True}], "every"),
         ([{"name": "x", "every": 2, "damage": "explode"}], "damage"),
         ([{"name": "x", "every": 2, "messages": {"hurt": "no_such"}}], "not one of the battle's messages"),
         ([{"name": "x", "every": 2, "messages": {"later": "rot_hurt"}}], "unknown"),
         ([{"name": "X", "every": 2}], "name"),
         ([{"name": "member", "every": 2}], "taken"),
         ([{"name": "x", "every": 2}, {"name": "x", "every": 3}], "name of its own"),
         ([{"name": "x", "every": 2, "label": ""}], "label"),
         ([{"name": "x", "every": 2, "script": ""}], "script"),
         ([{"name": f"s{k}", "every": 2} for k in range(bc.MAX_STEP_EFFECTS + 1)], "at most"),
         ([dict(good[0], color=1)], "unknown"),
      ]
      for edit, why in cases:
         with self.subTest(edit=edit[:1]), tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(bc.ManifestError, why):
               compile_with(edit, Path(tmp))


class LogicTests(unittest.TestCase):
   """The steps hook in effect logic."""

   def test_hook(self) -> None:
      """The hook's owners and value; field.step_effect only in a steps handler, compared with a step effect's
      name; a global rule may hold it."""
      spec = ec.HOOKS[ec.HOOK["steps"]]
      self.assertEqual(spec["owners"], {"global", "status", "passive", "item"})
      self.assertEqual(spec["value"], (0, ec.MAX_STEP_EFFECT))
      self.assertIn("steps", bc.GLOBAL_HOOKS)
      # appended (capsule::battle::effect::Hook::Steps); B1's combine hook follows it
      self.assertEqual(ec.HOOK["steps"], ec.HOOK["combine"] - 1)
      st = {"statuses": ["Sting"], "step_effects": [{"name": "sting"}, {"name": "rot"}], "items": ["Bell"]}
      ok = ec.Unit([("t.ceffect", 'status "Sting" { on steps { if field.step_effect == rot { fail; } } }')], st)
      self.assertTrue(ok.ok(), [str(d) for d in ok.diagnostics])
      bad = ec.Unit([("t.ceffect", 'item "Bell" { on use_field { if field.step_effect == rot { fail; } } }')], st)
      self.assertIn("steps-context", [d.code for d in bad.diagnostics])
      unknown = ec.Unit([("t.ceffect", 'status "Sting" { on steps { if field.step_effect == nope { fail; } } }')],
                        st)
      self.assertFalse(unknown.ok())
      move = ec.Unit([("t.ceffect", 'move "Spark" { on steps { fail; } }')], {"moves": ["Spark"]})
      self.assertFalse(move.ok())

   def test_fixture_handlers(self) -> None:
      """The fixture's owners with steps handlers compile into the bank: the global rule, both statuses, two
      passives, the Bell (an owner's hook mask names the hook)."""
      c = bc.compile_battle(BATTLE)
      self.assertEqual(c["effects"]["owners"], len(c["effects"]["bank"].owners))
      text = (FIXTURE / "logic.ceffect").read_text(encoding="utf-8")
      self.assertEqual(text.count("   on steps {"), 6)


class ScriptSideTests(unittest.TestCase):
   """The script compiler's implicit declarations and records; the save image's counters."""

   def test_bank(self) -> None:
      """Each effect's counter (u16, 0 to every - 1, permanent), the member / outcome variables, the records."""
      comp = sc.compile_file(SCRIPTS)
      self.assertEqual(comp.errors(), [])
      r = comp.result
      for e, every in zip(NAMES, (4, 2, 8, 3)):
         v = comp.vars[bc.step_effect_var(e)]
         self.assertEqual((v["lo"], v["hi"], v["lifetime"]), (0, every - 1, "permanent"))
      self.assertIn(bc.STEP_EFFECT_MEMBER_VAR, comp.vars)
      self.assertEqual(comp.vars[bc.STEP_EFFECT_OUTCOME_VAR]["hi"], len(bc.STEP_EFFECT_OUTCOMES) - 1)
      recs = r["step_effects"]
      self.assertEqual([x[-1] for x in recs], NAMES)
      sting, rot = recs[0], recs[1]
      self.assertEqual((sting[2], sting[3], sting[4]), (4, sc.NO_SCRIPT_ID, 1))
      self.assertEqual(rot[3], comp.script_ids["rot_down"])
      cpp = comp.emit_cpp("scripts.json")
      self.assertIn("const StepEffectInfo kStepEffects[] = {", cpp)
      self.assertIn("kStepEffects, 4, ", cpp)

   def test_bank_without(self) -> None:
      """A battle without step effects: no implicit variables, the bank's initializer as before F3."""
      comp = sc.compile_file(ROOT / "tests" / "data" / "field" / "scripts.json")
      self.assertEqual(comp.errors(), [])
      self.assertFalse(any(n.startswith(bc.STEP_EFFECT_VAR_PREFIX) for n in comp.vars))
      self.assertNotIn("kStepEffects", comp.emit_cpp("scripts.json"))

   def test_unknown_script(self) -> None:
      """A step effect naming a script the game does not declare is an error of the scripts."""
      import tempfile
      with tempfile.TemporaryDirectory() as tmp:
         for f in FIXTURE.iterdir():
            (Path(tmp) / f.name).write_bytes(f.read_bytes())
         st = structure()
         st["step_effects"][1]["script"] = "no_such"
         (Path(tmp) / "structure.json").write_text(json.dumps(st), encoding="utf-8")
         comp = sc.compile_file(Path(tmp) / "scripts.json")
      self.assertTrue(any("no_such" in e.message for e in comp.errors()))

   def test_save_shape(self) -> None:
      """The counters live in the permanent variables' sub-bank (its shape names them with their range)."""
      comp = sc.compile_file(SCRIPTS)
      banks = save_shape.shapes(comp)
      perm = next(b for b in banks if b["bank"] == "vars.permanent")
      names = [e[0] for e in perm["entries"]]
      for e in NAMES:
         self.assertIn(bc.step_effect_var(e), names)
      self.assertGreater(save_layout.script_bytes(banks), 0)
      # a test save's counters in the image (save_layout.script_values(): little-endian at their offsets, clamped)
      entry = next(e for e in perm["entries"] if e[0] == bc.step_effect_var("sting"))
      data = save_layout.script_values(banks, {bc.step_effect_var("sting"): 3, bc.step_effect_var("rot"): 9})
      raw = data["vars.permanent"]
      self.assertEqual(int.from_bytes(raw[entry[1]:entry[1] + entry[2]], "little"), 3)
      rot = next(e for e in perm["entries"] if e[0] == bc.step_effect_var("rot"))
      self.assertEqual(int.from_bytes(raw[rot[1]:rot[1] + rot[2]], "little"), 1)   # clamped to every - 1
      with self.assertRaises(KeyError):
         save_layout.script_values(banks, {"no_such": 1})
      c = bc.compile_battle(BATTLE)
      plain = save_layout.image(c, {}, banks=banks)
      counted = save_layout.image(c, {}, banks=banks, values={bc.step_effect_var("sting"): 3})
      self.assertEqual(len(plain), len(counted))
      self.assertNotEqual(plain, counted)


if __name__ == "__main__":
   unittest.main()
