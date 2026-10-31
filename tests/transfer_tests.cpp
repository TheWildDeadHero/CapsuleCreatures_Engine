/// @file
/// Tests for record transfer v2 (capsule/monster/transfer.h): the C++ export and import against the Python
/// mirror's test vectors (tools/record_transfer.py cases, tests/data/transfer/cases.json), byte for byte, and
/// the import rule on the fixtures: "six" (six stats, four move slots), "four" (four stats, two move slots, narrower
/// widths, other custom roles), "shuffled" (six's roles and stats in another order) and "twin" (the Starter
/// sample's layout); plus the build's own table (capsule::monster::kTransfer for the Starter sample's battle data).
#include "capsule/monster/monster.h"
#include "capsule/monster/transfer.h"

#include "transfer_cases.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

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
using namespace capsule::monster;
using transfer_cases::Case;
using transfer_cases::kCases;

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

constexpr int kCaseCount = static_cast<int>(sizeof(kCases) / sizeof(kCases[0]));   ///< Test vectors.

/// A test vector by name.
/// @param name  case name
/// @return the case (aborts when missing)
const Case& find_case(const char* name)
{
   for (const Case& c : kCases)
   {
      if (std::strcmp(c.name, name) == 0)
      {
         return c;
      }
   }
   std::printf("no case %s\n", name);
   std::abort();
}

/// Imports a case's canonical bytes into its receiver.
/// @param c       the case
/// @param record  receives the record
/// @return the result
TransferResult import_case(const Case& c, uint8_t* record)
{
   return import_record(*c.to, c.canonical, c.canonical_bytes, record, kMaxRecordBytes);
}

/// Every vector: the C++ export equals the mirror's (canonical and raw), the sizes agree, and the import gives the
/// mirror's record and flags.
void test_python_mirror()
{
   CHECK(kCaseCount >= 10);
   for (const Case& c : kCases)
   {
      uint8_t out[kMaxTransferBytes] = {};
      CHECK(c.source_bytes == c.from->layout->record_bytes);
      CHECK(transfer_size(*c.from, TransferForm::Canonical) == c.canonical_bytes);
      CHECK(transfer_size(*c.from, TransferForm::Raw) == c.raw_bytes);

      TransferResult r = export_record(*c.from, c.source, out, kMaxTransferBytes, TransferForm::Canonical);
      CHECK(r.status == TransferStatus::Ok && r.bytes == c.canonical_bytes);
      CHECK(std::memcmp(out, c.canonical, static_cast<size_t>(c.canonical_bytes)) == 0);
      r = export_record(*c.from, c.source, out, kMaxTransferBytes, TransferForm::Raw);
      CHECK(r.status == TransferStatus::Ok && r.bytes == c.raw_bytes && r.flags == kTransferRaw);
      CHECK(std::memcmp(out, c.raw, static_cast<size_t>(c.raw_bytes)) == 0);

      uint8_t record[kMaxRecordBytes] = {};
      r = import_case(c, record);
      CHECK(r.status == TransferStatus::Ok && r.flags == c.flags && r.bytes == c.canonical_bytes);
      CHECK(c.imported_bytes == c.to->layout->record_bytes);
      CHECK(std::memcmp(record, c.imported, static_cast<size_t>(c.imported_bytes)) == 0);
      if (std::memcmp(record, c.imported, static_cast<size_t>(c.imported_bytes)) != 0)
      {
         std::printf("  case %s: the import differs from the mirror's\n", c.name);
      }
   }
}

/// A stat's index in a fixture by its transfer key.
/// @param info  the table
/// @param key   transfer key ("hp", "atk", ...)
/// @return stat index (aborts when missing)
int stat_of(const TransferInfo& info, const char* key)
{
   const uint32_t k = transfer_key("stat:", key);
   for (int s = 0; s < info.layout->stat_count; ++s)
   {
      if (info.stat_keys[s] == k)
      {
         return s;
      }
   }
   std::printf("no stat %s\n", key);
   std::abort();
}

/// A custom role's ID in a fixture by name.
/// @param layout  the layout
/// @param name    role name
/// @return its ID
RoleId custom_of(const Layout& layout, const char* name)
{
   return find_custom(layout, name_hash(name));
}

