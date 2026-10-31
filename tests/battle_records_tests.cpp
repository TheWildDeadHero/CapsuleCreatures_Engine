/// @file
/// Tests for the E13a record features against the battle data compiled from
/// tests/data/battle_progress/records.json: a wide seed drawn low half first (generation "order": "low_first"),
/// bit operators in derived expressions (a stored lean from the species field balance, a stored glyph from 2-bit
/// groups of the seed, a trait the layout does not store), an unstored custom role (marked: the owner id, the
/// secret and the seed's halves xor-ed, below 8) that record_value() computes whenever it is read, rederive()
/// and the transfer import that re-derives the stored derived roles a sender lacks (battle::import_record()).
#include "capsule/battle/battle.h"
#include "capsule/monster/transfer.h"

#include "battle_ids.h"

#include <cstdio>
#include <cstdlib>

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

using namespace capsule;
using namespace capsule::battle;
using namespace battle_ids;
using monster::Role;

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

/// Records each species makes in the derived checks.
constexpr uint32_t kRecords = 300;
/// A wider glyph role of a sending game (C5: the receiver narrows it) ...
constexpr uint8_t kWideGlyphBits = 16;
/// ... holding a value the receiver's 8 bits do not.
constexpr uint32_t kWideGlyph = 300;
/// Level of the records.
constexpr uint8_t kLevel = 5;
/// Mask of a 16-bit half.
constexpr uint32_t kHalfMask = 0xFFFFu;
/// The fixture's lean rule: the seed's low byte below the species' balance.
constexpr uint32_t kByteMask = 0xFFu;
/// The species' balance fields (records.json): Blob, Cube.
constexpr uint32_t kBalance[] = {200, 0};
/// The fixture's marked rule: the xor of owner id, secret and the seed's halves below this.
constexpr uint32_t kMarkedBelow = 8;
/// The fixture's glyph rule: 2-bit groups of the seed ...
constexpr uint32_t kTwoBits = 3u;
/// ... taken at these shifts (seed_hi >> 8, seed_hi, seed_lo >> 8, seed_lo) ...
constexpr int kByteShift = 8;
/// ... placed at these shifts in the glyph ...
constexpr int kGlyphShifts[] = {6, 4, 2, 0};
/// ... modulo this.
constexpr uint32_t kGlyphs = 28;
/// A value no derived rule gives (glyphs stop at 27): a stored derived role overwritten with it is stale.
constexpr uint32_t kStaleGlyph = 200;
/// Bytes of a transferred record.
constexpr int kTransferBytes = monster::kMaxTransferBytes;

/// The fixture's lean of a record (C++ of its derived expression).
/// @param seed     the seed
/// @param species  species index
/// @return 0 or 1
uint32_t lean_of(uint32_t seed, uint16_t species)
{
   return (seed & kByteMask) < kBalance[species] ? 1u : 0u;
}

/// The fixture's marked value.
/// @param owner   owner id
/// @param secret  the secret
/// @param seed    the seed
/// @return 0 or 1
uint32_t marked_of(uint32_t owner, uint32_t secret, uint32_t seed)
{
   return (owner ^ secret ^ (seed >> rng::kDrawBits) ^ (seed & kHalfMask)) < kMarkedBelow ? 1u : 0u;
}

/// The fixture's glyph.
/// @param seed  the seed
/// @return 0 to 27
uint32_t glyph_of(uint32_t seed)
{
   const uint32_t hi = seed >> rng::kDrawBits;
   const uint32_t lo = seed & kHalfMask;
   const uint32_t groups[] = {(hi >> kByteShift) & kTwoBits, hi & kTwoBits, (lo >> kByteShift) & kTwoBits,
                              lo & kTwoBits};
   uint32_t v = 0;
   for (int g = 0; g < 4; ++g)
   {
      v |= groups[g] << kGlyphShifts[g];
   }
   return v % kGlyphs;
}

