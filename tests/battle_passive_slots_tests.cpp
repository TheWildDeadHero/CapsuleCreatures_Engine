/// @file
/// Tests for the passive slot model (D-25 decision 6, C4) against the battle data compiled from
/// tests/data/battle_progress/passives.json: three passive slots, the first a hidden one only data gives (not
/// rolled), then two rolled ones. Blob fills the hidden and the second slot and leaves the first rolled slot empty;
/// Cube fills the hidden and the first. An empty slot gives the passive of the first rolled slot the species fills,
/// never the hidden one (review 2: the empty slot fell back to slot 0), and new records draw the same way as before.
#include "capsule/battle/battle.h"

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

/// Record seeds the draws are tried with (enough for both rolled slots to come up).
constexpr uint32_t kSeeds = 64;
/// Level of the records made.
constexpr uint8_t kLevel = 5;
/// The battle's seed.
constexpr uint32_t kBattleSeed = 1;
/// The fixture's slots: the hidden one, then the two rolled ones.
constexpr uint8_t kSlotHidden = 0;
constexpr uint8_t kSlotFirst = 1;
constexpr uint8_t kSlotSecond = 2;
/// The player's battler.
constexpr uint8_t kPlayer = 0;
/// The foe party's first and second battlers in a single battle's order (its lead).
constexpr uint8_t kFoeLead = 1;

State g_state;               ///< Battle state (large; static storage).
monster::Party g_player{};   ///< The player's records.

/// Starts a single battle: the player's first record against a party.
/// @param party  the foe party
/// @return true if it started
bool begin(uint16_t party)
{
   Setup su{};
   su.participants[0] = Participant{nullptr, &g_player, nullptr, Controller::Human, 0, 0, 0};
   su.participants[1] = Participant{&kData.parties[party], nullptr, nullptr, Controller::Ai, 1, 0, 0};
   su.count = 2;
   su.format = 0;
   su.weather = kNone8;
   su.seed = kBattleSeed;
   return start(g_state, su);
}

/// The data: the hidden slot is not rolled, the other two are; the species as the file says.
void test_data()
{
   CHECK(kData.passive_slots == 3);
   CHECK(((kData.passive_rolled >> kSlotHidden) & 1u) == 0);
   CHECK(((kData.passive_rolled >> kSlotFirst) & 1u) == 1 && ((kData.passive_rolled >> kSlotSecond) & 1u) == 1);
   const SpeciesData& blob = kData.species[SPECIES_BLOB];
   CHECK(blob.passives[kSlotHidden] == PASSIVE_HIDDEN && blob.passives[kSlotFirst] == kNone8 &&
         blob.passives[kSlotSecond] == PASSIVE_SECOND);
}

/// New Blob records draw one of the two rolled slots (the hidden slot never: it is not rolled); a record holding
/// the empty first slot battles with the second slot's passive, not the hidden one (rv/t5.py's case).
void test_records()
{
   int drawn[kSlotSecond + 1] = {};
   for (uint32_t seed = 1; seed <= kSeeds; ++seed)
   {
      rng::Stream r{seed};
      g_player = monster::Party{};
      CHECK(make_monster(g_player.members[0], SPECIES_BLOB, kLevel, r));
      g_player.count = 1;
      const uint32_t slot = monster::get(g_player.members[0], Role::PassiveSelector);
      CHECK(slot == kSlotFirst || slot == kSlotSecond);
      if (slot <= kSlotSecond)
      {
         ++drawn[slot];
      }
      CHECK(begin(PARTY_TARGET));
      CHECK(g_state.battlers[kPlayer].passive == PASSIVE_SECOND);
   }
   CHECK(drawn[kSlotFirst] > 0 && drawn[kSlotSecond] > 0 && drawn[kSlotHidden] == 0);

   // Cube fills no rolled slot past the first: no draw, the first rolled slot
   rng::Stream r{kSeeds};
   g_player = monster::Party{};
   CHECK(make_monster(g_player.members[0], SPECIES_CUBE, kLevel, r));
   g_player.count = 1;
   CHECK(monster::get(g_player.members[0], Role::PassiveSelector) == kSlotFirst);
   CHECK(begin(PARTY_TARGET));
   CHECK(g_state.battlers[kPlayer].passive == PASSIVE_FIRST);

   // a record naming a slot past the manifest's acts as an empty one
   monster::set(g_player.members[0], Role::PassiveSelector, kSlotSecond + 1);
   CHECK(begin(PARTY_TARGET));
   CHECK(g_state.battlers[kPlayer].passive == PASSIVE_FIRST);
}

/// Party members: a member given the empty first slot gets the second slot's passive; one given the hidden slot
/// keeps the hidden passive (data may give it).
void test_members()
{
   rng::Stream r{1};
   g_player = monster::Party{};
   CHECK(make_monster(g_player.members[0], SPECIES_CUBE, kLevel, r));
   g_player.count = 1;
   CHECK(begin(PARTY_GIVEN));
   CHECK(g_state.battlers[kFoeLead].species == SPECIES_BLOB);
   CHECK(g_state.battlers[kFoeLead].passive == PASSIVE_SECOND);
   CHECK(kData.parties[PARTY_GIVEN].members[1].passive_slot == kSlotHidden);
}

} // namespace

/// Runs every test.
/// @return 0 when all passed
int main()
{
   test_data();
   test_records();
   test_members();
   std::printf("battle_passive_slots_tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