/// Six stats into four (four <- six): matching keys copied, two stats and three roles dropped, values narrowed,
/// two slots kept, one-sided custom roles defaulted.
void test_six_to_four()
{
   const Case& c = find_case("six_to_four");
   const Layout& to = *c.to->layout;
   const Layout& from = *c.from->layout;
   uint8_t rec[kMaxRecordBytes] = {};
   const TransferResult r = import_case(c, rec);
   CHECK(r.status == TransferStatus::Ok);
   CHECK(r.flags == (kTransferNarrowed | kTransferDropped | kTransferDefaulted));
   const RoleId genes = id_of(Role::StatGenes);
   const RoleId training = id_of(Role::StatTraining);

   // Stats by key, whatever the receiver calls them; genes clamp to 4 bits.
   CHECK(get(to, rec, genes, stat_of(*c.to, "hp")) == 15);   // 31 clamped
   CHECK(get(to, rec, genes, stat_of(*c.to, "atk")) == 15);  // 20 clamped
   CHECK(get(to, rec, genes, stat_of(*c.to, "def")) == get(from, c.source, genes, stat_of(*c.from, "def")));
   CHECK(get(to, rec, training, stat_of(*c.to, "spe")) == 255);
   CHECK(get(to, rec, training, stat_of(*c.to, "hp")) == 252);

   // Two move slots of four; 300 fits 10 bits; held item 300 clamps to 8 bits.
   CHECK(count(to, id_of(Role::Moves)) == 2);
   CHECK(get(to, rec, id_of(Role::Moves), 0) == 1 && get(to, rec, id_of(Role::Moves), 1) == 300);
   CHECK(get(to, rec, id_of(Role::Pp), 0) == 10 && get(to, rec, id_of(Role::Pp), 1) == 20);
   CHECK(get(to, rec, id_of(Role::HeldItem), 0) == 255);
   CHECK(get(to, rec, id_of(Role::Level), 0) == 120 && get(to, rec, id_of(Role::Experience), 0) == 1000000);
   CHECK(get(to, rec, id_of(Role::SpeciesKey), 0) == 0x12345678u);

   // Custom roles by key: friendship narrowed to 6 bits; met_level only here (its default); gender default 0.
   CHECK(get(to, rec, custom_of(to, "friendship"), 0) == 63);
   CHECK(get(to, rec, custom_of(to, "met_level"), 0) == 5);
   CHECK(get(to, rec, custom_of(to, "gender"), 0) == 0);

   // The nickname keeps its first 5 codes.
   for (int e = 0; e < 5; ++e)
   {
      CHECK(get(to, rec, id_of(Role::Nickname), e) == static_cast<uint32_t>(10 + e));
   }
}

/// A short nickname and empty move slots past the receiver's count are no loss; the stats still drop.
void test_empty_slots_are_no_loss()
{
   const Case& c = find_case("six_short_name_to_four");
   const Layout& to = *c.to->layout;
   uint8_t rec[kMaxRecordBytes] = {};
   CHECK(import_case(c, rec).status == TransferStatus::Ok);
   // "no move" (16 bits all set) stays "no move" at 10 bits.
   CHECK(get(to, rec, id_of(Role::Moves), 0) == 4 && get(to, rec, id_of(Role::Moves), 1) == 1023);
   CHECK(get(to, rec, id_of(Role::Nickname), 3) == 0xFF && get(to, rec, id_of(Role::Nickname), 4) == 0xFF);
}

/// Four stats into six (six <- four): missing stats take six's transfer_defaults, missing slots "no move", the
/// nickname is padded with end codes, four's own custom roles drop.
void test_four_to_six()
{
   const Case& c = find_case("four_to_six");
   const Layout& to = *c.to->layout;
   uint8_t rec[kMaxRecordBytes] = {};
   const TransferResult r = import_case(c, rec);
   CHECK(r.status == TransferStatus::Ok && r.flags == (kTransferDropped | kTransferDefaulted));
   const RoleId genes = id_of(Role::StatGenes);
   const RoleId training = id_of(Role::StatTraining);
   CHECK(get(to, rec, genes, stat_of(*c.to, "hp")) == 15 && get(to, rec, genes, stat_of(*c.to, "atk")) == 9);
   CHECK(get(to, rec, genes, stat_of(*c.to, "spe")) == 7);
   CHECK(get(to, rec, genes, stat_of(*c.to, "spa")) == 7);     // transfer_defaults spa: {stat_genes: 7}
   CHECK(get(to, rec, training, stat_of(*c.to, "spa")) == 0);
   CHECK(get(to, rec, genes, stat_of(*c.to, "spd")) == 3);     // transfer_defaults spd: 3 (every per-stat role)
   CHECK(get(to, rec, training, stat_of(*c.to, "spd")) == 3);
   CHECK(get(to, rec, id_of(Role::Moves), 0) == 17);
   for (int slot = 1; slot < 4; ++slot)
   {
      CHECK(get(to, rec, id_of(Role::Moves), slot) == 0xFFFF);   // 10-bit "no move" and missing slots
      CHECK(get(to, rec, id_of(Role::Pp), slot) == 0);
   }
   CHECK(get(to, rec, id_of(Role::Nickname), 4) == 24 && get(to, rec, id_of(Role::Nickname), 5) == 0xFF);
   CHECK(get(to, rec, id_of(Role::Nickname), 11) == 0xFF);
   CHECK(get(to, rec, custom_of(to, "friendship"), 0) == 63);
   CHECK(get(to, rec, custom_of(to, "nicknamed"), 0) == 0);
   CHECK(get(to, rec, id_of(Role::Seed), 0) == 0);
}

