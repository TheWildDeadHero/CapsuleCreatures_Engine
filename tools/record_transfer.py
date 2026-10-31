#!/usr/bin/env python3
"""!Record transfer v2 in Python: the mirror of src/monster/transfer.cpp (same bytes, same import rule; see
include/capsule/monster/transfer.h) for the studio, the tools and the reference repo, and the command line that
writes the C++ transfer tables.

A table is tools/battlec/transfer.compile_transfer()'s result: load() makes one from a battle manifest
(battle.json, compiled with tools/battle_compiler.py) or a transfer fixture (tools/battlec/transfer.py).

Python API: pack() / unpack() turn a record dict ({role name: value}; a per-stat role {stat name: value}, a role
of several elements a list, others an int) into record bytes and back; export_bytes() / import_bytes() are the
engine's export_record() / import_record() on record bytes; export_record() / import_record() the same on
record dicts. Errors raise TransferError with the engine's TransferStatus name. import_bytes(received=) /
import_record(received=) also collect the receiver roles the record held (monster::RoleSet), and
import_rederived() is capsule::battle::import_record(): the import, then the derived roles the sender did not have
re-derived (battlec.progression.rederive(), E13a).

Usage:
  record_transfer.py emit BATTLE.json OUT.cpp [--platform P] [--depfile D]
      the build's capsule::monster::kTransfer (for kLayout of the battle data)
  record_transfer.py emit FIXTURE.json OUT.cpp --namespace NS [--header OUT.h] [--platform P]
      a stand-alone table with its own kLayout in namespace NS (tests)
  record_transfer.py cases CASES.json OUT.h [--platform P] [--depfile D]
      C++ test vectors: each case's source record, canonical and raw bytes, and imported record (this mirror's)
  record_transfer.py export SOURCE RECORD.json [--raw] [--platform P]      prints the transfer bytes (hex)
  record_transfer.py import SOURCE HEX [--platform P]                       prints the record dict and flags
"""
from __future__ import annotations

import json
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import battle_compiler  # noqa: E402
import build_settings  # noqa: E402
from battlec.common import ManifestError  # noqa: E402
from battlec.layout import (CUSTOM_ROLE_BASE, MAX_ROLE_BITS, PER_STAT_ROLES, ROLE_ABSENT,  # noqa: E402
                            ROLE_NAMES, ROLES, role_key)
from battlec.nursery import EGG_FLAG_TRANSFER_HATCH, EGG_TRANSFER_NAME  # noqa: E402
from battlec.transfer import (FIXTURE_SCHEMA, TRANSFER_RAW_VERSION, TRANSFER_VERSION, compile_transfer,  # noqa: E402
                              emit_cpp, emit_header, load_fixture)

## A transfer table (battlec.transfer.compile_transfer() result).
Table = Mapping[str, Any]
## A record dict: {role name: int, list of ints, or {stat name: int}}.
RecordDict = dict[str, Any]

## Index of the count rule in a battlec.layout.ROLES entry (name, width, count, rule).
ROLE_RULE = 3
## Bits per byte.
BITS_PER_BYTE = 8
## Mask of one byte.
BYTE_MASK = 0xFF
## Bytes of a key (custom role, stat, transfer hash; little-endian).
KEY_BYTES = 4
## Bytes of a present role's shape (width, count).
SHAPE_BYTES = 2
## Bytes before the presence mask (version, engine role count).
HEADER_BYTES = 2
## Bytes before a raw record's bytes (version, hash).
RAW_HEADER_BYTES = 1 + KEY_BYTES
## Engine roles (capsule::monster::Role::NUM_ROLES; the presence mask's bits).
ENGINE_ROLES = len(ROLES)
## Bytes of the presence mask this mirror writes.
MASK_BYTES = (ENGINE_ROLES + BITS_PER_BYTE - 1) // BITS_PER_BYTE
## Roles whose empty element has every bit set (Moves: "no move"; Nickname: the end code).
FILL_ALL_ONES = {ROLE_NAMES.index("moves"), ROLE_NAMES.index("nickname")}
## Engine role IDs of the per-stat roles.
PER_STAT_IDS = [ROLE_NAMES.index(r) for r in PER_STAT_ROLES]
## Import flags (capsule::monster::kTransferNarrowed, kTransferDropped, kTransferDefaulted, kTransferRaw).
NARROWED, DROPPED, DEFAULTED, RAW = 0x01, 0x02, 0x04, 0x08
## C9: the record is an egg the receiver has no egg role for (capsule::monster::kTransferEgg).
EGG = 0x10
## The egg role's transfer key (capsule::monster::kEggTransferKey).
EGG_KEY = role_key(EGG_TRANSFER_NAME)
## Flag names, for messages.
FLAG_NAMES = {NARROWED: "narrowed", DROPPED: "dropped", DEFAULTED: "defaulted", RAW: "raw", EGG: "egg"}
## Arguments after the command of each command.
COMMAND_ARGS = {"emit": 2, "cases": 2, "export": 2, "import": 2}
## Words of an option with a value.
OPTION_WORDS = 2
## Exit status of a usage error.
EXIT_USAGE = 2
## Schema of a test case file.
CASES_SCHEMA = "capsule.transfer_cases/1"
## Bytes per line of a generated C++ byte array.
BYTES_PER_LINE = 16


