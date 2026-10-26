/// @file
/// The bag (D-26 section 2, package G2): fixed engine-maximum storage per platform (kMaxBagSlots slots, no heap),
/// laid out by the manifest's pockets at run time (kInventory: each pocket a slice of the slots, its capacity, its
/// most of one item and its sort rule). One slot holds one item and its count; a pocket's filled slots are packed
/// at the start of its slice (Bag::used).
///
/// Adding and taking are all or nothing: an item with no pocket, a full pocket, a count past the pocket's
/// quantity_max or more than the bag holds leaves the bag as it was. Pockets sorted by index or name keep their
/// order as items come in; "added" pockets append.
///
/// The bag is plain data a game keeps where it likes (EWRAM on the GBA: 1 KB of slots); the save image stores
/// the manifest's slots (capsule/save/save.h).
#pragma once

#include "capsule/core/pod.h"
#include "capsule/inventory/data.h"

#include <cstdint>

namespace capsule::inventory
{

/// One bag slot.
struct BagSlot
{
   uint16_t item;    ///< Item id (0: empty).
   uint16_t count;   ///< How many (1 to the pocket's quantity_max when used).
};
CAPSULE_POD(BagSlot, 4);

/// The bag: every pocket's slots and how many of each pocket's are filled.
struct Bag
{
   BagSlot slots[kMaxBagSlots];   ///< Pocket p uses slots [first, first + used[p]) of its slice.
   uint16_t used[kMaxPockets];    ///< Filled slots per pocket.
};

/// Empties the bag.
/// @param bag  the bag
void clear(Bag& bag);

/// How many of an item the bag holds.
/// @param bag   the bag
/// @param item  item id
/// @return the count (0 for none or an item with no pocket)
uint32_t count(const Bag& bag, uint16_t item);

/// How many more of an item fit (its pocket's free slot or the room left in its slot).
/// @param bag   the bag
/// @param item  item id
/// @return the count that fits (0: none)
uint32_t room_for(const Bag& bag, uint16_t item);

/// Adds items, all or nothing.
/// @param bag   the bag
/// @param item  item id
/// @param n     how many (1 or more)
/// @return true if all were added
bool add(Bag& bag, uint16_t item, uint32_t n);

/// Takes items, all or nothing (an emptied slot closes up its pocket).
/// @param bag   the bag
/// @param item  item id
/// @param n     how many (1 or more)
/// @return true if the bag held that many (they are gone)
bool remove(Bag& bag, uint16_t item, uint32_t n);

/// Re-orders a pocket by a rule (a player's "sort"; SortRule::Added keeps it).
/// @param bag     the bag
/// @param pocket  pocket index
/// @param rule    the order
void sort(Bag& bag, uint8_t pocket, SortRule rule);

/// Re-orders every pocket by its own rule (after loading a bag written by another order).
/// @param bag  the bag
void sort_all(Bag& bag);

/// A pocket's filled slots.
/// @param bag     the bag
/// @param pocket  pocket index
/// @param used    filled slots (out; 0 for a pocket that does not exist)
/// @return its first slot, or nullptr for a pocket that does not exist
const BagSlot* pocket_slots(const Bag& bag, uint8_t pocket, int& used);

/// Whether a bag's slots fit the build's pockets: counts within capacity and quantity_max, items of that pocket,
/// no empty slot inside a pocket's filled ones (a loaded bag).
/// @param bag  the bag
/// @return true if it is consistent
bool valid(const Bag& bag);

} // namespace capsule::inventory
