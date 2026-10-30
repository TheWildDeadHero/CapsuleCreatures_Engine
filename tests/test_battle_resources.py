#!/usr/bin/env python3
"""!Tests for manifest resources (D-25, E8): tools/battlec/resources.py through the battle compiler (the classic
sample's hit points and PP, the neutral fixture tests/data/battle_resources, refusals and the retired PP keys),
the bench scenario's resource keys (tools/bench_scenario.py) and the migration helper tools/battle_resources.py
(run by CTest, or directly)."""
from __future__ import annotations

import contextlib
import io
import json
import re
import shutil
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler as bc  # noqa: E402
import battle_resources as migrate  # noqa: E402
import battle_split as split  # noqa: E402
import bench_scenario as bs  # noqa: E402
import effect_compiler as ec  # noqa: E402
import ruleset_compiler as rc  # noqa: E402
from battlec import resources as bc_resources  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## A PP-shaped neutral data set (hit points and a move resource pp with raises and a max formula; the classic sample
## played this part before E11 and lives in the reference repo now).
SAMPLE = ROOT / "tests/data/battle_stats3/battle.json"
## The neutral resources fixture.
FIXTURE = ROOT / "tests/data/battle_resources/battle.json"
## The progression fixture (a move resource pp with the ruleset's own pp_max and raises; review2 rv/t7.py).
PROGRESS = ROOT / "tests/data/battle_progress/battle.json"


def compile_with(path: Path, edit: Callable[[JsonDict], None]) -> JsonDict:
   """!Compiles a battle after an edit of its merged view, from memory.
   @param path  battle.json
   @param edit  callable(merged view)
   @return compiled
   """
   data = bc.view_of(path)
   edit(data)
   return bc.compile_battle(path, None, bc.sources_of(path, data))


def resource(data: JsonDict, name: str) -> JsonDict:
   """!A resource entry of a merged view.
   @param data  the view
   @param name  its name
   @return the entry
   """
   return next(r for r in data["resources"] if r["name"] == name)


class SampleTests(unittest.TestCase):
   """!A PP-shaped data set declares its hit points and PP as resources (D-25 decision 7)."""

   def test_sample(self) -> None:
      """!hp (faint, current_hp) and pp (move, base pp, max pp_max, raises pp_ups, persisted); cost 1 PP each."""
      c = bc.compile_battle(SAMPLE)
      hp, pp = c["resources"]
      self.assertEqual((hp["name"], hp["scope_name"], bc.EMPTY_RULES[hp["empty"]], hp["persist_name"]),
                       ("hp", "battler", "faint", "current_hp"))
      self.assertEqual(c["stats"][hp["stat"]]["pool"], True)
      self.assertEqual((pp["name"], pp["scope_name"], pp["base"], pp["max"], pp["raises_name"], pp["persist_name"]),
                       ("pp", "move", "pp", "pp_max", "pp_ups", "pp"))
      self.assertTrue(all(m["cost"] == [{"resource": 1, "amount": 1, "formula": None}] for m in c["moves"]))
      self.assertEqual(c["resource_formulas"], ["pp_max"])
      ids = bc.emit_ids("battle.json", c)
      self.assertIn("inline constexpr uint8_t RESOURCE_PP = 1;", ids)
      self.assertNotIn("pp", bc.REQUIRED_ROLES)

   def test_fixture(self) -> None:
      """!The neutral fixture: three battler resources, a regen formula, number and formula costs, a free fallback."""
      c = bc.compile_battle(FIXTURE)
      self.assertEqual(c["names"]["resources"], ["hp", "mp", "ep"])
      self.assertEqual([r["slot"] for r in c["resources"]], [bc.NONE8, 0, 1])
      ep = c["resources"][2]
      self.assertEqual((ep["persist"], ep["regen_formula"]), (bc.NO_ROLE, "ep_regen"))
      moves = dict(zip(c["names"]["moves"], c["moves"]))
      self.assertEqual(moves["Spark"]["cost"], [{"resource": 1, "amount": 4, "formula": None}])
      self.assertEqual(moves["Rush"]["cost"], [{"resource": 2, "amount": 0, "formula": "rush_cost"}])
      self.assertEqual(moves["Tap"]["cost"], [])
      self.assertEqual(c["resource_formulas"], ["ep_regen", "rush_cost"])
      cpp = bc.emit("battle.json", c)
      self.assertIn("case 1: return formula_rush_cost(a, b);", cpp)
      catalog = c["effect_catalog"]
      self.assertEqual(catalog.lookup("resources", "mp"), 1)


