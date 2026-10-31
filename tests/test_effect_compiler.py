#!/usr/bin/env python3
"""Tests for tools/effect_compiler.py (D-23 section 1): the `.ceffect` language and its AST round trips, the canonical
formatter, every diagnostic, the budget and termination checks, the bytecode (golden listing), the pure backend
(evaluate_pure against the generated C++) and the command line.

Golden files: tests/data/effects/golden/ (the language examples, tests/data/effects/examples.ceffect). Rewrite them
with EFFECT_GOLDEN_RECORD=1 after an intended change."""
from __future__ import annotations

import copy
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import effect_compiler as ec  # noqa: E402

DATA = ROOT / "tests" / "data" / "effects"
GOLDEN = DATA / "golden"
RECORD = os.environ.get("EFFECT_GOLDEN_RECORD") == "1"


def load_json(name: str) -> dict[str, Any]:
   """Reads a fixture JSON file.
   @param name  file name under tests/data/effects
   @return the object
   """
   return json.loads((DATA / name).read_text(encoding="utf-8"))


## The language examples' names (tests/data/effects/examples_structure.json: invented, E11).
EXAMPLES = load_json("examples_structure.json")
VM = load_json("vm_structure.json")


def compile_text(text: str, structure: dict[str, Any] | None = None) -> ec.Unit:
   """Compiles one in-memory source.
   @param text       the source
   @param structure  names (default: the examples fixture's)
   @return the unit
   """
   return ec.Unit([("test.ceffect", text)], EXAMPLES if structure is None else structure)


def codes(unit: ec.Unit, severity: str = "error") -> list[str]:
   """Diagnostic codes of a unit.
   @param unit      the unit
   @param severity  'error' or 'warning'
   @return codes
   """
   return [d.code for d in unit.diagnostics if d.severity == severity]


def golden(name: str, text: str) -> str:
   """A golden file's text (recorded first when EFFECT_GOLDEN_RECORD=1).
   @param name  file name under golden/
   @param text  the current output
   @return the golden text
   """
   path = GOLDEN / name
   if RECORD:
      path.write_text(text, encoding="utf-8")
   return path.read_text(encoding="utf-8")


## Sources covering every syntax form (round trips).
CORPUS = [
   'move "Soar" {}',
   '# only a comment\n',
   'move "Soar" {   # trailing after the brace\n   use {}   # after an empty block\n}   # after the owner\n',
   'passive "Crackle" {\n   tags [hovering];\n   announce;\n   on residual {\n      # alone\n      text skip;   # x\n'
   '   }\n}\n',
   'move "Soar" { use { if user.hp > 1 { fail; } else if user.hp > 2 { stop; } else if 1 {} else {} } }',
   'move "Soar" { use { if 1 {} else { text skip; } chance 1/2 {} else {} chance (1 + 2)/(3 * 4) { fail; } } }',
   'move "Soar" { use { chance user.hp * 2/3 { } chance 1/(2 / 3) {} } }',
   'move "Soar" { use { let a = -1; let b = - a; let c = a - -1; let d = not not a; let e = -(a + 1); '
   'let f = a << 2 >> 1; let g = (a or b) and c; let h = a or b and c; let i = (a == b) == c; } }',
   'move "Soar" { use { let a = 1.50; let b = 0.667; let c = pick(a, 1.0, 2, 3); let x = scale(a, 0.25); '
   'let y = clamp(a, 0, 3) + abs(-a) + min(a, 1) + max(a, 2) + rand(3) + between(1, 2) + chance(1, 2); } }',
   'move "Soar" { use { repeat user.hp max 16 { yield; } for f in foes(user) { heal f 1; } ask choice user into k; '
   'call s; return; } sub s { } }',
   'move "Soar" { use { switch user; switch user pass; switch target random; lock user free; unlock user; '
   'use_move user Soar; credit 1; set_types user Ember, Tide; set_item user Berrypack; set_passive user Crackle; '
   'consume_item user; } }',
   'move "Soar" { hit { damage; damage_hp target 1 typed quiet no_faint_check; self_hit 40; '
   'inflict target Scorch quiet; '
   'inflict target "Deep Blight"; cure target; stage target atk -1; vol_on user aloft 2; vol_off user aloft; '
   'side_on side(user) Caltrops add_layer; side_on side(user) Barrier 5; side_off side(user) Barrier; '
   'weather Drizzle 5; weather Glare; text skip; announce; } }',
   'passive "Crackle" { on try_hit { immune; } on before_accuracy { block; } on set_status { block skip; } '
   'on before_damage { modify by 2; modify add -3; modify set value / 2; } }',
   'passive "Crackle" { on try_hit { immune heal 0.25; } point power when move.type == Ember { modify by 1.5; } }',
   'passive "Crackle" { on residual { holder.counter(daze) += 1; holder.word(decoy) -= 2; '
   'holder.status_aux = side(holder).layers(Caltrops) + field.weather_turns + user.taken.from + holder.type(0); } }',
   'global "Rules" { on residual when holder.has(hovering) and not holder.has_type(Wing) or move.has(contact) {} }',
   'status "Deep Blight" { on start {} }',
]


