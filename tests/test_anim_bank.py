#!/usr/bin/env python3
"""!Tests for tools/anim_bank.py (BF, D-38): the writer's layout (header, sections, symbols, opcodes, code), the
assembler and disassembler round trip, indexed symbols, argument lists, label fixups, the reader's checks and the
C++ output."""
from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import anim_bank  # noqa: E402

## The engine's test bank.
TEST_BANK = Path(__file__).resolve().parent / "data" / "anim" / "test_bank.canim"
## The sandbox's bank.
SHAPES_BANK = Path(__file__).resolve().parent.parent / "apps" / "sandbox_anim" / "data" / "shapes.canim"


class WriterTests(unittest.TestCase):
   """!BankWriter and the format."""

   def test_layout(self) -> None:
      """!Header fields, aligned sections, one command's bytes."""
      w = anim_bank.BankWriter()
      w.opcode(0x40, "shake", "bc")
      w.script("only")
      w.emit("create_sprite", "box", 3, [1, -2])
      w.emit("shake", 7, -1)
      w.emit("end")
      data = w.build(indexed=True)
      fields = struct.unpack_from(anim_bank.HEADER_FORMAT, data)
      self.assertEqual(fields[0], anim_bank.MAGIC)
      self.assertEqual(fields[1], anim_bank.VERSION)
      self.assertEqual(fields[2], anim_bank.FLAG_INDEXED)
      self.assertEqual(fields[3:6], (1, 1, 1))
      for offset in (fields[7], fields[8], fields[9], fields[10], fields[12]):   # the section offsets
         self.assertEqual(offset % anim_bank.SECTION_ALIGN, 0)
      bank = anim_bank.read_bank(data)
      self.assertEqual(bank.code[:1 + 2 + 1 + 1 + 4], bytes([9, 0, 0, 3, 2, 1, 0, 0xFE, 0xFF]))
      self.assertEqual(bank.code[9:12], bytes([0x40, 7, 0xFF]))
      self.assertEqual(bank.symbols, [(0, 0, "box")])
      self.assertEqual(bank.opcodes, {0x40: ("shake", "bc")})
      self.assertEqual(bank.scripts, [("only", 0)])
      self.assertEqual(anim_bank.check_bank(bank), [])

   def test_fixups_and_indices(self) -> None:
      """!Forward labels, explicit and assigned indices, names in index order."""
      w = anim_bank.BankWriter()
      w.symbol("task", "b", 1)
      w.script("s")
      w.emit("jump", "later")
      w.emit("create_task", "a", 0, [])
      w.label("later")
      w.emit("create_task", "b", 0, [])
      w.emit("end")
      data = w.build(indexed=True)
      bank = anim_bank.read_bank(data)
      self.assertEqual(struct.unpack_from("<I", bank.code, 1)[0], 5 + 5)
      self.assertEqual(bank.symbols, [(1, 1, "b"), (1, 0, "a")])
      self.assertEqual(w.names_by_kind()["task"], ["a", "b"])

   def test_errors(self) -> None:
      """!Refused inputs."""
      w = anim_bank.BankWriter()
      with self.assertRaises(anim_bank.BankError):
         w.opcode(0x20, "low", "")
      with self.assertRaises(anim_bank.BankError):
         w.opcode(0x41, "delay", "")
      with self.assertRaises(anim_bank.BankError):
         w.opcode(0x41, "bad", "q")
      with self.assertRaises(anim_bank.BankError):
         w.emit("delay", 256)
      with self.assertRaises(anim_bank.BankError):
         w.emit("create_task", "t", 0, list(range(9)))
      w.emit("jump", "nowhere")
      with self.assertRaises(anim_bank.BankError):
         w.build()


class TextTests(unittest.TestCase):
   """!The .canim assembler, the disassembler and the checks."""

   def test_round_trip(self) -> None:
      """!The engine's banks assemble, check clean and disassemble back to the same bytes."""
      for path in (TEST_BANK, SHAPES_BANK):
         data = anim_bank.assemble(path.read_text(encoding="utf-8"), indexed=True)
         bank = anim_bank.read_bank(data)
         self.assertEqual(anim_bank.check_bank(bank), [], path.name)
         again = anim_bank.assemble(anim_bank.disassemble(bank))
         self.assertEqual(again, data, path.name)

   def test_labels_like_numbers(self) -> None:
      """!A label spelled like hex digits is a label, a number a number."""
      data = anim_bank.assemble(".script s\n   call d1\n   end\nd1:\n   return\n")
      bank = anim_bank.read_bank(data)
      self.assertEqual(struct.unpack_from("<I", bank.code, 1)[0], 6)

   def test_check_finds_problems(self) -> None:
      """!An address off a command and a symbol of the wrong kind."""
      data = bytearray(anim_bank.assemble(".script s\n   create_task K:t 1 []\n   jump 1\n   end\n"))
      bank = anim_bank.read_bank(bytes(data))
      problems = anim_bank.check_bank(bank)
      self.assertEqual(len(problems), 1)
      self.assertIn("address 1", problems[0])
      bad = anim_bank.read_bank(bytes(data))
      bad.symbols[0] = (0, 0, "t")
      self.assertTrue(any("not a task" in p for p in anim_bank.check_bank(bad)))
      with self.assertRaises(anim_bank.BankError):
         anim_bank.read_bank(b"XXXX" + bytes(data[4:]))

   def test_cpp(self) -> None:
      """!The C++ output names the bytes and their count."""
      data = anim_bank.assemble(".script s\n   end\n")
      text = anim_bank.to_cpp(data, "kBank")
      self.assertIn("alignas(4) extern const uint8_t kBank[]", text)
      self.assertIn(f"kBank_size = {len(data)};", text)


if __name__ == "__main__":
   unittest.main()
