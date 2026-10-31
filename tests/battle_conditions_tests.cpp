/// @file
/// Tests for conditions as manifest records (D-22 A3-A5, D-23 sections 1.5 and 2, E5) against the battle data
/// compiled from tests/data/effects/conditions/battle.json: volatiles with a word (a veil that takes the damage, a
/// piercing move through it), a counted lock-in, a charge with Volatile (HUD) events and its already text, a tag
/// given by a volatile, side conditions with layers and with turns (residual phase and countdown), a field
/// condition, the taken record, a status with escalation and one with a length. The records' declarative fields
/// expand into effect code (tools/battlec/conditions.py); the rest is the fixture's conditions.ceffect.
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
/// Most steps one wait takes.
constexpr int kStepLimit = 2000;
/// The battle's seed.
constexpr uint32_t kSeed = 4321;
/// Codes a formatted text may take.
constexpr int kTextCap = 96;
/// Bits of a Q8 share.
constexpr int kQ8Shift = 8;
/// The Blight record's residual share (1/10, 26 in Q8) and escalation cap.
constexpr int32_t kBlightQ8 = 26;
constexpr int32_t kBlightCap = 3;   ///< See kBlightQ8.
/// The Veil move's word.
constexpr uint16_t kVeilWord = 25;
/// Turns the veil test waits at most for the word to run out.
constexpr int kVeilTurns = 8;
/// Turns of the Screen and Haze records.
constexpr int kScreenTurns = 3;
constexpr int kHazeTurns = 2;       ///< See kScreenTurns.
/// The thorns' share of max HP per layer count (1 / 10, 1 / 8, 1 / 6: conditions.ceffect).
constexpr int32_t kThornsDivisors[] = {10, 8, 6};
/// Layers the thorns stop at.
constexpr int kThornsLayers = 3;

/// Move slots of the Left party's lead (Blob).
enum LeadSlot : uint8_t
{
   SLOT_VEIL,
   SLOT_RAMPAGE,
   SLOT_BEAM,
   SLOT_THORNS,
   SLOT_SCREEN,
   SLOT_HAZE,
};
/// Move slots of the Mender party's lead (Cube).
enum MenderSlot : uint8_t
{
   SLOT_SPORE,
   SLOT_LULLABY,
   SLOT_RETORT,
   SLOT_HOVER,
   SLOT_WAIT,
};
/// Move slot of the Left party's second member that waits.
constexpr uint8_t kBenchWait = 0;
/// Battlers: the human party's members, then the foe's.
constexpr uint8_t kLead = 0;
constexpr uint8_t kBenched = 1;    ///< Left's second member.
constexpr uint8_t kLeftFoe = 2;    ///< The foe of the two-member Left party.
constexpr uint8_t kMenderFoe = 1;  ///< The foe of the one-member Mender party.

State g_state;   ///< Battle state (large; static storage).