class RoundTripTests(unittest.TestCase):
   """parse / format / JSON round trips."""

   def round_trip(self, text: str) -> None:
      """Checks every round trip of one source.
      @param text  the source
      """
      ast = ec.parse(text)
      canon = ec.format(ast)
      self.assertEqual(ec.parse(canon), ast, canon)
      self.assertEqual(ec.format(ec.parse(canon)), canon)
      self.assertEqual(ec.from_json(ec.to_json(ast)), ast)
      self.assertEqual(ec.from_json(json.loads(ec.to_json(ast))), ast)

   def test_corpus(self) -> None:
      """Every corpus source round-trips."""
      for text in CORPUS:
         with self.subTest(text=text[:60]):
            self.round_trip(text)

   def test_fixtures(self) -> None:
      """Every fixture source round-trips; the neutral fixture is canonical as stored."""
      for path in sorted(DATA.glob("**/*.ceffect")):
         with self.subTest(path=path.name):
            self.round_trip(path.read_text(encoding="utf-8"))
      text = (DATA / "vm_ops.ceffect").read_text(encoding="utf-8")
      self.assertEqual(ec.format(ec.parse(text)), text)

   def test_canonical_golden(self) -> None:
      """The language examples' canonical text."""
      text = ec.format(ec.parse((DATA / "examples.ceffect").read_text(encoding="utf-8")))
      self.assertEqual(text, golden("examples.canonical.ceffect", text))

   def test_d23_ast_example(self) -> None:
      """The AST of D-23 section 1.6's example."""
      ast = ec.parse('passive "Crackle" { on damaging_hit when move.has(contact) and other.alive '
                     '{ chance 1/3 { inflict other Numb quiet; } } }')
      want = {"owner": {"kind": "passive", "name": "Crackle"},
              "handlers": [{"on": "damaging_hit",
                            "when": {"op": "and", "args": [{"ref": "move.has", "arg": "contact"},
                                                            {"ref": "other.alive"}]},
                            "body": [{"stmt": "chance", "n": 1, "m": 3,
                                      "body": [{"stmt": "inflict", "who": "other", "status": "Numb",
                                                "quiet": True}]}]}]}
      self.assertEqual(ast["owners"][0], want)
      self.assertEqual(ec.owner_ast(ast, "passive:Crackle"), {"schema": ec.SCHEMA_AST, **want})
      self.assertEqual(ec.format(ec.owner_ast(ast, "Crackle")), ec.format(ast))

   def test_canonical_details(self) -> None:
      """Decimals normalize, negative literals fold, names quote when needed, comments keep their place."""
      ast = ec.parse('move "Soar" { use { let a = 1.50 + -(2); let b = "Deep Blight"; let c = "value"; } }')
      body = ast["owners"][0]["handlers"][0]["body"]
      self.assertEqual(body[0]["value"], {"op": "+", "args": [{"dec": "1.5"}, -2]})
      self.assertEqual(body[1]["value"], {"name": "Deep Blight"})
      text = ec.format(ast)
      self.assertIn('let b = "Deep Blight";', text)
      self.assertIn('let c = "value";', text)
      self.assertEqual(ec.decimal_q8("0.667"), 171)
      self.assertEqual(ec.decimal_q8("1.5"), 384)
      self.assertEqual(ec.decimal_q8("0.5"), 128)
      # a header comment followed by a blank line keeps it
      self.assertEqual(ec.format(ec.parse("# head\n\nmove \"Soar\" {}\n")), "# head\n\nmove \"Soar\" {}\n")
      self.assertEqual(ec.format(ec.parse("# doc\nmove \"Soar\" {}\n")), "# doc\nmove \"Soar\" {}\n")

   def test_per_foe(self) -> None:
      """`on switch_in per_foe` round-trips and sets the handler's kHandlerPerFoe flag; other handlers have none."""
      text = ('passive "Crackle" {\n   on switch_in per_foe {\n      stage other atk -1 quiet;\n   }\n'
              '   on residual {}\n}\n')
      ast = ec.parse(text)
      self.assertIs(ast["owners"][0]["handlers"][0]["per_foe"], True)
      self.assertNotIn("per_foe", ast["owners"][0]["handlers"][1])
      self.assertEqual(ec.format(ast), text)
      unit = compile_text(text)
      self.assertTrue(unit.ok(), [str(d) for d in unit.diagnostics])
      bank = unit.bank()
      self.assertEqual([h[1] for h in bank.handlers], [ec.HANDLER_PER_FOE, 0])
      self.assertIn("switch_in per_foe -> program", ec.disassemble(bank))

   def test_json_normalizes(self) -> None:
      """from_json folds a negated literal and rejects other documents."""
      ast = {"schema": ec.SCHEMA_AST, "owners": [{"owner": {"kind": "move", "name": "Soar"}, "handlers": [
         {"phase": "use", "body": [{"stmt": "let", "name": "a", "value": {"op": "neg", "args": [3]}}]}]}]}
      self.assertEqual(ec.from_json(ast)["owners"][0]["handlers"][0]["body"][0]["value"], -3)
      with self.assertRaises(ec.EffectError):
         ec.from_json({"schema": "other/1"})
      with self.assertRaises(ec.EffectError):
         ec.from_json({"schema": ec.SCHEMA_AST})

   def test_syntax_errors(self) -> None:
      """Syntax errors carry their position."""
      cases = [('move "Soar" { use { let = 1; } }', "expected a local name"),
               ('move "Soar" { use { if 1 { } # c\n else { } } }', "expected a statement"),
               ('move "Soar" { use { chance 1 { } } }', "chance takes odds"),
               ('move "Soar" { use { repeat 3 { } } }', "repeat needs"),
               ('move "Soar" { use { x; } }', "expected a statement"),
               ('bogus "Soar" {}', "unknown owner kind"),
               ('move Soar {}', "owner's name as a string"),
               ('move "Soar" { use { text skip }', "expected ';'"),
               ('move "Soar" { use { let a = 1 + # c\n 2; } }', "unexpected"),
               ('move "Soar" { tags [a]; tags [b]; }', "tags are given twice"),
               ('move "Soar" { use { lock user free free; } }', "given twice"),
               ('move "Soar" { use { lock user no_pp; } }', "`free` now"),
               ('move "Soar" { use { let a = "\\q"; } }', "unknown escape"),
               ('move "Soar" { use { let a = 1 @ 2; } }', "unexpected character"),
               ('move "Soar" { bogus {} }', "unknown member")]
      for text, message in cases:
         with self.subTest(text=text):
            with self.assertRaises(ec.EffectError if "escape" not in message else ec.ScriptError) as cm:
               ec.parse(text)
            self.assertIn(message, str(cm.exception))
            self.assertGreater(cm.exception.line, 0)


def big_structure() -> dict[str, Any]:
   """The examples fixture's names plus the extras some diagnostics need.
   @return a structure
   """
   s = copy.deepcopy(EXAMPLES)
   s["items"].append("Soar")                          # Soar is a move and an item: ambiguous
   s["statuses"].extend(f"S{i}" for i in range(300))   # ids above 255
   s["tags"].extend(f"t{i}" for i in range(16))      # 17 tags
   del s["species_fields"]                           # a list the structure does not declare
   # a stored custom role and an unstored one (E13a: computed on read; C5: effect logic may not write it)
   s["roles"] = [{"name": "score", "id": 0x80}, {"name": "marked", "id": 0x81, "stored": False}]
   return s


