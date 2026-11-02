#!/usr/bin/env python3
"""!VM extension manifests (capsule.vm-ext/1, ; docs/VM_EXT.md): load, check, generate dispatch, give the
assemblers their operand layouts, and record usage.

A manifest describes a game's opcodes for one of the engine's VMs: id, name, operand layout, and either an
**alias** (a short fixed sequence of the generic steps of include/capsule/core/vm_ext.h, data only) or a
**native** (a named C++ function in the game's tier, with the reason no generic step works). From the manifests
this tool writes the VM's dispatch (a C++ source, or a fragment a game's source includes) with ROM tables only, the
command list tools/script_bank.py assembles with, the opcode signatures tools/anim_bank.py declares, and the
checks §3.3 names:

    vm_ext.py check MANIFEST... [--sources FILE...] [--usage USAGE...]
    vm_ext.py dispatch MANIFEST... --out FILE [--usage USAGE...] [--exclusions OUT.json] [--depfile OUT.d]
    vm_ext.py layouts MANIFEST... --out FILE.json     (capsule.script-bank/1 "commands", for tools that want JSON)
    vm_ext.py layouts MANIFEST... --out FILE.h --cpp [--namespace NS]   (the layouts as a C++ header)
    vm_ext.py report MANIFEST... [--sizes NM.txt]     (native ops with their reasons, owners and sizes)

Generated files are written with LF line ends and only when their bytes change; paths written into them are posix
and relative to the output's folder.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any, NamedTuple

## The manifest schema.
SCHEMA = "capsule.vm-ext/1"
## The usage record schema.
USAGE_SCHEMA = "capsule.vm-usage/1"
## The exclusion record schema.
EXCLUSION_SCHEMA = "capsule.vm-exclusions/1"
## The VMs a manifest may extend.
VMS = ("script", "battle", "anim", "effect", "seq", "screen-task")
## The table of a VM that runs one command table.
MAIN_TABLE = "main"
## The VMs whose dispatch this tool generates today (the others: §6, later).
DISPATCH_VMS = ("battle", "anim")
## The largest opcode id.
OPCODE_MAX = 0xFF
## The animation player's first game opcode (include/capsule/anim/bank.h kFirstGameOpcode).
ANIM_FIRST_GAME_OPCODE = 0x40
## Op kinds.
KINDS = ("alias", "native")
## Parity states of a native that has an alias candidate.
PARITY_STATES = ("pending", "proven")

class OperandType(NamedTuple):
   """!One operand type of the shared vocabulary."""

   ## Bytes in the code (0: variable length).
   bytes: int
   ## Its tools/script_bank.py kind (None: not a battle operand).
   bank_kind: str | None
   ## Its tools/anim_bank.py signature letter (None: not an animation operand).
   anim_letter: str | None
   ## Its core/vm_ext.h Type when an alias step reads it (None: steps cannot read it).
   arg_type: str | None


## Operand types: (bytes, script_bank kind, anim_bank letter, alias argument type). None: not that VM's.
OPERAND_TYPES: dict[str, OperandType] = {
   "u8": OperandType(1, "u8", "b", "U8"),
   "s8": OperandType(1, "s8", "c", "S8"),
   "u16": OperandType(2, "u16", "h", "U16"),
   "s16": OperandType(2, "s16", "s", "S16"),
   "u32": OperandType(4, "u32", "w", "U32"),
   "code": OperandType(4, "code", "a", "Code"),
   "ref": OperandType(4, "ref", "w", "Ref"),
   "battler": OperandType(1, "u8", "b", "U8"),
   "string": OperandType(2, "u16", "h", "U16"),
   "template": OperandType(2, None, "T", None),
   "task": OperandType(2, None, "K", None),
   "sound_task": OperandType(2, None, "S", None),
   "callback": OperandType(2, None, "C", None),
   "data": OperandType(2, None, "D", None),
   "args": OperandType(0, None, "v", None),
}

class StepType(NamedTuple):
   """!One generic step of include/capsule/core/vm_ext.h as manifests name it."""

   ## Its C++ Op.
   cpp_op: str
   ## Its parameter names in argument order.
   params: tuple[str, ...]
   ## Whether it takes a width.
   takes_width: bool
   ## The mode it takes ("alu", "when" or None).
   mode: str | None
   ## The VMs that run it.
   vms: tuple[str, ...]


## Generic steps: name -> its StepType.
STEPS: dict[str, StepType] = {
   "nop": StepType("Nop", (), False, None, ("battle", "anim")),
   "jump": StepType("Jump", ("target",), False, None, ("battle", "anim")),
   "jump_if": StepType("JumpIf", ("compare", "ref", "value", "target"), True, None, ("battle", "anim")),
   "jump_if_bytes": StepType("JumpIfBytes", ("a", "b", "size", "target"), False, "when", ("battle", "anim")),
   "call": StepType("Call", ("target",), False, None, ("battle",)),
   "return": StepType("Return", (), False, None, ("battle",)),
   "end": StepType("End", (), False, None, ("battle",)),
   "yield": StepType("Yield", (), False, None, ("battle", "anim")),
   "pause": StepType("Pause", ("frames",), False, None, ("battle",)),
   "pause_if_set": StepType("PauseIfSet", ("flag", "frames"), False, None, ("battle",)),
   "wait_until": StepType("WaitUntil", ("compare", "ref", "value"), True, None, ("battle", "anim")),
   "hold": StepType("Hold", (), False, None, ("battle", "anim")),
   "modify": StepType("Modify", ("ref", "value"), True, "alu", ("battle", "anim")),
   "copy": StepType("Copy", ("dest", "src", "size"), False, None, ("battle", "anim")),
}
## Parameters that must be a reference (an operand of type ref, or a constant reference).
REF_PARAMS = ("ref", "flag", "a", "b", "dest", "src")
## Parameters that must be a code position.
CODE_PARAMS = ("target",)
## Compare names (core/vm_ext.h Compare).
COMPARES = ("equal", "not_equal", "greater", "less", "common_bits", "no_common_bits")
## Read-modify-write names (core/vm_ext.h Alu).
ALUS = ("set", "add", "sub", "or", "bic")
## jump_if_bytes modes (Compare values).
WHENS = {"equal": 0, "not_equal": 1}
## Variable widths a step may name.
WIDTHS = (1, 2, 4)
## Arguments of a step (core/vm_ext.h kStepArgs).
STEP_ARGS = 4

## The battle machine's reference kinds (include/capsule/battle/vm.h RefKind).
REF_KINDS = {"code": 0, "variable": 1, "table": 2, "value": 3}
## A reference's kind starts at this bit.
REF_KIND_SHIFT = 28
## A variable or table id starts at this bit.
REF_ID_SHIFT = 16
## The largest variable or table id.
REF_ID_MAX = 0xFFF
## The largest byte offset.
REF_OFFSET_MAX = 0xFFFF
## The largest constant.
U32_MAX = 0xFFFFFFFF
## A C++ identifier.
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_:]*$")
## Characters that may stay in a C++ name made from an op name.
CPP_WORD = re.compile(r"[^0-9A-Za-z]+")
## Indent of generated JSON.
JSON_INDENT = 2
## Fields of an `nm -S` line: address, size, type, name.
NM_FIELDS = 4
## Its size field.
NM_SIZE = 1
## Its type field.
NM_TYPE = 2
## Its name field (the rest of the line).
NM_NAME = 3
## nm types of code symbols.
NM_CODE_TYPES = ("t", "w")
## How a demangled name shows an anonymous namespace.
ANONYMOUS_NAMESPACE = "(anonymous namespace)"
## nm's sizes are hexadecimal.
HEX_BASE = 16
## Indent of generated C++ (the project's 3 spaces).
INDENT = "   "


class ManifestError(ValueError):
   """!A manifest that does not follow the schema (an error, not a warning)."""


@dataclass
class Operand:
   """!One operand of an op."""

   ## Its name (what alias arguments name).
   name: str
   ## Its type (an OPERAND_TYPES key).
   type: str
   ## Its byte offset from the opcode byte (None after a variable-length operand).
   at: int | None


@dataclass
class Op:
   """!One op of a manifest."""

   ## Its opcode.
   id: int
   ## Its name.
   name: str
   ## Its operands in order.
   operands: list[Operand]
   ## "alias" or "native".
   kind: str
   ## The manifest it comes from.
   manifest: "Manifest"
   ## The raw entry.
   raw: dict[str, Any]
   ## Bytes of the command (None: variable length).
   size: int | None = None
   ## A native's C++ function.
   symbol: str | None = None
   ## A native's reason.
   reason: str = ""
   ## An alias's steps (or a native's alias candidate).
   steps: list[dict[str, Any]] = field(default_factory=list)
   ## Whether it may wait on controllers or frames.
   waits: bool = False
   ## Its owner (a package or a person).
   owner: str = ""
   ## Its tier.
   tier: int = 0
   ## Glitch hook ids.
   glitch: list[str] = field(default_factory=list)
   ## A native at a generic opcode (the animation player's 0x00-0x3F).
   override: bool = False
   ## A native's alias candidate parity state ("" when it has none).
   parity: str = ""


@dataclass
class Manifest:
   """!One manifest read back."""

   ## Its file.
   path: Path
   ## Its VM.
   vm: str
   ## The command table of that VM it extends (a VM may run several tables: "main" when it runs one).
   table: str
   ## Its name.
   name: str
   ## The opcode space it claims (first, last).
   space: tuple[int, int]
   ## Its ops by id.
   ops: dict[int, Op]
   ## Named constants of references (alias arguments name them).
   refs: dict[str, int]
   ## The dispatch block (namespace, symbol, includes, resolver, unknown, trace, form, native_prefix).
   dispatch: dict[str, Any]
   ## Sub-op tables (parent op name -> (selector operand, ops)).
   sub_ops: dict[str, tuple[str, dict[int, Op]]]
   ## The raw JSON.
   raw: dict[str, Any]
   ## Its command table's name (table_key() of its VM and table).
   key: str = ""


def make_ref(kind: str | int, ident: int = 0, offset: int = 0) -> int:
   """!Encodes a battle machine reference (tools/script_bank.py make_ref()'s rule).
   @param kind    a REF_KINDS name or a number 0-15
   @param ident   a variable or table id, or a code / value payload
   @param offset  a byte offset
   @return the 32-bit reference
   @throws ManifestError on an out-of-range field
   """
   k = REF_KINDS[kind] if isinstance(kind, str) else kind
   if k in (REF_KINDS["code"], REF_KINDS["value"]):
      if not 0 <= ident < (1 << REF_KIND_SHIFT):
         raise ManifestError(f"reference payload {ident} out of range")
      return (k << REF_KIND_SHIFT) | ident
   if not 0 <= ident <= REF_ID_MAX or not 0 <= offset <= REF_OFFSET_MAX:
      raise ManifestError(f"reference id {ident} or offset {offset} out of range")
   return (k << REF_KIND_SHIFT) | (ident << REF_ID_SHIFT) | offset


def _operands(raw: list[Any], where: str) -> tuple[list[Operand], int | None]:
   """!An op's operands and its size.
   @param raw    the manifest's operand list (type strings or {"name", "type"} objects)
   @param where  the op, for messages
   @return (the operands, the command's bytes or None for a variable length)
   @throws ManifestError on an unknown type or a duplicate name
   """
   out: list[Operand] = []
   at: int | None = 1
   for i, o in enumerate(raw):
      name, kind = (f"op{i + 1}", o) if isinstance(o, str) else (o.get("name", f"op{i + 1}"), o.get("type"))
      if kind not in OPERAND_TYPES:
         raise ManifestError(f"{where}: operand {name!r} has an unknown type {kind!r}")
      if any(x.name == name for x in out):
         raise ManifestError(f"{where}: two operands are named {name!r}")
      out.append(Operand(name, kind, at))
      width = OPERAND_TYPES[kind].bytes
      at = None if at is None or kind == "args" else at + width
   return out, at


def _op(raw: dict[str, Any], manifest: Manifest, where: str) -> Op:
   """!Reads one op entry.
   @param raw       the entry
   @param manifest  its manifest
   @param where     the manifest, for messages
   @return the op
   @throws ManifestError on a schema error
   """
   name = raw.get("name")
   ident = raw.get("id")
   if not isinstance(name, str) or not name or not isinstance(ident, int) or not 0 <= ident <= OPCODE_MAX:
      raise ManifestError(f"{where}: an op needs a name and an id 0-{OPCODE_MAX} ({raw!r})")
   label = f"{where}: op {name} ({ident:#04x})"
   kind = raw.get("kind")
   if kind not in KINDS:
      raise ManifestError(f"{label}: kind must be one of {', '.join(KINDS)}")
   operands, size = _operands(raw.get("operands", []), label)
   op = Op(ident, name, operands, kind, manifest, raw, size=size, waits=bool(raw.get("waits", False)),
           owner=str(raw.get("owner", manifest.raw.get("owner", ""))),
           tier=int(raw.get("tier", manifest.raw.get("tier", 0))), glitch=list(raw.get("glitch", [])),
           override=bool(raw.get("override", False)))
   if kind == "native":
      op.symbol = raw.get("symbol")
      op.reason = str(raw.get("reason", "")).strip()
      if not isinstance(op.symbol, str) or not IDENT.match(op.symbol):
         raise ManifestError(f"{label}: a native needs its C++ symbol")
      if not op.reason:
         raise ManifestError(f"{label}: a native needs a reason (why no generic op or alias works)")
      candidate = raw.get("candidate")
      if candidate is not None:
         op.steps = list(candidate.get("expands", []))
         op.parity = candidate.get("parity", "pending")
         if op.parity not in PARITY_STATES:
            raise ManifestError(f"{label}: candidate parity must be one of {', '.join(PARITY_STATES)}")
   else:
      op.steps = list(raw.get("expands", []))
      if not op.steps:
         raise ManifestError(f"{label}: an alias needs its steps (expands)")
      if size is None:
         raise ManifestError(f"{label}: an alias needs a fixed operand layout (no args list)")
   return op


def _manifest_from(raw: dict[str, Any], path: Path) -> Manifest:
   """!Reads a parsed manifest.
   @param raw   the JSON
   @param path  its file
   @return the manifest
   @throws ManifestError on a schema error
   """
   where = path.name
   if raw.get("schema") != SCHEMA:
      raise ManifestError(f"{where}: schema must be {SCHEMA}")
   vm = raw.get("vm")
   if vm not in VMS:
      raise ManifestError(f"{where}: vm must be one of {', '.join(VMS)}")
   space = raw.get("space", {})
   first, last = space.get("first", 0), space.get("last", OPCODE_MAX)
   if not 0 <= first <= last <= OPCODE_MAX:
      raise ManifestError(f"{where}: opcode space {first}-{last} out of range")
   # named references: a number (the animation player's game numbering) or a battle reference {kind, id, offset}
   refs = {str(k): make_ref(v.get("kind", "variable"), v.get("id", 0), v.get("offset", 0)) if isinstance(v, dict)
           else int(v) for k, v in raw.get("refs", {}).items()}
   table = str(raw.get("table", MAIN_TABLE))
   m = Manifest(path, vm, table, str(raw.get("name", path.stem)), (first, last), {}, refs,
                dict(raw.get("dispatch", {})), {}, raw, table_key(vm, table))
   names: set[str] = set()
   for entry in raw.get("ops", []):
      op = _op(entry, m, where)
      if op.id in m.ops or op.name in names:
         raise ManifestError(f"{where}: op {op.name} ({op.id:#04x}) is a duplicate id or name")
      generic = vm == "anim" and op.id < ANIM_FIRST_GAME_OPCODE
      if generic and not op.override:
         raise ManifestError(f"{where}: op {op.name} ({op.id:#04x}) is a generic opcode: mark it override")
      if not first <= op.id <= last and not generic:
         raise ManifestError(f"{where}: op {op.name} ({op.id:#04x}) lies outside the space {first:#04x}-{last:#04x}")
      m.ops[op.id] = op
      names.add(op.name)
      sub = entry.get("sub_ops")
      if sub is not None:
         selector = sub.get("selector")
         if not any(o.name == selector for o in op.operands):
            raise ManifestError(f"{where}: op {op.name}: sub_ops selector {selector!r} is not one of its operands")
         table: dict[int, Op] = {}
         for s in sub.get("ops", []):
            child = _op(s, m, f"{where}: {op.name}")
            if child.id in table:
               raise ManifestError(f"{where}: {op.name}: sub-op id {child.id} twice")
            table[child.id] = child
         m.sub_ops[op.name] = (selector, table)
   for op in m.ops.values():
      if op.kind == "alias" or op.steps:
         _check_steps(op, m)
   return m


def load(path: Path) -> Manifest:
   """!Reads and checks one manifest.
   @param path  its file
   @return the manifest
   @throws ManifestError on a schema error (OSError / JSONDecodeError on a bad file)
   """
   return _manifest_from(json.loads(path.read_text(encoding="utf-8")), path)


def _check_steps(op: Op, m: Manifest) -> None:
   """!Checks an alias's (or a candidate's) steps against the generic step vocabulary.
   @param op  the op
   @param m   its manifest
   @return nothing
   @throws ManifestError on an unknown step, a step the VM does not run, or a bad argument
   """
   label = f"{m.path.name}: op {op.name}"
   for s in op.steps:
      name = s.get("op")
      if name not in STEPS:
         raise ManifestError(f"{label}: unknown step {name!r}")
      _, params, takes_width, mode, vms = STEPS[name]
      if m.vm not in vms:
         raise ManifestError(f"{label}: the {m.vm} VM does not run step {name!r}")
      args = s.get("args", {})
      for p in args:
         if p not in params:
            raise ManifestError(f"{label}: step {name} has no parameter {p!r}")
      for p in params:
         if p not in args:
            raise ManifestError(f"{label}: step {name} needs {p!r}")
         _arg(op, m, p, args[p])
      if takes_width and s.get("width", 1) not in WIDTHS:
         raise ManifestError(f"{label}: step {name}: width must be one of {WIDTHS}")
      if mode == "alu" and s.get("alu") not in ALUS:
         raise ManifestError(f"{label}: step {name}: alu must be one of {', '.join(ALUS)}")
      if mode == "when" and s.get("when") not in WHENS:
         raise ManifestError(f"{label}: step {name}: when must be one of {', '.join(WHENS)}")


def _arg(op: Op, m: Manifest, param: str, value: Any) -> tuple[str, str, int, int]:
   """!Resolves one step argument.
   @param op     the op
   @param m      its manifest
   @param param  the step's parameter
   @param value  an operand name, {"const": n or compare name}, {"ref": name or {kind, id, offset}}
   @return (from, type, at, value): from "Operand" / "Constant", an alias argument type, the operand's offset, a
           constant
   @throws ManifestError on an unknown operand, a wrong type or an out-of-range constant
   """
   label = f"{m.path.name}: op {op.name}: {param}"
   if isinstance(value, str):
      found = [o for o in op.operands if o.name == value]
      if not found:
         raise ManifestError(f"{label}: no operand named {value!r}")
      o = found[0]
      atype = OPERAND_TYPES[o.type].arg_type
      if atype is None or o.at is None:
         raise ManifestError(f"{label}: operand {value!r} ({o.type}) cannot be a step argument")
      if param in REF_PARAMS and atype != "Ref":
         raise ManifestError(f"{label}: operand {value!r} must be a ref")
      if param in CODE_PARAMS and atype != "Code":
         raise ManifestError(f"{label}: operand {value!r} must be code")
      return "Operand", atype, o.at, 0
   if isinstance(value, dict) and "ref" in value:
      r = value["ref"]
      if isinstance(r, str):
         if r not in m.refs:
            raise ManifestError(f"{label}: no ref named {r!r} in the manifest's refs")
         return "Constant", "Ref", 0, m.refs[r]
      return "Constant", "Ref", 0, make_ref(r.get("kind", "variable"), r.get("id", 0), r.get("offset", 0))
   if isinstance(value, dict) and "const" in value:
      c = value["const"]
      if param == "compare":
         c = COMPARES.index(c) if isinstance(c, str) and c in COMPARES else c
      if not isinstance(c, int) or not 0 <= c <= U32_MAX:
         raise ManifestError(f"{label}: constant {value['const']!r} out of range")
      if param in REF_PARAMS or param in CODE_PARAMS:
         raise ManifestError(f"{label}: a constant {param} must be given as a ref")
      return "Constant", "U32", 0, c
   raise ManifestError(f"{label}: {value!r} is neither an operand name, a const nor a ref")


def load_all(paths: Iterable[Path]) -> list[Manifest]:
   """!Reads manifests and checks their opcode spaces against each other.
   @param paths  the files
   @return the manifests
   @throws ManifestError on a schema error or two manifests of one VM claiming the same opcode
   """
   ms = [load(p) for p in paths]
   for i, a in enumerate(ms):
      for b in ms[i + 1:]:
         if a.key != b.key:
            continue
         if a.space[0] <= b.space[1] and b.space[0] <= a.space[1]:
            raise ManifestError(f"{a.path.name} and {b.path.name} both claim {a.vm} opcodes "
                                f"{max(a.space[0], b.space[0]):#04x}-{min(a.space[1], b.space[1]):#04x}")
         clash = set(a.ops) & set(b.ops)
         if clash:
            raise ManifestError(f"{a.path.name} and {b.path.name} both declare {a.vm} opcode {min(clash):#04x}")
   return ms


def table_key(vm: str, table: str = MAIN_TABLE) -> str:
   """!The name of one command table of a VM (usage records and ops_of() use it).
   @param vm     the VM
   @param table  its table
   @return "vm" for a VM's main table, else "vm:table"
   """
   return vm if table == MAIN_TABLE else f"{vm}:{table}"


def ops_of(manifests: Sequence[Manifest], key: str) -> dict[int, Op]:
   """!Every op of one command table.
   @param manifests  the manifests
   @param key        table_key() of the table (a VM's name for its main table)
   @return the ops by id
   """
   out: dict[int, Op] = {}
   for m in manifests:
      if m.key == key:
         out.update(m.ops)
   return out


# --- what the assemblers read --------------------------------------------------------------------------------


def script_bank_commands(manifests: Sequence[Manifest], vm: str | None = None) -> list[dict[str, Any]]:
   """!The command list tools/script_bank.py assembles with (capsule.script-bank/1 "commands").
   @param manifests  the manifests (one battle command table)
   @param vm         table_key() of the table (default: the first manifest's)
   @return one {"name", "id", "operands"} per op, in id order
   @throws ManifestError on an operand type script banks cannot hold
   """
   out = []
   key = vm if vm is not None else (manifests[0].key if manifests else "battle")
   for ident, op in sorted(ops_of(manifests, key).items()):
      kinds = []
      for o in op.operands:
         kind = OPERAND_TYPES[o.type].bank_kind
         if kind is None:
            raise ManifestError(f"op {op.name}: operand type {o.type} is not a script bank operand")
         kinds.append(kind)
      out.append({"name": op.name, "id": ident, "operands": kinds})
   return out


def anim_signatures(manifests: Sequence[Manifest]) -> dict[int, tuple[str, str]]:
   """!The game opcodes' names and signatures tools/anim_bank.py declares (BankWriter.opcode()).
   @param manifests  the manifests
   @return opcode -> (name, signature letters), game opcodes only
   """
   out: dict[int, tuple[str, str]] = {}
   for ident, op in sorted(ops_of(manifests, "anim").items()):
      if ident < ANIM_FIRST_GAME_OPCODE:
         continue
      letters = "".join(str(OPERAND_TYPES[o.type].anim_letter) for o in op.operands)
      out[ident] = (op.name, letters)
   return out


def layout_header(manifests: Sequence[Manifest], namespace: str) -> str:
   """!A C++ header of one command table's operand layouts (traces, disassemblers and tests read operands by it
   instead of keeping a copy): each op's id, size, operand count and operand types (core/vm_ext.h Type).
   @param manifests  the manifests (one command table)
   @param namespace  the header's namespace
   @return the header
   @throws ManifestError on a variable-length op or an operand alias steps cannot read
   """
   ops = ops_of(manifests, manifests[0].key)
   widest = max((len(op.operands) for op in ops.values()), default=0)
   lines = ["/// @file", f"/// Generated by tools/vm_ext.py from {', '.join(m.path.name for m in manifests)}: the "
            "operand layouts.", "/// Do not edit; change the manifests.", "#pragma once", "",
            "#include \"capsule/core/vm_ext.h\"", "", "#include <cstdint>", "", f"namespace {namespace}", "{", "",
            f"inline constexpr int kMaxOperands = {max(widest, 1)};   ///< Operands of the widest op.", "",
            "/// One op's layout.", "struct OpLayout", "{",
            f"{INDENT}uint8_t id;                                  ///< Its opcode.",
            f"{INDENT}uint8_t size;                                ///< Bytes of the command.",
            f"{INDENT}uint8_t count;                               ///< Operands.",
            f"{INDENT}capsule::vm_ext::Type types[kMaxOperands];   ///< Their types.",
            f"{INDENT}const char* name;                            ///< Its name.", "};", "",
            "/// Every op in id order.", "inline constexpr OpLayout kLayouts[] = {"]
   for ident, op in sorted(ops.items()):
      if op.size is None:
         raise ManifestError(f"op {op.name}: a variable-length op has no fixed layout")
      types = []
      for o in op.operands:
         arg_type = OPERAND_TYPES[o.type].arg_type
         if arg_type is None:
            raise ManifestError(f"op {op.name}: operand type {o.type} has no layout type")
         types.append(f"capsule::vm_ext::Type::{arg_type}")
      lines.append(f"{INDENT}{{{ident}, {op.size}, {len(types)}, {{{', '.join(types)}}}, \"{op.name}\"}},")
   lines += ["};", "", f"}}   // namespace {namespace}", ""]
   return "\n".join(lines)


# --- usage ---------------------------------------------------------------------------------------------------


def usage_record(vm: str, bank: str, counts: dict[str, int], table: str = MAIN_TABLE) -> dict[str, Any]:
   """!A usage record (capsule.vm-usage/1): how often a bank emits each op.
   @param vm      the VM
   @param bank    the bank's name
   @param counts  op name -> times emitted
   @param table   the VM's command table the bank runs on
   @return the record
   """
   record: dict[str, Any] = {"schema": USAGE_SCHEMA, "vm": vm}
   if table != MAIN_TABLE:
      record["table"] = table
   record["bank"] = bank
   record["ops"] = {k: counts[k] for k in sorted(counts)}
   return record


def read_usage(paths: Iterable[Path]) -> dict[str, dict[str, int]]:
   """!Reads usage records.
   @param paths  the files
   @return table_key() -> op name -> total count
   @throws ManifestError on a record of another schema
   """
   out: dict[str, dict[str, int]] = {}
   for p in paths:
      raw = json.loads(p.read_text(encoding="utf-8"))
      if raw.get("schema") != USAGE_SCHEMA:
         raise ManifestError(f"{p.name}: schema must be {USAGE_SCHEMA}")
      ops = out.setdefault(table_key(raw["vm"], raw.get("table", MAIN_TABLE)), {})
      for name, n in raw.get("ops", {}).items():
         ops[name] = ops.get(name, 0) + int(n)
   return out


# --- checks --------------------------------------------------------------------------------------------------


def generic_names(key: str) -> set[str]:
   """!The engine's own command names of a command table (declared without a manifest: the animation player's
   generic commands, tools/anim_bank.py GENERIC_OPS).
   @param key  table_key() of the table
   @return the names
   """
   if key != "anim":
      return set()
   sys.path.insert(0, str(Path(__file__).resolve().parent))
   try:
      import anim_bank   # pylint: disable=import-outside-toplevel
   finally:
      sys.path.pop(0)
   return {name for name, _sig in anim_bank.GENERIC_OPS}


## The first parameter of a VM's command functions (what the implementation check looks for).
COMMAND_PARAMETER = {"battle": "Machine", "anim": "Player"}


def _defined_functions(sources: Iterable[Path], prefix: str, vm: str) -> dict[str, Path]:
   """!The functions a set of C++ sources defines whose names start with a prefix and whose signature is a VM's
   command's (the first parameter a Machine& for the battle machine, a Player& for the animation player).
   @param sources  the files
   @param prefix   the name prefix ("" for every such function)
   @param vm       the VM
   @return name -> the file that defines it
   """
   parameter = COMMAND_PARAMETER.get(vm, "Machine")
   pattern = re.compile(r"^\s*(?:static\s+)?void\s+(" + re.escape(prefix) +
                        r"\w*)\s*\(\s*(?:\[\[\w+\]\]\s*)?(?:[\w:]*::)?" + parameter + r"\s*&[^;\n]*$", re.M)
   found: dict[str, Path] = {}
   for path in sources:
      for match in pattern.finditer(path.read_text(encoding="utf-8", errors="replace")):
         found.setdefault(match.group(1), path)
   return found


def check(manifests: Sequence[Manifest], sources: Sequence[Path] = (),
          usage: dict[str, dict[str, int]] | None = None) -> tuple[list[str], list[str]]:
   """!§3.3's checks.
   - A native without a reason, a schema error or a clash: errors (load() raises them).
   - A native whose alias candidate has proven parity: a warning (it should become the alias).
   - With sources: every native's function is defined, and every function of the manifest's native prefix is in
     the manifest (errors).
   - With usage: every op a bank emits is declared (errors).
   @param manifests  the manifests (already loaded: load_all())
   @param sources    C++ sources that define the natives
   @param usage      read_usage()'s result
   @return (errors, warnings)
   """
   errors: list[str] = []
   warnings: list[str] = []
   for m in manifests:
      for op in m.ops.values():
         if op.kind == "native" and op.parity == "proven":
            warnings.append(f"{m.path.name}: native {op.name} ({op.id:#04x}) matches its proven alias candidate: "
                            "make it an alias and remove the native")
   for m in manifests:
      # the manifest's own sources (dispatch.sources, relative to it), else the ones given
      own = [m.path.parent / s for s in m.dispatch.get("sources", [])]
      files = own or list(sources)
      if files:
         prefix = str(m.dispatch.get("native_prefix", ""))
         defined = _defined_functions(files, prefix, m.vm)
         declared = {op.symbol for op in m.ops.values() if op.kind == "native"}
         for table in m.sub_ops.values():
            declared |= {op.symbol for op in table[1].values() if op.kind == "native"}
         for op in sorted(m.ops.values(), key=lambda o: o.id):
            if op.kind == "native" and op.symbol not in defined:
               errors.append(f"{m.path.name}: native {op.name} ({op.id:#04x}): {op.symbol} is not defined in the "
                             "sources")
         extra = [p for p in m.dispatch.get("helpers", [])]
         if prefix:
            for name in sorted(set(defined) - declared - set(extra)):
               errors.append(f"{m.path.name}: {name} ({defined[name].name}) has no manifest entry")
   if usage:
      for key, ops in usage.items():
         names = {op.name for op in ops_of(manifests, key).values()} | generic_names(key)
         for name in sorted(ops):
            if name not in names:
               errors.append(f"usage: {key} op {name!r} is emitted by a bank but not declared")
   return errors, warnings


# --- dispatch ------------------------------------------------------------------------------------------------


def cpp_word(name: str) -> str:
   """!A snake_case C++ word for an op name.
   @param name  the op's name
   @return the word
   """
   return "_".join(w.lower() for w in CPP_WORD.split(name) if w)


def _step_cpp(op: Op, m: Manifest, s: dict[str, Any]) -> str:
   """!One step as a C++ initialiser.
   @param op  the op
   @param m   its manifest
   @param s   the step
   @return the initialiser
   """
   cpp_op, params, takes_width, mode_kind, _ = STEPS[s["op"]]
   mode = 0
   if mode_kind == "alu":
      mode = ALUS.index(s["alu"])
   elif mode_kind == "when":
      mode = WHENS[s["when"]]
   width = int(s.get("width", 1)) if takes_width else 0
   args = []
   for p in params:
      source, atype, at, value = _arg(op, m, p, s["args"][p])
      args.append(f"{{ext::From::{source}, ext::Type::{atype}, {at}, 0, {value:#x}u}}")
   while len(args) < STEP_ARGS:
      args.append("{}")
   return f"{{ext::Op::{cpp_op}, {mode}, {width}, 0, {{{', '.join(args)}}}}}"


@dataclass
class Dispatch:
   """!A generated dispatch."""

   ## The C++ text.
   text: str
   ## Ops left out because no bank uses them.
   excluded: list[Op]
   ## Aliases generated.
   aliases: list[Op]
   ## Natives referenced.
   natives: list[Op]


def _relative(path: Path, base: Path) -> str:
   """!A posix path relative to a folder (never absolute).
   @param path  the file
   @param base  the folder
   @return the relative path
   @throws ManifestError when it cannot be relative (another drive)
   """
   try:
      return PurePosixPath(Path(os.path.relpath(path.resolve(), base.resolve())).as_posix()).as_posix()
   except ValueError:
      raise ManifestError(f"{path} cannot be written relative to {base}") from None


def generate_dispatch(manifests: Sequence[Manifest], out: Path,
                      usage: dict[str, dict[str, int]] | None = None) -> Dispatch:
   """!Writes the dispatch of one VM's manifests as C++.
   The dispatch block of the first manifest (of that VM) names: namespace, symbol (the table's name), count (battle:
   entries of the table), includes, resolver (the alias references' resolver, or null), unknown and trace (battle),
   form ("source": a translation unit defining an extern const table; "fragment": text a game's source includes
   where its natives are visible, defining a constexpr table).
   @param manifests  the manifests (one VM)
   @param out        the output file (its folder: the base of the paths written)
   @param usage      read_usage()'s result: ops no bank uses are left out (None: every op)
   @return the dispatch
   @throws ManifestError when the VM has no dispatch generator or the manifests disagree
   """
   keys = {m.key for m in manifests}
   if len(keys) != 1:
      raise ManifestError("one dispatch is generated per command table: pass that table's manifests only")
   key = keys.pop()
   vm = manifests[0].vm
   if vm not in DISPATCH_VMS:
      raise ManifestError(f"the {vm} VM's dispatch is not generated yet")
   d = dict(manifests[0].dispatch)
   for m in manifests[1:]:
      for k, v in m.dispatch.items():
         if k == "includes":
            d.setdefault("includes", [])
            d["includes"] = list(d["includes"]) + [i for i in v if i not in d["includes"]]
         elif k not in d:
            d[k] = v
   ops = ops_of(manifests, key)
   # no record for this table (or none at all): nothing is left out, rather than everything
   used = None if usage is None or key not in usage else usage[key]
   # an override of a generic opcode is never left out (the generic command would come back in its place)
   excluded = [op for op in ops.values()
               if used is not None and op.name not in used and not op.raw.get("keep") and not op.override]
   keep = {op.id: op for op in ops.values() if op not in excluded}
   aliases = [op for _, op in sorted(keep.items()) if op.kind == "alias"]
   natives = [op for _, op in sorted(keep.items()) if op.kind == "native"]
   namespace = d.get("namespace", "game")
   symbol = d.get("symbol", "kCommandTable")
   fragment = d.get("form", "source") == "fragment"
   resolver = d.get("resolver") or "nullptr"
   # the manifests by name only: the same text from every checkout and build folder
   sources = ", ".join(m.path.name for m in manifests)
   lines: list[str] = []
   if not fragment:
      lines += ["/// @file", f"/// Generated by tools/vm_ext.py from {sources}:",
                f"/// the {vm} VM's dispatch. Do not edit; change the manifests."]
      lines += [f"#include \"{i}\"" for i in (["capsule/battle/vm.h"] if vm == "battle" else
                                              ["capsule/anim/player.h"])]
      lines += ["#include \"capsule/core/vm_ext.h\""]
      lines += [f"#include \"{i}\"" for i in d.get("includes", [])]
      lines += ["", "#include <cstdint>", "", f"namespace {namespace}", "{", "namespace", "{", ""]
   else:
      lines += [f"// Generated by tools/vm_ext.py from {sources}:",
                f"// the {vm} VM's dispatch, included by the game's source where its natives are visible.",
                "// Do not edit; change the manifests.", ""]
   ns = "vm" if vm == "battle" else "anim_vm"
   lines += [f"namespace {ns} = {'capsule::battle::vm' if vm == 'battle' else 'capsule::anim'};",
             "namespace ext = capsule::vm_ext;", ""]
   # the steps of every alias, in id order; aliases with the same steps and size share them and one function
   steps: list[str] = []
   first: dict[int, int] = {}
   shared: dict[tuple[tuple[str, ...], int | None], Op] = {}
   function: dict[int, str] = {}
   own: list[Op] = []
   for op in aliases:
      body = tuple(_step_cpp(op, op.manifest, s) for s in op.steps)
      twin = shared.get((body, op.size))
      if twin is not None:
         function[op.id] = function[twin.id]
         continue
      shared[(body, op.size)] = op
      function[op.id] = f"alias_{cpp_word(op.name)}"
      own.append(op)
      first[op.id] = len(steps)
      steps.extend(f"{INDENT}{b},   // {op.name}" for b in body)
   if aliases:
      lines += ["/// The aliases' steps (ROM).", "constexpr ext::Step kAliasSteps[] = {"] + steps + ["};", ""]
      lines += ["/// The steps and the references' resolver.",
                f"constexpr {ns}::AliasSet kAliasSet = {{kAliasSteps, sizeof(kAliasSteps) / sizeof(kAliasSteps[0]), "
                f"{resolver}}};", ""]
   arg = "vm::Machine& m, void* context" if vm == "battle" else "anim_vm::Player& p"
   call = "m, kAlias, kAliasSet, context" if vm == "battle" else "p, kAlias, kAliasSet"
   for op in own:
      twins = [o.name for o in aliases if function[o.id] == function[op.id] and o is not op]
      also = f" (also {', '.join(twins)})" if twins else ""
      lines += [f"/// {op.name} ({op.id:#04x}): an alias{also}.", f"/// @param {'m' if vm == 'battle' else 'p'}  the "
                f"{'machine' if vm == 'battle' else 'player'}"]
      if vm == "battle":
         lines += ["/// @param context  the game's"]
      lines += [f"void alias_{cpp_word(op.name)}({arg})", "{",
                f"{INDENT}constexpr ext::Alias kAlias = {{{first[op.id]}, {len(op.steps)}, {op.size}}};",
                f"{INDENT}{ns}::run_alias({call});", "}", ""]
   left_out = sorted({op.symbol for op in excluded if op.kind == "native" and op.symbol})
   if fragment and left_out:
      # a fragment's natives may be internal to the game's source: naming them here keeps them "used" for the
      # compiler, and the unused array lets the linker drop them all
      command = "vm::Command" if vm == "battle" else "anim_vm::Command"
      lines += ["/// The natives no bank uses: named only here, so the build drops them with this array.",
                f"[[maybe_unused]] constexpr {command} kExcludedNatives[] = {{"]
      lines += [f"{INDENT}{s}," for s in left_out] + ["};", ""]
   if vm == "battle":
      count = int(d.get("count", (max(ops) + 1) if ops else 0))
      entries = []
      for i in range(count):
         op = keep.get(i)
         if op is None:
            entries.append(f"{INDENT}nullptr,   // {i:#04x}" + (f" {ops[i].name} (excluded: unused)" if i in ops
                                                                    else ""))
         elif op.kind == "alias":
            entries.append(f"{INDENT}{function[op.id]},   // {i:#04x} {op.name}")
         else:
            entries.append(f"{INDENT}{op.symbol},   // {i:#04x}")
      lines += ["/// The commands by id.", f"constexpr vm::Command kCommands[{count}] = {{"] + entries + ["};", ""]
      unknown = d.get("unknown") or "nullptr"
      trace = d.get("trace") or "nullptr"
      init = f"{{kCommands, {count}, {unknown}, {trace}}}"
      if fragment:
         lines += ["/// The command table.", f"constexpr vm::CommandTable {symbol} = {init};"]
      else:
         lines += ["}   // namespace", "", f"extern const vm::CommandTable {symbol};",
                   f"const vm::CommandTable {symbol} = {init};", "", f"}}   // namespace {namespace}"]
   else:
      lines += ["/// The command table: the engine's generic commands, then the game's natives and aliases.",
                "constexpr anim_vm::CommandTable make_command_table()", "{",
                f"{INDENT}anim_vm::CommandTable t = anim_vm::generic_table();"]
      for _, op in sorted(keep.items()):
         fn = op.symbol if op.kind == "native" else function[op.id]
         lines.append(f"{INDENT}t = anim_vm::with(t, {op.id:#04x}, {fn});   // {op.name}")
      for op in sorted(excluded, key=lambda o: o.id):
         lines.append(f"{INDENT}// {op.id:#04x} {op.name}: excluded (no bank uses it)")
      lines += [f"{INDENT}return t;", "}", ""]
      if fragment:
         lines += ["/// The command table.", f"constexpr anim_vm::CommandTable {symbol} = make_command_table();"]
      else:
         lines += ["}   // namespace", "", f"extern const anim_vm::CommandTable {symbol};",
                   f"constexpr anim_vm::CommandTable {symbol} = make_command_table();", "",
                   f"}}   // namespace {namespace}"]
   text = "\n".join(lines) + "\n"
   return Dispatch(text, excluded, aliases, natives)


def exclusion_record(vm: str, excluded: Sequence[Op]) -> dict[str, Any]:
   """!The exclusion record of a dispatch: natives never compiled into the table.
   @param vm        the VM
   @param excluded  the ops left out
   @return the record
   """
   return {"schema": EXCLUSION_SCHEMA, "vm": vm,
           "excluded": [{"id": op.id, "name": op.name, "kind": op.kind, "symbol": op.symbol}
                        for op in sorted(excluded, key=lambda o: o.id)]}


# --- report --------------------------------------------------------------------------------------------------


def read_sizes(path: Path) -> dict[str, int]:
   """!Symbol sizes from `nm --size-sort -S -C` output (a GBA build's).
   @param path  the nm listing
   @return demangled function name (without parameters or namespaces) -> bytes
   """
   out: dict[str, int] = {}
   for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
      parts = line.split(maxsplit=NM_NAME)
      if len(parts) == NM_FIELDS and parts[NM_TYPE].lower() in NM_CODE_TYPES:
         # the function's own name: namespaces (an anonymous one too), parameters and clone suffixes dropped
         name = parts[NM_NAME].replace(ANONYMOUS_NAMESPACE, "").split("(")[0].split("::")[-1].split(".")[0]
         out[name] = out.get(name, 0) + int(parts[NM_SIZE], HEX_BASE)
   return out


def report(manifests: Sequence[Manifest], sizes: dict[str, int] | None = None) -> list[dict[str, Any]]:
   """!Every op with its kind, reason, owner and (natives) size on the GBA.
   @param manifests  the manifests
   @param sizes      read_sizes()'s result, or None
   @return one row per op
   """
   rows = []
   for m in manifests:
      for _, op in sorted(m.ops.items()):
         rows.append({"vm": m.vm, "manifest": m.name, "id": op.id, "name": op.name, "kind": op.kind,
                      "symbol": op.symbol, "reason": op.reason, "owner": op.owner, "tier": op.tier,
                      "waits": op.waits, "parity": op.parity, "size": op.size,
                      "gba_bytes": (sizes or {}).get(op.symbol or "") if op.kind == "native" else None})
   return rows


# --- files ---------------------------------------------------------------------------------------------------


def write_if_changed(path: Path, content: str | bytes) -> None:
   """!Writes a generated file only when its content changes (keeps builds incremental), creating its folder;
   text is written as UTF-8 with LF line ends on every system (RW3: the same bytes from every host).
   @param path     file
   @param content  text or bytes
   @return None
   """
   data = content.encode("utf-8") if isinstance(content, str) else content
   path.parent.mkdir(parents=True, exist_ok=True)
   if not path.exists() or path.read_bytes() != data:
      path.write_bytes(data)


def _json_text(value: Any) -> str:
   """!JSON as the generators write it (two-space indent, a final newline).
   @param value  the value
   @return the text
   """
   return json.dumps(value, indent=JSON_INDENT) + "\n"


def main(argv: Sequence[str] | None = None) -> int:
   """!The command line (see the module's comment).
   @param argv  arguments (default: sys.argv[1:])
   @return the exit status
   """
   p = argparse.ArgumentParser(description="VM extension manifests (capsule.vm-ext/1)")
   sub = p.add_subparsers(dest="action", required=True)
   c = sub.add_parser("check", help="check manifests (and their natives, and the banks' usage)")
   c.add_argument("manifests", type=Path, nargs="+")
   c.add_argument("--sources", type=Path, nargs="*", default=[])
   c.add_argument("--usage", type=Path, nargs="*", default=[])
   c.add_argument("--warnings-as-errors", action="store_true")
   g = sub.add_parser("dispatch", help="write a VM's dispatch as C++")
   g.add_argument("manifests", type=Path, nargs="+")
   g.add_argument("--out", type=Path, required=True)
   g.add_argument("--usage", type=Path, nargs="*", default=[])
   g.add_argument("--exclusions", type=Path)
   g.add_argument("--depfile", type=Path)
   lay = sub.add_parser("layouts", help="write the script bank command list")
   lay.add_argument("manifests", type=Path, nargs="+")
   lay.add_argument("--out", type=Path, required=True)
   lay.add_argument("--cpp", action="store_true", help="write a C++ header of the layouts instead of JSON")
   lay.add_argument("--namespace", default="layouts")
   r = sub.add_parser("report", help="list the ops with their kinds, reasons, owners and sizes")
   r.add_argument("manifests", type=Path, nargs="+")
   r.add_argument("--sizes", type=Path)
   r.add_argument("--json", action="store_true")
   args = p.parse_args(argv)
   try:
      manifests = load_all(args.manifests)
      if args.action == "check":
         errors, warnings = check(manifests, args.sources, read_usage(args.usage) if args.usage else None)
         for w in warnings:
            print(f"warning: {w}")
         for e in errors:
            print(f"error: {e}")
         ops = sum(len(m.ops) for m in manifests)
         aliases = sum(1 for m in manifests for op in m.ops.values() if op.kind == "alias")
         print(f"vm_ext: {len(manifests)} manifests, {ops} ops ({aliases} aliases, {ops - aliases} natives), "
               f"{len(errors)} errors, {len(warnings)} warnings")
         return 1 if errors or (warnings and args.warnings_as_errors) else 0
      if args.action == "dispatch":
         usage = read_usage(args.usage) if args.usage else None
         d = generate_dispatch(manifests, args.out, usage)
         write_if_changed(args.out, d.text)
         if args.exclusions:
            write_if_changed(args.exclusions, _json_text(exclusion_record(manifests[0].key, d.excluded)))
         if args.depfile:
            deps = " ".join(_relative(m.path, Path.cwd()) for m in manifests)
            write_if_changed(args.depfile, f"{_relative(args.out, Path.cwd())}: {deps}\n")
         return 0
      if args.action == "layouts":
         if args.cpp:
            write_if_changed(args.out, layout_header(manifests, args.namespace))
         else:
            write_if_changed(args.out, _json_text({"schema": "capsule.script-bank/1",
                                                   "commands": script_bank_commands(manifests)}))
         return 0
      rows = report(manifests, read_sizes(args.sizes) if args.sizes else None)
      if args.json:
         print(_json_text(rows), end="")
      else:
         for row in rows:
            size = f"{row['gba_bytes']} B" if row["gba_bytes"] is not None else "-"
            print(f"{row['vm']:6} {row['id']:#04x} {row['name']:32} {row['kind']:6} {size:>8}  {row['reason']}")
   except (ManifestError, OSError, KeyError, json.JSONDecodeError) as e:
      print(f"vm_ext: {e}", file=sys.stderr)
      return 1
   return 0


if __name__ == "__main__":
   sys.exit(main())
