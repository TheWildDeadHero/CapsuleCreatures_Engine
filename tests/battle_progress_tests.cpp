/// @file
/// Tests for progression from the manifest (D-22 A8 / A12, D-23 section 2, E6c) against the battle data compiled
/// from tests/data/battle_progress/battle.json, whose progression differs from the sample's (the engine's former
/// fixed rules) in every part: a full move list asks which move to forget (learn.on_full request), creatures built
/// without moves know the learnset's first moves (first_n), every usable battler of every party on the other side
/// shares a fainted foe's experience (share party, receivers opposing_sides) and a record whose owner_id is not
/// Setup::owner_id earns the ruleset's outsider bonus; new records come from a generation list of one draw (a
/// 16-bit seed), constants (genes 7, two PP raises), a derived custom role (gender) and a derived trait the layout
/// does not store (read through record_value()); and the ruleset's own pp_max (the move resource pp's max formula,
/// D-25) adds half the base PP per raise.
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
constexpr int kLogEvents = 1024;
/// Most steps one run takes.
constexpr int kStepLimit = 2000;
/// The battle's seed.
constexpr uint32_t kSeed = 2468;
/// Seed of the stream new records are made from.
constexpr uint32_t kRecordSeed = 1357;
/// The player's owner id (Setup::owner_id).
constexpr uint32_t kPlayerId = 1;
/// Another owner's id (an outsider's record).
constexpr uint32_t kOtherId = 2;
/// Level of the player's creatures (the next level teaches Glow).
constexpr uint8_t kStartLevel = 5;
/// The level that teaches Glow.
constexpr uint8_t kGlowLevel = 6;
/// The fixture's generation constants: genes and PP raises.
constexpr uint32_t kGenes = 7;
constexpr uint32_t kRaises = 2;
/// The fixture's pp_max formula: pp_base + pp_base * pp_ups / 2.
constexpr uint32_t kPpRaiseDiv = 2;
/// Mask of a seed's low half (the derived roles read seed_lo).
constexpr uint32_t kLowHalf = 0xFFFFu;
/// The fixture's gender rule: seed_lo % 3.
constexpr uint32_t kGenders = 3;
/// The species' experience yield.
constexpr uint32_t kYield = 300;
/// The fixture's exp_gain() outsider bonus: an outsider earns this many shares.
constexpr uint32_t kOutsiderShares = 2;
/// Battlers sharing the award in the first battle (both of the player's).
constexpr uint32_t kSharing = 2;
/// The player's two creatures and the foe.
/// Record seeds the record_stats() check makes records from (the review's probe found the derived trait ignored).
constexpr uint32_t kStatSeeds = 40;
/// Level of those records (stats far enough from their bases for a trait's percent to show).
constexpr uint8_t kStatLevel = 30;
constexpr uint8_t kFirst = 0;
constexpr uint8_t kSecond = 1;
constexpr uint8_t kSlotFirst = 0;
constexpr uint8_t kSlotSecond = 1;

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

/// Whether the log holds a message.
/// @param msg      message
/// @param battler  its battler
/// @param value    its value (move)
/// @return true if shown
bool said(Msg msg, uint8_t battler, uint16_t value)
{
   for (int k = 0; k < g_log.count; ++k)
   {
      const Event& e = g_log.events[k];
      if (e.kind == EventKind::Message && e.a == static_cast<uint16_t>(msg) && e.battler == battler && e.b == value)
      {
         return true;
      }
   }
   return false;
}

/// Experience a battler gained in the log.
/// @param battler  the battler
/// @return experience (0: none)
uint32_t gained(uint8_t battler)
{
   uint32_t sum = 0;
   for (int k = 0; k < g_log.count; ++k)
   {
      const Event& e = g_log.events[k];
      if (e.kind == EventKind::Exp && e.battler == battler)
      {
         sum += static_cast<uint32_t>(e.a) | static_cast<uint32_t>(e.b) << rng::kDrawBits;
      }
   }
   return sum;
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
   su.owner_id = kPlayerId;
   su.participants[su.count++] = Participant{nullptr, &g_player, nullptr, Controller::Human, 0, 0, 0};
   su.participants[su.count++] = Participant{&kData.parties[foe], nullptr, nullptr, Controller::Ai, 1, 0, 0};
   return start(g_state, su);
}

