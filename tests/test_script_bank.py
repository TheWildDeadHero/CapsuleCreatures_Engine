#!/usr/bin/env python3
"""!Tests for tools/script_bank.py, the battle script machine's bank writer and reader (run by CTest, or directly)."""
from __future__ import annotations

import io
import json
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import script_bank  # noqa: E402

## The C++ tests' bank source.
TEST_BANK = ROOT / "tests" / "data" / "battle_vm" / "test_bank.json"


class ScriptBankTests(unittest.TestCase):
   """!The writer, the reader, the disassembler and the command line."""

   def test_writer_layout(self) -> None:
      """!Header, label table, names and code at the offsets vm.h reads."""
      w = script_bank.BankWriter()
      w.label("Start")
      w.emit("u8", 1)
      w.emit_label_ref("Later")
      w.label("Later")
      w.emit("u16", 0x1234)
      data = w.build()
      magic, version, count, code_at, code_size, labels_at, names_at = struct.unpack_from(
         script_bank.HEADER_FORMAT, data)
      self.assertEqual((magic, version, count), (b"CBVM", 1, 2))
      self.assertEqual(labels_at, script_bank.HEADER_BYTES)
      self.assertEqual(struct.unpack_from("<2I", data, labels_at), (0, 5))
      self.assertEqual(code_size, 7)
      self.assertEqual(data[code_at:], bytes([1, 5, 0, 0, 0, 0x34, 0x12]))
      (first_name,) = struct.unpack_from("<I", data, names_at)
      self.assertEqual(data[first_name:first_name + 6], b"Start\0")
      bank = script_bank.read_bank(data)
      self.assertEqual(bank.labels, ["Start", "Later"])
      self.assertEqual(bank.offsets, [0, 5])
      no_names = script_bank.read_bank(w.build(names=False))
      self.assertEqual(no_names.labels, ["", ""])

   def test_references(self) -> None:
      """!References encode and decode as vm.h's make_ref() / ref_kind() / ref_id() / ref_offset()."""
      r = script_bank.make_ref("table", 0x123, 0x4567)
      self.assertEqual(r, 0x21234567)
      self.assertEqual(script_bank.split_ref(r), (2, 0x123, 0x4567))
      self.assertEqual(script_bank.split_ref(script_bank.make_ref("code", 99)), (0, 99, 0))
      with self.assertRaises(script_bank.BankFormatError):
         script_bank.make_ref("variable", 0x1000)
      with self.assertRaises(script_bank.BankFormatError):
         script_bank.make_ref(16)

   def test_errors(self) -> None:
      """!Unplaced labels, labels placed twice, bad sources and banks are refused."""
      w = script_bank.BankWriter()
      w.emit_label_ref("Nowhere")
      with self.assertRaises(script_bank.BankFormatError):
         w.build()
      w = script_bank.BankWriter()
      w.label("A")
      with self.assertRaises(script_bank.BankFormatError):
         w.label("A")
      with self.assertRaises(script_bank.BankFormatError):
         w.emit("u8", 256)
      with self.assertRaises(script_bank.BankFormatError):
         script_bank.assemble({"schema": "other"})
      source = {"schema": script_bank.SOURCE_SCHEMA, "commands": [{"name": "x", "id": 0, "operands": ["u8"]}],
                "script": [["x"]]}
      with self.assertRaises(script_bank.BankFormatError):
         script_bank.assemble(source)
      source["commands"].append({"name": "y", "id": 0})
      with self.assertRaises(script_bank.BankFormatError):
         script_bank.assemble(source)
      for bad in (b"CBVM", b"XXXX" + bytes(20), struct.pack(script_bank.HEADER_FORMAT, b"CBVM", 2, 0, 0, 0, 0, 0)):
         with self.assertRaises(script_bank.BankFormatError):
            script_bank.read_bank(bad)

   def test_test_bank(self) -> None:
      """!The C++ tests' bank: its labels where the tests expect them; the disassembly names its targets."""
      source = json.loads(TEST_BANK.read_text(encoding="utf-8"))
      data, labels = script_bank.assemble(source)
      bank = script_bank.read_bank(data)
      offsets = dict(zip(labels, bank.offsets))
      self.assertEqual(offsets, {"Main": 0, "Twice": 32, "Data": 42, "Recurse": 37, "Unknown": 49, "End": 50})
      self.assertEqual(len(bank.code), 50)
      lines = script_bank.disassemble(bank, script_bank.command_set(source["commands"]))
      self.assertIn("   000002  call Twice", lines)
      self.assertIn("   000009  set ref(1, 2, 1), 7", lines)
      w = script_bank.BankWriter()
      w.label("Odd")
      w.emit("u8", 0xEE)
      odd = script_bank.disassemble(script_bank.read_bank(w.build()), script_bank.command_set(source["commands"]))
      self.assertEqual(odd, ["Odd:", "   000000  .byte 0xee  ; not a command of the set: stopped"])

   def test_cpp_and_cli(self) -> None:
      """!The C++ outputs and the command line's actions."""
      cpp = script_bank.to_cpp(bytes([1, 2, 3]), "kBankX", "ns")
      self.assertIn("alignas(4) const uint8_t kBankX[] = {\n   0x01, 0x02, 0x03\n};", cpp)
      self.assertIn("const uint32_t kBankXSize = sizeof(kBankX);", cpp)
      self.assertEqual(script_bank.cpp_name("Battle_script-1"), "kLabelBattleScript1")
      self.assertIn("inline constexpr uint16_t kLabelB = 1;   ///< B", script_bank.to_ids(["A", "B"], "ns"))
      with tempfile.TemporaryDirectory() as d:
         out = Path(d)
         self.assertEqual(script_bank.main(["asm", str(TEST_BANK), "--out", str(out / "b.bin"), "--cpp",
                                            str(out / "b.cpp"), "--ids", str(out / "b.h")]), 0)
         for action in (["info"], ["labels"], ["disasm", "--spec", str(TEST_BANK)]):
            buf = io.StringIO()
            with redirect_stdout(buf):
               self.assertEqual(script_bank.main([action[0], str(out / "b.bin")] + action[1:]), 0)
            self.assertTrue(buf.getvalue())
         self.assertEqual(script_bank.main(["cpp", str(out / "b.bin"), "--out", str(out / "c.cpp")]), 0)
         (out / "bad.bin").write_bytes(b"nope")
         self.assertEqual(script_bank.main(["info", str(out / "bad.bin")]), 1)


if __name__ == "__main__":
   unittest.main()
