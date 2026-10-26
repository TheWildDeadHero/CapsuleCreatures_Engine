/// @file
/// Record transfer v2 (capsule/monster/transfer.h): the canonical and raw forms, export and import.
/// tools/record_transfer.py mirrors this file; a change here changes it there.
#include "capsule/monster/transfer.h"

namespace capsule::monster
{

namespace
{

/// Highest valid element width of an incoming role.
constexpr int kMaxShapeWidth = kMaxRoleBits;
/// Bits per byte, signed (offsets and sizes are ints).
constexpr int kByteBits = static_cast<int>(kBitsPerByte);
/// Mask of one byte.
constexpr uint32_t kByteMask = 0xFFu;
/// "No element" / "no stat" (index searches).
constexpr int kNoIndex = -1;

/// Bytes holding a number of bits.
/// @param bits  bits
/// @return bytes (rounded up)
constexpr uint32_t bytes_for_bits(uint32_t bits)
{
   return (bits + kBitsPerByte - 1u) / kBitsPerByte;
}

/// A value with every bit of a width set.
/// @param bits  width (1-32)
/// @return 2^bits - 1
constexpr uint32_t all_ones(int bits)
{
   // A 32-bit shift would be undefined, so the widest values are special-cased.
   return bits >= kMaxRoleBits ? kMaxRoleValue : (uint32_t{1} << bits) - 1u;
}

/// Whether an engine role ID names a per-stat role.
/// @param id  the role
/// @return true for genes, training and awakening
bool per_stat(RoleId id)
{
   return id < kTransferEngineRoles && kRoleChoices[id].count == RoleCount::PerStat;
}

/// Whether a role's empty element has every bit set (Moves: "no move"; Nickname: kCodeEnd), so an import keeps a
/// sender's all-ones value all-ones at the receiver's width.
/// @param id  the role
/// @return true for Moves and Nickname
bool fills_all_ones(RoleId id)
{
   return id == id_of(Role::Moves) || id == id_of(Role::Nickname);
}

/// Writes a little-endian 32-bit key.
/// @param out    first byte
/// @param value  the key
void put_key(uint8_t* out, uint32_t value)
{
   for (int i = 0; i < kTransferKeyBytes; ++i)
   {
      out[i] = static_cast<uint8_t>((value >> (i * kByteBits)) & kByteMask);
   }
}

/// Reads a little-endian 32-bit key.
/// @param in  first byte
/// @return the key
uint32_t get_key(const uint8_t* in)
{
   uint32_t value = 0;
   for (int i = 0; i < kTransferKeyBytes; ++i)
   {
      value |= static_cast<uint32_t>(in[i]) << (i * kByteBits);
   }
   return value;
}

/// The value a receiver's role element holds before (or without) an incoming value.
/// @param info     the receiver's layout
/// @param id       the role
/// @param element  element index (per-stat roles: the stat's index)
/// @return the default
uint32_t default_value(const TransferInfo& info, RoleId id, int element)
{
   const Layout& layout = *info.layout;
   if (id >= kFirstCustomRole)
   {
      return initial(layout, id);
   }
   if (fills_all_ones(id))
   {
      return max_value(layout, id);
   }
   if (per_stat(id) && info.stat_defaults != nullptr)
   {
      return info.stat_defaults[element * kPerStatRoles + (id - id_of(kFirstPerStatRole))];
   }
   return 0;
}

/// The stat a receiver gives a stat key.
/// @param info  the receiver's layout
/// @param key   stat key
/// @return its stat index, or kNoIndex
int find_stat(const TransferInfo& info, uint32_t key)
{
   for (int s = 0; s < info.layout->stat_count; ++s)
   {
      if (info.stat_keys[s] == key)
      {
         return s;
      }
   }
   return kNoIndex;
}

/// The custom role a receiver gives a role key.
/// @param info  the receiver's layout
/// @param key   role key
/// @return its role ID, or kNoRole
RoleId find_custom_key(const TransferInfo& info, uint32_t key)
{
   for (int i = 0; i < info.layout->custom_count; ++i)
   {
      if (info.custom_keys[i] == key)
      {
         return static_cast<RoleId>(kFirstCustomRole + i);
      }
   }
   return kNoRole;
}

/// The parsed header of a canonical record (pointers into the input; nothing copied).
struct Incoming
{
   const uint8_t* in;            ///< The input.
   int engine_count;             ///< Engine role IDs the presence mask covers.
   const uint8_t* mask;          ///< The presence mask.
   int custom_count;             ///< Custom roles.
   const uint8_t* custom_keys;   ///< Their keys (custom_count x 4 bytes).
   int stat_count;               ///< Stats.
   const uint8_t* stat_keys;     ///< Their keys (stat_count x 4 bytes).
   const uint8_t* shapes;        ///< Width and count per present role.
   uint32_t value_bit;           ///< First bit of the values (from in).
   uint32_t value_bits;          ///< Bits of the values.
};

/// One present role of an incoming record.
struct IncomingRole
{
   bool custom;      ///< A custom role (key), else an engine role (id).
   RoleId id;        ///< Engine role ID.
   uint32_t key;     ///< Custom role key.
   uint8_t width;    ///< Element width.
   int count;        ///< Elements.
   uint32_t bit;     ///< First bit of its values (from Incoming::in).
};

/// Walks an incoming record's present roles in canonical order.
struct RoleCursor
{
   int engine_id;      ///< Next engine role ID to look at.
   int custom_index;   ///< Next custom role.
   int shape;          ///< Next shape.
   uint32_t bit;       ///< First bit of the next role's values.
};

/// Whether an incoming record's mask marks an engine role present.
/// @param rec  the record
/// @param id   the engine role ID
/// @return true if present
bool present(const Incoming& rec, int id)
{
   return id < rec.engine_count && ((rec.mask[id / kByteBits] >> (id % kByteBits)) & 1u) != 0;
}

/// The next present role.
/// @param rec     the record
/// @param cursor  where the walk is (advanced)
/// @param out     receives the role
/// @return false after the last role
bool next_role(const Incoming& rec, RoleCursor& cursor, IncomingRole& out)
{
   while (cursor.engine_id < rec.engine_count && !present(rec, cursor.engine_id))
   {
      ++cursor.engine_id;
   }
   if (cursor.engine_id < rec.engine_count)
   {
      out.custom = false;
      out.id = static_cast<RoleId>(cursor.engine_id);
      out.key = 0;
      ++cursor.engine_id;
   }
   else if (cursor.custom_index < rec.custom_count)
   {
      out.custom = true;
      out.id = kNoRole;
      out.key = get_key(rec.custom_keys + cursor.custom_index * kTransferKeyBytes);
      ++cursor.custom_index;
   }
   else
   {
      return false;
   }
   out.width = rec.shapes[cursor.shape * kTransferShapeBytes];
   out.count = rec.shapes[cursor.shape * kTransferShapeBytes + 1];
   out.bit = rec.value_bit + cursor.bit;
   ++cursor.shape;
   cursor.bit += static_cast<uint32_t>(out.width) * static_cast<uint32_t>(out.count);
   return true;
}

/// Parses a key table: its count byte, then that many keys, strictly ascending (one encoding per record).
/// @param in     the input
/// @param size   its bytes
/// @param at     the count byte's offset (advanced past the table)
/// @param count  receives the count
/// @param keys   receives the first key's address
/// @return Ok, Truncated or BadShape
TransferStatus parse_keys(const uint8_t* in, int size, int& at, int& count, const uint8_t*& keys)
{
   if (size < at + 1)
   {
      return TransferStatus::Truncated;
   }
   count = in[at];
   at += 1;
   if (size < at + count * kTransferKeyBytes)
   {
      return TransferStatus::Truncated;
   }
   keys = in + at;
   for (int k = 1; k < count; ++k)
   {
      if (get_key(keys + (k - 1) * kTransferKeyBytes) >= get_key(keys + k * kTransferKeyBytes))
      {
         return TransferStatus::BadShape;
      }
   }
   at += count * kTransferKeyBytes;
   return TransferStatus::Ok;
}

/// Parses and checks a canonical record's header and shapes.
/// @param in    the input (starting at the version byte)
/// @param size  its bytes
/// @param rec   receives the header
/// @return Ok, Truncated or BadShape
TransferStatus parse(const uint8_t* in, int size, Incoming& rec)
{
   int at = kTransferHeaderBytes;
   if (size < at)
   {
      return TransferStatus::Truncated;
   }
   rec.in = in;
   rec.engine_count = in[1];
   const int mask_bytes = (rec.engine_count + kByteBits - 1) / kByteBits;
   if (size < at + mask_bytes)
   {
      return TransferStatus::Truncated;
   }
   rec.mask = in + at;
   int roles = 0;
   for (int id = 0; id < mask_bytes * kByteBits; ++id)
   {
      const bool bit = ((rec.mask[id / kByteBits] >> (id % kByteBits)) & 1u) != 0;
      if (bit && id >= rec.engine_count)
      {
         return TransferStatus::BadShape;   // the mask's padding bits are 0 (one encoding per record)
      }
      roles += bit ? 1 : 0;
   }
   at += mask_bytes;

   // The custom keys, then the stat table.
   TransferStatus status = parse_keys(in, size, at, rec.custom_count, rec.custom_keys);
   if (status == TransferStatus::Ok)
   {
      status = parse_keys(in, size, at, rec.stat_count, rec.stat_keys);
   }
   if (status != TransferStatus::Ok)
   {
      return status;
   }
   roles += rec.custom_count;
   if (size < at + roles * kTransferShapeBytes)
   {
      return TransferStatus::Truncated;
   }
   rec.shapes = in + at;
   at += roles * kTransferShapeBytes;
   rec.value_bit = static_cast<uint32_t>(at) * kBitsPerByte;
   rec.value_bits = 0;

   // Shapes: widths 1-32, at least one element, a per-stat role one element per stat of the table.
   RoleCursor cursor = {0, 0, 0, 0};
   IncomingRole role = {};
   while (next_role(rec, cursor, role))
   {
      if (role.width < 1 || role.width > kMaxShapeWidth || role.count < 1 ||
          (!role.custom && per_stat(role.id) && role.count != rec.stat_count))
      {
         return TransferStatus::BadShape;
      }
   }
   rec.value_bits = cursor.bit;
   if (static_cast<uint32_t>(size) < static_cast<uint32_t>(at) + bytes_for_bits(rec.value_bits))
   {
      return TransferStatus::Truncated;
   }
   return TransferStatus::Ok;
}

/// The incoming role a receiver role matches.
/// @param rec    the incoming record
/// @param id     the receiver's role ID
/// @param key    its key (custom roles)
/// @param found  receives the incoming role
/// @return false when the incoming record does not have it
bool find_incoming(const Incoming& rec, RoleId id, uint32_t key, IncomingRole& found)
{
   RoleCursor cursor = {0, 0, 0, 0};
   while (next_role(rec, cursor, found))
   {
      const bool same =
         found.custom ? id >= kFirstCustomRole && found.key == key : id < kFirstCustomRole && found.id == id;
      if (same)
      {
         return true;
      }
   }
   return false;
}

/// Whether an incoming record's stat table holds a key.
/// @param rec  the record
/// @param key  stat key
/// @return true if it does
bool has_stat(const Incoming& rec, uint32_t key)
{
   for (int k = 0; k < rec.stat_count; ++k)
   {
      if (get_key(rec.stat_keys + k * kTransferKeyBytes) == key)
      {
         return true;
      }
   }
   return false;
}

/// A sender's value at the receiver's width (the import rule: Moves / Nickname keep all-ones, others clamp).
/// @param layout    the receiver's layout
/// @param id        the receiver's role
/// @param value     the sender's value
/// @param width     the sender's element width
/// @param narrowed  set when the value was clamped
/// @return the receiver's value
uint32_t convert(const Layout& layout, RoleId id, uint32_t value, uint8_t width, bool& narrowed)
{
   const uint32_t top = max_value(layout, id);
   if (fills_all_ones(id) && value == all_ones(width))
   {
      return top;
   }
   if (value > top)
   {
      narrowed = true;
      return top;
   }
   return value;
}

/// Every role a layout holds (the raw form brings them all).
/// @param layout  the layout
/// @return the set
RoleSet held_roles(const Layout& layout)
{
   RoleSet out = {0, 0};
   for (int i = 0; i < kTransferEngineRoles + layout.custom_count; ++i)
   {
      const RoleId id = i < kTransferEngineRoles ? static_cast<RoleId>(i)
                                                 : static_cast<RoleId>(kFirstCustomRole + i - kTransferEngineRoles);
      if (has(layout, id))
      {
         add_role(out, id);
      }
   }
   return out;
}

/// Imports a canonical record whose header parsed.
/// @param info      the receiver's layout
/// @param rec       the incoming record
/// @param record    the receiver's record (written)
/// @param received  receives the receiver roles the record held
/// @return the import flags
uint8_t import_canonical(const TransferInfo& info, const Incoming& rec, uint8_t* record, RoleSet& received)
{
   const Layout& layout = *info.layout;
   uint8_t flags = 0;

   // Every role the receiver holds starts at its default.
   for (int b = 0; b < layout.record_bytes; ++b)
   {
      record[b] = 0;
   }
   for (int i = 0; i < kTransferEngineRoles + layout.custom_count; ++i)
   {
      const RoleId id = i < kTransferEngineRoles ? static_cast<RoleId>(i)
                                                 : static_cast<RoleId>(kFirstCustomRole + i - kTransferEngineRoles);
      for (int e = 0; has(layout, id) && e < count(layout, id); ++e)
      {
         set(layout, record, id, e, default_value(info, id, e));
      }
   }

   // The incoming roles the receiver holds, element by element (C5: the ones an element of was narrowed are not
   // received as they were: a battle game re-derives them)
   RoleSet narrowed_roles = {0, 0};
   RoleCursor cursor = {0, 0, 0, 0};
   IncomingRole role = {};
   while (next_role(rec, cursor, role))
   {
      RoleId id = kNoRole;
      if (role.custom)
      {
         id = find_custom_key(info, role.key);
      }
      else if (role.id < kTransferEngineRoles && has(layout, role.id))
      {
         id = role.id;
      }
      if (id == kNoRole)
      {
         flags |= kTransferDropped;
         const bool egg = role.custom && role.key == kEggTransferKey && role.count > 0 &&
                          read_bits(rec.in, role.bit, role.width) != 0;
         if (egg)
         {
            flags |= kTransferEgg;   // C9: an egg the receiver has no egg role for (the battle side decides)
         }
         continue;
      }
      for (int e = 0; e < role.count; ++e)
      {
         const uint32_t value = read_bits(rec.in, role.bit + static_cast<uint32_t>(e) * role.width, role.width);
         bool narrowed = false;
         const uint32_t mine = convert(layout, id, value, role.width, narrowed);
         int element = e;
         if (per_stat(id))
         {
            // Per-stat elements follow the stat table: element e is the stat of the table's key e.
            element = find_stat(info, get_key(rec.stat_keys + e * kTransferKeyBytes));
            if (element == kNoIndex)
            {
               flags |= kTransferDropped;
               continue;
            }
         }
         else if (e >= count(layout, id))
         {
            // Past the receiver's elements: lost only when it is not the receiver's default (an empty move slot).
            flags = static_cast<uint8_t>(flags | (mine != default_value(info, id, e) ? kTransferDropped : 0));
            continue;
         }
         flags = static_cast<uint8_t>(flags | (narrowed ? kTransferNarrowed : 0));
         if (narrowed)
         {
            add_role(narrowed_roles, id);
         }
         set(layout, record, id, element, mine);
      }
   }

   // Receiver roles, stats and elements the record did not fill.
   for (int i = 0; i < kTransferEngineRoles + layout.custom_count; ++i)
   {
      const bool custom = i >= kTransferEngineRoles;
      const RoleId id =
         custom ? static_cast<RoleId>(kFirstCustomRole + i - kTransferEngineRoles) : static_cast<RoleId>(i);
      if (!has(layout, id))
      {
         continue;
      }
      const uint32_t key = custom ? info.custom_keys[i - kTransferEngineRoles] : 0;
      IncomingRole found = {};
      if (!find_incoming(rec, id, key, found))
      {
         flags |= kTransferDefaulted;
         continue;
      }
      // C5: received only when every element came as it was (none defaulted, none narrowed)
      bool whole = !has_role(narrowed_roles, id);
      if (per_stat(id))
      {
         for (int s = 0; s < layout.stat_count; ++s)
         {
            const bool missing = !has_stat(rec, info.stat_keys[s]);
            whole = whole && !missing;
            flags = static_cast<uint8_t>(flags | (missing ? kTransferDefaulted : 0));
         }
      }
      else if (count(layout, id) > found.count)
      {
         whole = false;
         flags |= kTransferDefaulted;
      }
      if (whole)
      {
         add_role(received, id);
      }
   }
   return flags;
}

} // namespace

int transfer_size(const TransferInfo& info, TransferForm form)
{
   const Layout& layout = *info.layout;
   if (form == TransferForm::Raw)
   {
      return kTransferRawHeaderBytes + layout.record_bytes;
   }
   int bytes = kTransferHeaderBytes + kTransferMaskBytes + 1 + layout.custom_count * kTransferKeyBytes + 1 +
               layout.stat_count * kTransferKeyBytes;
   uint32_t bits = 0;
   for (int i = 0; i < kTransferEngineRoles + layout.custom_count; ++i)
   {
      const RoleId id = i < kTransferEngineRoles ? static_cast<RoleId>(i)
                                                 : static_cast<RoleId>(kFirstCustomRole + i - kTransferEngineRoles);
      if (has(layout, id))
      {
         bytes += kTransferShapeBytes;
         bits += static_cast<uint32_t>(width(layout, id)) * static_cast<uint32_t>(count(layout, id));
      }
   }
   return bytes + static_cast<int>(bytes_for_bits(bits));
}

TransferResult export_record(const TransferInfo& info, const uint8_t* record, uint8_t* out, int capacity,
                             TransferForm form)
{
   const Layout& layout = *info.layout;
   const int size = transfer_size(info, form);
   if (capacity < size)
   {
      return {TransferStatus::ShortBuffer, 0, 0};
   }
   for (int b = 0; b < size; ++b)
   {
      out[b] = 0;
   }
   if (form == TransferForm::Raw)
   {
      out[0] = kTransferRawVersion;
      put_key(out + 1, info.hash);
      for (int b = 0; b < layout.record_bytes; ++b)
      {
         out[kTransferRawHeaderBytes + b] = record[b];
      }
      return {TransferStatus::Ok, kTransferRaw, static_cast<uint16_t>(size)};
   }

   // Header: version, the engine roles the mask covers, the mask, the custom keys and the stat table (ascending).
   out[0] = kTransferVersion;
   out[1] = static_cast<uint8_t>(kTransferEngineRoles);
   int at = kTransferHeaderBytes;
   for (int id = 0; id < kTransferEngineRoles; ++id)
   {
      if (has(layout, static_cast<RoleId>(id)))
      {
         out[at + id / kByteBits] = static_cast<uint8_t>(out[at + id / kByteBits] | (1u << (id % kByteBits)));
      }
   }
   at += kTransferMaskBytes;
   out[at++] = layout.custom_count;
   for (int k = 0; k < layout.custom_count; ++k, at += kTransferKeyBytes)
   {
      put_key(out + at, info.custom_keys[info.custom_order[k]]);
   }
   out[at++] = layout.stat_count;
   for (int k = 0; k < layout.stat_count; ++k, at += kTransferKeyBytes)
   {
      put_key(out + at, info.stat_keys[info.stat_order[k]]);
   }

   // Shapes, then the values, both in canonical order (engine roles by ID, custom roles by key).
   int shapes = 0;
   for (int i = 0; i < kTransferEngineRoles + layout.custom_count; ++i)
   {
      const RoleId id = i < kTransferEngineRoles
                           ? static_cast<RoleId>(i)
                           : static_cast<RoleId>(kFirstCustomRole + info.custom_order[i - kTransferEngineRoles]);
      shapes += has(layout, id) ? 1 : 0;
   }
   uint32_t bit = static_cast<uint32_t>(at + shapes * kTransferShapeBytes) * kBitsPerByte;
   for (int i = 0; i < kTransferEngineRoles + layout.custom_count; ++i)
   {
      const RoleId id = i < kTransferEngineRoles
                           ? static_cast<RoleId>(i)
                           : static_cast<RoleId>(kFirstCustomRole + info.custom_order[i - kTransferEngineRoles]);
      if (!has(layout, id))
      {
         continue;
      }
      const uint8_t bits = width(layout, id);
      const int elements = count(layout, id);
      out[at++] = bits;
      out[at++] = static_cast<uint8_t>(elements);
      for (int e = 0; e < elements; ++e, bit += bits)
      {
         // A per-stat role's elements go in stat table order.
         const int element = per_stat(id) ? info.stat_order[e] : e;
         write_bits(out, bit, bits, get(layout, record, id, element));
      }
   }
   return {TransferStatus::Ok, 0, static_cast<uint16_t>(size)};
}

TransferResult import_record(const TransferInfo& info, const uint8_t* in, int size, uint8_t* record, int capacity,
                             RoleSet* received)
{
   const Layout& layout = *info.layout;
   if (size < 1)
   {
      return {TransferStatus::Truncated, 0, 0};
   }
   if (capacity < layout.record_bytes)
   {
      return {TransferStatus::ShortBuffer, 0, 0};
   }
   if (in[0] == kTransferRawVersion)
   {
      const int bytes = kTransferRawHeaderBytes + layout.record_bytes;
      if (size < kTransferRawHeaderBytes)
      {
         return {TransferStatus::Truncated, 0, 0};
      }
      if (get_key(in + 1) != info.hash)
      {
         return {TransferStatus::OtherLayout, 0, 0};
      }
      if (size < bytes)
      {
         return {TransferStatus::Truncated, 0, 0};
      }
      for (int b = 0; b < layout.record_bytes; ++b)
      {
         record[b] = in[kTransferRawHeaderBytes + b];
      }
      if (received != nullptr)
      {
         // the same layout: every role the receiver holds came with the record
         *received = held_roles(layout);
      }
      return {TransferStatus::Ok, kTransferRaw, static_cast<uint16_t>(bytes)};
   }
   if (in[0] != kTransferVersion)
   {
      return {TransferStatus::BadVersion, 0, 0};
   }
   Incoming rec = {};
   const TransferStatus status = parse(in, size, rec);
   if (status != TransferStatus::Ok)
   {
      return {status, 0, 0};
   }
   RoleSet got = {0, 0};
   const uint8_t flags = import_canonical(info, rec, record, got);
   if (received != nullptr)
   {
      *received = got;
   }
   const uint32_t bytes = rec.value_bit / kBitsPerByte + bytes_for_bits(rec.value_bits);
   return {TransferStatus::Ok, flags, static_cast<uint16_t>(bytes)};
}

} // namespace capsule::monster
