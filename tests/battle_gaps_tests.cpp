/// @file
/// Tests for G1's battle gaps (D-26 section 1) on the battle data compiled from tests/data/battle_gaps/battle.json
/// (gaps.ceffect): a volatile bound to its source (it ends when the source leaves the field or faints, and a passing
/// switch hands it on), delayed hits that keep a snapshot of their user (what the landing reads from the time of
/// use, a typeless hit, a landing that misses, a user off the field), use_move refusing a move with a move flag,
/// a decoy's absorbed hit that a secondary effect and a contact hook skip (the hit that breaks it too), and AI
/// profiles: the ai_score hook (a move's own handler, a passive's veto), the ai_score pipeline's veto and the
/// switching rules (a score below a bound, the hook with move none).
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
constexpr int kLogEvents = 2048;
/// Most steps one wait takes.
constexpr int kStepLimit = 4000;
/// The battle's seed.
constexpr uint32_t kSeed = 2468;
/// Codes a formatted text may take.
constexpr int kTextCap = 96;
/// Turns the AI tests watch.
constexpr int kAiTurns = 4;
/// The decoy's hit points (gaps.ceffect move Decoy).
constexpr int kDecoyHp = 10;
State g_state;   ///< Battle state (large; static storage).

/// What the battle showed.
struct Log
{
   Event events[kLogEvents];   ///< Events in order.
   int count;                  ///< Events kept.
};

Log g_log;   ///< The current test's events.

/// @name Battlers (Single: side 0's lead and second member, then side 1's)
///@{
constexpr uint8_t kLead = 0;
constexpr uint8_t kSecond = 1;
constexpr uint8_t kFoe = 2;
constexpr uint8_t kFoe2 = 3;
///@}

/// Side 1's position in the Single format.
constexpr uint8_t kFar = 1;

/// Move slots of Left's lead.
enum LeadSlot : uint8_t
{
   SLOT_TAP,
   SLOT_HOLD,
   SLOT_SIGHT,
   SLOT_LATE,
   SLOT_PUMP,
   SLOT_SECRET,
};
/// Move slots of Left's second member.
enum SecondSlot : uint8_t
{
   SLOT_WAIT2,
   SLOT_ECHO2,
   SLOT_DECOY2,
   SLOT_STING2,
   SLOT_SHAKY2,
   SLOT_RELAY2,
};
/// Move slots of Right's lead (Right's second: Wait, Tap, Sting).
enum FoeSlot : uint8_t
{
   SLOT_F_TAP,
   SLOT_F_DECOY,
   SLOT_F_WAIT,
   SLOT_F_HOLD,
   SLOT_F_ECHO,
   SLOT_F_RELAY,
};

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

/// Starts a Single battle of Left (human) against another party of the data.
/// @param right  side 1's party
/// @param ai     side 1 is AI-controlled (else human)
/// @return true if it started
bool begin(uint16_t right, bool ai = false)
{
   g_log = Log{};
   Setup su{};
   su.format = FORMAT_SINGLE;
   su.weather = kNone8;
   su.seed = kSeed;
   su.participants[su.count++] = Participant{&kData.parties[PARTY_LEFT], nullptr, nullptr, Controller::Human, 0, 0, 0};
   su.participants[su.count++] =
      Participant{&kData.parties[right], nullptr, nullptr, ai ? Controller::Ai : Controller::Human, 1, 0, 0};
   return start(g_state, su);
}

/// Runs the battle until it needs a choice (or ends).
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

/// A move choice.
/// @param slot  move slot
/// @return the action (default target)
Action use(uint8_t slot)
{
   return Action{ActionKind::Move, slot, kBench, 0};
}

/// A switch choice.
/// @param to  the battler to send in
/// @return the action
Action swap_to(uint8_t to)
{
   return Action{ActionKind::Switch, 0, kBench, to};
}

/// Plays one turn: each Action request of the turn takes the battler's entry of @p plan, a SwitchTarget request
/// (a passing switch) and a Replace request (a fainted battler) the battler's party's next member; then the battle
/// runs to the next turn's first request.
/// @param plan  an action per battler (indexed by battler)
/// @return false when a choice was refused
bool turn(const Action* plan)
{
   Request r{};
   if (!until_request(r))
   {
      return true;   // the battle is over
   }
   const uint16_t at = g_state.turn;
   while (g_state.turn == at)
   {
      Action a = plan[r.battler];
      if (r.kind == RequestKind::SwitchTarget)
      {
         a = Action{ActionKind::Answer, static_cast<uint8_t>(r.battler + 1), kBench, 0};
      }
      else if (r.kind == RequestKind::Replace)
      {
         a = swap_to(static_cast<uint8_t>(r.battler + 1));   // a fainted lead: its party's second member
      }
      if (!choose(g_state, r.battler, a))
      {
         return false;
      }
      if (!until_request(r))
      {
         break;
      }
   }
   return true;
}

