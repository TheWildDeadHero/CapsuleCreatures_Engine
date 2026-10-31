"""!The engine save image's sizes from a game's manifests (D-26 decision 1, package G2; versioned sections C6): the
same sections as capsule/save/save.h (src/save/save.cpp), worked out at build time and checked against the
platform's save medium, and the same bytes (image(): the studio's test saves, the tests' older-shape images).

Sections (most bytes; each section carries its own shape, so a save survives manifest changes: save.h):
  - Bag: 2 (entry count), then 6 per manifest slot (the item's stable key and count; written: the filled ones);
  - Party: 7 (record hash: the layout's without its party size, C7; record bytes, count), then party_size records
    of the layout's record bytes;
  - Boxes (G3): 10 (record hash, record bytes, box count, box size, current box), a bit per slot (low bit first),
    then every slot's record (written: the used ones); absent without storage boxes;
  - Script: 1 (sub-bank count), then per non-empty sub-bank 7 (index, shape hash, bytes) and its bytes
    (tools/save_shape.py); absent without script storage;
  - Nursery (B2): 9 (record hash, record bytes, slots, used bits, result waiting), then per slot 5 (its level when
    left, its gained experience) and its record (written: the used ones), then the waiting result's record;
    absent without a nursery.
The image adds a 32-byte header and 12 bytes per section in its table.

Media (C6, review4 finding 4): "gba" is the cartridge's 64 KB flash (4 KB sectors, each copy whole sectors),
"gba_sram" its 32 KB SRAM (byte units; CMake CAPSULE_GBA_SAVE_SRAM), every other platform its max_save_bytes in
byte units. The medium must keep two copies (a write cut short then leaves the older one), unless the game opts in
to one (project.json "save": {"single_copy": true}, --single-copy).

Copies (C7, final review): copy k lies at k * slot_bytes(), the medium split in MAX_COPIES slots of whole erase units,
so where a copy lies depends on the medium alone (an image of another size, written by an older build, is still
found). An image larger than its slot holds one copy (the whole medium): a build error unless --single-copy.

Script save shapes (C7): with --scripts, a game that keeps a save history must have recorded every non-empty
sub-bank's shape (tools/save_shape.py unrecorded()); the build fails otherwise.

Usage:
  save_layout.py battle.json [--scripts scripts.json] [--platform P] [--single-copy] [--out report.txt]
prints (and writes) the report; exit status 1 when the image is over the medium, the medium keeps fewer than two
copies without --single-copy, a script save shape is not recorded in the game's save history, or a manifest is
invalid.
"""
from __future__ import annotations

import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import battle_compiler as bc  # noqa: E402
import build_settings  # noqa: E402
import save_shape  # noqa: E402

## Header bytes (capsule::save::kHeaderBytes).
HEADER_BYTES = 32
## Bytes per section table entry (kSectionEntryBytes).
SECTION_ENTRY_BYTES = 12
## Bag: bytes of the entry count (kBagHeadBytes).
BAG_HEAD_BYTES = 2
## Bag: bytes per entry (kBagEntryBytes: item key 4, count 2).
BAG_ENTRY_BYTES = 6
## Party: bytes before the records (kPartyHeadBytes: record hash 4 (C7: the layout's record_hash, no party
## size), record bytes 2, count 1).
PARTY_HEAD_BYTES = 7
## Boxes: bytes before the used bits (kBoxesHeadBytes: record hash 4, record bytes 2, box count 1, box size 2,
## current box 1).
BOXES_HEAD_BYTES = 10
## Script: bytes of the sub-bank count (kScriptHeadBytes).
SCRIPT_HEAD_BYTES = 1
## Nursery (B2): the record hash (4), record bytes (2), slots, used bits, result waiting (capsule::save::
## kNurseryHeadBytes).
NURSERY_HEAD_BYTES = 9
## Nursery: per used slot its level (1) and gained experience (4) before its record (kNurserySlotBytes).
NURSERY_SLOT_BYTES = 5
## Script: bytes before each sub-bank's storage (kSubBankHeadBytes: index 1, hash 4, bytes 2).
SUB_BANK_HEAD_BYTES = 7
## Bits per byte of the storage's used bits (capsule::storage::kUsedBitsPerByte).
USED_BITS_PER_BYTE = 8
## Section names in id order (capsule::save::Section).
SECTIONS = ("bag", "party", "boxes", "script", "nursery", "game")
## Each section's version (src/save/save.cpp k*Version).
SECTION_VERSIONS = {"bag": 1, "party": 1, "boxes": 1, "script": 1, "nursery": 1, "game": 1}
## N3: the Game section's head: the game's version of its block (2) and its bytes (4).
GAME_HEAD_BYTES = 6
## Copies the medium keeps at most.
MAX_COPIES = 2
## A variable's shape entry without a range (save_shape.var_entry(): name, offset, width, signed).
VAR_ENTRY_PLAIN = ("name", "offset", "width", "signed")
## Copies a game needs unless it opts in to one.
REQUIRED_COPIES = 2
## Save media by save platform: (the engine platform it compiles for, bytes, erase unit).
SAVE_MEDIA = {"gba": ("gba", 0x10000, 0x1000), "gba_sram": ("gba", 0x8000, 1)}
## Command-line words of an option with a value (the flag and its value).
OPTION_WORDS = 2
## Exit status of a command line that is not understood.
EXIT_USAGE = 2
## Exit status of an image over its budget, too few copies, or an invalid manifest.
EXIT_FAILED = 1


