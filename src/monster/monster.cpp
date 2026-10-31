/// @file
/// Role access over packed records (C-07a): engine roles and the layout's custom roles.
#include "capsule/monster/monster.h"

namespace capsule::monster
{

namespace
{

/// A bit index's position inside its byte (bit & 7).
constexpr uint32_t kBitInByteMask = 7u;
/// Shift from a bit index to its byte index (bit / 8).
constexpr int kByteOfBitShift = 3;
/// Engine roles in the registry.
constexpr int kEngineRoles = static_cast<int>(Role::NUM_ROLES);

/// A layout's custom role of an ID.
/// @param layout  the layout
/// @param id      the role
/// @return its slot, or nullptr when @p id is no custom role of the layout
const RoleSlot* custom_slot(const Layout& layout, RoleId id)
{
   if (id < kFirstCustomRole || layout.custom == nullptr)
   {
      return nullptr;
   }
   // the stored custom roles, then the unstored ones (offset kAbsent: their reads fall through to 0)
   const int index = id - kFirstCustomRole;
   return index < layout.custom_count + layout.unstored_count ? &layout.custom[index] : nullptr;
}

/// First bit of a role (its element 0), or kAbsent.
/// @param layout  the layout
/// @param id      the role
/// @return bit offset
uint32_t base_of(const Layout& layout, RoleId id)
{
   if (const RoleSlot* slot = custom_slot(layout, id))
   {
      return slot->offset;
   }
   if (id >= kEngineRoles || layout.offsets == nullptr)
   {
      return kAbsent;
   }
   return layout.offsets[id];
}

/// Bit offset of a role element, or kAbsent.
/// @param layout   the layout
/// @param id       the role
/// @param element  element index
/// @return first bit, or kAbsent
uint32_t bit_of(const Layout& layout, RoleId id, int element)
{
   const uint32_t base = base_of(layout, id);
   if (base == kAbsent || element < 0 || element >= count(layout, id))
   {
      return kAbsent;
   }
   return base + static_cast<uint32_t>(element) * width(layout, id);
}

} // namespace

uint32_t read_bits(const uint8_t* bytes, uint32_t bit, uint8_t bits)
{
   uint32_t value = 0;
   for (uint8_t k = 0; k < bits;)
   {
      const uint32_t at = bit + k;
      // Fields may straddle bytes: take what is left of this byte, or what is left of the field.
      const uint32_t shift = at & kBitInByteMask;
      const uint32_t take = (kBitsPerByte - shift) < static_cast<uint32_t>(bits - k) ? kBitsPerByte - shift : bits - k;
      const uint32_t part = (bytes[at >> kByteOfBitShift] >> shift) & ((1u << take) - 1u);
      value |= part << k;
      k = static_cast<uint8_t>(k + take);
   }
   return value;
}

void write_bits(uint8_t* bytes, uint32_t bit, uint8_t bits, uint32_t value)
{
   for (uint8_t k = 0; k < bits;)
   {
      const uint32_t at = bit + k;
      // Fields may straddle bytes: take what is left of this byte, or what is left of the field.
      const uint32_t shift = at & kBitInByteMask;
      const uint32_t take = (kBitsPerByte - shift) < static_cast<uint32_t>(bits - k) ? kBitsPerByte - shift : bits - k;
      const uint32_t mask = ((1u << take) - 1u) << shift;
      const uint32_t part = ((value >> k) << shift) & mask;
      bytes[at >> kByteOfBitShift] = static_cast<uint8_t>((bytes[at >> kByteOfBitShift] & ~mask) | part);
      k = static_cast<uint8_t>(k + take);
   }
}

RoleId find_custom(const Layout& layout, uint32_t hash)
{
   for (int i = 0; layout.custom != nullptr && i < layout.custom_count + layout.unstored_count; ++i)
   {
      if (layout.custom[i].name_hash == hash)
      {
         return static_cast<RoleId>(kFirstCustomRole + i);
      }
   }
   return kNoRole;
}

uint8_t width(const Layout& layout, RoleId id)
{
   if (const RoleSlot* slot = custom_slot(layout, id))
   {
      return slot->width;
   }
   if (id >= kEngineRoles)
   {
      return 0;
   }
   return layout.widths != nullptr ? layout.widths[id] : kRoleShapes[id].width;
}

int count(const Layout& layout, RoleId id)
{
   if (const RoleSlot* slot = custom_slot(layout, id))
   {
      return slot->count;
   }
   if (id >= kEngineRoles)
   {
      return 0;
   }
   if (layout.counts != nullptr)
   {
      return layout.counts[id];
   }
   // A layout without a counts table keeps the registry's counts (per-stat roles follow its stats).
   return kRoleChoices[id].count == RoleCount::PerStat ? layout.stat_count : kRoleShapes[id].count;
}

uint32_t max_value(const Layout& layout, RoleId id)
{
   const uint8_t w = width(layout, id);
   // A 32-bit shift would be undefined, so the widest roles are special-cased.
   return w >= kMaxRoleBits ? kMaxRoleValue : (uint32_t{1} << w) - 1u;
}

uint32_t initial(const Layout& layout, RoleId id)
{
   const RoleSlot* slot = custom_slot(layout, id);
   return slot != nullptr ? slot->initial : 0;
}

bool has(const Layout& layout, RoleId id)
{
   return bit_of(layout, id, 0) != kAbsent;
}

uint32_t get(const Layout& layout, const uint8_t* bytes, RoleId id, int element)
{
   const uint32_t bit = bit_of(layout, id, element);
   return bit == kAbsent ? 0 : read_bits(bytes, bit, width(layout, id));
}

void set(const Layout& layout, uint8_t* bytes, RoleId id, int element, uint32_t value)
{
   const uint32_t bit = bit_of(layout, id, element);
   if (bit != kAbsent)
   {
      write_bits(bytes, bit, width(layout, id), value);
   }
}

// The build's layout: the functions above with kLayout.

RoleId find_custom(uint32_t hash)
{
   return find_custom(kLayout, hash);
}

uint8_t width(RoleId id)
{
   return width(kLayout, id);
}

int count(RoleId id)
{
   return count(kLayout, id);
}

uint32_t max_value(RoleId id)
{
   return max_value(kLayout, id);
}

uint32_t initial(RoleId id)
{
   return initial(kLayout, id);
}

bool has(RoleId id)
{
   return has(kLayout, id);
}

uint32_t get(const Record& rec, RoleId id, int element)
{
   return get(kLayout, rec.bytes, id, element);
}

void set(Record& rec, RoleId id, int element, uint32_t value)
{
   set(kLayout, rec.bytes, id, element, value);
}

void clear(Record& rec)
{
   for (uint8_t& b : rec.bytes)
   {
      b = 0;
   }
}

void reset(Record& rec)
{
   clear(rec);
   for (int i = 0; kLayout.custom != nullptr && i < kLayout.custom_count; ++i)
   {
      const RoleId id = static_cast<RoleId>(kFirstCustomRole + i);
      for (int e = 0; kLayout.custom[i].initial != 0 && e < kLayout.custom[i].count; ++e)
      {
         set(rec, id, e, kLayout.custom[i].initial);
      }
   }
}

} // namespace capsule::monster