/// The generation list: one draw (the 16-bit seed), the constants, the derived roles and the PP they give.
void test_generation()
{
   rng::Stream r{kRecordSeed};
   rng::Stream expect = r;
   const uint32_t seed = rng::next16(expect);   // the only draw the list makes
   monster::Record rec{};
   CHECK(make_monster(rec, SPECIES_BLOB, kStartLevel, r));
   CHECK(r.state == expect.state);
   CHECK(monster::get(rec, Role::Seed) == seed);
   for (int i = 0; i < kData.stat_count; ++i)
   {
      CHECK(monster::get(rec, Role::StatGenes, i) == kGenes);
   }
   CHECK(monster::get(rec, ROLE_GENDER) == (seed & kLowHalf) % kGenders);
   // the trait is derived and the layout does not store it: record_value() computes it
   CHECK(!monster::has(Role::Trait));
   CHECK(record_value(rec, monster::id_of(Role::Trait)) == (seed & kLowHalf) % kData.trait_count);
   // first_n: the learnset's first two moves (last_n_by_level would give Wait, Tap)
   CHECK(monster::get(rec, Role::Moves, kSlotFirst) == MOVE_SMASH);
   CHECK(monster::get(rec, Role::Moves, kSlotSecond) == MOVE_WAIT);
   const auto base = static_cast<uint32_t>(move_base(MOVE_SMASH, RESOURCE_PP));
   CHECK(monster::get(rec, Role::PpUps, kSlotFirst) == kRaises);
   CHECK(monster::get(rec, Role::Pp, kSlotFirst) == base + base * kRaises / kPpRaiseDiv);
}

/// Party share, outsider bonus, level-up offers answered with a slot and with kKeepMoves, PP raises reset.
void test_share_and_learn()
{
   g_player = monster::Party{};
   rng::Stream r{kRecordSeed};
   CHECK(make_monster(g_player.members[0], SPECIES_BLOB, kStartLevel, r));
   CHECK(make_monster(g_player.members[1], SPECIES_BLOB, kStartLevel, r));
   g_player.count = 2;
   monster::set(g_player.members[0], Role::OwnerId, kPlayerId);
   monster::set(g_player.members[1], Role::OwnerId, kOtherId);
   CHECK(begin(PARTY_TARGET));
   const auto base = static_cast<uint32_t>(move_base(MOVE_SMASH, RESOURCE_PP));
   CHECK(static_cast<uint32_t>(resource_max(g_state.battlers[kFirst], RESOURCE_PP, kSlotFirst)) ==
         base + base * kRaises / kPpRaiseDiv);
   CHECK(g_state.battlers[kFirst].trait == record_value(g_player.members[0], monster::id_of(Role::Trait)));

   Request q{};
   CHECK(until_request(q) && q.kind == RequestKind::Action && q.battler == kFirst);
   CHECK(choose(g_state, q.battler, Action{ActionKind::Move, kSlotFirst, kBench, 0}));   // Smash knocks it out

   // the first battler's level-up offers Glow: forget slot 1 (Wait)
   CHECK(until_request(q) && q.kind == RequestKind::LearnMove && q.battler == kFirst);
   CHECK(g_state.learn.move == MOVE_GLOW);
   CHECK(!choose(g_state, q.battler, Action{ActionKind::Answer, static_cast<uint8_t>(move_slots()), kBench, 0}));
   CHECK(choose(g_state, q.battler, Action{ActionKind::Answer, kSlotSecond, kBench, 0}));
   // the benched second battler shares the award (party share) and is offered Glow too: it keeps its moves
   CHECK(until_request(q) && q.kind == RequestKind::LearnMove && q.battler == kSecond);
   CHECK(choose(g_state, q.battler, Action{ActionKind::Answer, kKeepMoves, kBench, 0}));
   CHECK(!until_request(q));
   CHECK(g_state.outcome == Outcome::Victory);

   // shares: two battlers; the outsider earns kOutsiderShares of them
   CHECK(gained(kFirst) == kYield / kSharing);
   CHECK(gained(kSecond) == kYield * kOutsiderShares / kSharing);
   CHECK(said(Msg::Learned, kFirst, MOVE_GLOW));
   CHECK(said(Msg::NotLearned, kSecond, MOVE_GLOW));
   CHECK(g_state.battlers[kFirst].level == kGlowLevel);

   // written back: Glow in slot 1 with no raises and its own PP; slot 0 keeps its raises
   const monster::Record& a = g_player.members[0];
   CHECK(monster::get(a, Role::Moves, kSlotSecond) == MOVE_GLOW);
   CHECK(monster::get(a, Role::PpUps, kSlotSecond) == 0);
   CHECK(static_cast<int32_t>(monster::get(a, Role::Pp, kSlotSecond)) == move_base(MOVE_GLOW, RESOURCE_PP));
   CHECK(monster::get(a, Role::PpUps, kSlotFirst) == kRaises);
   const monster::Record& b = g_player.members[1];
   CHECK(monster::get(b, Role::Moves, kSlotSecond) == MOVE_WAIT);
}

