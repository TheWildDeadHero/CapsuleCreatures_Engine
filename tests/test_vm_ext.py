#!/usr/bin/env python3
"""!Tests for tools/vm_ext.py (VM extension manifests): the schema's refusals, the checks, the generated
dispatch, the assemblers' layouts from manifests (tools/script_bank.py, tools/anim_bank.py) and the usage records.
Simple test content only (tests/data/vm_ext)."""
from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

## The engine's root.
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import anim_bank  # noqa: E402
import script_bank  # noqa: E402
import vm_ext  # noqa: E402

## The test manifests.
DATA = ROOT / "tests" / "data" / "vm_ext"
## The battle alias manifest.
BATTLE = DATA / "battle_alias_vm_ext.json"
## The animation alias manifest.
ANIM = DATA / "anim_alias_vm_ext.json"
## The sandbox's manifest.
SANDBOX = ROOT / "apps" / "sandbox_battle_vm" / "data" / "sandbox_vm_ext.json"


def raw_battle() -> dict[str, Any]:
   """!The battle manifest's JSON (a fresh copy to change).
   @return it
   """
   return copy.deepcopy(json.loads(BATTLE.read_text(encoding="utf-8")))


class ManifestTests(unittest.TestCase):
   """!Loading and the schema's refusals."""

   def write(self, folder: Path, name: str, raw: dict[str, Any]) -> Path:
      """!Writes a manifest.
      @param folder  the folder
      @param name    its file name
      @param raw     its JSON
      @return its path
      """
      path = folder / name
      path.write_text(json.dumps(raw), encoding="utf-8")
      return path

   def refused(self, raw: dict[str, Any], words: str) -> None:
      """!Checks that a manifest is refused with a message naming some words.
      @param raw    the JSON
      @param words  what the message says
      @return nothing
      """
      with tempfile.TemporaryDirectory() as tmp:
         with self.assertRaises(vm_ext.ManifestError) as caught:
            vm_ext.load(self.write(Path(tmp), "m.json", raw))
      self.assertIn(words, str(caught.exception))

   def test_loads(self) -> None:
      """!The test manifests load; sizes follow the layouts."""
      m = vm_ext.load(BATTLE)
      self.assertEqual(m.vm, "battle")
      self.assertEqual(m.ops[4].size, 11)   # jumpifbyte: id, u8, ref, u8, code
      self.assertEqual(m.ops[3].size, 1)
      self.assertEqual(sum(1 for op in m.ops.values() if op.kind == "alias"), 23)
      a = vm_ext.load(ANIM)
      self.assertEqual(vm_ext.anim_signatures([a])[0x42], ("ifturn", "ba"))

   def test_native_needs_reason(self) -> None:
      """!A native without a reason is an error."""
      raw = raw_battle()
      raw["ops"][-1]["reason"] = " "
      self.refused(raw, "reason")

   def test_alias_needs_known_step(self) -> None:
      """!Unknown steps, steps the VM does not run and wrong argument types are refused."""
      raw = raw_battle()
      raw["ops"][1]["expands"][0]["op"] = "teleport"
      self.refused(raw, "unknown step")
      raw = json.loads(ANIM.read_text(encoding="utf-8"))
      raw["ops"][1]["expands"] = [{"op": "call", "args": {"target": "to"}}]
      raw["ops"][1]["operands"] = [{"name": "to", "type": "code"}]
      self.refused(raw, "does not run step")
      raw = raw_battle()
      raw["ops"][7]["expands"][0]["args"]["ref"] = "value"
      self.refused(raw, "must be a ref")

   def test_space_and_clash(self) -> None:
      """!An op outside the space, and two manifests claiming the same opcodes, are errors."""
      raw = raw_battle()
      raw["ops"][0]["id"] = 40
      self.refused(raw, "outside the space")
      with tempfile.TemporaryDirectory() as tmp:
         a = self.write(Path(tmp), "a.json", raw_battle())
         b = self.write(Path(tmp), "b.json", raw_battle())
         with self.assertRaises(vm_ext.ManifestError) as caught:
            vm_ext.load_all([a, b])
      self.assertIn("both claim", str(caught.exception))

   def test_generic_anim_opcode_needs_override(self) -> None:
      """!An animation op at a generic opcode must say it overrides it."""
      raw = json.loads(ANIM.read_text(encoding="utf-8"))
      raw["ops"][0]["id"] = 2
      self.refused(raw, "override")
      raw["ops"][0]["override"] = True
      with tempfile.TemporaryDirectory() as tmp:
         vm_ext.load(self.write(Path(tmp), "m.json", raw))