/// make_monster(): the seed is two draws, the low half first, then owner id and secret one draw each; the stored
/// derived roles and the unstored one follow the bit-operator rules; the unstored one has no bits.
void test_generation()
{
   CHECK(!monster::has(UNSTORED_ROLE_MARKED));
   CHECK(monster::find_custom(monster::name_hash("marked")) == UNSTORED_ROLE_MARKED);
   CHECK(monster::width(UNSTORED_ROLE_MARKED) == 1 && monster::count(UNSTORED_ROLE_MARKED) == 1);
   CHECK(monster::has(ROLE_LEAN) && monster::has(ROLE_GLYPH));
   CHECK(monster::kLayout.custom_count == 3 && monster::kLayout.unstored_count == 1);
   for (uint16_t species = SPECIES_BLOB; species <= SPECIES_CUBE; ++species)
   {
      rng::Stream r{species};
      for (uint32_t n = 0; n < kRecords; ++n)
      {
         rng::Stream expect = r;
         const uint32_t low = rng::next16(expect);
         const uint32_t high = rng::next16(expect);
         const uint32_t owner = rng::next16(expect);
         const uint32_t secret = rng::next16(expect);
         monster::Record rec{};
         CHECK(make_monster(rec, species, kLevel, r));
         const uint32_t seed = high << rng::kDrawBits | low;
         CHECK(monster::get(rec, Role::Seed) == seed);
         CHECK(monster::get(rec, Role::OwnerId) == owner);
         CHECK(monster::get(rec, ROLE_SECRET) == secret);
         CHECK(monster::get(rec, ROLE_LEAN) == lean_of(seed, species));
         CHECK(monster::get(rec, ROLE_GLYPH) == glyph_of(seed));
         CHECK(monster::get(rec, UNSTORED_ROLE_MARKED) == 0);   // no bits
         CHECK(record_value(rec, UNSTORED_ROLE_MARKED) == marked_of(owner, secret, seed));
         CHECK(record_value(rec, monster::id_of(Role::Trait)) == (~low & 1u));
         r = expect;   // the stream went on by the same four draws
      }
   }
}

/// An unstored role never goes stale: changing what it is computed from changes it on the next read.
void test_unstored()
{
   rng::Stream r{1};
   monster::Record rec{};
   CHECK(make_monster(rec, SPECIES_BLOB, kLevel, r));
   const uint32_t seed = monster::get(rec, Role::Seed);
   const uint32_t owner = monster::get(rec, Role::OwnerId);
   // the secret that makes the xor 0: marked
   const uint32_t secret = owner ^ (seed >> rng::kDrawBits) ^ (seed & kHalfMask);
   monster::set(rec, ROLE_SECRET, 0, secret);
   CHECK(record_value(rec, UNSTORED_ROLE_MARKED) == 1);
   monster::set(rec, ROLE_SECRET, 0, secret ^ kMarkedBelow);   // xor 8: not marked
   CHECK(record_value(rec, UNSTORED_ROLE_MARKED) == 0);
   monster::set(rec, UNSTORED_ROLE_MARKED, 0, 1);   // writes go nowhere
   CHECK(record_value(rec, UNSTORED_ROLE_MARKED) == 0);
}

/// rederive(): stale stored derived roles follow the record again; kept roles stay.
void test_rederive()
{
   rng::Stream r{2};
   monster::Record rec{};
   CHECK(make_monster(rec, SPECIES_CUBE, kLevel, r));
   const uint32_t glyph = monster::get(rec, ROLE_GLYPH);
   const uint32_t lean = monster::get(rec, ROLE_LEAN);
   monster::set(rec, ROLE_GLYPH, 0, kStaleGlyph);
   monster::set(rec, ROLE_LEAN, 0, lean ^ 1u);
   monster::RoleSet keep = {0, 0};
   monster::add_role(keep, ROLE_LEAN);
   rederive(rec, &keep);
   CHECK(monster::get(rec, ROLE_GLYPH) == glyph);
   CHECK(monster::get(rec, ROLE_LEAN) == (lean ^ 1u));
   rederive(rec);
   CHECK(monster::get(rec, ROLE_LEAN) == lean);
}

