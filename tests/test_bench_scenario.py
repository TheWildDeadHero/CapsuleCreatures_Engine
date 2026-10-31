#!/usr/bin/env python3
"""!Tests for the battle bench (D-23 section 3.3): tools/bench_scenario.py (scenario validation, output parsing, the
bench tables) and the effect compiler's debug line table; with a built bench (environment CAPSULE_BENCH = its path;
CTest sets it, see cmake/parts/bench.cmake) also the bench itself: golden transcripts reproduced exactly, battler
pokes with a VM and RNG trace, scripted answers and error handling. CAPSULE_BENCH_BUILD=1 also tests build() (a
full configure into a temporary cache; slow).
"""
from __future__ import annotations

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler as bc  # noqa: E402
import bench_scenario as bs  # noqa: E402
import effect_compiler as ec  # noqa: E402

## Scenarios that reproduce the Starter sample's goldens (the WP-2 goldens' scenarios are the reference repo's, E11).
GOLDEN_SCENARIOS = sorted((ROOT / "tests" / "bench" / "starter").glob("*.json"))
## The single battle the edited-scenario tests start from.
BASE = ROOT / "tests" / "bench" / "starter" / "unowned_single.json"
## Other scenarios (pokes, errors).
CASES = ROOT / "tests" / "bench" / "cases"
## The Starter sample's battle manifest (the bench of CTest is built from it).
MANIFEST = ROOT / "samples" / "starter" / "battle.json"
## The neutral effect examples (the line table tests).
EXAMPLES = ROOT / "tests" / "data" / "effects" / "examples.json"
## The built bench, if any.
BENCH = os.environ.get("CAPSULE_BENCH")


def scenario(path: Path) -> dict[str, Any]:
   """!A scenario file's data.
   @param path  the file
   @return the scenario
   """
   return json.loads(path.read_text(encoding="utf-8"))


