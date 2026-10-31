/// @file
/// Tests for the field rules (F1, D-27 sections 1 and 2) against the battle data compiled from tests/data/field: the
/// lead rule (an expression over a member), the lead rules of an encounter roll (capsule/battle/encounter_lead.h:
/// the `encounter` hook's primitives on the lead's passive and held item and the game's global owner, the
/// encounter_rate / encounter_level pipelines, the draws of a roll without hooks equal to encounter::roll_plain()'s),
/// the creature made (`encounter_made`: a role copied from the lead, a re-pick, a block) and the effect logic of the
/// timed field effects (`field_effect start`, `field.effect_active`). The script side of the timed field effects
/// (their variables, stack rules, steps, minutes, expiry and the statements) is tests/script_tests.cpp's.
///
/// The encounter bank is built here (encounter::set_bank()); the timed field effects are this program's own
/// stand-in for capsule/script/field_effects.h (a battle-library program has no script bank), so the test decides
/// which effect runs and sees what effect logic starts.
#include "capsule/battle/battle.h"
#include "capsule/battle/data.h"
#include "capsule/battle/encounter_lead.h"
#include "capsule/battle/field_context.h"
#include "capsule/core/random.h"
#include "capsule/encounter/encounter.h"
#include "capsule/script/field_effects.h"

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

int32_t g_left[3] = {0, 0, 0};   ///< What the stand-in field effects have left.
int g_starts = 0;                ///< start() calls that started one.
int32_t g_last_amount = 0;       ///< The amount of the last start().
bool g_refuse = false;           ///< start() refuses.

} // namespace

/// This program's stand-in for the timed field effects (the engine library's version keeps them in the script bank).
namespace capsule::script::field_effects
{

uint8_t count()
{
   return 3;
}

const FieldEffectInfo* info(uint8_t)
{
   return nullptr;
}

bool start(uint8_t effect, int32_t amount)
{
   if (effect >= count() || g_refuse)
   {
      return false;
   }
   g_left[effect] = amount;
   g_last_amount = amount;
   ++g_starts;
   return true;
}

bool active(uint8_t effect)
{
   return effect < count() && g_left[effect] > 0;
}

int32_t remaining(uint8_t effect)
{
   return effect < count() ? g_left[effect] : 0;
}

void stop(uint8_t effect)
{
   if (effect < count())
   {
      g_left[effect] = 0;
   }
}

void walked(uint32_t)
{
}

void tick()
{
}

void reset_clock()
{
}

bool take_expired(uint8_t*)
{
   return false;
}

} // namespace capsule::script::field_effects

