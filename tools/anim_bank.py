#!/usr/bin/env python3
"""!BF (D-38): writes, reads, checks and disassembles animation script banks (include/capsule/anim/bank.h,
docs/ANIM_SCRIPT.md). Standard library only, so converters in other repositories can import it.

   anim_bank.py assemble IN.canim OUT.bin [--indexed] [--cpp NAME]
                                                         a .canim text file -> a bank (--cpp: a C++ source that
                                                         defines `NAME` (the bytes) and `NAME_size`)
   anim_bank.py disasm BANK.bin                          a bank -> .canim text (it assembles back to the same bank)
   anim_bank.py check BANK.bin                           exits 1 and lists the problems of a bad bank

The Python API: BankWriter (symbol(), opcode(), script(), label(), emit(), build()), read_bank(), decode(),
disassemble(), check_bank(), assemble().
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

## The schema of a usage record (tools/vm_ext.py USAGE_SCHEMA).
USAGE_SCHEMA = "capsule.vm-usage/1"
## Indent of a usage record's JSON.
USAGE_INDENT = 2
## "CANM" as a little-endian u32.
MAGIC = 0x4D4E4143
## Format version.
VERSION = 1
## Bytes of the header.
HEADER_BYTES = 44
## struct layout of the header: magic, version, flags, scripts, symbols, opcodes, reserved, 7 offsets / sizes.
HEADER_FORMAT = "<IHHHHHHIIIIIII"
## Header flag: every symbol carries its game index.
FLAG_INDEXED = 1
## Sections start on a multiple of this.
SECTION_ALIGN = 4
## Bytes of a script entry.
SCRIPT_ENTRY_BYTES = 8
## Bytes of a symbol.
SYMBOL_BYTES = 8
## Bytes of an opcode entry.
OPCODE_BYTES = 12
## A string offset that names nothing.
NO_NAME = 0xFFFFFFFF
## A symbol index before binding.
UNBOUND = 0xFFFF
## Arguments a command passes at most.
MAX_ARGS = 8
## First game opcode.
FIRST_GAME_OPCODE = 0x40
## Last opcode.
LAST_OPCODE = 0xFF
## Largest u8.
U8_MAX = 0xFF
## Largest u16.
U16_MAX = 0xFFFF
## Largest u32.
U32_MAX = 0xFFFFFFFF
## Smallest s8.
S8_MIN = -0x80
## Largest s8.
S8_MAX = 0x7F
## Smallest s16.
S16_MIN = -0x8000
## Largest s16.
S16_MAX = 0x7FFF
## Field of a read symbol tuple (kind, index, name) holding the name.
SYMBOL_NAME_FIELD = 2
## Bytes per line of a C++ array.
CPP_BYTES_PER_LINE = 16
## Opcode column width in a disassembly's hex comments.
HEX_WIDTH = 4

## Symbol kinds in bank order (SymbolKind) and their operand letters.
KINDS = ("template", "task", "sound_task", "callback", "data")
## Operand letter of each kind.
KIND_LETTERS = {"T": 0, "K": 1, "S": 2, "C": 3, "D": 4}
## Bytes of each fixed-size operand letter.
OPERAND_BYTES = {"b": 1, "c": 1, "h": 2, "s": 2, "w": 4, "a": 4, "T": 2, "K": 2, "S": 2, "C": 2, "D": 2}
## struct codes of the fixed operands.
OPERAND_CODES = {"b": "<B", "c": "<b", "h": "<H", "s": "<h", "w": "<I", "a": "<I", "T": "<H", "K": "<H", "S": "<H",
                 "C": "<H", "D": "<H"}
## Argument list letter.
ARGS = "v"
## Address letter.
ADDRESS = "a"

## The engine's generic commands in opcode order: name, signature (include/capsule/anim/bank.h Op).
GENERIC_OPS: tuple[tuple[str, str], ...] = (
   ("end", ""), ("delay", "b"), ("wait_visual", ""), ("wait_sound", ""), ("jump", "a"), ("call", "a"),
   ("return", ""), ("set_arg", "bs"), ("jump_if_arg_eq", "bsa"), ("create_sprite", "Tbv"),
   ("create_task", "Kbv"), ("create_sound_task", "Sbv"), ("load_gfx", "h"), ("unload_gfx", "h"),
   ("play_se", "hc"), ("set_blend", "bbbbb"), ("blend_off", ""), ("bg_show", "bb"), ("bg_scroll", "bss"),
   ("nop", ""),
)
## Generic command names -> opcode.
GENERIC_BY_NAME = {name: op for op, (name, _sig) in enumerate(GENERIC_OPS)}


class BankError(ValueError):
   """!A bank or a .canim text that cannot be written or read."""


def _align(n: int) -> int:
   """!Rounds up to the section alignment.
   @param n  a byte count
   @return n rounded up to a multiple of SECTION_ALIGN
   """
   return (n + SECTION_ALIGN - 1) // SECTION_ALIGN * SECTION_ALIGN


def _check_signature(signature: str) -> None:
   """!Refuses a signature with unknown letters (raises BankError).
   @param signature  operand letters
   @return nothing
   """
   for letter in signature:
      if letter not in OPERAND_BYTES and letter != ARGS:
         raise BankError(f"unknown operand letter {letter!r} in signature {signature!r}")


@dataclass
class _Symbol:
   """!A symbol being written."""

   ## Kind number (KINDS index).
   kind: int
   ## Name.
   name: str
   ## Game index, or None (assigned at build).
   index: int | None


class AnimBankWriter:
   """!Builds a bank: symbols, game opcodes, scripts, labels and commands, then build() writes the bytes."""

   def __init__(self) -> None:
      """!An empty bank."""
      ## Symbols in bank order.
      self._symbols: list[_Symbol] = []
      ## (kind, name) -> symbol position.
      self._symbol_at: dict[tuple[int, str], int] = {}
      ## Game opcodes: opcode -> (name, signature).
      self._opcodes: dict[int, tuple[str, str]] = {}
      ## Game opcode names -> opcode.
      self._opcode_by_name: dict[str, int] = {}
      ## Script entries: (name, label of the first command).
      self._scripts: list[tuple[str, str]] = []
      ## Label name -> code address.
      self.labels: dict[str, int] = {}
      ## The code written so far.
      self.code = bytearray()
      ## Address operands to fill: (position in code, label).
      self._fixups: list[tuple[int, str]] = []

   # --- declarations -------------------------------------------------------------------------------------------

   def symbol(self, kind: str | int, name: str, index: int | None = None) -> int:
      """!A symbol (made on first use; a later call may give its index).
      @param kind   a KINDS name or number
      @param name   its name
      @param index  its game table index, or None
      @return its position in the bank's symbol table
      """
      symbols = self._symbols   # local names (Doxygen 1.9.8 reads some self.x uses as new members)
      k = KINDS.index(kind) if isinstance(kind, str) else kind
      if not 0 <= k < len(KINDS):
         raise BankError(f"unknown symbol kind {kind!r}")
      key = (k, name)
      if key in self._symbol_at:
         pos = self._symbol_at[key]
         if index is not None:
            known = symbols[pos].index
            if known is not None and known != index:
               raise BankError(f"symbol {name!r} given two indices ({known}, {index})")
            symbols[pos].index = index
         return pos
      if len(symbols) > U16_MAX:
         raise BankError("too many symbols")
      self._symbol_at[key] = len(symbols)
      symbols.append(_Symbol(k, name, index))
      return len(symbols) - 1

   def opcode(self, op: int, name: str, signature: str) -> None:
      """!Declares a game command.
      @param op         FIRST_GAME_OPCODE to LAST_OPCODE
      @param name       its name (not a generic command's)
      @param signature  its operand letters
      @return nothing
      """
      if not FIRST_GAME_OPCODE <= op <= LAST_OPCODE:
         raise BankError(f"game opcode {op:#x} outside {FIRST_GAME_OPCODE:#x}-{LAST_OPCODE:#x}")
      if name in GENERIC_BY_NAME:
         raise BankError(f"game opcode name {name!r} is a generic command's")
      _check_signature(signature)
      if op in self._opcodes and self._opcodes[op] != (name, signature):
         raise BankError(f"opcode {op:#x} declared twice")
      self._opcodes[op] = (name, signature)
      self._opcode_by_name[name] = op

   def label(self, name: str) -> None:
      """!Names the address of the next command.
      @param name  the label
      @return nothing
      """
      if name in self.labels:
         raise BankError(f"label {name!r} defined twice")
      self.labels[name] = len(self.code)

   def script(self, name: str, label: str | None = None) -> None:
      """!A script entry: at a label, or at the next command (which then gets the script's name as a label).
      @param name   the entry's name
      @param label  where it starts (None: here)
      @return nothing
      """
      scripts = self._scripts   # local names (see symbol())
      labels = self.labels
      if label is None:
         label = name
         if name not in labels:
            self.label(name)
      if len(scripts) >= U16_MAX:
         raise BankError("too many scripts")
      scripts.append((name, label))

   # --- commands -----------------------------------------------------------------------------------------------

   def signature_of(self, command: str | int) -> tuple[int, str]:
      """!A command's opcode and signature.
      @param command  a generic or declared game command's name, or an opcode
      @return (opcode, signature)
      """
      if isinstance(command, int):
         if command < len(GENERIC_OPS):
            return command, GENERIC_OPS[command][1]
         if command in self._opcodes:
            return command, self._opcodes[command][1]
         raise BankError(f"opcode {command:#x} is not declared")
      if command in GENERIC_BY_NAME:
         op = GENERIC_BY_NAME[command]
         return op, GENERIC_OPS[op][1]
      if command in self._opcode_by_name:
         op = self._opcode_by_name[command]
         return op, self._opcodes[op][1]
      raise BankError(f"unknown command {command!r}")

   def emit(self, command: str | int, *operands: object) -> int:
      """!Writes a command.
      @param command   its name or opcode
      @param operands  in signature order: ints; label names for 'a'; symbol names (str) or (kind, name) pairs for
                       T K S C D; a list of ints for 'v'
      @return the command's address
      """
      op, signature = self.signature_of(command)
      if len(operands) != len(signature):
         raise BankError(f"{command!r} takes {len(signature)} operands ({signature!r}), got {len(operands)}")
      address = len(self.code)
      out = bytearray([op])
      for letter, value in zip(signature, operands):
         out += self._operand(letter, value, address + len(out))
      self.code += out
      return address

   def _operand(self, letter: str, value: object, position: int) -> bytes:
      """!Encodes one operand.
      @param letter    its signature letter
      @param value     its value
      @param position  its position in the code (address fixups)
      @return its bytes
      """
      if letter == ARGS:
         if not isinstance(value, (list, tuple)) or len(value) > MAX_ARGS:
            raise BankError(f"argument list must be a list of at most {MAX_ARGS} values, got {value!r}")
         out = bytes([len(value)])
         for v in value:
            out += struct.pack("<h", _ranged(int(v), S16_MIN, S16_MAX))
         return out
      if letter == ADDRESS:
         if isinstance(value, str):
            self._fixups.append((position, value))
            return struct.pack("<I", 0)
         return struct.pack("<I", _ranged(int(value), 0, U32_MAX))
      if letter in KIND_LETTERS:
         if isinstance(value, tuple):
            kind, name = value
         else:
            kind, name = KIND_LETTERS[letter], value
         if isinstance(name, int):
            return struct.pack("<H", _ranged(name, 0, U16_MAX))
         pos = self.symbol(kind, str(name))
         if self._symbols[pos].kind != KIND_LETTERS[letter]:
            raise BankError(f"symbol {name!r} is not a {KINDS[KIND_LETTERS[letter]]}")
         return struct.pack("<H", pos)
      limits = {"b": (0, U8_MAX), "c": (S8_MIN, S8_MAX), "h": (0, U16_MAX), "s": (S16_MIN, S16_MAX),
                "w": (0, U32_MAX)}
      low, high = limits[letter]
      return struct.pack(OPERAND_CODES[letter], _ranged(int(value), low, high))

   # --- output -------------------------------------------------------------------------------------------------

   def names_by_kind(self) -> dict[str, list[str]]:
      """!Each kind's symbol names in game index order (what a generator needs for its tables); call after build().
      @return KINDS name -> names
      """
      out: dict[str, list[str]] = {k: [] for k in KINDS}
      for kind in range(len(KINDS)):
         ordered = sorted((s for s in self._symbols if s.kind == kind),
                          key=lambda s: s.index if s.index is not None else UNBOUND)
         out[KINDS[kind]] = [s.name for s in ordered]
      return out

   def build(self, indexed: bool = False) -> bytes:
      """!Writes the bank.
      @param indexed  give every symbol a game index (explicit ones kept, the others by first use within their kind)
                      and set the indexed flag
      @return the bytes
      """
      # local names (see symbol())
      all_symbols, entries, declared, labels = self._symbols, self._scripts, self._opcodes, self.labels
      code = bytearray(self.code)
      for position, name in self._fixups:
         if name not in labels:
            raise BankError(f"undefined label {name!r}")
         code[position:position + OPERAND_BYTES[ADDRESS]] = struct.pack("<I", labels[name])
      if indexed:
         for kind in range(len(KINDS)):
            taken = {s.index for s in all_symbols if s.kind == kind and s.index is not None}
            next_free = 0
            for s in all_symbols:
               if s.kind != kind or s.index is not None:
                  continue
               while next_free in taken:
                  next_free += 1
               s.index = next_free
               taken.add(next_free)
      strings = bytearray()
      offsets: dict[str, int] = {}

      def string(text: str) -> int:
         """!A string's offset (each text written once).
         @param text  ASCII text
         @return its offset in the strings section
         """
         if text not in offsets:
            offsets[text] = len(strings)
            strings.extend(text.encode("ascii") + b"\0")
         return offsets[text]

      scripts = bytearray()
      for name, label in entries:
         if label not in labels:
            raise BankError(f"script {name!r} starts at undefined label {label!r}")
         scripts += struct.pack("<II", labels[label], string(name))
      symbols = bytearray()
      for s in all_symbols:
         index = s.index if s.index is not None else UNBOUND
         symbols += struct.pack("<BBHI", s.kind, 0, index, string(s.name))
      opcodes = bytearray()
      for op in sorted(declared):
         name, signature = declared[op]
         opcodes += struct.pack("<BBBBII", op, 0, 0, 0, string(name), string(signature))

      scripts_at = HEADER_BYTES
      symbols_at = _align(scripts_at + len(scripts))
      opcodes_at = _align(symbols_at + len(symbols))
      code_at = _align(opcodes_at + len(opcodes))
      strings_at = _align(code_at + len(code))
      header = struct.pack(HEADER_FORMAT, MAGIC, VERSION, FLAG_INDEXED if indexed else 0, len(entries),
                           len(all_symbols), len(declared), 0, scripts_at, symbols_at, opcodes_at, code_at,
                           len(code), strings_at, len(strings))
      out = bytearray(header)
      for at, part in ((scripts_at, scripts), (symbols_at, symbols), (opcodes_at, opcodes), (code_at, code),
                       (strings_at, strings)):
         out += bytes(at - len(out))
         out += part
      out += bytes(_align(len(out)) - len(out))
      return bytes(out)


def _ranged(value: int, low: int, high: int) -> int:
   """!Refuses a value out of an operand's range.
   @param value  the value
   @param low    smallest allowed
   @param high   largest allowed
   @return value
   """
   if not low <= value <= high:
      raise BankError(f"value {value} outside {low}..{high}")
   return value


## The writer's short name (docs/ANIM_SCRIPT.md).
BankWriter = AnimBankWriter


# --- reading ---------------------------------------------------------------------------------------------------


@dataclass
class Bank:
   """!A bank as read."""

   ## Header flags.
   flags: int
   ## Script entries: (name, address).
   scripts: list[tuple[str, int]]
   ## Symbols: (kind, game index, name).
   symbols: list[tuple[int, int, str]]
   ## Game opcodes: opcode -> (name, signature).
   opcodes: dict[int, tuple[str, str]]
   ## The code.
   code: bytes
   ## Problems found while reading (bad offsets are fatal: BankError).
   problems: list[str] = field(default_factory=list)

   def signature(self, op: int) -> tuple[str, str] | None:
      """!A command's name and signature.
      @param op  an opcode
      @return (name, signature), or None when the opcode is unknown
      """
      if op < len(GENERIC_OPS):
         return GENERIC_OPS[op]
      return self.opcodes.get(op)


def read_bank(data: bytes) -> Bank:
   """!Reads a bank.
   @param data  its bytes
   @return the bank
   """
   if len(data) < HEADER_BYTES:
      raise BankError("shorter than the header")
   (magic, version, flags, n_scripts, n_symbols, n_opcodes, _reserved, scripts_at, symbols_at, opcodes_at, code_at,
    code_size, strings_at, strings_size) = struct.unpack_from(HEADER_FORMAT, data)
   if magic != MAGIC:
      raise BankError("not an animation bank (magic)")
   if version != VERSION:
      raise BankError(f"version {version}, this tool reads {VERSION}")
   for at, size in ((scripts_at, n_scripts * SCRIPT_ENTRY_BYTES), (symbols_at, n_symbols * SYMBOL_BYTES),
                    (opcodes_at, n_opcodes * OPCODE_BYTES), (code_at, code_size), (strings_at, strings_size)):
      if at % SECTION_ALIGN or at + size > len(data):
         raise BankError(f"section at {at} ({size} bytes) misaligned or past the bank")
   strings = data[strings_at:strings_at + strings_size]

   def string(offset: int) -> str:
      """!A string of the strings section.
      @param offset  its offset (NO_NAME: none)
      @return the text ("" for none)
      """
      if offset == NO_NAME:
         return ""
      end = strings.find(b"\0", offset)
      if offset >= len(strings) or end < 0:
         raise BankError(f"string offset {offset} past the strings")
      return strings[offset:end].decode("ascii")

   scripts = []
   for i in range(n_scripts):
      address, name = struct.unpack_from("<II", data, scripts_at + i * SCRIPT_ENTRY_BYTES)
      scripts.append((string(name), address))
   symbols = []
   for i in range(n_symbols):
      kind, _r, index, name = struct.unpack_from("<BBHI", data, symbols_at + i * SYMBOL_BYTES)
      symbols.append((kind, index, string(name)))
   opcodes = {}
   for i in range(n_opcodes):
      op, _r0, _r1, _r2, name, signature = struct.unpack_from("<BBBBII", data, opcodes_at + i * OPCODE_BYTES)
      opcodes[op] = (string(name), string(signature))
   return Bank(flags, scripts, symbols, opcodes, data[code_at:code_at + code_size])


@dataclass
class Command:
   """!One decoded command."""

   ## Its address.
   address: int
   ## Its opcode.
   op: int
   ## Its name.
   name: str
   ## Its signature.
   signature: str
   ## Its operands (ints; a list for 'v').
   operands: list[int | list[int]]
   ## Its bytes.
   size: int


def decode(bank: Bank, address: int) -> Command:
   """!Decodes the command at an address.
   @param bank     the bank
   @param address  a code address
   @return the command
   """
   code = bank.code
   if address >= len(code):
      raise BankError(f"address {address} past the code")
   op = code[address]
   known = bank.signature(op)
   if known is None:
      raise BankError(f"unknown opcode {op:#x} at {address}")
   name, signature = known
   at = address + 1
   operands: list[int | list[int]] = []
   for letter in signature:
      if letter == ARGS:
         if at >= len(code):
            raise BankError(f"argument list past the code at {address}")
         count = code[at]
         at += 1
         if count > MAX_ARGS or at + count * OPERAND_BYTES["s"] > len(code):
            raise BankError(f"bad argument list at {address}")
         operands.append(list(struct.unpack_from(f"<{count}h", code, at)))
         at += count * OPERAND_BYTES["s"]
         continue
      size = OPERAND_BYTES.get(letter)
      if size is None:
         raise BankError(f"opcode {op:#x} has unknown operand letter {letter!r}")
      if at + size > len(code):
         raise BankError(f"command at {address} runs past the code")
      operands.append(struct.unpack_from(OPERAND_CODES[letter], code, at)[0])
      at += size
   return Command(address, op, name, signature, operands, at - address)


def commands(bank: Bank) -> list[Command]:
   """!Every command of the code, in order.
   @param bank  the bank
   @return the commands
   """
   out = []
   at = 0
   while at < len(bank.code):
      c = decode(bank, at)
      out.append(c)
      at += c.size
   return out


def check_bank(bank: Bank) -> list[str]:
   """!The problems of a bank (the engine's anim::check_code() rules, and the script entries).
   @param bank  the bank
   @return problem texts (empty: good)
   """
   problems: list[str] = []
   try:
      decoded = commands(bank)
   except BankError as e:
      return [str(e)]
   starts = {c.address for c in decoded}
   for name, address in bank.scripts:
      if address not in starts:
         problems.append(f"script {name!r} starts at {address}, not on a command")
   for c in decoded:
      for letter, value in zip(c.signature, c.operands):
         if letter == ADDRESS and value not in starts:
            problems.append(f"{c.name} at {c.address}: address {value} is not on a command")
         if letter in KIND_LETTERS:
            if not isinstance(value, int) or value >= len(bank.symbols):
               problems.append(f"{c.name} at {c.address}: symbol {value} past the symbol table")
            elif bank.symbols[value][0] != KIND_LETTERS[letter]:
               problems.append(f"{c.name} at {c.address}: symbol {value} is not a {KINDS[KIND_LETTERS[letter]]}")
   if bank.flags & FLAG_INDEXED:
      for i, (kind, index, name) in enumerate(bank.symbols):
         if index == UNBOUND:
            problems.append(f"indexed bank: symbol {i} ({KINDS[kind]} {name!r}) has no index")
   return problems


def _symbol_text(bank: Bank, letter: str, value: int) -> str:
   """!A symbol operand as .canim text.
   @param bank    the bank
   @param letter  its letter
   @param value   its symbol position
   @return "T:name"
   """
   return f"{letter}:{bank.symbols[value][SYMBOL_NAME_FIELD]}" if value < len(bank.symbols) else str(value)


def disassemble(bank: Bank) -> str:
   """!A bank as .canim text (assembling it gives the same bank: symbols, entries and code in their order).
   @param bank  the bank
   @return the text
   """
   decoded = commands(bank)
   labels: dict[int, str] = {}
   for _name, address in bank.scripts:
      labels.setdefault(address, f"L_{address:0{HEX_WIDTH}x}")
   for c in decoded:
      for letter, value in zip(c.signature, c.operands):
         if letter == ADDRESS and isinstance(value, int):
            labels.setdefault(value, f"L_{value:0{HEX_WIDTH}x}")
   lines = [f"# animation bank: {len(bank.scripts)} scripts, {len(bank.symbols)} symbols, {len(decoded)} commands"]
   for op in sorted(bank.opcodes):
      name, signature = bank.opcodes[op]
      lines.append(f".opcode {op:#04x} {name} \"{signature}\"")
   indexed = bank.flags & FLAG_INDEXED
   if indexed:
      lines.append(".indexed")
   for kind, index, name in bank.symbols:
      if indexed and index != UNBOUND:
         lines.append(f".symbol {KINDS[kind]} {name} {index}")
      else:
         lines.append(f".symbol {KINDS[kind]} {name}")
   for name, address in bank.scripts:
      lines.append(f".entry \"{name}\" {labels.get(address, str(address))}")
   for c in decoded:
      if c.address in labels:
         lines.append(f"{labels[c.address]}:")
      parts = [c.name]
      for letter, value in zip(c.signature, c.operands):
         if letter == ARGS:
            parts.append("[" + ", ".join(str(v) for v in value) + "]")
         elif letter == ADDRESS:
            parts.append(labels.get(value, str(value)))
         elif letter in KIND_LETTERS:
            parts.append(_symbol_text(bank, letter, value))
         else:
            parts.append(str(value))
      lines.append("   " + " ".join(parts))
   return "\n".join(lines) + "\n"


# --- the .canim assembler -------------------------------------------------------------------------------------

## A number token (decimal or 0x hex); anything else in an address slot is a label.
NUMBER = re.compile(r"-?(0[xX][0-9a-fA-F]+|[0-9]+)")
## One token: an argument list, a quoted string or a word.
TOKEN = re.compile(r"\[[^\]]*\]|\"[^\"]*\"|\S+")


def _number(text: str) -> int:
   """!A number of a .canim file.
   @param text  decimal, 0x hex, maybe negative
   @return the value
   """
   return int(text, 0)


def declare_manifests(w: BankWriter, paths: list[Path]) -> None:
   """!Declares the game opcodes of VM extension manifests (D-39, tools/vm_ext.py beside this tool), so their
   names and signatures come from one place.
   @param w      the writer
   @param paths  the manifests (anim VM)
   @return nothing
   @throws BankError when a manifest does not load
   """
   sys.path.insert(0, str(Path(__file__).resolve().parent))
   try:
      import vm_ext   # pylint: disable=import-outside-toplevel
   finally:
      sys.path.pop(0)
   try:
      for op, (name, signature) in vm_ext.anim_signatures(vm_ext.load_all(paths)).items():
         w.opcode(op, name, signature)
   except vm_ext.ManifestError as e:
      raise BankError(str(e)) from None


def assemble(text: str, indexed: bool = False, base: Path | None = None,
             usage: dict[str, int] | None = None) -> bytes:
   """!Assembles a .canim text (docs/ANIM_SCRIPT.md section 6).
   @param text     the text
   @param indexed  write an indexed bank
   @param base     the folder `.manifest` paths are relative to (default: the current folder)
   @param usage    receives each command's emit count, or None
   @return the bank's bytes
   """
   w = BankWriter()
   for number, raw in enumerate(text.splitlines(), start=1):
      line = raw.split("#", 1)[0].strip()
      if not line:
         continue
      if line == ".indexed":
         indexed = True
         continue
      try:
         _assemble_line(w, line, base if base is not None else Path("."), usage)
      except (BankError, ValueError, IndexError) as e:
         raise BankError(f"line {number}: {e}") from e
   return w.build(indexed=indexed)


def _assemble_line(w: BankWriter, line: str, base: Path, usage: dict[str, int] | None) -> None:
   """!One line of a .canim text.
   @param w      the writer
   @param line   the line, without its comment
   @param base   the folder `.manifest` paths are relative to
   @param usage  receives each command's emit count, or None
   @return nothing
   """
   if line.endswith(":") and " " not in line:
      w.label(line[:-1])
      return
   tokens = TOKEN.findall(line)
   head, *rest = tokens
   if head == ".manifest":
      declare_manifests(w, [base / r.strip("\"") for r in rest])
      return
   if usage is not None and not head.startswith("."):
      usage[head] = usage.get(head, 0) + 1
   if head == ".opcode":
      op, name, *sig = rest
      w.opcode(_number(op), name, sig[0].strip("\"") if sig else "")
      return
   if head == ".symbol":
      kind, name, *index = rest
      w.symbol(kind, name, _number(index[0]) if index else None)
      return
   if head == ".script":
      w.script(rest[0])
      return
   if head == ".entry":
      name, label = rest
      w.script(name.strip("\""), label)
      return
   _op, signature = w.signature_of(head)
   operands: list[object] = []
   for letter, token in zip(signature, tokens[1:]):
      if letter == ARGS:
         inner = token.strip("[]").strip()
         operands.append([_number(v.strip()) for v in inner.split(",")] if inner else [])
      elif letter == ADDRESS:
         operands.append(_number(token) if NUMBER.fullmatch(token) else token)
      elif letter in KIND_LETTERS:
         prefix, _sep, name = token.partition(":")
         if prefix != letter or not name:
            raise BankError(f"operand {token!r} must be {letter}:name")
         operands.append(name)
      else:
         operands.append(_number(token))
   if len(tokens) - 1 != len(signature):
      raise BankError(f"{head} takes {len(signature)} operands, got {len(tokens) - 1}")
   w.emit(head, *operands)


def to_cpp(data: bytes, name: str) -> str:
   """!A bank as a C++ source (4-byte aligned bytes in ROM).
   @param data  the bank
   @param name  the array's name (`NAME_size` holds its bytes)
   @return the source text
   """
   if not re.fullmatch(r"[A-Za-z_]\w*", name):
      raise BankError(f"bad C++ name {name!r}")
   lines = ["// Generated by tools/anim_bank.py: an animation script bank (docs/ANIM_SCRIPT.md).",
            "#include <cstdint>", "",
            f"alignas(4) extern const uint8_t {name}[] = {{"]
   for at in range(0, len(data), CPP_BYTES_PER_LINE):
      lines.append("   " + ", ".join(f"0x{b:02x}" for b in data[at:at + CPP_BYTES_PER_LINE]) + ",")
   lines += ["};", f"extern const uint32_t {name}_size = {len(data)};", ""]
   return "\n".join(lines)



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


def main(argv: list[str] | None = None) -> int:
   """!The command line.
   @param argv  arguments (None: sys.argv)
   @return exit status
   """
   parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
   sub = parser.add_subparsers(dest="action", required=True)
   asm = sub.add_parser("assemble", help="a .canim text file -> a bank")
   asm.add_argument("source", type=Path)
   asm.add_argument("output", type=Path)
   asm.add_argument("--indexed", action="store_true", help="write every symbol's game index")
   asm.add_argument("--cpp", metavar="NAME", help="write a C++ source defining NAME and NAME_size")
   asm.add_argument("--usage", type=Path, help="write the commands it emits (capsule.vm-usage/1)")
   dis = sub.add_parser("disasm", help="a bank -> .canim text")
   dis.add_argument("bank", type=Path)
   chk = sub.add_parser("check", help="list a bank's problems")
   chk.add_argument("bank", type=Path)
   args = parser.parse_args(argv)
   try:
      if args.action == "assemble":
         counts: dict[str, int] = {}
         data = assemble(args.source.read_text(encoding="utf-8"), indexed=args.indexed, base=args.source.parent,
                         usage=counts)
         if args.usage:
            record = {"schema": USAGE_SCHEMA, "vm": "anim", "bank": args.source.stem,
                      "ops": {k: counts[k] for k in sorted(counts)}}
            write_if_changed(args.usage, json.dumps(record, indent=USAGE_INDENT) + "\n")
         if args.cpp:
            write_if_changed(args.output, to_cpp(data, args.cpp))
         else:
            write_if_changed(args.output, data)
         return 0
      bank = read_bank(args.bank.read_bytes())
      if args.action == "disasm":
         sys.stdout.write(disassemble(bank))
         return 0
      problems = check_bank(bank)
      for p in problems:
         print(p)
      return 1 if problems else 0
   except BankError as e:
      print(f"error: {e}", file=sys.stderr)
      return 1


if __name__ == "__main__":
   sys.exit(main())
