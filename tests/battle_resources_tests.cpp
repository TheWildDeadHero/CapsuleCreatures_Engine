/// @file
/// Tests for manifest resources (D-25, E8) against the battle data compiled from
/// tests/data/battle_resources/battle.json: hit points (the faint resource, persisted in current_hp), MP
/// (persisted in the custom role mp_left) and EP (refilled every battle, regenerating a quarter of its maximum in
/// the turn's regen end-of-turn phase); move costs as numbers (Spark: 4 MP) and as a formula (Rush: half the EP
/// maximum); Burst costs more MP than any battler holds and is never selectable (player or AI); Tap is the free
/// fallback move; Refresh restores MP through the effect VM's Restore op and Drain lowers the target's MP through
/// a resource write; Sacrifice costs 5 hit points and is usable only with more (paying never faints, C1). The
/// layout holds no PP roles at all.
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

/// Events one test keeps.
constexpr int kLogEvents = 2048;
/// Most steps one run takes.
constexpr int kStepLimit = 4000;
/// The battle's seed.
constexpr uint32_t kSeed = 4242;
/// Seed of the stream new records are made from.
constexpr uint32_t kRecordSeed = 777;
/// Level of the player's creature.
constexpr uint8_t kLevel = 20;
/// Level of the player's creature in the AI test (a sturdy target that only uses Refresh).
constexpr uint8_t kSturdyLevel = 50;
/// MP the player's record holds when the first battle starts (below Spark's cost).
constexpr uint32_t kLowMp = 3;
/// Spark's MP cost (the fixture's).
constexpr int32_t kSparkCost = 4;
/// What Refresh restores (the fixture's logic).
constexpr int32_t kRefreshMp = 6;
/// Refresh's EP cost.
constexpr int32_t kRefreshCost = 1;
/// What Drain takes from the target (the fixture's logic).
constexpr int32_t kDrainMp = 5;
/// The fixture's ep_regen: a quarter of the maximum; rush_cost: half of it.
constexpr int32_t kRegenShare = 4;
constexpr int32_t kRushShare = 2;
/// Sacrifice's hit point cost (the fixture's).
constexpr int32_t kSacrificeCost = 5;
/// Turns the AI test runs (the AI's MP covers four Sparks).
constexpr int kAiTurns = 6;
/// The player's battler and the foe's (single format: one battler each).
constexpr uint8_t kMine = 0;
constexpr uint8_t kFoe = 1;
/// The player's move slots (first_n: Spark, Rush, Refresh, Drain).
constexpr uint8_t kSlotSpark = 0;
constexpr uint8_t kSlotRush = 1;
constexpr uint8_t kSlotRefresh = 2;
constexpr uint8_t kSlotDrain = 3;

State g_state;               ///< Battle state (large; static storage).
monster::Party g_player{};   ///< The player's records.

/// What the battle showed.
struct Log
{
   Event events[kLogEvents];   ///< Events in order.
   int count;                  ///< Events kept.
};

Log g_log;   ///< The current battle's events.

/// Moves the queued events into the log.
void drain()
{
   Event e;
   while (pop_event(g_state, e))
   {
      if (g_log.count < kLogEvents)
      {
         g_log.events[g_log.count++] = e;
      }
   }
}

/// How often the log holds a message.
/// @param msg      message
/// @param battler  its battler
/// @param value    its value (move)
/// @return times shown
int said(Msg msg, uint8_t battler, uint16_t value)
{
   int n = 0;
   for (int k = 0; k < g_log.count; ++k)
   {
      const Event& e = g_log.events[k];
      n += e.kind == EventKind::Message && e.a == static_cast<uint16_t>(msg) && e.battler == battler && e.b == value
              ? 1
              : 0;
   }
   return n;
}

/// Whether the log holds a Resource event of a battler and resource.
/// @param battler   the battler
/// @param resource  the resource
/// @return true if shown
bool resource_event(uint8_t battler, uint8_t resource)
{
   for (int k = 0; k < g_log.count; ++k)
   {
      const Event& e = g_log.events[k];
      if (e.kind == EventKind::Resource && e.battler == battler && e.extra == resource)
      {
         return true;
      }
   }
   return false;
}

