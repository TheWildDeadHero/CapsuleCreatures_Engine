/// @file
/// The bag: pockets laid out by kInventory over fixed storage, with each pocket's rules (see capsule/inventory/bag.h).
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

/// Whether an item may fill several slots of a pocket.
/// @param p  the pocket
/// @return true with kPocketManyStacks
bool many_stacks(const PocketData& p)
{
   return (p.flags & kPocketManyStacks) != 0;
}

/// Whether a pocket keeps its emptied slots in place.
/// @param p  the pocket
/// @return true with kPocketKeepsEmpty
bool keeps_empty(const PocketData& p)
{
   return (p.flags & kPocketKeepsEmpty) != 0;
}

/// The slots a pocket's functions walk: its filled ones when it closes up, its whole capacity when it keeps empty
/// slots.
/// @param bag     the bag
/// @param p       the pocket
/// @param pocket  its index
/// @return the slots' number
int span(const Bag& bag, const PocketData& p, uint8_t pocket)
{
   return keeps_empty(p) ? p.capacity : bag.used[pocket];
}

/// The first slot of a pocket holding an item.
/// @param bag     the bag
/// @param p       the pocket
/// @param pocket  its index
/// @param item    item id
/// @return slot index in Bag::slots, or -1 when the pocket does not hold it
int find_slot(const Bag& bag, const PocketData& p, uint8_t pocket, uint16_t item)
{
   const int n = span(bag, p, pocket);
   for (int k = 0; k < n; ++k)
   {
      if (bag.slots[p.first + k].item == item)
      {
         return p.first + k;
      }
   }
   return -1;
}

/// The free slots of a pocket.
/// @param bag     the bag
/// @param p       the pocket
/// @param pocket  its index
/// @return how many: the empty slots of a pocket keeping them, the unfilled ones of one that closes up
int free_slots(const Bag& bag, const PocketData& p, uint8_t pocket)
{
   if (!keeps_empty(p))
   {
      return p.capacity - bag.used[pocket];
   }
   int n = 0;
   for (int k = 0; k < p.capacity; ++k)
   {
      n += bag.slots[p.first + k].item == 0 ? 1 : 0;
   }
   return n;
}

