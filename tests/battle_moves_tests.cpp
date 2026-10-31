/// @file
/// Tests for E13b's battle primitives on the battle data compiled from tests/data/battle_moves/battle.json
/// (moves.ceffect): a move's own point handlers (power against a volatile; the move_type point the engine runs on a
/// hit's type, which the weather's damage point then reads), side condition and volatile damage points (critical
/// hits pass a screen), the crit flag set before the before_damage hooks, silent stages and a stage reset, typed
/// fixed damage through before_damage, trapping (try_switch), redirection (redirect), a delayed effect (delay / the
/// land phase / the delayed end-of-turn phase), the reward pool, the battle ended against an unowned party, the last
/// move that targeted a battler with a chained move, species fields read by logic and point handlers, and the
/// transcript's opt-in owner texts (tests/support/battle_transcript.h kOwnerTexts). C5: hooks read the hit type
/// and the hit category (hit_category_by_type), foe traps (try_switch per_foe) and the texts-once trap query
/// (is_trapped()), redirection of chosen targets only (first claim wins; set_target moves too), the targeted_by
/// record past protection and reset on entry, and a battle ended in the middle of a unit (experience first).
#include "capsule/battle/battle.h"

#include "battle_ids.h"
#include "support/battle_transcript.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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
constexpr uint32_t kSeed = 1357;
/// Codes a formatted text may take.
constexpr int kTextCap = 96;
/// Positions of the Double format.
constexpr int kDoublePositions = 4;
/// A volatile word's highest value (a shell that never wears out).
constexpr uint16_t kMaxU16Word = 0xFFFF;
/// Turns of keen hits (crit stage 3: 3/10 each).
constexpr int kKeenTurns = 12;
/// Foresee's stored amount.
constexpr int kForeseeDamage = 25;
/// Coins: the user's level (20) times 5 per hit.
constexpr uint32_t kCoinsPerHit = 100;
/// The highest gene of the fixture's 4-bit genes.
constexpr uint8_t kHighGene = 15;
/// Turns the transcript tests play.
constexpr int kTranscriptTurns = 2;
/// Turns of random-foe moves against a spotlight (enough for both foes to be picked).
constexpr int kRandomTurns = 6;
/// Retort's fixed damage.
constexpr int kRetortDamage = 10;
State g_state;   ///< Battle state (large; static storage).

/// What the battle showed.
struct Log
{
   Event events[kLogEvents];   ///< Events in order.
   int count;                  ///< Events kept.
};

Log g_log;   ///< The current test's events.

/// @name Battlers
/// Single: Left's lead and its second member, then the foe (Hitter). Double: Left's two, then Waiters' two, each at
/// the position of its index (a Plan's target is a position).
///@{
constexpr uint8_t kLead = 0;
constexpr uint8_t kSecond = 1;
constexpr uint8_t kFoe = 2;
constexpr uint8_t kFoe2 = 3;
///@}