class TransferError(Exception):
   """!A failed export or import; str() is the engine's TransferStatus name (Truncated, BadVersion, ...)."""


# --- records of a layout (capsule::monster get / set on a Layout) -----------------------------------------------

def _custom(table: Table, rid: int) -> Mapping[str, Any] | None:
   """!The custom role of an ID.
   @param table  transfer table
   @param rid    role ID
   @return its compile_layout() entry, or None
   """
   index = rid - CUSTOM_ROLE_BASE
   custom = table["layout"]["custom"]
   return custom[index] if 0 <= index < len(custom) else None


def has(table: Table, rid: int) -> bool:
   """!Whether the layout holds a role.
   @param table  transfer table
   @param rid    role ID
   @return True if held
   """
   if rid >= CUSTOM_ROLE_BASE:
      return _custom(table, rid) is not None
   return rid < ENGINE_ROLES and table["layout"]["offsets"][rid] != ROLE_ABSENT


def width(table: Table, rid: int) -> int:
   """!A held role's element width.
   @param table  transfer table
   @param rid    role ID
   @return bits
   """
   c = _custom(table, rid)
   return c["width"] if c is not None else table["layout"]["widths"][rid]


def count(table: Table, rid: int) -> int:
   """!A held role's element count.
   @param table  transfer table
   @param rid    role ID
   @return elements
   """
   c = _custom(table, rid)
   return c["count"] if c is not None else table["layout"]["counts"][rid]


def max_value(table: Table, rid: int) -> int:
   """!The highest value of a held role's element.
   @param table  transfer table
   @param rid    role ID
   @return 2^width - 1
   """
   return (1 << width(table, rid)) - 1


def _offset(table: Table, rid: int) -> int:
   """!A held role's first bit.
   @param table  transfer table
   @param rid    role ID
   @return bit offset
   """
   c = _custom(table, rid)
   return c["offset"] if c is not None else table["layout"]["offsets"][rid]