/// Whether an event formats to exactly @p text.
/// @param e     the event
/// @param text  the expected text
/// @return true when it does
bool reads(const Event& e, const char* text)
{
   text::Code buf[kTextCap];
   if (!format_text(g_state, e, buf, kTextCap))
   {
      return false;
   }
   int i = 0;
   while (buf[i] != text::kCodeEnd && text[i] != '\0' && buf[i] == static_cast<text::Code>(text[i]))
   {
      ++i;
   }
   return buf[i] == text::kCodeEnd && text[i] == '\0';
}

/// Shown texts that read exactly @p text once formatted.
/// @param text  the expected text
/// @param from  first log index looked at
/// @return how many
int shown(const char* text, int from = 0)
{
   int n = 0;
   for (int k = from; k < g_log.count; ++k)
   {
      n += reads(g_log.events[k], text) ? 1 : 0;
   }
   return n;
}

/// Whether a battler has a volatile.
/// @param i  battler
/// @param v  volatile
/// @return true if it has
bool has_volatile(uint8_t i, uint8_t v)
{
   return (g_state.battlers[i].vol & (1u << v)) != 0;
}

/// Bound volatiles: the hold ends (its end text) when its source switches out; the holder could not switch before.
void test_bound_switch()
{
   CHECK(begin(PARTY_RIGHT));
   Action plan[] = {use(SLOT_HOLD), use(SLOT_WAIT2), use(SLOT_F_DECOY), use(0)};
   CHECK(turn(plan));
   CHECK(has_volatile(kFoe, VOLATILE_HELD));
   CHECK(is_trapped(g_state, kFoe));
   Request r{};
   CHECK(until_request(r) && r.battler == kLead);
   CHECK(choose(g_state, kLead, swap_to(kSecond)));
   CHECK(until_request(r) && r.battler == kFoe);
   CHECK(!choose(g_state, kFoe, swap_to(kFoe2)));   // still held: its source is on the field
   CHECK(choose(g_state, kFoe, use(SLOT_F_TAP)));
   CHECK(until_request(r));
   CHECK(!has_volatile(kFoe, VOLATILE_HELD));       // the holder's source left
   CHECK(shown("Cube slipped free.") == 1);
   CHECK(!is_trapped(g_state, kFoe));
}

/// Bound volatiles: the hold ends when its source faints.
void test_bound_faint()
{
   CHECK(begin(PARTY_RIGHT));
   Action plan[] = {use(SLOT_HOLD), use(SLOT_WAIT2), use(SLOT_F_DECOY), use(0)};
   CHECK(turn(plan));
   CHECK(has_volatile(kFoe, VOLATILE_HELD));
   g_state.battlers[kLead].hp = 1;   // the foe's next hit knocks the source out
   Action plan2[] = {use(SLOT_PUMP), use(SLOT_WAIT2), use(SLOT_F_TAP), use(0)};
   CHECK(turn(plan2));
   CHECK((g_state.battlers[kLead].flags & (1u << BATTLER_FAINTED)) != 0);
   CHECK(!has_volatile(kFoe, VOLATILE_HELD));
   CHECK(shown("Cube slipped free.") == 1);
}

/// Bound volatiles: a passing switch hands the hold the leaving battler started to its replacement, which then
/// ends it when it leaves.
void test_bound_pass()
{
   CHECK(begin(PARTY_RIGHT));
   Action plan[] = {use(SLOT_PUMP), use(SLOT_WAIT2), use(SLOT_F_HOLD), use(0)};
   CHECK(turn(plan));
   CHECK(has_volatile(kLead, VOLATILE_HELD));
   Action plan2[] = {use(SLOT_PUMP), use(SLOT_WAIT2), use(SLOT_F_RELAY), use(0)};
   CHECK(turn(plan2));
   CHECK(g_state.at[kFar] == kFoe2);
   CHECK(has_volatile(kLead, VOLATILE_HELD));   // handed on, not ended
   CHECK(shown("Blob slipped free.") == 0);
   Request r{};
   CHECK(until_request(r) && r.battler == kLead);
   CHECK(choose(g_state, kLead, use(SLOT_PUMP)));
   CHECK(until_request(r) && r.battler == kFoe2);
   CHECK(choose(g_state, kFoe2, swap_to(kFoe)));
   CHECK(until_request(r));
   CHECK(!has_volatile(kLead, VOLATILE_HELD));
   CHECK(shown("Blob slipped free.") == 1);
}

