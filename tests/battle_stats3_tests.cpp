/// @file
/// Tests for manifest-defined stats (D-20 C) against the battle data compiled
/// from tests/data/battle_stats3/battle.json: three stats (vigor is the pool
/// stat, power the offense and defense stat, pace the order stat), 4-bit genes
/// chosen by the layout, and a short battle that raises a stage of power. Its
/// layout is v2 (D-23 section 2): six move slots, parties of three, 3-bit PP
/// raises and the custom roles gender, ribbons (32 x 1 bit) and met_level.
#include "capsule/battle/battle.h"
#include "capsule/battle/ruleset.h"
#include "capsule/monster/monster.h"

#include "battle_ids.h"

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

State g_state;   ///< Battle state (large; static storage).

/// Whether a text equals an ASCII string.
/// @param codes  text (kCodeEnd-terminated)
/// @param ascii  expected
/// @return true if equal
bool same_text(const text::Code* codes, const char* ascii)
{
   int k = 0;
   for (; ascii[k] != '\0'; ++k)
   {
      if (codes == nullptr || codes[k] != static_cast<text::Code>(ascii[k]))
      {
         return false;
      }
   }
   return codes != nullptr && codes[k] == text::kCodeEnd;
}

/// The compiled stat table, stages and record layout.
void test_tables()
{
   CHECK(NUM_STATS == 3 && kData.stat_count == NUM_STATS);
   CHECK(kData.pool_stat == STAT_VIGOR && kData.order_stat == STAT_PACE);
   CHECK(same_text(kData.stats[STAT_POWER].label, "Power"));
   CHECK(stage_of(STAT_VIGOR) == kNone8);   // the pool stat has no stage
   CHECK(stage_of(STAT_POWER) == STAGE_POWER && stage_of(STAT_PACE) == STAGE_PACE);
   CHECK(kData.stage_count == NUM_STAGES && NUM_STAGES == 5);
   CHECK(kData.crit_stage == STAGE_CRITICAL && same_text(kData.stages[STAGE_CRITICAL].name, "focus"));
   CHECK(same_text(kData.stages[stage_of(STAT_PACE)].name, "Pace"));
   CHECK(kData.stages[STAGE_PACE].kind == static_cast<uint8_t>(StageKind::Stat));
   CHECK(kData.stages[STAGE_PACE].stat == STAT_PACE && kData.stat_stage == STAGE_POWER);
   CHECK((kData.stats[STAT_PACE].flags & (1u << STATINFO_TRAIT)) != 0);
   CHECK((kData.stats[STAT_VIGOR].flags & (1u << STATINFO_TRAIT)) == 0);

   // per-stat roles hold one element per stat; the layout chose 4-bit genes
   CHECK(monster::kLayout.stat_count == 3 && monster::kLayout.record_bytes == 56);
   CHECK(monster::count(Role::StatGenes) == 3 && monster::width(Role::StatGenes) == 4);
   CHECK(monster::max_value(Role::StatGenes) == 15);
   monster::Record r{};
   monster::set(r, Role::StatGenes, 2, 0x1F);   // truncated to 4 bits
   monster::set(r, Role::StatGenes, 3, 9);      // no fourth stat: ignored
   CHECK(monster::get(r, Role::StatGenes, 2) == 15 && monster::get(r, Role::StatGenes, 3) == 0);
}

