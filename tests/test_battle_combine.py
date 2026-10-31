#!/usr/bin/env python3
"""!Tests for the combination section of the battle compiler (D-28 section 1, package B1: tools/battlec/combine.py):
the methods' checks, the emitted tables and ids, species fields with value names and combine moves, the Python
mirrors the studio's previews use (preview(), move_candidates()) on the neutral fixture tests/data/combine (the
same cases tests/combine_tests.cpp runs in C++), the effect language's combine hook and inheritance primitives and
the script compiler's combine statements (run by CTest, or directly)."""
from __future__ import annotations

import copy
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
import script_compiler as sc  # noqa: E402
from battlec import combine as cb  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## The neutral combination fixture.
FIXTURE = ROOT / "tests/data/combine/battle.json"
## Its combine logic.
FIXTURE_LOGIC = ROOT / "tests/data/combine/combine.ceffect"
## The Starter sample.
STARTER = ROOT / "samples/starter/battle.json"
## The fixture's fusion fee per result level.
FEE_PER_LEVEL = 10
## Levels the cases use.
LOW, LOWER, HIGH, SYNTH = 3, 2, 20, 10


def compile_with(edit: Callable[[JsonDict], None] | None = None, logic: str | None = None) -> JsonDict:
   """!Compiles the fixture after an edit of its merged view (and, optionally, other combine logic), from memory.
   @param edit   callable(merged view), or None
   @param logic  replacement text of combine.ceffect, or None
   @return compiled
   """
   data = bc.view_of(FIXTURE)
   if edit is not None:
      edit(data)
   sources = bc.sources_of(FIXTURE, data)
   if logic is not None:
      sources[FIXTURE_LOGIC.resolve()] = logic
   return bc.compile_battle(FIXTURE, None, sources, platform="headless")


def method(data: JsonDict, name: str) -> JsonDict:
   """!A method of a merged view.
   @param data  the view
   @param name  its name
   @return the entry
   """
   return next(m for m in data["combine_methods"] if m["name"] == name)


def rec(name: str, level: int, **roles: Any) -> JsonDict:
   """!A record dict.
   @param name   species name
   @param level  level
   @param roles  other role values
   @return the record
   """
   return {"species": name, "level": level, **roles}