class RefusalTests(unittest.TestCase):
   """!Resources the compiler refuses, with a message naming the problem."""

   def test_refusals(self) -> None:
      """!Each broken resource list names what is wrong."""
      def drop_faint(d: JsonDict) -> None:
         resource(d, "hp")["empty"] = "none"

      def two_faint(d: JsonDict) -> None:
         d["resources"].append({"name": "hp2", "scope": "battler", "stat": "vigor", "empty": "faint"})

      def faint_not_pool(d: JsonDict) -> None:
         resource(d, "hp")["stat"] = "mana"

      def move_without_base(d: JsonDict) -> None:
         d["resources"].append({"name": "uses", "scope": "move"})

      def battler_with_base(d: JsonDict) -> None:
         resource(d, "mp")["base"] = "mp"

      def wrong_persist(d: JsonDict) -> None:
         resource(d, "ep")["persist"] = "level"

      def shared_persist(d: JsonDict) -> None:
         resource(d, "ep")["persist"] = "mp_left"

      def raises_without_max(d: JsonDict) -> None:
         d["resources"].append({"name": "uses", "scope": "move", "base": "uses", "raises": "mp_left"})

      def unknown_formula(d: JsonDict) -> None:
         resource(d, "ep")["regen"] = "nothing"

      def no_regen_phase(d: JsonDict) -> None:
         d["turn"]["end_of_turn"] = d["turn"]["end_of_turn"][:-1]

      def unknown_cost(d: JsonDict) -> None:
         d["moves"][0]["cost"] = {"fuel": 1}

      def too_many(d: JsonDict) -> None:
         d["resources"] += [{"name": f"extra{i}", "scope": "battler", "stat": "power"} for i in range(2)]

      def missing_base(d: JsonDict) -> None:
         d["resources"].append({"name": "uses", "scope": "move", "base": "uses"})

      cases = [(drop_faint, "exactly one battler resource has \"empty\": \"faint\""),
               (two_faint, "exactly one battler resource"),
               (faint_not_pool, "the faint resource is the pool stat's"),
               (move_without_base, ".base: the move field holding its base"),
               (battler_with_base, "only a move resource has a base"),
               (wrong_persist, "an engine role the engine fills itself"),
               (shared_persist, "the role is also mp's persist"),
               (raises_without_max, "raises only change a \"max\" formula"),
               (unknown_formula, "names a formula of ruleset.json"),
               (no_regen_phase, "needs a {\"run\": \"regen\"} phase"),
               (unknown_cost, "unknown resource 'fuel'"),
               (too_many, "this platform's engine holds 4"),
               (missing_base, "needs 'uses' (the base of the move resource 'uses'")]
      for edit, message in cases:
         with self.subTest(edit.__name__):
            with self.assertRaisesRegex(bc.ManifestError, re.escape(message)):
               compile_with(FIXTURE, edit)
      # the desktop holds more battler resources than the GBA
      data = bc.view_of(FIXTURE)
      too_many(data)
      c = bc.compile_battle(FIXTURE, None, bc.sources_of(FIXTURE, data), platform="desktop")
      self.assertEqual(len(c["resources"]), 5)

   def test_formula_ranges(self) -> None:
      """!A resource formula's inputs must cover what the engine passes: a battler pool's values reach its stat's
      highest value (the stamina stat's analysed bounds: 81), not the stat cap (C1)."""
      def narrow(d: JsonDict) -> None:
         d["inputs"]["pool_max"] = [0, 10]
      ruleset = FIXTURE.parent / "ruleset.json"
      data = json.loads(ruleset.read_text(encoding="utf-8"))
      narrow(data)
      with self.assertRaisesRegex(bc.ManifestError, re.escape("input 'pool_max' must cover [0, 81]")):
         bc.compile_battle(FIXTURE, None, {ruleset.resolve(): data})

   def test_retired_pp_keys(self) -> None:
      """!The PP-specific keys are refused with migration messages."""
      def no_pp_resource(d: JsonDict) -> None:
         d["resources"] = [r for r in d["resources"] if r["name"] != "pp"]
      with self.assertRaisesRegex(bc.ManifestError, re.escape("a move's pp is the base of a move resource now")):
         compile_with(SAMPLE, no_pp_resource)

      def old_message(d: JsonDict) -> None:
         d["messages"]["no_pp"] = d["messages"].pop("no_usable_move")
      with self.assertRaisesRegex(bc.ManifestError, "no_pp is no_usable_move now"):
         compile_with(SAMPLE, old_message)
      data = json.loads((SAMPLE.parent / "ruleset.json").read_text(encoding="utf-8"))
      del data["formulas"]["pp_max"]["args"]
      with self.assertRaisesRegex(rc.RulesetError, "the pp_max slot is gone"):
         rc.load(bc.strip(data))
      with self.assertRaisesRegex(ec.EffectError, "`free` now"):
         ec.parse('move "Fly" { use { lock user no_pp; } }')

   def test_layout_needs_move_slots(self) -> None:
      """!move_slots has no default in a battle (E8)."""
      def bare(d: JsonDict) -> None:
         del d["monster_layout"]["move_slots"]
      with self.assertRaisesRegex(bc.ManifestError, "needs move_slots"):
         compile_with(FIXTURE, bare)


