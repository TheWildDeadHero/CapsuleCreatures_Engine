"""!Record transfer tables (capsule::monster::TransferInfo, capsule/monster/transfer.h) of the battle compiler:
the transfer keys of a layout's stats and custom roles, their canonical order, the per-stat defaults an import
gives a stat the sender does not have, and the transfer hash (the raw form's key).

compile_transfer() takes a compiled layout (layout.compile_layout()) and the manifest's stats (their `transfer`
keys); emit_cpp() writes the C++ table. tools/record_transfer.py exports and imports records with the same
tables (the Python mirror of src/monster/transfer.cpp) and is the command line (`emit`).

A transfer fixture (tests, tools) is a layout without a battle: {"schema": "capsule.transfer_fixture/1", "stats":
[{"name": ..., "transfer": ...}], "monster_layout": ...}; load_fixture() compiles one.
"""
from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

from .common import Json, ManifestError, check_keys, register_keys
from .layout import (FNV_OFFSET_BASIS, FNV_PRIME, PER_STAT_ROLES, ROLE_ABSENT, ROLE_NAMES, ROLES,
                     U32_MASK, check_key_collisions, compile_layout, stat_key)

## The offset an unstored custom role's RoleSlot holds (no record bits; capsule::monster::kAbsent).
ABSENT = f"0x{ROLE_ABSENT:04X}"
## Schema of a transfer fixture.
FIXTURE_SCHEMA = "capsule.transfer_fixture/1"
## Transfer format version (capsule::monster::kTransferVersion; the canonical form's first byte).
TRANSFER_VERSION = 2
## First byte of the raw form (capsule::monster::kTransferRawVersion).
TRANSFER_RAW_VERSION = 0x82

register_keys({
   "transfer_fixture": {"schema", "stats", "monster_layout"},
   "transfer_fixture_stat": {"name", "transfer"},
})


def _fold(h: int, value: int) -> int:
   """!One FNV-1a step over a whole 32-bit value (as the layout hash folds words).
   @param h      hash so far
   @param value  value
   @return new hash
   """
   return ((h ^ value) * FNV_PRIME) & U32_MASK


def transfer_hash(layout_hash: int, stat_keys: Sequence[int]) -> int:
   """!The transfer hash (capsule::monster::TransferInfo::hash): the layout hash, then each stat key in the game's
   stat order, folded as words. Two games with the same transfer hash send records raw.
   @param layout_hash  compiled layout hash
   @param stat_keys    stat keys in stat order
   @return 32-bit hash
   """
   h = _fold(FNV_OFFSET_BASIS, layout_hash)
   for k in stat_keys:
      h = _fold(h, k)
   return h


def compile_transfer(layout: Mapping[str, Any], stats: Sequence[Mapping[str, Any]], where: str) -> dict[str, Any]:
   """!The transfer table of a layout.
   @param layout  compile_layout() result
   @param stats   the stats in order, each with "name" and "transfer" (stats.compile_stats() or a fixture's)
   @param where   location
   @return dict: layout, stat_names, stat_transfers, stat_keys (stat order), stat_order (stat indices by ascending
           key), custom_keys (layout order), custom_order (custom indices by ascending key), stat_defaults (per
           stat, one value per PER_STAT_ROLES entry), hash
   @throws ManifestError when keys collide or a transfer default names no stat
   """
   names = [st["name"] for st in stats]
   transfers = [st["transfer"] for st in stats]
   if len(stats) != layout["stat_count"]:
      raise ManifestError(f"{where}: {len(stats)} stats, the layout was compiled for {layout['stat_count']}")
   check_key_collisions(transfers, stat_key, "stat transfer keys", f"{where}: stats")
   stat_keys = [stat_key(t) for t in transfers]
   custom_keys = [c["transfer_key"] for c in layout["custom"]]
   defaults = layout.get("transfer_defaults", {})
   unknown = sorted(set(defaults) - set(names))
   if unknown:
      raise ManifestError(f"{where}: monster_layout.transfer_defaults names no stat: {', '.join(unknown)} "
                          f"(stats: {', '.join(names)})")
   stat_defaults = [[defaults.get(n, {}).get(r, 0) for r in PER_STAT_ROLES] for n in names]
   return {"layout": layout, "stat_names": names, "stat_transfers": transfers, "stat_keys": stat_keys,
           "stat_order": sorted(range(len(stat_keys)), key=lambda i: stat_keys[i]), "custom_keys": custom_keys,
           "custom_order": sorted(range(len(custom_keys)), key=lambda i: custom_keys[i]),
           "stat_defaults": stat_defaults, "hash": transfer_hash(layout["hash"], stat_keys)}


def load_fixture(data: Json, where: str, max_record_bytes: int | None = None,
                 max_party_size: int | None = None) -> dict[str, Any]:
   """!Compiles a transfer fixture (a layout and its stats, no battle).
   @param data              the fixture
   @param where             location
   @param max_record_bytes  largest record (the platform's)
   @param max_party_size    largest party (the platform's)
   @return compile_transfer() result
   """
   check_keys(data, "transfer_fixture", where)
   if data.get("schema") != FIXTURE_SCHEMA:
      raise ManifestError(f"{where}: schema must be {FIXTURE_SCHEMA!r}")
   entries = data.get("stats")
   if not isinstance(entries, list) or not entries:
      raise ManifestError(f"{where}: stats must list at least one stat")
   stats = []
   for i, sd in enumerate(entries):
      check_keys(sd, "transfer_fixture_stat", f"{where}: stats[{i}]")
      stats.append({"name": sd["name"], "transfer": sd.get("transfer", sd["name"])})
   layout = compile_layout(data.get("monster_layout"), f"{where}: monster_layout", max_record_bytes, len(stats),
                           max_party_size)
   return compile_transfer(layout, stats, where)