class MirrorTests(unittest.TestCase):
   """!preview() and move_candidates(): the C++ test's cases."""

   @classmethod
   def setUpClass(cls) -> None:
      """!Compiles the fixture once.
      @return None
      """
      ## The compiled fixture.
      cls.c = compile_with()

   def look(self, name: str, *records: JsonDict, **context: Any) -> JsonDict:
      """!A preview.
      @param name     method
      @param records  inputs
      @param context  context values
      @return the preview
      """
      return cb.preview(self.c, name, list(records), context or None)

   def test_fusion(self) -> None:
      """!The race table's pick and fallback, the 3-input recipe in any order, the moon recipe, the count rule.
      @return None
      """
      p = self.look("fusion", rec("Pixlet", LOW), rec("Pupkin", LOWER))
      self.assertEqual((p["status"], p["species"], p["level"], p["rule"]), ("ok", "Implet", 5, 2))
      self.assertEqual((p["odds"], p["fee"]), (1, 5 * FEE_PER_LEVEL + 2))
      self.assertEqual(self.look("fusion", rec("Pupkin", LOWER), rec("Pixlet", LOW))["species"], "Implet")
      p = self.look("fusion", rec("Pixlet", HIGH), rec("Pupkin", HIGH))
      self.assertEqual((p["species"], p["level"]), ("Gloomclaw", 16))
      p = self.look("fusion", rec("Implet", LOW), rec("Pixlet", LOW), rec("Pupkin", LOW))
      self.assertEqual((p["species"], p["level"], p["rule"]), ("Owlseer", 10, 0))
      self.assertEqual(self.look("fusion", rec("Pixlet", LOW), rec("Pupkin", LOW), rec("Wyrmling", LOW))["status"],
                       "refused")
      self.assertEqual(self.look("fusion", rec("Wyrmling", LOW), rec("Wyrmling", LOW))["status"], "refused")
      p = self.look("fusion", rec("Wyrmling", LOW), rec("Wyrmling", LOW), moon_phase="full")
      self.assertEqual((p["species"], p["level"], p["rule"]), ("Scaledrake", 24, 1))
      self.assertEqual(self.look("fusion", rec("Pixlet", LOW))["status"], "wrong_count")

   def test_others(self) -> None:
      """!Synthesis (requirement, recipe, random group), echo (base stage, odds), nest (nursery).
      @return None
      """
      p = self.look("synthesis", rec("Pupkin", LOW), rec("Pupkin", SYNTH))
      self.assertEqual((p["status"], p["slot"]), ("unfit", 0))
      p = self.look("synthesis", rec("Pupkin", SYNTH), rec("Pupkin", SYNTH))
      self.assertEqual((p["species"], p["level"]), ("Tuskhorn", 1))
      p = self.look("synthesis", rec("Pixlet", SYNTH), rec("Pupkin", SYNTH))
      fam = next(f for f in self.c["species_fields"] if f["name"] == "family")
      self.assertEqual((p["status"], p["species"], p["group"]), ("ok", None, fam["values"].index("beast")))
      # C9: a random pick's fee is the highest over its candidates (result.base_level * 2: Grimmaw's 28, not 2)
      self.assertEqual(p["fee"], 28 * 2)
      p = self.look("echo", rec("Faelord", HIGH), rec("Faelord", HIGH))
      self.assertEqual((p["species"], p["odds"], p["level"]), ("Pixlet", 100, 1))
      self.assertEqual(self.look("echo", rec("Faelord", HIGH), rec("Pupkin", HIGH))["status"], "incompatible")
      # B2: a nursery method previews as any other (the nursery runs it): same race, odds 50, Faelord's base stage
      nest = self.look("nest", rec("Faelord", HIGH), rec("Faelord", HIGH))
      self.assertEqual((nest["status"], nest["species"], nest["odds"]), ("ok", "Pixlet", 50))

   def test_moves(self) -> None:
      """!The sources' candidates: known moves (each once), the whole combine list, shared level-up moves.
      @return None
      """
      pix = rec("Pixlet", LOW, moves=["Spark", "Tap"])
      pup = rec("Pupkin", LOWER, moves=["Rush", "Tap"])
      self.assertEqual(cb.move_candidates(self.c, "fusion", "Implet", [pix, pup]), ["Spark", "Tap", "Rush"])
      self.assertEqual(cb.move_candidates(self.c, "synthesis", "Tuskhorn", [pup, pup]),
                       ["Rush", "Tap", "Smite", "Refresh"])
      fae = rec("Faelord", HIGH, moves=["Spark", "Burst"])
      self.assertEqual(cb.move_candidates(self.c, "echo", "Pixlet", [fae, fae]), ["Spark"])
      # C9: brood's sources read input b only (`from`), its moves in slot order: Burst (Pixlet's combine move), then
      # b's known moves; a's own moves are no candidates
      a = rec("Pixlet", LOW, moves=["Smite"])
      b = rec("Cube", LOW, moves=["Rush", "Burst", "Drain"])
      self.assertEqual(cb.move_candidates(self.c, "brood", "Pixlet", [a, b]), ["Burst", "Rush", "Drain"])

   def test_tables(self) -> None:
      """!The emitted tables and ids; a battle without methods still defines every combination symbol; base stages
      and combine moves.
      @return None
      """
      cpp = bc.emit(FIXTURE.name, self.c)
      self.assertIn("const CombineData kCombine", cpp)
      self.assertIn("int32_t combine_value(int32_t expr, const combine::ValueInputs& in)", cpp)
      ids = bc.emit_ids(FIXTURE.name, self.c)
      for line in ("COMBINE_METHOD_FUSION = 0", "COMBINE_METHOD_NEST = 3", "NUM_COMBINE_METHODS = 5",
                   "CONTEXT_MOON_PHASE_FULL = 2"):
         self.assertIn(line, ids)
      names = [sp["name"] for sp in self.c["species"]]
      base = self.c["combine"]["base"]
      self.assertEqual(names[base[names.index("Faelord")]], "Pixlet")
      self.assertEqual(names[base[names.index("Grimmaw")]], "Grimmaw")
      moves = self.c["names"]["moves"]
      self.assertEqual([moves[m] for m in self.c["combine"]["combine_moves"][names.index("Tuskhorn")]],
                       ["Smite", "Refresh"])
      starter = bc.compile_battle(STARTER, platform="headless")
      out = bc.emit(STARTER.name, starter)
      self.assertIn("const CombineData kCombine", out)
      self.assertNotIn("COMBINE_METHOD_", bc.emit_ids(STARTER.name, compile_with(
         lambda d: [d.pop(k) for k in ("combine_methods", "nursery", "eggs")], logic="# no combine logic\n")))