/// Move slots of Left's lead.
enum LeadSlot : uint8_t
{
   SLOT_TAP,
   SLOT_GUST,
   SLOT_SHIFT,
   SLOT_KEEN,
   SLOT_FIXED,
   SLOT_SHOO,
};
/// Move slots of Left's second member.
enum SecondSlot : uint8_t
{
   SLOT_WAIT2,
   SLOT_PUMP,
   SLOT_CALM,
   SLOT_COINS,
   SLOT_ECHO,
   SLOT_HEAVE,
};
/// Move slots of Waiters' first member.
enum WaiterSlot : uint8_t
{
   SLOT_W_WAIT,
   SLOT_W_RISE,
   SLOT_W_SCREEN,
   SLOT_W_BRACE,
   SLOT_W_SHELL,
   SLOT_W_SPOTLIGHT,
};
/// Move slots of Waiters' second member and of Hitter.
enum FoeSlot : uint8_t
{
   SLOT_F_FIRST,    ///< Waiters' second: Wait; Hitter: Tap.
   SLOT_F_SNARE,
   SLOT_F_FORESEE,
   SLOT_F_LAST,     ///< Waiters' second: Tap; Hitter: Wait.
   SLOT_F_HOLD,     ///< Hitter: Hold (C5).
};
/// Move slots of Lures' members (C5).
enum LureSlot : uint8_t
{
   SLOT_L_SPOTLIGHT,
   SLOT_L_WAIT,
   SLOT_L_THIRD,     ///< First member: Tap; second: Lash.
   SLOT_L_FOURTH,    ///< First member: Finale; second: Retort.
   SLOT_L_WARD,      ///< Second member.
   SLOT_L_SINK,      ///< Second member.
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

/// Starts a battle of two human-controlled data parties (every choice is the test's).
/// @param left        side 0's party
/// @param right       side 1's party
/// @param fmt         format
/// @param right_flags PartyFlag bits of side 1 (e.g. unowned)
/// @return true if it started
bool begin(uint16_t left, uint16_t right, uint8_t fmt = FORMAT_SINGLE, uint8_t right_flags = 0)
{
   g_log = Log{};
   Setup su{};
   su.format = fmt;
   su.weather = kNone8;
   su.seed = kSeed;
   su.participants[su.count++] = Participant{&kData.parties[left], nullptr, nullptr, Controller::Human, 0, 0, 0};
   su.participants[su.count++] =
      Participant{&kData.parties[right], nullptr, nullptr, Controller::Human, 1, right_flags, 0};
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

/// One battler's choice for a turn.
struct Plan
{
   uint8_t slot;     ///< Move slot.
   uint8_t target;   ///< Target position (kBench: the default).
};

/// Plays one turn: every Action request of the turn is answered from @p plans (by battler), then the battle runs to
/// the next turn's first request (or its end).
/// @param plans  a plan per battler (indexed by battler)
/// @return false when a choice was refused
bool turn(const Plan* plans)
{
   Request r{};
   if (!until_request(r))
   {
      return true;   // the battle is over
   }
   const uint16_t at = g_state.turn;
   while (r.kind == RequestKind::Action && g_state.turn == at)
   {
      const Plan& p = plans[r.battler];
      if (!choose(g_state, r.battler, Action{ActionKind::Move, p.slot, p.target, 0}))
      {
         return false;
      }
      if (!until_request(r))
      {
         break;   // it ended
      }
   }
   return true;
}

/// Runs to the first request of a turn (the battle start).
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

/// Prints the log's texts from @p from on when the environment variable BATTLE_MOVES_DUMP is set (debugging).
/// @param from  first log index
void dump(int from = 0)
{
   if (std::getenv("BATTLE_MOVES_DUMP") == nullptr)
   {
      return;
   }
   for (int k = from; k < g_log.count; ++k)
   {
      text::Code buf[kTextCap];
      const Event& e = g_log.events[k];
      if (format_text(g_state, e, buf, kTextCap))
      {
         std::string line;
         transcript::put_codes(line, buf);
         std::printf("  %d> %s\n", k, line.c_str());
      }
      else
      {
         std::printf("  %d: kind %d battler %d a %d b %d\n", k, static_cast<int>(e.kind), e.battler, e.a, e.b);
      }
   }
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

/// Logged events of a kind from @p from on.
/// @param kind  the kind
/// @param from  first log index
/// @return how many
int events_of(EventKind kind, int from = 0)
{
   int n = 0;
   for (int k = from; k < g_log.count; ++k)
   {
      n += g_log.events[k].kind == kind ? 1 : 0;
   }
   return n;
}

/// A damage preview of a move from one battler to another (lowest roll).
/// @param user    attacker
/// @param target  defender
/// @param move    move
/// @param crit    as a critical hit
/// @return the damage
int32_t preview(uint8_t user, uint8_t target, uint16_t move, bool crit = false)
{
   return damage_preview(g_state, user, target, move, false, 0, crit);
}

/// Points: the move's own power handler (twice against an aloft target), the move_type point (Shift is a Stone move
/// in the dust, which the weather's damage point doubles; Tap stays Plain), a screen on the target's side halving
/// non-critical hits, a guarded volatile halving every hit, and a power read from the target's species field.
void test_points()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITERS, FORMAT_DOUBLE));
   CHECK(settle());
   const int32_t tap = preview(kLead, kFoe, MOVE_TAP);
   const int32_t gust = preview(kLead, kFoe, MOVE_GUST);
   g_state.battlers[kFoe].vol |= 1u << VOLATILE_ALOFT;
   const int32_t gust_aloft = preview(kLead, kFoe, MOVE_GUST);
   CHECK(gust > 0 && gust_aloft >= 2 * gust - 1 && gust_aloft <= 2 * gust + 1);
   g_state.battlers[kFoe].vol = 0;

