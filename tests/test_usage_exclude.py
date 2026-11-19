#!/usr/bin/env python3
"""!Tests for tools/usage_exclude.py (D-36 package 3, the exclusion manifest): the engine modules an nm listing's
objects reference, the manifest generated from a report and those references, its check (errors, stale items,
suggestions), its shape, the command line, and the script compiler's --exclude (small invented reports and listings;
the test script app's scripts)."""
from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from typing import Any

## The engine's root.
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import script_compiler  # noqa: E402
import usage  # noqa: E402
import usage_exclude as ux  # noqa: E402

## The script test app's scripts.
TEST_SCRIPTS = ROOT / "apps" / "test_script" / "data" / "scripts.json"
## An `nm -A -P -u` listing: a game object referencing capsule::seq::start() and capsule::rng::next() (the core's), a
## second referencing capsule::growth::find(int) twice, a defined symbol (not a reference) and a C function.
LISTING = """\
build/CMakeFiles/capsule_demo.dir/src/game.cpp.o: _ZN7capsule3seq5startEv U
build/CMakeFiles/capsule_demo.dir/src/game.cpp.o: _ZN7capsule3rng4nextEv U
build/CMakeFiles/capsule_demo.dir/src/scene.cpp.o: _ZN7capsule6growth4findEi U
build/CMakeFiles/capsule_demo.dir/src/scene.cpp.o: _ZN7capsule6growth4findEi U
build/CMakeFiles/capsule_demo.dir/src/scene.cpp.o: _ZN7capsule7storage3putEv T 0 4
build/CMakeFiles/capsule_demo.dir/src/scene.cpp.o: memcpy U
"""
## The script VM's ops of the invented game.
OPS = ["End", "Msg", "Nursery", "ItemGive"]


def report(script_ops: dict[str, int] | None = None) -> dict[str, Any]:
   """!An invented usage report: storage used, seq / growth / combine / monster unused.
   @param script_ops  the scripts' opcodes (script.op), or None for a game without scripts
   @return it
   """
   modules = {m: {"used": False, "by": []} for m in ux.EXCLUDABLE_MODULES}
   modules["storage"] = {"used": True, "by": ["battle.feature"]}
   modules["core"] = {"used": True, "by": ["base"]}
   used = {} if script_ops is None else {ux.SCRIPT_OP_CATEGORY: script_ops}
   return {"schema": usage.REPORT_SCHEMA, "game": "demo", "modules": modules, "used": used}


def refs() -> dict[str, dict[str, str]]:
   """!LISTING's references.
   @return parse_references()'s result
   """
   return ux.parse_references(LISTING, list(report()["modules"]))


