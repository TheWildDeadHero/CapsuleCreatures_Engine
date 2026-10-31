"""!Inventory of the battle compiler (D-26 section 2, package G2): the structure's bag pockets, the items' bag and
use fields and the shop lists, emitted as capsule::inventory::kInventory (include/capsule/inventory/data.h) next
to the battle tables.

  - structure "pockets": [{name, label, capacity, quantity_max, sort}] (sort: added | index | name); the
    pockets' capacities are the bag's slots, at most the platform's max_bag_slots, at most max_pockets pockets;
  - battle.json items gain pocket (a pocket's name), price, use (none | field | battle | both), consumed,
    holdable, key (never consumed or sold) and target (none | member | move_slot | battler); a use names the
    effect logic hook the item needs (use_field / use_battle);
  - battle.json "shops": [{name, items: [item names]}] (the script primitive `shop` opens one).

Pocket, field and shop names are data; the engine reads only the table.
"""
from __future__ import annotations

import re
from collections.abc import Mapping, Sequence
from typing import Any

import effect_compiler as ec
from sequence_compiler import asset_key

from .common import NONE8, U16_MAX, Json, ManifestError, Texts, check_keys, ident, integer, register_keys

## A pocket's name: a lower-case identifier (items name their pocket by it; battle_ids.h POCKET_<NAME>).
POCKET_NAME = re.compile(r"[a-z][a-z0-9_]*\Z")
## Pocket sort rules, in capsule::inventory::SortRule order: as added, by item index, by display name.
SORT_RULES = ["added", "index", "name"]
## Default pocket sort rule.
DEFAULT_SORT = "added"
## Default most of one item a pocket slot holds.
DEFAULT_QUANTITY_MAX = 99
## Most of one item a slot holds (a 16-bit count).
MAX_QUANTITY = U16_MAX
## Item uses, in capsule::inventory::Use order.
ITEM_USES = ["none", "field", "battle", "both"]
## Default item use.
DEFAULT_USE = "none"
## Item targets, in capsule::inventory::Target order: nothing, a party member, a move slot of a member, a battler
## in battle.
ITEM_TARGETS = ["none", "member", "move_slot", "battler"]
## Default item target.
DEFAULT_TARGET = "none"
## The uses that run out of battle / in battle.
FIELD_USES = {"field", "both"}
## The uses that run in battle.
BATTLE_USES = {"battle", "both"}
## Effect logic hook of each kind of use (effect_compiler.HOOKS).
USE_HOOKS = {"field": "use_field", "battle": "use_battle"}
## Targets only a battle has.
BATTLE_TARGETS = {"battler"}
## Highest price (capsule::inventory::ItemInfo::price is 32 bits; a six-digit price fits every shop menu).
MAX_PRICE = 999999
## Item flag bits (capsule::inventory::kItemConsumed / kItemHoldable / kItemKey).
ITEM_FLAG_BITS = {"consumed": 1, "holdable": 2, "key": 4,
                  # B2: the item may be used on an egg (capsule::inventory::kItemEggs; default refused)
                  "allow_eggs": 8}
## The item fields this module reads (besides the effect keys: name, announce, handlers).
ITEM_FIELDS = ("pocket", "price", "use", "consumed", "holdable", "key", "target", "allow_eggs")
## Most shops (an 8-bit shop id).
MAX_SHOPS = 255
## Most items one shop lists (an 8-bit count).
MAX_SHOP_ITEMS = 255
## Pocket of an item that has none (capsule::inventory::kNoPocket): it cannot go into the bag.
NO_POCKET = NONE8
## Asset key namespace of the items' stable save keys (C6: "item:NAME"; a renamed item is another item to a save).
ITEM_KEY_NAMESPACE = "item"
## The save key no item may have (C7): the engine's "no item" (capsule::inventory::item_of_key()). Keys are FNV-1a 32
## of the name, so a removed item's key collides with a new item's with a chance of about one in 2^32 per pair: an
## old save's entry of the removed item would then load as the new item (the build cannot see removed names).
NO_SAVE_KEY = 0
## battle_ids.h prefix of the pocket indices.
POCKET_PREFIX = "POCKET"
## battle_ids.h prefix of the shop indices.
SHOP_PREFIX = "SHOP"