class LogicTests(unittest.TestCase):
   """!Resources in effect logic: refs, writes, restore and raise."""

   def test_refs_and_primitives(self) -> None:
      """!holder.resource(NAME) reads and writes, resource_max, move.base / move.cost, restore, raise."""
      structure = {"moves": ["Zap"], "stats": ["hp"],
                   "resources": [{"name": "hp", "scope": "battler"}, {"name": "pp", "scope": "move"}]}
      src = ('move "Zap" { use { user.resource(hp) += user.resource_max(pp) + move.base(pp) + move.cost(pp); '
             'restore user hp; restore user pp 2 every_slot; raise user pp 1; } }')
      bank = ec.emit_bytecode(ec.parse(src), structure)
      names = [name for _at, name, _v in ec.instructions(bank.code)]
      self.assertEqual((names.count("Restore"), names.count("Raise"), names.count("Set")), (2, 1, 1))
      self.assertIn("restore user pp 2 every_slot;", ec.format(ec.parse(src)))
      diags = ec.check(ec.parse('move "Zap" { use { restore user fuel; } }'), structure)
      self.assertTrue(any(d.severity == "error" and "fuel" in d.message for d in diags))


class BenchTests(unittest.TestCase):
   """!Bench scenarios set resources by name."""

   def test_keys(self) -> None:
      """!Members and pokes take "resources"; a member's "pp" is refused; names and shapes are checked."""
      base = {"schema": bs.SCHEMA, "seed": 1,
              "parties": [{"side": 0, "controller": "human", "members": [{"species": "Blob", "level": 5}]},
                          {"side": 1, "data": "Dummy"}]}
      ok = json.loads(json.dumps(base))
      ok["parties"][0]["members"][0]["resources"] = {"mp": 3}
      ok["pokes"] = [{"party": 0, "member": 0, "resources": {"ep": 2, "hp": 10}}]
      self.assertEqual(bs.validate(ok), [])
      c = bc.compile_battle(FIXTURE)
      self.assertEqual(bs.check_names(ok, c), [])
      old = json.loads(json.dumps(base))
      old["parties"][0]["members"][0]["pp"] = [5]
      self.assertTrue(any("PP is a resource now" in p for p in bs.validate(old)))
      refill = json.loads(json.dumps(base))
      refill["parties"][0]["members"][0]["resources"] = {"ep": 3, "fuel": 1}
      problems = bs.check_names(refill, c)
      self.assertTrue(any("refilled every battle" in p for p in problems))
      self.assertTrue(any("unknown resource 'fuel'" in p for p in problems))