/// Layout v2: custom roles by ID and name, six move slots, parties of three, a chosen PP-raise width.
void test_layout_v2()
{
   CHECK(monster::kLayout.move_slots == 6 && move_slots() == 6 && monster::kLayout.party_size == 3);
   CHECK(monster::count(Role::Moves) == 6 && monster::count(Role::Pp) == 6 && monster::count(Role::PpUps) == 6);
   CHECK(monster::width(Role::PpUps) == 3 && monster::count(Role::Nickname) == 8);
   CHECK(NUM_CUSTOM_ROLES == 3 && monster::kLayout.custom_count == 3);
   CHECK(ROLE_GENDER == monster::kFirstCustomRole && ROLE_RIBBONS == ROLE_GENDER + 1);
   CHECK(ROLE_MET_LEVEL == ROLE_GENDER + 2 && ROLE_MOVES == monster::id_of(Role::Moves));
   CHECK(monster::find_custom(monster::name_hash("ribbons")) == ROLE_RIBBONS);
   CHECK(monster::count(ROLE_RIBBONS) == 32 && monster::width(ROLE_RIBBONS) == 1);
   CHECK(monster::max_value(ROLE_GENDER) == 3 && monster::initial(ROLE_MET_LEVEL) == 5);
   CHECK(monster::initial(ROLE_GENDER) == 0 && monster::initial(Role::Level) == 0);

   monster::Record r{};
   monster::reset(r);
   CHECK(monster::get(r, ROLE_MET_LEVEL) == 5 && monster::get(r, ROLE_GENDER) == 0);
   // every ribbon is its own bit; the 32 of them fill one 32-bit word of the record
   constexpr int kRibbons = 32;
   for (int i = 0; i < kRibbons; i += 3)
   {
      monster::set(r, ROLE_RIBBONS, i, 1);
   }
   int on = 0;
   for (int i = 0; i < kRibbons; ++i)
   {
      on += static_cast<int>(monster::get(r, ROLE_RIBBONS, i));
      CHECK(monster::get(r, ROLE_RIBBONS, i) == (i % 3 == 0 ? 1u : 0u));
   }
   CHECK(on == 11);
   monster::set(r, ROLE_RIBBONS, kRibbons, 1);   // past its count: ignored
   CHECK(monster::get(r, ROLE_RIBBONS, kRibbons) == 0);
   monster::set(r, ROLE_GENDER, 0, 6);   // truncated to 2 bits
   CHECK(monster::get(r, ROLE_GENDER) == 2 && monster::get(r, ROLE_MET_LEVEL) == 5);
   CHECK(monster::get(r, monster::kFirstCustomRole + 3) == 0 && !monster::has(monster::kFirstCustomRole + 3));

   // a new creature's custom roles take their defaults; its moves are the learnset's last six by level
   rng::Stream rs{3};
   CHECK(make_monster(r, SPECIES_BLOB, 12, rs));
   CHECK(monster::get(r, ROLE_MET_LEVEL) == 5 && monster::get(r, ROLE_RIBBONS, 0) == 0);
   const uint16_t at12[] = {MOVE_TAP, MOVE_BRACE, MOVE_POKE, MOVE_NUDGE, MOVE_WIGGLE, MOVE_HUM};
   for (int i = 0; i < move_slots(); ++i)
   {
      CHECK(monster::get(r, Role::Moves, i) == at12[i]);
      CHECK(static_cast<int32_t>(monster::get(r, Role::Pp, i)) == move_base(at12[i], RESOURCE_PP));
   }
   CHECK(make_monster(r, SPECIES_BLOB, 30, rs));
   CHECK(monster::get(r, Role::Moves, 0) == MOVE_BRACE && monster::get(r, Role::Moves, 5) == MOVE_BOP);

   // a party of three, each using its sixth slot once; slot 6 is refused; the moves are written back
   monster::Party player{};
   for (int k = 0; k < monster::kLayout.party_size; ++k)
   {
      CHECK(make_monster(player.members[k], SPECIES_BLOB, 12, rs));
      monster::set(player.members[k], Role::PpUps, 5, 7);   // 3-bit PP raises: Hum gets 7 quarters more
   }
   player.count = 3;
   Setup su{};
   su.participants[0] = Participant{nullptr, &player, nullptr, Controller::Human, 0, 0, 0};
   su.participants[1] = Participant{&kData.parties[PARTY_RIVAL], nullptr, nullptr, Controller::Ai, 1, 0, 0};
   su.count = 2;
   su.format = FORMAT_SINGLE;
   su.weather = kNone8;
   su.seed = 5;
   CHECK(start(g_state, su));
   CHECK(g_state.parties[0].count == 3);
   const int32_t hum_base = move_base(MOVE_HUM, RESOURCE_PP);
   constexpr int32_t kPpShare = 4;   // the ruleset's PP_SHARE: each raise adds a quarter of the base PP
   const uint8_t hum_pp = static_cast<uint8_t>(hum_base + hum_base * 7 / kPpShare);
   CHECK(resource_max(g_state.battlers[0], RESOURCE_PP, 5) == hum_pp && g_state.battlers[0].moves[5] == MOVE_HUM);
   CHECK(g_state.battlers[0].moves[6] == kNone16);   // past the layout's slots
   CHECK(move_usable(g_state, 0, 5) && !move_usable(g_state, 0, 6));
   constexpr int kStepLimit = 2000;
   constexpr uint8_t kHumSlot = 5;
   bool refused = false;
   Progress p = Progress::Busy;
   for (int steps = 0; steps < kStepLimit && p != Progress::Ended; ++steps)
   {
      Event e;
      while (pop_event(g_state, e))
      {
      }
      p = step(g_state);
      if (p == Progress::Input)
      {
         Request q;
         CHECK(request(g_state, q));
         if (!refused)
         {
            refused = true;
            CHECK(!choose(g_state, q.battler, Action{ActionKind::Move, kHumSlot + 1, kBench, 0}));
         }
         CHECK(choose(g_state, q.battler, Action{ActionKind::Move, kHumSlot, kBench, 0}));
      }
   }
   CHECK(p == Progress::Ended && g_state.outcome == Outcome::Victory);
   CHECK(monster::get(player.members[0], Role::Pp, 5) < hum_pp);   // written back: Hum was used
   CHECK(monster::get(player.members[0], Role::Moves, 5) == MOVE_HUM);
}

