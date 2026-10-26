"""!Save shapes of a script bank (C6, review4 finding 1): how the save image's Script section (capsule/save/save.h)
keeps the flags, variables and name slots, and how an older save finds its way into a newer build.

The bank's storage is saved as sub-banks (capsule::script::kSaveSubBanks): the flags of each lifetime, the
variables of each lifetime, then the name slots. A sub-bank's shape is its entries in storage order (a flag's
name; a variable's name, byte offset, width and sign, then its range when it is not its type's default range (C7: a
narrowed range changes the shape, so the remap clamps the old values); a name slot's placeholder name,
offset and capacity) and its shape hash is FNV-1a 32 over them (0 for an empty sub-bank). The save stores each
sub-bank's hash with its bytes. A history may hold variable entries without a range (recorded before C7).

Migration (the cheapest correct option for the GBA: four bytes of hash per sub-bank in the save, nothing per entry):
a game records the shapes it ships in its save history, the JSON file the scripts manifest's "save_history" names
(default: save_history.json beside the manifest, read when it exists). The script compiler emits a remap for every
recorded shape that differs from the build's (capsule::script::SaveRemap / SaveMove): each old entry's new place by
name (a flag may change lifetime), or dropped. Loading a sub-bank:
  - its hash is the build's: the bytes are copied;
  - a remap names its hash: the entries move (variables clamp to their new range and width, names truncate);
  - else, for a transient lifetime (daily, area, map) the sub-bank is reset (cleared) and for the permanent
    lifetime or the name slots the save is refused (capsule::save::Status::OtherShape).
Record a release's shapes before changing flags or variables: `save_shape.py record scripts.json`. C7: once a
game keeps a history (a file that records shapes), every build checks that each non-empty sub-bank's shape is
recorded (unrecorded(); tools/save_layout.py fails the build), so no shipped shape is left without a remap.

Usage:
  save_shape.py show scripts.json     the build's sub-banks (hash, bytes, entries) and the recorded shapes
  save_shape.py record scripts.json   adds the build's shapes the history does not hold yet
  save_shape.py check scripts.json    exit status 1 when the history lacks one of the build's shapes
"""
from __future__ import annotations

import json
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

## History file format.
HISTORY_FORMAT = "capsule.save_history/1"
## Manifest key naming the history file.
HISTORY_KEY = "save_history"
## History file name when the manifest names none.
DEFAULT_HISTORY = "save_history.json"
## Lifetimes in bank order (script_compiler.LIFETIMES).
LIFETIMES = ("permanent", "daily", "area", "map")
## Lifetimes whose sub-banks reset when a save's shape has no remap.
TRANSIENT = ("daily", "area", "map")
## Sub-bank kinds.
FLAGS, VARS, NAMES = "flags", "vars", "names"
## FNV-1a 32 offset basis.
FNV_BASIS = 0x811C9DC5
## FNV-1a 32 prime.
FNV_PRIME = 0x01000193
## 32-bit mask.
U32_MASK = 0xFFFFFFFF
## capsule::script::kSaveDropped: an entry the build no longer has.
DROPPED = 0xFFFF
## Separator between hashed entries.
ENTRY_SEPARATOR = b"\n"
## Most entries one remap list holds (16-bit indices).
MAX_MOVES = 0xFFFF
## A name slot's placeholder kind (script_compiler.PLACEHOLDER_KINDS.index("name")).
NAME_PLACEHOLDER_KIND = 0
## Fields of a variable entry before its range (name, offset, width, signed).
VAR_SHAPE_FIELDS = 4
## Fields of a variable entry with its range (then lo, hi).
VAR_RANGED_FIELDS = 6
## A variable's range when its declaration gives none, by width (script_compiler.TYPES u8 / u16 / u32).
DEFAULT_RANGES = {1: (0, 0xFF), 2: (0, 0xFFFF), 4: (0, 0x7FFFFFFF)}
## Commands of the command line.
COMMANDS = ("show", "record", "check")
## Command-line words: the command and the manifest.
ARGUMENTS = 3
## Exit status of a command line that is not understood.
EXIT_USAGE = 2
## Exit status of a failed command.
EXIT_FAILED = 1