def compile_scripts(scripts: str | Path) -> Any:
   """!The scripts manifest compiled.
   @param scripts  the scripts manifest (capsule.scripts/1)
   @return the script_compiler.Compiler
   @throws bc.ManifestError when the scripts do not compile
   """
   import script_compiler as sc
   comp = sc.compile_file(scripts)
   if comp.errors():
      raise bc.ManifestError(f"{scripts}: the scripts do not compile ({comp.errors()[0].message})")
   return comp


def script_banks(scripts: str | Path | None) -> list[dict[str, Any]]:
   """!The scripts manifest's sub-banks (tools/save_shape.py shapes()).
   @param scripts  the scripts manifest (capsule.scripts/1), or None
   @return the sub-banks ([] without scripts)
   @throws bc.ManifestError when the scripts do not compile
   """
   if scripts is None:
      return []
   return save_shape.shapes(compile_scripts(scripts))


def slot_bytes(budget: int, unit: int) -> int:
   """!Bytes of one copy's slot (C7; capsule::save slot_bytes()): the medium split in MAX_COPIES whole erase units.
   @param budget  the medium's bytes
   @param unit    its erase unit
   @return bytes (copy k lies at k times them)
   """
   return budget // MAX_COPIES // unit * unit


def copies_of(size: int, budget: int, unit: int) -> int:
   """!Copies a medium keeps of an image (capsule::save::copy_count()).
   @param size    the image's most bytes
   @param budget  the medium's bytes
   @param unit    its erase unit
   @return MAX_COPIES when the image fits its slot, 1 when it fits the medium, else 0
   """
   need = (size + unit - 1) // unit * unit
   slot = slot_bytes(budget, unit)
   if slot and need <= slot:
      return MAX_COPIES
   return 1 if need <= budget else 0


def script_bytes(banks: Sequence[Mapping[str, Any]]) -> int:
   """!The Script section's size.
   @param banks  script_banks() result
   @return bytes (0 without script storage)
   """
   n = sum(SUB_BANK_HEAD_BYTES + b["bytes"] for b in banks if b["bytes"])
   return SCRIPT_HEAD_BYTES + n if n else 0


def sections_of(compiled: Mapping[str, Any], banks: Sequence[Mapping[str, Any]] = (),
                game_bytes: int = 0) -> dict[str, int]:
   """!The most bytes of each section of a compiled battle and its scripts.
   @param compiled    bc.compile_battle() result
   @param banks       script_banks() result
   @param game_bytes  N3: the game's block (capsule::save::game_block_bytes()), 0 without one
   @return {section: bytes} in id order (0: no such section; "game" only with a game block)
   """
   inv = compiled["inventory"]
   lay = compiled["layout"]
   out = {"bag": BAG_HEAD_BYTES + inv["slot_count"] * BAG_ENTRY_BYTES,
          "party": PARTY_HEAD_BYTES + lay["party_size"] * lay["bytes"],
          "boxes": boxes_bytes(compiled),
          "script": script_bytes(banks),
          "nursery": nursery_bytes(compiled)}
   if game_bytes:
      out["game"] = GAME_HEAD_BYTES + game_bytes
   return out