## (code, source) per diagnostic; every MESSAGES code appears (checked below).
DIAGNOSTICS = [
   ("syntax", {"schema": ec.SCHEMA_AST, "owners": [{"owner": {"kind": "move", "name": "Soar"}, "handlers": [
      {"phase": "use", "body": [{"stmt": "bogus"}]}]}]}),
   ("duplicate-owner", 'move "Soar" {} move "Soar" {}'),
   ("unknown-owner", 'move "Nope" {}'),
   ("duplicate-sub", 'move "Soar" { sub a {} sub a {} }'),
   ("unknown-hook", 'passive "Crackle" { on nope {} }'),
   ("hook-owner", 'passive "Crackle" { on start {} }'),
   ("move-hook", 'move "Soar" { on residual {} }'),
   ("phase-owner", 'passive "Crackle" { hit {} }'),
   ("unknown-point", 'passive "Crackle" { point nope {} }'),
   ("unknown-sub", 'move "Soar" { use { call nope; } }'),
   ("recursion", 'move "Soar" { sub a { call b; } sub b { call a; } use { call a; } }'),
   ("call-depth", 'move "Soar" { sub a { call b; } sub b { call c; } sub c { call d; } sub d { call e; } sub e {} '
                  'use { call a; } }'),
   ("unknown-name", 'move "Soar" { use { let x = Nonexistent; } }'),
   ("ambiguous-name", 'move "Soar" { use { let x = Soar; } }'),
   ("unknown-id", 'move "Soar" { use { inflict target Nope; } }'),
   ("no-list", 'move "Soar" { use { let x = user.species_field(weight); } }'),
   ("id-range", 'move "Soar" { use { inflict target S299; } }'),
   ("unknown-ref", 'move "Soar" { use { let x = user.nope; } }'),
   ("ref-arg", 'move "Soar" { use { let x = user.counter; } }'),
   ("unknown-subject", 'move "Soar" { use { heal nobody 1; } }'),
   ("not-writable", 'move "Soar" { use { user.hp = 1; } }'),
   ("unstored-role", 'move "Soar" { use { user.role(marked) = 1; } }'),
   ("none-kind", 'move "Soar" { use { let x = none; } }'),
   ("unknown-function", 'move "Soar" { use { let x = nope(1); } }'),
   ("arity", 'move "Soar" { use { let x = min(1); } }'),
   ("redeclared", 'move "Soar" { use { let x = 1; let x = 2; } }'),
   ("reserved", 'move "Soar" { use { let holder = 1; } }'),
   ("undeclared", 'move "Soar" { use { y = 1; } }'),
   ("too-many-locals", 'move "Soar" { use { ' + " ".join(f"let a{i} = {i};" for i in range(9)) + ' } }'),
   ("stack-depth", 'move "Soar" { use { let x = ' + "1 + (" * 16 + "1" + ")" * 16 + '; } }'),
   ("repeat-bound", 'move "Soar" { use { repeat 1 max 17 {} } }'),
   ("yield-context", 'passive "Crackle" { on residual { yield; } }'),
   ("damage-context", 'passive "Crackle" { on residual { damage; } }'),
   ("target-context", 'move "Soar" { use { set_target user; } }'),
   ("no-value", 'passive "Crackle" { on residual { modify set 1; } }'),
   ("per-foe", 'passive "Crackle" { on residual per_foe { } }'),
   ("no-result", 'passive "Crackle" { on residual { block; } }'),
   ("pure", 'passive "Quick Paddle" { point speed { text skip; } }'),
   # G1: ai_score handlers are pure; absorb only marks a hit a before_damage handler takes
   ("pure-hook", 'passive "Quick Paddle" { on ai_score { text skip; } }'),
   ("absorb-context", 'passive "Quick Paddle" { on damaging_hit { absorb; } }'),
   ("recruit-context", 'passive "Quick Paddle" { on damaging_hit { recruit other 1; } }'),
   # F1: the encounter primitives belong to the encounter hooks, a re-pick to encounter_made, operands in range
   ("encounter-context", 'passive "Quick Paddle" { on switch_in { encounter_rate 1 2; } }'),
   # F3: the step effect read outside a steps handler
   ("steps-context", 'passive "Quick Paddle" { on switch_in { if field.step_effect == 0 { fail; } } }'),
   ("reroll-context", 'passive "Quick Paddle" { on encounter { encounter_reroll; } }'),
   ("encounter-arg", 'passive "Quick Paddle" { on encounter { encounter_rate 300 1; } }'),
   ("field-effect-amount", 'passive "Quick Paddle" { on encounter { field_effect start ward 0; } }'),   # C8
   ("encounter-target", 'passive "Quick Paddle" { on encounter { target.role(score) = 1; } }'),   # C8
   # B1: the combine hook's reads, subjects and primitives outside it; an input that is not one; an unstored role
   ("combine-context", 'passive "Quick Paddle" { on switch_in { inherit score from input.0; } }'),
   ("unknown-input", 'global "g" { on combine { let x = input.nobody.level; } }'),
   ("inherit-role", 'global "g" { on combine { inherit marked from input.0; } }'),
   ("point-subject", 'passive "Quick Paddle" { point speed { let a = 1; modify set a.hp; } }'),
   ("hook-budget", 'passive "Crackle" { on residual { ' + "text skip; " * 33 + '} }'),
   ("step-budget", 'move "Soar" { use { ' + "text skip; " * 129 + '} }'),
   ("too-many-types", 'move "Soar" { use { set_types user Ember, Tide, Leaf, Stone; } }'),
   ("too-many-tags", 'passive "Crackle" { tags [' + ", ".join(f"t{i}" for i in range(16)) + ', hovering]; }'),
   ("unreachable", 'move "Soar" { use { fail; text skip; } }'),
   ("saturation", 'move "Soar" { use { let x = user.max_hp * user.max_hp * user.max_hp; } }'),
   ("program-size", 'move "Soar" { use { ' + "user.counter(daze) = 0; " * 11000 + '} }'),
   ("odds-range", 'move "Soar" { use { if odds(1, 70000) { fail; } } }'),
]