class ShapeError(Exception):
   """!A history file or a manifest that cannot be used."""


def sub_bank_names() -> list[str]:
   """!The sub-banks' labels in capsule::script order (flags per lifetime, variables per lifetime, names).
   @return labels
   """
   return [f"{FLAGS}.{life}" for life in LIFETIMES] + [f"{VARS}.{life}" for life in LIFETIMES] + [NAMES]


def fnv(data: bytes) -> int:
   """!FNV-1a 32.
   @param data  bytes
   @return the hash
   """
   h = FNV_BASIS
   for b in data:
      h = ((h ^ b) * FNV_PRIME) & U32_MASK
   return h


def shape_hash(kind: str, entries: Sequence[Any]) -> int:
   """!A sub-bank's shape hash.
   @param kind     FLAGS, VARS or NAMES
   @param entries  its entries (flags: names; variables: [name, offset, width, signed]; names: [name, offset,
                   capacity])
   @return the hash (0 without entries)
   """
   if not entries:
      return 0
   parts = [kind.encode("utf-8")]
   for e in entries:
      parts.append((e if isinstance(e, str) else ":".join(str(x) for x in e)).encode("utf-8"))
   return fnv(ENTRY_SEPARATOR.join(parts))


def default_range(width: int) -> tuple[int, int] | None:
   """!The range a variable of a width has when its declaration gives none (script_compiler.TYPES).
   @param width  bytes
   @return (lo, hi), or None for a width without a type
   """
   return DEFAULT_RANGES.get(width)


def var_entry(name: str, offset: int, width: int, lo: int, hi: int) -> list[Any]:
   """!A variable's shape entry (C7: its range only when it is not its type's default range, so the shapes of
   variables declared without a range keep their C6 hash).
   @param name    the variable
   @param offset  its byte offset in the sub-bank
   @param width   its bytes
   @param lo      its range's low end (negative: signed)
   @param hi      its range's high end
   @return [name, offset, width, signed] or [name, offset, width, signed, lo, hi]
   """
   entry: list[Any] = [name, offset, width, 1 if lo < 0 else 0]
   if (lo, hi) != default_range(width):
      entry += [lo, hi]
   return entry


def shapes(comp: Any) -> list[dict[str, Any]]:
   """!The build's sub-banks.
   @param comp  a script_compiler.Compiler that compiled (result set)
   @return [{"bank", "kind", "lifetime", "bytes", "entries", "hash"}] in sub-bank order
   """
   r = comp.result
   out = []
   for li, life in enumerate(LIFETIMES):
      names = [n for n in r["flag_names"] if comp.flags[n]["lifetime"] == life]
      out.append({"bank": f"{FLAGS}.{life}", "kind": FLAGS, "lifetime": life, "bytes": r["flag_bytes"][li],
                  "entries": names})
   var_names = r["var_names"]
   for li, life in enumerate(LIFETIMES):
      # [name, offset, width, signed(, lo, hi)]: a signed variable's old bytes are sign-extended when it moves
      entries = sorted((var_entry(var_names[i], off, w, lo, hi)
                        for i, (lo, hi, off, vl, w) in enumerate(r["vars"]) if vl == li), key=lambda e: e[1])
      out.append({"bank": f"{VARS}.{life}", "kind": VARS, "lifetime": life, "bytes": r["var_bytes"][li],
                  "entries": entries})
   names = []
   for pname, (kind, cap, _i, off) in zip(r["placeholder_names"], r["placeholders"]):
      if kind == NAME_PLACEHOLDER_KIND:
         names.append([pname, off, cap])
   out.append({"bank": NAMES, "kind": NAMES, "lifetime": None, "bytes": r["name_bytes"], "entries": names})
   for s in out:
      s["hash"] = shape_hash(s["kind"], s["entries"])
   return out


def history_path(manifest: str | Path, data: Mapping[str, Any]) -> Path:
   """!The history file of a scripts manifest.
   @param manifest  the manifest's path
   @param data      its JSON
   @return the path ("save_history", else DEFAULT_HISTORY beside it)
   """
   name = data.get(HISTORY_KEY, DEFAULT_HISTORY)
   return Path(manifest).parent / str(name)