/// record_stats() runs each stat's formula (battle_compiler.preview_stats gives the same numbers).
void test_record_stats()
{
   monster::Record r{};
   rng::Stream rs{3};
   CHECK(make_monster(r, SPECIES_BLOB, 10, rs));
   for (int i = 0; i < NUM_STATS; ++i)
   {
      CHECK(monster::get(r, Role::StatGenes, i) <= 15);
   }
   const uint32_t genes[NUM_STATS] = {15, 3, 7};
   for (int i = 0; i < NUM_STATS; ++i)
   {
      monster::set(r, Role::StatGenes, i, genes[i]);
   }
   monster::set(r, Role::Trait, TRAIT_QUICK);
   uint16_t st[kMaxStats];
   CHECK(record_stats(r, st));
   // tests/test_battle_compiler.py: preview_stats(stats3, "Blob", 10, genes=[15, 3, 7], trait="Quick")
   const uint16_t expect[NUM_STATS] = {45, 14, 16};
   for (int i = 0; i < NUM_STATS; ++i)
   {
      CHECK(st[i] == expect[i]);
   }
}

/// A short battle: the player braces (power +1), then taps until the rival's creature faints.
void test_battle()
{
   monster::Party player{};
   rng::Stream rs{11};
   CHECK(make_monster(player.members[0], SPECIES_BLOB, 12, rs));
   player.count = 1;
   Setup su{};
   su.participants[0] = Participant{nullptr, &player, nullptr, Controller::Human, 0, 0, 0};
   su.participants[1] = Participant{&kData.parties[PARTY_RIVAL], nullptr, nullptr, Controller::Ai, 1, 0, 0};
   su.count = 2;
   su.format = FORMAT_SINGLE;
   su.weather = kNone8;
   su.seed = 5;
   CHECK(start(g_state, su));
   constexpr int kStepLimit = 2000;
   constexpr int kTextCap = 96;
   constexpr uint8_t kTapSlot = 0;
   constexpr uint8_t kBraceSlot = 1;
   bool braced = false;
   bool power_text = false;
   int steps = 0;
   Progress p = Progress::Busy;
   for (; steps < kStepLimit && p != Progress::Ended; ++steps)
   {
      Event e;
      while (pop_event(g_state, e))
      {
         if (e.kind == EventKind::Message && e.a == static_cast<uint16_t>(Msg::StatUp))
         {
            text::Code buf[kTextCap];
            CHECK(format_text(g_state, e, buf, kTextCap));
            CHECK(e.extra == stage_of(STAT_POWER));
            // the {stat} placeholder names the staged stat by its label
            const char* want = "Power";
            const int len = static_cast<int>(std::strlen(want));
            for (int k = 0; buf[k] != text::kCodeEnd && k + len <= kTextCap; ++k)
            {
               power_text = power_text || std::memcmp(&buf[k], want, static_cast<size_t>(len)) == 0;
            }
         }
      }
      p = step(g_state);
      if (p == Progress::Input)
      {
         Request r;
         CHECK(request(g_state, r));
         const uint8_t slot = braced ? kTapSlot : kBraceSlot;
         CHECK(choose(g_state, r.battler, Action{ActionKind::Move, slot, kBench, 0}));
         braced = true;
      }
   }
   CHECK(p == Progress::Ended && g_state.outcome == Outcome::Victory);
   CHECK(power_text);
   const Battler& b = g_state.battlers[0];
   CHECK(b.stages[stage_of(STAT_POWER)] == 1);
   CHECK(max_hp(b) == b.stats[STAT_VIGOR] && b.hp <= max_hp(b));
}

