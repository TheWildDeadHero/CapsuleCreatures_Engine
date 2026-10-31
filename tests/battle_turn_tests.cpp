/// @file
/// Tests for the manifest's turn structure (D-22 A11, D-23 section 2 `turn`, E6b) against the battle data compiled
/// from tests/data/battle_turn/battle.json, whose `turn` differs from the sample's (the engine's former fixed
/// order) in every part: switches act after moves (bracket -10) while moves keep their priority, equal speeds act
/// in position order, the end of turn runs the field, then each battler's passive and status residual hooks only,
/// the sides once per condition and the weather last, every dispatch runs passives before statuses, and the battle
/// start sends out in position order with the switch-in hooks in that order. The owners' texts show the order
/// (turn.ceffect).
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
/// @param left     the human's party
/// @param right    the AI's party
/// @param fmt      format
/// @param weather  starting weather, or kNone8
/// @return true if it started
bool begin(uint16_t left, uint16_t right, uint8_t fmt = FORMAT_SINGLE, uint8_t weather = kNone8)
{
   g_log = Log{};
   Setup su{};
   su.format = fmt;
   su.weather = weather;
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

/// Whether an event formats to exactly @p text.
/// @param e     the event
/// @param text  the expected text (the default code page: ASCII codes)
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
/// @return how many
int shown(const char* text)
{
   int n = 0;
   for (int k = 0; k < g_log.count; ++k)
   {
      n += reads(g_log.events[k], text) ? 1 : 0;
   }
   return n;
}

/// The first logged event at or after @p from that reads @p text.
/// @param text  the expected text
/// @param from  first log index searched
/// @return its log index, or -1
int text_at(const char* text, int from = 0)
{
   for (int k = from < 0 ? 0 : from; k < g_log.count; ++k)
   {
      if (reads(g_log.events[k], text))
      {
         return k;
      }
   }
   return -1;
}

/// The first logged event of a kind and battler at or after @p from.
/// @param kind     kind
/// @param battler  battler
/// @param from     first log index searched
/// @return its log index, or -1
int event_at(EventKind kind, uint8_t battler, int from = 0)
{
   for (int k = from < 0 ? 0 : from; k < g_log.count; ++k)
   {
      if (g_log.events[k].kind == kind && g_log.events[k].battler == battler)
      {
         return k;
      }
   }
   return -1;
}

/// Whether a battler holds a volatile.
/// @param i  battler
/// @param v  volatile
/// @return true when on
bool has_volatile(uint8_t i, uint8_t v)
{
   return (g_state.battlers[i].vol >> v & 1u) != 0;
}

/// Battlers of a Left (two members) against a one-member party: Left's lead, its second member, the foe.
constexpr uint8_t kLead = 0;
constexpr uint8_t kSecond = 1;   ///< See kLead.
constexpr uint8_t kFoe = 2;      ///< See kLead.
/// Battlers of Left against Waiters in the Double format: Left's two, then the foes' two.
constexpr uint8_t kFirstFoe = 2;
/// Members of the Left party (both on the field in the Double format).
constexpr int kLeftMembers = 2;
/// Move slots of Left's lead.
enum LeadSlot : uint8_t
{
   SLOT_WAIT,
   SLOT_HAZE,
   SLOT_HUM,
   SLOT_SCREEN,
   SLOT_TAP,
   SLOT_DASH,
};
/// Turns the tie test runs.
constexpr int kTieTurns = 6;
/// Turns the screen test runs (one more than the screen's 3).
constexpr int kScreenTestTurns = 4;
/// Turns of the Screen record (its residual runs at the end of each).
constexpr int kScreenTurns = 3;
/// Move slots of the Gusty party's creature.
enum GustySlot : uint8_t
{
   SLOT_GUST_GALE,
   SLOT_GUST_WAIT,
};
/// Turns of the Gale record.
constexpr int kGaleTurns = 2;

/// Brackets: a switch (bracket -10) acts after the foe's move; a move's own priority still counts.
void test_brackets()
{
   CHECK(begin(PARTY_LEFT, PARTY_HITTER));
   const int from = g_log.count;
   CHECK(switch_to(kSecond));
   CHECK(settle());
   const int hit = event_at(EventKind::UseMove, kFoe, from);
   const int in = event_at(EventKind::SwitchIn, kSecond, from);
   CHECK(hit >= 0 && in > hit);   // the foe's move came first: it hit the lead, which then went back
   CHECK(g_state.battlers[kLead].hp < max_hp(g_state.battlers[kLead]));
   CHECK(g_state.battlers[kSecond].hp == max_hp(g_state.battlers[kSecond]));

   // the slower lead's priority move acts before the foe's
   CHECK(begin(PARTY_LEFT, PARTY_HITTER));
   CHECK(g_state.battlers[kLead].stats[STAT_PACE] < g_state.battlers[kFoe].stats[STAT_PACE]);
   const int turn = g_log.count;
   CHECK(use(SLOT_DASH));
   CHECK(settle());
   const int dash = event_at(EventKind::UseMove, kLead, turn);
   const int tap = event_at(EventKind::UseMove, kFoe, turn);
   CHECK(dash >= 0 && tap > dash);
   CHECK(g_state.vm.faults == 0);
}

/// Position ties: equal speeds act in position order every turn, and no tiebreak byte is drawn.
void test_position_ties()
{
   CHECK(begin(PARTY_LEFT, PARTY_TWIN));
   CHECK(g_state.battlers[kLead].stats[STAT_PACE] == g_state.battlers[kFoe].stats[STAT_PACE]);
   for (int turn = 0; turn < kTieTurns; ++turn)
   {
      const int from = g_log.count;
      CHECK(use(SLOT_WAIT));
      CHECK(settle());
      const int first = event_at(EventKind::UseMove, kLead, from);
      const int second = event_at(EventKind::UseMove, kFoe, from);
      CHECK(first >= 0 && second > first);
      CHECK(g_state.battlers[kLead].tiebreak == 0 && g_state.battlers[kFoe].tiebreak == 0);
   }
}

/// The end of turn: the field's haze first, then each battler's passive and status residual hooks (passive
/// first; the volatile's residual is not in the phase), the screen phase, the weather last.
void test_end_of_turn()
{
   CHECK(begin(PARTY_LEFT, PARTY_BLIGHTER, FORMAT_SINGLE, WEATHER_DUST));
   const int from = g_log.count;
   CHECK(use(SLOT_HAZE));   // the faster foe blights the lead first
   CHECK(settle());
   const int haze = text_at("Blob is in the haze.", from);
   const int foe_mend = text_at("Cube is mending.", from);
   const int mend = text_at("Blob is mending.", from);
   const int withers = text_at("Blob withers.", from);
   const int dust = text_at("The dust swirls.", haze);   // (the battle start said it once too)
   const int buffeted = text_at("Blob is buffeted.", from);
   CHECK(haze >= 0 && text_at("Cube is in the haze.", from) >= 0);
   CHECK(foe_mend > haze && foe_mend > text_at("Cube is in the haze.", from));   // the field phase first
   CHECK(mend > foe_mend);       // battlers fastest first
   CHECK(withers > mend);        // the passive before the status
   CHECK(dust > withers);        // the weather last
   CHECK(buffeted > dust);

   // the hum volatile's residual handler never runs: its source is not in the battlers phase
   CHECK(use(SLOT_HUM));
   CHECK(settle());
   CHECK(has_volatile(kLead, VOLATILE_HUM));
   CHECK(shown("Blob hums.") == 0);
   CHECK(g_state.vm.faults == 0);
}

/// Dispatch order: the passive's before_move handler runs before the status's skip (which stops the dispatch).
void test_dispatch_order()
{
   CHECK(begin(PARTY_LEFT, PARTY_SLEEPER));
   const int from = g_log.count;
   CHECK(use(SLOT_WAIT));   // the faster foe sends the lead to sleep first
   CHECK(settle());
   const int dozed = text_at("Blob dozes off.", from);
   const int ready = text_at("Blob is ready.", dozed);
   const int skip = text_at("Blob dozes.", dozed);
   CHECK(dozed >= 0 && ready > dozed && skip > ready);
   CHECK(event_at(EventKind::UseMove, kLead, from) < 0);   // the turn was lost
}

/// A sides phase with "each": "once": the screen's residual runs once per turn for its side of two battlers.
void test_sides_once()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITERS, FORMAT_DOUBLE));
   for (int turn = 0; turn < kScreenTestTurns; ++turn)
   {
      for (int k = 0; k < kLeftMembers; ++k)   // each of Left's battlers chooses
      {
         Request r{};
         CHECK(until_request(r) && r.kind == RequestKind::Action);
         const uint8_t slot = turn == 0 && r.battler == kLead ? SLOT_SCREEN : SLOT_WAIT;
         CHECK(choose(g_state, r.battler, Action{ActionKind::Move, slot, kBench, 0}));
      }
   }
   CHECK(settle());
   CHECK(g_state.at[0] == kLead && g_state.at[1] == kSecond && g_state.battlers[kFirstFoe].position != kBench);
   CHECK(shown("A screen went up!") == 1);
   CHECK(shown("Blob's side glows.") == kScreenTurns);   // once per turn, not once per battler
   CHECK(shown("The screen faded.") == 1);
}