def load_history(path: str | Path) -> list[dict[str, Any]]:
   """!A history file's recorded shapes.
   @param path  the file (a missing one holds none)
   @return [{"bank", "hash", "bytes", "entries"}]
   @throws ShapeError for a file that is not a history
   """
   p = Path(path)
   if not p.exists():
      return []
   try:
      data = json.loads(p.read_text(encoding="utf-8"))
   except (OSError, ValueError) as e:
      raise ShapeError(f"{p}: {e}") from e
   if not isinstance(data, dict) or data.get("format") != HISTORY_FORMAT or not isinstance(data.get("shapes"), list):
      raise ShapeError(f"{p}: not a {HISTORY_FORMAT} file")
   labels = sub_bank_names()
   for k, s in enumerate(data["shapes"]):
      if not isinstance(s, dict) or s.get("bank") not in labels or not isinstance(s.get("entries"), list):
         raise ShapeError(f"{p}: shapes[{k}]: a bank ({', '.join(labels)}) and its entries")
      kind = s["bank"].split(".")[0]
      if kind == VARS and not all(isinstance(e, list) and len(e) in (VAR_SHAPE_FIELDS, VAR_RANGED_FIELDS)
                                  for e in s["entries"]):
         raise ShapeError(f"{p}: shapes[{k}]: variables are [name, offset, width, signed] or [..., lo, hi]")
      s["hash"] = shape_hash(kind, s["entries"])
      s.setdefault("bytes", 0)
   return data["shapes"]


def unrecorded(comp: Any, history: Sequence[Mapping[str, Any]]) -> list[str]:
   """!The build's non-empty sub-banks whose shape the history does not record (C7, the build check: a shape that
   ships unrecorded leaves its saves without a remap once it changes again). A game without a history (no file, or
   one that records nothing) is not checked.
   @param comp     a compiled script_compiler.Compiler
   @param history  load_history() result
   @return the sub-banks' labels ([] when every shape is recorded)
   """
   if not history:
      return []
   known = {(s["bank"], s["hash"]) for s in history}
   return [s["bank"] for s in shapes(comp) if s["hash"] != 0 and (s["bank"], s["hash"]) not in known]


def record(comp: Any, path: str | Path) -> int:
   """!Adds the build's shapes the history does not hold (empty sub-banks are not recorded).
   @param comp  a compiled script_compiler.Compiler
   @param path  the history file (created when missing)
   @return shapes added
   """
   old = load_history(path)
   known = {(s["bank"], s["hash"]) for s in old}
   new = [s for s in shapes(comp) if s["hash"] != 0 and (s["bank"], s["hash"]) not in known]
   if not new:
      return 0
   kept = [{"bank": s["bank"], "bytes": s["bytes"], "entries": s["entries"]} for s in old]
   kept += [{"bank": s["bank"], "bytes": s["bytes"], "entries": s["entries"]} for s in new]
   Path(path).write_text(json.dumps({"format": HISTORY_FORMAT, "shapes": kept}, indent=1) + "\n", encoding="utf-8")
   return len(new)