register_keys({
   "pocket": {"name", "label", "capacity", "quantity_max", "sort"},
   "item": {"name", "announce", "handlers", *ITEM_FIELDS},
   "shop": {"name", "items"},
})


def compile_pockets(src: Json, limits: Mapping[str, int], where: str, enc: Texts) -> list[dict[str, Any]]:
   """!The structure's bag pockets (D-26 section 2): each a slice of the bag's slots.
   @param src     the structure's "pockets" (None: no bag)
   @param limits  the platform's limits (build_settings.platform_limits(): max_pockets, max_bag_slots)
   @param where   location (the structure manifest)
   @param enc     this compile's text encoders
   @return [{"name", "label" (codes), "label_text", "capacity", "quantity_max", "sort", "sort_id", "first"}]
   @throws ManifestError for a malformed pocket, a name given twice or more slots than the platform holds
   """
   src = [] if src is None else src
   w0 = f"{where}: pockets"
   if not isinstance(src, list) or len(src) > limits["max_pockets"]:
      raise ManifestError(f"{w0}: a list of at most {limits['max_pockets']} pockets (this platform's max_pockets)")
   out: list[dict[str, Any]] = []
   first = 0
   for i, p in enumerate(src):
      w = f"{w0}[{i}]"
      check_keys(p, "pocket", w)
      name = p.get("name")
      if not isinstance(name, str) or not POCKET_NAME.match(name):
         raise ManifestError(f"{w}.name: a lower-case identifier, got {name!r}")
      if any(o["name"] == name for o in out):
         raise ManifestError(f"{w}.name: {name!r} declared twice")
      label = p.get("label", name)
      capacity = integer(p.get("capacity"), 1, limits["max_bag_slots"], f"{w}.capacity")
      qmax = integer(p.get("quantity_max", DEFAULT_QUANTITY_MAX), 1, MAX_QUANTITY, f"{w}.quantity_max")
      sort = p.get("sort", DEFAULT_SORT)
      if sort not in SORT_RULES:
         raise ManifestError(f"{w}.sort: one of {', '.join(SORT_RULES)}, got {sort!r}")
      out.append({"name": name, "label": enc.text(label, f"{w}.label"), "label_text": label, "capacity": capacity,
                  "quantity_max": qmax, "sort": sort, "sort_id": SORT_RULES.index(sort), "first": first})
      first += capacity
   if first > limits["max_bag_slots"]:
      raise ManifestError(f"{w0}: {first} slots in all, this platform's bag holds {limits['max_bag_slots']} "
                          f"(max_bag_slots)")
   return out


