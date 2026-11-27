/// @file
/// The bag (D-26 section 2, package G2): fixed engine-maximum storage per platform (kMaxBagSlots slots, no heap),
/// laid out by the manifest's pockets at run time (kInventory: each pocket a slice of the slots, its capacity, its
/// most of one item, its sort rule and its rules). One slot holds one item and its count.
///
/// **A pocket's rules** (D-36 §3.1 G1, PocketData::flags; all off by default):
///   - one slot per item (default), or **many stacks** (kPocketManyStacks): adding fills the item's slots up to the
///     pocket's quantity_max in slot order, then takes free slots for the rest;
///   - a pocket that **closes up** (default: its filled slots are packed at the start of its slice, Bag::used), or one
///     that **keeps empty slots** (kPocketKeepsEmpty): an emptied slot stays where it is (item 0) until compact(), a
///     new item takes the first free slot, and the pocket's span is its whole capacity;
///   - a **store** (kPocketStore): a container any item may go into (an item's own pocket is never a store); the
///     *_in / *_to / *_from functions address it.
///
/// Adding and taking are all or nothing: an item a pocket does not take, a full pocket, a count past what fits or
/// more than the pocket holds leaves the bag as it was. Pockets sorted by index or name keep their order as items
/// come in; "added" pockets append. Taking starts at a given slot (remove_from(): a menu's cursor), then goes
/// through the pocket in order.
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
   BagSlot slots[kMaxBagSlots];   ///< Pocket p uses slots [first, first + span) of its slice (pocket_slots()).
   uint16_t used[kMaxPockets];    ///< Filled slots per pocket that closes up (a pocket keeping empty slots: unused).
};

/// Empties the bag.
/// @param bag  the bag
void clear(Bag& bag);

/// The pocket an item goes into (its own pocket; never a store).
/// @param item  item id
/// @return pocket index, or kNoPocket
uint8_t item_pocket(uint16_t item);

/// Whether a pocket takes an item: its own pocket, or any store.
/// @param pocket  pocket index
/// @param item    item id
/// @return true if the item may go into it
bool accepts(uint8_t pocket, uint16_t item);

/// How many of an item the bag holds in its own pocket.
/// @param bag   the bag
/// @param item  item id
/// @return the count (0 for none or an item with no pocket)
uint32_t count(const Bag& bag, uint16_t item);

/// How many of an item a pocket holds (every slot of it).
/// @param bag     the bag
/// @param pocket  pocket index
/// @param item    item id
/// @return the count (0 for a pocket that does not take it)
uint32_t count_in(const Bag& bag, uint8_t pocket, uint16_t item);

/// How many more of an item fit in its own pocket.
/// @param bag   the bag
/// @param item  item id
/// @return the count that fits (0: none)
uint32_t room_for(const Bag& bag, uint16_t item);

/// How many more of an item fit in a pocket: the room left in its slots (one, or every one with many stacks) and the
/// free slots at quantity_max each (one free slot when the item has none yet and the pocket keeps one per item).
/// @param bag     the bag
/// @param pocket  pocket index
/// @param item    item id
/// @return the count that fits (0: none, or a pocket that does not take it)
uint32_t room_in(const Bag& bag, uint8_t pocket, uint16_t item);

/// Adds items to their own pocket, all or nothing.
/// @param bag   the bag
/// @param item  item id
/// @param n     how many (1 or more)
/// @return true if all were added
bool add(Bag& bag, uint16_t item, uint32_t n);

/// Adds items to a pocket (a store, or the item's own), all or nothing.
/// @param bag     the bag
/// @param pocket  pocket index
/// @param item    item id
/// @param n       how many (1 or more)
/// @return true if all were added
bool add_to(Bag& bag, uint8_t pocket, uint16_t item, uint32_t n);

/// Takes items from their own pocket, all or nothing, from its first slot on.
/// @param bag   the bag
/// @param item  item id
/// @param n     how many (1 or more)
/// @return true if the bag held that many (they are gone)
bool remove(Bag& bag, uint16_t item, uint32_t n);

/// Takes items from a pocket, all or nothing: from the start slot when it holds the item, then from the pocket's
/// slots in order. An emptied slot closes up its pocket, or stays (item 0) in a pocket keeping empty slots.
/// @param bag     the bag
/// @param pocket  pocket index
/// @param item    item id
/// @param n       how many (1 or more)
/// @param start   the slot (within the pocket) taken from first, e.g. a menu's cursor
/// @return true if the pocket held that many (they are gone)
bool remove_from(Bag& bag, uint8_t pocket, uint16_t item, uint32_t n, int start = 0);

/// Moves a pocket's filled slots to its start, in their order, the empty ones after them (a pocket keeping empty
/// slots; one that closes up is compact already).
/// @param bag     the bag
/// @param pocket  pocket index
void compact(Bag& bag, uint8_t pocket);

/// Moves one slot of a pocket to another place, the slots between moving over by one (a player's reorder).
/// @param bag     the bag
/// @param pocket  pocket index
/// @param from    the slot moved (within the pocket's span)
/// @param to      its new place (within the span)
/// @return false (nothing moved) for a slot outside the span
bool move(Bag& bag, uint8_t pocket, int from, int to);

/// Re-orders a pocket by a rule (a player's "sort"; SortRule::Added keeps it). A pocket keeping empty slots is
/// compacted first.
/// @param bag     the bag
/// @param pocket  pocket index
/// @param rule    the order
void sort(Bag& bag, uint8_t pocket, SortRule rule);

/// Re-orders every pocket by its own rule (after loading a bag written by another order).
/// @param bag  the bag
void sort_all(Bag& bag);

/// A pocket's slots: its filled slots in a pocket that closes up; its whole capacity (filled and empty slots) in one
/// keeping empty slots.
/// @param bag     the bag
/// @param pocket  pocket index
/// @param used    the slots' number (out; 0 for a pocket that does not exist)
/// @return its first slot, or nullptr for a pocket that does not exist
const BagSlot* pocket_slots(const Bag& bag, uint8_t pocket, int& used);

/// Whether a bag's slots fit the build's pockets: counts within capacity and quantity_max, items the pocket takes,
/// one slot per item where the pocket keeps one, no empty slot inside the filled ones of a pocket that closes up (a
/// loaded bag).
/// @param bag  the bag
/// @return true if it is consistent
bool valid(const Bag& bag);

} // namespace capsule::inventory
