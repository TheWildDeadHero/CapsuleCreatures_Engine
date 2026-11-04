#!/usr/bin/env python3
"""!The field script VM's opcode table (D-36 package 2, step A; docs/VM_EXT.md): one manifest,
src/script/script_vm.json (capsule.vm-ext/1, vm "script"), defines the VM's opcodes in their bytecode numbering, the
kind enums their operands select (with each value's script word), and the operand constants. Everything else is
generated from it or reads it:

    script_vm.py header [--manifest FILE] [--out FILE] [--check]

writes include/capsule/script/ops.h (the C++ enums and constants; capsule/script/data.h includes it); with --check it
only says whether that file is up to date (exit 1 when not). tools/script_compiler.py takes its opcode list, kind
words and constants from table().

The ids and the value orders are the bytecode: a change renumbers every bank, so the manifest only ever grows at the
end of a list.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Sequence
from functools import lru_cache
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vm_ext  # noqa: E402

## The engine's root.
ROOT = Path(__file__).resolve().parent.parent
## The manifest.
MANIFEST = ROOT / "src" / "script" / "script_vm.json"
## The header it generates.
HEADER = ROOT / "include" / "capsule" / "script" / "ops.h"
## The VM the manifest describes.
SCRIPT_VM = "script"
## The C++ namespace of the generated header.
NAMESPACE = "capsule::script"
## Indentation of the generated C++ (the project's 3 spaces).
INDENT = "   "
## Separator between an entry and its trailing documentation in the generated C++.
DOC_GAP = "   "
## Exit status of a stale header under --check.
EXIT_STALE = 1


class ScriptVmError(Exception):
   """!A manifest the table cannot use."""


class ScriptVm:
   """!The table, as the script compiler uses it."""

   def __init__(self, raw: dict[str, Any]) -> None:
      """!Reads the manifest's JSON.
      @param raw  the manifest
      @throws ScriptVmError when an id is missing or out of order, or a kind enum has no words
      """
      ## The manifest's JSON.
      self.raw = raw
      ops = sorted(raw["ops"], key=lambda o: o["id"])
      if [o["id"] for o in ops] != list(range(len(ops))):
         raise ScriptVmError("the script VM's opcodes must be numbered 0 to n - 1 without gaps")
      ## Opcode names in id order.
      self.ops: list[str] = [o["name"] for o in ops]
      ## The kind enums by C++ name: their values (with "word" and the extra keys) in order.
      self.enums: dict[str, list[dict[str, Any]]] = {e["name"]: e["values"] for e in raw["enums"]}
      ## Constants by C++ name -> value.
      self.constants: dict[str, int] = {}
      for group in raw["constants"]:
         for item in group["items"]:
            self.constants[item["name"]] = int(item["value"], 0)

   def words(self, enum: str) -> list[str]:
      """!A kind enum's script words in value order.
      @param enum  its C++ name
      @return the words
      @throws ScriptVmError for an enum the manifest does not hold
      """
      if enum not in self.enums:
         raise ScriptVmError(f"the script VM's manifest has no enum {enum}")
      return [v["word"] for v in self.enums[enum]]

   def word_values(self, enum: str, key: str) -> dict[str, Any]:
      """!A kind enum's words with one extra key of each value (Query's "object", SoundOp's "operands").
      @param enum  its C++ name
      @param key   the key
      @return word -> value, in value order
      """
      return {v["word"]: v[key] for v in self.enums[enum]}


@lru_cache(maxsize=None)
def table(path: str = str(MANIFEST)) -> ScriptVm:
   """!The table of a manifest (read and checked once per process).
   @param path  the manifest
   @return the table
   @throws ScriptVmError, vm_ext.ManifestError on a bad manifest
   """
   raw = json.loads(Path(path).read_text(encoding="utf-8"))
   m = vm_ext._manifest_from(raw, Path(path))   # the shared checks: schema, ids, names, kinds
   if m.vm != SCRIPT_VM:
      raise ScriptVmError(f"{path}: not a {SCRIPT_VM} VM manifest")
   return ScriptVm(raw)


def doc_lines(text: str, indent: str = "") -> list[str]:
   """!A documentation comment.
   @param text    the text (lines separated by newlines)
   @param indent  the indentation
   @return the `///` lines
   """
   return [f"{indent}/// {line}".rstrip() for line in text.split("\n")]


def entry_line(name: str, doc: str, last: bool = False) -> list[str]:
   """!One enumerator with its trailing documentation (a second line of documentation continues below it).
   @param name  the enumerator
   @param doc   its documentation
   @param last  no comma (the count)
   @return the lines
   """
   first, *rest = doc.split("\n")
   head = f"{INDENT}{name}{'' if last else ','}"
   out = [f"{head}{DOC_GAP}///< {first}"]
   out += [f"{' ' * len(head)}{DOC_GAP}///< {line}" for line in rest]
   return out


def enum_lines(name: str, kind: str, doc: Sequence[str], values: Sequence[dict[str, Any]],
               count: dict[str, str]) -> list[str]:
   """!One enum.
   @param name    its name
   @param kind    its underlying type
   @param doc     its documentation lines
   @param values  its values ({"name", "doc"[, "comment"]})
   @param count   its count enumerator ({"name", "doc"})
   @return the lines
   """
   out = [f"/// {line}".rstrip() for line in doc]
   out += [f"enum class {name} : {kind}", "{"]
   for v in values:
      if "comment" in v:
         out.append(f"{INDENT}// {v['comment']}")
      out += entry_line(v["name"], v["doc"])
   out += entry_line(count["name"], count["doc"], last=True)
   out.append("};")
   return out


def op_table_lines(ops: Sequence[dict[str, Any]]) -> list[str]:
   """!The file comment's opcode table (consecutive ops with the same bytes and action share a row).
   @param ops  the ops in id order
   @return the lines
   """
   out = ["/// | Op | Bytes | Does |", "/// |---|---|---|"]
   i = 0
   while i < len(ops):
      j = i + 1
      while j < len(ops) and (ops[j]["bytes"], ops[j]["does"]) == (ops[i]["bytes"], ops[i]["does"]):
         j += 1
      names = " / ".join(o["name"] for o in ops[i:j])
      does = f" {ops[i]['does']} " if ops[i]["does"] else " "
      out.append(f"/// | {names} | {ops[i]['bytes']} |{does}|")
      i = j
   return out


def header_text(raw: dict[str, Any], manifest_rel: str) -> str:
   """!include/capsule/script/ops.h.
   @param raw           the manifest
   @param manifest_rel  the manifest's path relative to the engine (for the comment)
   @return the header's text
   """
   ops = sorted(raw["ops"], key=lambda o: o["id"])
   out = ["/// @file",
          f"/// The field script VM's opcodes, kind enums and operand constants. Generated by tools/script_vm.py from",
          f"/// {manifest_rel}: do not edit; change the manifest and run `python3 tools/script_vm.py header`.",
          "/// The ids and value orders are the bytecode (capsule/script/data.h describes the operands).",
          "///",
          "/// Opcodes and their bytes after the opcode:",
          "///"]
   out += op_table_lines(ops)
   out += ["#pragma once", "", "#include <cstdint>", "", f"namespace {NAMESPACE}", "{", ""]
   op_enum = raw["op_enum"]
   out += enum_lines("Op", "uint8_t", op_enum["doc"], ops, op_enum["count"])
   for e in raw["enums"]:
      out.append("")
      out += enum_lines(e["name"], e["type"], e["doc"], e["values"], e["count"])
   for group in raw["constants"]:
      out += ["", f"/// @name {group['title']}", "///@{"]
      for item in group["items"]:
         first, *rest = item["doc"].split("\n") if item["doc"] else ("",)
         decl = f"inline constexpr {item['type']} {item['name']} = {item['value']};"
         if rest:   # a long documentation goes above the declaration
            out += doc_lines(item["doc"])
            out.append(decl)
         else:
            out.append(f"{decl}{DOC_GAP}///< {first}" if first else decl)
      out.append("///@}")
   out += ["", f"}}   // namespace {NAMESPACE}", ""]
   return "\n".join(out)


def main(argv: Sequence[str] | None = None) -> int:
   """!Command line; see the module docstring.
   @param argv  the arguments
   @return 0, or EXIT_STALE when --check finds the header out of date
   """
   ap = argparse.ArgumentParser(description="The field script VM's opcode table.")
   sub = ap.add_subparsers(dest="command", required=True)
   h = sub.add_parser("header", help="write (or check) include/capsule/script/ops.h")
   h.add_argument("--manifest", type=Path, default=MANIFEST)
   h.add_argument("--out", type=Path, default=HEADER)
   h.add_argument("--check", action="store_true", help="only check that the header is up to date")
   args = ap.parse_args(argv)
   try:
      t = table(str(args.manifest))
   except (ScriptVmError, vm_ext.ManifestError, OSError, ValueError) as e:
      print(f"script_vm.py: {e}", file=sys.stderr)
      return EXIT_STALE
   rel = args.manifest.resolve().relative_to(ROOT).as_posix() if args.manifest.resolve().is_relative_to(ROOT) \
      else args.manifest.name
   text = header_text(t.raw, rel)
   current = args.out.read_text(encoding="utf-8") if args.out.exists() else None
   if args.check:
      if current != text:
         print(f"{args.out} is out of date: run `python3 tools/script_vm.py header`", file=sys.stderr)
         return EXIT_STALE
      return 0
   if current != text:
      args.out.write_bytes(text.encode("utf-8"))
   return 0


if __name__ == "__main__":
   sys.exit(main())