/// Round trips: same layout gives the same record; six -> four -> six keeps what four can hold.
void test_round_trips()
{
   for (const char* name : {"six_same", "four_same", "six_to_shuffled", "shuffled_to_six"})
   {
      const Case& c = find_case(name);
      uint8_t rec[kMaxRecordBytes] = {};
      const TransferResult r = import_case(c, rec);
      CHECK(r.status == TransferStatus::Ok && r.flags == 0);
      uint8_t back[kMaxTransferBytes] = {};
      CHECK(export_record(*c.to, rec, back, kMaxTransferBytes, TransferForm::Canonical).status == TransferStatus::Ok);
      // The canonical form does not depend on the layout: six and shuffled write the same bytes.
      CHECK(std::memcmp(back, c.canonical, static_cast<size_t>(c.canonical_bytes)) == 0);
   }
   CHECK(std::memcmp(find_case("six_same").canonical, find_case("shuffled_to_six").canonical,
                     static_cast<size_t>(find_case("six_same").canonical_bytes)) == 0);
   CHECK(std::memcmp(find_case("six_to_four").imported, find_case("shuffled_to_four").imported,
                     static_cast<size_t>(find_case("six_to_four").imported_bytes)) == 0);

   // six -> four -> six: the values four holds come back.
   const Case& out = find_case("six_to_four");
   uint8_t four_rec[kMaxRecordBytes] = {};
   CHECK(import_case(out, four_rec).status == TransferStatus::Ok);
   uint8_t wire[kMaxTransferBytes] = {};
   const TransferResult w = export_record(*out.to, four_rec, wire, kMaxTransferBytes, TransferForm::Canonical);
   uint8_t six_rec[kMaxRecordBytes] = {};
   CHECK(import_record(*out.from, wire, w.bytes, six_rec, kMaxRecordBytes).status == TransferStatus::Ok);
   const Layout& six = *out.from->layout;
   CHECK(get(six, six_rec, id_of(Role::Moves), 1) == 300 && get(six, six_rec, id_of(Role::Moves), 2) == 0xFFFF);
   CHECK(get(six, six_rec, id_of(Role::StatTraining), 0) == 252);
   CHECK(get(six, six_rec, id_of(Role::SpeciesKey), 0) == 0x12345678u);
}

/// The raw form: the build's table and its twin fixture share a transfer hash; another hash is refused.
void test_raw_form()
{
   const Case& c = find_case("sample_to_twin");
   CHECK(c.from == &kTransfer);
   CHECK(kTransfer.hash == c.to->hash && kTransfer.layout->hash == c.to->layout->hash);
   CHECK(form_for(kTransfer, c.to->hash) == TransferForm::Raw);
   CHECK(form_for(kTransfer, find_case("six_to_four").to->hash) == TransferForm::Canonical);

   // The build's overloads (Record, kTransfer).
   Record rec = {};
   std::memcpy(rec.bytes, c.source, static_cast<size_t>(c.source_bytes));
   uint8_t wire[kMaxTransferBytes] = {};
   TransferResult r = export_record(rec, wire, kMaxTransferBytes, TransferForm::Raw);
   CHECK(r.status == TransferStatus::Ok && r.bytes == c.raw_bytes && r.flags == kTransferRaw);
   uint8_t twin_rec[kMaxRecordBytes] = {};
   r = import_record(*c.to, wire, r.bytes, twin_rec, kMaxRecordBytes);
   CHECK(r.status == TransferStatus::Ok && r.flags == kTransferRaw);
   CHECK(std::memcmp(twin_rec, c.source, static_cast<size_t>(c.source_bytes)) == 0);
   Record back = {};
   r = import_record(wire, c.raw_bytes, back);
   CHECK(r.status == TransferStatus::Ok && std::memcmp(back.bytes, rec.bytes, sizeof(rec.bytes)) == 0);

   uint8_t four_rec[kMaxRecordBytes] = {};
   r = import_record(*find_case("six_to_four").to, wire, c.raw_bytes, four_rec, kMaxRecordBytes);
   CHECK(r.status == TransferStatus::OtherLayout);
   r = import_record(*c.to, wire, c.raw_bytes - 1, twin_rec, kMaxRecordBytes);
   CHECK(r.status == TransferStatus::Truncated);
}