def nursery_bytes(compiled: Mapping[str, Any]) -> int:
   """!The Nursery section's most bytes (B2): its head, every slot's level, experience and record, the result's
   record; 0 without a nursery.
   @param compiled  bc.compile_battle() result
   @return bytes
   """
   n = compiled["combine"].get("nursery")
   if n is None:
      return 0
   size = compiled["layout"]["bytes"]
   return NURSERY_HEAD_BYTES + n["slots"] * (NURSERY_SLOT_BYTES + size) + size


def boxes_bytes(compiled: Mapping[str, Any]) -> int:
   """!The Boxes section's most bytes (G3): its head, the used bits, every slot's record; 0 without boxes.
   @param compiled  bc.compile_battle() result
   @return bytes
   """
   st = compiled.get("storage") or {"slots": 0}
   slots = st["slots"]
   if slots == 0:
      return 0
   return BOXES_HEAD_BYTES + bc.box_used_bytes(slots) + slots * compiled["layout"]["bytes"]


def image_bytes(sections: Mapping[str, int]) -> int:
   """!The whole image's most bytes.
   @param sections  sections_of() result
   @return bytes (header, table, sections)
   """
   return HEADER_BYTES + sum(SECTION_ENTRY_BYTES + size for size in sections.values() if size)


def medium(platform: str) -> tuple[str, int, int]:
   """!A save platform's medium.
   @param platform  "gba", "gba_sram" or a build_settings platform
   @return (engine platform, bytes, erase unit)
   @throws build_settings.SettingsError for an unknown platform
   """
   if platform in SAVE_MEDIA:
      return SAVE_MEDIA[platform]
   return platform, build_settings.platform_limits(platform)["max_save_bytes"], 1


def layout(battle: str | Path, scripts: str | Path | None = None, platform: str | None = None,
           single_copy: bool = False) -> dict[str, Any]:
   """!The save image of a game for one save platform.
   @param battle       the battle manifest
   @param scripts      the scripts manifest, or None
   @param platform     a save platform (medium(); None: the GBA's flash)
   @param single_copy  the game opts in to one copy
   @return {"sections": {name: bytes}, "image": bytes, "budget": the medium's bytes, "unit": its erase unit,
           "slot": a copy's slot, "copies": copies it holds, "platform": name, "single_copy", "unrecorded": the
           script sub-banks whose save shape the game's save history lacks (C7), "ok": the image fits, enough
           copies do and every shape is recorded}
   @throws bc.ManifestError for an invalid manifest
   """
   name = platform or build_settings.DEFAULT_PLATFORM
   engine, budget, unit = medium(name)
   compiled = bc.compile_battle(battle, platform=engine, scripts=scripts)
   comp = compile_scripts(scripts) if scripts is not None else None
   banks = save_shape.shapes(comp) if comp is not None else []
   unrecorded = save_shape.unrecorded(comp, comp.save_history) if comp is not None else []
   sections = sections_of(compiled, banks)
   size = image_bytes(sections)
   copies = copies_of(size, budget, unit)
   needed = 1 if single_copy else REQUIRED_COPIES
   return {"sections": sections, "image": size, "budget": budget, "unit": unit, "slot": slot_bytes(budget, unit),
           "copies": copies, "platform": name, "single_copy": single_copy, "unrecorded": unrecorded,
           "ok": size <= budget and copies >= needed and not unrecorded}


## "CCSV" as the header's first word (capsule::save::kMagic).
MAGIC = 0x56534343
## Image format version (kVersion).
VERSION = 2
## Offset of the header's checksum field (the one header field its checksum leaves out).
CHECKSUM_OFFSET = 16
## Bytes of the checksum field.
CHECKSUM_BYTES = 4
## Reserved header bytes after the section count.
HEADER_RESERVED_BYTE = 1
## Reserved header bytes after the checksum.
HEADER_RESERVED_TAIL = 12
## Reserved bytes of a section table entry after its id and version.
ENTRY_RESERVED = 2
## FNV-1a 32 offset basis and prime (capsule::monster::kFnvOffsetBasis / kFnvPrime).
FNV_BASIS = 0x811C9DC5
## FNV-1a 32 prime.
FNV_PRIME = 0x01000193
## 32-bit mask.
U32_MASK = 0xFFFFFFFF
## Bytes of a 16-bit little-endian value.
U16_BYTES = 2
## Bytes of a 32-bit little-endian value.
U32_BYTES = 4