/// What the battle showed.
struct Log
{
   Event events[kLogEvents];   ///< Events in order.
   int count;                  ///< Events kept.
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

/// Runs to the next request and answers it with a switch.
/// @param to  battler to send in
/// @return true if the switch was chosen
bool switch_to(uint8_t to)
{
   Request r{};
   return until_request(r) && r.kind == RequestKind::Action &&
          choose(g_state, r.battler, Action{ActionKind::Switch, 0, kBench, to});
}

/// Runs to the next request (the turns before it end).
/// @return true when one came
bool settle()
{
   Request r{};
   return until_request(r);
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

/// Shown texts (owner texts, messages, status texts) that read exactly @p text once formatted.
/// @param text  the expected text (the default code page: ASCII codes)
/// @return how many
int shown(const char* text)
{
   int n = 0;
   for (int k = 0; k < g_log.count; ++k)
   {
      text::Code buf[kTextCap];
      if (!format_text(g_state, g_log.events[k], buf, kTextCap))
      {
         continue;
      }
      int i = 0;
      while (buf[i] != text::kCodeEnd && text[i] != '\0' && buf[i] == static_cast<text::Code>(text[i]))
      {
         ++i;
      }
      n += buf[i] == text::kCodeEnd && text[i] == '\0' ? 1 : 0;
   }
   return n;
}

/// The word slot of a volatile.
/// @param v  volatile
/// @return its slot
uint8_t word_of(uint8_t v)
{
   return kData.volatiles[v].word;
}

/// Whether a battler holds a volatile.
/// @param i  battler
/// @param v  volatile
/// @return true when on
bool has(uint8_t i, uint8_t v)
{
   return (g_state.battlers[i].vol >> v & 1u) != 0;
}

/// A word: the veil takes the hits until its word runs out; a piercing move goes through it.
void test_word()
{
   CHECK(begin(PARTY_LEFT, PARTY_HITTER));
   CHECK(use(SLOT_VEIL));
   CHECK(settle());
   CHECK(has(kLead, VOLATILE_VEIL));
   CHECK(shown("Blob raised a veil!") == 1);
   const uint16_t before = g_state.battlers[kLead].hp;
   const uint16_t word = g_state.battlers[kLead].vword[word_of(VOLATILE_VEIL)];
   CHECK(word < kVeilWord || g_state.battlers[kLead].hp < max_hp(g_state.battlers[kLead]));
   // the hits of the next turns go into the word, not the HP, until it breaks (the lead raises a screen meanwhile)
   for (int turn = 0; turn < kVeilTurns && has(kLead, VOLATILE_VEIL); ++turn)
   {
      const uint16_t hp = g_state.battlers[kLead].hp;
      CHECK(use(SLOT_SCREEN));
      CHECK(settle());
      CHECK(g_state.battlers[kLead].hp == hp || !has(kLead, VOLATILE_VEIL));
   }
   CHECK(!has(kLead, VOLATILE_VEIL));
   CHECK(shown("Blob's veil broke!") == 1);
   CHECK(g_state.battlers[kLead].vword[word_of(VOLATILE_VEIL)] == 0);
   CHECK(g_state.battlers[kLead].hp <= before);

   // a piercing move goes through a fresh veil
   CHECK(begin(PARTY_LEFT, PARTY_PIERCER));
   CHECK(use(SLOT_VEIL));
   CHECK(settle());
   const uint16_t hp = g_state.battlers[kLead].hp;
   CHECK(use(SLOT_VEIL));
   CHECK(settle());
   CHECK(has(kLead, VOLATILE_VEIL));
   CHECK(g_state.battlers[kLead].vword[word_of(VOLATILE_VEIL)] == kVeilWord);
   CHECK(g_state.battlers[kLead].hp < hp);
   CHECK(g_state.vm.faults == 0);
}

/// A lock-in: the volatile's duration [2, 2] counts down before each move; the locked turns need no choice.
void test_lock_in()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITER));
   CHECK(use(SLOT_RAMPAGE));
   // turn 1 starts it (counter 2) and locks; turns 2 and 3 count it down and lock again; turn 4 ends it first
   CHECK(settle());
   CHECK(g_state.turn == 5);
   CHECK(!has(kLead, VOLATILE_RAMPAGE));
   CHECK(shown("Blob calmed down.") == 1);
   CHECK(g_state.battlers[kLead].lock_flags == 0);
   CHECK(resource_value(g_state.battlers[kLead], RESOURCE_PP, SLOT_RAMPAGE) ==
         move_base(MOVE_RAMPAGE, RESOURCE_PP) - 4);   // its locks use PP
   CHECK(count(EventKind::Volatile) == 0);   // no HUD events (its record has no hud)
}

/// A charge: on, locked, off; its record has hud: Volatile events on and off.
void test_charge()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITER));
   CHECK(use(SLOT_BEAM));
   CHECK(settle());
   CHECK(g_state.turn == 3);
   CHECK(count(EventKind::Volatile, VOLATILE_CHARGING) == 2);
   int on = -1;
   int off = -1;
   for (int k = 0; k < g_log.count; ++k)
   {
      const Event& e = g_log.events[k];
      if (e.kind == EventKind::Volatile)
      {
         (e.extra == 1 ? on : off) = k;
         CHECK(e.battler == kLead);
      }
   }
   CHECK(on >= 0 && off > on);
   CHECK(shown("Blob is charging!") == 1);
   CHECK(!has(kLead, VOLATILE_CHARGING));
   CHECK(g_state.battlers[kLeftFoe].hp < max_hp(g_state.battlers[kLeftFoe]));
   CHECK(resource_value(g_state.battlers[kLead], RESOURCE_PP, SLOT_BEAM) ==
         move_base(MOVE_BEAM, RESOURCE_PP) - 1);   // the locked turn used none
}

/// Layers: three thorn layers on the user's side, a fourth fails; a switch-in is hurt by them unless it floats
/// (a tag its hover volatile gives).
void test_layers_and_tags()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITER));
   for (int k = 0; k < kThornsLayers; ++k)
   {
      CHECK(use(SLOT_THORNS));
   }
   CHECK(settle());
   CHECK(g_state.sides[0].layers[SIDE_COND_THORNS] == kThornsLayers);
   CHECK(shown("Thorns cover the ground!") == 1);
   int side_events = 0;
   for (int k = 0; k < g_log.count; ++k)
   {
      const Event& e = g_log.events[k];
      if (e.kind == EventKind::Side && e.a == SIDE_COND_THORNS)
      {
         ++side_events;
         CHECK(e.b == side_events && e.target == 0);   // layers 1, 2, 3 on side 0
      }
   }
   CHECK(side_events == kThornsLayers);
   CHECK(use(SLOT_THORNS));   // a fourth fails
   CHECK(switch_to(kBenched));
   CHECK(settle());
   const Battler& cube = g_state.battlers[kBenched];
   const int32_t hurt = max_hp(cube) / kThornsDivisors[kThornsLayers - 1];
   CHECK(cube.hp == max_hp(cube) - (hurt > 0 ? hurt : 1));
   CHECK(shown("Cube is hurt by thorns!") == 1);
   // the lead comes back floating: spared
   g_state.battlers[kLead].vol |= 1u << VOLATILE_HOVER;
   const uint16_t lead_hp = g_state.battlers[kLead].hp;
   CHECK(switch_to(kLead));
   CHECK(settle());
   CHECK(g_state.battlers[kLead].hp == lead_hp);
   CHECK(shown("Blob is hurt by thorns!") == 0);
}