class MigrationTests(unittest.TestCase):
   """!tools/battle_resources.py turns a pre-E8 battle into one with resources that compiles to the same tables."""

   def test_sample_round_trip(self) -> None:
      """!The sample without its resources (the pre-E8 form) migrates to the sample's resources and compiles to
      the same C++."""
      want = bc.emit("battle.json", bc.compile_battle(SAMPLE))
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         for name in ("ruleset.json", "battle.json", "structure.json"):
            shutil.copy(SAMPLE.parent / name, folder / name)
         for sub in ("effects", "sequences"):
            if (SAMPLE.parent / sub).is_dir():
               shutil.copytree(SAMPLE.parent / sub, folder / sub)
         structure = json.loads((folder / "structure.json").read_text(encoding="utf-8"))
         del structure["resources"]
         (folder / "structure.json").write_text(split.dump(structure), encoding="utf-8")
         ruleset = json.loads((folder / "ruleset.json").read_text(encoding="utf-8"))
         del ruleset["formulas"]["pp_max"]["args"]
         (folder / "ruleset.json").write_text(split.dump(ruleset), encoding="utf-8")
         battle = json.loads((folder / "battle.json").read_text(encoding="utf-8"))
         battle["messages"] = {("no_pp" if k == "no_usable_move" else k): v for k, v in battle["messages"].items()}
         (folder / "battle.json").write_text(split.dump(battle), encoding="utf-8")
         out = io.StringIO()
         with contextlib.redirect_stdout(out):
            self.assertEqual(migrate.main(["battle_resources.py", str(folder / "battle.json"), "--check"]), 0)
         self.assertIn("would change", out.getvalue())
         with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(migrate.main(["battle_resources.py", str(folder / "battle.json")]), 0)
         got = bc.emit("battle.json", bc.compile_battle(folder / "battle.json"))
         self.assertEqual(got, want)
         self.assertEqual(migrate.migrate_file(folder / "battle.json"), [])   # nothing left to do

   def test_refused(self) -> None:
      """!A single-file manifest is not migrated (split it first)."""
      with tempfile.TemporaryDirectory() as tmp:
         path = Path(tmp) / "battle.json"
         path.write_text(split.dump({"schema": bc.BATTLE_SCHEMA, "ruleset": "ruleset.json"}), encoding="utf-8")
         with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(migrate.main(["battle_resources.py", str(path)]), migrate.EXIT_REFUSED)