def fnv(data: bytes, h: int = FNV_BASIS) -> int:
   """!FNV-1a 32 of bytes.
   @param data  bytes
   @param h     hash so far
   @return the hash
   """
   for b in data:
      h = ((h ^ b) * FNV_PRIME) & U32_MASK
   return h


def u16(v: int) -> bytes:
   """!A 16-bit little-endian value.
   @param v  value
   @return 2 bytes
   """
   return v.to_bytes(U16_BYTES, "little")


def u32(v: int) -> bytes:
   """!A 32-bit little-endian value.
   @param v  value
   @return 4 bytes
   """
   return v.to_bytes(U32_BYTES, "little")


def pocket_order(compiled: Mapping[str, Any], pocket: int, slots: Sequence[tuple[int, int]]) -> list[tuple[int, int]]:
   """!A pocket's slots in the order the engine keeps them (capsule::inventory::add() and sort()): as given for
   "added", by item id for "index", by display name's codes (then id) for "name".
   @param compiled  bc.compile_battle() result
   @param pocket    pocket index
   @param slots     [(item id, count)]
   @return the ordered slots
   """
   rule = compiled["inventory"]["pockets"][pocket]["sort"]
   if rule == "index":
      return sorted(slots, key=lambda s: s[0])
   if rule == "name":
      return sorted(slots, key=lambda s: (compiled["items"][s[0]]["name"], s[0]))
   return list(slots)


def bag_payload(compiled: Mapping[str, Any], bag: Mapping[int, Sequence[tuple[int, int]]]) -> bytes:
   """!The Bag section: the entries pocket by pocket (each pocket ordered by pocket_order()) as item key and count.
   @param compiled  bc.compile_battle() result
   @param bag       {pocket index: [(item id, count)]}
   @return the bytes
   """
   items = compiled["inventory"]["items"]
   entries = []
   for k in range(len(compiled["inventory"]["pockets"])):
      entries += pocket_order(compiled, k, bag.get(k, []))
   return u16(len(entries)) + b"".join(u32(items[i]["save_key"]) + u16(n) for i, n in entries)


def party_payload(compiled: Mapping[str, Any], party: Sequence[bytes]) -> bytes:
   """!The Party section.
   @param compiled  bc.compile_battle() result
   @param party     the members' records (the layout's record size; shorter is zero-padded)
   @return the bytes
   @throws bc.ManifestError for more members than the party holds
   """
   lay = compiled["layout"]
   if len(party) > lay["party_size"]:
      raise bc.ManifestError(f"{len(party)} party members: the party holds {lay['party_size']}")
   size = lay["bytes"]
   recs = b"".join(bytes(r[:size]) + bytes(size - min(size, len(r))) for r in party)
   return u32(lay["record_hash"]) + u16(size) + bytes([len(party)]) + recs