class ValidateTests(unittest.TestCase):
   """!Scenario shape checks (no bench needed)."""

   def test_shipped_scenarios_are_valid(self) -> None:
      """!Every shipped scenario validates."""
      for path in GOLDEN_SCENARIOS + sorted(CASES.glob("*.json")):
         with self.subTest(path.name):
            self.assertEqual(bs.validate(scenario(path)), [])

   def test_problems(self) -> None:
      """!Each malformed part is reported with its path."""
      good = scenario(BASE)
      cases = [
         (lambda s: s.pop("seed"), 'needs "seed"'),
         (lambda s: s.update(schema="capsule.bench/0"), "schema"),
         (lambda s: s.update(colour="red"), "colour: unknown member"),
         (lambda s: s.update(seed=-1), "seed: an integer"),
         (lambda s: s.update(rng="Lcg"), "rng: a generator name"),
         (lambda s: s.update(rng=1), "rng: a generator name"),
         (lambda s: s.update(seed=True), "seed: an integer"),
         (lambda s: s.update(choices=["fight"]), "choices[0]"),
         (lambda s: s.update(choices=[[]]), "choices[0]: a non-empty list"),
         (lambda s: s.update(choices=[{"move": 0, "switch": 1}]), "choices[0]"),
         (lambda s: s.update(answers=[{"learn": 1}]), "answers[0]"),
         (lambda s: s.update(stop={"turns": 0}), "stop.turns"),
         (lambda s: s.update(stop="never"), "stop:"),
         (lambda s: s.update(trace={"rng": 1}), "trace.rng"),
         (lambda s: s.update(parties=[]), "parties: a list"),
         (lambda s: s["parties"][0].update(data="Novice Kit"), "either \"data\""),
         (lambda s: s["parties"][0]["members"][0].pop("level"), 'needs "species" and "level"'),
         (lambda s: s["parties"][0]["members"][0].update(level=0), "level: an integer"),
         (lambda s: s["parties"][1].update(flags=["shiny"]), "flags"),
         (lambda s: s.update(pokes=[{"party": 0}]), 'needs "party" and "member"'),
         (lambda s: s.update(pokes=[{"party": 0, "member": 0, "stages": {"attack": 16}}]), "stages.attack"),
         (lambda s: s.update(pokes=[{"party": 0, "member": 0, "counters": {"dazed": 256}}]),
          "counters.dazed"),
         (lambda s: s.update(pokes=[{"party": 0, "member": 0, "counters": [2]}]), "counters: an object"),
      ]
      for change, expected in cases:
         s = copy.deepcopy(good)
         change(s)
         problems = bs.validate(s)
         with self.subTest(expected):
            self.assertTrue(any(expected in p for p in problems), problems)

   def test_check_names(self) -> None:
      """!check_names(): pokes against the manifest (volatiles with counters, stage ranges, names); the engine's
      bench keeps only the generic poke keys (C1): a volatile counter is "counters": {volatile: n}, the status's
      counter and scratch status_counter / status_aux (the former names are refused, naming the new ones)."""
      c = bc.compile_battle(MANIFEST)
      good = scenario(CASES / "pokes_trace.json")
      self.assertEqual(bs.check_names(good, c), [])
      for old, new in bs.RETIRED_POKE_KEYS.items():
         retired = copy.deepcopy(good)
         retired["pokes"][1][old] = 1
         with self.subTest(retired=old):
            self.assertTrue(any(f"is {new!r} now" in p for p in bs.validate(retired)), bs.validate(retired))
      generic = copy.deepcopy(good)
      generic["pokes"][1].update(status_counter=2, status_aux=1)
      self.assertEqual(bs.validate(generic), [])
      bad = copy.deepcopy(good)
      bad["pokes"][0].update(stages={"attack": 7, "luck": 1}, counters={"flinch": 1, "nothing": 1}, status="Sleepy",
                             volatiles=["nope"])
      problems = bs.check_names(bad, c)
      for expected in ("stages.attack: -4 to 4", "stages.luck: unknown stage", "counters.nothing: unknown volatile",
                       "status: unknown status", "volatiles[0]: unknown volatile"):
         with self.subTest(expected):
            self.assertTrue(any(expected in p for p in problems), problems)
      no_counter = [v["source_name"] for v in c["volatiles"] if v["counter"] == bs.NO_SLOT]
      if no_counter:
         bad = copy.deepcopy(good)
         bad["pokes"][0]["counters"] = {no_counter[0]: 1}
         self.assertTrue(any("has no counter" in p for p in bs.check_names(bad, c)))

   def test_load_errors(self) -> None:
      """!load() raises BenchError for unreadable, unparsable and malformed files."""
      with tempfile.TemporaryDirectory() as tmp:
         bad = Path(tmp) / "bad.json"
         bad.write_text("{", encoding="utf-8")
         with self.assertRaises(bs.BenchError):
            bs.load(bad)
         bad.write_text(json.dumps({"schema": bs.SCHEMA}), encoding="utf-8")
         with self.assertRaises(bs.BenchError):
            bs.load(bad)
         with self.assertRaises(bs.BenchError):
            bs.load(Path(tmp) / "missing.json")

   def test_generators_manifest(self) -> None:
      """!A generators manifest (capsule.generators/1) declares generator names and their sources (C1: the studio
      reads it instead of guessing from file names); bad names, the engine's own name and missing sources fail."""
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         (folder / "src").mkdir()
         (folder / "src" / "lcg.cpp").write_text("// next16()\n", encoding="utf-8")
         path = folder / bs.GENERATORS_FILE
         good = {"schema": bs.GENERATORS_SCHEMA, "generators": [{"name": "lcg", "source": "src/lcg.cpp"}]}
         path.write_text(json.dumps(good), encoding="utf-8")
         self.assertEqual(bs.load_generators(path), {"lcg": (folder / "src" / "lcg.cpp").resolve()})
         for bad in ({"name": bs.DEFAULT_RNG, "source": "src/lcg.cpp"}, {"name": "Lcg", "source": "src/lcg.cpp"},
                     {"name": "gone", "source": "src/gone.cpp"}, {"name": "lcg"}):
            path.write_text(json.dumps({**good, "generators": [bad]}), encoding="utf-8")
            with self.subTest(bad=bad):
               with self.assertRaises(bs.BenchError):
                  bs.load_generators(path)


