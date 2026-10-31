/// @file
/// The engine save image (see capsule/save/save.h): versioned sections sized from the manifests, streamed to and
/// from the save medium through a small buffer, a checksum per section and one over the header and table; loading
/// migrates sections of older shapes (C6).
///
/// Code size (C6, review4 finding 22): the byte streams are two plain structs whose per-byte functions are out of
/// line (no template instantiated per stream kind); every section has one encoder and one decoder.
///
/// C7 (final review): copies lie in fixed slots (slot_bytes()), reading stages every section it loads before any
/// lands (the stack stage, then the caller's Stage), sequences compare by serial-number arithmetic (newer()).
#include "capsule/save/save.h"

#include "capsule/core/memory.h"
#include "capsule/hal/save.h"
#include "capsule/script/data.h"

namespace capsule::save
{

namespace
{

/// @name Header field offsets
///@{
constexpr uint32_t kMagicOffset = 0;      ///< Magic word.
constexpr uint32_t kVersionOffset = 4;    ///< Format version (16 bits).
constexpr uint32_t kCountOffset = 6;      ///< Section count (8 bits).
constexpr uint32_t kSequenceOffset = 8;   ///< Sequence number.
constexpr uint32_t kBytesOffset = 12;     ///< Bytes after the header.
constexpr uint32_t kChecksumOffset = 16;  ///< Header checksum (not hashed).
constexpr uint32_t kChecksumBytes = 4;    ///< Its size.
///@}
/// @name Section table entry field offsets
///@{
constexpr uint32_t kEntryId = 0;          ///< Section id.
constexpr uint32_t kEntryVersion = 1;     ///< The section's own version.
constexpr uint32_t kEntrySize = 4;        ///< Its bytes.
constexpr uint32_t kEntryChecksum = 8;    ///< Checksum of its bytes.
///@}
/// @name Section versions this build writes and reads
///@{
constexpr uint8_t kBagVersion = 1;        ///< Bag entries by item key.
constexpr uint8_t kPartyVersion = 1;      ///< Party with its layout hash.
constexpr uint8_t kBoxesVersion = 1;      ///< Boxes with their geometry.
constexpr uint8_t kScriptVersion = 1;     ///< Script sub-banks with their shape hashes.
constexpr uint8_t kNurseryVersion = 1;    ///< B2: the nursery with its geometry.
constexpr uint8_t kGameVersion = 1;       ///< N3: the game's block with its own version and size.
///@}
/// Copies of the image a medium keeps at most (the older one takes the next write).
constexpr int kMaxCopies = 2;
/// Bytes the image streams through at a time: an Atmel flash page (C7), and every flush of a write starts on a
/// page boundary (a slot starts on an erase unit), so each page is programmed once per write.
constexpr uint32_t kChunk = 128;
/// Bytes of the header and the longest table.
constexpr uint32_t kHeadRoom = kHeaderBytes + kMaxSections * kSectionEntryBytes;
/// Times a medium read is tried before it counts as failed.
constexpr int kReadRetries = 3;
#if defined(CAPSULE_SAVE_STAGE_BYTES)
/// Bytes of the built-in stage (a test build sets a small one: CAPSULE_SAVE_STAGE_BYTES).
constexpr uint32_t kStageBytes = CAPSULE_SAVE_STAGE_BYTES;
/// The built-in stage lives on the stack while read() runs.
#define CAPSULE_SAVE_STAGE_ON_STACK 1
#elif defined(CAPSULE_PLATFORM_GBA)
/// Bytes of the built-in stage the sections a read loads are staged in, back to back, before any lands (C7). GBA:
/// on the stack (IWRAM) while read() runs; the bag, party and script sections fit, the storage boxes need the
/// caller's Stage (see save.h).
constexpr uint32_t kStageBytes = 0x800u;
/// The built-in stage lives on the stack while read() runs.
#define CAPSULE_SAVE_STAGE_ON_STACK 1
#else
/// Bytes of the built-in stage (C7): the whole budget, a static buffer (hosts have the room), so every image stages.
constexpr uint32_t kStageBytes = kSaveBudget;
/// The built-in stage (hosts).
uint8_t g_stage[kStageBytes];
#endif
/// Half of the serial-number space (RFC 1982: a sequence is newer when it is less than this ahead).
constexpr uint32_t kSerialHalf = 0x80000000u;
/// Bits per byte.
constexpr uint32_t kByteBits = 8;
/// The low byte of a value.
constexpr uint32_t kByteMask = 0xFFu;
/// Bytes of a 16-bit value.
constexpr int kU16Bytes = 2;
/// Bytes of a 32-bit value.
constexpr int kU32Bytes = 4;
/// Bits of a 32-bit value.
constexpr uint32_t kU32Bits = 32;

/// FNV-1a 32 step.
/// @param h  hash so far
/// @param b  next byte
/// @return the hash
constexpr uint32_t fnv(uint32_t h, uint8_t b)
{
   return (h ^ b) * monster::kFnvPrime;
}

/// A little-endian value of bytes.
/// @param p  its first byte
/// @param n  bytes (1 to 4)
/// @return the value
uint32_t get_le(const uint8_t* p, int n)
{
   uint32_t v = 0;
   for (int k = 0; k < n; ++k)
   {
      v |= static_cast<uint32_t>(p[k]) << (static_cast<uint32_t>(k) * kByteBits);
   }
   return v;
}

/// Writes a little-endian value.
/// @param p  its first byte
/// @param v  the value
/// @param n  bytes (1 to 4)
void put_le(uint8_t* p, uint32_t v, int n)
{
   for (int k = 0; k < n; ++k)
   {
      p[k] = static_cast<uint8_t>((v >> (static_cast<uint32_t>(k) * kByteBits)) & kByteMask);
   }
}

/// A size rounded up to whole units.
/// @param v     bytes
/// @param unit  unit (1 or more)
/// @return the rounded size
uint32_t round_up(uint32_t v, uint32_t unit)
{
   return unit <= 1 ? v : (v + unit - 1) / unit * unit;
}

/// The medium's erase unit (1 or more).
/// @return bytes
uint32_t unit_bytes()
{
   const uint32_t unit = hal::save::erase_unit();
   return unit == 0 ? 1 : unit;
}

/// Bytes one copy of this build's largest image takes on the medium.
/// @return whole erase units holding it
uint32_t copy_bytes()
{
   return round_up(image_size(), unit_bytes());
}

/// Medium offset of a copy's slot (C7: from the medium alone, never from an image's size).
/// @param k  copy index
/// @return its first byte
uint32_t copy_base(int k)
{
   return slot_bytes() * static_cast<uint32_t>(k);
}

/// Bytes a copy may take from its slot's start. Writing copy 0: its slot when the medium keeps two copies, else the
/// whole medium; reading copy 0: the whole medium (an image an earlier one-copy build wrote); copy 1: the medium
/// past its slot's start.
/// @param k        copy index
/// @param writing  true: room for a write
/// @return bytes
uint32_t copy_room(int k, bool writing)
{
   const uint32_t capacity = hal::save::capacity();
   if (k == 0)
   {
      return writing && copy_count() == kMaxCopies ? slot_bytes() : capacity;
   }
   return capacity - copy_base(k);
}

/// Whether one sequence number is newer than another (RFC 1982 serial-number arithmetic: a write after
/// 0xFFFFFFFF counts on from 0 and is still the newer one).
/// @param a  a sequence
/// @param b  another
/// @return true if @p a is newer than @p b
bool newer(uint32_t a, uint32_t b)
{
   return a != b && a - b < kSerialHalf;
}

/// Bytes of the storage's used bits of a box geometry.
/// @param slots  slots
/// @return bytes
uint32_t used_bytes(uint32_t slots)
{
   return storage::used_bytes_for(slots);
}

// -- streams -----------------------------------------------------------------------------------------------------

/// Bytes read off the medium (through a chunk buffer) or out of a staged copy, hashed as they go. Reading past the
/// section's end gives zeros (a shorter older section reads as zero-filled).
struct Source
{
   const uint8_t* mem;   ///< Staged bytes, or nullptr: the medium.
   uint32_t at;          ///< Medium offset of the next fill (or index into mem).
   uint32_t left;        ///< Bytes left in the section.
   uint32_t hash;        ///< FNV-1a 32 of the bytes so far.
   bool ok;              ///< No medium error so far.
   uint32_t n;           ///< Bytes in buf.
   uint32_t pos;         ///< Next byte in buf.
   uint8_t buf[kChunk];  ///< Bytes read ahead.
};

/// A source over a section.
/// @param s     the source (out)
/// @param mem   staged bytes, or nullptr: the medium
/// @param at    the section's medium offset (or 0 for staged bytes)
/// @param size  its bytes
void open(Source& s, const uint8_t* mem, uint32_t at, uint32_t size)
{
   s.mem = mem;
   s.at = at;
   s.left = size;
   s.hash = monster::kFnvOffsetBasis;
   s.ok = true;
   s.n = 0;
   s.pos = 0;
}

/// Reads medium bytes, retried (kReadRetries).
/// @param at    offset
/// @param dst   destination
/// @param size  bytes
/// @return false when every try failed
[[gnu::noinline]] bool read_medium(uint32_t at, uint8_t* dst, uint32_t size)
{
   for (int t = 0; t < kReadRetries; ++t)
   {
      if (hal::save::read(at, dst, size))
      {
         return true;
      }
   }
   return false;
}

/// The next byte of a source.
/// @param s  the source
/// @return it (0 past the section's end or after a medium error)
[[gnu::noinline]] uint8_t next(Source& s)
{
   if (s.left == 0)
   {
      return 0;
   }
   --s.left;
   uint8_t b = 0;
   if (s.mem != nullptr)
   {
      b = s.mem[s.at++];
   }
   else
   {
      if (s.pos == s.n)
      {
         // a whole chunk, or what is left of the section
         s.n = s.left + 1 < kChunk ? s.left + 1 : kChunk;
         s.ok = s.ok && read_medium(s.at, s.buf, s.n);
         s.at += s.n;
         s.pos = 0;
      }
      b = s.ok ? s.buf[s.pos] : 0;
      ++s.pos;
   }
   s.hash = fnv(s.hash, b);
   return b;
}

/// The next little-endian value of a source.
/// @param s      the source
/// @param bytes  its size (1 to 4)
/// @return the value
[[gnu::noinline]] uint32_t next_le(Source& s, int bytes)
{
   uint32_t v = 0;
   for (int k = 0; k < bytes; ++k)
   {
      v |= static_cast<uint32_t>(next(s)) << (static_cast<uint32_t>(k) * kByteBits);
   }
   return v;
}

/// Skips bytes of a source (hashed).
/// @param s  the source
/// @param n  bytes
void skip(Source& s, uint32_t n)
{
   for (uint32_t k = 0; k < n; ++k)
   {
      next(s);
   }
}

/// Bytes hashed (the first pass of a write) or written to the medium through a chunk buffer.
struct Sink
{
   bool emit;            ///< false: hashed and counted only.
   uint32_t at;          ///< Medium offset of the next flush.
   uint32_t hash;        ///< FNV-1a 32 of the bytes so far.
   uint32_t size;        ///< Bytes so far.
   bool ok;              ///< No medium error so far.
   uint32_t n;           ///< Bytes in buf.
   uint8_t buf[kChunk];  ///< Pending bytes.
};

/// A sink.
/// @param s     the sink (out)
/// @param emit  false: hash only
/// @param at    medium offset of the first byte
void open(Sink& s, bool emit, uint32_t at)
{
   s.emit = emit;
   s.at = at;
   s.hash = monster::kFnvOffsetBasis;
   s.size = 0;
   s.ok = true;
   s.n = 0;
}

/// Writes the buffered bytes.
/// @param s  the sink
[[gnu::noinline]] void flush(Sink& s)
{
   if (s.n != 0)
   {
      s.ok = s.ok && hal::save::program(s.at, s.buf, s.n);
      s.at += s.n;
      s.n = 0;
   }
}

/// One byte into a sink.
/// @param s  the sink
/// @param b  the byte
[[gnu::noinline]] void put(Sink& s, uint8_t b)
{
   s.hash = fnv(s.hash, b);
   ++s.size;
   if (s.emit)
   {
      s.buf[s.n++] = b;
      if (s.n == kChunk)
      {
         flush(s);
      }
   }
}

/// A little-endian value into a sink.
/// @param s      the sink
/// @param v      the value
/// @param bytes  its size (1 to 4)
[[gnu::noinline]] void put_le(Sink& s, uint32_t v, int bytes)
{
   for (int k = 0; k < bytes; ++k)
   {
      put(s, static_cast<uint8_t>((v >> (static_cast<uint32_t>(k) * kByteBits)) & kByteMask));
   }
}

/// Raw bytes into a sink.
/// @param s     the sink
/// @param p     bytes
/// @param size  count
void put_block(Sink& s, const uint8_t* p, uint32_t size)
{
   for (uint32_t k = 0; k < size; ++k)
   {
      put(s, p[k]);
   }
}

// -- the table ---------------------------------------------------------------------------------------------------

/// One section table entry.
struct Entry
{
   uint8_t id;          ///< Section id.
   uint8_t version;     ///< Its version.
   uint32_t size;       ///< Its bytes.
   uint32_t checksum;   ///< Their checksum.
   uint32_t offset;     ///< Medium offset of its first byte (reading), or 0.
};

/// What a copy's header and table say.
struct Table
{
   uint32_t sequence;              ///< Sequence number.
   uint8_t count;                  ///< Entries.
   Entry entries[kMaxSections];    ///< The table.
};

/// The entry of a section in a table.
/// @param t   the table
/// @param id  the section
/// @return the entry, or nullptr when the image has none
const Entry* find(const Table& t, Section id)
{
   for (uint8_t k = 0; k < t.count; ++k)
   {
      if (t.entries[k].id == static_cast<uint8_t>(id))
      {
         return &t.entries[k];
      }
   }
   return nullptr;
}

/// Reads and checks a copy's header and table.
/// @param k  copy index
/// @param t  the table (out)
/// @return Ok, Empty, OtherVersion, Corrupt or MediumError
Status read_table(int k, Table& t)
{
   uint8_t head[kHeadRoom];
   const uint32_t base = copy_base(k);
   const uint32_t room = copy_room(k, false);
   if (!read_medium(base, head, kHeaderBytes))
   {
      return Status::MediumError;
   }
   if (get_le(head + kMagicOffset, kU32Bytes) != kMagic)
   {
      return Status::Empty;
   }
   if (get_le(head + kVersionOffset, kU16Bytes) != kVersion)
   {
      return Status::OtherVersion;
   }
   t.count = head[kCountOffset];
   t.sequence = get_le(head + kSequenceOffset, kU32Bytes);
   const uint32_t table_bytes = t.count * kSectionEntryBytes;
   if (t.count > kMaxSections || get_le(head + kBytesOffset, kU32Bytes) > room - kHeaderBytes)
   {
      return Status::Corrupt;
   }
   if (!read_medium(base + kHeaderBytes, head + kHeaderBytes, table_bytes))
   {
      return Status::MediumError;
   }
   uint32_t h = monster::kFnvOffsetBasis;
   for (uint32_t b = 0; b < kHeaderBytes + table_bytes; ++b)
   {
      if (b < kChecksumOffset || b >= kChecksumOffset + kChecksumBytes)
      {
         h = fnv(h, head[b]);
      }
   }
   if (h != get_le(head + kChecksumOffset, kU32Bytes))
   {
      return Status::Corrupt;
   }
   // the sections follow the table in its order; each must lie inside the copy
   uint32_t at = kHeaderBytes + table_bytes;
   for (uint8_t e = 0; e < t.count; ++e)
   {
      const uint8_t* p = head + kHeaderBytes + e * kSectionEntryBytes;
      Entry& en = t.entries[e];
      en.id = p[kEntryId];
      en.version = p[kEntryVersion];
      en.size = get_le(p + kEntrySize, kU32Bytes);
      en.checksum = get_le(p + kEntryChecksum, kU32Bytes);
      if (en.size > room - at)
      {
         return Status::Corrupt;
      }
      en.offset = base + at;
      at += en.size;
   }
   return Status::Ok;
}

/// Checks a copy whole: header, table and every section's checksum (nothing is stored).
/// @param k  copy index
/// @param t  its table (out)
/// @return Ok when it is a valid image, else why not
Status check_copy(int k, Table& t)
{
   const Status st = read_table(k, t);
   if (st != Status::Ok)
   {
      return st;
   }
   for (uint8_t e = 0; e < t.count; ++e)
   {
      Source s{};
      open(s, nullptr, t.entries[e].offset, t.entries[e].size);
      skip(s, t.entries[e].size);
      if (!s.ok)
      {
         return Status::MediumError;
      }
      if (s.hash != t.entries[e].checksum)
      {
         return Status::Corrupt;
      }
   }
   return Status::Ok;
}

/// The newest valid copy.
/// @param t    its table (out)
/// @param why  the most telling reason when none is valid (out)
/// @return its index, or -1 (then @p why says why)
int newest_copy(Table& t, Status& why)
{
   int best = -1;
   why = Status::Empty;
   const int copies = copy_count();
   if (copies == 0)
   {
      why = Status::NoRoom;
      return -1;
   }
   // every slot, also with one copy: a slot an earlier two-copy build wrote may hold the newest image
   const int slots = slot_bytes() == 0 ? 1 : kMaxCopies;
   for (int k = 0; k < slots && copy_base(k) + kHeaderBytes <= hal::save::capacity(); ++k)
   {
      Table c{};
      const Status st = check_copy(k, c);
      if (st == Status::Ok)
      {
         if (best < 0 || newer(c.sequence, t.sequence))
         {
            best = k;
            t = c;
         }
      }
      else if (st != Status::Empty)
      {
         why = st;   // a copy that is there but unusable says more than an empty one
      }
   }
   return best;
}

// -- Bag -----------------------------------------------------------------------------------------------------------

/// The bag's entries: per pocket its filled slots, as item key and count.
/// @param s    the sink
/// @param bag  the bag, or nullptr: none
void encode_bag(Sink& s, const inventory::Bag* bag)
{
   using inventory::kInventory;
   uint32_t n = 0;
   for (uint8_t p = 0; bag != nullptr && p < kInventory.pocket_count; ++p)
   {
      n += bag->used[p];
   }
   put_le(s, n, kU16Bytes);
   for (uint8_t p = 0; bag != nullptr && p < kInventory.pocket_count; ++p)
   {
      const inventory::PocketData& pd = kInventory.pockets[p];
      for (uint16_t k = 0; k < bag->used[p]; ++k)
      {
         const inventory::BagSlot& slot = bag->slots[pd.first + k];
         const uint32_t key = kInventory.item_keys != nullptr && slot.item < kInventory.item_count
                            ? kInventory.item_keys[slot.item] : 0;
         put_le(s, key, kU32Bytes);
         put_le(s, slot.count, kU16Bytes);
      }
   }
}

/// Loads bag entries into an empty bag (dropping unknown items and entries without room, clamping counts).
/// @param s      the source over the section
/// @param bag    the bag, or nullptr: only read
/// @param notes  what migrating changed, or nullptr
void decode_bag(Source& s, inventory::Bag* bag, Notes* notes)
{
   using inventory::kInventory;
   if (bag != nullptr)
   {
      inventory::clear(*bag);
   }
   const uint32_t n = next_le(s, kU16Bytes);
   for (uint32_t k = 0; k < n; ++k)
   {
      const uint32_t key = next_le(s, kU32Bytes);
      uint32_t count = next_le(s, kU16Bytes);
      if (bag == nullptr)
      {
         continue;
      }
      const uint16_t item = inventory::item_of_key(key);
      const inventory::ItemInfo* in = inventory::info(item);
      const bool pocketed = in != nullptr && in->pocket < kInventory.pocket_count;
      const uint16_t most = pocketed ? kInventory.pockets[in->pocket].quantity_max : 0;
      bool clamped = false;
      if (count > most)
      {
         count = most;   // review4 finding 2: a lowered quantity_max
         clamped = true;
      }
      const bool added = count != 0 && inventory::add(*bag, item, count);
      if (notes != nullptr)
      {
         notes->dropped_items = static_cast<uint16_t>(notes->dropped_items + (added ? 0 : 1));
         notes->clamped_items = static_cast<uint16_t>(notes->clamped_items + (added && clamped ? 1 : 0));
      }
   }
}

// -- Party ---------------------------------------------------------------------------------------------------------

/// The party: its layout, count and members.
/// @param s      the sink
/// @param party  the party, or nullptr: none
void encode_party(Sink& s, const monster::Party* party)
{
   const uint8_t count = party != nullptr && party->count <= monster::kLayout.party_size ? party->count : 0;
   put_le(s, monster::kLayout.record_hash, kU32Bytes);
   put_le(s, monster::kLayout.record_bytes, kU16Bytes);
   put(s, count);
   for (uint8_t m = 0; m < count; ++m)
   {
      put_block(s, party->members[m].bytes, monster::kLayout.record_bytes);
   }
}

/// Whether a section's stored layout hash is this build's record shape (C7: the record hash, which leaves the party
/// size out; the full layout hash too, as images before C7 stored it).
/// @param layout  the stored hash
/// @return true if the records load as they are
bool same_records(uint32_t layout)
{
   return layout == monster::kLayout.record_hash || layout == monster::kLayout.hash;
}

/// Loads (or only checks) a party (C7: of any party size; refused only for more members than the build holds).
/// @param s      the source over the section
/// @param party  the party, or nullptr: only checked
/// @return Ok, or OtherShape (another record layout, more members than the build's party holds)
Status decode_party(Source& s, monster::Party* party)
{
   const uint32_t layout = next_le(s, kU32Bytes);
   const uint32_t bytes = next_le(s, kU16Bytes);
   const uint8_t count = next(s);
   if (!same_records(layout) || bytes != monster::kLayout.record_bytes ||
       count > monster::kLayout.party_size || count > monster::kMaxPartySize)
   {
      return Status::OtherShape;
   }
   if (party != nullptr)
   {
      *party = monster::Party{};
      party->count = count;
   }
   for (uint8_t m = 0; m < count; ++m)
   {
      for (uint32_t b = 0; b < bytes; ++b)
      {
         const uint8_t v = next(s);
         if (party != nullptr)
         {
            party->members[m].bytes[b] = v;
         }
      }
   }
   return Status::Ok;
}

// -- Game (N3) -----------------------------------------------------------------------------------------------------

/// The game's block: its version, size and bytes (none: version 0, no bytes).
/// @param s  the sink
/// @param g  the block
void encode_game(Sink& s, const GameBlock& g)
{
   const uint32_t size = g.bytes != nullptr ? g.size : 0;
   put_le(s, g.version, kU16Bytes);
   put_le(s, size, kU32Bytes);
   if (size != 0)
   {
      put_block(s, g.bytes, size);
   }
}

/// Loads (or only checks) the game's block.
/// @param s      the source over the section
/// @param g      the destination
/// @param store  copy it (else only checked)
/// @return Ok, or OtherShape (another version or size than the destination's)
Status decode_game(Source& s, const GameBlock& g, bool store)
{
   const uint32_t version = next_le(s, kU16Bytes);
   const uint32_t size = next_le(s, kU32Bytes);
   if (version != g.version || size != g.size)
   {
      return Status::OtherShape;
   }
   for (uint32_t b = 0; b < size; ++b)
   {
      const uint8_t v = next(s);
      if (store)
      {
         g.bytes[b] = v;
      }
   }
   return Status::Ok;
}

// -- Nursery (B2) --------------------------------------------------------------------------------------------------

/// The build's nursery slots (its data only: a program that saves needs no nursery code).
/// @return 0 to combine::kMaxNurserySlots
uint8_t nursery_slots()
{
   const uint8_t n = combine::kCombine.nursery.slots;
   return n < combine::kMaxNurserySlots ? n : static_cast<uint8_t>(combine::kMaxNurserySlots);
}

/// The nursery: record hash and bytes, its slots, the used bits, whether a result waits, then each used slot's
/// level, gained experience and record (slot order), then the waiting result's record.
/// @param s  the sink
/// @param n  the nursery, or nullptr: empty
void encode_nursery(Sink& s, const combine::nursery::State* n)
{
   const uint8_t slots = nursery_slots();
   const uint8_t used = n != nullptr ? static_cast<uint8_t>(n->used & ((1u << slots) - 1u)) : uint8_t{0};
   const bool pending = n != nullptr && n->pending != 0;
   put_le(s, monster::kLayout.record_hash, kU32Bytes);
   put_le(s, monster::kLayout.record_bytes, kU16Bytes);
   put(s, slots);
   put(s, used);
   put(s, pending ? 1 : 0);
   for (uint8_t k = 0; k < slots; ++k)
   {
      if ((used >> k & 1u) == 0)
      {
         continue;
      }
      put(s, n->level[k]);
      put_le(s, n->gained[k], kU32Bytes);
      put_block(s, n->records[k].bytes, monster::kLayout.record_bytes);
   }
   if (pending)
   {
      put_block(s, n->result.bytes, monster::kLayout.record_bytes);
   }
}

/// Loads (or only checks) the nursery: a record keeps its slot when the build has it, else it goes into the first
/// free slot (Notes::moved_records); refused for another record layout or more records than the build's slots.
/// @param s      the source over the section
/// @param n      the nursery, or nullptr: only checked
/// @param notes  what migrating changed, or nullptr
/// @return Ok or OtherShape
Status decode_nursery(Source& s, combine::nursery::State* n, Notes* notes)
{
   const uint32_t layout = next_le(s, kU32Bytes);
   const uint32_t bytes = next_le(s, kU16Bytes);
   const uint8_t old_slots = next(s);
   const uint8_t used = next(s);
   const bool pending = next(s) != 0;
   const uint8_t slots = nursery_slots();
   uint8_t records = 0;
   for (uint8_t k = 0; k < old_slots && k < kByteBits; ++k)
   {
      records = static_cast<uint8_t>(records + ((used >> k & 1u) != 0 ? 1 : 0));
   }
   if (!same_records(layout) || bytes != monster::kLayout.record_bytes || old_slots > kByteBits ||
       records > slots || (pending && slots == 0))
   {
      return Status::OtherShape;
   }
   if (n != nullptr)
   {
      *n = combine::nursery::State{};
      // the records that keep their slot first, so a moved one cannot take it
      n->used = static_cast<uint8_t>(used & ((1u << slots) - 1u) & ((1u << old_slots) - 1u));
   }
   uint8_t free_from = 0;
   for (uint8_t k = 0; k < old_slots; ++k)
   {
      if ((used >> k & 1u) == 0)
      {
         continue;
      }
      uint8_t to = k;
      if (n != nullptr && k >= slots)
      {
         while ((n->used >> free_from & 1u) != 0)
         {
            ++free_from;   // records <= slots: a free slot is left
         }
         to = free_from;
         n->used = static_cast<uint8_t>(n->used | 1u << to);
         if (notes != nullptr)
         {
            ++notes->moved_records;
         }
      }
      const uint8_t level = next(s);
      const uint32_t gained = next_le(s, kU32Bytes);
      if (n != nullptr)
      {
         n->level[to] = level;
         n->gained[to] = gained;
      }
      for (uint32_t b = 0; b < bytes; ++b)
      {
         const uint8_t v = next(s);
         if (n != nullptr)
         {
            n->records[to].bytes[b] = v;
         }
      }
   }
   if (pending)
   {
      for (uint32_t b = 0; b < bytes; ++b)
      {
         const uint8_t v = next(s);
         if (n != nullptr)
         {
            n->result.bytes[b] = v;
         }
      }
      if (n != nullptr)
      {
         n->pending = 1;
      }
   }
   return Status::Ok;
}

// -- Boxes ---------------------------------------------------------------------------------------------------------

/// Whether a slot's used bit is set in used bits.
/// @param used  the bits
/// @param slot  slot index
/// @return true if set
bool bit_set(const uint8_t* used, uint32_t slot)
{
   return (used[slot / kByteBits] >> (slot % kByteBits) & 1u) != 0;
}

/// Sets a slot's used bit.
/// @param used  the bits
/// @param slot  slot index
void set_bit(uint8_t* used, uint32_t slot)
{
   used[slot / kByteBits] = static_cast<uint8_t>(used[slot / kByteBits] | 1u << (slot % kByteBits));
}

/// The storage boxes: layout, geometry, current box, used bits, the used slots' records.
/// @param s   the sink
/// @param st  the storage, or nullptr: empty boxes
void encode_boxes(Sink& s, const storage::Storage* st)
{
   const uint32_t slots = static_cast<uint32_t>(storage::slot_count());
   put_le(s, monster::kLayout.record_hash, kU32Bytes);
   put_le(s, monster::kLayout.record_bytes, kU16Bytes);
   put(s, storage::kStorage.box_count);
   put_le(s, storage::kStorage.box_size, kU16Bytes);
   put(s, st != nullptr ? st->current : 0);
   for (uint32_t b = 0; b < used_bytes(slots); ++b)
   {
      uint8_t v = 0;
      for (uint32_t k = 0; st != nullptr && k < kByteBits && b * kByteBits + k < slots; ++k)
      {
         v = static_cast<uint8_t>(v | (storage::used(*st, static_cast<int>(b * kByteBits + k)) ? 1u << k : 0u));
      }
      put(s, v);
   }
   for (uint32_t k = 0; st != nullptr && k < slots; ++k)
   {
      if (storage::used(*st, static_cast<int>(k)))
      {
         put_block(s, st->bytes + k * monster::kLayout.record_bytes, monster::kLayout.record_bytes);
      }
   }
}

/// Loads (or only checks) storage boxes: a record keeps its box and place when the build has them, else it goes
/// into the first free slot.
/// @param s      the source over the section
/// @param st     the storage, or nullptr: only checked
/// @param notes  what migrating changed, or nullptr
/// @return Ok, or OtherShape (another layout, more records than the build's slots, a geometry past this platform)
Status decode_boxes(Source& s, storage::Storage* st, Notes* notes)
{
   const uint32_t layout = next_le(s, kU32Bytes);
   const uint32_t bytes = next_le(s, kU16Bytes);
   const uint32_t old_boxes = next(s);
   const uint32_t old_size = next_le(s, kU16Bytes);
   const uint8_t current = next(s);
   const uint32_t old_slots = old_boxes * old_size;
   if (!same_records(layout) || bytes != monster::kLayout.record_bytes ||
       used_bytes(old_slots) > static_cast<uint32_t>(storage::kUsedBytes))
   {
      return Status::OtherShape;
   }
   // the old used bits (a small array: this platform's most slots)
   uint8_t old_used[storage::kUsedBytes] = {};
   for (uint32_t b = 0; b < used_bytes(old_slots); ++b)
   {
      old_used[b] = next(s);
   }
   const uint32_t boxes = storage::kStorage.box_count;
   const uint32_t size = storage::kStorage.box_size;
   const uint32_t slots = boxes * size;
   uint32_t records = 0;
   for (uint32_t k = 0; k < old_slots; ++k)
   {
      records += bit_set(old_used, k) ? 1 : 0;
   }
   if (records > slots)
   {
      return Status::OtherShape;
   }
   if (st != nullptr)
   {
      storage::clear(*st);
      st->current = static_cast<uint8_t>(boxes == 0 ? 0 : (current < boxes ? current : boxes - 1));
      // the records that keep their place first, so a moved one cannot take it
      for (uint32_t k = 0; k < old_slots; ++k)
      {
         if (bit_set(old_used, k) && k / old_size < boxes && k % old_size < size)
         {
            set_bit(st->used, k / old_size * size + k % old_size);
         }
      }
   }
   uint32_t free_from = 0;
   for (uint32_t k = 0; k < old_slots; ++k)
   {
      if (!bit_set(old_used, k))
      {
         continue;
      }
      uint32_t to = 0;
      if (st != nullptr)
      {
         const bool kept = k / old_size < boxes && k % old_size < size;
         if (kept)
         {
            to = k / old_size * size + k % old_size;
         }
         else
         {
            while (bit_set(st->used, free_from))
            {
               ++free_from;   // records <= slots: a free slot is left
            }
            to = free_from;
            set_bit(st->used, to);
            if (notes != nullptr)
            {
               ++notes->moved_records;
            }
         }
      }
      for (uint32_t b = 0; b < bytes; ++b)
      {
         const uint8_t v = next(s);
         if (st != nullptr)
         {
            st->bytes[to * bytes + b] = v;
         }
      }
   }
   return Status::Ok;
}

// -- Script --------------------------------------------------------------------------------------------------------

/// A sub-bank's storage in this build.
/// @param idx    sub-bank index
/// @param bytes  its size (out)
/// @return its storage (nullptr when empty)
uint8_t* sub_bank(int idx, uint32_t& bytes)
{
   const script::Bank& bank = script::kBank;
   if (idx < script::kSavedLifetimes)
   {
      bytes = bank.flag_bytes[idx];
      return bank.flag_storage[idx];
   }
   if (idx < script::kNameSubBank)
   {
      bytes = bank.var_bytes[idx - script::kSavedLifetimes];
      return bank.var_storage[idx - script::kSavedLifetimes];
   }
   bytes = bank.name_bytes;
   return bank.names;
}

/// The script section: every non-empty sub-bank with its shape hash.
/// @param s  the sink
void encode_script(Sink& s)
{
   uint8_t count = 0;
   for (int k = 0; k < script::kSaveSubBanks; ++k)
   {
      uint32_t bytes = 0;
      count = static_cast<uint8_t>(count + (sub_bank(k, bytes) != nullptr && bytes != 0 ? 1 : 0));
   }
   put(s, count);
   for (int k = 0; k < script::kSaveSubBanks; ++k)
   {
      uint32_t bytes = 0;
      const uint8_t* p = sub_bank(k, bytes);
      if (p == nullptr || bytes == 0)
      {
         continue;
      }
      put(s, static_cast<uint8_t>(k));
      put_le(s, script::kBank.save_hashes[k], kU32Bytes);
      put_le(s, bytes, kU16Bytes);
      put_block(s, p, bytes);
   }
}

/// The recorded remap of an older sub-bank shape.
/// @param idx   sub-bank index
/// @param hash  the older shape's hash
/// @return the remap, or nullptr
const script::SaveRemap* remap_of(int idx, uint32_t hash)
{
   const script::Bank& bank = script::kBank;
   for (uint16_t k = 0; k < bank.save_remap_count; ++k)
   {
      if (bank.save_remaps[k].sub_bank == idx && bank.save_remaps[k].from_hash == hash)
      {
         return &bank.save_remaps[k];
      }
   }
   return nullptr;
}

/// Sets a flag's bit in its bank (the banks were cleared before).
/// @param flag  FlagId
void set_flag_bit(uint16_t flag)
{
   const script::Bank& bank = script::kBank;
   for (int l = 0; l < script::kNumLifetimes; ++l)
   {
      if (flag >= bank.flag_first[l] && flag < bank.flag_first[l + 1] && bank.flag_storage[l] != nullptr)
      {
         set_bit(bank.flag_storage[l], static_cast<uint32_t>(flag - bank.flag_first[l]));
         return;
      }
   }
}

/// Writes a variable's moved value: sign-extended from its old width when it was signed, clamped to its range,
/// stored at its width.
/// @param var     VarId
/// @param raw     the old bytes' value
/// @param width   the old width in bytes
/// @param signed_old  whether the old variable was signed
void put_var(uint16_t var, uint32_t raw, uint8_t width, bool signed_old)
{
   const script::Bank& bank = script::kBank;
   if (var >= bank.var_count)
   {
      return;
   }
   const script::VarInfo& vi = bank.vars[var];
   uint8_t* store = bank.var_storage[static_cast<int>(vi.lifetime)];
   if (store == nullptr)
   {
      return;
   }
   const uint32_t bits = width * kByteBits;
   int64_t v = raw;
   if (signed_old && bits < kU32Bits && (raw >> (bits - 1) & 1u) != 0)
   {
      v -= static_cast<int64_t>(1) << bits;
   }
   else if (signed_old && bits == kU32Bits)
   {
      v = static_cast<int32_t>(raw);
   }
   v = v < vi.min ? vi.min : (v > vi.max ? vi.max : v);
   put_le(store + vi.offset, static_cast<uint32_t>(v), vi.width);
}

/// Counts one dropped entry that held something (C7: Notes::dropped_flags / dropped_vars).
/// @param counter  the count
/// @param held     whether the entry held a set flag, a non-zero value or a non-empty name
void count_dropped(uint16_t& counter, bool held)
{
   counter = static_cast<uint16_t>(counter + (held ? 1 : 0));
}

/// Moves an older sub-bank's entries by its remap.
/// @param s      the source over the sub-bank's bytes
/// @param idx    sub-bank index
/// @param r      the remap
/// @param bytes  the sub-bank's stored bytes
/// @param notes  what migrating changed (dropped entries that held something), or nullptr
void apply_remap(Source& s, int idx, const script::SaveRemap& r, uint32_t bytes, Notes* notes)
{
   uint16_t dropped_flags = 0;
   uint16_t dropped_vars = 0;
   const script::SaveMove* moves = script::kBank.save_moves + r.first;
   uint32_t at = 0;   // bytes of the sub-bank read so far
   uint8_t byte = 0;
   for (uint16_t k = 0; k < r.count; ++k)
   {
      const script::SaveMove& m = moves[k];
      if (idx < script::kSavedLifetimes)
      {
         // a flag: its old bit
         const uint32_t want = m.from / kByteBits;
         while (at <= want && at < bytes)
         {
            byte = next(s);
            ++at;
         }
         const bool set = at == want + 1 && (byte >> (m.from % kByteBits) & 1u) != 0;
         if (m.to != script::kSaveDropped && set)
         {
            set_flag_bit(m.to);
         }
         count_dropped(dropped_flags, m.to == script::kSaveDropped && set);
         continue;
      }
      // a variable or a name slot: its old bytes
      if (m.from < at)
      {
         continue;   // overlaps the entry before (a damaged history): skipped
      }
      skip(s, m.from - at);
      at = m.from;
      if (idx < script::kNameSubBank)
      {
         const uint32_t raw = next_le(s, m.from_size);
         at += m.from_size;
         if (m.to != script::kSaveDropped)
         {
            put_var(m.to, raw, m.from_size, m.to_size != 0);
         }
         count_dropped(dropped_vars, m.to == script::kSaveDropped && raw != 0);
         continue;
      }
      // a name slot: its length byte and codes, truncated to the new capacity
      uint32_t name_bytes = 0;
      uint8_t* names = sub_bank(script::kNameSubBank, name_bytes);
      const uint8_t length = next(s);
      const bool kept = m.to != script::kSaveDropped && names != nullptr && m.to + 1u + m.to_size <= name_bytes;
      const uint8_t keep = length < m.to_size ? length : m.to_size;
      if (kept)
      {
         names[m.to] = keep;
      }
      for (uint32_t c = 0; c < m.from_size; ++c)
      {
         const uint8_t code = next(s);
         if (kept && c < keep)
         {
            names[m.to + 1 + c] = code;
         }
      }
      at += 1u + m.from_size;
      count_dropped(dropped_vars, m.to == script::kSaveDropped && length != 0);
   }
   skip(s, bytes > at ? bytes - at : 0);
   if (notes != nullptr)
   {
      notes->dropped_flags = static_cast<uint16_t>(notes->dropped_flags + dropped_flags);
      notes->dropped_vars = static_cast<uint16_t>(notes->dropped_vars + dropped_vars);
   }
}

/// Stops the timed field effects that do not survive a save (F1, capsule/script/field_effects.h): their implicit
/// variables are cleared in the bank's storage (the load writes no script state through the VM).
void forget_unsaved_field_effects()
{
   const script::Bank& bank = script::kBank;
   for (uint8_t k = 0; k < bank.field_effect_count && bank.field_effects != nullptr; ++k)
   {
      const script::FieldEffectInfo& fx = bank.field_effects[k];
      if ((fx.flags & script::kFieldEffectPersists) != 0 || fx.var >= bank.var_count)
      {
         continue;
      }
      const script::VarInfo& vi = bank.vars[fx.var];
      uint8_t* p = bank.var_storage[static_cast<int>(vi.lifetime)];
      for (uint8_t b = 0; p != nullptr && b < vi.width; ++b)
      {
         p[vi.offset + b] = 0;
      }
   }
}

/// Loads (or only checks) the script section.
/// @param s      the source over the section
/// @param store  false: only checked
/// @param notes  what migrating changed, or nullptr
/// @return Ok, or OtherShape (a permanent or name sub-bank of an unrecorded shape)
Status decode_script(Source& s, bool store, Notes* notes)
{
   if (store)
   {
      // a sub-bank the save does not hold (or resets) starts clear
      for (int k = 0; k < script::kSaveSubBanks; ++k)
      {
         uint32_t bytes = 0;
         uint8_t* p = sub_bank(k, bytes);
         for (uint32_t b = 0; p != nullptr && b < bytes; ++b)
         {
            p[b] = 0;
         }
      }
   }
   const uint8_t count = next(s);
   for (uint8_t n = 0; n < count; ++n)
   {
      const int idx = next(s);
      const uint32_t hash = next_le(s, kU32Bytes);
      const uint32_t bytes = next_le(s, kU16Bytes);
      uint32_t have = 0;
      uint8_t* p = idx < script::kSaveSubBanks ? sub_bank(idx, have) : nullptr;
      if (idx >= script::kSaveSubBanks)
      {
         skip(s, bytes);   // a sub-bank this build does not know: not kept
         continue;
      }
      if (hash == script::kBank.save_hashes[idx] && bytes == have)
      {
         for (uint32_t b = 0; b < bytes; ++b)
         {
            const uint8_t v = next(s);
            if (store)
            {
               p[b] = v;
            }
         }
         continue;
      }
      const script::SaveRemap* r = remap_of(idx, hash);
      if (r != nullptr && r->from_bytes == bytes)
      {
         if (store)
         {
            apply_remap(s, idx, *r, bytes, notes);
            if (notes != nullptr)
            {
               ++notes->migrated_banks;
            }
         }
         else
         {
            skip(s, bytes);
         }
         continue;
      }
      // no remap: a transient lifetime's sub-bank resets, any other refuses the save
      const bool transient = idx != script::kNameSubBank &&
                             idx % script::kSavedLifetimes != static_cast<int>(script::Lifetime::Permanent);
      if (!transient)
      {
         return Status::OtherShape;
      }
      skip(s, bytes);
      if (store && notes != nullptr)
      {
         ++notes->reset_banks;
      }
   }
   if (store)
   {
      forget_unsaved_field_effects();
   }
   return Status::Ok;
}

// -- sections ------------------------------------------------------------------------------------------------------

/// The version this build writes and reads of a section.
/// @param id  section
/// @return its version
uint8_t version_of(Section id)
{
   switch (id)
   {
   case Section::Bag:
      return kBagVersion;
   case Section::Party:
      return kPartyVersion;
   case Section::Boxes:
      return kBoxesVersion;
   case Section::Nursery:
      return kNurseryVersion;
   case Section::Game:
      return kGameVersion;
   default:
      return kScriptVersion;
   }
}

/// Encodes a section from its source (nullptr sources write their empty section).
/// @param s    the sink
/// @param id   section
/// @param src  the sources
void encode(Sink& s, Section id, const Sources& src)
{
   switch (id)
   {
   case Section::Bag:
      encode_bag(s, src.bag);
      break;
   case Section::Party:
      encode_party(s, src.party);
      break;
   case Section::Boxes:
      encode_boxes(s, src.storage);
      break;
   case Section::Nursery:
      encode_nursery(s, src.nursery);
      break;
   case Section::Game:
      encode_game(s, src.game);
      break;
   default:
      encode_script(s);
      break;
   }
}

/// Whether a write has a source for a section, or a read a destination (the script bank always is one).
/// @param id   section
/// @param src  the sources
/// @return true if it has
bool has_source(Section id, const Sources& src)
{
   switch (id)
   {
   case Section::Bag:
      return src.bag != nullptr;
   case Section::Party:
      return src.party != nullptr;
   case Section::Boxes:
      return src.storage != nullptr;
   case Section::Nursery:
      return src.nursery != nullptr;
   case Section::Game:
      return src.game.bytes != nullptr;
   default:
      return true;
   }
}

/// Decodes a section (store: into its destination; else only checks that it loads).
/// @param s      the source over the section
/// @param id     section
/// @param dst    the destinations
/// @param store  false: only checked
/// @param notes  what migrating changed, or nullptr
/// @return Ok or why it does not load
Status decode(Source& s, Section id, const Sources& dst, bool store, Notes* notes)
{
   switch (id)
   {
   case Section::Bag:
      decode_bag(s, store ? dst.bag : nullptr, notes);
      return Status::Ok;
   case Section::Party:
      return decode_party(s, store ? dst.party : nullptr);
   case Section::Boxes:
      return decode_boxes(s, store ? dst.storage : nullptr, store ? notes : nullptr);
   case Section::Nursery:
      return decode_nursery(s, store ? dst.nursery : nullptr, store ? notes : nullptr);
   case Section::Game:
      return decode_game(s, dst.game, store);
   default:
      return decode_script(s, store, notes);
   }
}

/// Empties a destination whose section the image does not have.
/// @param id   section
/// @param dst  the destinations
void load_empty(Section id, const Sources& dst)
{
   switch (id)
   {
   case Section::Bag:
      inventory::clear(*dst.bag);
      break;
   case Section::Party:
      *dst.party = monster::Party{};
      break;
   case Section::Boxes:
      storage::clear(*dst.storage);
      break;
   case Section::Nursery:
      *dst.nursery = combine::nursery::State{};
      break;
   case Section::Game:
      for (uint32_t b = 0; b < dst.game.size; ++b)
      {
         dst.game.bytes[b] = 0;   // N3: no block in the image: zeros (the game tells an empty block apart)
      }
      break;
   default:
   {
      Source none{};
      open(none, nullptr, 0, 0);
      decode_script(none, true, nullptr);   // every sub-bank clear (no bytes: the count reads 0)
      break;
   }
   }
}

/// Copies a section's bytes from another copy (a write without its source), checking their checksum again.
/// @param out  the sink (emitting)
/// @param e    the section's entry in the newest copy
/// @return false on a medium error or bytes that changed
bool copy_through(Sink& out, const Entry& e)
{
   Source in{};
   open(in, nullptr, e.offset, e.size);
   for (uint32_t k = 0; k < e.size; ++k)
   {
      put(out, next(in));
   }
   return in.ok && in.hash == e.checksum;
}

/// N3: reads a section's bytes from the newest copy into RAM, checking their checksum (a single-copy write keeps a
/// section without its source this way: its bytes are where the write erases).
/// @param e   the section's entry in the newest copy
/// @param to  e.size bytes
/// @return false on a medium error or bytes that changed
bool stage_section(const Entry& e, uint8_t* to)
{
   Source in{};
   open(in, nullptr, e.offset, e.size);
   for (uint32_t k = 0; k < e.size; ++k)
   {
      to[k] = next(in);
   }
   return in.ok && in.hash == e.checksum;
}

} // namespace

uint32_t section_size(Section s)
{
   using inventory::kInventory;
   switch (s)
   {
   case Section::Bag:
      return kBagHeadBytes + kInventory.slot_count * kBagEntryBytes;
   case Section::Party:
      return kPartyHeadBytes + static_cast<uint32_t>(monster::kLayout.party_size) * monster::kLayout.record_bytes;
   case Section::Boxes:
   {
      const uint32_t slots = static_cast<uint32_t>(storage::slot_count());
      return slots == 0 ? 0 : kBoxesHeadBytes + used_bytes(slots) + slots * monster::kLayout.record_bytes;
   }
   case Section::Nursery:
   {
      // B2: the head, then per slot its level, gained experience and record, then the result's record
      const uint32_t slots = nursery_slots();
      const uint32_t rb = monster::kLayout.record_bytes;
      return slots == 0 ? 0 : kNurseryHeadBytes + slots * (kNurserySlotBytes + rb) + rb;
   }
   case Section::Game:
   {
      const uint32_t bytes = game_block_bytes();
      return bytes == 0 ? 0 : kGameHeadBytes + bytes;
   }
   case Section::Script:
   {
      uint32_t n = 0;
      for (int k = 0; k < script::kSaveSubBanks; ++k)
      {
         uint32_t bytes = 0;
         n += sub_bank(k, bytes) != nullptr && bytes != 0 ? kSubBankHeadBytes + bytes : 0;
      }
      return n == 0 ? 0 : kScriptHeadBytes + n;
   }
   default:
      return 0;
   }
}

uint32_t image_size()
{
   uint32_t n = kHeaderBytes;
   for (int k = 0; k < kSectionCount; ++k)
   {
      const uint32_t size = section_size(static_cast<Section>(k));
      n += size == 0 ? 0 : kSectionEntryBytes + size;
   }
   return n;
}

int copy_count()
{
   const uint32_t one = copy_bytes();
   const uint32_t slot = slot_bytes();
   if (slot != 0 && one <= slot)
   {
      return kMaxCopies;
   }
   return one <= hal::save::capacity() ? 1 : 0;
}

Status write(const Sources& src)
{
   const hal::save::Session session(true);
   const int copies = copy_count();
   if (!session.ok)
   {
      return Status::MediumError;
   }
   if (copies == 0 || image_size() > kSaveBudget)
   {
      return Status::NoRoom;
   }
   Table last{};
   Status why = Status::Empty;
   const int newest = newest_copy(last, why);
   // the older copy (the other one), or copy 0 on an empty medium; the sequence counts on from the newest
   const int k = copies == 1 || newest < 0 ? 0 : 1 - newest;
   // pass 1: each section's size and checksum (a section without its source: the newest copy's, unchanged)
   Entry entries[kSectionCount] = {};
   bool copied[kSectionCount] = {};
   uint8_t count = 0;
   uint32_t total = kHeaderBytes;
   for (int s = 0; s < kSectionCount; ++s)
   {
      const Section id = static_cast<Section>(s);
      const Entry* old = newest >= 0 ? find(last, id) : nullptr;
      Entry& e = entries[count];
      if (!has_source(id, src) && old != nullptr)
      {
         e = *old;
         copied[count] = true;
      }
      else if (section_size(id) != 0)
      {
         Sink h{};
         open(h, false, 0);
         encode(h, id, src);
         e = Entry{static_cast<uint8_t>(s), version_of(id), h.size, h.hash, 0};
      }
      else
      {
         continue;   // the build has no such section and no copy keeps one
      }
      total += kSectionEntryBytes + e.size;
      ++count;
   }
   if (total > copy_room(k, true))
   {
      return Status::NoRoom;   // a section copied from an older build's image is larger than the slot
   }
   // N3: one copy only (the image is larger than a slot, so it spans both): the sections kept from the newest copy
   // may be where this write erases, so they are read into the built-in stage first (more than it holds: NoRoom;
   // give their sources)
#if defined(CAPSULE_SAVE_STAGE_ON_STACK)
   uint8_t own[kStageBytes];
#else
   uint8_t* own = g_stage;
#endif
   uint8_t* staged[kSectionCount] = {};
   if (copies == 1)
   {
      uint32_t used = 0;
      for (uint8_t e = 0; e < count; ++e)
      {
         if (!copied[e])
         {
            continue;
         }
         if (entries[e].size > kStageBytes - used)
         {
            return Status::NoRoom;
         }
         staged[e] = own + used;
         used += entries[e].size;
         if (!stage_section(entries[e], staged[e]))
         {
            return Status::MediumError;
         }
      }
   }
   // the header and table, with their checksum
   uint8_t head[kHeadRoom] = {};
   put_le(head + kMagicOffset, kMagic, kU32Bytes);
   put_le(head + kVersionOffset, kVersion, kU16Bytes);
   head[kCountOffset] = count;
   // the next sequence (it wraps from 0xFFFFFFFF to 0, and newer() still ranks it newest)
   put_le(head + kSequenceOffset, newest < 0 ? 1u : last.sequence + 1u, kU32Bytes);
   put_le(head + kBytesOffset, total - kHeaderBytes, kU32Bytes);
   for (uint8_t e = 0; e < count; ++e)
   {
      uint8_t* p = head + kHeaderBytes + e * kSectionEntryBytes;
      p[kEntryId] = entries[e].id;
      p[kEntryVersion] = entries[e].version;
      put_le(p + kEntrySize, entries[e].size, kU32Bytes);
      put_le(p + kEntryChecksum, entries[e].checksum, kU32Bytes);
   }
   const uint32_t head_bytes = kHeaderBytes + count * kSectionEntryBytes;
   uint32_t h = monster::kFnvOffsetBasis;
   for (uint32_t b = 0; b < head_bytes; ++b)
   {
      if (b < kChecksumOffset || b >= kChecksumOffset + kChecksumBytes)
      {
         h = fnv(h, head[b]);
      }
   }
   put_le(head + kChecksumOffset, h, kU32Bytes);
   // pass 2: the bytes (the erase units the image takes; its slot starts on one)
   const uint32_t base = copy_base(k);
   if (!hal::save::erase(base, round_up(total, unit_bytes())))
   {
      return Status::MediumError;
   }
   Sink w{};
   open(w, true, base);
   put_block(w, head, head_bytes);
   bool same = true;
   for (uint8_t e = 0; e < count; ++e)
   {
      if (staged[e] != nullptr)
      {
         put_block(w, staged[e], entries[e].size);   // N3: kept through the built-in stage (one copy)
      }
      else if (copied[e])
      {
         same = copy_through(w, entries[e]) && same;
      }
      else
      {
         encode(w, static_cast<Section>(entries[e].id), src);
      }
   }
   flush(w);
   // C7: durable before it counts as written (the desktop file is synced to its disk)
   return w.ok && same && hal::save::commit() ? Status::Ok : Status::MediumError;
}

Status read(const Sources& dst, Notes* notes, const Stage& stage)
{
   const hal::save::Session session(false);
   if (!session.ok)
   {
      return Status::MediumError;
   }
   if (dst.storage != nullptr && !storage::fits(*dst.storage))
   {
      return Status::NoRoom;
   }
   Table t{};
   Status why = Status::Empty;
   if (newest_copy(t, why) < 0)
   {
      return why;
   }
#if defined(CAPSULE_SAVE_STAGE_ON_STACK)
   uint8_t own[kStageBytes];
#else
   uint8_t* own = g_stage;
#endif
   // C7: every section this read loads gets its place in a stage, back to back: the built-in stage first, then
   // the caller's; a section that fits neither refuses the read before anything is read again
   uint8_t* home[kMaxSections] = {};
   uint32_t own_used = 0;
   uint32_t extra_used = 0;
   for (uint8_t e = 0; e < t.count; ++e)
   {
      const Entry& en = t.entries[e];
      if (en.id >= kSectionCount || !has_source(static_cast<Section>(en.id), dst))
      {
         continue;
      }
      if (en.version != version_of(static_cast<Section>(en.id)))
      {
         return Status::OtherVersion;
      }
      if (en.size <= kStageBytes - own_used)
      {
         home[e] = own + own_used;
         own_used += en.size;
      }
      else if (stage.bytes != nullptr && en.size <= stage.size - extra_used)
      {
         home[e] = stage.bytes + extra_used;
         extra_used += en.size;
      }
      else
      {
         return Status::NoRoom;   // a section larger than the stages (GBA: pass a Stage for the boxes)
      }
   }
   // each section staged and its checksum checked again (the medium may have changed since newest_copy())
   for (uint8_t e = 0; e < t.count; ++e)
   {
      if (home[e] == nullptr)
      {
         continue;
      }
      const Entry& en = t.entries[e];
      if (!read_medium(en.offset, home[e], en.size))
      {
         return Status::MediumError;
      }
      Source s{};
      open(s, home[e], 0, en.size);
      skip(s, en.size);
      if (s.hash != en.checksum)
      {
         return Status::MediumError;   // the medium changed since it was checked
      }
   }
   // every staged section must load into this build (the decoders' answers) before anything changes
   for (uint8_t e = 0; e < t.count; ++e)
   {
      if (home[e] == nullptr)
      {
         continue;
      }
      Source s{};
      open(s, home[e], 0, t.entries[e].size);
      const Status st = decode(s, static_cast<Section>(t.entries[e].id), dst, false, nullptr);
      if (st != Status::Ok)
      {
         return st;
      }
   }
   // commit: the staged bytes land (nothing reads the medium now); a destination whose section the image lacks
   // loads empty
   if (notes != nullptr)
   {
      *notes = Notes{};
   }
   for (int id = 0; id < kSectionCount; ++id)
   {
      const Section sec = static_cast<Section>(id);
      if (!has_source(sec, dst))
      {
         continue;
      }
      const Entry* en = find(t, sec);
      if (en == nullptr)
      {
         load_empty(sec, dst);
         continue;
      }
      const uint8_t e = static_cast<uint8_t>(en - t.entries);
      Source s{};
      open(s, home[e], 0, en->size);
      const Status st = decode(s, sec, dst, true, notes);
      if (st != Status::Ok)
      {
         return st;   // not reached: the same bytes loaded in the check above
      }
   }
   return Status::Ok;
}

uint32_t slot_bytes()
{
   return hal::save::capacity() / kMaxCopies / unit_bytes() * unit_bytes();
}

Status newest(uint32_t& out)
{
   const hal::save::Session session(false);
   Table t{};
   Status why = Status::Empty;
   if (!session.ok)
   {
      return Status::MediumError;
   }
   if (newest_copy(t, why) < 0)
   {
      return why;
   }
   out = t.sequence;
   return Status::Ok;
}

} // namespace capsule::save

/// The engine's default: no game block (a game with one defines game_block_bytes()).
/// @return 0
CAPSULE_WEAK uint32_t capsule::save::game_block_bytes()
{
   return 0;
}
