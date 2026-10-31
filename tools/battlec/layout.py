"""!Creature record layout (capsule::monster::kLayout) of the battle compiler:
the frozen role registry and compile_layout(), which packs a manifest's
monster_layout into bit offsets, element widths and counts, the custom roles,
a byte size and the layout hash.

monster_layout is a list of role entries, or (layout v2) an object
{"party_size": n, "move_slots": n, "roles": [...]} (the registry's counts 6
and 4 when left out; a battle manifest must state both, require_counts(): no
game's numbers by default, D-22 decision 8). An entry is:

- an engine role name (ROLES), or {"role": name, "width": bits, "count": n}
  for an engine role whose width (ROLE_MAX_WIDTHS) or count (ROLE_MAX_COUNTS)
  the layout chooses (D-23 decision 4: up to the engine's maximum);
- a custom role (D-22 decision 2), {"custom": name, "width": bits, "count": n,
  "default": value} (count 1 and default 0 when left out): a field the engine
  stores and scripts read and write (script_compiler role_get / role_set),
  ID CUSTOM_ROLE_BASE + its index among the custom roles;
- an unstored custom role (E13a), the same with "stored": false: a value the
  generation list computes whenever it is read (battle::record_value(): its
  derived or constant entry), so it never goes stale. It takes no record bits,
  is left out of the hash, the transfer and field scripts (it is not held:
  role_info() is None), and its ID follows every stored custom role's
  (CUSTOM_ROLE_BASE + stored custom roles + its index among the unstored
  ones; capsule::monster::Layout::unstored_count); effect logic reads it;
- a retired engine role's name (RETIRED_ROLES, A10), read as the custom role of
  that name and its registry shape (old manifests keep compiling).

The v2 object may also give "status_bits": "auto" (default) or a number: the
bits of the primary_status role that hold the status id, the rest holding its
counter (Battler::status_counter). "auto" takes half the role (4 of 8 bits)
while the statuses' ids fit, else as few bits as fit them (at most 6: 63
statuses); a choice other than 4 is folded into the hash.

The v2 object may also give "transfer_defaults": {stat name: value, or
{per-stat role: value}}: what a record transfer (capsule/monster/transfer.h,
tools/battlec/transfer.py) gives a stat the sending game does not have
(default 0); the stat names are checked against the manifest's stats when the
transfer table is compiled. Every custom role has a transfer key (role_key():
FNV-1a 32 of "role:" + its name); keys that collide are an error.

Per-stat roles hold one element per stat of the manifest (stat_count), the
move roles one per move slot. The hash (FNV-1a 32) folds every held engine
role's ID in record order, a chosen width or count only where it differs from
the registry, each custom role as (name hash, width, count) — a custom role
with a retired role's name and registry shape folds that role's ID, as the
engine role did — and the party size where it differs from the default; so a
layout that keeps the registry's shapes keeps its hash. Unstored custom roles
change no record byte, so they are not folded.
"""
from __future__ import annotations

import re
from collections.abc import Callable
from typing import Any

from .common import Json, ManifestError, check_keys, integer, register_keys