/// The room left in one slot (a slot over the pocket's quantity_max, from a bag of another build, has none).
/// @param p     the pocket
/// @param slot  the slot
/// @return how many more it holds
uint32_t slot_room(const PocketData& p, const BagSlot& slot)
{
   return slot.count < p.quantity_max ? static_cast<uint32_t>(p.quantity_max - slot.count) : 0u;
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

/// Where a new slot goes in a pocket that closes up: the end, or its place under the pocket's rule.
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

/// Puts a new slot of an item into a pocket that has a free one: the first empty slot of a pocket keeping them, or
/// the item's place in one that closes up (the slots after it move up one).
/// @param bag     the bag
/// @param p       the pocket
/// @param pocket  its index
/// @param item    item id
/// @param n       how many (at most quantity_max)
void new_slot(Bag& bag, const PocketData& p, uint8_t pocket, uint16_t item, uint32_t n)
{
   const BagSlot slot = {item, static_cast<uint16_t>(n)};
   if (keeps_empty(p))
   {
      for (int k = 0; k < p.capacity; ++k)
      {
         if (bag.slots[p.first + k].item == 0)
         {
            bag.slots[p.first + k] = slot;
            return;
         }
      }
      return;
   }
   const int k = insert_at(bag, p, pocket, item);
   for (int j = bag.used[pocket]; j > k; --j)
   {
      bag.slots[p.first + j] = bag.slots[p.first + j - 1];
   }
   bag.slots[p.first + k] = slot;
   ++bag.used[pocket];
}

/// Packs a pocket's filled slots at its start, in order, and empties the rest of its span.
/// @param bag     the bag
/// @param p       the pocket
/// @param pocket  its index
/// @return the filled slots
int pack(Bag& bag, const PocketData& p, uint8_t pocket)
{
   const int n = span(bag, p, pocket);
   int w = 0;
   for (int k = 0; k < n; ++k)
   {
      const BagSlot s = bag.slots[p.first + k];
      if (s.item != 0 && s.count != 0)
      {
         bag.slots[p.first + w] = s;
         ++w;
      }
   }
   for (int k = w; k < n; ++k)
   {
      bag.slots[p.first + k] = BagSlot{0, 0};
   }
   if (!keeps_empty(p))
   {
      bag.used[pocket] = static_cast<uint16_t>(w);
   }
   return w;
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

uint8_t item_pocket(uint16_t item)
{
   const ItemInfo* i = info(item);
   return i != nullptr && i->pocket < kInventory.pocket_count ? i->pocket : kNoPocket;
}

bool accepts(uint8_t pocket, uint16_t item)
{
   const PocketData* p = pocket_at(pocket);
   if (p == nullptr || info(item) == nullptr)
   {
      return false;
   }
   return (p->flags & kPocketStore) != 0 || item_pocket(item) == pocket;
}

uint32_t count(const Bag& bag, uint16_t item)
{
   return count_in(bag, item_pocket(item), item);
}

uint32_t count_in(const Bag& bag, uint8_t pocket, uint16_t item)
{
   if (!accepts(pocket, item))
   {
      return 0;
   }
   const PocketData& p = kInventory.pockets[pocket];
   const int n = span(bag, p, pocket);
   uint32_t total = 0;
   for (int k = 0; k < n; ++k)
   {
      const BagSlot& s = bag.slots[p.first + k];
      total += s.item == item ? s.count : 0u;
   }
   return total;
}

uint32_t room_for(const Bag& bag, uint16_t item)
{
   return room_in(bag, item_pocket(item), item);
}

uint32_t room_in(const Bag& bag, uint8_t pocket, uint16_t item)
{
   if (!accepts(pocket, item))
   {
      return 0;
   }
   const PocketData& p = kInventory.pockets[pocket];
   const uint32_t free_room = static_cast<uint32_t>(free_slots(bag, p, pocket)) * p.quantity_max;
   if (!many_stacks(p))
   {
      // one slot per item: the room in its slot, or a free slot's worth when it has none yet
      const int at = find_slot(bag, p, pocket, item);
      return at >= 0 ? slot_room(p, bag.slots[at]) : (free_room > 0 ? p.quantity_max : 0u);
   }
   uint32_t room = free_room;
   const int n = span(bag, p, pocket);
   for (int k = 0; k < n; ++k)
   {
      const BagSlot& s = bag.slots[p.first + k];
      room += s.item == item ? slot_room(p, s) : 0u;
   }
   return room;
}

bool add(Bag& bag, uint16_t item, uint32_t n)
{
   return add_to(bag, item_pocket(item), item, n);
}

bool add_to(Bag& bag, uint8_t pocket, uint16_t item, uint32_t n)
{
   if (n == 0 || n > room_in(bag, pocket, item))
   {
      return false;
   }
   const PocketData& p = kInventory.pockets[pocket];
   // the item's slots first, in slot order, up to quantity_max (one slot per item: its only one)
   const int span_n = span(bag, p, pocket);
   for (int k = 0; k < span_n && n > 0; ++k)
   {
      BagSlot& s = bag.slots[p.first + k];
      if (s.item == item)
      {
         const uint32_t put = n < slot_room(p, s) ? n : slot_room(p, s);
         s.count = static_cast<uint16_t>(s.count + put);
         n -= put;
         if (!many_stacks(p))
         {
            break;
         }
      }
   }
   // then new slots for the rest (room_in() counted them: at most one without many stacks)
   while (n > 0)
   {
      const uint32_t put = n < p.quantity_max ? n : p.quantity_max;
      new_slot(bag, p, pocket, item, put);
      n -= put;
   }
   return true;
}

bool remove(Bag& bag, uint16_t item, uint32_t n)
{
   return remove_from(bag, item_pocket(item), item, n);
}

bool remove_from(Bag& bag, uint8_t pocket, uint16_t item, uint32_t n, int start)
{
   if (n == 0 || n > count_in(bag, pocket, item))
   {
      return false;
   }
   const PocketData& p = kInventory.pockets[pocket];
   const int span_n = span(bag, p, pocket);
   // one slot: takes what it can; an emptied slot becomes empty (closed up below in a pocket that closes up)
   auto take = [&](BagSlot& s)
   {
      const uint32_t got = n < s.count ? n : s.count;
      s.count = static_cast<uint16_t>(s.count - got);
      n -= got;
      if (s.count == 0)
      {
         s.item = 0;
      }
   };
   if (start >= 0 && start < span_n && bag.slots[p.first + start].item == item)
   {
      take(bag.slots[p.first + start]);
   }
   for (int k = 0; k < span_n && n > 0; ++k)
   {
      if (bag.slots[p.first + k].item == item)
      {
         take(bag.slots[p.first + k]);
      }
   }
   if (!keeps_empty(p))
   {
      pack(bag, p, pocket);   // the pocket closes up: the slots after an emptied one move down
   }
   return true;
}

void compact(Bag& bag, uint8_t pocket)
{
   const PocketData* p = pocket_at(pocket);
   if (p != nullptr)
   {
      pack(bag, *p, pocket);
   }
}

bool move(Bag& bag, uint8_t pocket, int from, int to)
{
   const PocketData* p = pocket_at(pocket);
   const int n = p != nullptr ? span(bag, *p, pocket) : 0;
   if (from < 0 || from >= n || to < 0 || to >= n)
   {
      return false;
   }
   // the slot is lifted out; the ones between move over by one toward its old place
   BagSlot* s = &bag.slots[p->first];
   const BagSlot moved = s[from];
   const int step = from < to ? 1 : -1;
   for (int k = from; k != to; k += step)
   {
      s[k] = s[k + step];
   }
   s[to] = moved;
   return true;
}

void sort(Bag& bag, uint8_t pocket, SortRule rule)
{
   const PocketData* p = pocket_at(pocket);
   if (p == nullptr || rule == SortRule::Added)
   {
      return;
   }
   const int filled = pack(bag, *p, pocket);
   // insertion sort: a pocket holds at most kMaxBagSlots slots, usually a few dozen, and the order is stable
   BagSlot* s = &bag.slots[p->first];
   for (int i = 1; i < filled; ++i)
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
   used = p != nullptr ? span(bag, *p, pocket) : 0;
   return p != nullptr ? &bag.slots[p->first] : nullptr;
}

bool valid(const Bag& bag)
{
   for (uint8_t k = 0; k < kInventory.pocket_count; ++k)
   {
      const PocketData& p = kInventory.pockets[k];
      if (!keeps_empty(p) && bag.used[k] > p.capacity)
      {
         return false;
      }
      const int filled_end = span(bag, p, k);
      for (int j = 0; j < p.capacity; ++j)
      {
         const BagSlot& s = bag.slots[p.first + j];
         const bool empty = s.item == 0 && s.count == 0;
         if (j >= filled_end || (keeps_empty(p) && empty))
         {
            // past a closed-up pocket's filled slots, or a kept empty slot: nothing in it
            if (!empty)
            {
               return false;
            }
            continue;
         }
         // a filled slot: an item the pocket takes, 1 to quantity_max of it, and its only slot unless many stacks
         if (!accepts(k, s.item) || s.count == 0 || s.count > p.quantity_max)
         {
            return false;
         }
         if (!many_stacks(p) && find_slot(bag, p, k, s.item) != p.first + j)
         {
            return false;
         }
      }
   }
   return true;
}

} // namespace capsule::inventory