/// The HP a delayed hit of side 0's lead takes from the foe: the lead uses @p first on turn 1, then @p second on
/// turn 2 (the hit lands at the end of turn 2); the foe waits both turns.
/// @param first   the lead's turn-1 slot (a delayed hit)
/// @param second  its turn-2 action
/// @return HP the foe lost
int delayed_damage(uint8_t first, Action second)
{
   if (!begin(PARTY_RIGHT))
   {
      return -1;
   }
   const Action one[] = {use(first), use(SLOT_WAIT2), use(SLOT_F_WAIT), use(0)};
   turn(one);
   const int before = g_state.battlers[kFoe].hp;
   const Action two[] = {second, use(SLOT_WAIT2), use(SLOT_F_WAIT), use(0)};
   turn(two);
   return before - g_state.battlers[kFoe].hp;
}

/// Delayed hits: Sight keeps the user's stages from the time of use (a stage raised later does not count) and hits
/// without a type (no same-type factor); Late reads the user when it lands (the raise counts) and has the move's
/// type; a user off the field when it lands still hits with its snapshot.
void test_delayed_snapshot()
{
   const int sight = delayed_damage(SLOT_SIGHT, use(SLOT_HOLD));
   const int sight_pumped = delayed_damage(SLOT_SIGHT, use(SLOT_PUMP));
   CHECK(sight > 0);
   CHECK(sight_pumped == sight);   // stages taken when it was used
   CHECK(shown("Cube took the hit!") == 1);
   const int late = delayed_damage(SLOT_LATE, use(SLOT_HOLD));
   const int late_pumped = delayed_damage(SLOT_LATE, use(SLOT_PUMP));
   CHECK(late_pumped > late);   // read when it lands
   CHECK(late > sight);   // typed: the same-type factor; Sight has no type
   const int away = delayed_damage(SLOT_SIGHT, swap_to(kSecond));
   CHECK(away == sight);   // its user left the field: the snapshot hits
   CHECK(g_state.delayed[kFar].turns == 0);
}

/// Delayed hits: a landing that misses its accuracy check says its record's message and deals nothing.
void test_delayed_miss()
{
   CHECK(begin(PARTY_RIGHT));
   Request r{};
   CHECK(until_request(r) && r.battler == kLead);
   CHECK(choose(g_state, kLead, swap_to(kSecond)));
   CHECK(until_request(r) && r.battler == kFoe);
   CHECK(choose(g_state, kFoe, use(SLOT_F_HOLD)));
   Action plan[] = {use(0), use(SLOT_SHAKY2), use(SLOT_F_HOLD), use(0)};
   const int before = g_state.battlers[kFoe].hp;
   const int from = g_log.count;
   CHECK(turn(plan));
   CHECK(shown("Cube took the hit!", from) == 1);
   CHECK(shown("But it failed!", from) == 1);   // the 1% landing missed (shaky: miss_message failed)
   CHECK(g_state.battlers[kFoe].hp == before);
}

/// use_move ... unless unmirrorable: Echo copies Tap; it refuses Secret (the move fails).
void test_call_refused()
{
   CHECK(begin(PARTY_RIGHT));
   Action plan[] = {use(SLOT_SECRET), use(SLOT_WAIT2), use(SLOT_F_HOLD), use(0)};
   CHECK(turn(plan));
   CHECK(g_state.battlers[kFoe].targeted_by == MOVE_SECRET);
   int from = g_log.count;
   Action plan2[] = {use(SLOT_PUMP), use(SLOT_WAIT2), use(SLOT_F_ECHO), use(0)};
   CHECK(turn(plan2));
   CHECK(shown("Cube used Echo!", from) == 1);
   CHECK(shown("But it failed!", from) == 1);
   CHECK(shown("Cube used Secret!", from) == 0);
   Action plan3[] = {use(SLOT_TAP), use(SLOT_WAIT2), use(SLOT_F_HOLD), use(0)};
   CHECK(turn(plan3));
   from = g_log.count;
   CHECK(turn(plan2));
   CHECK(shown("Cube used Echo!", from) == 1);
   CHECK(shown("Cube used Tap!", from) == 1);   // copied
}