class CheckTests(unittest.TestCase):
   """!The methods' checks refuse what the engine cannot do."""

   def refused(self, pattern: str, edit: Callable[[JsonDict], None]) -> None:
      """!Asserts a compile error.
      @param pattern  regex of the message
      @param edit     the edit
      @return None
      """
      with self.assertRaisesRegex(bc.ManifestError, pattern):
         compile_with(edit)

   def test_methods(self) -> None:
      """!Names, kinds, counts, slots, deliveries.
      @return None
      """
      self.refused("unknown key", lambda d: method(d, "fusion").update(bogus=1))
      self.refused("a name of its own", lambda d: method(d, "echo").update(name="fusion"))
      self.refused("kind: one of", lambda d: method(d, "echo").update(kind="later"))
      self.refused("max_combine_inputs", lambda d: method(d, "echo")["inputs"].update(count=9))
      self.refused("one slot per input", lambda d: method(d, "echo")["inputs"].update(count=3))
      self.refused("role: a name of its own", lambda d: method(d, "echo")["inputs"]["slots"][1].update(role="a"))
      self.refused("role: a name of its own", lambda d: method(d, "echo")["inputs"]["slots"][1].update(role="count"))
      self.refused("delivery: one of", lambda d: method(d, "echo").update(delivery="mail"))
      self.refused("afterwards: one of", lambda d: method(d, "echo").update(afterwards="lost"))
      self.refused("not one of the battle's messages", lambda d: method(d, "echo")["messages"].update(refused="x"))
      # GBA: four inputs at most
      data = bc.view_of(FIXTURE)
      method(data, "echo")["inputs"] = {"count": 5, "slots": [{"role": f"r{k}"} for k in range(5)]}
      with self.assertRaisesRegex(bc.ManifestError, "max_combine_inputs"):
         bc.compile_battle(FIXTURE, None, bc.sources_of(FIXTURE, data), platform="gba")

   def test_rules(self) -> None:
      """!Recipes, tables, picks, expressions.
      @return None
      """
      self.refused("one of recipe", lambda d: method(d, "echo")["results"].append({"when": "1"}))
      self.refused("matchers", lambda d: method(d, "echo")["results"].insert(0, {"recipe": ["Pixlet"],
                                                                                   "result": "Pixlet"}))
      self.refused("is not a species", lambda d: method(d, "echo")["results"].insert(
         0, {"recipe": ["Pixlet", "Nobody"], "result": "Pixlet"}))
      self.refused("result: a species", lambda d: method(d, "echo")["results"].insert(
         0, {"recipe": ["Pixlet", "Pixlet"], "result": "Nobody"}))
      self.refused("is not a value of 'race'", lambda d: method(d, "echo")["results"].insert(
         0, {"recipe": [{"race": "giant"}, "Pixlet"], "result": "Pixlet"}))
      self.refused("one of the method's tables", lambda d: method(d, "echo")["results"].insert(0, {"table": "x"}))
      self.refused("a species field .* or 'type'", lambda d: method(d, "fusion")["tables"]["races"].update(
         key="nothing"))
      self.refused("given twice", lambda d: method(d, "fusion")["tables"]["races"]["cells"].append(
         ["beast", "sprite", "sage"]))
      self.refused("order: 'index' or a species field", lambda d: method(d, "fusion")["results"][2]["pick"].update(
         order="nothing"))
      self.refused("choose: one of", lambda d: method(d, "fusion")["results"][2]["pick"].update(choose="best"))
      self.refused("unknown name 'cand.species'", lambda d: method(d, "echo").update(level="cand.species"))
      self.refused("unknown name 'result.level'", lambda d: method(d, "echo").update(level="result.level"))
      self.refused("only a recipe names its result", lambda d: method(d, "echo")["results"][0].update(
         result="Pixlet"))
      self.refused("never hold|expected", lambda d: method(d, "synthesis")["inputs"]["slots"][0].update(
         requires="level >="))

   def test_moves_and_fields(self) -> None:
      """!Move sources, species fields with value names, combine moves.
      @return None
      """
      self.refused("each once", lambda d: method(d, "echo")["moves"]["sources"].append({"kind": "shared_level_up"}))
      self.refused("only combine_moves takes the whole list", lambda d: method(d, "echo")["moves"]["sources"][0]
                   .update(all=True))
      self.refused("limit: must be an integer 1-4", lambda d: method(d, "echo")["moves"].update(limit=5))
      self.refused("chooser: one of", lambda d: method(d, "echo")["moves"].update(chooser="ai"))
      # C9: a source's `from` names a slot role of its method and does not go with `all`; `push` is a flag
      self.refused("from: a slot role", lambda d: method(d, "brood")["moves"]["sources"][0].update({"from": "x"}))
      self.refused("reads one input", lambda d: method(d, "brood")["moves"]["sources"][0].update(all=True))
      self.refused("push are true or false", lambda d: method(d, "brood")["moves"].update(push=1))
      self.refused("a name is given twice", lambda d: d["species_fields"][0].update(values=["a", "a"]))
      self.refused("do not fit 3 bits", lambda d: d["species_fields"][0].update(values=[f"v{k}" for k in range(9)]))
      self.refused("is not a value of 'race'", lambda d: d["species"][0]["fields"].update(race="giant"))
      self.refused("'Zap' is not a move", lambda d: d["species"][0].update(combine_moves=["Zap"]))