/// Turns: the screen lasts its record's 3 turns; its residual phase runs each turn, then it fades.
void test_side_turns()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITER));
   CHECK(use(SLOT_SCREEN));
   CHECK(settle());
   CHECK((g_state.sides[0].cond >> SIDE_COND_SCREEN & 1u) != 0);
   CHECK(g_state.sides[0].turns[SIDE_COND_SCREEN] == kScreenTurns - 1);
   for (int turn = 1; turn < kScreenTurns; ++turn)
   {
      CHECK(use(SLOT_SCREEN));   // fails while it is up
      CHECK(settle());
   }
   CHECK(shown("Blob's side glows.") == kScreenTurns);
   CHECK(shown("The screen faded.") == 1);
   CHECK(g_state.sides[0].cond == 0);
}

/// A field condition: every battler loses 1 HP at the end of each of its 2 turns, then it lifts.
void test_field()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITER));
   CHECK(use(SLOT_HAZE));
   CHECK(settle());
   CHECK(g_state.field.cond == 1u << FIELD_COND_HAZE);
   CHECK(use(SLOT_HAZE));   // fails while it lasts
   CHECK(settle());
   CHECK(g_state.field.cond == 0);
   CHECK(shown("A haze rolled in!") == 1 && shown("The haze lifted.") == 1);
   CHECK(count(EventKind::Field, FIELD_COND_HAZE) == 2);
   const Battler& lead = g_state.battlers[kLead];
   const Battler& foe = g_state.battlers[kLeftFoe];
   CHECK(lead.hp == max_hp(lead) - kHazeTurns);
   CHECK(foe.hp == max_hp(foe) - kHazeTurns);
}

/// Escalation: Blight's residual damage grows by its share each turn up to its cap.
void test_escalation()
{
   CHECK(begin(PARTY_MENDER, PARTY_WAITER));
   CHECK(use(SLOT_SPORE));
   CHECK(settle());
   const Battler& foe = g_state.battlers[kMenderFoe];
   CHECK(foe.status == STATUS_BLIGHT);
   int32_t expect = max_hp(foe);
   for (int turn = 1; turn <= kBlightCap + 1; ++turn)
   {
      const int32_t step = kBlightQ8 * (turn < kBlightCap ? turn : kBlightCap);
      const int32_t loss = (static_cast<int32_t>(max_hp(foe)) * step) >> kQ8Shift;
      expect -= loss > 0 ? loss : 1;
      CHECK(foe.hp == expect);
      CHECK(foe.status_aux == (turn < kBlightCap ? turn : kBlightCap));
      CHECK(use(SLOT_WAIT));
      CHECK(settle());
   }
   CHECK(shown("Cube withers.") == kBlightCap + 2);
}

/// A length: Doze lasts 2 of the holder's moves (one lost turn, then it wakes).
void test_length()
{
   CHECK(begin(PARTY_MENDER, PARTY_WAITER));
   CHECK(use(SLOT_LULLABY));
   CHECK(settle());
   CHECK(g_state.battlers[kMenderFoe].status == STATUS_DOZE);
   for (int turn = 0; turn < kHazeTurns + 1 && g_state.battlers[kMenderFoe].status != 0; ++turn)
   {
      CHECK(use(SLOT_WAIT));
      CHECK(settle());
   }
   CHECK(g_state.battlers[kMenderFoe].status == 0);
   CHECK(shown("Cube dozes.") == 1);
   CHECK(shown("Cube woke.") == 1);
}

/// The taken record: the foe's hit is kept (amount, from, move, turn); Retort hits back one and a half times the amount.
void test_taken()
{
   CHECK(begin(PARTY_MENDER, PARTY_HITTER));
   CHECK(use(SLOT_RETORT));
   CHECK(settle());
   const Taken& t = g_state.battlers[kLead].taken;
   CHECK(t.amount > 0 && t.from == kMenderFoe && t.move == MOVE_TAP && t.turn == 1);
   CHECK(t.category == kData.moves[MOVE_TAP].category);
   const Battler& foe = g_state.battlers[kMenderFoe];
   CHECK(foe.hp == max_hp(foe) - t.amount * 3 / 2);
}

/// Already: a volatile started again runs its already hook (its text).
void test_already()
{
   CHECK(begin(PARTY_MENDER, PARTY_WAITER));
   CHECK(use(SLOT_HOVER));
   CHECK(use(SLOT_HOVER));
   CHECK(settle());
   CHECK(has(kLead, VOLATILE_HOVER));
   CHECK(shown("Cube already hovers.") == 1);
}

} // namespace

int main()
{
   test_word();
   test_lock_in();
   test_charge();
   test_layers_and_tags();
   test_side_turns();
   test_field();
   test_escalation();
   test_length();
   test_taken();
   test_already();
   std::printf("battle condition tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
