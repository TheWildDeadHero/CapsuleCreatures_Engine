/// @file
/// Tests for the save image's versioned sections (C6, review4 findings 1, 2, 5, 6; the probe of review4 as
/// regressions) on the recruit fixture (tests/data/recruit: pockets goods / orbs / key, two boxes of three) with the
/// save fixture's script bank (tests/data/save: four flags, four variables, a name slot, a save history of three
/// recorded shapes), over this file's flash-like medium (4 KB sectors, AND programming, a write cut after a number
/// of operations, reads that fail):
///   - the current image (tests/data/save/current.bin, written by tools/save_layout.py) loads and the engine writes
///     it back byte for byte;
///   - older shapes migrate: items reordered / removed / added since, a pocket added, a lowered quantity_max
///     (clamped), a box geometry changed (records keep their box and place, the others move), script banks of
///     recorded shapes (moved by name), a transient bank of an unrecorded shape (reset);
///   - what cannot migrate is refused and changes nothing: more boxed records than slots, a permanent bank of an
///     unrecorded shape, another record layout;
///   - a write without a source copies its section from the newest copy;
///   - writes cut at every 97th medium operation always leave the old or the new image (never a damaged one) and the
///     next write lands; a read that fails while a section is staged changes nothing, a read that fails once is
///     retried;
///   - C7 (the final review's probe as regressions): copies lie in fixed slots (an older build's copy of another
///     size is found in the other slot), parties of another party size load (grown, shrunk) or are refused (more
///     members), a narrowed variable range clamps, dropped flags and variables are counted, a read whose later
///     section fails to stage or whose medium changes after the check changes nothing, sequences wrap, a write
///     commits. Built a second time with a small built-in stage (CAPSULE_SAVE_STAGE_BYTES: save_stage_tests): a
///     section past it needs the caller's Stage, without one the read refuses (NoRoom) and changes nothing.
#include "capsule/hal/save.h"
#include "capsule/inventory/bag.h"
#include "capsule/monster/monster.h"
#include "capsule/save/save.h"
#include "capsule/script/data.h"
#include "capsule/storage/storage.h"

#include "battle_ids.h"
#include "save_script_ids.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace capsule::hal::system
{
/// Test build: report and abort.
/// @param expr  failed condition
/// @param file  file
/// @param line  line
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   std::printf("ASSERT %s:%d: %s\n", file, line, expr);
   std::abort();
}
} // namespace capsule::hal::system

namespace
{

/// @name The test medium
///@{
constexpr uint32_t kMediumBytes = 0x10000;   ///< 64 KB, like the GBA's flash.
constexpr uint32_t kSector = 0x1000;         ///< Its erase unit.
constexpr uint8_t kErased = 0xFF;            ///< An erased byte.
constexpr long kUnlimited = -1;              ///< No cut / no failing read.
///@}

uint8_t g_medium[kMediumBytes];       ///< The medium's bytes.
long g_ops_left = kUnlimited;         ///< Erase / program operations until the power is cut.
bool g_cut = false;                   ///< The power is cut (every later operation fails).
long g_reads_ok = kUnlimited;         ///< Reads that succeed before failing ones.
long g_reads_failing = 0;             ///< Reads that fail after them.
long g_reads = 0;                     ///< Reads so far.
long g_flip_at = kUnlimited;          ///< A medium offset whose byte changes on a read covering it, or none.
int g_flip_on = 0;                    ///< The read covering it that changes it (1: the first).
int g_flip_hits = 0;                  ///< Reads that covered it so far.
int g_commits = 0;                    ///< commit() calls.

/// One erase or program operation: false once the power is cut.
/// @return whether it runs
bool tick()
{
   if (g_cut)
   {
      return false;
   }
   if (g_ops_left == 0)
   {
      g_cut = true;
      return false;
   }
   if (g_ops_left > 0)
   {
      --g_ops_left;
   }
   return true;
}

} // namespace