   // the move's type: Plain without weather, Stone in the dust: no same-type bonus for the Plain user any more, and
   // the dust's damage point (which reads the hit's type) doubles it; Tap stays Plain
   CHECK(preview(kLead, kFoe, MOVE_SHIFT) == tap);
   g_state.battlers[kLead].types[0] = TYPE_STONE;
   const int32_t plain_hit = preview(kLead, kFoe, MOVE_TAP);   // no same-type bonus
   g_state.battlers[kLead].types[0] = TYPE_PLAIN;
   CHECK(plain_hit < tap);
   g_state.weather = WEATHER_DUST;
   const int32_t shift_dust = preview(kLead, kFoe, MOVE_SHIFT);
   CHECK(preview(kLead, kFoe, MOVE_TAP) == tap);
   CHECK(shift_dust >= 2 * plain_hit - 1 && shift_dust <= 2 * plain_hit + 1);
   g_state.weather = kNone8;

   // a screen on the foes' side: hits on its battlers halved, critical ones not
   const int32_t crit = preview(kLead, kFoe, MOVE_TAP, true);
   g_state.sides[1].cond = static_cast<uint16_t>(1u << SIDE_COND_SCREEN);
   CHECK(preview(kLead, kFoe, MOVE_TAP) == tap / 2);
   CHECK(preview(kLead, kFoe, MOVE_TAP, true) == crit);
   CHECK(preview(kFoe, kLead, MOVE_TAP) > preview(kLead, kFoe, MOVE_TAP));   // the user's side has none
   g_state.sides[1].cond = 0;

   // a volatile on the target
   g_state.battlers[kFoe].vol |= 1u << VOLATILE_GUARDED;
   CHECK(preview(kLead, kFoe, MOVE_TAP) == tap / 2);
   CHECK(preview(kLead, kFoe, MOVE_TAP, true) == crit / 2);
   g_state.battlers[kFoe].vol = 0;

   // the user's gene and its side's active battlers: 10 + 4 x gene + 10 x 2
   g_state.battlers[kLead].genes[STAT_POWER] = 0;
   const int32_t knack_low = preview(kLead, kFoe, MOVE_KNACK);
   g_state.battlers[kLead].genes[STAT_POWER] = kHighGene;
   CHECK(preview(kLead, kFoe, MOVE_KNACK) > knack_low);
   CHECK(knack_low == gust);   // power 30, as Gust's

   // the target's species field as the power: a Cube's mass 90 against Tap's 40
   CHECK(preview(kSecond, kFoe, MOVE_HEAVE) > preview(kSecond, kFoe, MOVE_TAP));
   CHECK(preview(kFoe, kLead, MOVE_HEAVE) < preview(kFoe, kLead, MOVE_TAP));   // a Blob's mass is 30
   CHECK(g_state.vm.faults == 0);
}

/// The crit flag before the before_damage hooks, and typed fixed damage through them: the shell on the foe says it
/// saw every critical hit (the engine says "A critical hit!" after the damage), and soaks up fixed damage.
void test_shell()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITERS, FORMAT_DOUBLE));
   const Plan shell[kDoublePositions] = {{SLOT_FIXED, kFoe}, {SLOT_WAIT2, kBench}, {SLOT_W_SHELL, kBench},
                                         {SLOT_F_FIRST, kBench}};
   const int from = g_log.count;
   const uint16_t hp = g_state.battlers[kFoe].hp;
   CHECK(turn(shell));
   const uint8_t word = kData.volatiles[VOLATILE_SHELL].word;
   CHECK(g_state.battlers[kFoe].hp == hp);   // the shell (30) took the fixed 20
   CHECK(g_state.battlers[kFoe].vword[word] == 10);
   CHECK(text_at("Cube felt the crit coming!", from) < 0);

   // keen hits until some are critical: each one was seen before the damage
   g_state.battlers[kFoe].vword[word] = kMaxU16Word;
   const Plan keen[kDoublePositions] = {{SLOT_KEEN, kFoe}, {SLOT_WAIT2, kBench}, {SLOT_W_WAIT, kBench},
                                        {SLOT_F_FIRST, kBench}};
   const int hits = g_log.count;
   for (int k = 0; k < kKeenTurns; ++k)
   {
      g_state.battlers[kFoe].vword[word] = kMaxU16Word;
      CHECK(turn(keen));
   }
   dump(hits);
   const int crits = shown("A critical hit!", hits);
   CHECK(crits > 0);
   CHECK(shown("Cube felt the crit coming!", hits) == crits);
   const int seen = text_at("Cube felt the crit coming!", hits);
   CHECK(seen >= 0 && text_at("A critical hit!", hits) > seen);
   CHECK(g_state.battlers[kFoe].hp == max_hp(g_state.battlers[kFoe]));
   CHECK(g_state.vm.faults == 0);
}