/// Runs the battle until it needs a human choice (or ends).
/// @param r  the request
/// @return true when a request is pending
bool until_request(Request& r)
{
   for (int k = 0; k < kStepLimit; ++k)
   {
      drain();
      const Progress p = step(g_state);
      drain();
      if (p == Progress::Ended)
      {
         return false;
      }
      if (p == Progress::Input && request(g_state, r))
      {
         return true;
      }
   }
   return false;
}

/// Starts the player's records against a data party.
/// @param foe  the AI's party
/// @return true if it started
bool begin(uint16_t foe)
{
   g_log = Log{};
   Setup su{};
   su.format = FORMAT_SINGLE;
   su.weather = kNone8;
   su.seed = kSeed;
   su.participants[su.count++] = Participant{nullptr, &g_player, nullptr, Controller::Human, 0, 0, 0};
   su.participants[su.count++] = Participant{&kData.parties[foe], nullptr, nullptr, Controller::Ai, 1, 0, 0};
   return start(g_state, su);
}

/// Plays one turn: waits for the player's action request and gives a move slot.
/// @param slot  move slot or kFallbackSlot
/// @return true when the choice was taken
bool play(uint8_t slot)
{
   Request q{};
   return until_request(q) && q.kind == RequestKind::Action && choose(g_state, q.battler, Action{ActionKind::Move,
                                                                                                  slot, kBench, 0});
}

/// The data: three resources in order, the faint one first, no move resources and no PP roles.
void test_data()
{
   CHECK(kData.resource_count == NUM_RESOURCES && NUM_RESOURCES == 3);
   CHECK(kData.faint_resource == RESOURCE_HP && kData.move_resource_count == 0);
   CHECK(kData.resource_count - kData.move_resource_count - 1 == 2);   // battler pools besides the hit points
   CHECK(kData.resources[RESOURCE_HP].role == monster::id_of(Role::CurrentHp));
   CHECK(kData.resources[RESOURCE_MP].role == ROLE_MP_LEFT);
   CHECK(kData.resources[RESOURCE_EP].role == monster::kNoRole);   // refilled every battle
   CHECK(kData.resources[RESOURCE_EP].regen_formula != kNone8);
   CHECK(!monster::has(Role::Pp) && !monster::has(Role::PpUps));
   CHECK(kData.moves[MOVE_TAP].cost_count == 0 && kData.rules.fallback_move == MOVE_TAP);
   CHECK(kData.moves[MOVE_SPARK].cost_count == 1);
   const MoveCost& spark = kData.move_costs[kData.moves[MOVE_SPARK].cost_first];
   CHECK(spark.resource == RESOURCE_MP && spark.amount == kSparkCost && spark.formula == kNone8);
   const MoveCost& rush = kData.move_costs[kData.moves[MOVE_RUSH].cost_first];
   CHECK(rush.resource == RESOURCE_EP && rush.formula != kNone8);
}

/// A new record's persisted pools are full (make_monster(): fill_resources()).
void test_new_record()
{
   rng::Stream r{kRecordSeed};
   monster::Record rec{};
   CHECK(make_monster(rec, SPECIES_BLOB, kLevel, r));
   uint16_t stats[kMaxStats] = {};
   CHECK(record_stats(rec, stats));
   CHECK(monster::get(rec, ROLE_MP_LEFT) == stats[STAT_MANA]);
   CHECK(monster::get(rec, Role::CurrentHp) == stats[STAT_VIGOR]);
}