namespace capsule::hal::save
{

bool begin(bool)
{
   return true;
}

void end()
{
}

uint32_t capacity()
{
   return kMediumBytes;
}

uint32_t erase_unit()
{
   return kSector;
}

bool read(uint32_t offset, uint8_t* dst, uint32_t size)
{
   ++g_reads;
   if (g_reads_ok == 0 && g_reads_failing > 0)
   {
      --g_reads_failing;
      return false;
   }
   if (g_reads_ok > 0)
   {
      --g_reads_ok;
   }
   if (offset > kMediumBytes || size > kMediumBytes - offset)
   {
      return false;
   }
   const bool covers = g_flip_at >= 0 && static_cast<long>(offset) <= g_flip_at &&
                       g_flip_at < static_cast<long>(offset + size);
   if (covers && ++g_flip_hits == g_flip_on)
   {
      g_medium[g_flip_at] = static_cast<uint8_t>(~g_medium[g_flip_at]);   // the medium changes under the read
   }
   std::memcpy(dst, g_medium + offset, size);
   return true;
}

bool erase(uint32_t offset, uint32_t size)
{
   for (uint32_t at = offset - offset % kSector; at < offset + size; at += kSector)
   {
      if (!tick())
      {
         // the cut leaves half the sector erased
         std::memset(g_medium + at, kErased, kSector / 2);
         return false;
      }
      std::memset(g_medium + at, kErased, kSector);
   }
   return true;
}

bool program(uint32_t offset, const uint8_t* src, uint32_t size)
{
   for (uint32_t i = 0; i < size; ++i)
   {
      if (!tick())
      {
         return false;
      }
      g_medium[offset + i] &= src[i];   // flash: programming only clears bits
   }
   return true;
}

bool commit()
{
   ++g_commits;
   return !g_cut;
}

} // namespace capsule::hal::save