class CheckAndBuildStepTests(unittest.TestCase):
   """!Transcript comparison and build steps without a bench (E7: the studio's API asks)."""

   def test_compare_transcripts(self) -> None:
      """!The first differing line is named; equal texts give no problem."""
      self.assertEqual(bs.compare_transcripts("a\nb\n", "a\nb\n"), [])
      problem = bs.compare_transcripts("a\nb\n", "a\nc\n", "x.txt")[0]
      self.assertIn("x.txt:2", problem)
      self.assertIn("expected: b", problem)
      self.assertIn("<end>", bs.compare_transcripts("a\n", "a\nb\n")[0])

   def test_check_expected_from_a_result(self) -> None:
      """!A run the caller made is compared without running the bench again."""
      path = BASE
      want = (path.parent / scenario(path)["expect"]).read_text(encoding="utf-8")
      lines = [{"t": "line", "text": t} for t in want.splitlines()]
      self.assertEqual(bs.check_expected(None, path, bs.Result(0, lines)), [])
      self.assertIn("expected:", bs.check_expected(None, path, bs.Result(0, lines[:-1]))[0])
      self.assertIn("the bench failed", bs.check_expected(None, path, bs.Result(1, lines))[0])
      with self.assertRaisesRegex(bs.BenchError, "needs the bench or a result"):
         bs.check_expected(None, path)

   def test_step_progress_and_cancel(self) -> None:
      """!A step passes its output lines on; cancel terminates it; a failing step keeps its log."""
      seen: list[str] = []
      bs._step([sys.executable, "-c", "print('one'); print('two')"], "step", seen.append, None)
      self.assertEqual(seen, ["one", "two"])
      with self.assertRaises(bs.BenchCancelled):
         bs._step([sys.executable, "-c", "import time; time.sleep(30)"], "slow step", None, lambda: True)
      with self.assertRaisesRegex(bs.BenchError, "bad step failed:\n.*boom"):
         bs._step([sys.executable, "-c", "print('boom'); raise SystemExit(3)"], "bad step", None, None)
      with self.assertRaisesRegex(bs.BenchError, "jobs"):
         bs.build(MANIFEST, tempfile.gettempdir(), jobs=0)


class OutputTests(unittest.TestCase):
   """!Parsing the bench's JSON lines."""

   def test_parse(self) -> None:
      """!Records by type, the transcript and the errors."""
      text = "\n".join(json.dumps(r) for r in [
         {"t": "bench"}, {"t": "line", "text": "-- x"}, {"t": "rng", "n": 1}, {"t": "line", "text": "turn 1"},
         {"t": "error", "message": "m", "where": "seed"}]) + "\n"
      res = bs.Result(0, bs.parse_output(text))
      self.assertEqual(res.transcript(), "-- x\nturn 1\n")
      self.assertEqual(len(res.of("rng")), 1)
      self.assertEqual(res.errors(), ["seed: m"])
      with self.assertRaises(bs.BenchError):
         bs.parse_output("not json\n")
      with self.assertRaises(bs.BenchError):
         bs.parse_output("[1]\n")


class LineTableTests(unittest.TestCase):
   """!The effect compiler's debug pc -> source line table (effect_compiler.track_lines / emit_line_table)."""

   def test_tracking_keeps_the_bytecode(self) -> None:
      """!Tracking records lines without changing the bank; without it nothing is recorded."""
      plain = ec.load_unit(EXAMPLES).bank()
      ec.track_lines(True)
      try:
         tracked = ec.load_unit(EXAMPLES).bank()
      finally:
         ec.track_lines(False)
      self.assertEqual(plain.code, tracked.code)
      self.assertEqual(plain.programs, tracked.programs)
      self.assertEqual(sum(len(x) for x in plain.lines), 0)
      self.assertGreater(sum(len(x) for x in tracked.lines), 0)
      self.assertEqual(ec.NODE_SOURCES, {})
      for entries in tracked.lines:
         pcs = [e[0] for e in entries]
         self.assertEqual(pcs, sorted(pcs))
         self.assertTrue(all(e[2] >= 1 and e[3] >= 1 for e in entries))
      text = "\n".join(ec.emit_line_table(tracked))
      self.assertIn("const effect::LineTable kEffectLines = {kEffectLinesEntries, kEffectLinesFiles,", text)
      self.assertIn("examples.ceffect", text)

   def test_statement_lines(self) -> None:
      """!Each statement's code starts at an entry with the statement's line."""
      src = 'move "Zap" {\n   hit {\n      damage;\n      stage target atk -1;\n   }\n}\n'
      structure = {"stats": ["atk"], "stages": ["accuracy", "evasion", "critical", "atk"], "moves": ["Zap"]}
      ec.track_lines(True)
      try:
         bank = ec.emit_bytecode(ec.parse(src, "zap.ceffect"), structure)
      finally:
         ec.track_lines(False)
      lines = [e[2] for e in bank.lines[0]]
      self.assertIn(3, lines)
      self.assertIn(4, lines)
      self.assertEqual({e[1] for e in bank.lines[0]}, {"zap.ceffect"})

   def test_empty_table(self) -> None:
      """!An untracked bank gives an empty table with one placeholder entry."""
      bank = ec.load_unit(EXAMPLES).bank()
      text = "\n".join(ec.emit_line_table(bank, "kT"))
      self.assertIn("{kTEntries, kTFiles, 0, 0}", text)