class DiagnosticTests(unittest.TestCase):
   """Every diagnostic code, with its position."""

   def test_every_code(self) -> None:
      """Each case reports its code; every MESSAGES code has a case."""
      structure = big_structure()
      for code, source in DIAGNOSTICS:
         with self.subTest(code=code):
            if isinstance(source, dict):
               diags = ec.check(source, structure)
            else:
               diags = ec.Unit([("t.ceffect", source)], structure).diagnostics
            self.assertIn(code, [d.code for d in diags], [str(d) for d in diags])
            d = next(d for d in diags if d.code == code)
            self.assertEqual(d.severity, "warning" if code in ("unreachable", "saturation") else "error")
            if isinstance(source, str) and code not in ("too-many-locals", "stack-depth", "hook-budget",
                                                        "step-budget", "program-size"):
               self.assertGreater(d.line, 0, str(d))
      self.assertEqual({c for c, _s in DIAGNOSTICS}, set(ec.MESSAGES))

   def test_unstored_role_reads(self) -> None:
      """An unstored role is read like any role; a stored one is written (C5: only the write of an unstored one is
      refused, the engine's role_set would drop it)."""
      unit = ec.Unit([("t.ceffect", 'move "Soar" { use { let m = user.role(marked); user.role(score) = m + 1; } }')],
                     big_structure())
      self.assertTrue(unit.ok(), [str(d) for d in unit.diagnostics])

   def test_none_word(self) -> None:
      """C5: `none` is the none value of the kind it is compared with (a move: 65535, a type: 255); it is not a local's
      name."""
      unit = ec.Unit([("t.ceffect", 'move "Soar" { use { if user.targeted_by == none or user.type(0) != none '
                                    '{ fail; } } }')], big_structure())
      self.assertTrue(unit.ok(), [str(d) for d in unit.diagnostics])
      bad = ec.Unit([("t.ceffect", 'move "Soar" { use { let none = 1; } }')], big_structure())
      self.assertIn("reserved", [d.code for d in bad.diagnostics])
      self.assertEqual((ec.NONE_VALUES["move"], ec.NONE_VALUES["type"]), (ec.U16_MAX, ec.U8_MAX))

   def test_try_switch_per_foe(self) -> None:
      """C5: try_switch takes the per_foe mark (a foe's trap: kHandlerPerFoe); other hooks but switch_in do not."""
      unit = ec.Unit([("t.ceffect", 'passive "Crackle" { on try_switch per_foe { block; } }')], big_structure())
      self.assertTrue(unit.ok(), [str(d) for d in unit.diagnostics])
      self.assertIn("try_switch", ec.PER_FOE_HOOKS)
      self.assertIn(ec.PER_FOE_HOOK, ec.PER_FOE_HOOKS)

   def test_fixtures_are_clean(self) -> None:
      """Both fixtures compile without errors or warnings."""
      for manifest in ("examples.json", "vm.json"):
         unit = ec.load_unit(DATA / manifest)
         self.assertEqual([str(d) for d in unit.diagnostics], [])

   def test_condition_texts_and_turns(self) -> None:
      """A condition's own texts resolve before the structure's (a status text is a kConditionText id); a condition
      statement without turns gives the condition's own; a file a manifest lists must exist (E5)."""
      structure = {**big_structure(), "statuses": [{"name": "Scorch", "texts": {"skip": 0x8002}}],
                   "weathers": [{"name": "Drizzle", "turns": 5}]}
      unit = ec.Unit([("t.ceffect", 'status "Scorch" { on before_move { text skip; stop; } }\n'
                                    'move "Soar" { use { weather Drizzle; } }\n')], structure)
      self.assertTrue(unit.ok(), [str(d) for d in unit.diagnostics])
      listing = ec.disassemble(unit.bank())
      self.assertIn("Text  32770", listing)
      self.assertIn("Push8  5", listing)
      with tempfile.TemporaryDirectory() as tmp:
         manifest = Path(tmp) / "effects.json"
         manifest.write_text(json.dumps({"schema": ec.SCHEMA_MANIFEST, "sources": ["gone.ceffect"]}),
                             encoding="utf-8")
         with self.assertRaisesRegex(ec.EffectError, "gone.ceffect does not exist"):
            ec.load_unit(manifest)

   def test_names_by_kind(self) -> None:
      """A name resolves in the kind of the value it is compared with (category status vs. a status)."""
      unit = compile_text('passive "Crackle" { on try_hit when move.category == status {} '
                          'on residual when holder.status == Scorch {} }')
      self.assertTrue(unit.ok(), [str(d) for d in unit.diagnostics])
      unit = compile_text('passive "Crackle" { on residual when holder.status == physical {} }')
      self.assertIn("unknown-id", codes(unit))

   def test_suspension_through_subs(self) -> None:
      """A sub that yields may be called from a top-level phase only."""
      unit = compile_text('move "Soar" { sub wait { yield; } use { call wait; } }')
      self.assertTrue(unit.ok(), [str(d) for d in unit.diagnostics])
      unit = compile_text('status "Doze" { sub wait { yield; } on before_move { call wait; } on residual { call wait; '
                          'ask choice holder into k; } }')
      self.assertEqual(codes(unit), ["yield-context"])
      unit = compile_text('passive "Crackle" { on residual { switch holder pass; } }')
      self.assertEqual(codes(unit), ["yield-context"])
      unit = compile_text('passive "Crackle" { on residual { switch holder random; } }')
      self.assertEqual(codes(unit), [])


