#!/usr/bin/env python3
"""!Tests of the field rules in the compilers (D-27 sections 1 and 2, package F1): the structure's lead rule and
timed field effects (tools/battlec/field.py), the battle's global owners (the game's encounter rules), the effect
language's encounter hooks and primitives (operands interval-checked) and its field effect statement and reads
(tools/effect_compiler.py), the ruleset pipelines encounter_rate and encounter_level (tools/ruleset_compiler.py), and
the script compiler's implicit field effect variables, expiry scripts and statements (tools/script_compiler.py), on
the neutral fixture tests/data/field (run by CTest, or directly)."""
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
import effect_compiler as ec  # noqa: E402
import ruleset_compiler as rc  # noqa: E402
import save_shape  # noqa: E402
import script_compiler as sc  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## The neutral field fixture.
FIXTURE = ROOT / "tests/data/field/battle.json"
## Its effect logic.
FIXTURE_LOGIC = ROOT / "tests/data/field/logic.ceffect"
## Its ruleset.
FIXTURE_RULESET = ROOT / "tests/data/field/ruleset.json"
## Its scripts manifest.
FIXTURE_SCRIPTS = ROOT / "tests/data/field/scripts.json"
## A ruleset without the encounter pipelines (the defaults).
PLAIN_RULESET = ROOT / "tests/data/inventory/ruleset.json"
## The kind's place in an effect bank's owner tuple (effect_compiler.Bank.owners).
OWNER_KIND_FIELD = 6