/// Silent stages and a stage reset: no stage text, the Stage events stay; the owner text says what happened.
void test_stages()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITERS, FORMAT_DOUBLE));
   const Plan pump[kDoublePositions] = {{SLOT_TAP, kFoe}, {SLOT_PUMP, kBench}, {SLOT_W_WAIT, kBench},
                                        {SLOT_F_FIRST, kBench}};
   const int from = g_log.count;
   CHECK(turn(pump));
   CHECK(g_state.battlers[kSecond].stages[STAGE_POWER] == 2);
   CHECK(shown("Blob's Power rose sharply!", from) == 0);
   CHECK(text_at("Blob pumped up quietly.", from) >= 0);
   CHECK(events_of(EventKind::Stage, from) == 1);
   const Plan calm[kDoublePositions] = {{SLOT_TAP, kFoe}, {SLOT_CALM, kBench}, {SLOT_W_WAIT, kBench},
                                        {SLOT_F_FIRST, kBench}};
   const int reset = g_log.count;
   CHECK(turn(calm));
   dump(from);
   CHECK(g_state.battlers[kSecond].stages[STAGE_POWER] == 0);
   CHECK(events_of(EventKind::Stage, reset) == 1);
   CHECK(shown("Blob's Power harshly fell!", reset) == 0);
   CHECK(g_state.vm.faults == 0);
}

/// Trapping: a snared battler cannot switch out or run away (the snare says why); its moves still go.
void test_trap()
{
   CHECK(begin(PARTY_LEFT, PARTY_HITTER));
   const Plan snare[kFoe + 1] = {{SLOT_TAP, kBench}, {SLOT_TAP, kBench}, {SLOT_F_SNARE, kBench}};
   CHECK(turn(snare));
   CHECK((g_state.battlers[kLead].vol >> VOLATILE_SNARED & 1u) != 0);
   Request r{};
   CHECK(until_request(r) && r.battler == kLead);
   const int from = g_log.count;
   // C5: the query says nothing and changes nothing; a refused choice says why once a turn
   const uint16_t queued = g_state.ev_count;
   const uint32_t stream = g_state.rng.state;
   CHECK(is_trapped(g_state, kLead));
   CHECK(!can_escape(g_state, kLead) && !can_switch_to(g_state, kLead, kSecond));
   CHECK(g_state.ev_count == queued && g_state.rng.state == stream);
   CHECK(!choose(g_state, kLead, Action{ActionKind::Switch, 0, kBench, kSecond}));
   CHECK(!choose(g_state, kLead, Action{ActionKind::Run, 0, kBench, 0}));
   CHECK(!choose(g_state, kLead, Action{ActionKind::Switch, 0, kBench, kSecond}));
   drain();
   CHECK(shown("Blob can't get away!", from) == 1);
   CHECK(!is_trapped(g_state, kFoe));
   CHECK(choose(g_state, kLead, Action{ActionKind::Move, SLOT_TAP, kBench, 0}));
   CHECK(g_state.vm.faults == 0);
}