def boxes_payload(compiled: Mapping[str, Any], boxes: Mapping[int, bytes], current: int) -> bytes:
   """!The Boxes section's bytes (G3): the geometry, the used bits, the used slots' records in slot order.
   @param compiled  bc.compile_battle() result
   @param boxes     {storage slot: record bytes (the layout's record size; shorter is zero-padded)}
   @param current   the current box
   @return the bytes (empty without boxes)
   @throws bc.ManifestError for a slot past the boxes
   """
   st = compiled.get("storage") or {"slots": 0}
   slots = st["slots"]
   if slots == 0:
      return b""
   lay = compiled["layout"]
   size = lay["bytes"]
   used = bytearray(bc.box_used_bytes(slots))
   for slot in boxes:
      if not 0 <= slot < slots:
         raise bc.ManifestError(f"storage slot {slot}: the boxes have {slots} slots")
      used[slot // USED_BITS_PER_BYTE] |= 1 << (slot % USED_BITS_PER_BYTE)
   recs = b"".join(bytes(boxes[s][:size]) + bytes(size - min(size, len(boxes[s]))) for s in sorted(boxes))
   head = u32(lay["record_hash"]) + u16(size) + bytes([st["box_count"]]) + u16(st["box_size"]) + bytes([current])
   return head + bytes(used) + recs


def nursery_payload(compiled: Mapping[str, Any], slots: Mapping[int, tuple[int, int, bytes]],
                    result: bytes | None = None) -> bytes:
   """!The Nursery section's bytes (B2): the head, each held slot's level, gained experience and record in slot
   order, then the waiting result's record.
   @param compiled  bc.compile_battle() result
   @param slots     {nursery slot: (level when left, experience gained, record bytes)}
   @param result    the waiting result's record bytes, or None
   @return the bytes (empty without a nursery)
   @throws bc.ManifestError for a slot past the nursery's
   """
   n = compiled["combine"].get("nursery")
   if n is None:
      return b""
   lay = compiled["layout"]
   size = lay["bytes"]
   used = 0
   body = b""
   for slot in sorted(slots):
      if not 0 <= slot < n["slots"]:
         raise bc.ManifestError(f"nursery slot {slot}: the nursery has {n['slots']} slots")
      level, gained, rec = slots[slot]
      used |= 1 << slot
      body += bytes([level]) + u32(gained) + bytes(rec[:size]) + bytes(size - min(size, len(rec)))
   if result is not None:
      body += bytes(result[:size]) + bytes(size - min(size, len(result)))
   head = u32(lay["record_hash"]) + u16(size) + bytes([n["slots"], used, 1 if result is not None else 0])
   return head + body


def script_payload(banks: Sequence[Mapping[str, Any]], data: Mapping[str, bytes] | None = None) -> bytes:
   """!The Script section: every non-empty sub-bank with its shape hash.
   @param banks  script_banks() result
   @param data   {sub-bank label: its bytes} (others zero), or None: all zero
   @return the bytes (empty without script storage)
   """
   parts = []
   for k, b in enumerate(banks):
      if not b["bytes"]:
         continue
      raw = bytes((data or {}).get(b["bank"], b""))[:b["bytes"]]
      parts.append(bytes([k]) + u32(b["hash"]) + u16(b["bytes"]) + raw + bytes(b["bytes"] - len(raw)))
   return bytes([len(parts)]) + b"".join(parts) if parts else b""


def script_values(banks: Sequence[Mapping[str, Any]], values: Mapping[str, int]) -> dict[str, bytes]:
   """!Variables' values as their sub-banks' bytes (F3: a test save's step effect counters), little-endian at their
   offsets as the script VM stores them; each value clamped to its variable's range.
   @param banks   script_banks() result
   @param values  {variable name: value}
   @return {sub-bank label: its bytes} for script_payload()'s data
   @throws KeyError for a name no variables' sub-bank holds
   """
   out: dict[str, bytearray] = {}
   left = dict(values)
   for b in banks:
      if b["kind"] != save_shape.VARS:
         continue
      for entry in b["entries"]:
         # [name, offset, width, signed] and, for a variable with its own range, [lo, hi] after them
         plain = len(VAR_ENTRY_PLAIN)
         name, offset, width, signed = entry[:plain]
         if name not in left:
            continue
         lo, hi = entry[plain:] if len(entry) > plain else save_shape.default_range(width)
         value = max(lo, min(hi, int(left.pop(name))))
         raw = out.setdefault(b["bank"], bytearray(b["bytes"]))
         raw[offset:offset + width] = value.to_bytes(width, "little", signed=bool(signed))
   if left:
      raise KeyError(f"no script variable {', '.join(sorted(left))}")
   return {k: bytes(v) for k, v in out.items()}


def game_payload(block: bytes, version: int) -> bytes:
   """!N3: the Game section: the game's version of its block, its size and its bytes.
   @param block    the game's block
   @param version  its layout version
   @return the bytes (empty for an empty block: no section)
   """
   return u16(version) + u32(len(block)) + block if block else b""


def assemble(payloads: Mapping[str, bytes], sequence: int = 1) -> bytes:
   """!One copy of an image from its section bytes (an empty payload: no such section).
   @param payloads  {section name: bytes}, in any order (written in id order)
   @param sequence  the image's sequence number
   @return the bytes
   """
   present = [(k, name) for k, name in enumerate(SECTIONS) if payloads.get(name)]
   table = b"".join(bytes([k, SECTION_VERSIONS[name]]) + bytes(ENTRY_RESERVED) + u32(len(payloads[name])) +
                    u32(fnv(payloads[name])) for k, name in present)
   body = b"".join(payloads[name] for _k, name in present)
   head = u32(MAGIC) + u16(VERSION) + bytes([len(present)]) + bytes(HEADER_RESERVED_BYTE) + u32(sequence)
   head += u32(len(table) + len(body))
   tail = bytes(HEADER_RESERVED_TAIL)
   checksum = fnv(table, fnv(tail, fnv(head)))
   return head + u32(checksum) + tail + table + body


def image(compiled: Mapping[str, Any], bag: Mapping[int, Sequence[tuple[int, int]]], sequence: int = 1,
          banks: Sequence[Mapping[str, Any]] = (), boxes: Mapping[int, bytes] | None = None, current_box: int = 0,
          party: Sequence[bytes] = (), values: Mapping[str, int] | None = None,
          nursery: Mapping[int, tuple[int, int, bytes]] | None = None, nursery_result: bytes | None = None) -> bytes:
   """!One copy of the engine save image (capsule::save::write()) holding a bag, a party, the storage boxes and zero
   script banks: what a test save (the studio's Bag and Boxes pages) exports.
   @param compiled     bc.compile_battle() result
   @param bag          {pocket index: [(item id, count)]}
   @param sequence     the image's sequence number
   @param banks        the game's script sub-banks (script_banks(); written as zeros with their shape hashes)
   @param boxes        {storage slot: record bytes} (G3), or None: empty boxes
   @param current_box  the storage's current box
   @param party        the party's records
   @param values       {script variable name: value} written into the script banks (F3: step effect counters;
                       script_values()), or None: all zero
   @param nursery      {nursery slot: (level, gained, record bytes)} (B2), or None: an empty nursery
   @param nursery_result  the nursery's waiting result (B2: an egg's record), or None
   @return the bytes
   """
   data = script_values(banks, values) if values else None
   return assemble({"bag": bag_payload(compiled, bag), "party": party_payload(compiled, party),
                    "boxes": boxes_payload(compiled, boxes or {}, current_box), "script": script_payload(banks, data),
                    "nursery": nursery_payload(compiled, nursery or {}, nursery_result)},
                   sequence)


def report(result: Mapping[str, Any], battle: str) -> str:
   """!The report text.
   @param result  layout() result
   @param battle  the battle manifest's name
   @return lines
   """
   lines = [f"save image of {battle} ({result['platform']}): {result['image']} B of {result['budget']} B "
            f"({result['copies']} copies{', single copy allowed' if result['single_copy'] else ''})"]
   lines += [f"  {name:7} {size} B" for name, size in result["sections"].items()]
   return "\n".join(lines) + "\n"


def main(argv: Sequence[str]) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return process exit status
   """
   args = list(argv[1:])
   single = "--single-copy" in args
   if single:
      args.remove("--single-copy")
   opts: dict[str, str] = {}
   for flag in ("--scripts", "--platform", "--out"):
      if flag in args:
         i = args.index(flag)
         if i + 1 >= len(args):
            print(__doc__)
            return EXIT_USAGE
         opts[flag] = args[i + 1]
         del args[i:i + OPTION_WORDS]
   if len(args) != 1:
      print(__doc__)
      return EXIT_USAGE
   try:
      result = layout(args[0], opts.get("--scripts"), opts.get("--platform"), single)
   except (bc.ManifestError, build_settings.SettingsError, OSError) as e:
      print(f"error: {e}", file=sys.stderr)
      return EXIT_FAILED
   text = report(result, Path(args[0]).name)
   if "--out" in opts:
      Path(opts["--out"]).parent.mkdir(parents=True, exist_ok=True)
      Path(opts["--out"]).write_text(text, encoding="utf-8", newline="\n")
   print(text, end="")
   if result["image"] > result["budget"]:
      print(f"error: the save image is over the {result['platform']} medium", file=sys.stderr)
      return EXIT_FAILED
   if result["unrecorded"]:
      print(f"error: the save history lacks the save shape of {', '.join(result['unrecorded'])}: an older save of "
            f"this shape would find no remap once it changes. Record it (tools/save_shape.py record "
            f"{opts['--scripts']})", file=sys.stderr)
      return EXIT_FAILED
   if not result["ok"]:
      print(f"error: the {result['platform']} medium keeps {result['copies']} copy of the save image; a write cut "
            f"short would lose the save. Shrink the image (boxes, pockets) or opt in to one copy (project.json "
            f"\"save\": {{\"single_copy\": true}})", file=sys.stderr)
      return EXIT_FAILED
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv))