/// Persist vs refill, costs (a number, a formula), an unpayable move, regen, the VM's Restore and a resource
/// write, and the write-back.
void test_battle()
{
   g_player = monster::Party{};
   rng::Stream r{kRecordSeed};
   CHECK(make_monster(g_player.members[0], SPECIES_BLOB, kLevel, r));
   g_player.count = 1;
   monster::set(g_player.members[0], ROLE_MP_LEFT, 0, kLowMp);
   CHECK(begin(PARTY_DUMMY));
   const Battler& me = g_state.battlers[kMine];
   const Battler& foe = g_state.battlers[kFoe];
   const int32_t ep_max = resource_max(me, RESOURCE_EP);

   // MP comes from the record, EP starts full (no persist role)
   CHECK(resource_value(me, RESOURCE_MP) == static_cast<int32_t>(kLowMp));
   CHECK(resource_value(me, RESOURCE_EP) == ep_max && ep_max == me.stats[STAT_STAMINA]);
   CHECK(resource_value(me, RESOURCE_HP) == me.hp && resource_max(me, RESOURCE_HP) == max_hp(me));

   // Spark costs 4 MP: not selectable with 3; the other moves are
   Request q{};
   CHECK(until_request(q) && q.kind == RequestKind::Action);
   CHECK(!move_usable(g_state, kMine, kSlotSpark));
   CHECK(move_usable(g_state, kMine, kSlotRush) && move_usable(g_state, kMine, kSlotRefresh));
   CHECK(move_usable(g_state, kMine, kSlotDrain));
   CHECK(!choose(g_state, kMine, Action{ActionKind::Move, kSlotSpark, kBench, 0}));
   CHECK(!choose(g_state, kMine, Action{ActionKind::Move, kFallbackSlot, kBench, 0}));   // a move is usable

   // Rush pays half the EP maximum (a formula); the regen phase gives back a quarter
   CHECK(choose(g_state, kMine, Action{ActionKind::Move, kSlotRush, kBench, 0}));
   CHECK(until_request(q));
   CHECK(resource_value(me, RESOURCE_EP) == ep_max - ep_max / kRushShare + ep_max / kRegenShare);
   CHECK(resource_event(kMine, RESOURCE_EP));
   CHECK(said(Msg::UsedMove, kMine, MOVE_RUSH) == 1);

   // Refresh (1 EP) restores 6 MP through effect logic: Spark is usable again
   const int32_t ep_before = resource_value(me, RESOURCE_EP);
   CHECK(choose(g_state, kMine, Action{ActionKind::Move, kSlotRefresh, kBench, 0}));
   CHECK(until_request(q));
   CHECK(resource_value(me, RESOURCE_MP) == static_cast<int32_t>(kLowMp) + kRefreshMp);
   const int32_t ep_regen = ep_before - kRefreshCost + ep_max / kRegenShare;
   CHECK(resource_value(me, RESOURCE_EP) == (ep_regen < ep_max ? ep_regen : ep_max));
   CHECK(resource_event(kMine, RESOURCE_MP));
   CHECK(move_usable(g_state, kMine, kSlotSpark));

   // Drain (1 MP) takes 5 MP from the target through a resource write
   const int32_t foe_mp = resource_value(foe, RESOURCE_MP);
   CHECK(foe_mp == resource_max(foe, RESOURCE_MP));
   CHECK(choose(g_state, kMine, Action{ActionKind::Move, kSlotDrain, kBench, 0}));
   CHECK(until_request(q));
   CHECK(resource_value(foe, RESOURCE_MP) == foe_mp - kDrainMp);
   CHECK(resource_value(me, RESOURCE_MP) == static_cast<int32_t>(kLowMp) + kRefreshMp - 1);

   // Spark (4 MP)
   CHECK(choose(g_state, kMine, Action{ActionKind::Move, kSlotSpark, kBench, 0}));
   CHECK(until_request(q));
   const int32_t mp_left = static_cast<int32_t>(kLowMp) + kRefreshMp - 1 - kSparkCost;
   CHECK(resource_value(me, RESOURCE_MP) == mp_left);
   CHECK(said(Msg::UsedMove, kMine, MOVE_SPARK) == 1);

   // run until the battle ends: the record keeps the MP and the HP, EP has no role
   while (choose(g_state, q.battler, Action{ActionKind::Run, 0, kBench, 0}) && until_request(q))
   {
   }
   CHECK(g_state.outcome == Outcome::Escaped);
   const monster::Record& rec = g_player.members[0];
   CHECK(static_cast<int32_t>(monster::get(rec, ROLE_MP_LEFT)) == mp_left);
   CHECK(monster::get(rec, Role::CurrentHp) == me.hp);

   // the next battle: MP as written back, EP full again
   CHECK(begin(PARTY_DUMMY));
   CHECK(resource_value(g_state.battlers[kMine], RESOURCE_MP) == mp_left);
   CHECK(resource_value(g_state.battlers[kMine], RESOURCE_EP) == ep_max);
}

