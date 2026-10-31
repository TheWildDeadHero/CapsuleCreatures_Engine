/// @file
/// Tests for the effect VM inside battles (D-23 section 1, E4b) against the battle data compiled from
/// tests/data/effects/battle/battle.json, whose moves and passive are effect code (logic.ceffect): a lock-in move
/// (lock no_pp, stop, the locked turn), switch pass with a human's SwitchTarget answer and an AI's own answer,
/// credit, script targeting (set_target), a Choice request answered through choose(), a multi-hit whose hits are
/// one step each, and nested hooks past kMaxEffectDepth (counted faults, the battle goes on).
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
/// Message id of hit_count (battlec.texts.MESSAGES; an optional message the engine's Msg does not name, C4).
constexpr uint16_t kMsgHitCount = 11;
/// Most steps one wait takes.
constexpr int kStepLimit = 2000;
/// The battle's seed.
constexpr uint32_t kSeed = 1234;
/// Stage index of the power stat.
constexpr uint8_t kPowerStage = battle_ids::STAGE_POWER;
/// Move slots of the Left party's first member (logic.ceffect moves).
enum Slot : uint8_t
{
   SLOT_CHARGE,
   SLOT_RELAY,
   SLOT_COUNT,
   SLOT_AIM,
   SLOT_PICK,
   SLOT_FLURRY,
};
/// Battlers: the Left party's two members, then the foe's.
constexpr uint8_t kLead = 0;
constexpr uint8_t kBenched = 1;   ///< Left's second member.
constexpr uint8_t kFoe = 2;       ///< The foe's first member.
constexpr uint8_t kFoeBench = 3;  ///< The foe's second member.

State g_state;   ///< Battle state (large; static storage).

/// What the battle showed.
struct Log
{
   Event events[kLogEvents];   ///< Events in order.
   int step_of[kLogEvents];    ///< The step that queued each.
   int count;                  ///< Events kept.
   int steps;                  ///< Steps run.
};

Log g_log;   ///< The current test's events.

/// Moves the queued events into the log.
void drain()
{
   Event e;
   while (pop_event(g_state, e))
   {
      if (g_log.count < kLogEvents)
      {
         g_log.step_of[g_log.count] = g_log.steps;
         g_log.events[g_log.count++] = e;
      }
   }
}