class ReviewTests(unittest.TestCase):
   """!The engine review's resource findings (C1): persist roles hold their pools, number regens fit the engine's
   int16_t, the fallback states no cost, a cost no pool can pay is refused, formula ids stay below kNone8."""

   def test_persist_roles_hold_their_pools(self) -> None:
      """!A persisted pool's role must hold its highest value: the hit points in a narrow custom role, MP in a 4-bit
      role and a move pool whose max formula passes its role are refused (a write-back would truncate them)."""
      def hp_narrow(d: JsonDict) -> None:
         d["monster_layout"]["roles"].append({"custom": "hp_left", "width": 6})
         resource(d, "hp")["persist"] = "hp_left"
      with self.assertRaisesRegex(bc.ManifestError, r"\(hp\)\.persist: the role 'hp_left' is 6 bits wide"):
         compile_with(FIXTURE, hp_narrow)

      def mp_narrow(d: JsonDict) -> None:
         roles = d["monster_layout"]["roles"]
         k = next(i for i, r in enumerate(roles) if isinstance(r, dict) and r.get("custom") == "mp_left")
         roles[k] = {"custom": "mp_left", "width": 4}
      with self.assertRaisesRegex(bc.ManifestError, r"\(mp\)\.persist: .* 4 bits wide \(up to 15\); its stat"):
         compile_with(FIXTURE, mp_narrow)

      def pp_narrow(d: JsonDict) -> None:
         roles = d["monster_layout"]["roles"]
         roles[roles.index("pp")] = {"role": "pp", "width": 4}
      with self.assertRaisesRegex(bc.ManifestError, r"\(pp\)\.persist: .* its max formula 'pp_max' reaches"):
         compile_with(SAMPLE, pp_narrow)

   def test_regen_fits_int16(self) -> None:
      """!A number regen lies within ResourceData::regen's int16_t; a formula regen is checked against the pool."""
      for amount in (bc.REGEN_RANGE[1] + 1, bc.REGEN_RANGE[0] - 1):
         def regen(d: JsonDict, amount: int = amount) -> None:
            resource(d, "ep")["regen"] = amount
         with self.subTest(amount=amount):
            with self.assertRaisesRegex(bc.ManifestError, r"regen"):
               compile_with(FIXTURE, regen)

      def most(d: JsonDict) -> None:
         resource(d, "ep")["regen"] = bc.REGEN_RANGE[1]
      self.assertEqual(resource(compile_with(FIXTURE, most), "ep")["regen"], bc.REGEN_RANGE[1])

   def test_costs(self) -> None:
      """!The fallback move states no cost; a number cost above what its pool reaches is refused (the hit points
      need more than the cost: paying never faints)."""
      data = bc.view_of(FIXTURE)
      fallback = data["rules"]["fallback_move"]

      def fallback_cost(d: JsonDict) -> None:
         next(m for m in d["moves"] if m["name"] == fallback)["cost"] = {"mp": 1}
      with self.assertRaisesRegex(bc.ManifestError, "rules.fallback_move, which pays nothing"):
         compile_with(FIXTURE, fallback_cost)

      def too_dear(d: JsonDict) -> None:
         next(m for m in d["moves"] if m["name"] != fallback)["cost"] = {"ep": bc.U16_MAX}
      with self.assertRaisesRegex(bc.ManifestError, "can never be paid"):
         compile_with(FIXTURE, too_dear)

      c = bc.compile_battle(FIXTURE)
      hp = next(r for r in c["resources"] if r["name"] == "hp")
      top = c["stats"][hp["stat"]]["bounds"][1]   # the most hit points a battler has

      def whole_hp(d: JsonDict) -> None:
         next(m for m in d["moves"] if m["name"] != fallback)["cost"] = {"hp": top}
      with self.assertRaisesRegex(bc.ManifestError, "paying never faints the user, so the pool must hold more "
                                                    "than the cost"):
         compile_with(FIXTURE, whole_hp)

   def test_move_cost_bounded_by_its_max(self) -> None:
      """!A number cost on a move resource is bounded by that move's own pool (its max formula over the move's base
      and the raises range), not the pool's storage (C4, review2 rv/t7.py): pp 200 on a move whose pp reaches
      far less is refused in clear words; the move's own highest pp is accepted."""
      data = bc.view_of(PROGRESS)
      pp = next(r for r in data["resources"] if r.get("scope") == "move")
      c = bc.compile_battle(PROGRESS)
      res = next(r for r in c["resources"] if r["name"] == pp["name"])
      move = c["moves"][1]
      top, _why = bc_resources.move_pool_top(res, move, c["ruleset"], c["layout"])
      self.assertLess(top, bc_resources.STORAGE_MAX["move"])

      def dear(d: JsonDict, amount: int) -> None:
         d["moves"][1]["cost"] = {pp["name"]: amount}
      with self.assertRaisesRegex(bc.ManifestError, rf"moves\[1\]\.cost\.{pp['name']}: 200 can never be paid: the "
                                                    rf"{pp['name']} pool holds at most {top} \(its max formula"):
         compile_with(PROGRESS, lambda d: dear(d, 200))
      kept = compile_with(PROGRESS, lambda d: dear(d, top))
      self.assertEqual(kept["moves"][1]["cost"][0]["amount"], top)
      with self.assertRaisesRegex(bc.ManifestError, rf"holds at most {top}"):
         compile_with(PROGRESS, lambda d: dear(d, top + 1))

   def test_formula_ids_below_none(self) -> None:
      """!At most MAX_FORMULAS distinct resource formulas (id kNone8 is "no formula")."""
      moves = [{"cost": [{"formula": f"f{k}"}]} for k in range(bc.MAX_FORMULAS + 1)]
      with self.assertRaisesRegex(bc.ManifestError, "at most 255"):
         bc.formula_ids([], moves)
      self.assertEqual(len(bc.formula_ids([], moves[:bc.MAX_FORMULAS])), bc.MAX_FORMULAS)