/// Nothing payable: every move refused, the fallback move (free) taken; the AI never picks the unpayable Burst
/// and falls back to Tap once its MP is spent.
void test_fallback_and_ai()
{
   g_player = monster::Party{};
   rng::Stream r{kRecordSeed};
   CHECK(make_monster(g_player.members[0], SPECIES_BLOB, kLevel, r));
   g_player.count = 1;
   monster::set(g_player.members[0], Role::Moves, kSlotSpark, MOVE_BURST);
   monster::set(g_player.members[0], Role::Moves, kSlotRush, MOVE_SPARK);
   monster::set(g_player.members[0], Role::Moves, kSlotRefresh, monster::kNoMove);
   monster::set(g_player.members[0], Role::Moves, kSlotDrain, monster::kNoMove);
   monster::set(g_player.members[0], ROLE_MP_LEFT, 0, 0);
   CHECK(begin(PARTY_DUMMY));
   Request q{};
   CHECK(until_request(q) && q.kind == RequestKind::Action);
   for (uint8_t k = 0; k < move_slots(); ++k)
   {
      CHECK(!move_usable(g_state, kMine, k));
   }
   CHECK(!choose(g_state, kMine, Action{ActionKind::Move, kSlotSpark, kBench, 0}));
   CHECK(choose(g_state, kMine, Action{ActionKind::Move, kFallbackSlot, kBench, 0}));
   CHECK(until_request(q));
   CHECK(said(Msg::UsedMove, kMine, MOVE_TAP) == 1 && said(Msg::NoUsableMove, kMine, 0) == 0);

   // the AI's Blob knows Burst (never payable) and Spark (4 of its MP each); the player only refreshes
   g_player = monster::Party{};
   CHECK(make_monster(g_player.members[0], SPECIES_BLOB, kSturdyLevel, r));
   g_player.count = 1;
   monster::set(g_player.members[0], Role::Moves, kSlotSpark, MOVE_REFRESH);
   for (uint8_t k = 1; k < move_slots(); ++k)
   {
      monster::set(g_player.members[0], Role::Moves, k, monster::kNoMove);
   }
   CHECK(begin(PARTY_CASTER));
   const int32_t sparks = resource_max(g_state.battlers[kFoe], RESOURCE_MP) / kSparkCost;
   for (int t = 0; t < kAiTurns; ++t)
   {
      CHECK(play(0));
   }
   CHECK(said(Msg::UsedMove, kFoe, MOVE_BURST) == 0);
   CHECK(said(Msg::UsedMove, kFoe, MOVE_SPARK) == sparks);
   CHECK(said(Msg::UsedMove, kFoe, MOVE_TAP) == kAiTurns - 1 - sparks);   // the last turn is still running
}

/// A cost on the hit points needs more than its amount: a battler with exactly the cost cannot use the move (it
/// would faint paying), one with a point more can, and pays down to that point (C1).
void test_hp_cost()
{
   g_player = monster::Party{};
   rng::Stream r{kRecordSeed};
   CHECK(make_monster(g_player.members[0], SPECIES_BLOB, kLevel, r));
   g_player.count = 1;
   monster::set(g_player.members[0], Role::Moves, kSlotSpark, MOVE_SACRIFICE);
   monster::set(g_player.members[0], Role::CurrentHp, 0, static_cast<uint32_t>(kSacrificeCost));
   CHECK(begin(PARTY_DUMMY));
   Request q{};
   CHECK(until_request(q) && q.kind == RequestKind::Action);
   CHECK(g_state.battlers[kMine].hp == kSacrificeCost);
   CHECK(!move_usable(g_state, kMine, kSlotSpark));
   CHECK(!choose(g_state, kMine, Action{ActionKind::Move, kSlotSpark, kBench, 0}));

   monster::set(g_player.members[0], Role::CurrentHp, 0, static_cast<uint32_t>(kSacrificeCost + 1));
   CHECK(begin(PARTY_DUMMY));
   CHECK(until_request(q) && q.kind == RequestKind::Action);
   CHECK(move_usable(g_state, kMine, kSlotSpark));
   CHECK(choose(g_state, kMine, Action{ActionKind::Move, kSlotSpark, kBench, 0}));
   (void)until_request(q);   // the turn runs (one point left, the foe's Tap may end the battle)
   CHECK(said(Msg::UsedMove, kMine, MOVE_SACRIFICE) == 1);
   // the payment: an Hp event from the cost down to one point (the foe's Tap may follow it)
   bool paid = false;
   for (int k = 0; k < g_log.count; ++k)
   {
      const Event& e = g_log.events[k];
      paid = paid || (e.kind == EventKind::Hp && e.battler == kMine && e.a == 1 && e.b == kSacrificeCost + 1);
   }
   CHECK(paid);
}

} // namespace

int main()
{
   test_data();
   test_new_record();
   test_battle();
   test_fallback_and_ai();
   test_hp_cost();
   std::printf("battle_resources_tests: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