/// Starts a battle of a human data party against an AI data party.
/// @param left   the human's party
/// @param right  the AI's party
/// @return true if it started
bool begin(uint16_t left, uint16_t right)
{
   g_log = Log{};
   Setup su{};
   su.format = FORMAT_SINGLE;
   su.weather = kNone8;
   su.seed = kSeed;
   su.participants[su.count++] = Participant{&kData.parties[left], nullptr, nullptr, Controller::Human, 0, 0, 0};
   su.participants[su.count++] = Participant{&kData.parties[right], nullptr, nullptr, Controller::Ai, 1, 0, 0};
   return start(g_state, su);
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
      ++g_log.steps;
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

/// Runs to the next request and answers an Action request with a move slot.
/// @param slot  move slot
/// @return true if the move was chosen
bool use(uint8_t slot)
{
   Request r{};
   return until_request(r) && r.kind == RequestKind::Action &&
          choose(g_state, r.battler, Action{ActionKind::Move, slot, kBench, 0});
}

/// Events of a kind in the log (optionally only those with a value).
/// @param kind  kind
/// @param a     its a, or -1 for any
/// @return how many
int count(EventKind kind, int a = -1)
{
   int n = 0;
   for (int k = 0; k < g_log.count; ++k)
   {
      n += g_log.events[k].kind == kind && (a < 0 || g_log.events[k].a == a) ? 1 : 0;
   }
   return n;
}

/// The value of the first hit_count message.
/// @return its b, or -1
int hit_count_value()
{
   for (int k = 0; k < g_log.count; ++k)
   {
      const Event& e = g_log.events[k];
      if (e.kind == EventKind::Message && e.a == kMsgHitCount)
      {
         return e.b;
      }
   }
   return -1;
}

/// Lock-in: the first turn locks the next action (no PP) and stops; the locked turn hits without a choice.
void test_lock()
{
   CHECK(begin(PARTY_LEFT, PARTY_RIGHT));
   CHECK(use(SLOT_CHARGE));
   Request r{};
   CHECK(until_request(r) == false || r.kind == RequestKind::Action);
   // turn 1 charged: a text and a cue, no damage; turn 2 was locked: no request came for it
   CHECK(g_state.turn == 3);
   CHECK(count(EventKind::Text, 0) == 1);   // "charging"
   CHECK(count(EventKind::Cue, 0) == 1);
   const Battler& foe = g_state.battlers[kFoe];
   CHECK(foe.hp < max_hp(foe));
   CHECK(resource_value(g_state.battlers[kLead], RESOURCE_PP, SLOT_CHARGE) ==
         move_base(MOVE_CHARGE, RESOURCE_PP) - 1);   // the locked turn used none
   CHECK(g_state.battlers[kLead].lock_flags == 0);
   CHECK(g_state.vm.faults == 0);
}

/// Switch pass: the human answers the SwitchTarget request; a wrong answer is refused.
void test_pass()
{
   CHECK(begin(PARTY_LEFT, PARTY_RIGHT));
   CHECK(use(SLOT_RELAY));
   Request r{};
   CHECK(until_request(r) && r.kind == RequestKind::SwitchTarget && r.battler == kLead);
   CHECK(!choose(g_state, kLead, Action{ActionKind::Move, 0, kBench, 0}));      // not an answer
   CHECK(!choose(g_state, kLead, Action{ActionKind::Answer, kFoe, kBench, 0}));  // not a replacement
   CHECK(choose(g_state, kLead, Action{ActionKind::Answer, kBenched, kBench, 0}));
   CHECK(until_request(r) && r.kind == RequestKind::Action && r.battler == kBenched);
   CHECK(g_state.battlers[kBenched].position == 0 && g_state.battlers[kLead].position == kBench);
   CHECK(g_state.battlers[kBenched].stages[kPowerStage] == 2);   // passed on
   CHECK(g_state.battlers[kLead].stages[kPowerStage] == 0);
}

/// An AI's switch pass answers itself (its next battler).
void test_ai_pass()
{
   CHECK(begin(PARTY_LEFT, PARTY_RELAYER));
   CHECK(use(SLOT_COUNT));
   Request r{};
   CHECK(until_request(r) && r.kind == RequestKind::Action);
   CHECK(g_state.battlers[kFoeBench].position == 1 && g_state.battlers[kFoe].position == kBench);
   CHECK(g_state.battlers[kFoeBench].stages[kPowerStage] == 2);
}

/// Credit adds to the move's dealt (the message shows it).
void test_credit()
{
   CHECK(begin(PARTY_LEFT, PARTY_RIGHT));
   CHECK(use(SLOT_COUNT));
   Request r{};
   CHECK(until_request(r));
   CHECK(hit_count_value() == 9);
}

/// Script targeting: the move hits its user, not the foe.
void test_set_target()
{
   CHECK(begin(PARTY_LEFT, PARTY_RIGHT));
   CHECK(use(SLOT_AIM));
   Request r{};
   CHECK(until_request(r));
   const Battler& lead = g_state.battlers[kLead];
   const Battler& foe = g_state.battlers[kFoe];
   CHECK(lead.hp < max_hp(lead));
   CHECK(foe.hp == max_hp(foe));
}

/// Ask / answer: the run waits for choose(); the answer goes on into the program.
void test_ask()
{
   CHECK(begin(PARTY_LEFT, PARTY_RIGHT));
   CHECK(use(SLOT_PICK));
   Request r{};
   CHECK(until_request(r) && r.kind == RequestKind::Choice && r.battler == kLead);
   CHECK(step(g_state) == Progress::Input);   // it waits
   CHECK(choose(g_state, kLead, Action{ActionKind::Answer, 4, kBench, 0}));
   CHECK(until_request(r) && r.kind == RequestKind::Action);
   CHECK(hit_count_value() == 40);
}

/// A multi-hit yields after each hit: every hit is its own step, then the text.
void test_multi_hit()
{
   CHECK(begin(PARTY_LEFT, PARTY_RIGHT));
   CHECK(use(SLOT_FLURRY));
   Request r{};
   CHECK(until_request(r));
   int hits = 0;
   int last_step = -1;
   bool own_steps = true;
   for (int k = 0; k < g_log.count; ++k)
   {
      const Event& e = g_log.events[k];
      if (e.kind == EventKind::Hp && e.battler == kFoe)
      {
         own_steps = own_steps && g_log.step_of[k] != last_step;
         last_step = g_log.step_of[k];
         ++hits;
      }
   }
   CHECK(hits == 3);
   CHECK(own_steps);
   CHECK(count(EventKind::Text, 1) == 1);   // "done"
}

/// Nested hooks past the depth limit: the deepest dispatch is skipped and counted; the battle goes on.
void test_depth()
{
   CHECK(begin(PARTY_ECHOES, PARTY_ECHOES));
   CHECK(use(0));
   Request r{};
   const bool more = until_request(r);
   CHECK(g_state.vm.faults > 0);
   CHECK(g_state.vm.last_fault == effect::Fault::Depth);
   CHECK(more || g_state.phase == Phase::Ended);
   CHECK(g_state.dispatch_depth == 0 && g_state.vm.depth == 0);
   CHECK(g_state.events_lost == 0);
}

} // namespace

int main()
{
   test_lock();
   test_pass();
   test_ai_pass();
   test_credit();
   test_set_target();
   test_ask();
   test_multi_hit();
   test_depth();
   std::printf("battle VM tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