def item_fields(entry: Mapping[str, Any], pockets: Sequence[Mapping[str, Any]], where: str) -> dict[str, Any]:
   """!One item's bag and use fields, defaults filled in (no effect logic checks: check_item_logic()).
   @param entry    the manifest's item
   @param pockets  compile_pockets() result
   @param where    location of the item
   @return {"pocket" (index or NO_POCKET), "pocket_name", "price", "use", "use_id", "target", "target_id",
           "consumed", "holdable", "key", "flags", "save_key" (C6: the asset key "item:NAME", the save image's
           bag entries)}
   @throws ManifestError for an unknown pocket, use or target, a key item that is consumed, or a battler target
           on an item used out of battle
   """
   names = [p["name"] for p in pockets]
   pocket_name = entry.get("pocket")
   if pocket_name is None:
      pocket = NO_POCKET
   elif pocket_name in names:
      pocket = names.index(pocket_name)
   else:
      raise ManifestError(f"{where}.pocket: unknown pocket {pocket_name!r} (pockets: {', '.join(names) or 'none'})")
   use = entry.get("use", DEFAULT_USE)
   if use not in ITEM_USES:
      raise ManifestError(f"{where}.use: one of {', '.join(ITEM_USES)}, got {use!r}")
   target = entry.get("target", DEFAULT_TARGET)
   if target not in ITEM_TARGETS:
      raise ManifestError(f"{where}.target: one of {', '.join(ITEM_TARGETS)}, got {target!r}")
   if target in BATTLE_TARGETS and use != "battle":
      raise ManifestError(f"{where}.target: {target!r} is a battle's target; the item's use must be \"battle\"")
   flags_in = {}
   for word in ("key", "consumed", "holdable", "allow_eggs"):
      v = entry.get(word)
      if v is not None and not isinstance(v, bool):
         raise ManifestError(f"{where}.{word}: true or false, got {v!r}")
      flags_in[word] = v
   key = bool(flags_in["key"])
   consumed = (not key) if flags_in["consumed"] is None else flags_in["consumed"]
   holdable = (not key) if flags_in["holdable"] is None else flags_in["holdable"]
   if key and consumed:
      raise ManifestError(f"{where}: a key item is never consumed (consumed must be false)")
   price = integer(entry.get("price", 0), 0, MAX_PRICE, f"{where}.price")
   flags = ((ITEM_FLAG_BITS["consumed"] if consumed else 0) | (ITEM_FLAG_BITS["holdable"] if holdable else 0) |
            (ITEM_FLAG_BITS["key"] if key else 0) | (ITEM_FLAG_BITS["allow_eggs"] if flags_in["allow_eggs"] else 0))
   return {"pocket": pocket, "pocket_name": pocket_name, "price": price, "use": use, "use_id": ITEM_USES.index(use),
           "target": target, "target_id": ITEM_TARGETS.index(target), "consumed": consumed, "holdable": holdable,
           "key": key, "allow_eggs": bool(flags_in["allow_eggs"]), "flags": flags,
           "save_key": asset_key(f"{ITEM_KEY_NAMESPACE}:{entry.get('name')}", where)}


def item_hooks(item: Mapping[str, Any], bank: Any) -> set[str]:
   """!The hooks an item's effect logic has handlers for.
   @param item  a compiled item (compile_holders(); "owner" set by compile_logic())
   @param bank  the compiled effect bank (effect_compiler.Bank)
   @return hook names
   """
   owner = item.get("owner")
   if owner is None or owner >= len(bank.owners):
      return set()
   mask = bank.owners[owner][0]
   return {h["name"] for i, h in enumerate(ec.HOOKS) if mask >> i & 1}


def check_item_logic(fields: Mapping[str, Any], hooks: set[str], where: str) -> None:
   """!An item's use against its logic: a field use needs a use_field handler, a battle use a use_battle one, and
   a handler of a use the item does not have is refused (it would never run).
   @param fields  item_fields() result
   @param hooks   item_hooks() result
   @param where   location of the item
   @return None
   @throws ManifestError when they disagree
   """
   uses = {"field"} if fields["use"] == "field" else {"battle"} if fields["use"] == "battle" else \
      {"field", "battle"} if fields["use"] == "both" else set()
   for use, hook in USE_HOOKS.items():
      if use in uses and hook not in hooks:
         raise ManifestError(f"{where}: use {fields['use']!r} needs an `on {hook}` handler in its effect logic")
      if use not in uses and hook in hooks:
         raise ManifestError(f"{where}: its effect logic has `on {hook}` but its use is {fields['use']!r} (the "
                             f"handler would never run)")