/// A foe's trap (C5: try_switch per_foe): the holder's foes cannot switch out or run away; the holder's own switch is
/// not asked of its per_foe handler.
void test_foe_trap()
{
   CHECK(begin(PARTY_LEFT, PARTY_HITTER));
   const Plan hold[kFoe + 1] = {{SLOT_TAP, kBench}, {SLOT_TAP, kBench}, {SLOT_F_HOLD, kBench}};
   CHECK(turn(hold));
   CHECK((g_state.battlers[kFoe].vol >> VOLATILE_HOLD & 1u) != 0);
   CHECK(is_trapped(g_state, kLead));
   CHECK(!is_trapped(g_state, kFoe));
   CHECK(!can_escape(g_state, kLead) && !can_switch_to(g_state, kLead, kSecond));
   Request r{};
   CHECK(until_request(r) && r.battler == kLead);
   const int from = g_log.count;
   CHECK(!choose(g_state, kLead, Action{ActionKind::Run, 0, kBench, 0}));
   CHECK(!choose(g_state, kLead, Action{ActionKind::Switch, 0, kBench, kSecond}));
   drain();
   CHECK(shown("Cube won't let Blob go!", from) == 1);
   CHECK(choose(g_state, kLead, Action{ActionKind::Move, SLOT_TAP, kBench, 0}));
   CHECK(g_state.vm.faults == 0);
}

/// The hit type in hooks and the hit category (C5): Shift is a Stone move in the dust, which a sink's try_hit hook
/// absorbs (a Plain Shift passes it); a Stone hit takes the blast category (hit_category_by_type), recorded as the
/// target's taken category, a Plain one stays strike.
void test_hit_type()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITERS, FORMAT_DOUBLE));
   CHECK(settle());
   g_state.battlers[kFoe].vol |= 1u << VOLATILE_SINK;
   const Plan shift[kDoublePositions] = {{SLOT_SHIFT, kFoe}, {SLOT_WAIT2, kBench}, {SLOT_W_WAIT, kBench},
                                         {SLOT_F_FIRST, kBench}};
   uint16_t hp = g_state.battlers[kFoe].hp;
   CHECK(turn(shift));
   CHECK(g_state.battlers[kFoe].hp < hp);   // Plain: through
   CHECK(g_state.battlers[kFoe].taken.category == CATEGORY_STRIKE);
   g_state.weather = WEATHER_DUST;
   g_state.weather_turns = 0;
   hp = g_state.battlers[kFoe].hp;
   const int from = g_log.count;
   CHECK(turn(shift));
   CHECK(g_state.battlers[kFoe].hp == hp);   // Stone: absorbed
   CHECK(text_at("It doesn't affect Cube...", from) >= 0);
   const Plan other[kDoublePositions] = {{SLOT_SHIFT, kFoe2}, {SLOT_WAIT2, kBench}, {SLOT_W_WAIT, kBench},
                                         {SLOT_F_FIRST, kBench}};
   CHECK(turn(other));
   CHECK(g_state.battlers[kFoe2].taken.move == MOVE_SHIFT);
   CHECK(g_state.battlers[kFoe2].taken.category == CATEGORY_BLAST);
   const Plan tap[kDoublePositions] = {{SLOT_TAP, kFoe2}, {SLOT_WAIT2, kBench}, {SLOT_W_WAIT, kBench},
                                       {SLOT_F_FIRST, kBench}};
   CHECK(turn(tap));
   CHECK(g_state.battlers[kFoe2].taken.category == CATEGORY_STRIKE);
   CHECK(g_state.vm.faults == 0);
}