namespace
{

using namespace capsule;
using namespace battle_ids;
using save::Status;

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

/// @name The fixtures' numbers (tests/test_save_migration.py writes the images)
///@{
constexpr uint32_t kGoodsMax = 5;      ///< The goods pocket's quantity_max.
constexpr uint32_t kRecordSlot = 4;    ///< current.bin's boxed record.
constexpr uint32_t kCoins = 5;         ///< current.bin's coins.
constexpr uint32_t kOldCoins = 700;    ///< script.bin's coins.
constexpr uint32_t kWalkedMax = 500;   ///< walked's range top (script.bin holds 900).
constexpr uint32_t kOldMood = 7;       ///< script.bin's mood.
constexpr uint8_t kHeroCodes = 4;      ///< The name slot's capacity (script.bin held six codes).
constexpr int kCutStep = 97;           ///< Operations between two cut points.
constexpr long kCutRange = 40000;      ///< Cut points up to this many operations.
constexpr size_t kFileBytes = 0x4000;  ///< Room for one image file.
constexpr uint32_t kSlot = kMediumBytes / 2;   ///< Copy 1's slot (C7: half the medium, whatever the image's size).
constexpr uint32_t kOldCopyOne = 0x1000;       ///< Where a C6 build put copy 1 (this image rounded to a sector).
constexpr uint32_t kMagicWord = 0x56534343u;   ///< "CCSV".
constexpr uint8_t kNewerParty = 0x91;  ///< newer.bin's first member.
constexpr uint8_t kWrapOld = 0x61;     ///< wrap_old.bin's member (sequence 0xFFFFFFFF).
constexpr uint8_t kWrapNew = 0x62;     ///< wrap_new.bin's member (sequence 0).
constexpr uint32_t kRangeCoins = 3;    ///< range.bin's coins.
constexpr uint32_t kRangeMood = 4;     ///< range.bin's mood.
constexpr int kPartySize = 3;          ///< The recruit fixture's party.
///@}

inventory::Bag g_bag{};                                             ///< The bag.
monster::Party g_party{};                                           ///< The party.
storage::Buffer<STORAGE_BYTES, STORAGE_USED_BYTES> g_box_bytes{};   ///< The boxes' bytes.
storage::Storage g_boxes = storage::view(g_box_bytes);              ///< The boxes.
const char* g_folder = nullptr;                                     ///< tests/data/save.
#if defined(CAPSULE_SAVE_STAGE_BYTES)
/// The room lent to every read past the small built-in stage.
constexpr uint32_t kLentBytes = 0x400;
uint8_t g_lent_bytes[kLentBytes];                                   ///< Its bytes.
save::Stage g_lent{g_lent_bytes, kLentBytes};                       ///< The Stage passed.
#else
save::Stage g_lent{nullptr, 0};                                     ///< No Stage (the built-in one holds all).
#endif

/// Every source.
/// @return bag, party and boxes
save::Sources all()
{
   return save::Sources{&g_bag, &g_party, &g_boxes, nullptr};
}

/// Reads every source (with the lent Stage of the small-stage build).
/// @param notes  what migrating changed, or nullptr
/// @return read()'s answer
save::Status read_all(save::Notes* notes = nullptr)
{
   return save::read(all(), notes, g_lent);
}

/// The medium erased, every cut and failure off.
void wipe()
{
   std::memset(g_medium, kErased, sizeof(g_medium));
   g_ops_left = kUnlimited;
   g_cut = false;
   g_reads_ok = kUnlimited;
   g_reads_failing = 0;
   g_flip_at = kUnlimited;
   g_flip_hits = 0;
}

/// Reads an image file onto the medium.
/// @param name    file name in the save fixture
/// @param offset  where it goes
/// @return its size (0 when it does not open)
size_t put_image_at(const char* name, uint32_t offset);

/// Reads an image file into copy 0 of an erased medium.
/// @param name  file name in the save fixture
/// @param out   its bytes (or nullptr)
/// @return its size (0 when it does not open)
size_t put_image(const char* name, uint8_t* out = nullptr)
{
   char path[512];
   std::snprintf(path, sizeof(path), "%s/%s", g_folder, name);
   std::FILE* f = std::fopen(path, "rb");
   CHECK(f != nullptr);
   if (f == nullptr)
   {
      return 0;
   }
   uint8_t data[kFileBytes] = {};
   const size_t n = std::fread(data, 1, sizeof(data), f);
   std::fclose(f);
   wipe();
   std::memcpy(g_medium, data, n);
   if (out != nullptr)
   {
      std::memcpy(out, data, n);
   }
   return n;
}

size_t put_image_at(const char* name, uint32_t offset)
{
   char path[512];
   std::snprintf(path, sizeof(path), "%s/%s", g_folder, name);
   std::FILE* f = std::fopen(path, "rb");
   CHECK(f != nullptr);
   if (f == nullptr)
   {
      return 0;
   }
   uint8_t data[kFileBytes] = {};
   const size_t n = std::fread(data, 1, sizeof(data), f);
   std::fclose(f);
   std::memcpy(g_medium + offset, data, n);
   return n;
}

/// The little-endian word at a medium offset.
/// @param offset  its first byte
/// @return the word
uint32_t medium_word(uint32_t offset)
{
   uint32_t v = 0;
   for (uint32_t k = 0; k < 4; ++k)
   {
      v |= static_cast<uint32_t>(g_medium[offset + k]) << (8 * k);
   }
   return v;
}

/// Whether a flag is set in the script bank's storage.
/// @param f  FlagId
/// @return its bit
bool flag_bit(script::FlagId f)
{
   const script::Bank& bank = script::kBank;
   const uint16_t id = static_cast<uint16_t>(f);
   for (int l = 0; l < script::kNumLifetimes; ++l)
   {
      if (id >= bank.flag_first[l] && id < bank.flag_first[l + 1])
      {
         const uint32_t bit = static_cast<uint32_t>(id - bank.flag_first[l]);
         return (bank.flag_storage[l][bit / 8] >> (bit % 8) & 1u) != 0;
      }
   }
   return false;
}

/// A variable's value in the script bank's storage.
/// @param v  VarId
/// @return its value
uint32_t var_value(script::VarId v)
{
   const script::VarInfo& vi = script::kBank.vars[static_cast<uint16_t>(v)];
   const uint8_t* p = script::kBank.var_storage[static_cast<int>(vi.lifetime)] + vi.offset;
   uint32_t value = 0;
   for (int k = 0; k < vi.width; ++k)
   {
      value |= static_cast<uint32_t>(p[k]) << (8 * k);
   }
   return value;
}

/// Whether every byte of a boxed record is one value.
/// @param slot   storage slot
/// @param value  the byte
/// @return true if so
bool boxed_as(int slot, uint8_t value)
{
   monster::Record r{};
   if (!storage::read(g_boxes, slot, r))
   {
      return false;
   }
   bool same = true;
   for (int b = 0; b < monster::kLayout.record_bytes; ++b)
   {
      same = same && r.bytes[b] == value;
   }
   return same;
}

/// The current shapes: every section loads, and the engine writes the Python image back byte for byte.
void test_current()
{
   uint8_t file[kFileBytes] = {};
   const size_t n = put_image("current.bin", file);
   save::Notes notes{};
   CHECK(n > 0 && n <= save::image_size() && read_all(&notes) == Status::Ok);
   CHECK(inventory::count(g_bag, ITEM_SALVE) == 2 && inventory::count(g_bag, ITEM_ETHER) == 1);
   CHECK(inventory::count(g_bag, ITEM_BADGE) == 1 && inventory::valid(g_bag));
   CHECK(g_party.count == 1 && g_party.members[0].bytes[0] == 1);
   CHECK(storage::count(g_boxes) == 1 && storage::used(g_boxes, kRecordSlot) && g_boxes.current == 1);
   CHECK(flag_bit(script_ids::FLAG_LIT) && flag_bit(script_ids::FLAG_MET) && !flag_bit(script_ids::FLAG_FRESH));
   CHECK(var_value(script_ids::VAR_COINS) == kCoins);
   CHECK(notes.dropped_items == 0 && notes.clamped_items == 0 && notes.moved_records == 0);
   CHECK(notes.migrated_banks == 0 && notes.reset_banks == 0);
   wipe();
   CHECK(save::write(all()) == Status::Ok);
   bool same = true;
   for (size_t k = 0; k < n; ++k)
   {
      same = same && g_medium[k] == file[k];
   }
   CHECK(same);
}

/// Older bag, box and script shapes load into this build.
void test_migrations()
{
   save::Notes notes{};
   // items reordered, Beacon added since, a Relic removed since: by key, not by id
   put_image("items.bin");
   CHECK(read_all(&notes) == Status::Ok);
   CHECK(inventory::count(g_bag, ITEM_ETHER) == 3 && inventory::count(g_bag, ITEM_SALVE) == 2);
   CHECK(inventory::count(g_bag, ITEM_BADGE) == 1 && inventory::count(g_bag, ITEM_BEACON) == 0);
   CHECK(notes.dropped_items == 1 && inventory::valid(g_bag));
   // the orbs pocket added since: Lure and Snare go to it
   put_image("pockets.bin");
   CHECK(read_all(&notes) == Status::Ok && notes.dropped_items == 0);
   int used = 0;
   const inventory::BagSlot* orbs = inventory::pocket_slots(g_bag, POCKET_ORBS, used);
   CHECK(used == 2 && orbs[0].item == ITEM_LURE && orbs[0].count == 2 && orbs[1].item == ITEM_SNARE);
   CHECK(inventory::count(g_bag, ITEM_SALVE) == 1 && inventory::valid(g_bag));
   // review4 finding 2: 12 Salves of a pocket that held 20 clamp to its 5, and the bag stays valid
   put_image("quantity.bin");
   CHECK(read_all(&notes) == Status::Ok && notes.clamped_items == 1);
   CHECK(inventory::count(g_bag, ITEM_SALVE) == kGoodsMax && inventory::valid(g_bag));
   CHECK(inventory::room_for(g_bag, ITEM_SALVE) == 0 && !inventory::add(g_bag, ITEM_SALVE, 1));
   // three boxes of two into two of three: slots 0 and 3 keep box and place (0, 4), slot 5 moves to slot 1
   put_image("boxes.bin");
   CHECK(read_all(&notes) == Status::Ok && notes.moved_records == 1);
   CHECK(storage::count(g_boxes) == 3 && boxed_as(0, 1) && boxed_as(4, 3) && boxed_as(1, 5));
   CHECK(g_boxes.current == 1 && storage::valid(g_boxes));
   CHECK(g_party.count == 0 && inventory::count(g_bag, ITEM_SALVE) == 0);   // absent sections load empty
   // the script banks: recorded shapes move by name, the map bank (no remap) resets
   put_image("script.bin");
   CHECK(read_all(&notes) == Status::Ok && notes.migrated_banks == 3 && notes.reset_banks == 1);
   // C7: the dropped gone flag (set) and retired variable (200) are counted
   CHECK(notes.dropped_flags == 1 && notes.dropped_vars == 1);
   CHECK(flag_bit(script_ids::FLAG_MET) && flag_bit(script_ids::FLAG_LIT) && !flag_bit(script_ids::FLAG_FRESH));
   CHECK(flag_bit(script_ids::FLAG_SEEN));   // the daily flags' shape is today's: copied
   CHECK(var_value(script_ids::VAR_COINS) == kOldCoins && var_value(script_ids::VAR_WALKED) == kWalkedMax);
   CHECK(var_value(script_ids::VAR_MOOD) == kOldMood && var_value(script_ids::VAR_DOOR) == 0);
   const uint8_t* hero = script::kBank.names;
   CHECK(hero[0] == kHeroCodes && hero[1] == 'A' && hero[kHeroCodes] == 'D');
}

/// What cannot migrate is refused and nothing changes.
void test_refusals()
{
   inventory::clear(g_bag);
   CHECK(inventory::add(g_bag, ITEM_ETHER, 1));
   storage::clear(g_boxes);
   g_party = monster::Party{};
   g_party.count = 1;
   g_party.members[0].bytes[0] = 0x77;
   const char* const refused[] = {"boxes_full.bin", "script_refused.bin", "party_layout.bin"};
   for (const char* name : refused)
   {
      put_image(name);
      CHECK(read_all() == Status::OtherShape);
      CHECK(inventory::count(g_bag, ITEM_ETHER) == 1 && inventory::count(g_bag, ITEM_SALVE) == 0);
      CHECK(g_party.count == 1 && g_party.members[0].bytes[0] == 0x77 && storage::count(g_boxes) == 0);
   }
   // a storage view too small for the build's boxes is refused before anything is read
   storage::Storage small = g_boxes;
   small.capacity = 0;
   CHECK(save::read(save::Sources{&g_bag, &g_party, &small, nullptr}) == Status::NoRoom);
}

/// review4 finding 5: a write without a source copies its section from the newest copy.
void test_copy_through()
{
   wipe();
   g_party = monster::Party{};
   g_party.count = 1;
   g_party.members[0].bytes[0] = 0x31;
   CHECK(save::write(all()) == Status::Ok);
   g_party.members[0].bytes[0] = 0x32;
   inventory::clear(g_bag);
   CHECK(inventory::add(g_bag, ITEM_SALVE, 1));
   CHECK(save::write(save::Sources{&g_bag, nullptr, nullptr, nullptr}) == Status::Ok);
   g_party = monster::Party{};
   CHECK(read_all() == Status::Ok && g_party.count == 1 && g_party.members[0].bytes[0] == 0x31);
   CHECK(inventory::count(g_bag, ITEM_SALVE) == 1);
   uint32_t seq = 0;
   CHECK(save::newest(seq) == Status::Ok && seq == 2);
}

/// The review4 probe's torn writes as a regression: cut the third write at every 97th operation; the newest
/// readable image is the second or the third, and a further write lands.
void test_torn_writes()
{
   wipe();
   g_party = monster::Party{};
   g_party.count = 1;
   g_party.members[0].bytes[0] = 0x11;
   CHECK(save::write(all()) == Status::Ok);
   g_party.members[0].bytes[0] = 0x22;
   CHECK(save::write(all()) == Status::Ok);
   static uint8_t snap[kMediumBytes];
   std::memcpy(snap, g_medium, sizeof(snap));
   int bad = 0;
   int fresh = 0;
   int old = 0;
   for (long cut = 0; cut < kCutRange; cut += kCutStep)
   {
      std::memcpy(g_medium, snap, sizeof(snap));
      g_cut = false;
      g_ops_left = cut;
      g_party.members[0].bytes[0] = 0x33;
      (void)save::write(all());
      g_cut = false;
      g_ops_left = kUnlimited;
      g_party.members[0].bytes[0] = 0;
      const Status st = read_all();
      const uint8_t v = g_party.members[0].bytes[0];
      bad += st != Status::Ok || (v != 0x33 && v != 0x22) ? 1 : 0;
      fresh += st == Status::Ok && v == 0x33 ? 1 : 0;
      old += st == Status::Ok && v == 0x22 ? 1 : 0;
      g_party.members[0].bytes[0] = 0x44;
      CHECK(save::write(all()) == Status::Ok);
      g_party.members[0].bytes[0] = 0;
      CHECK(read_all() == Status::Ok && g_party.members[0].bytes[0] == 0x44);
   }
   std::printf("torn writes: %d bad, %d new, %d old\n", bad, fresh, old);
   CHECK(bad == 0 && fresh > 0 && old > 0);
}

/// review4 finding 6: a read failing while the first section is staged changes nothing; one failing read is retried.
void test_failing_reads()
{
   wipe();
   g_party = monster::Party{};
   g_party.count = 1;
   g_party.members[0].bytes[0] = 0x55;
   inventory::clear(g_bag);
   CHECK(inventory::add(g_bag, ITEM_SALVE, 2) && save::write(all()) == Status::Ok);
   // how many medium reads a whole read takes; the last ones stage the sections (one read each)
   g_reads = 0;
   CHECK(read_all() == Status::Ok);
   const long total = g_reads;
   int sections = 0;
   sections += save::section_size(save::Section::Bag) != 0 ? 1 : 0;
   sections += save::section_size(save::Section::Party) != 0 ? 1 : 0;
   sections += save::section_size(save::Section::Boxes) != 0 ? 1 : 0;
   sections += save::section_size(save::Section::Script) != 0 ? 1 : 0;
   // the first staging read fails every try: MediumError, nothing changed (the probe's case)
   g_party.members[0].bytes[0] = 0x77;
   inventory::clear(g_bag);
   g_reads_ok = total - sections;
   g_reads_failing = kCutRange;
   CHECK(read_all() == Status::MediumError);
   CHECK(g_party.members[0].bytes[0] == 0x77 && inventory::count(g_bag, ITEM_SALVE) == 0);
   // one failing read: the retry reads it
   g_reads_ok = total - sections;
   g_reads_failing = 1;
   CHECK(read_all() == Status::Ok && g_party.members[0].bytes[0] == 0x55);
   CHECK(inventory::count(g_bag, ITEM_SALVE) == 2);
   g_reads_ok = kUnlimited;
   g_reads_failing = 0;
}

/// C7 (probe C): copies lie in fixed slots. Two writes fill both slots (copy 1 at half the medium, never at the
/// image's rounded size); an older build's copy of another size and a higher sequence in the other slot is the
/// one read; the next write goes to the older slot.
void test_fixed_slots()
{
   CHECK(save::slot_bytes() == kSlot);
   wipe();
   g_party = monster::Party{};
   g_party.count = 1;
   g_party.members[0].bytes[0] = 0x11;
   CHECK(save::write(all()) == Status::Ok);
   g_party.members[0].bytes[0] = 0x22;
   CHECK(save::write(all()) == Status::Ok);
   CHECK(medium_word(0) == kMagicWord && medium_word(kSlot) == kMagicWord && medium_word(kOldCopyOne) != kMagicWord);
   // current.bin (sequence 1) in slot 0, newer.bin (sequence 2, larger) in slot 1
   const size_t small = put_image("current.bin");
   const size_t large = put_image_at("newer.bin", kSlot);
   CHECK(small > 0 && large > small);
   uint32_t seq = 0;
   CHECK(save::newest(seq) == Status::Ok && seq == 2);
   CHECK(read_all() == Status::Ok && g_party.count == 2 && g_party.members[0].bytes[0] == kNewerParty);
   CHECK(inventory::count(g_bag, ITEM_SALVE) == 4 && storage::count(g_boxes) == 3);
   // and the other way round: the larger newer copy in slot 0
   put_image("newer.bin");
   put_image_at("current.bin", kSlot);
   CHECK(read_all() == Status::Ok && g_party.members[0].bytes[0] == kNewerParty);
   // the next write replaces the older copy (slot 1 here) and counts on
   CHECK(save::write(all()) == Status::Ok && save::newest(seq) == Status::Ok && seq == 3);
   CHECK(medium_word(kSlot) == kMagicWord && medium_word(0) == kMagicWord);
}

/// C7 (review: party_size in the layout hash refused the whole save): the party size is not the record's shape.
void test_party_sizes()
{
   // a party of two (that build's party size) loads into today's three
   put_image("party_grow.bin");
   CHECK(read_all() == Status::Ok && g_party.count == 2);
   CHECK(g_party.members[0].bytes[0] == 0x21 && g_party.members[1].bytes[0] == 0x22);
   // a party of five holding three loads (with its boxes), one holding four is refused and nothing changes
   put_image("party_shrink.bin");
   CHECK(read_all() == Status::Ok && g_party.count == kPartySize && g_party.members[2].bytes[0] == 0x53);
   CHECK(storage::count(g_boxes) == 1 && boxed_as(2, 0x5B));
   put_image("party_over.bin");
   CHECK(read_all() == Status::OtherShape && g_party.count == kPartySize && storage::count(g_boxes) == 1);
}

/// C7 (review: a narrowed range was copied unclamped): walked saved as 900 under the recorded range 0..900 loads
/// as today's top, 500.
void test_narrowed_range()
{
   put_image("range.bin");
   save::Notes notes{};
   CHECK(read_all(&notes) == Status::Ok && notes.migrated_banks == 1);
   CHECK(var_value(script_ids::VAR_WALKED) == kWalkedMax && var_value(script_ids::VAR_COINS) == kRangeCoins);
   CHECK(var_value(script_ids::VAR_MOOD) == kRangeMood);
}

/// C7 (probe D and E): a read is all or nothing. The last section's staging read failing every retry, or the
/// medium changing after the check, leaves every destination as it was.
void test_all_or_nothing()
{
   wipe();
   g_party = monster::Party{};
   g_party.count = 1;
   g_party.members[0].bytes[0] = 0x55;
   inventory::clear(g_bag);
   CHECK(inventory::add(g_bag, ITEM_SALVE, 2) && save::write(all()) == Status::Ok);
   g_reads = 0;
   CHECK(read_all() == Status::Ok);
   const long total = g_reads;
   // every staging read works but the last (the script section's): nothing lands, the bag neither
   g_party.members[0].bytes[0] = 0x77;
   inventory::clear(g_bag);
   g_reads_ok = total - 1;
   g_reads_failing = kCutRange;
   CHECK(read_all() == Status::MediumError);
   CHECK(g_party.members[0].bytes[0] == 0x77 && inventory::count(g_bag, ITEM_SALVE) == 0);
   g_reads_ok = kUnlimited;
   g_reads_failing = 0;
   // a party byte changes after newest() checked the copy: the staged checksum fails, nothing lands
   uint32_t at = 0;
   while (at + 2 < kSlot && !(g_medium[at] == 0x55 && g_medium[at + 1] == 0 && g_medium[at + 2] == 0))
   {
      ++at;
   }
   CHECK(at + 2 < kSlot);
   g_flip_at = static_cast<long>(at);
   g_flip_hits = 0;
   g_flip_on = 2;   // the check pass reads it once, the staging read second
   CHECK(read_all() == Status::MediumError && g_flip_hits >= 2);
   CHECK(g_party.members[0].bytes[0] == 0x77 && inventory::count(g_bag, ITEM_SALVE) == 0);
   g_flip_at = kUnlimited;
}

/// C7: sequences compare by serial-number arithmetic: 0 is newer than 0xFFFFFFFF, and a write after it counts on.
void test_sequence_wrap()
{
   put_image("wrap_old.bin");
   put_image_at("wrap_new.bin", kSlot);
   uint32_t seq = 1;
   CHECK(save::newest(seq) == Status::Ok && seq == 0);
   CHECK(read_all() == Status::Ok && g_party.members[0].bytes[0] == kWrapNew);
   put_image("wrap_new.bin");
   put_image_at("wrap_old.bin", kSlot);
   CHECK(read_all() == Status::Ok && g_party.members[0].bytes[0] == kWrapNew);
   // the write goes to the older copy (slot 1, 0xFFFFFFFF) as sequence 1
   g_party.members[0].bytes[0] = 0x63;
   CHECK(save::write(all()) == Status::Ok && save::newest(seq) == Status::Ok && seq == 1);
   CHECK(medium_word(kSlot + 8) == 1 && medium_word(8) == 0);
}

/// C7: a write commits its bytes (desktop: synced to the disk), and a commit that fails fails the write.
void test_commit()
{
   wipe();
   g_commits = 0;
   CHECK(save::write(all()) == Status::Ok && g_commits == 1);
}

#if defined(CAPSULE_SAVE_STAGE_BYTES)
/// The small built-in stage: the boxes do not fit it; without a Stage the read refuses (NoRoom) and changes
/// nothing, with one it loads.
void test_small_stage()
{
   put_image("current.bin");
   inventory::clear(g_bag);
   g_party = monster::Party{};
   g_party.members[0].bytes[0] = 0x77;
   CHECK(save::read(all()) == Status::NoRoom);
   CHECK(g_party.members[0].bytes[0] == 0x77 && inventory::count(g_bag, ITEM_SALVE) == 0);
   CHECK(save::read(all(), nullptr, save::Stage{g_lent_bytes, 1}) == Status::NoRoom);
   CHECK(read_all() == Status::Ok && inventory::count(g_bag, ITEM_SALVE) == 2 && storage::count(g_boxes) == 1);
   // the bag (it fits the built-in stage) and the script bank (always read): a Stage of the script section's size
   inventory::clear(g_bag);
   const save::Sources bag_only{&g_bag, nullptr, nullptr, nullptr};
   const save::Stage exact{g_lent_bytes, save::section_size(save::Section::Script)};
   CHECK(save::read(bag_only) == Status::NoRoom && inventory::count(g_bag, ITEM_SALVE) == 0);
   CHECK(save::read(bag_only, nullptr, exact) == Status::Ok && inventory::count(g_bag, ITEM_SALVE) == 2);
}
#endif

} // namespace

/// Runs the tests.
/// @param argc  2: the save fixture's folder follows
/// @param argv  arguments
/// @return 0 when every check passed
int main(int argc, char** argv)
{
   if (argc < 2)
   {
      std::printf("usage: save_tests <tests/data/save>\n");
      return 1;
   }
   g_folder = argv[1];
   CHECK(save::copy_count() == 2 && storage::fits(g_boxes));
   test_current();
   test_migrations();
   test_refusals();
   test_copy_through();
   test_torn_writes();
   test_failing_reads();
   test_fixed_slots();
   test_party_sizes();
   test_narrowed_range();
   test_all_or_nothing();
   test_sequence_wrap();
   test_commit();
#if defined(CAPSULE_SAVE_STAGE_BYTES)
   test_small_stage();
#endif
   std::printf("save tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
