/// @file
/// The Starter's Grove (D-28 sections 2 and 3, C9 regression from the D-28 review): the two-slot nursery runs the
/// grove method (same `kin`: 50%, else incompatible) and its egg is the first held record's base stage, with that
/// species' combine move when an input knows it. Records: Breezle and Duskowl (wing kin), Breezmaw (Breezle's second
/// stage), Sproutle and Tidepup (other kin). The review read the Grove's HUD row as the wrong pair; this pins what
/// the nursery holds and what it makes (samples/starter/README.md, row Nursery and eggs).
#include "capsule/battle/battle.h"
#include "capsule/combine/eggs.h"
#include "capsule/combine/nursery.h"
#include "capsule/monster/monster.h"

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
using namespace battle_ids;
namespace cn = capsule::combine::nursery;

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
   } while (0)

constexpr uint32_t kSeed = 91;       ///< Seed of the records' stream.
constexpr uint32_t kChecks = 5150;   ///< Seed of the checks' stream.
constexpr uint8_t kLevel = 5;        ///< The members' level.
constexpr int32_t kSameKin = 50;     ///< The grove's odds for one kin.
constexpr int kMaxChecks = 64;       ///< Checks tried before a 50% check must have hit.
constexpr uint32_t kHatchSteps = 5 * 256;   ///< A Starter egg's counter: 5 cycles of 256 steps.

battle::State g_state;               ///< The State lent to the checks.
monster::Party g_party{};            ///< The party.
rng::Stream g_rng{kChecks};          ///< The checks' draws.

/// A record's species.
/// @param rec  the record
/// @return its species index
uint16_t species_of(const monster::Record& rec)
{
   return battle::find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
}

/// Whether a record knows a move.
/// @param rec   the record
/// @param move  the move
/// @return true when a slot holds it
bool knows(const monster::Record& rec, uint16_t move)
{
   for (int k = 0; k < battle::move_slots(); ++k)
   {
      if (monster::get(rec, monster::Role::Moves, k) == move)
      {
         return true;
      }
   }
   return false;
}

/// A party of fresh records (one more, Clodbun, stays behind: the party's last member never leaves).
/// @param a  first species
/// @param b  second species
void party_of(uint16_t a, uint16_t b)
{
   rng::Stream r{kSeed};
   g_party = monster::Party{};
   const uint16_t species[] = {a, b, SPECIES_CLODBUN};
   for (uint8_t k = 0; k < 3; ++k)
   {
      CHECK(battle::make_monster(g_party.members[k], species[k], kLevel, r));
   }
   g_party.count = 3;
}

/// Leaves both first members in the Grove and checks until an egg comes (or the tries end).
/// @return true when one came
bool breed()
{
   cn::clear();
   CHECK(cn::deposit(g_party, 0) == cn::Result::Ok && cn::deposit(g_party, 0) == cn::Result::Ok);
   bool made = false;
   for (int k = 0; k < kMaxChecks && !made; ++k)
   {
      made = cn::check(g_state, g_party, g_rng);
   }
   return made;
}

/// Breezle + Duskowl: the Grove holds them in that order; the egg is Breezle's (first held), 5 x 256 steps.
void test_wing_pair()
{
   party_of(SPECIES_BREEZLE, SPECIES_DUSKOWL);
   cn::clear();
   CHECK(cn::slot_count() == 2);
   CHECK(cn::deposit(g_party, 0) == cn::Result::Ok && cn::deposit(g_party, 0) == cn::Result::Ok);
   CHECK(species_of(*cn::record(0)) == SPECIES_BREEZLE && species_of(*cn::record(1)) == SPECIES_DUSKOWL);
   CHECK(cn::odds() == kSameKin);
   party_of(SPECIES_BREEZLE, SPECIES_DUSKOWL);
   CHECK(breed());
   const monster::Record& egg = *cn::result();
   CHECK(combine::is_egg(egg) && species_of(egg) == SPECIES_BREEZLE && combine::hatch_left(egg) == kHatchSteps);
   // Breezle's combine move (Updraft) only when an input knows it
   CHECK(knows(egg, MOVE_UPDRAFT) == (knows(*cn::record(0), MOVE_UPDRAFT) || knows(*cn::record(1), MOVE_UPDRAFT)));
   // the other order: Duskowl's egg
   party_of(SPECIES_DUSKOWL, SPECIES_BREEZLE);
   CHECK(breed() && species_of(*cn::result()) == SPECIES_DUSKOWL);
   // a second stage gives its base stage: Breezmaw's egg is a Breezle
   party_of(SPECIES_BREEZMAW, SPECIES_DUSKOWL);
   CHECK(breed() && species_of(*cn::result()) == SPECIES_BREEZLE);
}

/// Sproutle + Tidepup: other kin, the odds are 0 and no egg ever comes.
void test_other_kin()
{
   party_of(SPECIES_SPROUTLE, SPECIES_TIDEPUP);
   CHECK(!breed() && cn::odds() == 0 && !cn::result_ready());
}

} // namespace

int main()
{
   test_wing_pair();
   test_other_kin();
   std::printf("starter grove tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
