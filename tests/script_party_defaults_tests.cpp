/// @file
/// C7 / C8: the engine's default party statements (src/script/script_party.cpp) on a real ruleset with boxes and
/// eggs (tests/data/combine): give into the party, then the boxes when the party is full, then nowhere without
/// boxes; an egg (left out of the hatched count); heal clearing the status; the move search with its species.
#include "capsule/battle/battle.h"
#include "capsule/monster/monster.h"
#include "capsule/script/ops.h"
#include "capsule/script/port.h"
#include "capsule/storage/storage.h"

#include "battle_ids.h"

#include <cstdio>
#include <cstdlib>

namespace capsule::hal::system
{
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   std::printf("ASSERT %s:%d: %s\n", file, line, expr);
   std::abort();
}
} // namespace capsule::hal::system

namespace
{
capsule::monster::Party g_party{};      ///< The party the defaults work on.
capsule::storage::Buffer<battle_ids::STORAGE_BYTES, battle_ids::STORAGE_USED_BYTES> g_box_bytes{};   ///< The boxes' bytes.
capsule::storage::Storage g_boxes = capsule::storage::view(g_box_bytes);   ///< Its boxes.
capsule::storage::Storage* g_attached = &g_boxes;   ///< The boxes the defaults see (nullptr: none).
} // namespace

/// The box statements' accessors, as script_boxes.cpp's attach() would give them (that file needs the whole VM).
namespace capsule::script::boxes
{
monster::Party* party()
{
   return &g_party;
}

storage::Storage* attached()
{
   return g_attached;
}
} // namespace capsule::script::boxes

namespace
{

using namespace capsule;
using namespace battle_ids;
using monster::Role;

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.

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

constexpr uint8_t kLevel = 5;        ///< The level given creatures start at.
constexpr uint32_t kSleeping = 1;    ///< A status the heal clears.
constexpr uint16_t kUnknownMove = 0xFFF0; ///< A move no member knows.
constexpr uint16_t kGiven = SPECIES_PUPKIN;   ///< The species given.

/// Gives until the party is full, then once more into the boxes; without boxes, nowhere.
void test_give()
{
   g_party = monster::Party{};
   storage::clear(g_boxes);
   g_attached = &g_boxes;
   for (int i = 0; i < monster::kLayout.party_size; ++i)
   {
      CHECK(script::party::give(kGiven, kLevel, 0) == script::kGivenParty);
   }
   CHECK(g_party.count == monster::kLayout.party_size);
   CHECK(script::party::give(kGiven, kLevel, 0) == script::kGivenBoxes);
   CHECK(storage::count(g_boxes) == 1);
   g_attached = nullptr;
   CHECK(script::party::give(kGiven, kLevel, 0) == script::kGivenNone);
   CHECK(script::party::give_egg(kGiven) == script::kGivenNone);   // nowhere for it either
}

/// An egg joins the party and is left out of the hatched count.
void test_egg()
{
   g_party = monster::Party{};
   g_attached = &g_boxes;
   CHECK(script::party::give(kGiven, kLevel, 0) == script::kGivenParty);
   CHECK(script::party::give_egg(kGiven) == script::kGivenParty);
   CHECK(script::party::count(false) == 2 && script::party::count(true) == 1);
}

/// The counts, the move search and the heal on a party of one.
void test_queries()
{
   g_party = monster::Party{};
   g_attached = &g_boxes;
   CHECK(script::party::give(kGiven, kLevel, 0) == script::kGivenParty);
   CHECK(script::party::count(false) == 1 && script::party::count(true) == 1);
   int32_t slot = -1;
   int32_t species = -1;
   const auto known = static_cast<uint16_t>(monster::get(g_party.members[0], Role::Moves, 0));   // its first move
   CHECK(script::party::find_move(known, slot, species) && slot == 0 && species == kGiven);
   CHECK(!script::party::find_move(kUnknownMove, slot, species) && slot == monster::kLayout.party_size);
   monster::set(g_party.members[0], Role::PrimaryStatus, kSleeping);
   script::party::heal();
   CHECK(monster::get(g_party.members[0], Role::PrimaryStatus) == 0);
}

} // namespace

int main()
{
   test_give();
   test_queries();
   test_egg();
   std::printf("script_party_defaults_tests: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