## Stats of the layout the registry's per-stat counts describe (the hash folds a per-stat role's count only when
## it differs).
DEFAULT_STAT_COUNT = 6
## Move slots of a layout that names none (capsule::monster::kDefaultMoveSlots; the registry's move-role counts).
DEFAULT_MOVE_SLOTS = 4
## Most move slots (capsule::monster::kMaxMoveSlots).
MAX_MOVE_SLOTS = 8
## Party size of a layout that names none (capsule::monster::kDefaultPartySize).
DEFAULT_PARTY_SIZE = 6
## Element count rules, in capsule::monster::RoleCount order.
COUNT_RULES = ["one", "stat", "move", "chosen", "retired"]
## Role registry, in capsule::monster::Role order: (name, width, count, count rule). Frozen. A per-stat role
## ("stat") holds one element per stat, a move role ("move") one per move slot; their counts here are the
## defaults the hash takes as given. "retired" roles are custom roles now (A10).
ROLES = [
   ("species_key", 32, 1, "one"), ("seed", 32, 1, "one"), ("owner_id", 16, 1, "one"),
   ("owner_secret", 16, 1, "retired"), ("experience", 24, 1, "one"), ("level", 8, 1, "one"),
   ("moves", 16, DEFAULT_MOVE_SLOTS, "move"), ("pp", 7, DEFAULT_MOVE_SLOTS, "move"),
   ("pp_ups", 2, DEFAULT_MOVE_SLOTS, "move"), ("stat_genes", 5, DEFAULT_STAT_COUNT, "stat"),
   ("stat_training", 8, DEFAULT_STAT_COUNT, "stat"), ("stat_awaken", 8, DEFAULT_STAT_COUNT, "stat"),
   ("trait", 5, 1, "one"), ("passive_selector", 4, 1, "one"), ("held_item", 16, 1, "one"),
   ("current_hp", 16, 1, "one"), ("primary_status", 8, 1, "one"), ("friendship", 8, 1, "retired"),
   ("form", 8, 1, "retired"), ("nicknamed", 1, 1, "retired"), ("nickname", 8, 12, "chosen"),
   ("owner_name", 8, 12, "retired"), ("capture_device", 32, 1, "retired"), ("text_language", 8, 1, "one"),
]
## Engine role names, in ID order.
ROLE_NAMES = [r[0] for r in ROLES]
## Registry (width, count) per engine role name.
ROLE_SHAPES = {name: (width, count) for name, width, count, _rule in ROLES}
## Per-stat engine roles, in ID order (capsule::monster::kFirstPerStatRole on; transfer defaults name them).
PER_STAT_ROLES = [r[0] for r in ROLES if r[3] == "stat"]
## Retired engine roles (A10): custom roles now; a layout may still name them (as custom roles of their shape).
RETIRED_ROLES = [r[0] for r in ROLES if r[3] == "retired"]
## Widest element a layout may choose per engine role (capsule::monster::RoleChoice::max_width); roles not listed
## have their registry width. Battlers keep the 8-bit and 16-bit ones in bytes and halfwords.
ROLE_MAX_WIDTHS = {"seed": 32, "owner_id": 32, "experience": 32, "level": 8, "moves": 16, "pp": 8, "pp_ups": 8,
                   "stat_genes": 8, "stat_training": 8, "stat_awaken": 8, "trait": 8, "passive_selector": 8,
                   "held_item": 16, "current_hp": 16, "text_language": 8}