/// Redirection: the spotlight draws a foe's single-target move to its holder.
void test_redirect()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITERS, FORMAT_DOUBLE));
   const Plan plans[kDoublePositions] = {{SLOT_TAP, kFoe2}, {SLOT_WAIT2, kBench}, {SLOT_W_SPOTLIGHT, kBench},
                                         {SLOT_F_FIRST, kBench}};
   const int from = g_log.count;
   CHECK(turn(plans));
   CHECK(text_at("Cube drew the attention!", from) >= 0);
   CHECK(g_state.battlers[kFoe2].hp == max_hp(g_state.battlers[kFoe2]));
   CHECK(g_state.battlers[kFoe].hp < max_hp(g_state.battlers[kFoe]));
   CHECK(g_state.battlers[kFoe].targeted_by == MOVE_TAP);
   CHECK(g_state.battlers[kFoe2].targeted_by == kNone16);
   CHECK(g_state.vm.faults == 0);

   // C5: the first claim wins, even one that keeps the picked target (both Lures draw; the move picked the first)
   CHECK(begin(PARTY_LEFT, PARTY_LURES, FORMAT_DOUBLE));
   const Plan both[kDoublePositions] = {{SLOT_TAP, kFoe}, {SLOT_WAIT2, kBench}, {SLOT_L_SPOTLIGHT, kBench},
                                        {SLOT_L_SPOTLIGHT, kBench}};
   CHECK(turn(both));
   CHECK(g_state.battlers[kFoe].hp < max_hp(g_state.battlers[kFoe]));
   CHECK(g_state.battlers[kFoe2].hp == max_hp(g_state.battlers[kFoe2]));

   // a random foe is not a chosen target: the spotlight does not draw it every time
   CHECK(begin(PARTY_LURES, PARTY_WAITERS, FORMAT_DOUBLE));
   const Plan lash[kDoublePositions] = {{SLOT_L_WAIT, kBench}, {SLOT_L_THIRD, kBench}, {SLOT_W_SPOTLIGHT, kBench},
                                        {SLOT_F_FIRST, kBench}};
   for (int k = 0; k < kRandomTurns; ++k)
   {
      CHECK(turn(lash));
   }
   CHECK(g_state.battlers[kFoe2].hp < max_hp(g_state.battlers[kFoe2]));

   // a target phase's single target is drawn too (Retort aims at the foe that hit it, the spotlight takes it)
   CHECK(begin(PARTY_LURES, PARTY_WAITERS, FORMAT_DOUBLE));
   const Plan retort[kDoublePositions] = {{SLOT_L_WAIT, kBench}, {SLOT_L_FOURTH, kBench},
                                          {SLOT_W_SPOTLIGHT, kBench}, {SLOT_F_LAST, kSecond}};
   CHECK(turn(retort));
   dump(0);
   CHECK(g_state.battlers[kFoe2].hp == max_hp(g_state.battlers[kFoe2]));
   CHECK(g_state.battlers[kFoe].hp == max_hp(g_state.battlers[kFoe]) - kRetortDamage);
   CHECK(g_state.vm.faults == 0);
}

/// The targeted_by record (C5): a move a protection blocked did not reach its target (no record); a battler that
/// enters starts without one.
void test_targeted()
{
   CHECK(begin(PARTY_LEFT, PARTY_LURES, FORMAT_DOUBLE));
   const Plan ward[kDoublePositions] = {{SLOT_TAP, kFoe2}, {SLOT_WAIT2, kBench}, {SLOT_L_WAIT, kBench},
                                        {SLOT_L_WARD, kBench}};
   const int from = g_log.count;
   CHECK(turn(ward));
   CHECK(text_at("Cube protected itself!", from) >= 0);
   CHECK(g_state.battlers[kFoe2].hp == max_hp(g_state.battlers[kFoe2]));
   CHECK(g_state.battlers[kFoe2].targeted_by == kNone16);

   CHECK(begin(PARTY_LEFT, PARTY_HITTER));
   Request r{};
   CHECK(until_request(r) && r.battler == kLead);
   g_state.battlers[kSecond].targeted_by = MOVE_TAP;   // a stale record on the bench
   CHECK(choose(g_state, kLead, Action{ActionKind::Switch, 0, kBench, kSecond}));
   CHECK(until_request(r) && r.battler == kFoe);
   CHECK(choose(g_state, kFoe, Action{ActionKind::Move, SLOT_F_LAST, kBench, 0}));
   CHECK(until_request(r));
   CHECK(g_state.battlers[kSecond].position != kBench);
   CHECK(g_state.battlers[kSecond].targeted_by == kNone16);
   CHECK(g_state.vm.faults == 0);
}