/// Transfer v2: a record of this layout keeps its values (every role came with it, the stale glyph too); a record
/// from a layout without the glyph gets the glyph re-derived; the raw form brings every role.
void test_transfer()
{
   rng::Stream r{3};
   monster::Record rec{};
   CHECK(make_monster(rec, SPECIES_BLOB, kLevel, r));
   const uint32_t glyph = monster::get(rec, ROLE_GLYPH);
   monster::set(rec, ROLE_GLYPH, 0, kStaleGlyph);

   uint8_t bytes[kTransferBytes];
   monster::TransferResult out = monster::export_record(monster::kTransfer, rec.bytes, bytes, kTransferBytes,
                                                        monster::TransferForm::Canonical);
   CHECK(out.status == monster::TransferStatus::Ok);
   monster::Record got{};
   monster::RoleSet received = {0, 0};
   monster::TransferResult in = monster::import_record(monster::kTransfer, bytes, out.bytes, got.bytes,
                                                       static_cast<int>(sizeof(got.bytes)), &received);
   CHECK(in.status == monster::TransferStatus::Ok);
   CHECK(monster::has_role(received, ROLE_GLYPH) && monster::has_role(received, ROLE_SECRET));
   CHECK(monster::has_role(received, monster::id_of(Role::Seed)));
   CHECK(!monster::has_role(received, UNSTORED_ROLE_MARKED));   // unstored: never sent
   in = import_record(monster::kTransfer, bytes, out.bytes, got);
   CHECK(in.status == monster::TransferStatus::Ok);
   CHECK(monster::get(got, ROLE_GLYPH) == kStaleGlyph);   // the sender had it: kept

   // the sender's layout: this one without its last stored custom role (the glyph)
   monster::Layout sender_layout = monster::kLayout;
   sender_layout.custom_count = static_cast<uint8_t>(ROLE_GLYPH - monster::kFirstCustomRole);
   sender_layout.unstored_count = 0;
   uint8_t order[monster::kMaxCustomRoles];
   int n = 0;
   for (int k = 0; k < monster::kLayout.custom_count; ++k)
   {
      const uint8_t i = monster::kTransfer.custom_order[k];
      if (i < sender_layout.custom_count)
      {
         order[n++] = i;
      }
   }
   monster::TransferInfo sender = monster::kTransfer;
   sender.layout = &sender_layout;
   sender.custom_order = order;
   out = monster::export_record(sender, rec.bytes, bytes, kTransferBytes, monster::TransferForm::Canonical);
   CHECK(out.status == monster::TransferStatus::Ok);
   monster::Record moved{};
   in = import_record(monster::kTransfer, bytes, out.bytes, moved);
   CHECK(in.status == monster::TransferStatus::Ok);
   CHECK((in.flags & monster::kTransferDefaulted) != 0);
   CHECK(monster::get(moved, ROLE_GLYPH) == glyph);   // re-derived from the seed
   CHECK(monster::get(moved, Role::Seed) == monster::get(rec, Role::Seed));

   // C5: a sender whose glyph is wider: the receiver narrows it, so it is not received whole and is re-derived
   monster::RoleSlot slots[monster::kMaxCustomRoles];
   const int custom = monster::kLayout.custom_count + monster::kLayout.unstored_count;
   for (int k = 0; k < custom; ++k)
   {
      slots[k] = monster::kLayout.custom[k];
   }
   const int glyph_slot = ROLE_GLYPH - monster::kFirstCustomRole;
   slots[glyph_slot].width = kWideGlyphBits;   // the last stored role: nothing after it moves
   monster::Layout wide_layout = monster::kLayout;
   wide_layout.custom = slots;
   wide_layout.record_bytes = static_cast<uint16_t>(wide_layout.record_bytes + 1);
   monster::TransferInfo wide = monster::kTransfer;
   wide.layout = &wide_layout;
   monster::Record wide_rec = rec;
   monster::set(wide_layout, wide_rec.bytes, ROLE_GLYPH, 0, kWideGlyph);
   out = monster::export_record(wide, wide_rec.bytes, bytes, kTransferBytes, monster::TransferForm::Canonical);
   CHECK(out.status == monster::TransferStatus::Ok);
   received = {0, 0};
   in = monster::import_record(monster::kTransfer, bytes, out.bytes, got.bytes, static_cast<int>(sizeof(got.bytes)),
                               &received);
   CHECK(in.status == monster::TransferStatus::Ok && (in.flags & monster::kTransferNarrowed) != 0);
   CHECK(!monster::has_role(received, ROLE_GLYPH) && monster::has_role(received, ROLE_SECRET));
   monster::Record narrowed{};
   in = import_record(monster::kTransfer, bytes, out.bytes, narrowed);
   CHECK(in.status == monster::TransferStatus::Ok);
   CHECK(monster::get(narrowed, ROLE_GLYPH) == glyph);   // re-derived, not the narrowed 255

   // the raw form: every held role came with the record
   out = monster::export_record(monster::kTransfer, rec.bytes, bytes, kTransferBytes, monster::TransferForm::Raw);
   received = {0, 0};
   in = monster::import_record(monster::kTransfer, bytes, out.bytes, got.bytes, static_cast<int>(sizeof(got.bytes)),
                               &received);
   CHECK(in.status == monster::TransferStatus::Ok && (in.flags & monster::kTransferRaw) != 0);
   CHECK(monster::has_role(received, ROLE_GLYPH) && monster::has_role(received, ROLE_LEAN));
   CHECK(!monster::has_role(received, UNSTORED_ROLE_MARKED));
}

