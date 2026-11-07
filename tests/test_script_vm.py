#!/usr/bin/env python3
"""!Tests for the field script VM's opcode table (D-36 package 2, step A; tools/script_vm.py): the manifest loads
through the shared vm_ext checks, its numbering is the bytecode's, the generated header is up to date, and the script
compiler's tables are the manifest's."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

## The engine's root.
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import script_compiler as sc  # noqa: E402
import script_vm  # noqa: E402
import vm_ext  # noqa: E402

## Opcodes the bytecode has numbered so far, by name: the first, the start of the imported block and the last
## (renumbering any of them would change every bank).
PINNED = {"End": 0, "Native": 20, "Nursery": 65, "Jump": 66, "Override": 86, "Battle": 88}


class Table(unittest.TestCase):
   """The manifest and what reads it."""

   def test_numbering(self) -> None:
      """Ids run 0 to n - 1; the pinned opcodes keep their numbers; every op is a generic op."""
      t = script_vm.table()
      for name, ident in PINNED.items():
         self.assertEqual(t.ops.index(name), ident, name)
      self.assertTrue(all(o["kind"] == "generic" for o in t.raw["ops"]))

   def test_header_up_to_date(self) -> None:
      """include/capsule/script/ops.h is what the manifest generates."""
      self.assertEqual(script_vm.main(["header", "--check"]), 0)

   def test_compiler_reads_it(self) -> None:
      """The compiler's opcode list, kind words and constants come from the manifest."""
      t = script_vm.table()
      self.assertIs(sc.OPS, t.ops)
      self.assertEqual(sc.OP["Counter"], len(t.ops) - 1)   # the last op (C5)
      self.assertEqual(sc.TRANSITIONS, t.words("Transition"))
      self.assertEqual(sc.SOUND_KINDS, t.word_values("SoundOp", "operands"))
      self.assertEqual(sc.OBJECT_PLAYER, t.constants["kObjectPlayer"])

   def test_refused(self) -> None:
      """A gap in the numbering and a generic op in another VM's manifest are refused."""
      raw = json.loads(script_vm.MANIFEST.read_text(encoding="utf-8"))
      raw["ops"] = [o for o in raw["ops"] if o["name"] != "Native"]
      with self.assertRaises(script_vm.ScriptVmError):
         script_vm.ScriptVm(raw)
      other = {"schema": vm_ext.SCHEMA, "vm": "battle", "ops": [{"id": 1, "name": "x", "kind": "generic"}]}
      with self.assertRaises(vm_ext.ManifestError):
         vm_ext._manifest_from(other, Path("other.json"))

   def test_dispatch(self) -> None:
      """The default dispatch table is up to date and holds every module op; a game's table holds only the module
      ops its scripts use and the waits those ops set."""
      self.assertEqual(script_vm.main(["dispatch", "--check"]), 0)
      every = script_vm.DISPATCH_ALL.read_text(encoding="utf-8")
      self.assertIn("op_combine,", every)
      self.assertIn("= poll_transform;", every)
      with tempfile.TemporaryDirectory() as tmp:
         record = Path(tmp) / "scripts.json"
         record.write_text(json.dumps({"schema": script_vm.USAGE_RECORD_SCHEMA, "game": "g", "source": "script",
                                       "used": {script_vm.OP_CATEGORY: {"End": 3, "BagOpen": 1, "Message": 2}}}),
                           encoding="utf-8")
         out = Path(tmp) / "script_dispatch.cpp"
         self.assertEqual(script_vm.main(["dispatch", "--usage", str(record), "--out", str(out)]), 0)
         text = out.read_text(encoding="utf-8")
         self.assertIn("op_bag_open,", text)
         self.assertIn("= poll_bag;", text)
         for absent in ("op_combine", "op_transform", "op_role_get", "poll_transform", "poll_shop"):
            self.assertNotIn(absent, text)

   def test_stale_header(self) -> None:
      """--check reports a header that differs from the manifest's."""
      with tempfile.TemporaryDirectory() as tmp:
         out = Path(tmp) / "ops.h"
         out.write_text("// stale\n", encoding="utf-8")
         self.assertEqual(script_vm.main(["header", "--out", str(out), "--check"]), script_vm.EXIT_STALE)
         self.assertEqual(script_vm.main(["header", "--out", str(out)]), 0)
         self.assertEqual(script_vm.main(["header", "--out", str(out), "--check"]), 0)


if __name__ == "__main__":
   unittest.main()