class CheckTests(unittest.TestCase):
   """!§3.3's checks."""

   def test_sources(self) -> None:
      """!Every native is defined and every function of the prefix is declared."""
      ms = vm_ext.load_all([BATTLE])
      errors, warnings = vm_ext.check(ms, [ROOT / "tests" / "battle_vm_alias_tests.cpp"])
      self.assertEqual((errors, warnings), ([], []))
      with tempfile.TemporaryDirectory() as tmp:
         src = Path(tmp) / "x.cpp"
         src.write_text("void native_poke(vm::Machine& m, void*)\n{\n}\nvoid native_extra(vm::Machine& m, void*)\n"
                        "{\n}\n", encoding="utf-8")
         errors, _ = vm_ext.check(ms, [src])
         self.assertEqual(len(errors), 1)
         self.assertIn("native_extra", errors[0])
         src.write_text("void other(vm::Machine& m, void*)\n{\n}\n", encoding="utf-8")
         errors, _ = vm_ext.check(ms, [src])
         self.assertIn("native_poke is not defined", errors[0])

   def test_proven_candidate_warns(self) -> None:
      """!A native whose alias candidate has proven parity is a warning."""
      raw = raw_battle()
      raw["ops"][-1]["candidate"] = {"parity": "proven", "expands": [{"op": "nop", "args": {}}]}
      with tempfile.TemporaryDirectory() as tmp:
         path = Path(tmp) / "m.json"
         path.write_text(json.dumps(raw), encoding="utf-8")
         _, warnings = vm_ext.check(vm_ext.load_all([path]))
      self.assertEqual(len(warnings), 1)
      self.assertIn("proven alias candidate", warnings[0])

   def test_usage_declared(self) -> None:
      """!An op a bank emits must be declared."""
      ms = vm_ext.load_all([BATTLE])
      errors, _ = vm_ext.check(ms, usage={"battle": {"goto": 1, "warp": 2}})
      self.assertEqual(errors, ["usage: battle op 'warp' is emitted by a bank but not declared"])


