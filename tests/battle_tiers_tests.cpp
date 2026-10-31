/// @file
/// Tests for manifest stages and type chart tiers (D-22 A6 / A7, E6a) against the battle data compiled from
/// tests/data/battle_tiers/battle.json: a sequential chart with non-power-of-two tiers (x0.625, x1.75) and decisive
/// tiers (immune, fatal) over species with two types; stages with their own ranges (accuracy -3..3, a power stage
/// -2..3 with its own table, crit 0..2 with its own odds) and a formula stage (pace: v + v * stage / 4).
#include "capsule/battle/battle.h"
#include "capsule/battle/pipeline.h"
#include "capsule/battle/ruleset.h"
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
using namespace capsule::battle;
using namespace battle_ids;

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

State g_state;   ///< Battle state (large; static storage).

/// effectiveness() of a decisive Ko tier.
constexpr int32_t kKo = 0x7FFF;
/// The battlers of the 1-on-1 battle.
/// Message id of super_effective (battlec.texts.MESSAGES; an optional message the engine's Msg does not name, C4).
constexpr uint8_t kMsgSuperEffective = 8;
constexpr uint8_t kUser = 0;
constexpr uint8_t kFoe = 1;

/// Starts the data parties' battle: Left's Ember (Hot) against Right's Slush (Cold, Hot).
/// @param human  the player answers requests (else both are AI)
void start_battle(bool human)
{
   Setup su{};
   su.participants[0] = Participant{&kData.parties[PARTY_LEFT], nullptr, nullptr,
                                    human ? Controller::Human : Controller::Ai, 0, 0, 0};
   su.participants[1] = Participant{&kData.parties[PARTY_RIGHT], nullptr, nullptr, Controller::Ai, 1, 0, 0};
   su.count = 2;
   su.format = FORMAT_SINGLE;
   su.weather = kNone8;
   su.seed = 2;   // with the engine's default generator, Ember lives through three turns (E11: was 7 for Method 1)
   CHECK(start(g_state, su));
}

/// The compiled tiers and stages.
void test_tables()
{
   CHECK(kData.tier_count == NUM_TIERS && NUM_TIERS == 5);
   CHECK(kData.combine == static_cast<uint8_t>(Combine::Sequential));
   CHECK(kData.species_types == 2 && kData.added_types == 1);
   CHECK(kData.tiers[TIER_STRONG].q8 == 448 && kData.tiers[TIER_WEAK].q8 == 160);
   CHECK(kData.tiers[TIER_STRONG].kind == static_cast<uint8_t>(TierKind::Scale));
   CHECK(kData.tiers[TIER_IMMUNE].kind == static_cast<uint8_t>(TierKind::Zero));
   CHECK(kData.tiers[TIER_IMMUNE].message == static_cast<uint8_t>(Msg::NoEffect));
   CHECK(kData.tiers[TIER_FATAL].kind == static_cast<uint8_t>(TierKind::Ko));
   CHECK(chart(TYPE_COLD, TYPE_HOT) == TIER_STRONG && chart(TYPE_PLAIN, TYPE_HOT) == TIER_EVEN);   // 1.75; default
   CHECK(kData.above_message == kMsgSuperEffective);

   CHECK(kData.stage_count == NUM_STAGES && NUM_STAGES == 5);
   CHECK(kData.accuracy_stage == STAGE_ACCURACY && kData.evasion_stage == STAGE_EVASION);
   CHECK(kData.crit_stage == STAGE_CRITICAL && kData.stat_stage == kNone8);   // the stat stages differ
   const StageData& power = kData.stages[STAGE_POWER];
   CHECK(power.lo == -2 && power.hi == 3 && power.table != nullptr && power.stat == STAT_POWER);
   const StageData& pace = kData.stages[STAGE_PACE];
   CHECK(pace.table == nullptr && pace.formula == 0 && stage_of(STAT_PACE) == STAGE_PACE);
   CHECK(kData.stages[STAGE_CRITICAL].odds != nullptr && kData.stages[STAGE_CRITICAL].hi == 2);
   CHECK(stage_of(STAT_VIGOR) == kNone8);
}

/// effectiveness() and pipeline_type_scale(): decisive tiers first in type order, the scaling tiers in turn.
void test_chart()
{
   start_battle(false);
   Battler& foe = g_state.battlers[kFoe];
   CHECK(foe.species == SPECIES_SLUSH && foe.types[0] == TYPE_COLD && foe.types[1] == TYPE_HOT);
   CHECK(foe.types[2] == kNone8);
   CHECK(effectiveness(g_state, TYPE_HOT, kFoe) == 280);    // x1.75 then x0.625
   CHECK(effectiveness(g_state, TYPE_COLD, kFoe) == 280);   // x0.625 then x1.75
   CHECK(effectiveness(g_state, TYPE_PLAIN, kFoe) == 256);

   // sequential: 1003 x1.75 = 1755, x0.625 = 1096 (one combined x1.09375 factor would give 1097)
   PipelineCtx c{&g_state, nullptr, kUser, kFoe, MOVE_BLAZE, kNoFixedRoll, 0, kNone8, kNone8, kNoDraw};
   CHECK(pipeline_type_scale(c, 1003) == 1096);
   c.move = MOVE_TAP;
   CHECK(pipeline_type_scale(c, 1003) == 1003);
   c.move = kNone16;
   CHECK(pipeline_type_scale(c, 1003) == 1003);   // no move: x1

   // decisive tiers win in the defender's type order: Void first
   const uint8_t saved[kMaxTypesPer] = {foe.types[0], foe.types[1], foe.types[2]};
   foe.types[0] = TYPE_VOID;
   foe.types[1] = TYPE_COLD;
   CHECK(effectiveness(g_state, TYPE_HOT, kFoe) == kKo);
   CHECK(effectiveness(g_state, TYPE_PLAIN, kFoe) == 0);
   CHECK(damage_preview(g_state, kUser, kFoe, MOVE_TAP, false, 0, false) == 0);
   CHECK(damage_preview(g_state, kUser, kFoe, MOVE_BLAZE, false, 0, false) == foe.hp);
   foe.types[2] = TYPE_HOT;   // an added type joins the matchup
   foe.types[0] = TYPE_PLAIN;
   CHECK(effectiveness(g_state, TYPE_HOT, kFoe) == 280);   // even, strong, weak
   for (int k = 0; k < kMaxTypesPer; ++k)
   {
      foe.types[k] = saved[k];
   }
   CHECK(damage_preview(g_state, kUser, kFoe, MOVE_BLAZE, false, 0, false) >
         damage_preview(g_state, kUser, kFoe, MOVE_TAP, false, 0, false));
}