def compile_shops(src: Json, item_names: Sequence[str], fields: Sequence[Mapping[str, Any] | None],
                  where: str, enc: Texts) -> list[dict[str, Any]]:
   """!The shop lists (battle.json "shops"): what the script primitive `shop` offers, at the items' prices.
   @param src         the manifest's "shops" (None: none)
   @param item_names  the items' names (index 0 = the first item; item ids are 1 + index)
   @param fields      item_fields() per item id (index 0 None)
   @param where       location (battle.json)
   @param enc         this compile's text encoders
   @return [{"name", "label" (codes), "items": [item ids]}]
   @throws ManifestError for an unknown item, an item twice, one without a price or a pocket
   """
   src = [] if src is None else src
   w0 = f"{where}: shops"
   if not isinstance(src, list) or len(src) > MAX_SHOPS:
      raise ManifestError(f"{w0}: a list of at most {MAX_SHOPS} shops")
   out: list[dict[str, Any]] = []
   for i, sh in enumerate(src):
      w = f"{w0}[{i}]"
      check_keys(sh, "shop", w)
      name = sh.get("name")
      if not isinstance(name, str) or not name.strip():
         raise ManifestError(f"{w}.name: needs a name")
      if any(o["name"] == name for o in out):
         raise ManifestError(f"{w}.name: {name!r} declared twice")
      items = sh.get("items", [])
      if not isinstance(items, list) or len(items) > MAX_SHOP_ITEMS:
         raise ManifestError(f"{w}.items: a list of at most {MAX_SHOP_ITEMS} item names")
      ids: list[int] = []
      for j, it in enumerate(items):
         wj = f"{w}.items[{j}]"
         if it not in item_names:
            raise ManifestError(f"{wj}: unknown item {it!r}")
         iid = item_names.index(it) + 1
         if iid in ids:
            raise ManifestError(f"{wj}: {it!r} listed twice")
         f = fields[iid]
         if f is None or f["price"] == 0:
            raise ManifestError(f"{wj}: {it!r} has no price")
         if f["pocket"] == NO_POCKET:
            raise ManifestError(f"{wj}: {it!r} has no pocket (it cannot go into the bag)")
         ids.append(iid)
      out.append({"name": name, "label": enc.text(name, f"{w}.name"), "items": ids})
   return out


def compile_inventory(b: Mapping[str, Any], item_entries: Sequence[Json], items: Sequence[Mapping[str, Any] | None],
                      bank: Any, limits: Mapping[str, int], swhere: str, where: str, enc: Texts) -> dict[str, Any]:
   """!The whole inventory section: pockets, the items' fields (checked against their logic) and shops.
   @param b             the merged battle manifest
   @param item_entries  the manifest's items
   @param items         compiled items (index 0 None; compile_holders() + compile_logic()'s owner)
   @param bank          the compiled effect bank
   @param limits        the platform's limits
   @param swhere        location of the structure manifest
   @param where         location of battle.json
   @param enc           this compile's text encoders
   @return {"pockets", "items" (item_fields() per id, index 0 None), "shops", "slot_count"}
   """
   pockets = compile_pockets(b.get("pockets"), limits, swhere, enc)
   fields: list[dict[str, Any] | None] = [None]
   for i, entry in enumerate(item_entries):
      w = f"{where}: items[{i}] ({entry.get('name')})"
      f = item_fields(entry, pockets, w)
      check_item_logic(f, item_hooks(items[i + 1], bank), w)
      fields.append(f)
   keys: dict[int, str] = {}
   for entry, f in zip(item_entries, fields[1:]):
      if f["save_key"] == NO_SAVE_KEY:
         # C7: key 0 is the engine's "no item" (capsule::inventory::item_of_key()): its entries would never load
         raise ManifestError(f"{where}: item {entry.get('name')!r} has save key 0 (the engine's no item); rename it")
      other = keys.setdefault(f["save_key"], entry.get("name"))
      if other != entry.get("name"):
         raise ManifestError(f"{where}: items {other!r} and {entry.get('name')!r} have one save key "
                             f"(0x{f['save_key']:08X}); rename one")
   shops = compile_shops(b.get("shops"), [e["name"] for e in item_entries], fields, where, enc)
   return {"pockets": pockets, "items": fields, "shops": shops, "slot_count": sum(p["capacity"] for p in pockets)}


def check_held_items(parties: Sequence[Json], item_names: Sequence[str], fields: Sequence[Mapping[str, Any] | None],
                     where: str) -> None:
   """!Party members hold only holdable items.
   @param parties     the manifest's parties
   @param item_names  the items' names
   @param fields      item_fields() per item id (index 0 None)
   @param where       location (battle.json)
   @return None
   @throws ManifestError for a member holding an item that is not holdable
   """
   for i, p in enumerate(parties):
      for j, m in enumerate(p.get("members", []) if isinstance(p, dict) else []):
         it = m.get("item") if isinstance(m, dict) else None
         if it in item_names and not fields[item_names.index(it) + 1]["holdable"]:
            raise ManifestError(f"{where}: parties[{i}].members[{j}].item: {it!r} is not holdable")