class BudgetTests(unittest.TestCase):
   """Event budgets (with the hooks primitives trigger) and termination."""

   def events(self, unit: ec.Unit, label: str) -> int:
      """Worst-case events of a program.
      @param unit   compiled unit
      @param label  program label
      @return events
      """
      assert unit.compiler is not None
      return next(p.events for p in unit.compiler.programs if p.label == label)

   def test_triggered_hooks_count(self) -> None:
      """Inflict counts the status's own start hook and every set_status hook one level deeper."""
      base = 'status "Scorch" { on start { ' + "text skip; " * 20 + '} }\n'
      unit = compile_text(base + 'passive "Crackle" { on residual { inflict holder Scorch; } }')
      self.assertTrue(unit.ok())
      self.assertEqual(self.events(unit, 'passive "Crackle" on residual'), 2 + 20)
      unit = compile_text(base + 'item "Berrypack" { on set_status { text skip; text skip; } }\n'
                          'passive "Crackle" { on residual { inflict holder Scorch; } }')
      self.assertEqual(self.events(unit, 'passive "Crackle" on residual'), 2 + 20 + 2)
      unit = compile_text(base.replace("text skip; " * 20, "text skip; " * 31) +
                          'passive "Crackle" { on residual { inflict holder Scorch; } }')
      self.assertIn("hook-budget", codes(unit))

   def test_kind_fan(self) -> None:
      """One passive at a time (the worst), but every volatile (the sum)."""
      text = ('passive "Crackle" { on update { text skip; } }\n'
              'passive "Hover" { on update { text skip; text skip; } }\n'
              'volatile "guarded" { on update { text skip; } }\nvolatile "aloft" { on update { text skip; } }\n'
              'item "Berrypack" { on residual { heal holder 1; } }')
      unit = compile_text(text)
      self.assertEqual(self.events(unit, 'item "Berrypack" on residual'), 1 + 2 + 1 + 1)

   def test_depth_limit_ends_costs(self) -> None:
      """A status that inflicts itself on start: the cost stops at kMaxEffectDepth."""
      unit = compile_text('status "Scorch" { on start { text skip; inflict holder Scorch; } }\n'
                          'passive "Crackle" { on residual { inflict holder Scorch; } }')
      self.assertTrue(unit.ok(), [str(d) for d in unit.diagnostics])
      per_level = 1 + 2   # text + inflict
      self.assertEqual(self.events(unit, 'passive "Crackle" on residual'), 2 + per_level * (ec.MAX_EFFECT_DEPTH - 1))

   def test_segments(self) -> None:
      """Top-level phases are budgeted per segment between yields (multi-hit damage yields after each hit)."""
      unit = compile_text('move "Soar" { use { ' + "text skip; " * 100 + "yield; " + "text skip; " * 100 + '} }')
      self.assertTrue(unit.ok())
      self.assertEqual(self.events(unit, 'move "Soar" use'), 100)
      unit = compile_text('move "Soar" { hit { repeat 5 max 16 { damage; text skip; } } }')
      self.assertTrue(unit.ok())
      multi = self.events(unit, 'move "Soar" hit')
      single = self.events(compile_text('move "Soar" { hit { damage; text skip; } }'), 'move "Soar" hit')
      self.assertEqual(multi, single)   # each hit is its own step
      self.assertTrue(unit.bank().programs[0][4] & ec.PROGRAM_YIELDS)

   def test_loops_are_bounded(self) -> None:
      """repeat counts its max; for counts kMaxPositions; both clamp at run time."""
      unit = compile_text('passive "Crackle" { on residual { repeat holder.hp max 7 { text skip; } } }')
      self.assertEqual(self.events(unit, 'passive "Crackle" on residual'), 7)
      unit = compile_text('passive "Crackle" { on residual { for f in foes(holder) { text skip; } } }')
      self.assertEqual(self.events(unit, 'passive "Crackle" on residual'), ec.MAX_POSITIONS)
      unit = compile_text('passive "Crackle" { on residual { repeat 3 max 0 {} } }')
      self.assertIn("repeat-bound", codes(unit))

   def test_jumps_are_forward(self) -> None:
      """Every jump of every compiled program goes forward; only Loop / Next go back."""
      for manifest in ("examples.json", "vm.json"):
         bank = ec.load_unit(DATA / manifest).bank()
         for off, length, *_rest in bank.programs:
            for at, name, values in ec.instructions(bank.code[off:off + length]):
               kinds = ec.OPS[ec.OP[name]][1]
               if "off" not in kinds:
                  continue
               target = values[kinds.index("off")]
               if name in ("Loop", "Next"):
                  self.assertLessEqual(target, at)
               else:
                  self.assertGreater(target, at)
                  self.assertLess(target, length)


class BytecodeTests(unittest.TestCase):
   """The bank: golden listing, opcode coverage, program info, determinism."""

   def test_golden_listing(self) -> None:
      """The language examples' bytecode."""
      text = ec.disassemble(ec.load_unit(DATA / "examples.json").bank())
      self.assertEqual(text, golden("examples.disasm", text))

   def test_odds(self) -> None:
      """!odds(n, m[, by, on]) (E9): n, m and two battlers (user and target by default), then Odds; it draws."""
      unit = compile_text('global "g" { on before_damage { modify set odds(1, 2) + odds(1, 2, holder, other); } }')
      self.assertEqual(codes(unit), [])
      names = [name for _at, name, _v in ec.instructions(unit.bank().code)]
      odds = [i for i, name in enumerate(names) if name == "Odds"]
      self.assertEqual(len(odds), 2)
      self.assertTrue(all(names[i - 2:i] == ["Subj", "Subj"] for i in odds))
      self.assertIn("arity", codes(compile_text('global "g" { on before_damage { modify set odds(1); } }')))
      self.assertIn("pure", codes(compile_text('passive "Float" { point power { modify by odds(1, 2); } }')))

   def test_opcode_coverage(self) -> None:
      """The neutral fixture uses every opcode the compiler emits."""
      bank = ec.load_unit(DATA / "vm.json").bank()
      used = {name for _at, name, _v in ec.instructions(bank.code)}
      self.assertEqual({name for name, *_r in ec.OPS} - used, ec.UNEMITTED_OPS)

   def test_program_info(self) -> None:
      """Stack, locals and flags of a few programs."""
      unit = ec.load_unit(DATA / "examples.json")
      bank = unit.bank()
      info = dict(zip(bank.labels, bank.programs))
      _off, _len, stack, locals_, flags, _events = info['move "Guard" use']
      self.assertEqual((stack, locals_), (5, 1))
      self.assertTrue(flags & ec.PROGRAM_RNG)
      self.assertFalse(flags & ec.PROGRAM_YIELDS)
      self.assertTrue(info['move "Relay" use'][4] & ec.PROGRAM_ASKS)
      self.assertEqual(info['passive "Glower" on switch_in'][3], 2)   # loop variable + mask
      for off, length, stack, *_r in bank.programs:
         self.assertLessEqual(stack, ec.EFFECT_STACK)
         self.assertEqual(ec.prove_stack(bank.code[off:off + length], {}), stack)

   def test_sub_locals_do_not_overlap(self) -> None:
      """A sub's locals start above every caller's."""
      unit = compile_text('move "Soar" { sub s { let t = 1; credit t; } use { let a = 1; let b = 2; call s; '
                          'credit a + b; } }')
      bank = unit.bank()
      labels = dict(zip(bank.labels, bank.programs))
      off, length, *_r = labels['move "Soar" sub s']
      stores = [v for _a, n, v in ec.instructions(bank.code[off:off + length]) if n == "Store"]
      self.assertEqual(stores, [[2]])
      self.assertEqual(labels['move "Soar" use'][3], 3)

   def test_emit_is_deterministic(self) -> None:
      """Two compiles write the same C++; it names every table."""
      first = ec.emit_cpp(ec.load_unit(DATA / "vm.json").bank(), "ns", "vm.json")
      second = ec.emit_cpp(ec.load_unit(DATA / "vm.json").bank(), "ns", "vm.json")
      self.assertEqual(first, second)
      for name in ("kCode", "kPrograms", "kHandlers", "kOwners", "kOwnerNames", "kBank", "point_handler"):
         self.assertIn(name, first)
      self.assertIn("static_assert(effect::kOpCount == ", first)