def compile_with(edit: Callable[[JsonDict], None], logic: str | None = None,
                 ruleset: Callable[[JsonDict], None] | None = None) -> JsonDict:
   """!Compiles the fixture after an edit of its merged view (and, optionally, other logic or ruleset), from memory.
   @param edit     callable(merged view)
   @param logic    replacement text of logic.ceffect, or None
   @param ruleset  callable(ruleset data), or None
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
   return bc.compile_battle(FIXTURE, None, sources)


def keep(_data: JsonDict) -> None:
   """!An edit that changes nothing.
   @param _data  the merged view
   @return None
   """


## F2: what the fixture's field actions need of its scripts (their scripts, the flag and variable they read), added
## after a test's own text.
F2_STUB = ("\nflag badge_one;\nvar story : u8 0..9;\nscript do_paddle { }\nscript do_shove { }\n"
           "script do_lamp { }\nscript do_rope { }\n")


def scripts(text: str) -> sc.Compiler:
   """!Compiles the fixture's scripts manifest with another source text (and F2_STUB, with the fixture's world).
   @param text  field.cscript's text
   @return the compiler (run)
   """
   manifest = json.loads(FIXTURE_SCRIPTS.read_text(encoding="utf-8"))
   comp = sc.Compiler(manifest, FIXTURE_SCRIPTS.parent, sources={"field.cscript": text + F2_STUB})
   comp.world_field = sc.load_world_field(FIXTURE_SCRIPTS.parent / manifest["world"])
   return comp.run()


class FieldSectionTests(unittest.TestCase):
   """!The structure's lead and field_effects sections."""

   def test_fixture(self) -> None:
      """The fixture's three effects, its lead rule (one more growth rule) and the ids."""
      c = compile_with(keep)
      effects = c["field"]["effects"]
      self.assertEqual([e["name"] for e in effects], ["ward", "glow", "lure"])
      self.assertEqual([(e["unit"], e["stack"], e["persists"]) for e in effects],
                       [("steps", "refuse", True), ("minutes", "add", False), ("steps", "refresh", True)])
      rule = c["transform"]["rules"][c["field"]["lead_rule"]]
      self.assertEqual((rule["owner"], rule["expr"]), ("lead", "current_hp > 0 && level >= 3"))
      ids = bc.emit_ids("battle.json", c)
      self.assertIn("FIELD_EFFECT_LURE = 2", ids)
      self.assertIn("NUM_FIELD_EFFECTS = 3", ids)
      # C8: the hooks' member's rule (hook_when) follows the lead rule
      hook = c["field"]["hook_lead_rule"]
      self.assertEqual(hook, 1)
      rule = c["transform"]["rules"][hook]
      self.assertEqual((rule["owner"], rule["expr"]), ("lead", "level >= 3"))
      first = c["field"]["actions"]["first_rule"]   # F2: the fixture's four field actions follow the lead rules
      self.assertIn(f"kFieldRules = {{0x0000, 3, 4, 0x{first:04X}, 0, 0, 0x{hook:04X}, 0}}",
                    bc.emit("battle.json", c))

   def test_no_sections(self) -> None:
      """Without the sections: no lead rule (the first member that can battle), no effects."""
      self.assertEqual(bc.compile_field_effects(None, "s"), [])
      self.assertEqual(bc.compile_lead(None, None, [], "s"), bc.NO_LEAD_RULE)
      self.assertEqual(bc.compile_hook_lead(None, None, [], "s"), bc.NO_LEAD_RULE)

   def test_hook_lead(self) -> None:
      """C8: hook_when alone keeps the default lead; an empty one is refused; without it the lead runs the hooks."""
      def lead(section: Any) -> Callable[[JsonDict], None]:
         """!An edit setting the lead section.
         @param section  the section
         @return the edit
         """
         return lambda d: d.__setitem__("lead", section)
      c = compile_with(lead({"hook_when": "1"}))
      self.assertEqual(c["field"]["lead_rule"], bc.NO_LEAD_RULE)
      self.assertEqual(c["transform"]["rules"][c["field"]["hook_lead_rule"]]["expr"], "1")
      self.assertEqual(compile_with(lead({"when": "level >= 1"}))["field"]["hook_lead_rule"], bc.NO_LEAD_RULE)
      with self.assertRaisesRegex(bc.ManifestError, "hook_when"):
         compile_with(lead({"when": "level >= 1", "hook_when": " "}))

   def test_refused(self) -> None:
      """Bad records name what is wrong."""
      cases = [
         ({"name": "Ward", "unit": "steps"}, "lower-case identifier"),
         ({"name": "ward", "unit": "hours"}, "unit"),
         ({"name": "ward", "unit": "steps", "stack": "merge"}, "stack"),
         ({"name": "ward", "unit": "steps", "label": "x" * (bc.FIELD_EFFECT_MAX_LABEL + 1)}, "label"),
         ({"name": "ward", "unit": "steps", "persists": 1}, "persists"),
         ({"name": "ward", "unit": "steps", "colour": 1}, "unknown key"),
      ]
      for record, words in cases:
         with self.subTest(record=record):
            with self.assertRaisesRegex(bc.ManifestError, words):
               bc.compile_field_effects([record], "structure.json")
      with self.assertRaisesRegex(bc.ManifestError, "own"):
         bc.compile_field_effects([{"name": "a", "unit": "steps"}, {"name": "a", "unit": "steps"}], "s")
      with self.assertRaisesRegex(bc.ManifestError, "at most"):
         bc.compile_field_effects([{"name": f"e{k}", "unit": "steps"} for k in range(bc.MAX_FIELD_EFFECTS + 1)], "s")

   def test_lead_refused(self) -> None:
      """A lead rule needs an expression that can hold."""
      def bad(text: Any) -> Callable[[JsonDict], None]:
         """!An edit setting the lead rule's expression."""
         def edit(d: JsonDict) -> None:
            """!Sets it."""
            d["lead"] = {"when": text}
         return edit
      for text, words in ((None, "expression"), ("level > 300", "never hold"), ("nope > 1", "nope")):
         with self.subTest(text=text):
            with self.assertRaisesRegex(bc.ManifestError, words):
               compile_with(bad(text))