def _list(values: Sequence[int], fmt: str = "{}") -> str:
   """!A C++ initializer list.
   @param values  values
   @param fmt     format of one value
   @return "{a, b, ...}"
   """
   return "{" + ", ".join(fmt.format(v) for v in values) + "}"


def emit_cpp(src_name: str, info: Mapping[str, Any], namespace: str | None = None) -> str:
   """!The C++ transfer table.
   @param src_name   source manifest name (the header comment)
   @param info       compile_transfer() result
   @param namespace  None: capsule::monster::kTransfer for the build's kLayout; else the namespace of a stand-alone
                     table that also holds its own kLayout (a fixture: several layouts in one program)
   @return C++ source
   """
   lay = info["layout"]
   key = "0x{:08X}u"
   L = [f"// Generated by tools/record_transfer.py from {src_name}. Do not edit.",
        "#include \"capsule/monster/transfer.h\"", ""]
   if namespace is None:
      L += ["namespace capsule::monster", "{", ""]
   else:
      # const objects at namespace scope link internally unless declared extern first.
      L += ["using namespace capsule::monster;", "", f"namespace {namespace}", "{", "",
            "extern const Layout kLayout;", "extern const TransferInfo kTransfer;", ""]
   L.append("namespace")
   L.append("{")
   L.append("")
   n, c = len(info["stat_keys"]), len(info["custom_keys"])
   # Zero-length arrays are not C++: an empty table is a nullptr.
   tables = [("uint32_t", "kStatKeys", info["stat_keys"], key), ("uint8_t", "kStatOrder", info["stat_order"], "{}"),
             ("uint32_t", "kCustomKeys", info["custom_keys"], key),
             ("uint8_t", "kCustomOrder", info["custom_order"], "{}"),
             ("uint8_t", "kStatDefaults", [v for row in info["stat_defaults"] for v in row], "{}")]
   names = {}
   for ctype, name, values, fmt in tables:
      # all-0 stat defaults are left out too: the engine reads nullptr as 0
      if values and (name != "kStatDefaults" or any(values)):
         L.append(f"const {ctype} {name}[] = {_list(values, fmt)};")
      names[name] = name if values and (name != "kStatDefaults" or any(values)) else "nullptr"
   if namespace is not None:
      L += [f"const uint16_t kOffsets[] = {_list(lay['offsets'], '0x{:04X}')};",
            f"const uint8_t kWidths[] = {_list(lay['widths'])};",
            f"const uint8_t kCounts[] = {_list(lay['counts'])};",
            "static_assert(sizeof(kOffsets) / sizeof(kOffsets[0]) == static_cast<int>(Role::NUM_ROLES));"]
      # the stored custom roles, then the unstored ones (E13a; C5: a fixture may have them too) without bits
      slots = lay["custom"] + lay.get("unstored", [])
      if slots:
         L.append("const RoleSlot kCustom[] = {")
         L += [f"   {{0x{r['name_hash']:08X}u, {r['default']}u, {r['offset'] if 'offset' in r else ABSENT}, "
               f"{r['width']}, {r['count']}}},   // {r['name']}{'' if 'offset' in r else ' (unstored)'}"
               for r in slots]
         L.append("};")
   L += ["", "} // namespace", ""]
   if namespace is not None:
      L += ["/// The fixture's layout.",
            f"const Layout kLayout = {{kOffsets, kWidths, kCounts, "
            f"{'kCustom' if lay['custom'] or lay.get('unstored') else 'nullptr'}, "
            f"0x{lay['hash']:08X}u, {lay['bytes']}, {lay['stat_count']}, {lay['move_slots']}, {lay['party_size']}, "
            f"{len(lay['custom'])}, {len(lay.get('unstored', []))}, {{0}}, "
            f"0x{lay['record_hash']:08X}u}};", ""]
   L += ["/// The layout's transfer table.",
         f"const TransferInfo kTransfer = {{&kLayout, {names['kStatKeys']}, {names['kStatOrder']}, "
         f"{names['kCustomKeys']}, {names['kCustomOrder']}, {names['kStatDefaults']}, 0x{info['hash']:08X}u}};",
         f"static_assert({n} <= kMaxTransferStats && {c} <= kMaxCustomRoles);", ""]
   L.append(f"}} // namespace {namespace or 'capsule::monster'}")
   return "\n".join(L) + "\n"


def emit_header(namespace: str) -> str:
   """!The C++ header declaring a stand-alone (fixture) table of emit_cpp().
   @param namespace  its namespace
   @return C++ header
   """
   return "\n".join(["// Generated by tools/record_transfer.py. Do not edit.", "#pragma once", "",
                     "#include \"capsule/monster/transfer.h\"", "", f"namespace {namespace}", "{", "",
                     "/// The fixture's layout.", "extern const capsule::monster::Layout kLayout;",
                     "/// The layout's transfer table.", "extern const capsule::monster::TransferInfo kTransfer;", "",
                     f"}} // namespace {namespace}", ""])


def role_names(info: Mapping[str, Any]) -> list[str]:
   """!The roles a table's layout holds, in canonical order (engine roles by ID, then custom roles by key).
   @param info  compile_transfer() result
   @return role names
   """
   lay = info["layout"]
   engine = [ROLE_NAMES[i] for i in range(len(ROLES)) if lay["offsets"][i] != ROLE_ABSENT]
   return engine + [lay["custom"][i]["name"] for i in info["custom_order"]]
