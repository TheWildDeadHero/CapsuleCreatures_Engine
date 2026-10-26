/// @file
/// Inventory tables (D-26 section 2, package G2): the bag's pockets, each item's bag and use fields and the shop
/// lists, compiled from the battle manifests by tools/battlec/inventory.py into capsule::inventory::kInventory
/// (next to capsule::battle::kData; src/inventory/inventory_empty.cpp is the empty default of a build without
/// battle data). Pocket names, labels, capacities, sort rules and every item field are manifest data; the engine
/// reads only this table.
///
/// Item ids are the battle's (capsule::battle::kData.items, 0 = no item): kInventory.items has the same
/// item_count entries, entry 0 the empty one.
#pragma once

#include "capsule/core/pod.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace capsule::inventory
{

/// @name Engine maxima per platform (tools/build_settings.py PLATFORM_LIMITS max_pockets / max_bag_slots)
///@{
#if defined(CAPSULE_PLATFORM_GBA)
inline constexpr int kMaxPockets = 8;      ///< Pockets a bag holds (GBA: EWRAM and save space).
inline constexpr int kMaxBagSlots = 256;   ///< Slots of every pocket together (GBA: 1 KB of slots).
#else
inline constexpr int kMaxPockets = 16;     ///< Pockets a bag holds.
inline constexpr int kMaxBagSlots = 1024;  ///< Slots of every pocket together.
#endif
///@}

/// The pocket of an item that has none (it cannot go into the bag).
inline constexpr uint8_t kNoPocket = 0xFF;
/// Most of one item a slot holds (BagSlot::count is 16 bits; a pocket's quantity_max is at most this).
inline constexpr uint16_t kMaxQuantity = 0xFFFF;

/// How a pocket orders its slots (structure pockets[].sort).
enum class SortRule : uint8_t
{
   Added,      ///< In the order the items came in.
   Index,      ///< By item id (the manifest's order).
   Name,       ///< By display name (code by code).
   NUM_RULES   ///< Number of rules.
};

/// Where an item can be used (items[].use).
enum class Use : uint8_t
{
   None,       ///< Nowhere (held, or kept).
   Field,      ///< Out of battle (effect hook use_field).
   Battle,     ///< In battle, as the Item action (effect hook use_battle; G3).
   Both,       ///< Both.
   NUM_USES    ///< Number of uses.
};

/// What an item is used on (items[].target).
enum class Target : uint8_t
{
   None,       ///< Nothing to choose.
   Member,     ///< A party member.
   MoveSlot,   ///< A move slot of a party member (the context move of its logic).
   Battler,    ///< A battler in battle (use "battle" only).
   NUM_TARGETS ///< Number of targets.
};

/// @name ItemInfo::flags
///@{
inline constexpr uint8_t kItemConsumed = 1;   ///< A use that had an effect takes one from the bag.
inline constexpr uint8_t kItemHoldable = 2;   ///< A party member may hold it.
inline constexpr uint8_t kItemKey = 4;        ///< A key item: never consumed or sold.
inline constexpr uint8_t kItemEggs = 8;       ///< It may be used on an egg (B2; default refused).
///@}

/// One pocket: a slice of the bag's slots.
struct PocketData
{
   const text::Code* label;   ///< Display label.
   uint16_t first;            ///< First slot of the pocket in Bag::slots.
   uint16_t capacity;         ///< Slots it has (distinct items it holds).
   uint16_t quantity_max;     ///< Most of one item a slot holds (1 to kMaxQuantity).
   SortRule sort;             ///< How its slots are ordered.
   uint8_t reserved;          ///< 0.
};

/// One item's bag and use fields.
struct ItemInfo
{
   uint32_t price;   ///< Price in a shop (0: no shop sells it).
   uint8_t pocket;   ///< Its pocket, or kNoPocket.
   Use use;          ///< Where it can be used.
   Target target;    ///< What it is used on.
   uint8_t flags;    ///< kItem* bits.
};
CAPSULE_POD(ItemInfo, 8);

/// One shop list (battle.json shops; the script primitive `shop` opens one).
struct ShopData
{
   const text::Code* name;   ///< Display name.
   uint16_t first;           ///< First entry in InventoryData::shop_items.
   uint8_t count;            ///< Entries.
   uint8_t reserved;         ///< 0.
};

/// The game's inventory tables.
struct InventoryData
{
   const PocketData* pockets;     ///< pocket_count pockets.
   const ItemInfo* items;         ///< item_count entries by item id (0: no item).
   const ShopData* shops;         ///< shop_count shops.
   const uint16_t* shop_items;    ///< Item ids of every shop, shop by shop.
   uint16_t item_count;           ///< Entries of items (the battle's item_count).
   uint16_t slot_count;           ///< Slots of every pocket together (<= kMaxBagSlots).
   uint8_t pocket_count;          ///< Pockets (<= kMaxPockets).
   uint8_t shop_count;            ///< Shops.
   uint8_t reserved[kPadAfterTwoBytes];   ///< 0.
   /// item_count stable keys by item id (C6: the asset key "item:NAME", FNV-1a 32; entry 0 is 0): the save image
   /// keeps bag entries by key, so a save outlives items added, removed or reordered. nullptr: no items.
   const uint32_t* item_keys;
};

/// The build's tables (generated with the battle data; the default has no pockets, items or shops).
extern const InventoryData kInventory;

/// An item's fields.
/// @param item  item id
/// @return its entry, or nullptr for 0 or an id past the table
inline const ItemInfo* info(uint16_t item)
{
   return item != 0 && item < kInventory.item_count ? &kInventory.items[item] : nullptr;
}

/// Whether an item has a flag.
/// @param item  item id
/// @param flag  a kItem* bit
/// @return true if it has it (false for no item)
inline bool has_flag(uint16_t item, uint8_t flag)
{
   const ItemInfo* i = info(item);
   return i != nullptr && (i->flags & flag) != 0;
}

/// The item a stable key names (a linear search: the save image's bag entries, a few dozen on loading).
/// @param key  an item key (InventoryData::item_keys)
/// @return the item id, or 0 when no item of this build has it
uint16_t item_of_key(uint32_t key);

/// Whether an item can be used out of battle.
/// @param item  item id
/// @return true for use field or both
inline bool field_usable(uint16_t item)
{
   const ItemInfo* i = info(item);
   return i != nullptr && (i->use == Use::Field || i->use == Use::Both);
}

} // namespace capsule::inventory
