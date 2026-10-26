/// @file
/// Storage boxes (see capsule/storage/storage.h): a game's bytes laid out slot by slot at the layout's record size,
/// a used bit per slot, every access checked against the view's capacity.
#include "capsule/storage/storage.h"

namespace capsule::storage
{

namespace
{

/// The used bit of a slot within its byte.
/// @param slot  slot index
/// @return the mask
constexpr uint8_t used_mask(int slot)
{
   return static_cast<uint8_t>(1u << (slot % kUsedBitsPerByte));
}

/// Bytes of one stored record (the layout's record bytes).
/// @return bytes
inline uint32_t record_bytes()
{
   return monster::kLayout.record_bytes;
}

/// Whether a slot index is one of the build's slots and its record and used bit fit the storage (C6: the view's
/// capacity, sized from the manifest).
/// @param st    the storage
/// @param slot  slot index
/// @return true if it is
bool in_range(const Storage& st, int slot)
{
   return slot >= 0 && slot < slot_count() && static_cast<uint32_t>(slot + 1) * record_bytes() <= st.capacity &&
          slot / kUsedBitsPerByte < st.used_capacity;
}

/// Marks a slot used or free.
/// @param st    the storage
/// @param slot  slot index (in range)
/// @param on    used
void mark(Storage& st, int slot, bool on)
{
   uint8_t& b = st.used[slot / kUsedBitsPerByte];
   b = static_cast<uint8_t>(on ? b | used_mask(slot) : b & ~used_mask(slot));
}

/// The first free slot of a box.
/// @param st   the storage
/// @param box  box index
/// @return its slot, or kNoSlot when the box is full
uint16_t free_in(const Storage& st, int box)
{
   const int first = box * kStorage.box_size;
   for (int k = 0; k < kStorage.box_size; ++k)
   {
      if (in_range(st, first + k) && !used(st, first + k))
      {
         return static_cast<uint16_t>(first + k);
      }
   }
   return kNoSlot;
}

} // namespace

bool fits(const Storage& st)
{
   const uint32_t slots = static_cast<uint32_t>(slot_count());
   return slots * record_bytes() <= st.capacity && used_bytes_for(slots) <= st.used_capacity;
}

void clear(Storage& st)
{
   for (uint16_t k = 0; k < st.used_capacity; ++k)
   {
      st.used[k] = 0;
   }
   st.current = 0;
}

bool used(const Storage& st, int slot)
{
   return in_range(st, slot) && (st.used[slot / kUsedBitsPerByte] & used_mask(slot)) != 0;
}

int count(const Storage& st)
{
   int n = 0;
   for (int k = 0; k < slot_count(); ++k)
   {
      n += used(st, k) ? 1 : 0;
   }
   return n;
}

int box_count(const Storage& st, int box)
{
   if (box < 0 || box >= kStorage.box_count)
   {
      return 0;
   }
   int n = 0;
   for (int k = 0; k < kStorage.box_size; ++k)
   {
      n += used(st, box * kStorage.box_size + k) ? 1 : 0;
   }
   return n;
}

bool read(const Storage& st, int slot, monster::Record& out)
{
   if (!used(st, slot))
   {
      return false;
   }
   out = monster::Record{};
   const uint8_t* src = st.bytes + static_cast<uint32_t>(slot) * record_bytes();
   for (uint32_t b = 0; b < record_bytes(); ++b)
   {
      out.bytes[b] = src[b];
   }
   return true;
}

bool put(Storage& st, int slot, const monster::Record& rec)
{
   if (!in_range(st, slot) || used(st, slot))
   {
      return false;
   }
   uint8_t* dst = st.bytes + static_cast<uint32_t>(slot) * record_bytes();
   for (uint32_t b = 0; b < record_bytes(); ++b)
   {
      dst[b] = rec.bytes[b];
   }
   mark(st, slot, true);
   return true;
}

uint16_t deposit(Storage& st, const monster::Record& rec)
{
   // the current box first, then every box from the first (the current one again finds nothing)
   const int boxes = kStorage.box_count;
   for (int k = -1; k < boxes; ++k)
   {
      const int box = k < 0 ? st.current : k;
      if (box >= boxes)
      {
         continue;
      }
      const uint16_t slot = free_in(st, box);
      if (slot != kNoSlot && put(st, slot, rec))
      {
         return slot;
      }
   }
   return kNoSlot;
}

bool withdraw(Storage& st, int slot, monster::Record& out)
{
   if (!read(st, slot, out))
   {
      return false;
   }
   mark(st, slot, false);
   return true;
}

bool release(Storage& st, int slot)
{
   if (!used(st, slot))
   {
      return false;
   }
   mark(st, slot, false);
   return true;
}

bool move(Storage& st, int from, int to)
{
   monster::Record rec{};
   if (!used(st, from) || !in_range(st, to) || used(st, to) || !withdraw(st, from, rec))
   {
      return false;
   }
   return put(st, to, rec);
}

bool set_current(Storage& st, int box)
{
   if (box < 0 || box >= kStorage.box_count)
   {
      return false;
   }
   st.current = static_cast<uint8_t>(box);
   return true;
}

bool valid(const Storage& st)
{
   if (kStorage.box_count != 0 && st.current >= kStorage.box_count)
   {
      return false;
   }
   for (int k = slot_count(); k < st.used_capacity * kUsedBitsPerByte; ++k)
   {
      if ((st.used[k / kUsedBitsPerByte] & used_mask(k)) != 0)
      {
         return false;   // a used bit past the build's slots: another build's boxes
      }
   }
   return true;
}

} // namespace capsule::storage