class ExcludeTests(unittest.TestCase):
   """!parse_references(), generate(), check(), validate() and the command line."""

   def test_references(self) -> None:
      """!Undefined engine symbols, by module and object; defined symbols and C functions are not references."""
      self.assertEqual(refs(), {"seq": {"src/game.cpp.o": "_ZN7capsule3seq5startEv"},
                                "core": {"src/game.cpp.o": "_ZN7capsule3rng4nextEv"},
                                "growth": {"src/scene.cpp.o": "_ZN7capsule6growth4findEi"}})

   def test_generate(self) -> None:
      """!Unused modules no object references, and the ops no script emits."""
      manifest = ux.generate(report({"End": 3, "Msg": 2, "Nursery": 0}), refs(), OPS, "usage/demo.json")
      self.assertEqual(ux.names(manifest, "modules"), ["combine", "monster"])
      self.assertEqual(ux.names(manifest, "script_ops"), ["Nursery", "ItemGive"])
      self.assertEqual(manifest["modules"][0]["reason"], ux.MODULE_REASON)
      self.assertEqual(ux.validate(json.loads(ux.manifest_text(manifest))), manifest)
      # a game without scripts excludes no ops
      self.assertEqual(ux.generate(report(), {}, OPS)["script_ops"], [])

   def test_check(self) -> None:
      """!Errors, stale items and suggestions."""
      manifest = {"schema": ux.SCHEMA,
                  "modules": [{"name": n} for n in ("seq", "storage", "core", "combine", "combine", "lost")],
                  "script_ops": [{"name": n} for n in ("Msg", "Gone")]}
      found = ux.check(manifest, report({"End": 1, "Msg": 2}), refs(), OPS)
      self.assertEqual(found.errors, [
         "modules: combine is listed more than once",
         "module seq is excluded but src/game.cpp.o references it (_ZN7capsule3seq5startEv)",
         "module storage is excluded but used: battle.feature (the usage report)",
         f"module core: cannot be excluded (excludable: {', '.join(ux.EXCLUDABLE_MODULES)})",
         "script op Msg is excluded but the scripts emit it 2 times"])
      self.assertEqual(found.warnings, ["module lost: this engine has no such module (a stale exclusion)",
                                        "script op Gone: the script VM has no such op (a stale exclusion)"])
      self.assertEqual(found.suggestions, ["module monster: nothing uses it; it could be excluded",
                                           "script op Nursery: no script emits it; it could be excluded",
                                           "script op ItemGive: no script emits it; it could be excluded"])
      self.assertTrue(ux.findings_text(found).endswith("exclude_check: 5 errors, 2 warnings, 3 suggestions\n"))
      # the generated manifest passes its own check
      clean = ux.generate(report({"End": 1}), refs(), OPS)
      self.assertEqual(ux.check(clean, report({"End": 1}), refs(), OPS).errors, [])

   def test_unreadable(self) -> None:
      """!An object nm cannot read (Xcode's nm and LTO bitcode) is listed, the others are read, and the check warns."""
      game, bad = "build/CMakeFiles/capsule_demo.dir/src/game.cpp.o", "build/CMakeFiles/capsule_demo.dir/src/bad.o"

      def fake_nm(nm: str, files: list[str]) -> str | None:
         """!Lists LISTING's lines of the files given, refusing a run that includes bad.o."""
         given = [f.replace("\\", "/") for f in files]   # str(Path) uses backslashes on Windows
         if bad in given:
            return None
         return "".join(line + "\n" for line in LISTING.splitlines() if line.split(":")[0] in given)

      real = ux.run_nm
      ux.run_nm = fake_nm
      try:
         unreadable: list[str] = []
         found = ux.read_references([Path(game), Path(bad)], "nm", list(report()["modules"]), unreadable)
      finally:
         ux.run_nm = real
      self.assertEqual(unreadable, ["src/bad.o"])
      self.assertEqual(sorted(found), ["core", "seq"])
      warnings = ux.check({"schema": ux.SCHEMA}, report(), found, OPS, unreadable).warnings
      self.assertEqual(len(warnings), 1)
      self.assertIn("could not read 1 object file(s) (first: src/bad.o)", warnings[0])

   def test_validate(self) -> None:
      """!The schema, the keys and the items' shape."""
      for bad in ({"schema": "x"}, {"schema": ux.SCHEMA, "extra": 1}, {"schema": ux.SCHEMA, "modules": "seq"},
                  {"schema": ux.SCHEMA, "modules": ["seq"]}, {"schema": ux.SCHEMA, "script_ops": [{"op": "End"}]}):
         with self.assertRaises(ux.ExcludeError):
            ux.validate(bad)

   def test_command_line(self) -> None:
      """!`exclude` writes the manifest; `exclude-check` fails on an error and writes the findings."""
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp) / "usage"
         folder.mkdir()
         path = folder / "demo.json"
         path.write_text(json.dumps(report({"End": 1, "Message": 1})), encoding="utf-8")
         out, findings = folder / "demo.exclude.json", folder / "demo.exclude-check.txt"
         with redirect_stdout(io.StringIO()):
            self.assertEqual(usage.main(["exclude", str(path), "--out", str(out)]), 0)
            written = ux.load(out)
            self.assertEqual(written["generated_from"], "usage/demo.json")
            self.assertIn("monster", ux.names(written, "modules"))
            self.assertEqual(usage.main(["exclude-check", str(out), str(path), "--out", str(findings)]), 0)
            written["script_ops"].append({"name": "Message"})
            out.write_text(ux.manifest_text(written), encoding="utf-8")
            self.assertEqual(usage.main(["exclude-check", str(out), str(path), "--out", str(findings)]), 1)
         self.assertIn("error: script op Message is excluded", findings.read_text(encoding="utf-8"))


class CompilerTests(unittest.TestCase):
   """!The script compiler's --exclude."""

   def test_excluded_op(self) -> None:
      """!An excluded op the scripts emit is an error at the statement; without it the compile is clean."""
      clean = script_compiler.compile_file(TEST_SCRIPTS)
      self.assertEqual(clean.errors(), [])
      emitted = sorted(n for n, c in clean.op_counts.items() if c > 0)
      op = "Message" if "Message" in emitted else emitted[0]
      comp = script_compiler.compile_file(TEST_SCRIPTS, excluded_ops=[op])
      errors = comp.errors()
      self.assertTrue(errors)
      self.assertTrue(all(op in e.message and e.line > 0 for e in errors))
      self.assertEqual(len({(e.file, e.line) for e in errors}), len(errors))   # once per statement

   def test_option(self) -> None:
      """!--exclude reads the manifest; a bad one fails the compile."""
      with tempfile.TemporaryDirectory() as tmp:
         manifest = Path(tmp) / "exclude.json"
         manifest.write_text(json.dumps({"schema": ux.SCHEMA, "script_ops": [{"name": "End"}]}), encoding="utf-8")
         out = Path(tmp) / "scripts.cpp"
         with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as err:
            status = script_compiler.main(["x", str(TEST_SCRIPTS), str(out), "--exclude", str(manifest)])
         self.assertEqual(status, 1)
         self.assertIn("script op End is excluded", err.getvalue())
         manifest.write_text("{}", encoding="utf-8")
         with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as err:
            status = script_compiler.main(["x", str(TEST_SCRIPTS), str(out), "--exclude", str(manifest)])
         self.assertEqual(status, 1)
         self.assertIn(ux.SCHEMA, err.getvalue())


if __name__ == "__main__":
   unittest.main()