/// The battle start: send-outs in position order, then the switch-in hooks in that order (the faster foe's
/// passive speaks second).
void test_start()
{
   CHECK(begin(PARTY_LEFT, PARTY_HITTER));
   CHECK(settle());
   const int lead_in = event_at(EventKind::SwitchIn, kLead);
   const int foe_in = event_at(EventKind::SwitchIn, kFoe);
   CHECK(lead_in >= 0 && foe_in > lead_in);
   const int lead_hello = text_at("Blob says hello.");
   const int foe_hello = text_at("Cube says hello.");
   CHECK(lead_hello > foe_in && foe_hello > lead_hello);
}

/// The weather's lifecycle as a field condition's (C1: weather_on() / weather_off()): a Setup weather's start hook
/// runs once the battlers are out; set again, its already hook runs (no start); set by a move, try_start, the
/// started text and start run; when its turns run out, the ended text and its end hook. Its start hook starts a
/// volatile on the same holder, whose own start hook runs inside it (the guard is keyed on the owner).
void test_weather_lifecycle()
{
   CHECK(begin(PARTY_GUSTY, PARTY_WAITERS, FORMAT_SINGLE, WEATHER_GALE));
   CHECK(settle());
   CHECK(shown("Blob feels the gale.") == 1 && shown("Blob catches a breeze.") == 1);
   CHECK(shown("The gale gathers.") == 0);   // a Setup weather is not tried
   CHECK(use(SLOT_GUST_GALE));
   CHECK(settle());
   CHECK(shown("The gale blows harder.") == 1 && shown("A gale blows!") == 0);
   CHECK(shown("Blob feels the gale.") == 1);   // no second start

   CHECK(begin(PARTY_GUSTY, PARTY_WAITERS));
   CHECK(use(SLOT_GUST_GALE));
   for (int t = 0; t < kGaleTurns; ++t)
   {
      CHECK(use(SLOT_GUST_WAIT));
   }
   CHECK(settle());
   const int tried = text_at("The gale gathers.");
   const int started = text_at("A gale blows!", tried + 1);
   const int on = text_at("Blob feels the gale.", started + 1);
   const int breeze = text_at("Blob catches a breeze.", on + 1);
   const int ended = text_at("The gale died down.", breeze + 1);
   const int gone = text_at("The gale is gone.", ended + 1);
   CHECK(tried >= 0 && started > tried && on > started && breeze > on && ended > breeze && gone > ended);
   CHECK(g_state.weather == kNone8);
}