def read_bits(data: bytes | bytearray, bit: int, bits: int) -> int:
   """!Reads a bit field (LSB first; capsule::monster::read_bits()).
   @param data  bytes
   @param bit   first bit
   @param bits  width
   @return value
   """
   value = 0
   for k in range(bits):
      at = bit + k
      value |= ((data[at // BITS_PER_BYTE] >> (at % BITS_PER_BYTE)) & 1) << k
   return value


def write_bits(data: bytearray, bit: int, bits: int, value: int) -> None:
   """!Writes a bit field (LSB first; capsule::monster::write_bits()); bits above the width are dropped.
   @param data   bytes
   @param bit    first bit
   @param bits   width
   @param value  value
   @return None
   """
   for k in range(bits):
      at = bit + k
      mask = 1 << (at % BITS_PER_BYTE)
      if (value >> k) & 1:
         data[at // BITS_PER_BYTE] |= mask
      else:
         data[at // BITS_PER_BYTE] &= ~mask & BYTE_MASK


def get(table: Table, record: bytes | bytearray, rid: int, element: int) -> int:
   """!One element of a role (capsule::monster::get() on the table's layout).
   @param table    transfer table
   @param record   record bytes
   @param rid      role ID
   @param element  element index
   @return value (0 when not held)
   """
   if not has(table, rid) or not 0 <= element < count(table, rid):
      return 0
   w = width(table, rid)
   return read_bits(record, _offset(table, rid) + element * w, w)


def put(table: Table, record: bytearray, rid: int, element: int, value: int) -> None:
   """!Writes one element of a role (capsule::monster::set() on the table's layout; truncated to the width).
   @param table    transfer table
   @param record   record bytes
   @param rid      role ID
   @param element  element index
   @param value    value
   @return None
   """
   if has(table, rid) and 0 <= element < count(table, rid):
      w = width(table, rid)
      write_bits(record, _offset(table, rid) + element * w, w, value)


def _held(table: Table, canonical: bool = False) -> list[int]:
   """!The held role IDs: engine roles by ID, then custom roles (layout order, or by key).
   @param table      transfer table
   @param canonical  custom roles by ascending key (the canonical order)
   @return role IDs
   """
   engine = [i for i in range(ENGINE_ROLES) if has(table, i)]
   order = table["custom_order"] if canonical else range(len(table["custom_keys"]))
   return engine + [CUSTOM_ROLE_BASE + i for i in order]


def _role_id(table: Table, name: str) -> int:
   """!The ID of a role the layout holds, by name.
   @param table  transfer table
   @param name   role name
   @return role ID
   @throws ValueError when the layout does not hold it
   """
   rid = table["layout"]["ids"].get(name)
   if rid is None or not has(table, rid):
      raise ValueError(f"the layout holds no role {name!r}")
   return rid


def _role_name(table: Table, rid: int) -> str:
   """!A held role's name.
   @param table  transfer table
   @param rid    role ID
   @return name
   """
   c = _custom(table, rid)
   return c["name"] if c is not None else ROLE_NAMES[rid]


def _single(table: Table, rid: int) -> bool:
   """!Whether a record dict holds a role as one int (engine "one" roles, custom roles of one element).
   @param table  transfer table
   @param rid    role ID
   @return True for an int
   """
   c = _custom(table, rid)
   return c["count"] == 1 if c is not None else ROLES[rid][ROLE_RULE] == "one"


def new_record(table: Table) -> bytearray:
   """!A new record (capsule::monster::reset(): custom roles at their defaults, everything else 0).
   @param table  transfer table
   @return record bytes
   """
   record = bytearray(table["layout"]["bytes"])
   for c in table["layout"]["custom"]:
      for e in range(c["count"]):
         put(table, record, c["id"], e, c["default"])
   return record


def pack(table: Table, values: Mapping[str, Any]) -> bytes:
   """!Record bytes from a record dict (roles left out keep new_record()'s values).
   @param table   transfer table
   @param values  record dict
   @return record bytes
   @throws ValueError for a role or stat the layout does not hold, or a value that does not fit
   """
   record = new_record(table)
   for name, value in values.items():
      rid = _role_id(table, name)
      if rid in PER_STAT_IDS:
         if not isinstance(value, Mapping):
            raise ValueError(f"{name}: a per-stat role takes {{stat name: value}}")
         unknown = sorted(set(value) - set(table["stat_names"]))
         if unknown:
            raise ValueError(f"{name}: no stat {', '.join(unknown)}")
         elements = {table["stat_names"].index(s): v for s, v in value.items()}
      elif isinstance(value, int):
         elements = {0: value}
      else:
         if len(value) > count(table, rid):
            raise ValueError(f"{name}: at most {count(table, rid)} elements")
         elements = dict(enumerate(value))
      for e, v in elements.items():
         if not isinstance(v, int) or not 0 <= v <= max_value(table, rid):
            raise ValueError(f"{name}[{e}]: {v!r} does not fit {width(table, rid)} bits")
         put(table, record, rid, e, v)
   return bytes(record)


def unpack(table: Table, record: bytes | bytearray) -> RecordDict:
   """!The record dict of record bytes (every role the layout holds, layout order).
   @param table   transfer table
   @param record  record bytes
   @return record dict
   """
   out: RecordDict = {}
   for rid in _held(table):
      values = [get(table, record, rid, e) for e in range(count(table, rid))]
      name = _role_name(table, rid)
      if rid in PER_STAT_IDS:
         out[name] = dict(zip(table["stat_names"], values))
      else:
         out[name] = values[0] if _single(table, rid) else values
   return out


# --- the transfer forms (src/monster/transfer.cpp) ----------------------------------------------------------------

def _key_bytes(value: int) -> bytes:
   """!A key, little-endian.
   @param value  32-bit key
   @return 4 bytes
   """
   return value.to_bytes(KEY_BYTES, "little")


def _default(table: Table, rid: int, element: int) -> int:
   """!The value a receiver's role element has before (or without) an incoming value.
   @param table    receiver's table
   @param rid      role ID
   @param element  element (per-stat roles: the stat index)
   @return default
   """
   c = _custom(table, rid)
   if c is not None:
      return c["default"]
   if rid in FILL_ALL_ONES:
      return max_value(table, rid)
   if rid in PER_STAT_IDS:
      return table["stat_defaults"][element][PER_STAT_IDS.index(rid)]
   return 0


def transfer_size(table: Table, raw: bool = False) -> int:
   """!Bytes an export writes (capsule::monster::transfer_size()).
   @param table  sender's table
   @param raw    the raw form
   @return bytes
   """
   if raw:
      return RAW_HEADER_BYTES + table["layout"]["bytes"]
   held = _held(table)
   bits = sum(width(table, r) * count(table, r) for r in held)
   return (HEADER_BYTES + MASK_BYTES + 1 + KEY_BYTES * len(table["custom_keys"]) + 1 +
           KEY_BYTES * len(table["stat_keys"]) + SHAPE_BYTES * len(held) + (bits + BITS_PER_BYTE - 1) // BITS_PER_BYTE)


def export_bytes(table: Table, record: bytes | bytearray, raw: bool = False) -> bytes:
   """!A record in a transfer form (capsule::monster::export_record()).
   @param table   sender's table
   @param record  record bytes (the layout's size)
   @param raw     the raw form (for a receiver with the same transfer hash)
   @return transfer bytes
   """
   if len(record) != table["layout"]["bytes"]:
      raise ValueError(f"a record of this layout has {table['layout']['bytes']} bytes, got {len(record)}")
   if raw:
      return bytes([TRANSFER_RAW_VERSION]) + _key_bytes(table["hash"]) + bytes(record)
   held = _held(table, canonical=True)
   mask = 0
   for rid in held:
      mask |= 1 << rid if rid < ENGINE_ROLES else 0
   out = bytearray([TRANSFER_VERSION, ENGINE_ROLES]) + mask.to_bytes(MASK_BYTES, "little")
   for keys, order in ((table["custom_keys"], table["custom_order"]), (table["stat_keys"], table["stat_order"])):
      out.append(len(keys))
      for i in order:
         out += _key_bytes(keys[i])
   for rid in held:
      out += bytes([width(table, rid), count(table, rid)])
   bits = sum(width(table, r) * count(table, r) for r in held)
   values = bytearray((bits + BITS_PER_BYTE - 1) // BITS_PER_BYTE)
   bit = 0
   for rid in held:
      w = width(table, rid)
      for e in range(count(table, rid)):
         # a per-stat role's elements go in stat table order
         write_bits(values, bit, w, get(table, record, rid, table["stat_order"][e] if rid in PER_STAT_IDS else e))
         bit += w
   return bytes(out + values)


def _need(data: bytes, n: int) -> None:
   """!Raises Truncated when the input is shorter than n bytes.
   @param data  transfer bytes
   @param n     bytes needed
   @return None
   """
   if len(data) < n:
      raise TransferError("Truncated")


def _keys(data: bytes, at: int) -> tuple[list[int], int]:
   """!Parses a key table: its count byte, then that many keys, strictly ascending (parse_keys()).
   @param data  transfer bytes
   @param at    the count byte's offset
   @return (keys, offset after the table)
   @throws TransferError Truncated or BadShape
   """
   _need(data, at + 1)
   n = data[at]
   at += 1
   _need(data, at + n * KEY_BYTES)
   keys = [int.from_bytes(data[at + k * KEY_BYTES:at + (k + 1) * KEY_BYTES], "little") for k in range(n)]
   if any(a >= b for a, b in zip(keys, keys[1:])):
      raise TransferError("BadShape")
   return keys, at + n * KEY_BYTES


def _parse(data: bytes) -> dict[str, Any]:
   """!Parses and checks a canonical record's header and shapes (src/monster/transfer.cpp parse()).
   @param data  transfer bytes
   @return {"roles": [(custom, id or key, width, count, first bit)], "stats": [keys], "bytes": bytes read}
   @throws TransferError Truncated or BadShape
   """
   _need(data, HEADER_BYTES)
   engine_count = data[1]
   mask_bytes = (engine_count + BITS_PER_BYTE - 1) // BITS_PER_BYTE
   at = HEADER_BYTES
   _need(data, at + mask_bytes)
   mask = int.from_bytes(data[at:at + mask_bytes], "little")
   if mask >> engine_count:
      raise TransferError("BadShape")   # the mask's padding bits are 0
   at += mask_bytes
   custom_keys, at = _keys(data, at)
   stat_keys, at = _keys(data, at)
   present = [(False, i) for i in range(engine_count) if (mask >> i) & 1] + [(True, k) for k in custom_keys]
   _need(data, at + SHAPE_BYTES * len(present))
   value_bit = (at + SHAPE_BYTES * len(present)) * BITS_PER_BYTE
   roles = []
   bit = 0
   for i, (custom, ident) in enumerate(present):
      w, n = data[at + i * SHAPE_BYTES], data[at + i * SHAPE_BYTES + 1]
      if not 1 <= w <= MAX_ROLE_BITS or n < 1 or (not custom and ident in PER_STAT_IDS and n != len(stat_keys)):
         raise TransferError("BadShape")
      roles.append((custom, ident, w, n, value_bit + bit))
      bit += w * n
   end = value_bit // BITS_PER_BYTE + (bit + BITS_PER_BYTE - 1) // BITS_PER_BYTE
   _need(data, end)
   return {"roles": roles, "stats": stat_keys, "bytes": end}


def _convert(table: Table, rid: int, value: int, sender_width: int) -> tuple[int, bool]:
   """!A sender's value at the receiver's width (Moves / Nickname keep all-ones; others clamp).
   @param table         receiver's table
   @param rid           receiver's role ID
   @param value         sender's value
   @param sender_width  sender's element width
   @return (value, narrowed)
   """
   top = max_value(table, rid)
   if rid in FILL_ALL_ONES and value == (1 << sender_width) - 1:
      return top, False
   return (top, True) if value > top else (value, False)


def import_bytes(table: Table, data: bytes | bytearray, received: set[int] | None = None) -> tuple[bytes, int, int]:
   """!A transferred record into a record of the table's layout (capsule::monster::import_record()).
   @param table     receiver's table
   @param data      transfer bytes (bytes past the record are ignored)
   @param received  set receiving the receiver role IDs the record held whole (every held role for the raw form; C5:
                    not a role an element of was defaulted or narrowed), or None; filled only when the import
                    succeeds
   @return (record bytes, flags, bytes read)
   @throws TransferError Truncated, BadVersion, BadShape or OtherLayout
   """
   data = bytes(data)
   size = table["layout"]["bytes"]
   if not data:
      raise TransferError("Truncated")
   if data[0] == TRANSFER_RAW_VERSION:
      if len(data) < RAW_HEADER_BYTES:
         raise TransferError("Truncated")
      if int.from_bytes(data[1:RAW_HEADER_BYTES], "little") != table["hash"]:
         raise TransferError("OtherLayout")
      if len(data) < RAW_HEADER_BYTES + size:
         raise TransferError("Truncated")
      if received is not None:
         received.update(_held(table))
      return data[RAW_HEADER_BYTES:RAW_HEADER_BYTES + size], RAW, RAW_HEADER_BYTES + size
   if data[0] != TRANSFER_VERSION:
      raise TransferError("BadVersion")
   parsed = _parse(data)
   flags = 0
   record = bytearray(size)
   for rid in _held(table):
      for e in range(count(table, rid)):
         put(table, record, rid, e, _default(table, rid, e))

   # the incoming roles the receiver holds, element by element (C5: a narrowed one is not received whole)
   narrowed_ids: set[int] = set()
   for custom, ident, w, n, bit in parsed["roles"]:
      if custom:
         rid = CUSTOM_ROLE_BASE + table["custom_keys"].index(ident) if ident in table["custom_keys"] else None
      else:
         rid = ident if ident < ENGINE_ROLES and has(table, ident) else None
      if rid is None:
         flags |= DROPPED
         if custom and ident == EGG_KEY and n > 0 and read_bits(data, bit, w) != 0:
            flags |= EGG   # C9: an egg the receiver has no egg role for (the battle side decides)
         continue
      for e in range(n):
         value, narrowed = _convert(table, rid, read_bits(data, bit + e * w, w), w)
         element = e
         if rid in PER_STAT_IDS:
            key = parsed["stats"][e]
            if key not in table["stat_keys"]:
               flags |= DROPPED
               continue
            element = table["stat_keys"].index(key)
         elif e >= count(table, rid):
            flags |= DROPPED if value != _default(table, rid, e) else 0
            continue
         flags |= NARROWED if narrowed else 0
         if narrowed:
            narrowed_ids.add(rid)
         put(table, record, rid, element, value)

   # receiver roles, stats and elements the record did not fill
   incoming = {(c, i): n for c, i, _w, n, _bit in parsed["roles"]}
   held_incoming = set()
   for rid in _held(table):
      custom = rid >= CUSTOM_ROLE_BASE
      ident = table["custom_keys"][rid - CUSTOM_ROLE_BASE] if custom else rid
      if (custom, ident) not in incoming:
         flags |= DEFAULTED
         continue
      partial = False
      if rid in PER_STAT_IDS:
         partial = any(k not in parsed["stats"] for k in table["stat_keys"])
      elif count(table, rid) > incoming[(custom, ident)]:
         partial = True
      flags |= DEFAULTED if partial else 0
      if not partial and rid not in narrowed_ids:
         held_incoming.add(rid)
   if received is not None:
      received.update(held_incoming)
   return bytes(record), flags, parsed["bytes"]


def export_record(table: Table, values: Mapping[str, Any], raw: bool = False) -> bytes:
   """!A record dict in a transfer form.
   @param table   sender's table
   @param values  record dict (pack())
   @param raw     the raw form
   @return transfer bytes
   """
   return export_bytes(table, pack(table, values), raw)


def import_record(table: Table, data: bytes | bytearray, received: set[str] | None = None) -> tuple[RecordDict, int]:
   """!A transferred record as a record dict of the table's layout.
   @param table     receiver's table
   @param data      transfer bytes
   @param received  set receiving the names of the receiver roles the record held whole (import_bytes()), or None
   @return (record dict, flags)
   """
   ids: set[int] = set()
   record, flags, _read = import_bytes(table, data, ids)
   if received is not None:
      received.update(_role_name(table, rid) for rid in ids)
   return unpack(table, record), flags


def import_rederived(table: Table, data: bytes | bytearray, battle: Mapping[str, Any]) -> tuple[RecordDict, int]:
   """!A transferred record with the receiver's derived roles the sender did not have re-derived
   (capsule::battle::import_record(), E13a): import_record(), then battlec.progression.rederive() keeping the roles
   the record held.
   @param table   receiver's table (of the battle's layout)
   @param data    transfer bytes
   @param battle  the receiver's compiled battle (battle_compiler.compile_battle(): generation list, species)
   @return (record dict, flags)
   @throws TransferError EggRefused (C9: an egg sent to a layout without an egg role, unless the structure's
           transfer_eggs is "hatch"), or as import_record()
   """
   received: set[str] = set()
   record, flags = import_record(table, data, received)
   combine = battle.get("combine") or {}
   eggs = combine.get("eggs")
   hatch = ((eggs or {}).get("flags", combine.get("transfer_flags", 0)) & EGG_FLAG_TRANSFER_HATCH) != 0
   if flags & EGG and not hatch:
      raise TransferError("EggRefused")
   return battle_compiler.rederive(battle, record, sorted(received)), flags


def flag_names(flags: int) -> list[str]:
   """!The names of import flags.
   @param flags  NARROWED | DROPPED | DEFAULTED | RAW
   @return names
   """
   return [name for bit, name in FLAG_NAMES.items() if flags & bit]


# --- loading and the command line ---------------------------------------------------------------------------------

def load(path: str | Path, platform: str | None = None, deps: list[Path] | None = None) -> dict[str, Any]:
   """!The transfer table of a battle manifest or a transfer fixture.
   @param path      battle.json or a fixture (schema capsule.transfer_fixture/1)
   @param platform  target platform (build_settings.PLATFORMS), None for the GBA's limits
   @param deps      list collecting files read (depfile)
   @return compile_transfer() result
   @throws ManifestError
   """
   path = Path(path)
   data = json.loads(path.read_text(encoding="utf-8"))
   if deps is not None:
      deps.append(path.resolve())
   if isinstance(data, dict) and data.get("schema") == FIXTURE_SCHEMA:
      limits = build_settings.platform_limits(platform)
      return load_fixture(data, path.name, limits["max_record_bytes"], limits["max_party_size"])
   c = battle_compiler.compile_battle(path, deps, platform=platform)
   return compile_transfer(c["layout"], c["stats"], path.name)


def _cpp_bytes(name: str, data: bytes) -> list[str]:
   """!A C++ byte array.
   @param name  array name
   @param data  bytes
   @return lines
   """
   lines = [f"const uint8_t {name}[] = {{"]
   for i in range(0, max(len(data), 1), BYTES_PER_LINE):
      chunk = data[i:i + BYTES_PER_LINE] or b"\0"
      lines.append("   " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
   return lines + ["};"]


def emit_cases(path: Path, platform: str | None, deps: list[Path]) -> str:
   """!C++ test vectors of a case file: each case's source record (packed by this mirror), its canonical and raw
   exports and its import into the receiver's layout, as this mirror computes them.
   @param path      the case file ({"schema", "tables": {name: {"source", "namespace"}}, "records": {name:
                    record dict}, "cases": [{"name", "from", "to", "record"}]})
   @param platform  target platform
   @param deps      list collecting files read
   @return C++ header
   """
   spec = json.loads(path.read_text(encoding="utf-8"))
   deps.append(path.resolve())
   if spec.get("schema") != CASES_SCHEMA:
      raise ManifestError(f"{path.name}: schema must be {CASES_SCHEMA!r}")
   tables = {n: (load(path.parent / t["source"], platform, deps), t.get("namespace")) for n, t in
             spec["tables"].items()}
   L = ["// Generated by tools/record_transfer.py from " + path.name + ". Do not edit.", "#pragma once", "",
        "#include \"capsule/monster/transfer.h\"", ""]
   L += [f"#include \"transfer_{ns}.h\"" for _t, ns in tables.values() if ns]
   L += ["", "namespace transfer_cases", "{", "", "/// One test vector (this mirror's results).", "struct Case", "{",
         "   const char* name;                               ///< Case name.",
         "   const capsule::monster::TransferInfo* from;     ///< Sender.",
         "   const capsule::monster::TransferInfo* to;       ///< Receiver.",
         "   const uint8_t* source;                          ///< Sender's record.",
         "   int source_bytes;                               ///< Its bytes.",
         "   const uint8_t* canonical;                       ///< Canonical export.",
         "   int canonical_bytes;                            ///< Its bytes.",
         "   const uint8_t* raw;                             ///< Raw export.",
         "   int raw_bytes;                                  ///< Its bytes.",
         "   const uint8_t* imported;                        ///< The receiver's record after import.",
         "   int imported_bytes;                             ///< Its bytes.",
         "   uint8_t flags;                                  ///< Import flags.",
         "};", ""]
   rows = []
   for i, case in enumerate(spec["cases"]):
      src, src_ns = tables[case["from"]]
      dst, dst_ns = tables[case["to"]]
      record = pack(src, spec["records"][case["record"]])
      canonical = export_bytes(src, record)
      raw = export_bytes(src, record, raw=True)
      imported, flags, _read = import_bytes(dst, canonical)
      for name, data in (("source", record), ("canonical", canonical), ("raw", raw), ("imported", imported)):
         L += _cpp_bytes(f"k{name.capitalize()}{i}", data)
      ref = {"from": src_ns, "to": dst_ns}
      info = {k: f"&{ns}::kTransfer" if ns else "&capsule::monster::kTransfer" for k, ns in ref.items()}
      rows.append(f"   {{\"{case['name']}\", {info['from']}, {info['to']}, kSource{i}, {len(record)}, "
                  f"kCanonical{i}, {len(canonical)}, kRaw{i}, {len(raw)}, kImported{i}, {len(imported)}, "
                  f"0x{flags:02X}}},")
   L += ["", "/// The cases.", "const Case kCases[] = {"] + rows + ["};", "",
                                                                    "} // namespace transfer_cases", ""]
   return "\n".join(L)


def _options(args: list[str], flags: Sequence[str]) -> dict[str, str]:
   """!Removes `--flag value` options from an argument list.
   @param args   arguments (changed)
   @param flags  option names
   @return {flag: value}
   """
   opts = {}
   for flag in flags:
      if flag in args:
         i = args.index(flag)
         if i + 1 >= len(args):
            raise ValueError(f"{flag} needs a value")
         opts[flag] = args[i + 1]
         del args[i:i + OPTION_WORDS]
   return opts


def _write_if_changed(path: Path, text: str) -> None:
   """!Writes a text file unless it already holds the text (keeps build times).
   @param path  file
   @param text  content
   @return None
   """
   if not path.exists() or path.read_bytes() != text.encode("utf-8"):
      path.parent.mkdir(parents=True, exist_ok=True)
      path.write_text(text, encoding="utf-8", newline="\n")


def main(argv: Sequence[str]) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return process exit status
   """
   args = list(argv[1:])
   raw = "--raw" in args
   if raw:
      args.remove("--raw")
   try:
      opts = _options(args, ("--platform", "--depfile", "--namespace", "--header"))
   except ValueError:
      print(__doc__)
      return EXIT_USAGE
   if not args or args[0] not in COMMAND_ARGS or len(args) != COMMAND_ARGS[args[0]] + 1:
      print(__doc__)
      return EXIT_USAGE
   command, first, second = args
   platform = opts.get("--platform")
   deps: list[Path] = []
   try:
      if command == "emit":
         info = load(first, platform, deps)
         _write_if_changed(Path(second), emit_cpp(Path(first).name, info, opts.get("--namespace")))
         if "--header" in opts and "--namespace" in opts:
            _write_if_changed(Path(opts["--header"]), emit_header(opts["--namespace"]))
      elif command == "cases":
         _write_if_changed(Path(second), emit_cases(Path(first), platform, deps))
      elif command == "export":
         values = json.loads(Path(second).read_text(encoding="utf-8"))
         print(export_record(load(first, platform), values, raw).hex())
      else:
         values, flags = import_record(load(first, platform), bytes.fromhex(second))
         print(json.dumps({"record": values, "flags": flag_names(flags)}, indent=1))
   except (ManifestError, TransferError, ValueError, KeyError, OSError) as e:
      print(f"{first}: error: {type(e).__name__ if isinstance(e, TransferError) else 'error'}: {e}",
            file=sys.stderr)
      return 1
   if "--depfile" in opts:
      out = Path(second)
      def esc(p: Path) -> str:
         """!A path as a Make depfile writes it (forward slashes, spaces escaped)."""
         return str(p).replace("\\", "/").replace(" ", "\\ ")
      deps.append(Path(__file__).resolve())
      _write_if_changed(Path(opts["--depfile"]), f"{esc(out)}: " + " ".join(esc(p) for p in sorted(set(deps))) + "\n")
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv))