/// Stage tables, formula stages and odds, each clamped to its own range.
void test_stages()
{
   start_battle(false);
   CHECK(pipeline_stage_scale(STAGE_POWER, 100, 3) == 200);
   CHECK(pipeline_stage_scale(STAGE_POWER, 100, 9) == 200);    // clamped to 3
   CHECK(pipeline_stage_scale(STAGE_POWER, 100, -5) == 50);    // clamped to -2
   CHECK(pipeline_stage_scale(STAGE_POWER, 100, 1) == 125);
   CHECK(pipeline_stage_scale(kNone8, 100, 3) == 100);         // no stage: x1
   CHECK(pipeline_stage_scale(STAGE_PACE, 100, 2) == 150);     // the formula: v + v * stage / 4
   CHECK(pipeline_stage_scale(STAGE_PACE, 100, -9) == 25);     // clamped to -3

   // speed: the order stat through its formula stage
   Battler& b = g_state.battlers[kUser];
   const int32_t pace = b.stats[STAT_PACE];
   b.stages[STAGE_PACE] = 2;
   CHECK(speed_preview(g_state, kUser) == pace + pace * 2 / 4);
   b.stages[STAGE_PACE] = 0;
   CHECK(speed_preview(g_state, kUser) == pace);

   // crit odds: Blaze adds 1 to the crit stage; the odds clamp to the stage's range 0..2
   Odds o = crit_chance_preview(g_state, kUser, MOVE_BLAZE);
   CHECK(o.n == 1 && o.m == 6);
   b.stages[STAGE_CRITICAL] = 5;
   o = crit_chance_preview(g_state, kUser, MOVE_BLAZE);
   CHECK(o.n == 1 && o.m == 3);
   o = crit_chance_preview(g_state, kUser, MOVE_TAP);
   CHECK(o.n == 1 && o.m == 3);
   b.stages[STAGE_CRITICAL] = 0;

   // accuracy: the accuracy stage's table over accuracy minus evasion, clamped to -3..3
   o = hit_chance_preview(g_state, kUser, kFoe, MOVE_BLAZE);
   CHECK(o.n == 90 && o.m == 100);
   b.stages[STAGE_ACCURACY] = 2;
   g_state.battlers[kFoe].stages[STAGE_EVASION] = -3;   // 5: clamped to 3, x7/4
   o = hit_chance_preview(g_state, kUser, kFoe, MOVE_BLAZE);
   CHECK(o.n == 157);
   b.stages[STAGE_ACCURACY] = -3;
   g_state.battlers[kFoe].stages[STAGE_EVASION] = 0;
   o = hit_chance_preview(g_state, kUser, kFoe, MOVE_BLAZE);
   CHECK(o.n == 22);
}

/// A battle: Surge raises power by 2 three times; the stage stops at its range's top (3) with the max message.
void test_stage_range_in_battle()
{
   start_battle(true);
   constexpr int kStepLimit = 2000;
   constexpr uint8_t kSurgeSlot = 0;
   constexpr int kSurges = 3;
   int surges = 0;
   int maxed = 0;
   Progress p = Progress::Busy;
   for (int steps = 0; steps < kStepLimit && p != Progress::Ended; ++steps)
   {
      Event e;
      while (pop_event(g_state, e))
      {
         if (e.kind == EventKind::Message && e.a == static_cast<uint16_t>(Msg::StatMax) && e.extra == STAGE_POWER)
         {
            ++maxed;
         }
      }
      p = step(g_state);
      if (p == Progress::Input)
      {
         if (surges == kSurges)
         {
            break;
         }
         Request r;
         CHECK(request(g_state, r));
         CHECK(choose(g_state, r.battler, Action{ActionKind::Move, kSurgeSlot, kBench, 0}));
         ++surges;
      }
   }
   CHECK(surges == kSurges);
   CHECK(g_state.battlers[kUser].hp > 0);   // the seed lets Ember live through three turns
   CHECK(g_state.battlers[kUser].stages[STAGE_POWER] == 3);   // 2, then 3 (not 4), then 3 again
   CHECK(maxed == 1);
}

} // namespace

int main()
{
   test_tables();
   test_chart();
   test_stages();
   test_stage_range_in_battle();
   std::printf("battle_tiers_tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
