/// @file
/// The bag: pockets laid out by kInventory over fixed storage (see capsule/inventory/bag.h).
#include "capsule/inventory/bag.h"

#include "capsule/battle/data.h"

namespace capsule::inventory
{

namespace
{

/// A pocket's table entry.
/// @param pocket  pocket index
/// @return the entry, or nullptr for a pocket that does not exist
const PocketData* pocket_at(uint8_t pocket)
{
   return pocket < kInventory.pocket_count ? &kInventory.pockets[pocket] : nullptr;
}

/// The pocket an item goes into.
/// @param item  item id
/// @return pocket index, or kNoPocket
uint8_t pocket_of(uint16_t item)
{
   const ItemInfo* i = info(item);
   return i != nullptr && i->pocket < kInventory.pocket_count ? i->pocket : kNoPocket;
}

/// The slot of an item in its pocket.
/// @param bag   the bag
/// @param p       its pocket
/// @param pocket  the pocket's index
/// @param item    item id
/// @return slot index in Bag::slots, or -1 when the pocket does not hold it
int find_slot(const Bag& bag, const PocketData& p, uint8_t pocket, uint16_t item)
{
   for (int k = 0; k < bag.used[pocket]; ++k)
   {
      if (bag.slots[p.first + k].item == item)
      {
         return p.first + k;
      }
   }
   return -1;
}

/// An item's display name (the battle's item table).
/// @param item  item id
/// @return its codes, or nullptr
const text::Code* name_of(uint16_t item)
{
   return item < battle::kData.item_count ? battle::kData.items[item].name : nullptr;
}

/// Display names compared code by code (a shorter name that is a prefix comes first).
/// @param a  one name (nullptr sorts first)
/// @param b  the other
/// @return true if @p a comes before @p b
bool name_before(const text::Code* a, const text::Code* b)
{
   if (a == nullptr || b == nullptr)
   {
      return a == nullptr && b != nullptr;
   }
   int k = 0;
   while (a[k] != text::kCodeEnd && a[k] == b[k])
   {
      ++k;
   }
   return a[k] != b[k] && (a[k] == text::kCodeEnd || (b[k] != text::kCodeEnd && a[k] < b[k]));
}

/// Whether one slot's item goes before another's under a rule.
/// @param rule  the order (not Added)
/// @param a     one item
/// @param b     the other
/// @return true if @p a comes first
bool before(SortRule rule, uint16_t a, uint16_t b)
{
   if (rule == SortRule::Name)
   {
      const text::Code* na = name_of(a);
      const text::Code* nb = name_of(b);
      // equal names fall back to the id, so the order is total
      return name_before(na, nb) || (!name_before(nb, na) && a < b);
   }
   return a < b;
}

/// Where a new item goes in its pocket: the end, or its place under the pocket's rule.
/// @param bag     the bag
/// @param p       the pocket
/// @param pocket  its index
/// @param item    item id
/// @return slot offset within the pocket
int insert_at(const Bag& bag, const PocketData& p, uint8_t pocket, uint16_t item)
{
   const int used = bag.used[pocket];
   if (p.sort == SortRule::Added)
   {
      return used;
   }
   int k = 0;
   while (k < used && !before(p.sort, item, bag.slots[p.first + k].item))
   {
      ++k;
   }
   return k;
}

} // namespace

uint16_t item_of_key(uint32_t key)
{
   if (key == 0 || kInventory.item_keys == nullptr)
   {
      return 0;
   }
   for (uint16_t k = 1; k < kInventory.item_count; ++k)
   {
      if (kInventory.item_keys[k] == key)
      {
         return k;
      }
   }
   return 0;
}

void clear(Bag& bag)
{
   for (BagSlot& s : bag.slots)
   {
      s = BagSlot{0, 0};
   }
   for (uint16_t& u : bag.used)
   {
      u = 0;
   }
}

uint32_t count(const Bag& bag, uint16_t item)
{
   const uint8_t pocket = pocket_of(item);
   if (pocket == kNoPocket)
   {
      return 0;
   }
   const int at = find_slot(bag, kInventory.pockets[pocket], pocket, item);
   return at < 0 ? 0 : bag.slots[at].count;
}

uint32_t room_for(const Bag& bag, uint16_t item)
{
   const uint8_t pocket = pocket_of(item);
   if (pocket == kNoPocket)
   {
      return 0;
   }
   const PocketData& p = kInventory.pockets[pocket];
   const int at = find_slot(bag, p, pocket, item);
   if (at >= 0)
   {
      // C6: a slot over the pocket's quantity_max (a bag of another build) has no room, not 65535 minus a little
      const uint16_t held = bag.slots[at].count;
      return held < p.quantity_max ? static_cast<uint32_t>(p.quantity_max - held) : 0u;
   }
   return bag.used[pocket] < p.capacity ? p.quantity_max : 0;
}

bool add(Bag& bag, uint16_t item, uint32_t n)
{
   if (n == 0 || n > room_for(bag, item))
   {
      return false;
   }
   const uint8_t pocket = pocket_of(item);
   const PocketData& p = kInventory.pockets[pocket];
   const int at = find_slot(bag, p, pocket, item);
   if (at >= 0)
   {
      bag.slots[at].count = static_cast<uint16_t>(bag.slots[at].count + n);
      return true;
   }
   // a new slot: the slots after its place move up one
   const int k = insert_at(bag, p, pocket, item);
   for (int j = bag.used[pocket]; j > k; --j)
   {
      bag.slots[p.first + j] = bag.slots[p.first + j - 1];
   }
   bag.slots[p.first + k] = BagSlot{item, static_cast<uint16_t>(n)};
   ++bag.used[pocket];
   return true;
}

bool remove(Bag& bag, uint16_t item, uint32_t n)
{
   const uint8_t pocket = pocket_of(item);
   if (n == 0 || pocket == kNoPocket)
   {
      return false;
   }
   const PocketData& p = kInventory.pockets[pocket];
   const int at = find_slot(bag, p, pocket, item);
   if (at < 0 || bag.slots[at].count < n)
   {
      return false;
   }
   bag.slots[at].count = static_cast<uint16_t>(bag.slots[at].count - n);
   if (bag.slots[at].count == 0)
   {
      // the pocket closes up: the slots after it move down one
      const int last = p.first + bag.used[pocket] - 1;
      for (int j = at; j < last; ++j)
      {
         bag.slots[j] = bag.slots[j + 1];
      }
      bag.slots[last] = BagSlot{0, 0};
      --bag.used[pocket];
   }
   return true;
}

void sort(Bag& bag, uint8_t pocket, SortRule rule)
{
   const PocketData* p = pocket_at(pocket);
   if (p == nullptr || rule == SortRule::Added)
   {
      return;
   }
   // insertion sort: a pocket holds at most kMaxBagSlots slots, usually a few dozen, and the order is stable
   BagSlot* s = &bag.slots[p->first];
   for (int i = 1; i < bag.used[pocket]; ++i)
   {
      const BagSlot v = s[i];
      int j = i;
      while (j > 0 && before(rule, v.item, s[j - 1].item))
      {
         s[j] = s[j - 1];
         --j;
      }
      s[j] = v;
   }
}

void sort_all(Bag& bag)
{
   for (uint8_t k = 0; k < kInventory.pocket_count; ++k)
   {
      sort(bag, k, kInventory.pockets[k].sort);
   }
}

const BagSlot* pocket_slots(const Bag& bag, uint8_t pocket, int& used)
{
   const PocketData* p = pocket_at(pocket);
   used = p != nullptr ? bag.used[pocket] : 0;
   return p != nullptr ? &bag.slots[p->first] : nullptr;
}

bool valid(const Bag& bag)
{
   for (uint8_t k = 0; k < kInventory.pocket_count; ++k)
   {
      const PocketData& p = kInventory.pockets[k];
      if (bag.used[k] > p.capacity)
      {
         return false;
      }
      for (int j = 0; j < p.capacity; ++j)
      {
         const BagSlot& s = bag.slots[p.first + j];
         const bool filled = j < bag.used[k];
         // a filled slot: an item of this pocket, 1 to quantity_max of it; an unfilled one is empty
         if (filled ? (pocket_of(s.item) != k || s.count == 0 || s.count > p.quantity_max) : s.item != 0)
         {
            return false;
         }
      }
   }
   return true;
}

} // namespace capsule::inventory