class LogicTests(unittest.TestCase):
   """!The combine hook and the inheritance primitives in effect logic."""

   def test_hook(self) -> None:
      """!The hook's owners and value; it is the last hook; the fixture's logic compiles; a global rule may hold it.
      @return None
      """
      spec = ec.HOOKS[ec.HOOK["combine"]]
      self.assertEqual(spec["owners"], {"global"})
      self.assertEqual(spec["value"], (0, ec.MAX_COMBINE_METHOD))
      self.assertEqual(ec.HOOK["combine"], len(ec.HOOKS) - 2)   # appended (Hook::Combine; B2's hatch after it)
      self.assertIn("combine", bc.GLOBAL_HOOKS)
      c = compile_with()
      bank = c["effects"]["bank"]
      self.assertTrue(any(ec.HOOKS[h]["name"] == "combine" for h, _f, _p in bank.handlers))

   def test_primitives(self) -> None:
      """!input.N / input.ROLE subjects, the field reads, every primitive's Inherit op and mode; refusals.
      @return None
      """
      st = {"roles": [{"name": "plus", "id": 0x80}, {"name": "quiet", "id": 0x81, "stored": False}],
            "combine_methods": ["mix"], "combine_slots": [{"name": "lead", "input": 0}, {"name": "odd", "input": None}]}
      ok = ec.Unit([("t.ceffect", 'global "g" { on combine { if field.combine_method == mix { '
                                  'inherit plus from input.lead; inherit_pick plus 1; '
                                  'inherit_odds plus from input.1 1 2; inherit_add plus input.0.role(plus); } } }')],
                   st)
      self.assertTrue(ok.ok(), [str(d) for d in ok.diagnostics])
      names = [(name, v) for _at, name, v in ec.instructions(ok.bank().code) if name == "Inherit"]
      self.assertEqual([v[2] for _n, v in names], [0, 1, 2, 3])
      self.assertEqual(names[0][1][0], ec.SUBJ_BATTLER + ec.FIRST_INPUT_BATTLER)
      self.assertEqual(names[2][1][0], ec.SUBJ_BATTLER + ec.FIRST_INPUT_BATTLER + 1)

      def codes(src: str) -> list[str]:
         """!Diagnostic codes of one source."""
         return [d.code for d in ec.Unit([("t.ceffect", src)], st).diagnostics]
      self.assertIn("unknown-input", codes('global "g" { on combine { let x = input.odd.level; } }'))
      self.assertIn("unknown-input", codes('global "g" { on combine { let x = input.8.level; } }'))
      self.assertIn("combine-context", codes('global "g" { on steps { let x = field.combine_inputs; } }'))
      self.assertIn("inherit-role", codes('global "g" { on combine { inherit quiet from input.0; } }'))
      self.assertIn("encounter-arg", codes('global "g" { on combine { inherit_pick plus 40; } }'))
      self.assertIn("ref-arg", codes('global "g" { on combine { inherit plus; } }'))
      formatted = ec.format(ec.parse('global "g" {\n   on combine {\n      inherit plus from input.lead;\n   }\n}\n'))
      self.assertIn("inherit plus from input.lead;", formatted)

   def test_qualified_inputs(self) -> None:
      """!C9: `input.METHOD.ROLE` names one method's slot role (as subject and in a read), also when two methods give
      the role other indices; the catalog lists them (the fixture: fusion's c is input 2).
      @return None
      """
      st = {"roles": [{"name": "plus", "id": 0x80}], "combine_methods": ["mix", "fuse"],
            "combine_slots": [{"name": "odd", "input": None}, {"name": "mix.odd", "input": 0},
                              {"name": "fuse.odd", "input": 1}]}
      ok = ec.Unit([("t.ceffect", 'global "g" { on combine { inherit plus from input.fuse.odd; '
                                  'inherit_add plus input.mix.odd.role(plus); } }')], st)
      self.assertTrue(ok.ok(), [str(d) for d in ok.diagnostics])
      names = [v for _at, name, v in ec.instructions(ok.bank().code) if name == "Inherit"]
      self.assertEqual(names[0][0], ec.SUBJ_BATTLER + ec.FIRST_INPUT_BATTLER + 1)
      bad = ec.Unit([("t.ceffect", 'global "g" { on combine { inherit plus from input.mix.even; } }')], st)
      self.assertIn("unknown-input", [d.code for d in bad.diagnostics])
      text = 'global "g" {\n   on combine {\n      inherit plus from input.fuse.odd;\n   }\n}\n'
      formatted = ec.format(ec.parse(text))
      self.assertIn("inherit plus from input.fuse.odd;", formatted)
      slots = {e["name"]: e["input"] for e in compile_with()["names"]["combine_slots"]}
      self.assertEqual((slots["fusion.c"], slots["nest.b"], slots["brood.a"]), (2, 1, 0))