class EncounterLogicTests(unittest.TestCase):
   """!The encounter hooks, their primitives and the game's global owners."""

   def test_global_owners_last(self) -> None:
      """A global owner comes after every other owner (their ids stay) and holds the encounter hooks only."""
      c = compile_with(keep)
      global_kind = list(ec.OWNER_KINDS).index("global")
      kinds = [o[OWNER_KIND_FIELD] for o in c["effects"]["bank"].owners]
      self.assertEqual(kinds[-1], global_kind)
      self.assertEqual(kinds.count(global_kind), 1)
      bad = FIXTURE_LOGIC.read_text(encoding="utf-8") + '\nglobal "late" { on switch_in { text skip; } }\n'
      with self.assertRaisesRegex(bc.ManifestError, "encounter, encounter_made, steps"):
         compile_with(keep, logic=bad)

   def test_encoding(self) -> None:
      """Each primitive is the Encounter op with its kind and three operands (the unused ones 0)."""
      unit = ec.load_unit(ROOT / "tests/data/effects/vm.json")
      bank = unit.bank()
      program = dict(zip(bank.labels, bank.programs))['item "Charm" on encounter']
      code = bank.code[program[0]:program[0] + program[1]]
      ops = [(name, values) for _at, name, values in ec.instructions(code)]
      kinds = [values[0] for name, values in ops if name == "Encounter"]
      self.assertEqual(kinds, [0, 1, 2, 3, 4, 5])
      self.assertIn(("Encounter", [ec.ENCOUNTER_KINDS.index("encounter_type"), 1]), ops)   # type Spark
      self.assertEqual(ec.OPS[ec.OP["Encounter"]][2], ec.ENCOUNTER_OPERANDS)

   def test_operand_ranges(self) -> None:
      """Operands whose analysed range leaves the allowed one are refused; unknown ones are left to the engine."""
      for stmt in ("encounter_rate 1 0", "encounter_levels 300 0", "encounter_skip_below 256",
                   "encounter_type Volt 1 0", "encounter_field size 70000 1 1"):
         with self.subTest(stmt=stmt):
            logic = FIXTURE_LOGIC.read_text(encoding="utf-8").replace("encounter_rate 2 1;", stmt + ";")
            with self.assertRaisesRegex(bc.ManifestError, "it takes"):
               compile_with(keep, logic=logic)

   def test_field_effect_statement(self) -> None:
      """field_effect start names an effect of the structure; field.effect_active / effect_remaining read it."""
      logic = FIXTURE_LOGIC.read_text(encoding="utf-8").replace("field_effect start lure 10;",
                                                                 "field_effect start nope 10;")
      with self.assertRaisesRegex(bc.ManifestError, "nope"):
         compile_with(keep, logic=logic)
      c = compile_with(keep)
      names = {name for _at, name, _v in ec.instructions(c["effects"]["bank"].code)}
      self.assertTrue({"Encounter", "FieldEffect"} <= names)

   def test_field_effect_amount_range(self) -> None:
      """C8 (D-27 review): a constant amount outside 1..MAX_AMOUNT is refused with the script compiler's message;
      the effect compiler's limit is battlec/field.py's."""
      self.assertEqual(ec.FIELD_EFFECT_MAX_AMOUNT, bc.FIELD_EFFECT_MAX_AMOUNT)
      text = FIXTURE_LOGIC.read_text(encoding="utf-8")
      for amount in ("0", "-5", "65536", "100000"):
         with self.subTest(amount=amount):
            logic = text.replace("field_effect start lure 10;", f"field_effect start lure {amount};")
            with self.assertRaisesRegex(bc.ManifestError, f"the amount is {amount}; it is 1 to 65535"):
               compile_with(keep, logic=logic)
      for amount in ("1", "65535"):
         with self.subTest(amount=amount):
            compile_with(keep, logic=text.replace("field_effect start lure 10;", f"field_effect start lure {amount};"))

   def test_hooks(self) -> None:
      """Both hooks: owners passive, item and global; value ranges; block allowed."""
      specs = {h["name"]: h for h in ec.HOOKS}
      for name in ("encounter", "encounter_made"):
         self.assertEqual(specs[name]["owners"], {"passive", "item", "global"})
         self.assertEqual(specs[name]["results"], {"block"})
      # F3: the steps hook follows them (B1: then the combine hook)
      self.assertEqual([h["name"] for h in ec.HOOKS][-5:], ["encounter", "encounter_made", "steps", "combine",
                                                            "hatch"])