/// receivers opposing_sides: an AI battler earns experience when the player's creature faints.
void test_ai_receives()
{
   g_player = monster::Party{};
   rng::Stream r{kRecordSeed};
   CHECK(make_monster(g_player.members[0], SPECIES_BLOB, kStartLevel, r));
   g_player.count = 1;
   monster::set(g_player.members[0], Role::CurrentHp, 1);
   monster::set(g_player.members[0], Role::OwnerId, kPlayerId);
   CHECK(begin(PARTY_BRUISER));
   Request q{};
   CHECK(until_request(q) && q.kind == RequestKind::Action);
   CHECK(choose(g_state, q.battler, Action{ActionKind::Move, kSlotSecond, kBench, 0}));   // Wait; Tap knocks it out
   CHECK(!until_request(q));
   CHECK(g_state.outcome == Outcome::Defeat);
   const uint8_t foe = g_state.parties[1].first_battler;
   CHECK(gained(foe) == kYield);   // one receiver, not an outsider (no record)
}

} // namespace

/// record_stats() reads the derived roles the layout leaves out (the trait here) through record_value(), as a
/// battle does: over many records its stats are the battler's, and the pools a new record fills follow them (C1).
void test_record_stats()
{
   int differ = 0;
   int traited = 0;
   for (uint32_t seed = 1; seed <= kStatSeeds; ++seed)
   {
      rng::Stream r{seed};
      g_player = monster::Party{};
      CHECK(make_monster(g_player.members[0], SPECIES_BLOB, kStatLevel, r));
      g_player.count = 1;
      uint16_t stats[kMaxStats] = {};
      CHECK(record_stats(g_player.members[0], stats));
      CHECK(monster::get(g_player.members[0], Role::CurrentHp) == stats[kData.pool_stat]);
      CHECK(begin(PARTY_TARGET));
      const Battler& b = g_state.battlers[kFirst];
      traited += b.trait != 0 ? 1 : 0;
      for (int i = 0; i < kData.stat_count; ++i)
      {
         differ += stats[i] != b.stats[i] ? 1 : 0;
      }
      CHECK(b.hp == stats[kData.pool_stat]);   // the persisted hit points are full
   }
   CHECK(differ == 0);
   CHECK(traited > 0);   // the check saw records whose derived trait is not the stored default
}

int main()
{
   test_generation();
   test_record_stats();
   test_share_and_learn();
   test_ai_receives();
   std::printf("battle_progress_tests: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