/// A weather replaced by another (C4): the old weather's end hook runs while it is still the weather (its
/// `when field.weather == Dust` guard holds), before the new one's started text and start hook.
void test_weather_replaced()
{
   CHECK(begin(PARTY_GUSTY, PARTY_WAITERS, FORMAT_SINGLE, WEATHER_DUST));
   CHECK(use(SLOT_GUST_GALE));
   CHECK(settle());
   const int dust_off = text_at("The dust is gone.");
   const int started = text_at("A gale blows!");
   CHECK(dust_off >= 0 && started > dust_off);
   CHECK(shown("The dust is gone.") == 1);
   CHECK(g_state.weather == WEATHER_GALE);
}

/// C6 (review4 finding 19): the weather_changed hook runs as soon as the acting weather changes (the gale starts:
/// Vane turns before the turn ends; the gale ends: it turns again), and a holder of the weather_suppress_tag
/// (Calm) keeps the weather from acting: no turn, no residual damage, field.weather none.
void test_weather_changed()
{
   CHECK(begin(PARTY_VANES, PARTY_WAITERS));
   const int from = g_log.count;
   CHECK(use(SLOT_GUST_GALE));
   CHECK(settle());
   const int started = text_at("A gale blows!", from);
   const int turned = text_at("Vanebird turns.", started + 1);
   const int mended = text_at("Cube is mending.", started + 1);
   CHECK(started >= 0 && turned > started && mended > turned);   // at once, before the end of the turn
   for (int t = 0; t < kGaleTurns; ++t)
   {
      CHECK(use(SLOT_GUST_WAIT));
   }
   CHECK(settle());
   CHECK(shown("Vanebird turns.") == 2 && g_state.weather == kNone8);
   // Calm on the field: the dust stays but does not act (no buffeting); Vane turned once, when Calm came out
   CHECK(begin(PARTY_VANES, PARTY_CALMERS, FORMAT_SINGLE, WEATHER_DUST));
   CHECK(use(SLOT_GUST_WAIT));
   CHECK(settle());
   CHECK(g_state.weather == WEATHER_DUST && shown("Vanebird is buffeted.") == 0);
   CHECK(shown("Calmer is buffeted.") == 0 && shown("Vanebird turns.") == 1);
   CHECK(kData.weather_suppress_tags == 1u << TAG_CALM);
}

} // namespace

/// Runs every test.
/// @return 0 when all passed
int main()
{
   test_brackets();
   test_position_ties();
   test_end_of_turn();
   test_dispatch_order();
   test_sides_once();
   test_start();
   test_weather_lifecycle();
   test_weather_replaced();
   test_weather_changed();
   std::printf("battle_turn_tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