/// C9 (finding 12): a record whose role keyed like the eggs' (monster::kEggTransferKey) is set is an egg; this
/// build's layout has no egg role, so battle::import_record() refuses it (the structure has no
/// `transfer_eggs: "hatch"`) and leaves the receiving record as it was; with the role at 0 it is no egg.
void test_egg_transfer()
{
   rng::Stream r{3};
   monster::Record rec{};
   CHECK(make_monster(rec, SPECIES_BLOB, kLevel, r));
   // the sender: this layout with its secret role keyed as the egg role (custom roles sorted by key again)
   const int secret = ROLE_SECRET - monster::kFirstCustomRole;
   const int count = monster::kLayout.custom_count;
   uint32_t keys[monster::kMaxCustomRoles];
   uint8_t order[monster::kMaxCustomRoles];
   for (int k = 0; k < count; ++k)
   {
      keys[k] = k == secret ? monster::kEggTransferKey : monster::kTransfer.custom_keys[k];
      order[k] = static_cast<uint8_t>(k);
   }
   for (int k = 1; k < count; ++k)
   {
      // insertion sort by key (a handful of roles)
      for (int j = k; j > 0 && keys[order[j]] < keys[order[j - 1]]; --j)
      {
         const uint8_t s = order[j];
         order[j] = order[j - 1];
         order[j - 1] = s;
      }
   }
   monster::TransferInfo sender = monster::kTransfer;
   sender.custom_keys = keys;
   sender.custom_order = order;
   uint8_t bytes[kTransferBytes];
   monster::set(rec, ROLE_SECRET, 0, 1);
   monster::TransferResult out = monster::export_record(sender, rec.bytes, bytes, kTransferBytes,
                                                        monster::TransferForm::Canonical);
   CHECK(out.status == monster::TransferStatus::Ok);
   monster::Record got{};
   monster::TransferResult in = monster::import_record(monster::kTransfer, bytes, out.bytes, got.bytes,
                                                       static_cast<int>(sizeof(got.bytes)));
   CHECK(in.status == monster::TransferStatus::Ok && (in.flags & monster::kTransferEgg) != 0);
   CHECK(!combine::transfer_hatches_eggs());
   monster::Record kept{};
   in = import_record(monster::kTransfer, bytes, out.bytes, kept);
   CHECK(in.status == monster::TransferStatus::EggRefused);
   CHECK(monster::get(kept, Role::Seed) == 0);   // nothing written
   // the role at 0: no egg
   monster::set(rec, ROLE_SECRET, 0, 0);
   out = monster::export_record(sender, rec.bytes, bytes, kTransferBytes, monster::TransferForm::Canonical);
   in = import_record(monster::kTransfer, bytes, out.bytes, kept);
   CHECK(in.status == monster::TransferStatus::Ok && (in.flags & monster::kTransferEgg) == 0);
   CHECK(monster::get(kept, Role::Seed) == monster::get(rec, Role::Seed));
}

} // namespace

int main()
{
   test_generation();
   test_unstored();
   test_rederive();
   test_transfer();
   test_egg_transfer();
   std::printf("battle_records_tests: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