class BattleFormTests(unittest.TestCase):
   """The forms the battle integration added (E4b): ruleset points, say at / about / value, stage quiet."""

   def test_structure_points(self) -> None:
      """A structure's "points" replace the default catalog (ids are their order, ranges their values')."""
      structure = dict(VM, points=[{"name": "reach", "range": [0, 9]}, "power"])
      unit = compile_text('passive "Wall" {\n   point power {\n      modify by 2;\n   }\n}\n', structure)
      self.assertTrue(unit.ok(), unit.diagnostics)
      bank = unit.bank()
      self.assertEqual(bank.owners[0][1], 1 << 1)              # "power" is point 1 here
      self.assertIn("point power -> fn 0", ec.disassemble(bank))
      self.assertIn("unknown-point", codes(compile_text('passive "Wall" { point speed { modify by 2; } }', structure)))
      self.assertEqual(codes(compile_text('passive "Wall" { point speed { modify by 2; } }', VM)), [])

   def test_say_at_and_quiet_stage(self) -> None:
      """`say MSG at S about S value E` is SayAt (value, two subjects); `stage ... quiet` sets kStageQuiet."""
      text = ('move "Everything" {\n   use {\n      say generic at target about user value 7;\n'
              '      stage user def 1 quiet;\n      say generic;\n   }\n}\n')
      ast = ec.parse(text)
      self.assertEqual(ec.format(ast), text)
      body = ast["owners"][0]["handlers"][0]["body"]
      self.assertEqual(body[0], {"stmt": "say", "msg": "generic", "at": "target", "about": "user", "value": 7})
      self.assertTrue(body[1]["quiet"])
      unit = compile_text(text, VM)
      self.assertTrue(unit.ok(), unit.diagnostics)
      ops = [(n, v) for _at, n, v in ec.instructions(unit.bank().code)]
      self.assertIn(("SayAt", [0, ec.SUBJ_TARGET, ec.SUBJ_USER]), ops)
      self.assertIn(("Say", [0]), ops)
      stage = VM["stages"].index("def")
      self.assertIn(("Stage", [ec.SUBJ_USER, stage | ec.STAGE_QUIET]), ops)

   def test_new_refs(self) -> None:
      """`acted` and `locked` are host reads after the VM's own refs (C5: player_side; F1: the field effect refs
      appended last)."""
      self.assertEqual(ec.REFS[-14:], ["acted", "locked", "resource", "resource_max", "move_cost", "targeted_by",
                                       "delayed_turns", "human", "unowned", "gene", "player_side", "absorbed",
                                       "field_effect_active", "field_effect_remaining"])
      unit = compile_text('move "Everything" { use { if user.acted or user.locked { fail; } } }', VM)
      self.assertTrue(unit.ok(), unit.diagnostics)


def mock_read(k: int) -> Any:
   """The read the cross-check uses (the generated harness has the same formula).
   @param k  case number
   @return read(kind, index, subj, arg)
   """
   def read(kind: str, index: int, subj: int, arg: int) -> int:
      """A deterministic answer.
      @param kind   'get' or 'has'
      @param index  Ref or HasKind index
      @param subj   subj8
      @param arg    argument
      @return the value
      """
      if kind == "has":
         return (index + subj + arg + k) % 2
      return (index * 131 + subj * 17 + arg * 5 + k * 29) % 300 - 50
   return read


## The cross-check harness's formula and loop (C++).
HARNESS = """
#include "capsule/battle/effect.h"
#include <cstdio>
namespace capsule::battle::effect
{
struct PointCtx
{
   int k;
};
int32_t point_get(PointCtx& c, Ref ref, uint8_t subj, uint8_t arg)
{
   return (static_cast<int32_t>(ref) * 131 + subj * 17 + arg * 5 + c.k * 29) % 300 - 50;
}
bool point_has(PointCtx& c, HasKind kind, uint8_t subj, uint8_t id)
{
   return (static_cast<int>(kind) + subj + id + c.k) % 2 == 1;
}
}
namespace gen
{
using namespace capsule::battle;
@BODY@
}
int main()
{
   const int32_t values[] = {0, 1, 7, 100, 255, 1000, 65535, -9, 2147483647, -2147483647 - 1};
   for (int fn = 0; fn < @COUNT@; ++fn)
   {
      for (int k = 0; k < 40; ++k)
      {
         for (int32_t v : values)
         {
            capsule::battle::effect::PointCtx c{k};
            std::printf("%d %d %d %d\\n", fn, k, v, gen::point_handler(static_cast<uint16_t>(fn), c, v));
         }
      }
   }
}
"""


