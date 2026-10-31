/// @file
/// Tests for a layout v2 with fewer move slots (D-23 section 2) against the
/// battle data compiled from tests/data/battle_stats3/slots2.json: the
/// three-stat data with two move slots, a 5-code nickname and 7-bit levels.
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

/// The layout: two move slots, a 5-code nickname, 7-bit levels, no custom roles.
void test_layout()
{
   CHECK(monster::kLayout.move_slots == 2 && move_slots() == 2 && monster::kLayout.party_size == 6);
   CHECK(monster::count(Role::Moves) == 2 && monster::count(Role::Pp) == 2 && monster::count(Role::Nickname) == 5);
   CHECK(monster::width(Role::Level) == 7 && monster::max_value(Role::Level) == 127);
   CHECK(monster::kLayout.custom_count == 0 && NUM_CUSTOM_ROLES == 0);
   CHECK(monster::find_custom(monster::name_hash("gender")) == monster::kNoRole);
   monster::Record r{};
   monster::set(r, Role::Moves, 2, MOVE_TAP);   // no third slot: ignored
   CHECK(monster::get(r, Role::Moves, 2) == 0);
   monster::set(r, Role::Level, 200);   // truncated to 7 bits
   CHECK(monster::get(r, Role::Level) == 200 - 128);
}

/// New records hold the learnset's last two moves; battles use two slots and write two back.
void test_two_slots()
{
   monster::Party player{};
   rng::Stream rs{7};
   CHECK(make_monster(player.members[0], SPECIES_BLOB, 12, rs));
   CHECK(monster::get(player.members[0], Role::Moves, 0) == MOVE_WIGGLE);
   CHECK(monster::get(player.members[0], Role::Moves, 1) == MOVE_HUM);
   // a nickname of the layout's five codes names the battler
   const char* nick = "Gloop";
   for (int i = 0; i < monster::count(Role::Nickname); ++i)
   {
      monster::set(player.members[0], Role::Nickname, i, static_cast<uint8_t>(nick[i]));
   }
   player.count = 1;
   Setup su{};
   su.participants[0] = Participant{nullptr, &player, nullptr, Controller::Human, 0, 0, 0};
   su.participants[1] = Participant{&kData.parties[PARTY_RIVAL], nullptr, nullptr, Controller::Ai, 1, 0, 0};
   su.count = 2;
   su.format = FORMAT_SINGLE;
   su.weather = kNone8;
   su.seed = 3;
   CHECK(start(g_state, su));
   const Battler& b = g_state.battlers[0];
   CHECK(std::memcmp(b.name, nick, std::strlen(nick)) == 0 && b.name[std::strlen(nick)] == text::kCodeEnd);
   CHECK(b.moves[0] == MOVE_WIGGLE && b.moves[1] == MOVE_HUM && b.moves[2] == kNone16);
   CHECK(move_usable(g_state, 0, 1) && !move_usable(g_state, 0, 2));
   // the rival's member data names no moves: the learnset's last two by level (Cube knows only Tap)
   CHECK(g_state.battlers[1].moves[0] == MOVE_TAP && g_state.battlers[1].moves[1] == kNone16);
   constexpr int kStepLimit = 2000;
   constexpr uint8_t kSecondSlot = 1;
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
         CHECK(!choose(g_state, q.battler, Action{ActionKind::Move, kSecondSlot + 1, kBench, 0}));
         CHECK(choose(g_state, q.battler, Action{ActionKind::Move, kSecondSlot, kBench, 0}));
      }
   }
   CHECK(p == Progress::Ended && g_state.outcome == Outcome::Victory);
   CHECK(static_cast<int32_t>(monster::get(player.members[0], Role::Pp, 1)) < move_base(MOVE_HUM, RESOURCE_PP));
   CHECK(static_cast<int32_t>(monster::get(player.members[0], Role::Pp, 0)) == move_base(MOVE_WIGGLE, RESOURCE_PP));
}

} // namespace

/// Runs the tests.
/// @return 0 if every check passed
int main()
{
   test_layout();
   test_two_slots();
   std::printf("battle stats3 slots2 tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