/// A decoy's hit: the hit that breaks it keeps Sting's secondary effect and Prickle's contact hook off; the next
/// hit (no decoy) has both.
void test_absorbed()
{
   CHECK(begin(PARTY_RIGHT));
   Request r{};
   CHECK(until_request(r) && r.battler == kLead);
   CHECK(choose(g_state, kLead, swap_to(kSecond)));
   CHECK(until_request(r) && r.battler == kFoe);
   CHECK(choose(g_state, kFoe, use(SLOT_F_DECOY)));
   CHECK(until_request(r));
   CHECK(has_volatile(kFoe, VOLATILE_DECOY));
   CHECK(g_state.battlers[kFoe].vword[0] == kDecoyHp);
   const int hp = g_state.battlers[kFoe].hp;
   Action plan[] = {use(0), use(SLOT_STING2), use(SLOT_F_HOLD), use(0)};
   CHECK(choose(g_state, kSecond, plan[kSecond]));
   CHECK(until_request(r) && r.battler == kFoe);
   CHECK(choose(g_state, kFoe, plan[kFoe]));
   CHECK(until_request(r));
   CHECK(!has_volatile(kFoe, VOLATILE_DECOY));                       // broken by the hit
   CHECK(shown("Cube's decoy faded.") == 1);
   CHECK(g_state.battlers[kFoe].hp == hp);                           // the decoy took it all
   CHECK(g_state.battlers[kFoe].stages[STAGE_PACE] == 0);           // no secondary effect
   CHECK(g_state.battlers[kSecond].stages[STAGE_POWER] == 0);       // no contact hook
   CHECK(turn(plan));
   CHECK(g_state.battlers[kFoe].hp < hp);
   CHECK(g_state.battlers[kFoe].stages[STAGE_PACE] == -1);
   CHECK(g_state.battlers[kSecond].stages[STAGE_POWER] == -1);
}

/// Counts what the AI's battler @p name used over kAiTurns turns while the lead pumps.
/// @param party  the AI party
/// @param used   "NAME used MOVE!" text
/// @return how many times it showed
int ai_uses(uint16_t party, const char* used)
{
   if (!begin(party, true))
   {
      return -1;
   }
   const Action plan[] = {use(SLOT_PUMP), use(SLOT_WAIT2), use(0), use(0)};
   for (int k = 0; k < kAiTurns; ++k)
   {
      turn(plan);
   }
   return shown(used);
}

/// AI profiles: the ai_score hook (a passive's veto: Picky never stings; a move's own handler: Wait's +500), the
/// pipeline's veto (non-damaging moves below 0).
void test_ai_scoring()
{
   CHECK(ai_uses(PARTY_PICKY, "Picker used Sting!") == 0);
   CHECK(shown("Picker used Tap!") == kAiTurns);
   CHECK(ai_uses(PARTY_WAITER, "Blob used Wait!") == kAiTurns);
   CHECK(ai_uses(PARTY_PIPED, "Blob used Tap!") == kAiTurns);
   CHECK(shown("Blob used Wait!") == 0);
}

/// AI profiles: switching rules. Switcher's bound (a best score below 32767) switches at once; Nervous's hook
/// (move none) switches once its lead is hurt.
void test_ai_switching()
{
   CHECK(begin(PARTY_SWITCHER, true));
   const Action plan[] = {use(SLOT_PUMP), use(SLOT_WAIT2), use(0), use(0)};
   CHECK(turn(plan));
   CHECK(g_state.at[kFar] == kFoe2);
   CHECK(begin(PARTY_NERVOUS, true));
   CHECK(turn(plan));
   CHECK(g_state.at[kFar] == kFoe);   // unhurt: it stays
   const Action hit[] = {use(SLOT_TAP), use(SLOT_WAIT2), use(0), use(0)};
   CHECK(turn(hit));
   CHECK(g_state.at[kFar] == kFoe);
   CHECK(turn(plan));
   CHECK(g_state.at[kFar] == kFoe2);   // hurt: the hook makes switching worth more than any move
}

} // namespace

int main()
{
   test_bound_switch();
   test_bound_faint();
   test_bound_pass();
   test_delayed_snapshot();
   test_delayed_miss();
   test_call_refused();
   test_absorbed();
   test_ai_scoring();
   test_ai_switching();
   std::printf("%d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