class ScriptTests(unittest.TestCase):
   """!The script compiler's combine statements against a battle with methods."""

   def compile(self, body: str) -> tuple[bool, list[str]]:
      """!Compiles one script over the fixture's methods.
      @param body  the script's statements
      @return (ok, error messages)
      """
      with tempfile.TemporaryDirectory() as tmp:
         d = Path(tmp)
         # B2: the fixture's eggs name their hatch script orb_hatch
         (d / "s.cscript").write_text("var v : u16;\nscript orb_hatch {\n}\nscript t {\n" + body + "\n}\n",
                                      encoding="utf-8")
         (d / "scripts.json").write_text(json.dumps({"schema": "capsule.scripts/1", "sources": ["s.cscript"],
                                                     "battle": str(FIXTURE)}), encoding="utf-8")
         try:
            result = sc.compile_file(d / "scripts.json")
         except sc.ScriptError as e:
            return False, [str(e)]
         errors = [str(x) for x in result.errors()]
         return not errors, errors

   def test_statements(self) -> None:
      """!Every kind compiles; unknown methods, counts and a missing result are refused.
      @return None
      """
      ok, errors = self.compile("combine open fusion -> var(v);\ncombine run fusion (0, 1, 2) -> var(v);\n"
                                "combine preview echo (0, 1) -> var(v);\ncombine status echo (0, 1) -> var(v);\n"
                                "combine fee fusion (0, 1) -> var(v);")
      self.assertTrue(ok, errors)
      self.assertEqual(sc.COMBINE_KINDS, ["open", "run", "preview", "status", "fee"])
      ok, errors = self.compile("combine open nothing -> var(v);")
      self.assertFalse(ok)
      self.assertIn("not a combination method", " ".join(errors))
      ok, errors = self.compile("combine run echo (0, 1, 2) -> var(v);")
      self.assertFalse(ok)
      self.assertIn("the method takes 2 to 2", " ".join(errors))
      ok, errors = self.compile("combine open fusion;")
      self.assertFalse(ok)
      self.assertIn("needs -> var(v)", " ".join(errors))


if __name__ == "__main__":
   unittest.main()