class PipelineTests(unittest.TestCase):
   """!The encounter pipelines."""

   def test_defaults(self) -> None:
      """Without them: the table's odds times the scale, a level between the bounds by one roll."""
      data = json.loads(PLAIN_RULESET.read_text(encoding="utf-8"))
      self.assertNotIn("encounter_rate", data.get("pipelines", {}))
      bound = rc.bind(rc.load({k: v for k, v in data.items() if k != "editor"}, "t"))
      odds = rc.run_pipeline(bound, "encounter_rate", {"rate_n": 32, "rate_m": 256, "rate_scale": 512,
                                                       "lead_level": 9, "method": 0})["outputs"]
      self.assertEqual((odds["sure"], odds["chance"], odds["out_of"]), (0, 32 * 512, 256 * 256))
      wide = rc.run_pipeline(bound, "encounter_rate", {"rate_n": 300, "rate_m": 1000, "rate_scale": 512,
                                                       "lead_level": 9, "method": 0})["outputs"]
      self.assertEqual((wide["chance"], wide["out_of"]), (600, 1000))   # no room for the fraction bits
      level = rc.run_pipeline(bound, "encounter_level", {"min_level": 3, "max_level": 7, "lead_level": 0,
                                                         "level_bias": 0, "method": 0}, roll=4)["outputs"]
      self.assertEqual(level["level"], 7)
      # the fixture's own: a bias asks for the highest level
      fixture = rc.bind(rc.load({k: v for k, v in json.loads(FIXTURE_RULESET.read_text(encoding="utf-8")).items()
                                 if k != "editor"}, "t"))
      biased = rc.run_pipeline(fixture, "encounter_level", {"min_level": 3, "max_level": 7, "lead_level": 0,
                                                            "level_bias": 1, "method": 0}, roll=0)["outputs"]
      self.assertEqual(biased["level"], 7)

   def test_points_refused(self) -> None:
      """The field pipelines run out of battle: no point steps."""
      def with_point(rs: JsonDict) -> None:
         """!A point step in encounter_rate."""
         rs["points"] = {**rs.get("points", {}), "lure": {"holder": "user", "sources": ["user.passive"],
                                                           "range": [0, 65535]}}
         point = "lure"
         rs["pipelines"]["encounter_rate"] = [{"let": "sure", "expr": "0"}, {"let": "chance", "expr": "rate_n"},
                                              {"point": point, "on": "chance"}, {"let": "out_of", "expr": "rate_m"}]
      with self.assertRaisesRegex(bc.ManifestError, "point steps"):
         compile_with(keep, ruleset=with_point)


class ScriptTests(unittest.TestCase):
   """!The script compiler's side: implicit variables and expiry scripts, the statements."""

   def test_implicit(self) -> None:
      """Each effect has a permanent u16 variable; those with an expiry message a script; the records."""
      comp = scripts("")
      self.assertFalse([d for d in comp.errors()], [str(d) for d in comp.errors()])
      r = comp.result
      self.assertEqual([n for n in r["var_names"] if n.startswith("field_effect_")],
                       ["field_effect_ward", "field_effect_glow", "field_effect_lure"])
      self.assertIn("field_effect_end_ward", r["names"])
      self.assertNotIn("field_effect_end_glow", r["names"])
      records = r["field_effects"]
      self.assertEqual([(u, st, fl) for _lb, _v, _sc, u, st, fl in records], [(0, 2, 1), (1, 1, 0), (0, 0, 1)])
      self.assertEqual(records[1][2], sc.NO_VAR)   # glow: no expiry script
      cpp = comp.emit_cpp("scripts.json")
      self.assertIn("kFieldEffects", cpp)
      # the save shape of the permanent variables follows them
      shapes = {sb["bank"]: sb for sb in save_shape.shapes(comp)}
      self.assertIn("field_effect_ward", json.dumps(shapes["vars.permanent"]["entries"]))

   def test_statements(self) -> None:
      """start / active / remaining and their checks."""
      comp = scripts("var ok : u8 0..1;\nscript s { field_effect start ward 5 -> var(ok); "
                     "field_effect active lure -> var(ok); }\n")
      self.assertFalse(comp.errors(), [str(d) for d in comp.errors()])
      bad = [("script s { field_effect start nope 5; }", "nope"),
             ("script s { field_effect start ward 0; }", "it is 1 to 65535"),
             ("script s { field_effect start ward 70000; }", "it is 1 to 65535"),
             ("var v : u8;\nscript s { field_effect remaining ward; }", "needs -> var"),
             ("var field_effect_ward : u8;", "already declared")]
      for text, words in bad:
         with self.subTest(text=text):
            comp = scripts(text)
            self.assertTrue(any(words in str(d) for d in comp.errors()), [str(d) for d in comp.errors()])

   def test_expiry_text(self) -> None:
      """An expiry message with quotes and a line break stays one message."""
      effects = [{"name": "ward", "expiry": 'The "ward"\nfades.\\'}]
      source = sc.field_effect_source(effects)
      items = sc.Parser(source, "x").file_items()
      body = [i for i in items if i["k"] == "script"][0]["body"]
      self.assertEqual(body[0]["text"]["text"], 'The "ward"\nfades.\\')


if __name__ == "__main__":
   unittest.main()