/// The three-stat ruleset's pipelines (no points): speed is the staged order stat, hits use the move's
/// accuracy, crit odds come from the table, damage grows with the roll and the critical hit.
void test_pipelines()
{
   monster::Party a{};
   monster::Party d{};
   rng::Stream rs{3};
   CHECK(make_monster(a.members[0], SPECIES_BLOB, 30, rs));
   CHECK(make_monster(d.members[0], SPECIES_BLOB, 30, rs));
   a.count = 1;
   d.count = 1;
   Setup su{};
   su.participants[0] = Participant{nullptr, &a, nullptr, Controller::Human, 0, 0, 0};
   su.participants[1] = Participant{nullptr, &d, nullptr, Controller::Ai, 1, 1u << PARTY_UNOWNED, 0};
   su.count = 2;
   su.format = FORMAT_SINGLE;
   su.weather = kNone8;
   su.seed = 9;
   CHECK(start(g_state, su));
   constexpr int kRollHigh = 16;   // the ruleset's damage roll: 17 outcomes
   constexpr uint16_t kSure = 100;
   constexpr uint16_t kCritOutOf = 20;   // crit stage 0: 1/20
   const Battler& b = g_state.battlers[0];
   CHECK(speed_preview(g_state, 0) == b.stats[STAT_PACE]);
   const Odds hit = hit_chance_preview(g_state, 0, 1, MOVE_TAP);
   CHECK(hit.n == kSure && hit.m == kSure);
   const Odds crit = crit_chance_preview(g_state, 0, MOVE_TAP);
   CHECK(crit.n == 1 && crit.m == kCritOutOf);
   const uint32_t before = g_state.rng.state;
   const int32_t low = damage_preview(g_state, 0, 1, MOVE_TAP, false, 0, false);
   const int32_t high = damage_preview(g_state, 0, 1, MOVE_TAP, false, kRollHigh, false);
   const int32_t critical = damage_preview(g_state, 0, 1, MOVE_TAP, false, kRollHigh, true);
   CHECK(low >= 1 && low <= high && high < critical);
   CHECK(g_state.rng.state == before);   // previews take fixed rolls: no draw
}

} // namespace

/// Runs the tests.
/// @return 0 if every check passed
int main()
{
   test_tables();
   test_layout_v2();
   test_record_stats();
   test_battle();
   test_pipelines();
   std::printf("battle stats3 tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