/// A delayed effect: it lands at the end of the turn after the one it was made in, on whoever stands there, through
/// typed damage; a second one fails while one is pending; the position's turns are readable.
void test_delayed()
{
   CHECK(begin(PARTY_LEFT, PARTY_HITTER));
   const Plan foresee[kFoe + 1] = {{SLOT_TAP, kBench}, {SLOT_TAP, kBench}, {SLOT_F_FORESEE, kBench}};
   const int from = g_log.count;
   CHECK(turn(foresee));
   CHECK(text_at("Cube foresaw an attack!", from) >= 0);
   CHECK(g_state.delayed[g_state.battlers[kLead].position].turns == 1);
   const uint16_t hp = g_state.battlers[kLead].hp;
   const int second = g_log.count;
   CHECK(turn(foresee));
   CHECK(text_at("But it failed!", second) >= 0);   // one is pending at that position
   const int landed = text_at("Blob took the attack!", second);
   CHECK(landed > text_at("But it failed!", second));
   CHECK(g_state.delayed[g_state.battlers[kLead].position].turns == 0);
   CHECK(hp - g_state.battlers[kLead].hp >= kForeseeDamage);   // the foe's Foresee failed, its earlier one hit
   CHECK(g_state.battlers[kLead].taken.amount == kForeseeDamage);
   CHECK(g_state.vm.faults == 0);
}

/// The reward pool: each hit adds the user's level times 5 (announced with the rest on a victory).
void test_reward()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITERS, FORMAT_DOUBLE));
   const Plan coins[kDoublePositions] = {{SLOT_TAP, kFoe}, {SLOT_COINS, kFoe2}, {SLOT_W_WAIT, kBench},
                                         {SLOT_F_FIRST, kBench}};
   CHECK(turn(coins));
   CHECK(turn(coins));
   CHECK(shown("Coins scattered everywhere!") == 2);
   CHECK(g_state.reward == 2 * kCoinsPerHit);
   CHECK(g_state.vm.faults == 0);
}

/// Ending the battle: shooing an unowned foe ends it (nobody wins), with the End event; against an owned foe
/// without a replacement the move fails and the battle goes on.
void test_end_battle()
{
   CHECK(begin(PARTY_LEFT, PARTY_HITTER, FORMAT_SINGLE, static_cast<uint8_t>(1u << PARTY_UNOWNED)));
   const Plan shoo[kFoe + 1] = {{SLOT_SHOO, kBench}, {SLOT_SHOO, kBench}, {SLOT_F_LAST, kBench}};
   const int from = g_log.count;
   turn(shoo);
   CHECK(g_state.phase == Phase::Ended);
   CHECK(g_state.outcome == Outcome::Escaped);
   CHECK(text_at("Cube ran off!", from) >= 0);
   CHECK(events_of(EventKind::End, from) == 1);
   CHECK(step(g_state) == Progress::Ended);

   CHECK(begin(PARTY_LEFT, PARTY_HITTER));
   const int owned = g_log.count;
   CHECK(turn(shoo));
   CHECK(text_at("But it failed!", owned) >= 0);
   CHECK(g_state.phase != Phase::Ended);
   CHECK(g_state.vm.faults == 0);

   // C5: a knockout and the end in one move: the after_move phase ends it, the user's after_move hooks do not run,
   // and the experience earned is paid before the End event
   CHECK(begin(PARTY_LURES, PARTY_HITTER, FORMAT_SINGLE));
   CHECK(settle());
   g_state.battlers[kLead].vol |= 1u << VOLATILE_CHEER;
   const uint32_t exp = g_state.battlers[kLead].exp;
   const Plan finale[kFoe + 1] = {{SLOT_L_FOURTH, kBench}, {SLOT_L_WAIT, kBench}, {SLOT_F_LAST, kBench}};
   const int last = g_log.count;
   turn(finale);
   dump(last);
   CHECK(g_state.phase == Phase::Ended && g_state.outcome == Outcome::Escaped);
   CHECK(text_at("Cube cheered!", last) < 0);
   int exp_at = -1;
   int end_at = -1;
   for (int k = last; k < g_log.count; ++k)
   {
      exp_at = exp_at < 0 && g_log.events[k].kind == EventKind::Exp ? k : exp_at;
      end_at = end_at < 0 && g_log.events[k].kind == EventKind::End ? k : end_at;
   }
   CHECK(exp_at >= 0 && end_at > exp_at);
   CHECK(g_state.battlers[kLead].exp > exp);
   CHECK(g_state.vm.faults == 0);
}