namespace
{

using namespace capsule;
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

/// @name The test's encounter bank: one step method, a table of three types and sizes, a low-level table
///@{
const encounter::Method kMethods[] = {{encounter::Trigger::Step, 1, {0, 0}}};
/// Table 0: Blob (Plain, size 1) L2-4 w50, Zap (Volt) L3-5 w30, Fern (Leaf, size 3) L5-7 w20; 32 in 256.
/// Table 1: Blob L2-4 only, always rolls.
encounter::Entry kEntries[] = {
   {0, 0, SPECIES_BLOB, 50, 0, 0, 2, 4, {0, 0, 0}},
   {0, 0, SPECIES_ZAP, 30, 0, 0, 3, 5, {0, 0, 0}},
   {0, 0, SPECIES_FERN, 20, 0, 0, 5, 7, {0, 0, 0}},
   {0, 0, SPECIES_BLOB, 1, 0, 0, 2, 4, {0, 0, 0}},
};
const encounter::Table kTables[] = {{0, 3, 32, 256}, {3, 1, 1, 1}};
const encounter::Bank kBank = {kMethods, kTables, kEntries, nullptr, nullptr, nullptr, nullptr, 2, 4, 0, 0, 1, 0};
constexpr uint16_t kGrass = 0;   ///< Table 0.
constexpr uint16_t kLow = 1;     ///< Table 1.
///@}

battle::State g_state{};          ///< The State the lead rules lend for their field contexts.
monster::Party g_party{};         ///< The player's party.

/// A fresh record.
/// @param rec      it
/// @param species  species
/// @param level    level
/// @param seed     stream seed for its random parts
void make(monster::Record& rec, uint16_t species, uint8_t level, uint32_t seed = 1)
{
   rng::Stream r{seed};
   CHECK(battle::make_monster(rec, species, level, r));
}

/// Makes the party: a member below the lead rule's level, then the lead.
/// @param lead_species  the lead's species (its passive)
/// @param lead_level    its level
void party_with(uint16_t lead_species, uint8_t lead_level)
{
   g_party = monster::Party{};
   make(g_party.members[0], SPECIES_BLOB, 2);
   make(g_party.members[1], lead_species, lead_level);
   g_party.count = 2;
}

/// One roll of a table with the attached rules.
/// @param table  bank table index
/// @param r      stream
/// @param out    the encounter
/// @return true when one starts
bool roll(uint16_t table, rng::Stream& r, encounter::Result& out)
{
   return encounter::rules() != nullptr ? encounter::rules()(table, 0, r, &out)
                                        : encounter::roll_plain(table, 0, r, &out);
}

/// The lead rule: the first member at level 3 or more with hit points.
void test_lead()
{
   party_with(SPECIES_LEADLURE, 10);
   CHECK(battle::lead_of(g_party) == 1);
   monster::set(g_party.members[1], Role::CurrentHp, 0);
   CHECK(battle::lead_of(g_party) == battle::kBench);   // the level-2 member does not lead either
   CHECK(battle::hook_lead_of(g_party) == 1);   // C8: hook_when ("level >= 3") does not ask for hit points
   make(g_party.members[0], SPECIES_BLOB, 3);
   CHECK(battle::lead_of(g_party) == 0);
   g_party.count = 0;
   CHECK(battle::lead_of(g_party) == battle::kBench);
}

/// Without hooks (a lead whose passive has none; the ward rule's hook reads no draw) the rolls draw exactly what
/// roll_plain() draws: the same results, the stream left in the same state.
void test_identity()
{
   party_with(SPECIES_BLOB, 10);
   monster::set(g_party.members[0], Role::Level, 10);   // Blob leads: its passive Calm has no logic
   rng::Stream a{77};
   rng::Stream b{77};
   int same = 0;
   int met = 0;
   for (int i = 0; i < 300; ++i)
   {
      encounter::Result x{};
      encounter::Result y{};
      const bool px = encounter::roll_plain(kGrass, 0, a, &x);
      bool py = false;
      {
         battle::LeadScope scope(g_state, g_party);
         py = roll(kGrass, b, y);
      }
      met += px ? 1 : 0;
      same += px == py && (!px || (x.species == y.species && x.level == y.level && x.entry == y.entry)) ? 1 : 0;
   }
   CHECK(same == 300 && met > 0);
   CHECK(rng::next16(a) == rng::next16(b));
   CHECK(encounter::rules() == nullptr);   // the scope gave the rules back
}

/// The rate: the lead's passive doubles it, another's and a held item's halve it (they multiply).
void test_rate()
{
   rng::Stream r{5};
   encounter::Result e{};
   party_with(SPECIES_LEADLURE, 10);
   {
      battle::LeadScope scope(g_state, g_party);
      roll(kGrass, r, e);
      // 32 x 2 in 256, kept in Q8 (32 x 512 in 256 x 256)
      CHECK(battle::last_encounter().chance == 32 * 512 && battle::last_encounter().out_of == 256 * 256);
      CHECK(battle::last_encounter().lead == 1 && battle::last_encounter().rate_scale == 512);
   }
   party_with(SPECIES_LEADHUSH, 10);
   monster::set(g_party.members[1], Role::HeldItem, ITEM_QUIET_BELL);
   {
      battle::LeadScope scope(g_state, g_party);
      roll(kGrass, r, e);
      CHECK(battle::last_encounter().chance == 32 * 64);   // 32 x 1/2 x 1/2 (Q8: 64) in 256 x 256
      CHECK(battle::last_encounter().handlers == 3);   // the ward rule, the passive, the item
   }
}

/// C8 (D-27 review): the hooks' member (`lead.hook_when`) is another member than the lead: a fainted Lure leads the
/// hooks (its passive doubles the rate, its held item halves it) while the Blob that can battle is the lead (the
/// level the pipelines see, the global ward rule's holder).
void test_hook_lead()
{
   rng::Stream r{19};
   encounter::Result e{};
   g_party = monster::Party{};
   make(g_party.members[0], SPECIES_LEADLURE, 10);
   monster::set(g_party.members[0], Role::CurrentHp, 0);
   make(g_party.members[1], SPECIES_BLOB, 7);
   g_party.count = 2;
   CHECK(battle::lead_of(g_party) == 1 && battle::hook_lead_of(g_party) == 0);
   {
      battle::LeadScope scope(g_state, g_party);
      roll(kGrass, r, e);
      CHECK(battle::last_encounter().lead == 1 && battle::last_encounter().rate_scale == 512);
      CHECK(battle::last_encounter().handlers == 2);   // the ward rule (on the lead), the Lure's passive
   }
   monster::set(g_party.members[0], Role::HeldItem, ITEM_QUIET_BELL);
   {
      battle::LeadScope scope(g_state, g_party);
      roll(kGrass, r, e);
      CHECK(battle::last_encounter().rate_scale == 256 && battle::last_encounter().handlers == 3);
   }
}

/// The levels: moved bounds, the level pipeline's bias (the fixture's: the highest level).
void test_levels()
{
   rng::Stream r{9};
   party_with(SPECIES_LEADSHIFT, 10);
   bool ok = true;
   {
      battle::LeadScope scope(g_state, g_party);
      for (int i = 0; i < 40; ++i)
      {
         encounter::Result e{};
         CHECK(roll(kLow, r, e));
         ok = ok && e.species == SPECIES_BLOB && e.level >= 4 && e.level <= 6;
      }
   }
   CHECK(ok);
   party_with(SPECIES_LEADHASTY, 10);
   ok = true;
   {
      battle::LeadScope scope(g_state, g_party);
      for (int i = 0; i < 40; ++i)
      {
         encounter::Result e{};
         CHECK(roll(kLow, r, e));
         ok = ok && e.level == 4;
      }
   }
   CHECK(ok);
}

/// The biases: a type (Magnet: Volt with odds 1 in 1) and a species field (Big: size 3).
void test_bias()
{
   rng::Stream r{11};
   int zaps = 0;
   int bigs = 0;
   int picks = 0;
   party_with(SPECIES_LEADMAGNET, 10);
   {
      battle::LeadScope scope(g_state, g_party);
      for (int i = 0; i < 400; ++i)
      {
         encounter::Result e{};
         if (roll(kGrass, r, e))
         {
            ++picks;
            zaps += e.species == SPECIES_ZAP ? 1 : 0;
         }
      }
   }
   CHECK(picks > 0 && zaps == picks);
   picks = 0;
   party_with(SPECIES_LEADBIG, 10);
   {
      battle::LeadScope scope(g_state, g_party);
      for (int i = 0; i < 400; ++i)
      {
         encounter::Result e{};
         if (roll(kGrass, r, e))
         {
            ++picks;
            bigs += e.species == SPECIES_FERN ? 1 : 0;
         }
      }
   }
   CHECK(picks > 0 && bigs == picks);
}

/// C8 (D-27 review): a bias whose only matching entry is condition-gated off (Zap needs a condition that is not
/// active) still yields encounters: the pick falls back to the unbiased one instead of finding no entry.
void test_bias_gated()
{
   constexpr uint32_t kGateBit = 1u;   // a condition bit the test never activates
   constexpr int kZapEntry = 1;        // kEntries' Zap row
   rng::Stream r{17};
   encounter::set_conditions(0);
   kEntries[kZapEntry].required = kGateBit;
   int picks = 0;
   int zaps = 0;
   party_with(SPECIES_LEADMAGNET, 10);
   {
      battle::LeadScope scope(g_state, g_party);
      for (int i = 0; i < 400; ++i)
      {
         encounter::Result e{};
         if (roll(kGrass, r, e))
         {
            ++picks;
            zaps += e.species == SPECIES_ZAP ? 1 : 0;
         }
         CHECK(battle::last_encounter().end != battle::EncounterEnd::NoEntry);
      }
   }
   CHECK(picks > 0 && zaps == 0);
   // with the condition active the bias applies again
   encounter::set_conditions(kGateBit);
   picks = 0;
   {
      battle::LeadScope scope(g_state, g_party);
      for (int i = 0; i < 400; ++i)
      {
         encounter::Result e{};
         if (roll(kGrass, r, e))
         {
            ++picks;
            zaps += e.species == SPECIES_ZAP ? 1 : 0;
         }
      }
   }
   CHECK(picks > 0 && zaps == picks);
   encounter::set_conditions(0);
   kEntries[kZapEntry].required = 0;
}

/// The skips: Keen (below the lead's level + 1) and the global ward rule while the ward runs.
void test_skip()
{
   rng::Stream r{13};
   party_with(SPECIES_LEADKEEN, 3);
   int skipped = 0;
   int kept = 0;
   {
      battle::LeadScope scope(g_state, g_party);
      for (int i = 0; i < 60; ++i)
      {
         encounter::Result e{};
         if (roll(kLow, r, e))
         {
            kept += e.level >= 4 ? 1 : 0;
         }
         else
         {
            skipped += battle::last_encounter().end == battle::EncounterEnd::Skipped ? 1 : 0;
         }
      }
   }
   CHECK(skipped > 0 && kept > 0 && skipped + kept == 60);
   // the ward: nothing below the lead's level (10) is met while it runs
   party_with(SPECIES_BLOB, 10);
   monster::set(g_party.members[0], Role::Level, 10);
   g_left[FIELD_EFFECT_WARD] = 5;
   {
      battle::LeadScope scope(g_state, g_party);
      encounter::Result e{};
      CHECK(!roll(kLow, r, e) && battle::last_encounter().end == battle::EncounterEnd::Skipped);
      CHECK(battle::last_encounter().skip_below == 10);
   }
   g_left[FIELD_EFFECT_WARD] = 0;
   {
      battle::LeadScope scope(g_state, g_party);
      encounter::Result e{};
      CHECK(roll(kLow, r, e));
   }
}

/// The creature made: the lead's mark copied, a re-pick until Zap, a block.
void test_made()
{
   rng::Stream r{17};
   monster::Party wild{};
   party_with(SPECIES_LEADMATCH, 10);
   monster::set(g_party.members[1], monster::find_custom(monster::name_hash("mark")), 0, 9);
   {
      battle::LeadScope scope(g_state, g_party);
      const encounter::Result e{SPECIES_BLOB, 3, 0, kLow, 3};
      CHECK(battle::make_encounter(e, wild, r) == battle::Made::Battle);
      CHECK(wild.count == 1 && monster::get(wild.members[0], monster::find_custom(monster::name_hash("mark"))) == 9);
      CHECK(monster::get(g_party.members[1], Role::Level) == 10);   // the lead is not written back
   }
   party_with(SPECIES_LEADREPICK, 10);
   {
      battle::LeadScope scope(g_state, g_party);
      // a roll first (the re-pick uses its plan), then the creature
      encounter::Result e{};
      int made_zap = 0;
      int tries = 0;
      for (int i = 0; i < 50; ++i)
      {
         if (!roll(kGrass, r, e))
         {
            continue;
         }
         ++tries;
         if (battle::make_encounter(e, wild, r) == battle::Made::Battle &&
             battle::find_species(AssetKey{monster::get(wild.members[0], Role::SpeciesKey)}) == SPECIES_ZAP)
         {
            ++made_zap;
         }
         CHECK(battle::last_encounter().rerolls <= battle::kMaxEncounterRerolls);
      }
      CHECK(tries > 0 && made_zap * 2 > tries);   // Zap weighs 30 of 100: re-picks make it most of them
   }
   party_with(SPECIES_LEADSHY, 10);
   {
      battle::LeadScope scope(g_state, g_party);
      const encounter::Result e{SPECIES_BLOB, 3, 0, kLow, 3};
      CHECK(battle::make_encounter(e, wild, r) == battle::Made::Skipped && wild.count == 0);
   }
   // without a scope: just the creature
   const encounter::Result e{SPECIES_ZAP, 4, 0, kGrass, 1};
   CHECK(battle::make_encounter(e, wild, r) == battle::Made::Battle && wild.count == 1);
   CHECK(monster::get(wild.members[0], Role::Level) == 4);
}

/// Outside an encounter hook the Encounter op fails; the field effect items start their effects through
/// `field_effect start` (refused: the run fails, so the item stays).
void test_primitives()
{
   battle::effect::Action a{};
   a.op = battle::effect::Op::Encounter;
   a.id = static_cast<uint16_t>(battle::effect::EncounterOp::Rate);
   CHECK(battle::encounter_primitive(a).fail);
   party_with(SPECIES_BLOB, 10);
   CHECK(battle::field_open(g_state, g_party, 0));
   g_starts = 0;
   g_refuse = false;
   CHECK(battle::field_run(g_state, battle::kData.items[ITEM_WARD_CHARM].owner, battle::effect::Hook::UseField,
                           battle::kNone16) == battle::effect::Status::Done);
   CHECK(g_starts == 1 && g_last_amount == 5 && g_left[FIELD_EFFECT_WARD] == 5);
   g_refuse = true;
   CHECK(battle::field_run(g_state, battle::kData.items[ITEM_GLOW_STICK].owner, battle::effect::Hook::UseField,
                           battle::kNone16) == battle::effect::Status::Failed);
   g_refuse = false;
   g_left[FIELD_EFFECT_WARD] = 0;
   battle::field_detach(g_state);
}

} // namespace

/// Runs the tests.
/// @return 0 when all pass
int main()
{
   encounter::set_bank(&kBank);
   test_lead();
   test_identity();
   test_rate();
   test_hook_lead();
   test_levels();
   test_bias();
   test_bias_gated();
   test_skip();
   test_made();
   test_primitives();
   encounter::set_bank(nullptr);
   std::printf("field_tests: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