## Most elements a layout may choose per "chosen" engine role (capsule::monster::RoleChoice::max_count; the
## nickname: capsule::battle::kNameCodes).
ROLE_MAX_COUNTS = {"nickname": 12}
## Roles a battle needs (D-25: the pools are the resources' persist roles, checked by battlec/resources.py; pp,
## pp_ups and current_hp are plain storage roles a resource may name).
REQUIRED_ROLES = ["species_key", "level", "experience", "moves", "primary_status"]
## capsule::monster::kMaxRecordBytes on the GBA (build_settings.PLATFORM_LIMITS has every platform's).
MAX_RECORD_BYTES = 100
## capsule::monster::kMaxPartySize on the GBA (build_settings.PLATFORM_LIMITS has every platform's).
MAX_PARTY_SIZE = 6
## Most custom roles (capsule::monster::kMaxCustomRoles).
MAX_CUSTOM_ROLES = 32
## ID of the first custom role (capsule::monster::kFirstCustomRole).
CUSTOM_ROLE_BASE = 0x80
## Most elements of a custom role (capsule::monster::RoleSlot::count is 8 bits).
MAX_CUSTOM_COUNT = 255
## Widest role element.
MAX_ROLE_BITS = 32
## Offset of a role the layout leaves out (capsule::monster::kAbsent).
ROLE_ABSENT = 0xFFFF
## FNV-1a 32-bit offset basis (the layout hash; capsule::monster::kFnvOffsetBasis).
FNV_OFFSET_BASIS = 0x811C9DC5
## FNV-1a 32-bit prime (capsule::monster::kFnvPrime).
FNV_PRIME = 0x01000193
## Mask keeping 32 bits.
U32_MASK = 0xFFFFFFFF
## Hash tag of a per-stat role's stat count (above every role ID, so it never reads as a role).
HASH_STAT_COUNT_TAG = 0x100
## Hash tag of a role's chosen width.
HASH_WIDTH_TAG = 0x200
## Hash tag of a role's element count (move slots or a chosen count) where it differs from the registry's.
HASH_COUNT_TAG = 0x300
## Hash tag opening a custom role (then its name hash, width and count).
HASH_CUSTOM_TAG = 0x400
## Hash tag of a party size other than DEFAULT_PARTY_SIZE.
HASH_PARTY_SIZE_TAG = 0x500
## Hash tag of a primary-status id width other than DEFAULT_STATUS_BITS.
HASH_STATUS_BITS_TAG = 0x600
## Status id bits of the primary_status role before E5 (the "auto" choice while the statuses fit: half the role).
DEFAULT_STATUS_BITS = 4
## Widest status id (capsule::battle::kMaxStatusBits: 63 statuses).
MAX_STATUS_BITS = 6
## "auto" status bits take the role's width divided by this (half: the rest holds the status counter).
AUTO_STATUS_SHARE = 2
## Bits per byte.
BITS_PER_BYTE = 8
## Bits per record word (records are stored in whole 32-bit words).
BITS_PER_WORD = 32
## Bytes per record word.
BYTES_PER_WORD = 4
## Creatures per party of a layout that names none (kept for the studio: party_max()).
PARTY_SIZE = DEFAULT_PARTY_SIZE
## Keys a battle manifest's monster_layout must state (require_counts(): no game's numbers by default, D-22
## decision 8; E8 made move_slots required as well).
LAYOUT_REQUIRED = ("party_size", "move_slots")
## Prefix of a custom role's transfer key (capsule::monster::transfer_key("role:", name)).
ROLE_KEY_PREFIX = "role:"
## Prefix of a stat's transfer key (capsule::monster::transfer_key("stat:", key)).
STAT_KEY_PREFIX = "stat:"
## A custom role's name: a lower-case identifier.
CUSTOM_NAME = re.compile(r"[a-z][a-z0-9_]*\Z")

register_keys({
   "layout": {"party_size", "move_slots", "roles", "transfer_defaults", "status_bits"},
   "layout_role": {"role", "width", "count"},
   "layout_custom": {"custom", "width", "count", "default", "stored"},
})


def name_hash(name: str) -> int:
   """!The hash a custom role is found by (capsule::monster::name_hash(): FNV-1a 32 over its UTF-8 bytes).
   @param name  role name
   @return 32-bit hash
   """
   h = FNV_OFFSET_BASIS
   for b in name.encode("utf-8"):
      h = ((h ^ b) * FNV_PRIME) & U32_MASK
   return h


def role_key(name: str) -> int:
   """!A custom role's transfer key (capsule::monster::transfer_key("role:", name)).
   @param name  role name
   @return 32-bit key
   """
   return name_hash(ROLE_KEY_PREFIX + name)


def stat_key(transfer: str) -> int:
   """!A stat's transfer key (capsule::monster::transfer_key("stat:", key)).
   @param transfer  the stat's transfer key (the manifest's `transfer`, default its name)
   @return 32-bit key
   """
   return name_hash(STAT_KEY_PREFIX + transfer)


def check_key_collisions(names: list[str], key: Callable[[str], int], what: str, where: str) -> None:
   """!Rejects names whose transfer keys collide (two names, one key: a transfer could not tell them apart).
   @param names  the names (distinct)
   @param key    their key function (role_key or stat_key)
   @param what   what they name (messages)
   @param where  location
   @return None
   @throws ManifestError naming both
   """
   seen: dict[int, str] = {}
   for name in names:
      k = key(name)
      if k in seen:
         raise ManifestError(f"{where}: the {what} {seen[k]!r} and {name!r} have the same transfer key 0x{k:08X}; "
                             f"rename one")
      seen[k] = name