class MigrationReviewTests(unittest.TestCase):
   """!tools/battle_resources.py after the review (C1): the ruleset's pp_max is kept without raises, no_pp beside
   no_usable_move, clear refusals, all files or none."""

   def copy_sample(self, folder: Path) -> Path:
      """!The PP-shaped sample in its pre-E8 form (no resources, the pp_max slot, the message no_pp).
      @param folder  where
      @return its battle.json
      """
      for name in ("ruleset.json", "battle.json", "structure.json"):
         shutil.copy(SAMPLE.parent / name, folder / name)
      if (SAMPLE.parent / "effects").is_dir():
         shutil.copytree(SAMPLE.parent / "effects", folder / "effects")
      structure = json.loads((folder / "structure.json").read_text(encoding="utf-8"))
      del structure["resources"]
      (folder / "structure.json").write_text(split.dump(structure), encoding="utf-8")
      ruleset = json.loads((folder / "ruleset.json").read_text(encoding="utf-8"))
      del ruleset["formulas"]["pp_max"]["args"]
      (folder / "ruleset.json").write_text(split.dump(ruleset), encoding="utf-8")
      return folder / "battle.json"

   def test_pp_max_without_raises(self) -> None:
      """!A ruleset's pp_max slot stays the pp resource's max when the layout has no pp_ups (E7 applied it)."""
      structure = {"stats": [{"name": "hp", "pool": True}], "monster_layout": {"roles": ["pp"], "move_slots": 4}}
      new, _notes, has_max = migrate.migrate_structure(structure, [], ruleset_max=True)
      pp = next(r for r in new["resources"] if r["name"] == "pp")
      self.assertTrue(has_max)
      self.assertEqual(pp.get("max"), "pp_max")
      self.assertNotIn("raises", pp)

   def test_messages_and_refusals(self) -> None:
      """!no_pp beside no_usable_move is dropped (the latter kept); a battle without a ruleset or a structure
      without stats is refused naming what is missing."""
      new, notes = migrate.migrate_content({"messages": {"no_usable_move": "A", "no_pp": "B"}}, False)
      self.assertEqual(new["messages"], {"no_usable_move": "A"})
      self.assertTrue(any("dropped" in n for n in notes))
      with self.assertRaisesRegex(bc.ManifestError, "predates the manifest stats"):
         migrate.migrate_structure({"monster_layout": {"roles": []}}, [])
      with tempfile.TemporaryDirectory() as tmp:
         path = Path(tmp) / "battle.json"
         path.write_text(split.dump({"schema": bc.BATTLE_SCHEMA, "structure": "structure.json"}), encoding="utf-8")
         with self.assertRaisesRegex(bc.ManifestError, "names no ruleset"):
            migrate.migrate_file(path)

   def test_all_or_nothing(self) -> None:
      """!A write that fails part way leaves every file as it was."""
      with tempfile.TemporaryDirectory() as tmp:
         path = self.copy_sample(Path(tmp))
         before = {p.name: p.read_text(encoding="utf-8") for p in Path(tmp).glob("*.json")}
         listing = sorted(p.name for p in Path(tmp).iterdir())
         real = Path.write_text
         calls = []

         def failing(self: Path, text: str, encoding: str | None = None) -> int:
            """!Fails the second temporary file."""
            calls.append(self)
            if len(calls) == 2:
               raise OSError(28, "No space left on device")
            return real(self, text, encoding=encoding)
         Path.write_text = failing
         try:
            with self.assertRaisesRegex(bc.ManifestError, "nothing written"):
               migrate.migrate_file(path)
         finally:
            Path.write_text = real
         after = {p.name: p.read_text(encoding="utf-8") for p in Path(tmp).glob("*.json")}
         self.assertEqual(after, before)
         self.assertEqual(sorted(p.name for p in Path(tmp).iterdir()), listing)   # no temporary file is left


if __name__ == "__main__":
   unittest.main()