class PrimitiveTests(unittest.TestCase):
   """E13b: silent stages, reset_stages, delay with the land phase, reward, end_battle, the try_switch / redirect
   hooks, the new battler refs, typed damage_hp counting before_damage."""

   def ops(self, unit: ec.Unit) -> list[tuple[str, list[int]]]:
      """Every instruction of a unit's bank.
      @param unit  compiled unit
      @return (name, operands) in order
      """
      return [(name, values) for _at, name, values in ec.instructions(unit.bank().code)]

   def test_primitives(self) -> None:
      """The new primitives compile to their ops; `silent` is a stage id bit; round trips keep them."""
      text = ('move "Everything" {\n   use {\n      stage user def -1 silent;\n      reset_stages target;\n'
              '      delay target 40 2;\n      reward user.level * 5;\n      end_battle;\n   }\n'
              '   land {\n      damage_hp target value typed;\n   }\n}\n')
      unit = compile_text(text, VM)
      self.assertEqual(codes(unit), [])
      ops = self.ops(unit)
      stage = VM["stages"].index("def")
      self.assertIn(("Stage", [ec.SUBJ_USER, stage | ec.STAGE_SILENT]), ops)
      self.assertIn(("ResetStages", [ec.SUBJ_TARGET]), ops)
      self.assertIn(("Delay", [ec.SUBJ_TARGET]), ops)
      self.assertIn(("Reward", []), ops)
      self.assertIn(("EndBattle", []), ops)
      self.assertEqual(ec.format(ec.parse(text)), text)
      self.assertEqual(ec.MAX_STAGE_ID, ec.STAGE_SILENT - 1)

   def test_land_phase(self) -> None:
      """land is a move phase (Hook land), a hook program: it may not yield."""
      unit = compile_text('move "Everything" { land { damage_hp target value typed; } }', VM)
      self.assertEqual(codes(unit), [])
      self.assertIn('move "Everything" land', unit.bank().labels)
      unit = compile_text('move "Everything" { land { yield; } }', VM)
      self.assertIn("yield-context", codes(unit))
      self.assertIn("move-hook", codes(compile_text('move "Everything" { on land { } }', VM)))

   def test_hooks(self) -> None:
      """try_switch blocks; redirect has a battler value modify can set; the new refs read."""
      first = ec.HOOK["try_switch"]
      self.assertEqual([h["name"] for h in ec.HOOKS[first:first + 3]], ["try_switch", "redirect", "land"])
      unit = compile_text('volatile "Shield" {\n   on try_switch when holder.unowned or holder.human { block; }\n'
                          '   on redirect when is_foe(holder, other) { modify set holder; }\n'
                          '   on before_move when holder.targeted_by != 65535 and holder.delayed_turns > 0 { stop; }\n'
                          '}\n', VM)
      self.assertEqual(codes(unit), [])
      self.assertIn("no-result", codes(compile_text('volatile "Shield" { on try_switch { immune; } }', VM)))

   def test_typed_damage_counts_before_damage(self) -> None:
      """A typed damage_hp counts the before_damage hooks it runs; an untyped one does not."""
      hooks = 'volatile "Shield" { on before_damage { text a; text b; text done; } }\n'
      typed = compile_text(hooks + 'move "Aimer" { hit { damage_hp target 5 typed; } }', VM)
      plain = compile_text(hooks + 'move "Aimer" { hit { damage_hp target 5; } }', VM)
      events = {u: next(p.events for p in u.compiler.programs if p.label == 'move "Aimer" hit') for u in (typed, plain)}
      self.assertEqual(events[typed], events[plain] + 3)

   def test_reset_stages_cost(self) -> None:
      """reset_stages counts a Stage event per stage of the structure."""
      unit = compile_text('passive "Wall" { on residual { reset_stages holder; } }', VM)
      self.assertEqual(next(p.events for p in unit.compiler.programs if p.label == 'passive "Wall" on residual'),
                       len(VM["stages"]))


class PureTests(unittest.TestCase):
   """Point handlers: the pure subset, evaluate_pure, and the generated C++ agreeing with it."""

   def points(self) -> list[tuple[dict[str, Any], dict[str, Any], ec.Catalog]]:
      """Every point handler of both fixtures, plus extra pure code.
      @return (point entry, handler node, catalog)
      """
      extra = ('passive "Crackle" { point damage { let a = value; a += 3; if a > 100 { modify set a * 2; return; } '
               'else { modify add -a; } modify by pick(holder.stage(atk), 0.5, 1, 1.5); } '
               'point speed when holder.has(daze) or not move.has(loud) { modify set clamp(value / 0, -5, 5) '
               '+ (value % 7) + (value << 3) + (value >> 2) + abs(value - 70000) + max(value, holder.level) + '
               'scale(value, 0.75) + min(user.hp, 3) - -value + (user == holder) + is_ally(user, target); } }')
      out = []
      for name in ("examples.json", "vm.json"):
         unit = ec.load_unit(DATA / name)
         assert unit.compiler is not None
         out.extend((pt, pt["node"], unit.catalog) for pt in unit.compiler.points)
      unit = compile_text(extra)
      self.assertTrue(unit.ok(), [str(d) for d in unit.diagnostics])
      assert unit.compiler is not None
      out.extend((pt, pt["node"], unit.catalog) for pt in unit.compiler.points)
      return out

   def test_evaluate_pure(self) -> None:
      """Hand-checked values."""
      catalog = ec.Catalog(EXAMPLES)
      ast = ec.parse((DATA / "examples.ceffect").read_text(encoding="utf-8"))
      barrier = ec.owner_ast(ast, "side_condition:Barrier")["handlers"][-1]
      side_active = ec.REFS.index("side_active")
      category = ec.REFS.index("move_category")

      def read(kind: str, index: int, subj: int, arg: int) -> int:
         """Physical, two active, no crit, and the target is the holder.
         @param kind   'get' or 'has'
         @param index  Ref / HasKind
         @param subj   subj8
         @param arg    argument
         @return the value
         """
         if kind == "has":
            return 0
         if index == side_active:
            return 2
         if index == category:
            return 0
         if index == ec.REFS.index("battler"):
            return 0
         return 0

      self.assertEqual(ec.evaluate_pure(barrier, catalog, 100, read), ec.mul_q8(100, 192))   # 0.75
      swift = ec.owner_ast(ast, "Quick Paddle")["handlers"][0]
      self.assertEqual(ec.evaluate_pure(swift, catalog, 80, read), 104)   # x1.3 (333 in Q8)
      with self.assertRaises(ec.EffectError):
         ec.evaluate_pure({"point": "speed", "body": [{"stmt": "text", "text": "skip"}]}, catalog, 1, read)

   @unittest.skipUnless(shutil.which("c++"), "needs a C++ compiler")
   def test_cpp_agrees(self) -> None:
      """The generated C++ gives evaluate_pure's results on a grid (values, reads, every handler)."""
      entries = self.points()
      body = ec.emit_pure_cpp([pt for pt, _n, _c in entries])
      with tempfile.TemporaryDirectory() as tmp:
         src = Path(tmp) / "harness.cpp"
         exe = Path(tmp) / "harness"
         src.write_text(HARNESS.replace("@BODY@", body).replace("@COUNT@", str(len(entries))), encoding="utf-8")
         build = subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                                 str(src), "-o", str(exe)], capture_output=True, text=True)
         self.assertEqual(build.returncode, 0, build.stderr[-3000:])
         run = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
      lines = run.stdout.split()
      self.assertGreater(len(lines), 0)
      for i in range(0, len(lines), 4):
         fn, k, v, got = (int(x) for x in lines[i:i + 4])
         _pt, node, catalog = entries[fn]
         want = ec.evaluate_pure(node, catalog, v, mock_read(k))
         self.assertEqual(got, want, f"{entries[fn][0]['label']} k={k} v={v}")


