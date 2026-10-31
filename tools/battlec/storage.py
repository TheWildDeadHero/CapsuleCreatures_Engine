"""!Storage boxes (D-26 section 3, package G3): structure.json `storage`, emitted as capsule::storage::kStorage
(include/capsule/storage/storage.h) next to the battle tables.

The section is {"box_count", "box_size", "names"}: box_count boxes of box_size slots, each slot one creature record
(the layout's record bytes); "names" is a list of box_count display names, or one text with {n} (the box number,
from 1) that names them all (default DEFAULT_NAMES). The platform bounds the boxes, their slots and their record
bytes (build_settings PLATFORM_LIMITS max_boxes / max_box_slots / max_storage_bytes). A structure without the
section has no boxes (a recruit with a full party then has no room).

The save image keeps the boxes as its Boxes section (tools/save_layout.py: every slot's record bytes, a bit per slot,
the current box). The ids header gives STORAGE_BYTES and STORAGE_USED_BYTES, the sizes of the game's
capsule::storage::Buffer (C6: the storage is sized from the manifest, not the platform's maximum).
"""
from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from .common import Json, ManifestError, Texts, check_keys, integer, register_keys

## Key of the section in structure.json.
STORAGE_KEY = "storage"
## Placeholder of a box's number in a name pattern.
NUMBER_PLACEHOLDER = "{n}"
## Box names when the section gives none.
DEFAULT_NAMES = "Box {n}"
## Longest box name (letters).
MAX_NAME_CHARS = 16
## Bits per byte of the slots' occupancy bits.
BYTE_BITS = 8

register_keys({"storage": {"box_count", "box_size", "names"}})


def compile_storage(src: Json, layout: Mapping[str, Any], limits: Mapping[str, int], where: str,
                    enc: Texts) -> dict[str, Any]:
   """!The structure's storage boxes.
   @param src     structure.json storage (None: no boxes)
   @param layout  compiled layout (its record bytes)
   @param limits  the platform's limits (max_boxes, max_box_slots, max_storage_bytes)
   @param where   location (the structure manifest)
   @param enc     this compile's text encoders
   @return {"box_count", "box_size", "slots", "record_bytes", "bytes" (record bytes of every slot), "names"
           (texts), "name_codes"}
   """
   record_bytes = layout["bytes"]
   if src is None:
      return {"box_count": 0, "box_size": 0, "slots": 0, "record_bytes": record_bytes, "bytes": 0, "names": [],
              "name_codes": []}
   w = f"{where}: {STORAGE_KEY}"
   check_keys(src, "storage", w)
   count = integer(src.get("box_count"), 1, limits["max_boxes"], f"{w}.box_count")
   size = integer(src.get("box_size"), 1, limits["max_box_slots"], f"{w}.box_size")
   if count * size > limits["max_box_slots"]:
      raise ManifestError(f"{w}: {count} boxes of {size} slots are {count * size} slots; this platform holds "
                          f"{limits['max_box_slots']} (max_box_slots)")
   if count * size * record_bytes > limits["max_storage_bytes"]:
      raise ManifestError(f"{w}: {count * size} records of {record_bytes} bytes are {count * size * record_bytes} "
                          f"bytes; this platform holds {limits['max_storage_bytes']} (max_storage_bytes)")
   names = src.get("names", DEFAULT_NAMES)
   if isinstance(names, str):
      names = [names.replace(NUMBER_PLACEHOLDER, str(k + 1)) for k in range(count)]
   if not isinstance(names, list) or len(names) != count or not all(isinstance(n, str) for n in names):
      raise ManifestError(f"{w}.names: a list of {count} names, or one name with {NUMBER_PLACEHOLDER}")
   for k, n in enumerate(names):
      if not n or len(n) > MAX_NAME_CHARS:
         raise ManifestError(f"{w}.names[{k}]: 1 to {MAX_NAME_CHARS} letters, got {n!r}")
   return {"box_count": count, "box_size": size, "slots": count * size, "record_bytes": record_bytes,
           "bytes": count * size * record_bytes, "names": names,
           "name_codes": [enc.text(n, f"{w}.names[{k}]") for k, n in enumerate(names)]}


def used_bytes(slots: int) -> int:
   """!Bytes of the slots' occupancy bits (a bit per slot).
   @param slots  slots
   @return bytes
   """
   return (slots + BYTE_BITS - 1) // BYTE_BITS


def emit_storage(storage: Mapping[str, Any]) -> list[str]:
   """!The C++ of capsule::storage::kStorage (its own namespace, after the battle's tables).
   @param storage  compile_storage() result
   @return lines
   """
   def codes(c: list[int]) -> str:
      """!A text's codes as a brace list."""
      return "{" + ", ".join(f"0x{x:02X}" for x in c) + "}"
   L = ["", "namespace capsule::storage {", "", "namespace {", ""]
   for k, c in enumerate(storage["name_codes"]):
      L.append(f"const text::Code kBoxName{k}[] = {codes(c)};   // {storage['names'][k]}")
   names = ", ".join(f"kBoxName{k}" for k in range(len(storage["name_codes"])))
   L.append(f"const text::Code* const kBoxNames[] = {{{names or 'nullptr'}}};")
   L += ["", "} // namespace", "",
         f"const StorageData kStorage = {{kBoxNames, {storage['box_size']}, {storage['box_count']}, 0}};",
         f"static_assert({storage['slots']} <= kMaxBoxSlots && {storage['box_count']} <= kMaxBoxes);",
         f"static_assert({storage['bytes']} <= kMaxStorageBytes, \"more boxed record bytes than this platform's "
         f"engine holds\");",
         "", "} // namespace capsule::storage", ""]
   return L


def storage_ids(storage: Mapping[str, Any]) -> list[str]:
   """!The ids header's storage constants.
   @param storage  compile_storage() result
   @return lines
   """
   return [f"inline constexpr int NUM_BOXES = {storage['box_count']};",
           f"inline constexpr int BOX_SIZE = {storage['box_size']};",
           "/// Record bytes of every storage slot (capsule::storage::Buffer's first size; C6).",
           f"inline constexpr uint32_t STORAGE_BYTES = {storage['bytes']};",
           "/// Bytes of the storage slots' used bits (capsule::storage::Buffer's second size; C6).",
           f"inline constexpr uint32_t STORAGE_USED_BYTES = {used_bytes(storage['slots'])};", ""]