def emit_inventory(inv: Mapping[str, Any]) -> list[str]:
   """!C++ of capsule::inventory::kInventory (include/capsule/inventory/data.h).
   @param inv  compile_inventory() result
   @return lines
   """
   L = ["namespace capsule::inventory {", "", "namespace {", ""]
   for i, p in enumerate(inv["pockets"]):
      L.append(f"const text::Code kPocketLabel{i}[] = {{{', '.join(f'0x{x:02X}' for x in p['label'])}}};")
   for i, s in enumerate(inv["shops"]):
      L.append(f"const text::Code kShopName{i}[] = {{{', '.join(f'0x{x:02X}' for x in s['label'])}}};")
   # PocketData {label, first slot, capacity, quantity max, sort rule, reserved}
   L.append("const PocketData kPockets[] = {")
   L += [f"    {{kPocketLabel{i}, {p['first']}, {p['capacity']}, {p['quantity_max']}, "
         f"static_cast<SortRule>({p['sort_id']}), 0}},   // {p['name']}" for i, p in enumerate(inv["pockets"])]
   if not inv["pockets"]:
      L.append("    {nullptr, 0, 0, 0, SortRule::Added, 0},")
   L.append("};")
   # ItemInfo {price, pocket, use, target, flags}; [0] = no item
   L.append("const ItemInfo kItemInfo[] = {")
   L.append(f"    {{0, kNoPocket, Use::None, Target::None, 0}},")
   for f in inv["items"][1:]:
      L.append(f"    {{{f['price']}u, {f['pocket']}, static_cast<Use>({f['use_id']}), "
               f"static_cast<Target>({f['target_id']}), {f['flags']}}},")
   L.append("};")
   # C6: the items' stable keys (the save image's bag entries), by item id; [0] = no item
   keys = [0] + [f["save_key"] for f in inv["items"][1:]]
   L.append(f"const uint32_t kItemKeys[] = {{{', '.join(f'0x{k:08X}u' for k in keys)}}};")
   flat = [iid for s in inv["shops"] for iid in s["items"]]
   L.append(f"const uint16_t kShopItems[] = {{{', '.join(str(x) for x in flat) or '0'}}};")
   L.append("const ShopData kShops[] = {")
   first = 0
   for i, s in enumerate(inv["shops"]):
      L.append(f"    {{kShopName{i}, {first}, {len(s['items'])}, 0}},   // {s['name']}")
      first += len(s["items"])
   if not inv["shops"]:
      L.append("    {nullptr, 0, 0, 0},")
   L += ["};", "", "} // namespace", "",
         f"static_assert({len(inv['pockets'])} <= kMaxPockets, \"more pockets than this platform's bag holds\");",
         f"static_assert({inv['slot_count']} <= kMaxBagSlots, \"more bag slots than this platform's bag holds\");",
         f"const InventoryData kInventory = {{kPockets, kItemInfo, kShops, kShopItems, {len(inv['items'])}, "
         f"{inv['slot_count']}, {len(inv['pockets'])}, {len(inv['shops'])}, {{0, 0}}, kItemKeys}};",
         "", "} // namespace capsule::inventory", ""]
   return L


def inventory_ids(inv: Mapping[str, Any], src_name: str) -> list[str]:
   """!battle_ids.h constants of the pockets and shops (POCKET_<NAME>, SHOP_<NAME>).
   @param inv       compile_inventory() result
   @param src_name  manifest name (messages)
   @return lines
   @throws ManifestError when two names give one identifier
   """
   L = []
   for prefix, plural, names in ((POCKET_PREFIX, "POCKETS", [p["name"] for p in inv["pockets"]]),
                                 (SHOP_PREFIX, "SHOPS", [s["name"] for s in inv["shops"]])):
      seen = set()
      for i, name in enumerate(names):
         idn = f"{prefix}_{ident(name)}"
         if idn in seen:
            raise ManifestError(f"{src_name}: two {prefix.lower()} names give the identifier {idn}")
         seen.add(idn)
         L.append(f"inline constexpr uint8_t {idn} = {i};")
      L.append(f"inline constexpr int NUM_{plural} = {len(names)};")
      L.append("")
   return L