def _transfer_defaults(value: Json, widths: list[int], offsets: list[int], where: str) -> dict[str, dict[str, int]]:
   """!Checks a layout's transfer_defaults (the stat names are checked later, against the manifest's stats).
   @param value    the manifest's transfer_defaults
   @param widths   compiled element width per engine role
   @param offsets  compiled bit offset per engine role (ROLE_ABSENT if not held)
   @param where    location
   @return {stat name: {per-stat role name: value}} (an integer expanded to every held per-stat role)
   """
   if not isinstance(value, dict):
      raise ManifestError(f"{where}: an object {{stat name: value or {{per-stat role: value}}}}")
   held = [r for r in PER_STAT_ROLES if offsets[ROLE_NAMES.index(r)] != ROLE_ABSENT]
   out = {}
   for stat, given in value.items():
      w = f"{where}.{stat}"
      per_role = {r: given for r in held} if not isinstance(given, dict) else given
      for role in per_role:
         if role not in held:
            raise ManifestError(f"{w}: {role!r} is no per-stat role of the layout ({', '.join(held) or 'none'})")
      out[stat] = {r: integer(v, 0, (1 << widths[ROLE_NAMES.index(r)]) - 1, f"{w}.{r}") for r, v in per_role.items()}
   return out


def role_width_max(name: str) -> int:
   """!The widest element a layout may give an engine role.
   @param name  role name
   @return bits
   """
   return ROLE_MAX_WIDTHS.get(name, ROLE_SHAPES[name][0])


def role_count_max(name: str) -> int:
   """!The most elements a layout may give an engine role ("chosen" roles; the others their registry count).
   @param name  role name
   @return elements
   """
   return ROLE_MAX_COUNTS.get(name, ROLE_SHAPES[name][1])


def role_max_value(layout: dict[str, Any], name: str) -> int:
   """!The highest value an element of a role holds in a compiled layout (capsule::monster::max_value()).
   @param layout  compile_layout() result
   @param name    role name (an engine role, or a custom role of the layout)
   @return 2^width - 1
   """
   if name in layout.get("unstored_ids", {}):
      return (1 << unstored_info(layout, name)[1]) - 1
   if name in ROLE_NAMES and name not in layout["custom_ids"]:
      return (1 << layout["widths"][ROLE_NAMES.index(name)]) - 1
   return (1 << layout["custom"][layout["custom_ids"][name] - CUSTOM_ROLE_BASE]["width"]) - 1


def role_info(layout: dict[str, Any], name: str) -> tuple[int, int, int] | None:
   """!A role the compiled layout holds, by name.
   @param layout  compile_layout() result
   @param name    role name
   @return (ID, width, count), or None when the layout does not hold it
   """
   if name in layout["custom_ids"]:
      c = layout["custom"][layout["custom_ids"][name] - CUSTOM_ROLE_BASE]
      return c["id"], c["width"], c["count"]
   if name in ROLE_NAMES and layout["offsets"][ROLE_NAMES.index(name)] != ROLE_ABSENT:
      rid = ROLE_NAMES.index(name)
      return rid, layout["widths"][rid], layout["counts"][rid]
   return None


def unstored_info(layout: dict[str, Any], name: str) -> tuple[int, int, int] | None:
   """!An unstored custom role of the compiled layout (E13a: computed on read, no record bits), by name.
   @param layout  compile_layout() result
   @param name    role name
   @return (ID, width, count), or None when the layout declares no unstored role of that name
   """
   for u in layout.get("unstored", []):
      if u["name"] == name:
         return u["id"], u["width"], u["count"]
   return None


def _fold(h: int, value: int) -> int:
   """!One FNV-1a step over a whole value (the layout hash folds IDs, tags and shapes as one word each).
   @param h      hash so far
   @param value  value
   @return new hash
   """
   return ((h ^ value) * FNV_PRIME) & U32_MASK