class DispatchTests(unittest.TestCase):
   """!The generated dispatch."""

   def test_battle_fragment(self) -> None:
      """!Every id gets its alias function or native, in order; the text is stable."""
      with tempfile.TemporaryDirectory() as tmp:
         out = Path(tmp) / "d.inc"
         d = vm_ext.generate_dispatch(vm_ext.load_all([BATTLE]), out)
         again = vm_ext.generate_dispatch(vm_ext.load_all([BATTLE]), out)
      self.assertEqual(d.text, again.text)
      self.assertIn("alias_jumpifbyte,   // 0x04 jumpifbyte", d.text)
      self.assertIn("native_poke,   // 0x17", d.text)
      self.assertIn("constexpr vm::CommandTable kAliasTable = {kCommands, 24, nullptr, nullptr};", d.text)
      self.assertNotIn(str(ROOT), d.text)
      self.assertEqual(len(d.aliases), 23)

   def test_exclusions(self) -> None:
      """!With usage, ops no bank uses are left out and recorded."""
      with tempfile.TemporaryDirectory() as tmp:
         out = Path(tmp) / "d.inc"
         d = vm_ext.generate_dispatch(vm_ext.load_all([BATTLE]), out, {"battle": {"goto": 3, "poke": 1}})
      self.assertEqual(len(d.aliases), 1)
      self.assertIn("nullptr,   // 0x00 end (excluded: unused)", d.text)
      record = vm_ext.exclusion_record("battle", d.excluded)
      self.assertEqual(record["schema"], vm_ext.EXCLUSION_SCHEMA)
      self.assertEqual(len(record["excluded"]), 22)

   def test_anim_table(self) -> None:
      """!The animation table starts from the generic one and adds the game's commands."""
      with tempfile.TemporaryDirectory() as tmp:
         d = vm_ext.generate_dispatch(vm_ext.load_all([ANIM]), Path(tmp) / "a.inc")
      self.assertIn("anim_vm::CommandTable t = anim_vm::generic_table();", d.text)
      self.assertIn("t = anim_vm::with(t, 0x40, native_mark);   // mark", d.text)
      self.assertIn("t = anim_vm::with(t, 0x43, alias_choose);   // choose", d.text)

   def test_unsupported_vm(self) -> None:
      """!The VMs whose dispatch comes later are refused, not guessed."""
      raw = raw_battle()
      raw["vm"] = "effect"
      for op in raw["ops"]:
         op.pop("expands", None)
         op["kind"] = "native"
         op["symbol"] = "native_x" + str(op["id"])
         op["reason"] = "test"
      with tempfile.TemporaryDirectory() as tmp:
         path = Path(tmp) / "m.json"
         path.write_text(json.dumps(raw), encoding="utf-8")
         with self.assertRaises(vm_ext.ManifestError) as caught:
            vm_ext.generate_dispatch(vm_ext.load_all([path]), Path(tmp) / "d.inc")
      self.assertIn("not generated yet", str(caught.exception))

   def test_write_if_changed(self) -> None:
      """!Generated files are LF and written only on change."""
      with tempfile.TemporaryDirectory() as tmp:
         out = Path(tmp) / "sub" / "x.inc"
         vm_ext.write_if_changed(out, "a\nb\n")
         first = out.stat().st_mtime_ns
         vm_ext.write_if_changed(out, "a\nb\n")
         self.assertEqual(out.stat().st_mtime_ns, first)
         self.assertEqual(out.read_bytes(), b"a\nb\n")


class AssemblerTests(unittest.TestCase):
   """!The assemblers read the layouts from the manifests and record usage."""

   def test_script_bank_from_manifest(self) -> None:
      """!A source naming manifests assembles like one listing the same commands."""
      source = json.loads((DATA / "battle_alias_bank.json").read_text(encoding="utf-8"))
      counts: dict[str, int] = {}
      data, labels = script_bank.assemble(source, DATA, counts)
      listed = dict(source)
      del listed["manifests"]
      listed["commands"] = vm_ext.script_bank_commands(vm_ext.load_all([BATTLE]))
      self.assertEqual(script_bank.assemble(listed)[0], data)
      self.assertIn("Main", labels)
      self.assertEqual(counts["choose"], 2)
      both = dict(source)
      both["commands"] = []
      with self.assertRaises(script_bank.BankFormatError):
         script_bank.assemble(both, DATA)

   def test_sandbox_bank(self) -> None:
      """!The sandbox's bank assembles from its manifest."""
      source = json.loads((SANDBOX.parent / "sandbox_bank.json").read_text(encoding="utf-8"))
      data, _ = script_bank.assemble(source, SANDBOX.parent)
      self.assertEqual(script_bank.read_bank(data).code[0], 1)   # slide

   def test_anim_bank_from_manifest(self) -> None:
      """!A .canim's .manifest line declares the game opcodes with the manifest's signatures."""
      counts: dict[str, int] = {}
      data = anim_bank.assemble((DATA / "anim_alias_bank.canim").read_text(encoding="utf-8"), base=DATA,
                                usage=counts)
      bank = anim_bank.read_bank(data)
      self.assertEqual(bank.opcodes[0x43], ("choose", "aa"))
      self.assertEqual(counts["mark"], 12)
      self.assertNotIn(".manifest", counts)


if __name__ == "__main__":
   unittest.main()