class EmitDataTests(unittest.TestCase):
   """!The bench tables (emit-data)."""

   def test_tables(self) -> None:
      """!Names, owners and the line table of the Starter sample; the bank is the battle compiler's."""
      deps: list[Path] = []
      text = bs.emit_data(MANIFEST, "headless", deps)
      self.assertIn('"Emberkit"', text)
      self.assertIn('"Novice Kit"', text)
      self.assertIn("kEffectLinesEntries", text)
      self.assertIn("const BenchData kBenchData", text)
      self.assertIn(MANIFEST.resolve(), [d.resolve() for d in deps])
      self.assertFalse(ec.LINE_TRACKING[0])
      plain = bc.compile_battle(MANIFEST, platform="headless")["effects"]["bank"]
      ec.track_lines(True)
      try:
         tracked = bc.compile_battle(MANIFEST, platform="headless")["effects"]["bank"]
      finally:
         ec.track_lines(False)
      self.assertEqual(plain.code, tracked.code)
      self.assertEqual(plain.programs, tracked.programs)
      self.assertEqual(plain.handlers, tracked.handlers)


@unittest.skipUnless(BENCH, "set CAPSULE_BENCH to a built capsule_starter_bench (CTest does)")
class BenchTests(unittest.TestCase):
   """!The bench itself."""

   def test_goldens(self) -> None:
      """!Every golden scenario reproduces its transcript byte for byte."""
      self.assertGreaterEqual(len(GOLDEN_SCENARIOS), 3)
      for path in GOLDEN_SCENARIOS:
         with self.subTest(path.name):
            self.assertEqual(bs.check_expected(BENCH, path), [])

   def test_traces_do_not_change_the_transcript(self) -> None:
      """!Tracing everything leaves the transcript as it is; the draws counted equal the rng records."""
      path = BASE
      plain = bs.run(BENCH, path)
      traced = bs.run(BENCH, path, ["rng", "vm", "events"])
      self.assertEqual(plain.transcript(), traced.transcript())
      stop = traced.of("stop")[0]
      self.assertEqual(stop["draws"], len(traced.of("rng")))
      self.assertEqual([r["n"] for r in traced.of("rng")], list(range(1, stop["draws"] + 1)))
      self.assertGreater(len(traced.of("event")), 0)

   def test_pokes_and_vm_trace(self) -> None:
      """!Pokes reach the battle; VM records carry owner, hook, pc and a source position; a turn stop."""
      res = bs.run(BENCH, CASES / "pokes_trace.json")
      self.assertEqual(res.status, 0, res.errors())
      text = res.transcript()
      self.assertIn("> Emberkit is dazed!", text)              # the volatile and its counter
      self.assertIn("> Sproutle is hurt by its scorch!", text)  # the status
      self.assertIn("hp #1 Sproutle 24 (was 30)", text)        # the HP (Scorch: 1/12, C2)
      self.assertIn("item Focus Lens", text)                   # the record item
      stop = res.of("stop")[0]
      self.assertEqual(stop["reason"], "turns")
      self.assertEqual(stop["turn"], 2)
      vm = res.of("vm")
      self.assertGreater(len(vm), 0)
      for r in vm:
         for key in ("op", "owner", "hook", "program", "pc", "line", "column"):
            self.assertIn(key, r)
      sourced = [r for r in vm if r.get("file") == "effects/moves.ceffect"]
      self.assertGreater(len(sourced), 0)
      # a condition template's code (the scorch's residual) is a "template", not a "file"
      self.assertIn("status Scorch", {r["template"] for r in vm if "template" in r})
      self.assertFalse(any(r.get("file", "").endswith(bc.TEMPLATE_SUFFIX) for r in vm))
      # the source line names the owner's code: the owner's name appears at or above that line
      moves = (MANIFEST.parent / "effects" / "moves.ceffect").read_text(encoding="utf-8").splitlines()
      r = sourced[0]
      name = r["owner"].split(" ", 1)[1]
      self.assertTrue(any(f'"{name}"' in line or f" {name} " in line for line in moves[:r["line"]]))
      self.assertGreater(len(res.of("rng")), 0)

   def test_rng_mismatch(self) -> None:
      """!A scenario made with another generator than the bench's is refused; its own runs."""
      data = scenario(BASE)
      self.assertEqual(data["rng"], "default")
      self.assertEqual(bs.run(BENCH, data).errors(), [])
      data["rng"] = "method1"
      res = bs.run(BENCH, data)
      self.assertNotEqual(res.status, 0)
      self.assertTrue(any("needs the method1 generator" in e for e in res.errors()), res.errors())

   def test_unknown_names(self) -> None:
      """!Unknown names are errors with their path; nothing runs."""
      res = bs.run(BENCH, CASES / "bad_names.json")
      self.assertEqual(res.status, bs.EXIT_FAILED)
      wheres = {r.get("where") for r in res.of("error")}
      for w in ("format", "parties[0].members[0].species", "parties[1].data", "pokes[0].volatiles[0]",
                "pokes[0].stages.luck"):
         self.assertIn(w, wheres)
      self.assertEqual(res.of("line"), [])

   def test_bad_documents(self) -> None:
      """!Syntax errors, fractions, a wrong schema and unknown members are reported by the bench too."""
      base = scenario(BASE)
      wrong = dict(base, schema="capsule.bench/9")
      res = bs.run(BENCH, wrong, check=False)
      self.assertEqual(res.status, bs.EXIT_FAILED)
      self.assertIn("schema", res.errors()[0])
      extra = dict(base, colour="red")
      res = bs.run(BENCH, extra, check=False)
      self.assertIn("unknown member \"colour\"", res.errors()[0])
      for text, expected in (("[", "unexpected end"), ('{"seed": 1.5}', "integers"), ('{"a": 1} x', "after")):
         done = subprocess.run([BENCH, "-"], input=text, capture_output=True, text=True)
         self.assertEqual(done.returncode, bs.EXIT_FAILED)
         self.assertIn(expected, bs.parse_output(done.stdout)[0]["message"])
      done = subprocess.run([BENCH], capture_output=True, text=True)
      self.assertEqual(done.returncode, bs.EXIT_USAGE)
      with self.assertRaises(bs.BenchError):
         bs.run(BENCH, wrong)   # validated first

   def test_answers(self) -> None:
      """!Answers are used only when an effect asks (the test data never does): the transcript stays the same."""
      path = BASE
      base = scenario(path)
      res = bs.run(BENCH, dict(base, answers=[{"learn_move": 0}]))
      self.assertEqual(res.transcript(), (path.parent / base["expect"]).read_text(encoding="utf-8"))

   def test_stop_steps_and_move_by_name(self) -> None:
      """!A step stop ends early without "unfinished"; a move the battler lacks is a note."""
      base = scenario(BASE)
      res = bs.run(BENCH, dict(base, stop={"steps": 3}))
      self.assertEqual(res.of("stop")[0]["reason"], "steps")
      self.assertNotIn("unfinished", res.transcript())
      self.assertIn("-- outcome None", res.transcript())
      res = bs.run(BENCH, dict(base, choices=[{"move": "Quake"}]))   # a Starter move Emberkit does not know
      self.assertGreater(len(res.of("note")), 0)


@unittest.skipUnless(os.environ.get("CAPSULE_BENCH_BUILD") == "1", "set CAPSULE_BENCH_BUILD=1 to test build()")
class BuildTests(unittest.TestCase):
   """!build(): configure once into a cache, then rebuild incrementally."""

   def test_build_and_rebuild(self) -> None:
      """!The game bench builds, runs a golden scenario with the default generator, and a second build reuses the
      cache; a game's own generator needs its source."""
      with self.assertRaisesRegex(bs.BenchError, "needs its source file"):
         bs.build(MANIFEST, tempfile.gettempdir(), "method1")
      with tempfile.TemporaryDirectory() as tmp:
         exe = bs.build(MANIFEST, Path(tmp) / "cache")
         self.assertEqual(bs.check_expected(exe, BASE), [])
         cache = (Path(tmp) / "cache" / "CMakeCache.txt").stat().st_mtime
         self.assertEqual(bs.build(MANIFEST, Path(tmp) / "cache"), exe)
         self.assertEqual((Path(tmp) / "cache" / "CMakeCache.txt").stat().st_mtime, cache)


if __name__ == "__main__":
   unittest.main()