def _custom_entry(entry: Json, w: str) -> tuple[str, int, int, int, bool]:
   """!Checks a custom role entry.
   @param entry  the {"custom", ...} object
   @param w      location
   @return (name, width, count, default, stored)
   """
   check_keys(entry, "layout_custom", w)
   name = entry["custom"]
   if not isinstance(name, str) or not CUSTOM_NAME.match(name):
      raise ManifestError(f"{w}: custom must be a lower-case identifier, got {name!r}")
   if name in ROLE_NAMES and name not in RETIRED_ROLES:
      raise ManifestError(f"{w}: {name!r} is an engine role (list it by name)")
   if "width" not in entry:
      raise ManifestError(f"{w}: a custom role needs a width (1-{MAX_ROLE_BITS} bits)")
   width = integer(entry["width"], 1, MAX_ROLE_BITS, f"{w}.width")
   count = integer(entry.get("count", 1), 1, MAX_CUSTOM_COUNT, f"{w}.count")
   default = integer(entry.get("default", 0), 0, (1 << width) - 1, f"{w}.default")
   stored = entry.get("stored", True)
   if not isinstance(stored, bool):
      raise ManifestError(f"{w}.stored: true (the record holds it) or false (computed on read)")
   return name, width, count, default, stored


def status_id_bits(choice: Json, status_count: int, where: str) -> int:
   """!The bits of the primary_status role that hold the status id (the rest hold its counter).
   @param choice        the layout's status_bits: "auto" (half the role while the ids fit, else as few as fit)
                        or a number
   @param status_count  statuses (ids 1 to status_count; 0 = none)
   @param where         location
   @return 1 to MAX_STATUS_BITS
   """
   width = ROLE_SHAPES["primary_status"][0]
   need = max(1, status_count.bit_length())
   if need > MAX_STATUS_BITS:
      raise ManifestError(f"{where}: {status_count} statuses need {need} status bits; at most {MAX_STATUS_BITS}")
   if choice == "auto":
      return max(min(width // AUTO_STATUS_SHARE, MAX_STATUS_BITS), need)
   bits = integer(choice, 1, min(width, MAX_STATUS_BITS), where)
   if bits < need:
      raise ManifestError(f"{where}: {bits} bits hold ids up to {(1 << bits) - 1}, the data has {status_count} "
                          f"statuses")
   return bits


def require_counts(roles: Json, where: str) -> None:
   """!Checks that a battle manifest's monster_layout states its party size and move slots (LAYOUT_REQUIRED: no
   default, D-22 decision 8; compile_layout() alone keeps the registry's counts for transfer fixtures and tests).
   @param roles  the manifest's monster_layout
   @param where  location
   @return None
   @throws ManifestError when it is a bare role list or leaves one out
   """
   if not isinstance(roles, dict):
      raise ManifestError(f"{where}: an object {{\"party_size\": n, \"move_slots\": n, \"roles\": [...]}} (a "
                          f"battle states its party size and move slots)")
   missing = [k for k in LAYOUT_REQUIRED if k not in roles]
   if missing:
      raise ManifestError(f"{where}: needs {', '.join(missing)} (no default)")


def compile_layout(roles: Json, where: str, max_record_bytes: int | None = None,
                   stat_count: int = DEFAULT_STAT_COUNT, max_party_size: int | None = None,
                   status_count: int = 0) -> dict[str, Any]:
   """!Packs the monster layout.
   @param roles             the role entries in order (see the module docstring), or the v2 object
   @param where             location
   @param max_record_bytes  largest record (default MAX_RECORD_BYTES; the driver passes its platform's)
   @param stat_count        stats of the manifest (elements of each per-stat role)
   @param max_party_size    largest party (default MAX_PARTY_SIZE; the driver passes its platform's)
   @param status_count      statuses of the manifest (status_bits "auto" fits their ids)
   @return dict with offsets, widths and counts (per engine role), custom (per stored custom role: name, id,
           offset, width, count, default, name_hash, transfer_key), unstored (per unstored custom role: name, id,
           width, count, default, name_hash; E13a), custom_ids / unstored_ids and ids (name -> role ID, unstored
           ones too), bytes, hash, record_hash (C7: the hash without the party size), bits, roles (held names in
           order), stat_count, move_slots, party_size, status_bits, transfer_defaults ({stat name: {per-stat role:
           value}})
   """
   party_limit = MAX_PARTY_SIZE if max_party_size is None else max_party_size
   move_slots, party_size = DEFAULT_MOVE_SLOTS, DEFAULT_PARTY_SIZE
   transfer_defaults: Json = {}
   defaults_where = f"{where}.transfer_defaults"
   status_choice: Json = "auto"
   if isinstance(roles, dict):
      check_keys(roles, "layout", where)
      move_slots = integer(roles.get("move_slots", DEFAULT_MOVE_SLOTS), 1, MAX_MOVE_SLOTS, f"{where}.move_slots")
      party_size = integer(roles.get("party_size", DEFAULT_PARTY_SIZE), 1, party_limit, f"{where}.party_size")
      transfer_defaults = roles.get("transfer_defaults", {})
      status_choice = roles.get("status_bits", "auto")
      roles, where = roles.get("roles"), f"{where}.roles"
   status_bits = status_id_bits(status_choice, status_count, f"{where}: status_bits")
   if party_size > party_limit:
      raise ManifestError(f"{where}: a party of {party_size}, the engine holds at most {party_limit}")
   if not isinstance(roles, list) or not roles:
      raise ManifestError(f"{where}: a list of role entries")
   offsets = [ROLE_ABSENT] * len(ROLES)
   widths = [w for _name, w, _count, _rule in ROLES]
   counts = [stat_count if rule == "stat" else move_slots if rule == "move" else n for _name, _w, n, rule in ROLES]
   custom = []
   unstored = []
   bit = 0
   seen = []
   h = FNV_OFFSET_BASIS
   for i, entry in enumerate(roles):
      w = f"{where}[{i}]"
      name, width, count, default, is_custom, stored = entry, None, None, 0, False, True
      if isinstance(entry, dict) and "custom" in entry:
         name, width, count, default, stored = _custom_entry(entry, w)
         is_custom = True
      elif isinstance(entry, dict):
         check_keys(entry, "layout_role", w)
         name = entry.get("role")
         if name in RETIRED_ROLES:
            raise ManifestError(f"{w}: {name!r} is a custom role now: {{\"custom\": {name!r}, \"width\": ...}}")
      if not is_custom and name in RETIRED_ROLES:   # a bare retired name: the custom role of its old shape
         width, count = ROLE_SHAPES[name]
         is_custom = True
      if not is_custom and name not in ROLE_NAMES:
         raise ManifestError(f"{w}: unknown role {name!r} (engine roles: "
                             f"{', '.join(n for n in ROLE_NAMES if n not in RETIRED_ROLES)}; "
                             f"or {{\"custom\": name, \"width\": bits}})")
      if name in seen or any(u["name"] == name for u in unstored):
         raise ManifestError(f"{w}: role {name!r} listed twice")
      if is_custom and len(custom) + len(unstored) >= MAX_CUSTOM_ROLES:
         raise ManifestError(f"{w}: at most {MAX_CUSTOM_ROLES} custom roles (stored and unstored)")
      if not stored:
         # computed on read (E13a): no bits, no hash; its ID is given once the stored roles are counted
         unstored.append({"name": name, "width": width, "count": count, "default": default,
                          "name_hash": name_hash(name)})
         continue
      seen.append(name)

      if is_custom:
         size = width * count
      else:
         rid = ROLE_NAMES.index(name)
         _, default_width, default_count, rule = ROLES[rid]
         if isinstance(entry, dict) and "width" in entry:
            if name not in ROLE_MAX_WIDTHS:
               raise ManifestError(f"{w}: the width of role {name!r} is fixed ({default_width} bits)")
            width = integer(entry["width"], 1, ROLE_MAX_WIDTHS[name], f"{w}.width")
            widths[rid] = width
         if isinstance(entry, dict) and "count" in entry:
            if name not in ROLE_MAX_COUNTS:
               raise ManifestError(f"{w}: the count of role {name!r} is fixed by the layout "
                                   f"({'move_slots' if rule == 'move' else 'stats' if rule == 'stat' else 1})")
            count = integer(entry["count"], 1, ROLE_MAX_COUNTS[name], f"{w}.count")
            counts[rid] = count
         size = widths[rid] * counts[rid]
      if size >= BITS_PER_BYTE and bit % BITS_PER_BYTE:   # byte-align fields of a byte or more (cheaper reads)
         bit += BITS_PER_BYTE - bit % BITS_PER_BYTE

      # FNV-1a over the layout in record order: the hash changes with the layout. Shapes are folded only where
      # they differ from the registry, so layouts with the registry's shapes keep their hash.
      if is_custom:
         legacy = name in RETIRED_ROLES and (width, count) == ROLE_SHAPES[name]
         if legacy:   # the retired engine role's own ID: its records and hash are what they were
            h = _fold(h, ROLE_NAMES.index(name))
         else:
            for v in (HASH_CUSTOM_TAG, name_hash(name), width, count):
               h = _fold(h, v)
         custom.append({"name": name, "id": CUSTOM_ROLE_BASE + len(custom), "offset": bit, "width": width,
                        "count": count, "default": default, "name_hash": name_hash(name),
                        "transfer_key": role_key(name)})
      else:
         offsets[rid] = bit
         h = _fold(h, rid)
         if rule == "stat" and stat_count != DEFAULT_STAT_COUNT:
            h = _fold(h, HASH_STAT_COUNT_TAG | stat_count)
         if width is not None and width != default_width:
            h = _fold(h, HASH_WIDTH_TAG | width)
         if rule in ("move", "chosen") and counts[rid] != default_count:
            h = _fold(h, HASH_COUNT_TAG | counts[rid])
      bit += size
   # C7: the record's shape alone (no party size): what the save image's party and boxes sections check, so a
   # party that grows or shrinks keeps its saves (capsule::monster::Layout::record_hash)
   record_h = h
   if party_size != DEFAULT_PARTY_SIZE:
      h = _fold(h, HASH_PARTY_SIZE_TAG | party_size)
   if status_bits != DEFAULT_STATUS_BITS:
      h = _fold(h, HASH_STATUS_BITS_TAG | status_bits)
      record_h = _fold(record_h, HASH_STATUS_BITS_TAG | status_bits)
   missing = [r for r in REQUIRED_ROLES if r not in seen]
   if missing:
      raise ManifestError(f"{where}: battles need the role(s) {', '.join(missing)}")

   # the record is a whole number of 32-bit words
   nbytes = (bit + BITS_PER_WORD - 1) // BITS_PER_WORD * BYTES_PER_WORD
   limit = MAX_RECORD_BYTES if max_record_bytes is None else max_record_bytes
   if nbytes > limit:
      raise ManifestError(f"{where}: the record needs {nbytes} bytes, the engine stores at most {limit}")
   check_key_collisions([c["name"] for c in custom], role_key, "custom roles", where)
   defaults = _transfer_defaults(transfer_defaults, widths, offsets, defaults_where)
   custom_ids = {c["name"]: c["id"] for c in custom}
   ids = {n: custom_ids.get(n, ROLE_NAMES.index(n) if n in ROLE_NAMES else None) for n in seen}
   # unstored roles follow every stored custom role (capsule::monster::Layout::unstored_count)
   for j, u in enumerate(unstored):
      u["id"] = CUSTOM_ROLE_BASE + len(custom) + j
   unstored_ids = {u["name"]: u["id"] for u in unstored}
   ids.update(unstored_ids)
   return {"offsets": offsets, "widths": widths, "counts": counts, "custom": custom, "custom_ids": custom_ids,
           "unstored": unstored, "unstored_ids": unstored_ids,
           "ids": ids, "bytes": nbytes, "hash": h, "record_hash": record_h, "bits": bit, "roles": list(seen),
           "stat_count": stat_count, "move_slots": move_slots, "party_size": party_size, "status_bits": status_bits,
           "transfer_defaults": defaults}


def check_layout_fits(layout: dict[str, Any], needs: dict[str, int], where: str) -> None:
   """!Checks that the layout's chosen widths hold the values the data puts in its engine roles.
   @param layout  compile_layout() result
   @param needs   engine role name -> highest value an element must hold (e.g. moves: the move count, so that
                  the all-ones "no move" is never a move)
   @param where   location
   @return None
   @throws ManifestError naming the first role too narrow
   """
   for name, need in needs.items():
      rid = ROLE_NAMES.index(name)
      if layout["offsets"][rid] != ROLE_ABSENT and need > role_max_value(layout, name):
         raise ManifestError(f"{where}: role {name!r} is {layout['widths'][rid]} bits wide, the data needs "
                             f"values up to {need}")
