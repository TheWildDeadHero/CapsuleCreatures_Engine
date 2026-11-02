#!/usr/bin/env python3
"""!Writes and reads script banks for the battle script machine (include/capsule/battle/vm.h, M5 BX).

A bank holds a header, a label table, an optional name table and the code (a stream of one-byte command ids and
their little-endian operands; pointer operands are references: a code offset, or a game variable / table by id and
byte offset). The library (BankWriter, read_bank(), disassemble(), to_cpp()) is what a converter that writes banks
uses; the command line assembles a bank source, prints a bank's header and labels, disassembles it with a command
spec, and writes a bank as a C++ source:

    script_bank.py asm SOURCE.json --out BANK.bin [--cpp OUT.cpp --symbol NAME --namespace NS] [--ids OUT.h]
    script_bank.py info BANK.bin
    script_bank.py labels BANK.bin
    script_bank.py disasm BANK.bin --spec SOURCE.json
    script_bank.py cpp BANK.bin --out OUT.cpp --symbol NAME --namespace NS

A bank source (capsule.script-bank/1) is JSON: "commands" lists the command set (name, id, operand kinds: u8, s8,
u16, s16, u32, code (a label's name), ref (a reference: {"kind", "id", "offset"} or a code label's name), and
"script" the code: "label:NAME" strings and lists [command, operand...]; the raw items ["u8", v], ["u16", v],
["u32", v] and ["code", label] put data inside the code. Instead of "commands", a source may name the VM extension
manifests that declare its command set (D-39: "manifests", paths relative to the source; tools/vm_ext.py), so the
operand layouts live in one place. `asm --usage OUT.json` records how often the bank emits each command
(capsule.vm-usage/1, for the generated dispatch's exclusions).
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

## The bank's magic, "CBVM".
BANK_MAGIC = b"CBVM"
## The format version (vm.h kBankVersion).
BANK_VERSION = 1
## Header bytes (vm.h kHeaderBytes).
HEADER_BYTES = 24
## The header's layout: magic, version, label count, code offset, code size, label table, name table.
HEADER_FORMAT = "<4sHHIIII"
## Bytes of one label or name table entry.
ENTRY_BYTES = 4
## The schema of a bank source.
SOURCE_SCHEMA = "capsule.script-bank/1"
## The schema of a usage record (tools/vm_ext.py USAGE_SCHEMA).
USAGE_SCHEMA = "capsule.vm-usage/1"
## Indent of a usage record's JSON.
USAGE_INDENT = 2
## A label item of a source's script.
LABEL_PREFIX = "label:"

## Reference kinds (vm.h RefKind).
REF_KINDS = {"code": 0, "variable": 1, "table": 2, "value": 3}
## A reference's kind starts at this bit.
REF_KIND_SHIFT = 28
## A variable or table id starts at this bit.
REF_ID_SHIFT = 16
## The largest variable or table id.
REF_ID_MAX = 0xFFF
## The largest byte offset.
REF_OFFSET_MAX = 0xFFFF
## The largest code offset or value.
REF_PAYLOAD_MAX = (1 << REF_KIND_SHIFT) - 1
## Game kinds go up to this one.
REF_KIND_MAX = 15

## Operand kinds and their struct formats (code and ref are 4-byte references).
OPERAND_FORMATS = {"u8": "<B", "s8": "<b", "u16": "<H", "s16": "<h", "u32": "<I", "code": "<I", "ref": "<I"}
## The largest command id.
COMMAND_ID_MAX = 0xFF
## Bytes per line of a C++ byte array.
CPP_BYTES_PER_LINE = 16
## Characters that may stay in a C++ name made from a label.
CPP_NAME = re.compile(r"[^0-9A-Za-z]+")


class BankFormatError(ValueError):
   """!A bank or a bank source that does not follow the format."""


def make_ref(kind: int | str, ident: int = 0, offset: int = 0) -> int:
   """!Encodes a reference.
   @param kind    a REF_KINDS name or a number 0-15
   @param ident   a variable or table id (0-4095), or for code / value the payload
   @param offset  a byte offset (0-65535; variables and tables)
   @return the 32-bit reference
   @throws BankFormatError on an out-of-range field
   """
   k = REF_KINDS[kind] if isinstance(kind, str) else kind
   if not 0 <= k <= REF_KIND_MAX:
      raise BankFormatError(f"reference kind {kind!r} out of range")
   if k in (REF_KINDS["code"], REF_KINDS["value"]):
      if not 0 <= ident <= REF_PAYLOAD_MAX:
         raise BankFormatError(f"reference payload {ident} out of range")
      return (k << REF_KIND_SHIFT) | ident
   if not 0 <= ident <= REF_ID_MAX or not 0 <= offset <= REF_OFFSET_MAX:
      raise BankFormatError(f"reference id {ident} or offset {offset} out of range")
   return (k << REF_KIND_SHIFT) | (ident << REF_ID_SHIFT) | offset


def split_ref(ref: int) -> tuple[int, int, int]:
   """!Decodes a reference.
   @param ref  the 32-bit reference
   @return (kind, id or payload, offset): code and value references give their payload and offset 0
   """
   kind = ref >> REF_KIND_SHIFT
   if kind in (REF_KINDS["code"], REF_KINDS["value"]):
      return kind, ref & REF_PAYLOAD_MAX, 0
   return kind, (ref >> REF_ID_SHIFT) & REF_ID_MAX, ref & REF_OFFSET_MAX


class BankWriter:
   """!Builds a bank: labels, then bytes and references; label references resolve at build()."""

   def __init__(self) -> None:
      """!An empty bank."""
      ## The code so far.
      self.code = bytearray()
      ## Label names in id order.
      self.labels: list[str] = []
      ## Each label's code offset (None until placed).
      self.offsets: dict[str, int | None] = {}
      ## Code offsets of 4-byte label references to fill, with their labels.
      self.fixups: list[tuple[int, str]] = []

   def declare(self, name: str) -> int:
      """!Gives a label its id without placing it (forward references, a fixed id order).
      @param name  the label
      @return its id
      """
      if name not in self.offsets:
         self.offsets[name] = None
         self.labels.append(name)
      return self.labels.index(name)

   def label(self, name: str) -> int:
      """!Places a label at the current code offset.
      @param name  the label
      @return its id
      @throws BankFormatError when it was placed already
      """
      ident = self.declare(name)
      if self.offsets[name] is not None:
         raise BankFormatError(f"label {name!r} placed twice")
      self.offsets[name] = len(self.code)
      return ident

   def emit(self, kind: str, value: int) -> None:
      """!Appends an operand or a data item.
      @param kind   an OPERAND_FORMATS kind (code and ref take the reference's number)
      @param value  its value
      @return nothing
      @throws BankFormatError on an unknown kind or a value out of its range
      """
      if kind not in OPERAND_FORMATS:
         raise BankFormatError(f"unknown operand kind {kind!r}")
      try:
         self.code += struct.pack(OPERAND_FORMATS[kind], value)
      except struct.error as e:
         raise BankFormatError(f"{kind} {value}: {e}") from None

   def emit_label_ref(self, name: str) -> None:
      """!Appends a code reference to a label (resolved at build()).
      @param name  the label
      @return nothing
      """
      self.declare(name)
      self.fixups.append((len(self.code), name))
      self.code += bytes(ENTRY_BYTES)

   def build(self, names: bool = True) -> bytes:
      """!The bank.
      @param names  include the name table
      @return its bytes
      @throws BankFormatError when a referenced or declared label was never placed
      """
      code = bytearray(self.code)
      fixups = self.fixups
      labels = self.labels
      for at, name in fixups:
         target = self.offsets.get(name)
         if target is None:
            raise BankFormatError(f"label {name!r} is referenced but never placed")
         code[at:at + ENTRY_BYTES] = struct.pack("<I", make_ref("code", target))
      missing = [n for n in labels if self.offsets[n] is None]
      if missing:
         raise BankFormatError(f"labels never placed: {', '.join(missing)}")
      count = len(labels)
      labels_at = HEADER_BYTES
      names_at = labels_at + count * ENTRY_BYTES if names else 0
      text = bytearray()
      name_offsets: list[int] = []
      text_at = (names_at + count * ENTRY_BYTES) if names else labels_at + count * ENTRY_BYTES
      if names:
         for n in labels:
            name_offsets.append(text_at + len(text))
            text += n.encode("ascii") + b"\0"
      code_at = text_at + len(text)
      header = struct.pack(HEADER_FORMAT, BANK_MAGIC, BANK_VERSION, count, code_at, len(code), labels_at, names_at)
      table = b"".join(struct.pack("<I", self.offsets[n]) for n in labels)
      name_table = b"".join(struct.pack("<I", o) for o in name_offsets)
      return header + table + name_table + bytes(text) + bytes(code)


@dataclass
class Bank:
   """!A bank read back."""

   ## The code.
   code: bytes
   ## Label names (empty strings without a name table), in id order.
   labels: list[str]
   ## Each label's code offset, in id order.
   offsets: list[int]


def read_bank(data: bytes) -> Bank:
   """!Reads a bank (the checks of vm.h's open()).
   @param data  its bytes
   @return it
   @throws BankFormatError when it is not a valid bank
   """
   if len(data) < HEADER_BYTES:
      raise BankFormatError("shorter than a bank's header")
   magic, version, count, code_at, code_size, labels_at, names_at = struct.unpack_from(HEADER_FORMAT, data)
   if magic != BANK_MAGIC:
      raise BankFormatError("not a bank (bad magic)")
   if version != BANK_VERSION:
      raise BankFormatError(f"bank version {version}, expected {BANK_VERSION}")
   if code_at + code_size > len(data) or labels_at + count * ENTRY_BYTES > len(data):
      raise BankFormatError("a table or the code lies outside the bank")
   offsets = list(struct.unpack_from(f"<{count}I", data, labels_at))
   if any(o > code_size for o in offsets):
      raise BankFormatError("a label lies outside the code")
   names = [""] * count
   if names_at:
      for i, at in enumerate(struct.unpack_from(f"<{count}I", data, names_at)):
         end = data.index(b"\0", at)
         names[i] = data[at:end].decode("ascii")
   return Bank(bytes(data[code_at:code_at + code_size]), names, offsets)


def command_set(commands: Sequence[dict[str, Any]]) -> dict[str, dict[str, Any]]:
   """!Checks a source's command list.
   @param commands  its entries (name, id, operands)
   @return the entries by name
   @throws BankFormatError on a duplicate name or id, an id out of range or an unknown operand kind
   """
   by_name: dict[str, dict[str, Any]] = {}
   ids: set[int] = set()
   for c in commands:
      name, ident, operands = c["name"], c["id"], c.get("operands", [])
      if name in by_name or ident in ids or not 0 <= ident <= COMMAND_ID_MAX:
         raise BankFormatError(f"command {name!r} (id {ident}) is a duplicate or out of range")
      for o in operands:
         if o not in OPERAND_FORMATS:
            raise BankFormatError(f"command {name!r}: unknown operand kind {o!r}")
      by_name[name] = {"id": ident, "operands": list(operands)}
      ids.add(ident)
   return by_name


def emit_operand(writer: BankWriter, kind: str, value: Any) -> None:
   """!Appends one operand of a source line.
   @param writer  the bank being built
   @param kind    its kind
   @param value   a number (code: a code offset), a label name (code / ref) or a reference object (ref)
   @return nothing
   """
   if kind == "code" and isinstance(value, int):
      writer.emit("code", make_ref("code", value))   # a code offset given as a number (0: none, a NULL target)
   elif kind == "code" or (kind == "ref" and isinstance(value, str)):
      writer.emit_label_ref(value)
   elif kind == "ref" and isinstance(value, dict):
      writer.emit("ref", make_ref(value["kind"], value.get("id", 0), value.get("offset", 0)))
   else:
      writer.emit(kind, int(value))


def manifest_commands(paths: Sequence[Path]) -> list[dict[str, Any]]:
   """!A command list read from VM extension manifests (tools/vm_ext.py, which lives beside this tool).
   @param paths  the manifests (battle VM)
   @return the command list (command_set()'s input)
   @throws BankFormatError when a manifest does not load
   """
   sys.path.insert(0, str(Path(__file__).resolve().parent))
   try:
      import vm_ext   # pylint: disable=import-outside-toplevel
   finally:
      sys.path.pop(0)
   try:
      return vm_ext.script_bank_commands(vm_ext.load_all(paths))
   except vm_ext.ManifestError as e:
      raise BankFormatError(str(e)) from None


def source_commands(source: dict[str, Any], base: Path | None = None) -> list[dict[str, Any]]:
   """!A source's command list: its "commands", or the commands of the manifests it names.
   @param source  the parsed JSON (capsule.script-bank/1)
   @param base    the folder its manifest paths are relative to (default: the current folder)
   @return the command list
   @throws BankFormatError when it has both or a manifest does not load
   """
   if "manifests" in source:
      if "commands" in source:
         raise BankFormatError("a source gives either commands or manifests, not both")
      folder = base if base is not None else Path(".")
      return manifest_commands([folder / m for m in source["manifests"]])
   return list(source.get("commands", []))


def assemble(source: dict[str, Any], base: Path | None = None,
             usage: dict[str, int] | None = None) -> tuple[bytes, list[str]]:
   """!Assembles a bank source.
   @param source  the parsed JSON (capsule.script-bank/1)
   @param base    the folder its manifest paths are relative to
   @param usage   receives each command's emit count, or None
   @return (the bank, its label names in id order)
   @throws BankFormatError on a schema, command or operand error
   """
   if source.get("schema") != SOURCE_SCHEMA:
      raise BankFormatError(f"schema must be {SOURCE_SCHEMA}")
   commands = command_set(source_commands(source, base))
   w = BankWriter()
   for item in source.get("script", []):
      if isinstance(item, str) and item.startswith(LABEL_PREFIX):
         w.label(item[len(LABEL_PREFIX):])
         continue
      if not isinstance(item, list) or not item:
         raise BankFormatError(f"script item {item!r} is neither a label nor a list")
      head, args = item[0], item[1:]
      if head in OPERAND_FORMATS and head not in commands:   # raw data inside the code
         for a in args:
            emit_operand(w, head, a)
         continue
      if head not in commands:
         raise BankFormatError(f"unknown command {head!r}")
      c = commands[head]
      if usage is not None:
         usage[head] = usage.get(head, 0) + 1
      if len(args) != len(c["operands"]):
         raise BankFormatError(f"{head}: {len(c['operands'])} operands expected, {len(args)} given")
      w.emit("u8", c["id"])
      for kind, a in zip(c["operands"], args):
         emit_operand(w, kind, a)
   return w.build(), list(w.labels)


def disassemble(bank: Bank, commands: dict[str, dict[str, Any]]) -> list[str]:
   """!Lists a bank's code with a command set (stops at an id the set does not have: data or a variable-length
   command follows).
   @param bank      the bank
   @param commands  the command set (command_set())
   @return one line per label and command
   """
   by_id = {c["id"]: (name, c["operands"]) for name, c in commands.items()}
   at_offset: dict[int, list[str]] = {}
   for name, offset in zip(bank.labels, bank.offsets):
      at_offset.setdefault(offset, []).append(name or f"label_{offset:x}")
   lines: list[str] = []
   ip = 0
   while ip < len(bank.code):
      for name in at_offset.get(ip, []):
         lines.append(f"{name}:")
      ident = bank.code[ip]
      if ident not in by_id:
         lines.append(f"   {ip:06x}  .byte 0x{ident:02x}  ; not a command of the set: stopped")
         break
      name, operands = by_id[ident]
      at = ip + 1
      if at + sum(struct.calcsize(OPERAND_FORMATS[k]) for k in operands) > len(bank.code):
         lines.append(f"   {ip:06x}  .byte 0x{ident:02x}  ; operands past the code (data): stopped")
         break
      shown: list[str] = []
      for kind in operands:
         (value,) = struct.unpack_from(OPERAND_FORMATS[kind], bank.code, at)
         at += struct.calcsize(OPERAND_FORMATS[kind])
         if kind in ("code", "ref"):
            k, ident_or_payload, offset = split_ref(value)
            targets = [n for n, o in zip(bank.labels, bank.offsets) if o == ident_or_payload and k == 0]
            shown.append(targets[0] if targets and targets[0] else
                         f"ref({k}, {ident_or_payload}, {offset})" if k else f"@{ident_or_payload:x}")
         else:
            shown.append(str(value))
      lines.append(f"   {ip:06x}  {name} {', '.join(shown)}".rstrip())
      ip = at
   return lines


def cpp_name(label: str) -> str:
   """!A C++ constant name for a label (kLabel + its words).
   @param label  the label
   @return the name
   """
   words = [w for w in CPP_NAME.split(label) if w]
   return "kLabel" + "".join(w[:1].upper() + w[1:] for w in words)


def to_cpp(data: bytes, symbol: str, namespace: str) -> str:
   """!A C++ source defining a bank as a byte array (4-byte aligned) and its size.
   @param data       the bank
   @param symbol     the array's name (the size is the same name with "Size" added)
   @param namespace  their namespace
   @return the source
   """
   rows = [", ".join(f"0x{b:02x}" for b in data[i:i + CPP_BYTES_PER_LINE])
           for i in range(0, len(data), CPP_BYTES_PER_LINE)]
   body = ",\n   ".join(rows)
   return (f"// Generated by tools/script_bank.py: a script bank (include/capsule/battle/vm.h).\n"
           f"#include <cstdint>\n\nnamespace {namespace}\n{{\n\n"
           f"extern const uint8_t {symbol}[];\nextern const uint32_t {symbol}Size;\n"
           f"alignas(4) const uint8_t {symbol}[] = {{\n   {body}\n}};\n"
           f"const uint32_t {symbol}Size = sizeof({symbol});\n\n}}   // namespace {namespace}\n")


def to_ids(labels: Sequence[str], namespace: str) -> str:
   """!A C++ header of label ids.
   @param labels     label names in id order
   @param namespace  the namespace
   @return the header
   """
   lines = [f"inline constexpr uint16_t {cpp_name(n)} = {i};   ///< {n}" for i, n in enumerate(labels)]
   return ("/// @file\n/// Generated by tools/script_bank.py: a script bank's label ids.\n#pragma once\n\n"
           f"#include <cstdint>\n\nnamespace {namespace}\n{{\n\n" + "\n".join(lines) +
           f"\n\n}}   // namespace {namespace}\n")



def write_if_changed(path: Path, content: str | bytes) -> None:
   """!Writes a generated file only when its content changes (keeps builds incremental), creating its folder;
   text is written as UTF-8 with LF line ends on every system (D-40 RW3: the same bytes from every host).
   @param path     file
   @param content  text or bytes
   @return None
   """
   data = content.encode("utf-8") if isinstance(content, str) else content
   path.parent.mkdir(parents=True, exist_ok=True)
   if not path.exists() or path.read_bytes() != data:
      path.write_bytes(data)


def main(argv: Sequence[str] | None = None) -> int:
   """!The command line (see the module's comment).
   @param argv  arguments (default: sys.argv[1:])
   @return the exit status
   """
   p = argparse.ArgumentParser(description="Script banks of the battle script machine")
   sub = p.add_subparsers(dest="action", required=True)
   a = sub.add_parser("asm", help="assemble a bank source")
   a.add_argument("source", type=Path)
   a.add_argument("--out", type=Path, help="the bank's binary")
   a.add_argument("--cpp", type=Path, help="the bank as a C++ source")
   a.add_argument("--ids", type=Path, help="its label ids as a C++ header")
   a.add_argument("--usage", type=Path, help="the commands it emits (capsule.vm-usage/1)")
   a.add_argument("--bank-name", help="the bank's name in the usage record (default: the source's stem)")
   a.add_argument("--symbol", default="kBank")
   a.add_argument("--namespace", default="bank")
   for name in ("info", "labels"):
      sub.add_parser(name).add_argument("bank", type=Path)
   d = sub.add_parser("disasm", help="list a bank's code")
   d.add_argument("bank", type=Path)
   d.add_argument("--spec", type=Path, required=True, help="a bank source whose commands to use")
   c = sub.add_parser("cpp", help="write a bank as a C++ source")
   c.add_argument("bank", type=Path)
   c.add_argument("--out", type=Path, required=True)
   c.add_argument("--symbol", default="kBank")
   c.add_argument("--namespace", default="bank")
   args = p.parse_args(argv)
   try:
      if args.action == "asm":
         counts: dict[str, int] = {}
         data, labels = assemble(json.loads(args.source.read_text(encoding="utf-8")), args.source.parent, counts)
         if args.usage:
            record = {"schema": USAGE_SCHEMA, "vm": "battle", "bank": args.bank_name or args.source.stem,
                      "ops": {k: counts[k] for k in sorted(counts)}}
            write_if_changed(args.usage, json.dumps(record, indent=USAGE_INDENT) + "\n")
         if args.out:
            write_if_changed(args.out, data)
         if args.cpp:
            write_if_changed(args.cpp, to_cpp(data, args.symbol, args.namespace))
         if args.ids:
            write_if_changed(args.ids, to_ids(labels, args.namespace))
         return 0
      data = args.bank.read_bytes()
      bank = read_bank(data)
      if args.action == "info":
         print(f"bank: {len(data)} bytes, code {len(bank.code)} bytes, {len(bank.labels)} labels, "
               f"names {'yes' if any(bank.labels) else 'no'}")
      elif args.action == "labels":
         for i, (name, offset) in enumerate(zip(bank.labels, bank.offsets)):
            print(f"{i:5d}  {offset:06x}  {name}")
      elif args.action == "disasm":
         spec = json.loads(args.spec.read_text(encoding="utf-8"))
         print("\n".join(disassemble(bank, command_set(source_commands(spec, args.spec.parent)))))
      else:
         write_if_changed(args.out, to_cpp(data, args.symbol, args.namespace))
   except (BankFormatError, OSError, KeyError, json.JSONDecodeError) as e:
      print(f"script_bank: {e}", file=sys.stderr)
      return 1
   return 0


if __name__ == "__main__":
   sys.exit(main())