/// Malformed input, small buffers and the size bound.
void test_errors_and_bounds()
{
   const Case& c = find_case("six_to_four");
   uint8_t rec[kMaxRecordBytes] = {};
   uint8_t buf[kMaxTransferBytes] = {};

   // Every strict prefix is truncated; nothing is written.
   for (int n = 0; n < c.canonical_bytes; ++n)
   {
      std::memset(rec, 0xAA, sizeof(rec));
      CHECK(import_record(*c.to, c.canonical, n, rec, kMaxRecordBytes).status == TransferStatus::Truncated);
      CHECK(rec[0] == 0xAA);
   }
   // Bytes past the record are ignored.
   std::memcpy(buf, c.canonical, static_cast<size_t>(c.canonical_bytes));
   TransferResult r = import_record(*c.to, buf, c.canonical_bytes + 4, rec, kMaxRecordBytes);
   CHECK(r.status == TransferStatus::Ok && r.bytes == c.canonical_bytes);

   // Header offsets of the six layout's canonical record: version, E, a 3-byte mask, then the custom keys.
   constexpr int kCustomCount = 5;
   constexpr int kFirstCustomKey = kCustomCount + 1;
   CHECK(c.canonical[0] == kTransferVersion && c.canonical[1] == kTransferEngineRoles);
   CHECK(c.canonical[kCustomCount] == 3);

   buf[0] = 3;
   CHECK(import_record(*c.to, buf, c.canonical_bytes, rec, kMaxRecordBytes).status == TransferStatus::BadVersion);
   buf[0] = kTransferVersion;
   // Keys out of order.
   uint8_t swapped[kTransferKeyBytes];
   std::memcpy(swapped, buf + kFirstCustomKey, kTransferKeyBytes);
   std::memcpy(buf + kFirstCustomKey, buf + kFirstCustomKey + kTransferKeyBytes, kTransferKeyBytes);
   std::memcpy(buf + kFirstCustomKey + kTransferKeyBytes, swapped, kTransferKeyBytes);
   CHECK(import_record(*c.to, buf, c.canonical_bytes, rec, kMaxRecordBytes).status == TransferStatus::BadShape);
   std::memcpy(buf, c.canonical, static_cast<size_t>(c.canonical_bytes));
   // A padding bit of the mask (role ID 31; the mask covers 24).
   buf[kCustomCount - 1] = static_cast<uint8_t>(buf[kCustomCount - 1] | 0x80);
   CHECK(import_record(*c.to, buf, c.canonical_bytes, rec, kMaxRecordBytes).status == TransferStatus::BadShape);
   std::memcpy(buf, c.canonical, static_cast<size_t>(c.canonical_bytes));
   // The first shape (species key): width 0, then 33; then a per-stat count other than the stat table's.
   const int stats_at = kFirstCustomKey + 3 * kTransferKeyBytes;
   const int shapes_at = stats_at + 1 + buf[stats_at] * kTransferKeyBytes;
   for (uint8_t bad : {uint8_t{0}, uint8_t{33}})
   {
      buf[shapes_at] = bad;
      CHECK(import_record(*c.to, buf, c.canonical_bytes, rec, kMaxRecordBytes).status == TransferStatus::BadShape);
   }
   std::memcpy(buf, c.canonical, static_cast<size_t>(c.canonical_bytes));
   // Shapes in order: species key, seed, owner id, experience, level, moves, pp, pp_ups, stat_genes (9th).
   CHECK(buf[shapes_at + 8 * kTransferShapeBytes] == 5 && buf[shapes_at + 8 * kTransferShapeBytes + 1] == 6);
   buf[shapes_at + 8 * kTransferShapeBytes + 1] = 5;
   CHECK(import_record(*c.to, buf, c.canonical_bytes, rec, kMaxRecordBytes).status == TransferStatus::BadShape);

   // Small buffers.
   CHECK(export_record(*c.from, c.source, buf, c.canonical_bytes - 1, TransferForm::Canonical).status ==
         TransferStatus::ShortBuffer);
   CHECK(import_record(*c.to, c.canonical, c.canonical_bytes, rec, c.to->layout->record_bytes - 1).status ==
         TransferStatus::ShortBuffer);

   // The bound covers every case and this build's own table.
   for (const Case& v : kCases)
   {
      CHECK(v.canonical_bytes <= transfer_bound(v.from->layout->record_bytes));
      CHECK(v.raw_bytes <= kMaxTransferBytes);
   }
   CHECK(transfer_size(kTransfer, TransferForm::Canonical) <= kMaxTransferBytes);
   static_assert(transfer_key("role:", "friendship") != name_hash("friendship"));
}

} // namespace

int main()
{
   test_python_mirror();
   test_six_to_four();
   test_empty_slots_are_no_loss();
   test_four_to_six();
   test_round_trips();
   test_raw_form();
   test_errors_and_bounds();
   std::printf("transfer tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