class CommandLineTests(unittest.TestCase):
   """effect_compiler.py FILE --check | --format | --ast | --emit | --disasm."""

   def run_main(self, *args: str) -> tuple[int, str, str]:
      """Runs main().
      @param args  arguments
      @return (status, stdout, stderr)
      """
      out, err = io.StringIO(), io.StringIO()
      with redirect_stdout(out), redirect_stderr(err):
         status = ec.main(list(args))
      return status, out.getvalue(), err.getvalue()

   def test_modes(self) -> None:
      """Each mode on the fixtures."""
      examples = str(DATA / "examples.json")
      self.assertEqual(self.run_main(examples, "--check")[0], 0)
      status, out, _err = self.run_main(str(DATA / "examples.ceffect"), "--format")
      self.assertEqual((status, out), (0, (GOLDEN / "examples.canonical.ceffect").read_text(encoding="utf-8")))
      status, out, _err = self.run_main(examples, "--ast", "status:Chill")
      self.assertEqual(status, 0)
      self.assertEqual(json.loads(out)["owner"], {"kind": "status", "name": "Chill"})
      self.assertEqual(self.run_main(examples, "--ast", "Caltrops")[0], 1)   # a move and a side condition
      status, out, _err = self.run_main(examples, "--disasm")
      self.assertEqual(out, (GOLDEN / "examples.disasm").read_text(encoding="utf-8"))
      with tempfile.TemporaryDirectory() as tmp:
         target = Path(tmp) / "out" / "bank.cpp"
         self.assertEqual(self.run_main(examples, "--emit", str(target), "--namespace", "x")[0], 0)
         self.assertIn("namespace x", target.read_text(encoding="utf-8"))
         bad = Path(tmp) / "bad.ceffect"
         bad.write_text('move "Soar" { use { inflict target Nope; } }', encoding="utf-8")
         status, _out, err = self.run_main(str(bad), "--check", "--structure", str(DATA / "examples_structure.json"))
         self.assertEqual(status, 1)
         self.assertIn("bad.ceffect:1:21: error:", err)   # column after `move "Soar" { use { inflict target `
         bad.write_text('move "Soar" { use {', encoding="utf-8")
         self.assertEqual(self.run_main(str(bad), "--format")[0], 1)
         self.assertEqual(self.run_main(str(Path(tmp) / "missing.ceffect"), "--check")[0], 1)


class BattleGapsTests(unittest.TestCase):
   """G1 (D-26 section 1): call exclusions, delayed hits, the decoy mark, the AI's pure ai_score hook."""

   def test_use_move_unless(self) -> None:
      """`use_move WHO MOVE unless FLAG` round-trips and emits CallMove with the flag; without it UseMove stays."""
      text = 'move "Everything" {\n   use {\n      use_move user Multi unless loud;\n   }\n}\n'
      self.assertEqual(ec.format(ec.parse(text)), text)
      self.assertEqual(ec.parse(text)["owners"][0]["handlers"][0]["body"][0]["unless"], "loud")
      unit = compile_text(text, VM)
      self.assertTrue(unit.ok(), unit.diagnostics)
      ops = [(n, v) for _at, n, v in ec.instructions(unit.bank().code)]
      self.assertIn(("CallMove", [ec.SUBJ_USER, VM["move_flags"].index("loud")]), ops)
      self.assertNotIn("Drop", [n for n, _v in ops])   # CallMove pushes nothing
      self.assertIn("unknown-id", codes(compile_text(text.replace("loud", "nope"), VM)))

   def test_delay_hit(self) -> None:
      """delay_hit names a delayed hit; the land phase may `damage` (and only a hit or land phase may)."""
      text = ('move "Everything" {\n   hit {\n      delay_hit target 2 foresight;\n   }\n'
              '   land {\n      damage;\n   }\n}\n')
      self.assertEqual(ec.format(ec.parse(text)), text)
      unit = compile_text(text, VM)
      self.assertTrue(unit.ok(), unit.diagnostics)
      ops = [(n, v) for _at, n, v in ec.instructions(unit.bank().code)]
      self.assertIn(("DelayHit", [ec.SUBJ_TARGET, 0]), ops)
      self.assertIn("unknown-id", codes(compile_text(text.replace("foresight", "hindsight"), VM)))
      self.assertIn("damage-context", codes(compile_text('move "Everything" { use { damage; } }', VM)))

   def test_absorb(self) -> None:
      """absorb marks a hit in before_damage; `absorbed` reads it anywhere."""
      unit = compile_text('volatile "Shield" { on before_damage { absorb; modify set 0; } '
                          'on damaging_hit when absorbed { stop; } }', VM)
      self.assertTrue(unit.ok(), unit.diagnostics)
      names = [n for _at, n, _v in ec.instructions(unit.bank().code)]
      self.assertIn("Absorb", names)
      self.assertIn("absorb-context", codes(compile_text('volatile "Shield" { on damaging_hit { absorb; } }', VM)))

   def test_ai_score_is_pure(self) -> None:
      """ai_score handlers read, modify and block; they never write, draw, act or call."""
      ok = compile_text('passive "Wall" { on ai_score when move.power > 50 and target.hp < 10 { modify add 30; } '
                        'on ai_score when move == none { block; } }\nmove "Everything" { ai_score { modify set 0; } }',
                        VM)
      self.assertTrue(ok.ok(), ok.diagnostics)
      self.assertIn('move "Everything" ai_score', ok.bank().labels)
      for body in ("heal holder 1;", "holder.counter(cnt_a) = 1;", "modify add rand(3);", "chance 1/2 { block; }",
                   "text hello;"):
         with self.subTest(body=body):
            self.assertIn("pure-hook", codes(compile_text(f'passive "Wall" {{ on ai_score {{ {body} }} }}', VM)))
      self.assertIn("pure-hook", codes(compile_text('passive "Wall" { sub s { } on ai_score { call s; } }', VM)))


if __name__ == "__main__":
   unittest.main()
