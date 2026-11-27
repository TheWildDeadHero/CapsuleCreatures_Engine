/// @file
/// D-36 §3.1 G1: the pockets' rules and stores (capsule/inventory/bag.h) on the inventory-only fixture
/// tests/data/bag_rules (tools/inventory_compiler.py): an item filling several slots, emptied slots kept in place,
/// taking from a start slot, compact(), move(), one slot per item, a sorted pocket of many stacks, a store any item
/// goes into, valid(); and the save image's Bag version 2 keeping every slot in place (kept empty slots, repeated
/// items) on the engine's RAM medium.
#include "capsule/inventory/bag.h"
#include "capsule/save/save.h"

#include "bag_rules_ids.h"

#include <cstdio>

namespace
{

using namespace capsule;
using namespace capsule::inventory;

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.

/// Records one check.
/// @param cond  condition that must hold
#define CHECK(cond)                                                                                                    \
   do                                                                                                                  \
   {                                                                                                                   \
      ++g_checks;                                                                                                      \
      if (!(cond))                                                                                                     \
      {                                                                                                                \
         ++g_failures;                                                                                                 \
         std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)

/// @name The fixture's items (tests/data/bag_rules/inventory.json, ids in its order)
///@{
constexpr uint16_t kPebble = 1;   ///< loose.
constexpr uint16_t kShell = 2;    ///< loose.
constexpr uint16_t kSeed = 3;     ///< single.
constexpr uint16_t kTwig = 4;     ///< single.
constexpr uint16_t kAcorn = 5;    ///< sorted.
constexpr uint16_t kBead = 6;     ///< sorted.
constexpr uint16_t kCharm = 7;    ///< plain (a key item).
constexpr uint16_t kNote = 8;     ///< no pocket.
///@}
/// @name The fixture's pockets' numbers
///@{
constexpr uint32_t kStack = 9;          ///< loose, single and sorted: quantity_max.
constexpr int kLooseSlots = 5;          ///< loose: capacity.
constexpr uint32_t kChestStack = 99;    ///< chest: quantity_max.
///@}

Bag g_bag;   ///< The bag under test.

/// One slot of a pocket.
/// @param pocket  pocket index
/// @param k       slot within it
/// @return the slot (an empty one past the pocket's span)
BagSlot slot(uint8_t pocket, int k)
{
   int n = 0;
   const BagSlot* s = pocket_slots(g_bag, pocket, n);
   return s != nullptr && k < n ? s[k] : BagSlot{0, 0};
}

/// Whether a slot holds an item and count.
/// @param pocket  pocket index
/// @param k       slot within it
/// @param item    item id (0: empty)
/// @param n       count
/// @return true if it does
bool holds(uint8_t pocket, int k, uint16_t item, uint16_t n)
{
   const BagSlot s = slot(pocket, k);
   return s.item == item && s.count == n;
}

/// Many stacks in a pocket keeping empty slots: filling, taking from a start slot, holes, compact(), move().
void test_loose()
{
   clear(g_bag);
   int span = 0;
   pocket_slots(g_bag, POCKET_LOOSE, span);
   CHECK(span == kLooseSlots);   // a pocket keeping empty slots spans its capacity
   CHECK(room_for(g_bag, kPebble) == kLooseSlots * kStack);
   // 12 Pebbles: a full slot, then the rest in the next free one
   CHECK(add(g_bag, kPebble, 12) && holds(POCKET_LOOSE, 0, kPebble, 9) && holds(POCKET_LOOSE, 1, kPebble, 3));
   CHECK(add(g_bag, kShell, 2) && holds(POCKET_LOOSE, 2, kShell, 2));
   // 7 more: Pebble's second slot fills to 9, the last one goes to a new slot after the Shells
   CHECK(add(g_bag, kPebble, 7) && holds(POCKET_LOOSE, 1, kPebble, 9) && holds(POCKET_LOOSE, 3, kPebble, 1));
   CHECK(count(g_bag, kPebble) == 19 && room_for(g_bag, kPebble) == 8 + kStack);
   // all or nothing: 18 do not fit (17 do)
   CHECK(!add(g_bag, kPebble, 18) && count(g_bag, kPebble) == 19);
   // taking 10 from slot 1 (a cursor): slot 1 empties and stays, then slot 0 gives one
   CHECK(remove_from(g_bag, POCKET_LOOSE, kPebble, 10, 1));
   CHECK(holds(POCKET_LOOSE, 0, kPebble, 8) && holds(POCKET_LOOSE, 1, 0, 0) && holds(POCKET_LOOSE, 2, kShell, 2));
   CHECK(count(g_bag, kPebble) == 9 && valid(g_bag));
   // a new item takes the first free slot: the hole
   CHECK(add(g_bag, kShell, 9) && holds(POCKET_LOOSE, 2, kShell, 9) && holds(POCKET_LOOSE, 1, kShell, 2));
   // more than the pocket holds: refused, nothing taken
   CHECK(!remove(g_bag, kShell, 12) && count(g_bag, kShell) == 11);
   CHECK(remove(g_bag, kShell, 11) && holds(POCKET_LOOSE, 1, 0, 0) && holds(POCKET_LOOSE, 2, 0, 0));
   // compact: the filled slots to the front, in order
   compact(g_bag, POCKET_LOOSE);
   CHECK(holds(POCKET_LOOSE, 0, kPebble, 8) && holds(POCKET_LOOSE, 1, kPebble, 1) && holds(POCKET_LOOSE, 2, 0, 0));
   // move: slot 1 to the front, slot 0 moves over
   CHECK(add(g_bag, kShell, 4) && move(g_bag, POCKET_LOOSE, 2, 0));
   CHECK(holds(POCKET_LOOSE, 0, kShell, 4) && holds(POCKET_LOOSE, 1, kPebble, 8) && holds(POCKET_LOOSE, 2, kPebble, 1));
   CHECK(!move(g_bag, POCKET_LOOSE, 0, kLooseSlots) && valid(g_bag));
}

/// One slot per item in a pocket keeping empty slots; a sorted pocket of many stacks that closes up; the engine's
/// first rules.
void test_single_sorted_plain()
{
   clear(g_bag);
   CHECK(add(g_bag, kSeed, kStack) && !add(g_bag, kSeed, 1));   // one slot per item: no second stack
   CHECK(room_for(g_bag, kSeed) == 0 && room_for(g_bag, kTwig) == kStack);
   CHECK(add(g_bag, kTwig, 1) && holds(POCKET_SINGLE, 1, kTwig, 1));
   CHECK(remove(g_bag, kSeed, kStack) && holds(POCKET_SINGLE, 0, 0, 0) && holds(POCKET_SINGLE, 1, kTwig, 1));
   // sorted by index, many stacks, closing up: Bead before nothing, Acorn's stacks before it
   CHECK(add(g_bag, kBead, 2) && add(g_bag, kAcorn, 11));
   CHECK(holds(POCKET_SORTED, 0, kAcorn, 9) && holds(POCKET_SORTED, 1, kAcorn, 2) && holds(POCKET_SORTED, 2, kBead, 2));
   CHECK(remove(g_bag, kAcorn, 9) && holds(POCKET_SORTED, 0, kAcorn, 2) && holds(POCKET_SORTED, 1, kBead, 2));
   int span = 0;
   pocket_slots(g_bag, POCKET_SORTED, span);
   CHECK(span == 2);   // a pocket that closes up spans its filled slots
   // the engine's first rules: one slot, closes up; an item with no pocket goes nowhere
   CHECK(add(g_bag, kCharm, 3) && !add(g_bag, kCharm, 1) && !add(g_bag, kNote, 1));
   CHECK(valid(g_bag));
}

/// A store: any item, by add_to() / remove_from(); never an item's own pocket.
void test_store()
{
   clear(g_bag);
   CHECK(accepts(POCKET_CHEST, kNote) && accepts(POCKET_CHEST, kCharm) && !accepts(POCKET_LOOSE, kSeed));
   CHECK(item_pocket(kPebble) == POCKET_LOOSE && item_pocket(kNote) == kNoPocket);
   CHECK(add_to(g_bag, POCKET_CHEST, kNote, 120) && holds(POCKET_CHEST, 0, kNote, 99) && holds(POCKET_CHEST, 1, kNote, 21));
   CHECK(add_to(g_bag, POCKET_CHEST, kPebble, 5) && count(g_bag, kPebble) == 0);   // the store's are not the bag's
   CHECK(count_in(g_bag, POCKET_CHEST, kPebble) == 5 && room_in(g_bag, POCKET_CHEST, kSeed) == kChestStack);
   CHECK(remove_from(g_bag, POCKET_CHEST, kNote, 21, 1) && holds(POCKET_CHEST, 1, 0, 0));
   CHECK(valid(g_bag));
}

/// valid() refuses what the rules do not allow.
void test_valid()
{
   clear(g_bag);
   CHECK(add(g_bag, kSeed, 1) && valid(g_bag));
   g_bag.slots[kInventory.pockets[POCKET_SINGLE].first + 2] = BagSlot{kSeed, 1};   // a second slot of one item
   CHECK(!valid(g_bag));
   clear(g_bag);
   g_bag.slots[kInventory.pockets[POCKET_LOOSE].first] = BagSlot{kSeed, 1};        // another pocket's item
   CHECK(!valid(g_bag));
}

/// Bag version 2: every slot back in place, kept empty slots and repeated items included.
void test_save()
{
   using save::Status;
   CHECK(save::section_size(save::Section::Bag) ==
         save::kBagHeadBytes + kInventory.pocket_count * save::kBagPocketBytes + kInventory.slot_count *
                                                                                 save::kBagEntryBytes);
   clear(g_bag);
   CHECK(add(g_bag, kPebble, 12) && add(g_bag, kShell, 1) && remove_from(g_bag, POCKET_LOOSE, kPebble, 9, 0));
   CHECK(add(g_bag, kTwig, 4) && add(g_bag, kAcorn, 10) && add(g_bag, kCharm, 1));
   CHECK(add_to(g_bag, POCKET_CHEST, kNote, 100));
   const Bag saved = g_bag;
   save::Sources src{&g_bag, nullptr, nullptr, nullptr};
   CHECK(save::write(src) == Status::Ok);
   clear(g_bag);
   save::Notes notes{};
   CHECK(save::read(src, &notes) == Status::Ok);
   bool same = true;
   for (int k = 0; k < kInventory.slot_count; ++k)
   {
      same = same && g_bag.slots[k].item == saved.slots[k].item && g_bag.slots[k].count == saved.slots[k].count;
   }
   CHECK(same && holds(POCKET_LOOSE, 0, 0, 0) && holds(POCKET_LOOSE, 1, kPebble, 3));
   CHECK(notes.dropped_items == 0 && notes.clamped_items == 0 && valid(g_bag));
}

} // namespace

int main()
{
   test_loose();
   test_single_sorted_plain();
   test_store();
   test_valid();
   test_save();
   std::printf("bag rules: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