/// The last move that targeted a battler (any move) and a chained move: Echo fails before anything targeted its
/// user, then uses the foe's Tap; it reads the species field of its user with effect logic (Heave).
void test_echo()
{
   CHECK(begin(PARTY_LEFT, PARTY_WAITERS, FORMAT_DOUBLE));
   const Plan first[kDoublePositions] = {{SLOT_TAP, kFoe}, {SLOT_ECHO, kBench}, {SLOT_W_WAIT, kBench},
                                         {SLOT_F_FIRST, kBench}};
   const int from = g_log.count;
   CHECK(turn(first));
   CHECK(text_at("But it failed!", from) >= 0);
   const Plan tapped[kDoublePositions] = {{SLOT_TAP, kFoe}, {SLOT_ECHO, kBench}, {SLOT_W_WAIT, kBench},
                                          {SLOT_F_LAST, kSecond}};
   const int echo = g_log.count;
   CHECK(turn(tapped));
   CHECK(g_state.battlers[kSecond].targeted_by == MOVE_TAP);
   const int used = text_at("Blob used Echo!", echo);
   CHECK(used >= 0 && text_at("Blob used Tap!", used) > used);

   const Plan heave[kDoublePositions] = {{SLOT_TAP, kFoe}, {SLOT_HEAVE, kFoe2}, {SLOT_W_WAIT, kBench},
                                         {SLOT_F_FIRST, kBench}};
   const int weighed = g_log.count;
   CHECK(turn(heave));
   CHECK(text_at("Hit 30 time(s)!", weighed) >= 0);   // the user's mass
   CHECK(g_state.vm.faults == 0);
}

/// The transcript's owner texts: off by default (the goldens), with kOwnerTexts an owner text, a side condition and
/// a volatile line.
void test_transcript()
{
   for (int pass = 0; pass < 2; ++pass)
   {
      const uint8_t extras = pass == 0 ? transcript::kPlainTranscript : transcript::kOwnerTexts;
      CHECK(begin(PARTY_LEFT, PARTY_WAITERS, FORMAT_DOUBLE));
      std::string out;
      const Plan plans[kDoublePositions] = {{SLOT_TAP, kFoe}, {SLOT_PUMP, kBench}, {SLOT_W_SCREEN, kBench},
                                            {SLOT_F_FIRST, kBench}};
      Request r{};
      for (int k = 0; k < kStepLimit && g_state.turn < kTranscriptTurns; ++k)
      {
         transcript::drain(g_state, out, nullptr, nullptr, extras);
         if (step(g_state) == Progress::Input && request(g_state, r))
         {
            choose(g_state, r.battler, Action{ActionKind::Move, plans[r.battler].slot, plans[r.battler].target, 0});
         }
      }
      transcript::drain(g_state, out, nullptr, nullptr, extras);
      const bool owner = pass == 1;
      CHECK((out.find("> Blob pumped up quietly.\n") != std::string::npos) == owner);
      CHECK((out.find("side 1 Screen turns 5 layers 1\n") != std::string::npos) == owner);
      CHECK(out.find("> Cube used Screen!\n") != std::string::npos);
   }
   // the volatile line (aloft has "hud")
   CHECK(begin(PARTY_LEFT, PARTY_WAITERS, FORMAT_DOUBLE));
   std::string out;
   const Plan rise[kDoublePositions] = {{SLOT_TAP, kFoe2}, {SLOT_WAIT2, kBench}, {SLOT_W_RISE, kBench},
                                        {SLOT_F_FIRST, kBench}};
   Request r{};
   for (int k = 0; k < kStepLimit && g_state.turn < kTranscriptTurns; ++k)
   {
      transcript::drain(g_state, out, nullptr, nullptr, transcript::kOwnerTexts);
      if (step(g_state) == Progress::Input && request(g_state, r))
      {
         choose(g_state, r.battler, Action{ActionKind::Move, rise[r.battler].slot, rise[r.battler].target, 0});
      }
   }
   CHECK(out.find("volatile #2 Cube aloft on\n") != std::string::npos);
}

} // namespace

/// Runs every test.
/// @return 0 when all passed
int main()
{
   test_points();
   test_shell();
   test_stages();
   test_trap();
   test_foe_trap();
   test_hit_type();
   test_redirect();
   test_targeted();
   test_delayed();
   test_reward();
   test_end_battle();
   test_echo();
   test_transcript();
   std::printf("battle_moves_tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