def remaps(comp: Any, history: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
   """!The remap of every recorded shape that is not the build's: each old entry's new place by name.
   @param comp     a compiled script_compiler.Compiler
   @param history  load_history() result
   @return [{"sub_bank", "bank", "from_hash", "from_bytes", "moves": [(from, to, from_size, to_size)]}] (moves
           sorted by from)
   """
   current = shapes(comp)
   labels = sub_bank_names()
   r = comp.result
   var_ids = {n: i for i, n in enumerate(r["var_names"])}
   slots = {name: (off, cap) for name, off, cap in current[labels.index(NAMES)]["entries"]}
   out = []
   seen = set()
   for s in history:
      k = labels.index(s["bank"])
      if s["hash"] == 0 or s["hash"] == current[k]["hash"] or (k, s["hash"]) in seen:
         continue
      seen.add((k, s["hash"]))
      kind = s["bank"].split(".")[0]
      moves = []
      if kind == FLAGS:
         for bit, name in enumerate(s["entries"]):
            moves.append((bit, comp.flag_ids.get(name, DROPPED), 1, 0))
      elif kind == VARS:
         for e in s["entries"]:
            # the range (when recorded) is the old one: the engine clamps to the build's (put_var())
            name, off, width, signed = e[:VAR_SHAPE_FIELDS]
            moves.append((off, var_ids.get(name, DROPPED), width, signed))
      else:
         for name, off, cap in s["entries"]:
            to, new_cap = slots.get(name, (DROPPED, 0))
            moves.append((off, to, cap, new_cap))
      moves.sort()
      out.append({"sub_bank": k, "bank": s["bank"], "from_hash": s["hash"], "from_bytes": int(s.get("bytes", 0)),
                  "moves": moves})
   return out


def emit(comp: Any, history: Sequence[Mapping[str, Any]], lines: list[str]) -> tuple[str, str, int]:
   """!The bank's save tables (appended to the generated source's anonymous namespace).
   @param comp     a compiled script_compiler.Compiler
   @param history  load_history() result
   @param lines    the source's lines (the arrays are appended)
   @return (remaps array name or "nullptr", moves array name or "nullptr", remap count)
   @throws ShapeError when the remaps hold more entries than 16-bit indices reach
   """
   rm = remaps(comp, history)
   if not rm:
      return "nullptr", "nullptr", 0
   moves = []
   heads = []
   for m in rm:
      heads.append(f"    {{0x{m['from_hash']:08X}u, {m['from_bytes']}, {len(moves)}, {len(m['moves'])}, "
                   f"{m['sub_bank']}, 0}},   // {m['bank']}")
      moves += m["moves"]
   if len(moves) > MAX_MOVES:
      raise ShapeError(f"the save history's remaps hold {len(moves)} entries; at most {MAX_MOVES}")
   lines.append("// C6: the recorded older save shapes (tools/save_shape.py) and where their entries went")
   lines.append("const SaveMove kSaveMoves[] = {")
   lines += [f"    {{{f}, 0x{t:04X}, {fs}, {ts}, {{0, 0}}}}," for f, t, fs, ts in moves]
   lines.append("};")
   lines.append("const SaveRemap kSaveRemaps[] = {")
   lines += heads
   lines.append("};")
   return "kSaveRemaps", "kSaveMoves", len(rm)


def hashes_initializer(comp: Any) -> str:
   """!The Bank::save_hashes initializer.
   @param comp  a compiled script_compiler.Compiler
   @return "{0x..u, ...}"
   """
   return "{" + ", ".join(f"0x{s['hash']:08X}u" for s in shapes(comp)) + "}"


def main(argv: Sequence[str]) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return process exit status
   """
   if len(argv) != ARGUMENTS or argv[1] not in COMMANDS:
      print(__doc__)
      return EXIT_USAGE
   sys.path.insert(0, str(Path(__file__).resolve().parent))
   import script_compiler as sc
   manifest = argv[-1]
   comp = sc.compile_file(manifest)
   if comp.errors():
      print(f"{manifest}: error: the scripts do not compile ({comp.errors()[0].message})", file=sys.stderr)
      return EXIT_FAILED
   try:
      data = json.loads(Path(manifest).read_text(encoding="utf-8"))
      path = history_path(manifest, data)
      if argv[1] == "record":
         print(f"{path}: {record(comp, path)} shapes added")
         return 0
      history = load_history(path)
   except (OSError, ValueError, ShapeError) as e:
      print(f"error: {e}", file=sys.stderr)
      return EXIT_FAILED
   if argv[1] == "check":
      missing = unrecorded(comp, history)
      for bank in missing:
         print(f"{path}: error: the {bank} save shape is not recorded (save_shape.py record {manifest})",
               file=sys.stderr)
      return EXIT_FAILED if missing else 0
   for s in shapes(comp):
      print(f"{s['bank']:16} 0x{s['hash']:08X} {s['bytes']} B {len(s['entries'])} entries")
   for m in remaps(comp, history):
      print(f"recorded {m['bank']:7} 0x{m['from_hash']:08X} {m['from_bytes']} B -> "
            f"{sum(1 for mv in m['moves'] if mv[1] != DROPPED)} kept, "
            f"{sum(1 for mv in m['moves'] if mv[1] == DROPPED)} dropped")
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv))
