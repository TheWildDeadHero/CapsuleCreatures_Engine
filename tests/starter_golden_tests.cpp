/// @file
/// Neutral golden tests of capsule::battle against the Starter sample (samples/starter, E9 / D-25): the engine's
/// neutral reference fixture, built with the engine's default random generator (src/core/random_default.cpp).
///
/// Golden files, under the folder given as the first argument (tests/data/starter/golden):
///
/// - transcripts/NAME.txt: scripted battles in the WP-2 transcript format (tests/support/battle_transcript.h),
///   with the Starter sample's MP and EP: a line per Resource event (costs, the regen phase, effect logic) and the
///   pools in the summary's battler and record lines. The bench scenarios tests/bench/starter/*.json reproduce them.
/// - odds_grid.txt: battle::effect_odds_preview() (the ruleset's effect_odds pipeline) over the user's and the
///   target's Intelligence, with and without the talents that change the odds.
///
/// Besides the files, the program checks the sample's rules directly: its own numbers (C2: caps, stage tables,
/// crit odds and factor, chart tiers, the damage roll, regen, escape, experience, reward), Intelligence raises the
/// user's odds and lowers the target's, the odds / resist talents scale them, a move whose cost cannot be paid is
/// refused and the free fallback Strike runs, EP regenerates in the regen phase and MP does not, and MP / EP
/// persist in the records.
///
/// The test compares each file exactly and reports the first differing line. `--record` (or the environment
/// variable CAPSULE_GOLDEN_RECORD=1) rewrites the files instead; they were recorded in E9 and re-recorded once in C2
/// (the Starter's own numbers and formulas, README "Starter rules").
#include "capsule/battle/battle.h"
#include "capsule/battle/pipeline.h"
#include "capsule/battle/ruleset.h"
#include "capsule/monster/monster.h"

#include "battle_ids.h"
#include "support/battle_transcript.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
using namespace capsule::battle::transcript;

// -- output ---------------------------------------------------------------------------------------

/// Golden folder (first argument).
const char* g_dir = nullptr;
/// Rewrite the files instead of comparing.
bool g_record = false;
int g_failures = 0;   ///< Failed comparisons and checks.
int g_files = 0;      ///< Files compared or written.
int g_checks = 0;     ///< Direct checks made.

/// Bytes read from a golden file at a time.
constexpr size_t kReadChunk = 4096;

/// Reads a whole file.
/// @param path  the file
/// @param text  filled with its bytes
/// @return false when it cannot be opened
bool read_file(const std::string& path, std::string& text)
{
   FILE* f = std::fopen(path.c_str(), "rb");
   if (f == nullptr)
   {
      return false;
   }
   char buf[kReadChunk];
   size_t n = 0;
   while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
   {
      text.append(buf, n);
   }
   std::fclose(f);
   return true;
}

/// Reports the first line where two texts differ.
/// @param rel   the golden file (messages)
/// @param want  the golden text
/// @param have  the produced text
void report_difference(const char* rel, const std::string& want, const std::string& have)
{
   size_t a = 0;
   size_t b = 0;
   for (int line = 1;; ++line)
   {
      const size_t ea = want.find('\n', a);
      const size_t eb = have.find('\n', b);
      const std::string la = want.substr(a, ea == std::string::npos ? std::string::npos : ea - a);
      const std::string lb = have.substr(b, eb == std::string::npos ? std::string::npos : eb - b);
      if (la != lb || ea == std::string::npos || eb == std::string::npos)
      {
         std::printf("FAIL %s:%d\n  golden: %s\n  now:    %s\n", rel, line, la.c_str(), lb.c_str());
         return;
      }
      a = ea + 1;
      b = eb + 1;
   }
}

/// Compares text with a golden file, or writes it in record mode.
/// @param rel   path under the golden folder
/// @param text  produced text
void golden(const char* rel, const std::string& text)
{
   ++g_files;
   const std::string path = std::string(g_dir) + "/" + rel;
   if (g_record)
   {
      FILE* f = std::fopen(path.c_str(), "wb");
      if (f == nullptr || std::fwrite(text.data(), 1, text.size(), f) != text.size())
      {
         std::printf("FAIL cannot write %s\n", path.c_str());
         ++g_failures;
      }
      if (f != nullptr)
      {
         std::fclose(f);
      }
      return;
   }
   std::string want;
   if (!read_file(path, want))
   {
      std::printf("FAIL missing %s (record with --record)\n", path.c_str());
      ++g_failures;
      return;
   }
   if (want != text)
   {
      ++g_failures;
      report_difference(rel, want, text);
   }
}

/// Checks a condition (counted; failures are reported with their line).
#define CHECK(cond)                                                                                        \
   do                                                                                                      \
   {                                                                                                       \
      ++g_checks;                                                                                          \
      if (!(cond))                                                                                         \
      {                                                                                                    \
         std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
         ++g_failures;                                                                                     \
      }                                                                                                    \
   } while (false)

// -- creatures --------------------------------------------------------------------------------------

/// Party records used by the scenarios (static storage).
monster::Party g_parties[kMaxParties];
State g_state;   ///< The battle (large; static storage).

/// Adds a creature to a party (the goldens' deterministic record seed).
/// @param party    party index in g_parties
/// @param species  species index
/// @param level    level
/// @return the record
monster::Record& add(int party, uint16_t species, uint8_t level)
{
   monster::Party& p = g_parties[party];
   rng::Stream r{member_seed(party, p.count, species, level)};
   monster::Record& rec = p.members[p.count];
   rec = monster::Record{};
   if (!make_monster(rec, species, level, r))
   {
      std::printf("FAIL make_monster %d\n", species);
      ++g_failures;
   }
   ++p.count;
   return rec;
}

/// Sets a persisted battler resource of a record (MP, EP: their custom roles).
/// @param rec       record
/// @param resource  resource index (RESOURCE_MP, RESOURCE_EP)
/// @param value     the value
void set_pool(monster::Record& rec, uint8_t resource, uint32_t value)
{
   monster::set(rec, kData.resources[resource].role, 0, value);
}

/// A persisted battler resource of a record.
/// @param rec       record
/// @param resource  resource index
/// @return its value
uint32_t pool_of(const monster::Record& rec, uint8_t resource)
{
   return monster::get(rec, kData.resources[resource].role);
}

// -- transcripts ------------------------------------------------------------------------------------

/// @name Plan entries: what human battlers choose on a turn
///@{
constexpr int kFirstUsable = -1;   ///< The first move slot that can be chosen.
constexpr int kRun = 8;            ///< Run away (move slots are 0-7).
///@}

/// Longest plan.
constexpr int kMaxPlan = 8;
/// Most step() calls one scenario may take.
constexpr int kStepLimit = 20000;

/// One scripted battle.
struct Scenario
{
   const char* name;          ///< File name without ".txt".
   void (*build)(Setup& su);  ///< Fills g_parties and the setup.
   int plan[kMaxPlan];        ///< Human choice per turn (the last repeats).
   int plan_count;            ///< Entries used (0 = kFirstUsable throughout).
};

/// The plan entry for a turn.
/// @param sc    scenario
/// @param turn  turn (1 = first)
/// @return entry
int plan_for(const Scenario& sc, int turn)
{
   if (sc.plan_count == 0)
   {
      return kFirstUsable;
   }
   const int k = turn - 1 < sc.plan_count ? turn - 1 : sc.plan_count - 1;
   return sc.plan[k < 0 ? 0 : k];
}

/// Answers one human request from the plan (refused choices go into the transcript and fall back to the first
/// usable move, else the fallback Strike).
/// @param s    battle
/// @param r    request
/// @param sc   scenario
/// @param out  transcript
void answer(State& s, const Request& r, const Scenario& sc, std::string& out)
{
   if (r.kind == RequestKind::Replace)
   {
      replace(s, r, out);
      return;
   }
   if (r.kind != RequestKind::Action)
   {
      choose(s, r.battler, Action{ActionKind::Answer, 0, kBench, 0});   // effect requests: the first answer
      return;
   }
   const int p = plan_for(sc, s.turn);
   Choice c{ChoiceKind::First, 0, kBench, 0};
   if (p == kRun)
   {
      c.kind = ChoiceKind::Run;
   }
   else if (p >= 0)
   {
      c.kind = ChoiceKind::Move;
      c.slot = static_cast<uint8_t>(p);
   }
   act(s, r, c, out);
}

/// Runs one scenario to its end.
/// @param sc   scenario
/// @param out  its transcript
void play(const Scenario& sc, std::string& out)
{
   for (monster::Party& p : g_parties)
   {
      p = monster::Party{};
   }
   Setup su{};
   su.weather = kNone8;
   su.format = FORMAT_SINGLE;
   sc.build(su);
   put(out, "-- %s\n", sc.name);
   if (!start(g_state, su))
   {
      out += "start refused\n";
      return;
   }
   State& s = g_state;
   bool ended = false;
   for (int k = 0; k < kStepLimit && !ended; ++k)
   {
      drain(s, out);
      const Progress p = step(s);
      if (p == Progress::Ended)
      {
         drain(s, out);
         ended = true;
      }
      else if (p == Progress::Input)
      {
         Request r;
         if (request(s, r))
         {
            answer(s, r, sc, out);
         }
      }
   }
   if (!ended)
   {
      out += "unfinished\n";
   }
   summary(s, out);
}

/// Runs one scenario and compares its transcript.
/// @param sc  scenario
void run(const Scenario& sc)
{
   std::string out;
   play(sc, out);
   const std::string rel = std::string("transcripts/") + sc.name + ".txt";
   golden(rel.c_str(), out);
}

/// @name Participants
///@{

/// A human party with records.
/// @param su     setup
/// @param party  g_parties index (also the participant index)
/// @param side   side
void human(Setup& su, int party, uint8_t side)
{
   su.participants[su.count++] = Participant{nullptr, &g_parties[party], nullptr, Controller::Human, side, 0, 0};
}

/// An unowned party (AI: creatures without an owner).
/// @param su     setup
/// @param party  g_parties index
/// @param side   side
void unowned(Setup& su, int party, uint8_t side)
{
   su.participants[su.count++] =
      Participant{nullptr, &g_parties[party], nullptr, Controller::Ai, side, 1u << PARTY_UNOWNED, 0};
}

/// A party from the data (AI).
/// @param su     setup
/// @param party  party index
/// @param side   side
void rival(Setup& su, uint16_t party, uint8_t side)
{
   su.participants[su.count++] = Participant{&kData.parties[party], nullptr, nullptr, Controller::Ai, side, 0, 0};
}
///@}

/// @name Scenario seeds and levels (fixed: every transcript replays from its scenario)
///@{
constexpr uint32_t kSeedUnowned = 42;     ///< unowned_single.
constexpr uint32_t kSeedNovice = 7;       ///< rival_novice.
constexpr uint32_t kSeedScholar = 11;     ///< rival_scholar.
constexpr uint32_t kSeedDuo = 5;          ///< double_duo.
constexpr uint32_t kSeedEnergy = 3;       ///< out_of_energy.
constexpr uint32_t kSeedOdds = 21;        ///< odds_duel.
constexpr uint32_t kSeedRun = 9;          ///< unowned_run.
constexpr uint8_t kLowEp = 1;             ///< out_of_energy: the EP the creature starts with (Gale Kick costs 4).
///@}

// Scenario builders.

/// Single battle against an unowned creature: Fireball (8 MP a cast) while the MP lasts, then the first usable move.
/// @param su  setup
void build_unowned_single(Setup& su)
{
   add(0, SPECIES_EMBERKIT, 10);
   add(1, SPECIES_SPROUTLE, 7);
   human(su, 0, 0);
   unowned(su, 1, 1);
   su.seed = kSeedUnowned;
}

/// A rival with a presenter and two creatures: replacements, reward.
/// @param su  setup
void build_rival_novice(Setup& su)
{
   add(0, SPECIES_TIDEPUP, 12);
   human(su, 0, 0);
   rival(su, PARTY_NOVICE_KIT, 1);
   su.seed = kSeedNovice;
}

/// A rival with status moves, charms (Focus Lens, Mana Drop), the status AI and save_resources (C2).
/// @param su  setup
void build_rival_scholar(Setup& su)
{
   add(0, SPECIES_VOLTMINK, 16);
   add(0, SPECIES_GLIMMET, 15);
   human(su, 0, 0);
   rival(su, PARTY_SCHOLAR_WEN, 1);
   su.seed = kSeedScholar;
}

/// A double battle (a spread move, Vigor Bar).
/// @param su  setup
void build_double_duo(Setup& su)
{
   add(0, SPECIES_CLODBUN, 18);
   add(0, SPECIES_STARMOTE, 18);
   human(su, 0, 0);
   rival(su, PARTY_DUO_ASH_BLY, 1);
   su.format = FORMAT_DOUBLE;
   su.seed = kSeedDuo;
}

/// No MP and too little EP: the plan's moves are refused, Strike runs, EP regenerates until Gale Kick is usable.
/// @param su  setup
void build_out_of_energy(Setup& su)
{
   monster::Record& rec = add(0, SPECIES_BREEZLE, 20);
   set_pool(rec, RESOURCE_MP, 0);
   set_pool(rec, RESOURCE_EP, kLowEp);
   add(1, SPECIES_CLODBUN, 16);
   human(su, 0, 0);
   unowned(su, 1, 1);
   su.seed = kSeedEnergy;
}

/// A clever user against a dull target: Stardust Lull and Night Pulse (Intelligence -1) through effect_odds.
/// @param su  setup
void build_odds_duel(Setup& su)
{
   add(0, SPECIES_DUSKOWL, 30);
   add(1, SPECIES_CLODBUN, 30);
   human(su, 0, 0);
   unowned(su, 1, 1);
   su.seed = kSeedOdds;
}

/// Running from an unowned creature (the escape odds).
/// @param su  setup
void build_unowned_run(Setup& su)
{
   add(0, SPECIES_BREEZLE, 14);
   add(1, SPECIES_EMBERKIT, 10);
   human(su, 0, 0);
   unowned(su, 1, 1);
   su.seed = kSeedRun;
}

/// @name Move slots the plans use (the species' default moves: the last eight learned by their level; Strike, the
/// fallback, is never learned)
///@{
constexpr int kSlotFireball = 2;       ///< Emberkit Lv10: Flare Claw, Smolder, Fireball, Gale Kick.
constexpr int kSlotTideSlam = 0;       ///< Tidepup Lv12: Tide Slam, Mist Veil, Torrent, Quake.
constexpr int kSlotTorrent = 2;        ///< ...
constexpr int kSlotStaticField = 1;    ///< Voltmink Lv16: Shock Fang, Static Field, Arc Bolt, Gale Kick, Night Pulse.
constexpr int kSlotArcBolt = 2;        ///< ...
constexpr int kSlotThird = 2;          ///< Clodbun Lv18 Quake, Starmote Lv18 Starfall.
constexpr int kSlotGaleKick = 0;       ///< Breezle Lv20: Gale Kick, Updraft, Cyclone, Vine Lash, Arc Bolt.
constexpr int kSlotCyclone = 2;        ///< ...
constexpr int kSlotLull = 5;           ///< Duskowl Lv30: Shadow Bite, Dread Gaze, Night Pulse, ..., Stardust Lull.
constexpr int kSlotNightPulse = 2;     ///< ...
///@}

/// Every scripted battle.
const Scenario kScenarios[] = {
   {"unowned_single", build_unowned_single, {kSlotFireball}, 1},
   {"rival_novice", build_rival_novice, {kSlotTorrent, kSlotTideSlam}, 2},
   {"rival_scholar", build_rival_scholar, {kSlotStaticField, kSlotArcBolt}, 2},
   {"double_duo", build_double_duo, {kSlotThird}, 1},
   {"out_of_energy", build_out_of_energy, {kSlotCyclone, kSlotGaleKick}, 2},
   {"odds_duel", build_odds_duel, {kSlotLull, kSlotNightPulse}, 2},
   {"unowned_run", build_unowned_run, {kRun}, 1},
};

// -- effect odds ------------------------------------------------------------------------------------

/// Intelligence values of the odds grid.
constexpr uint16_t kGridIntelligence[] = {10, 50, 100, 200, 400};
/// Odds the grid asks for: a secondary effect (10 in 100) and a status move (90 in 100).
constexpr uint16_t kGridChances[] = {10, 90};
/// Outcomes of the grid's odds.
constexpr uint16_t kGridOutOf = 100;
/// Level of the grid's battlers.
constexpr uint8_t kGridLevel = 20;

/// Starts the grid's battle: battler 0 (Duskowl) against battler 1 (Clodbun), neither with a talent or a charm.
/// @return false when it cannot start
bool start_grid()
{
   for (monster::Party& p : g_parties)
   {
      p = monster::Party{};
   }
   add(0, SPECIES_DUSKOWL, kGridLevel);
   add(1, SPECIES_CLODBUN, kGridLevel);
   Setup su{};
   su.weather = kNone8;
   su.format = FORMAT_SINGLE;
   human(su, 0, 0);
   unowned(su, 1, 1);
   su.seed = kSeedOdds;
   if (!start(g_state, su))
   {
      return false;
   }
   for (uint8_t i = 0; i < g_state.battler_count; ++i)
   {
      g_state.battlers[i].passive = kNone8;
      g_state.battlers[i].item = 0;   // no charm
   }
   return true;
}

/// Odds of the grid: battler 0's effect against battler 1 at two Intelligence values.
/// @param by_int  battler 0's Intelligence
/// @param on_int  battler 1's Intelligence
/// @param n       odds asked for (in kGridOutOf)
/// @return the odds after the pipeline
Odds grid_odds(uint16_t by_int, uint16_t on_int, uint16_t n)
{
   g_state.battlers[0].stats[STAT_INTELLIGENCE] = by_int;
   g_state.battlers[1].stats[STAT_INTELLIGENCE] = on_int;
   return effect_odds_preview(g_state, 0, 1, n, kGridOutOf);
}

/// Appends one grid table (rows: the user's Intelligence; columns: the target's).
/// @param out    the grid text
/// @param title  what the table shows
/// @param n      odds asked for
void grid_table(std::string& out, const char* title, uint16_t n)
{
   put(out, "-- %s: odds(%u, %u), user Intelligence (rows) against target Intelligence (columns)\n", title,
       static_cast<unsigned>(n), static_cast<unsigned>(kGridOutOf));
   out += "user\\target";
   for (uint16_t t : kGridIntelligence)
   {
      put(out, " %8u", static_cast<unsigned>(t));
   }
   out += "\n";
   for (uint16_t u : kGridIntelligence)
   {
      put(out, "%11u", static_cast<unsigned>(u));
      for (uint16_t t : kGridIntelligence)
      {
         const Odds o = grid_odds(u, t, n);
         put(out, " %4u/%-3u", static_cast<unsigned>(o.n), static_cast<unsigned>(o.m));
      }
      out += "\n";
   }
}

/// The odds grid golden and the direct odds checks.
void test_odds()
{
   if (!start_grid())
   {
      std::printf("FAIL the odds grid's battle did not start\n");
      ++g_failures;
      return;
   }
   constexpr uint16_t kEven = 100;   // equal Intelligence: the odds asked for
   constexpr uint16_t kHigh = 200;
   constexpr uint16_t kLow = 50;
   constexpr uint16_t kAsked = 10;
   CHECK(grid_odds(kEven, kEven, kAsked).n == kAsked);
   CHECK(grid_odds(kHigh, kEven, kAsked).n > kAsked);   // a clever user lands it more often
   CHECK(grid_odds(kEven, kHigh, kAsked).n < kAsked);   // a clever target resists it
   CHECK(grid_odds(kLow, kEven, kAsked).n < grid_odds(kEven, kEven, kAsked).n);
   CHECK(grid_odds(kEven, kEven, kAsked).m == kGridOutOf);

   std::string out;
   for (uint16_t n : kGridChances)
   {
      grid_table(out, "no talents", n);
   }
   // the user's Insight (the odds point: x1.5) and the target's Stubborn (the resist point: x0.5)
   g_state.battlers[0].passive = PASSIVE_INSIGHT;
   const uint16_t insight = grid_odds(kEven, kEven, kAsked).n;
   grid_table(out, "user Insight", kGridChances[0]);
   g_state.battlers[0].passive = kNone8;
   g_state.battlers[1].passive = PASSIVE_STUBBORN;
   const uint16_t stubborn = grid_odds(kEven, kEven, kAsked).n;
   grid_table(out, "target Stubborn", kGridChances[0]);
   g_state.battlers[1].passive = kNone8;
   g_state.battlers[0].item = ITEM_FOCUS_LENS;
   const uint16_t lens = grid_odds(kEven, kEven, kAsked).n;
   grid_table(out, "user Focus Lens", kGridChances[0]);
   g_state.battlers[0].item = 0;
   CHECK(insight == kAsked + kAsked / 2);
   CHECK(stubborn == kAsked / 2);
   CHECK(lens > kAsked);
   // C2: (2 x user + 60) / (user + target + 60) at most doubles the odds, however clever the user
   constexpr uint16_t kGenius = 600;
   constexpr uint16_t kDull = 1;
   CHECK(grid_odds(kGenius, kDull, kAsked).n < 2 * kAsked);
   golden("odds_grid.txt", out);
}

// -- the Starter's own numbers (C2) ---------------------------------------------------------------

/// The Starter's own rule numbers (README "Starter rules"), checked on the odds grid's battle (Duskowl against
/// Clodbun at level 20, no talents or charms).
void test_rules()
{
   if (!start_grid())
   {
      std::printf("FAIL the rules' battle did not start\n");
      ++g_failures;
      return;
   }
   // caps
   CHECK(kData.rules.level_max == 50);
   CHECK(kData.rules.stat_cap == 700);
   CHECK(kData.rules.self_hit_power == 30);

   // stat stages: +25% a stage up, -15% a stage down, -4..+4; accuracy 10% a stage
   constexpr int32_t kHundred = 100;
   CHECK(pipeline_stage_scale(STAGE_ATTACK, kHundred, 2) == 150);
   CHECK(pipeline_stage_scale(STAGE_ATTACK, kHundred, -2) == 70);
   CHECK(pipeline_stage_scale(STAGE_ATTACK, kHundred, 6) == 200);    // clamped to +4
   CHECK(pipeline_stage_scale(STAGE_ATTACK, kHundred, -6) == 40);    // clamped to -4
   CHECK(pipeline_accuracy_staged(kHundred, -1) == 90);
   CHECK(pipeline_accuracy_staged(kHundred, 4) == 140);

   // crit odds 1/20 .. 3/10; chart tiers x0.6 / x1.75
   const StageData& crit = kData.stages[STAGE_CRITICAL];
   CHECK(crit.odds[0].n == 1 && crit.odds[0].m == 20);
   CHECK(crit.odds[crit.hi - crit.lo].n == 3 && crit.odds[crit.hi - crit.lo].m == 10);
   CHECK(kData.tiers[TIER_WEAK].q8 == 154 && kData.tiers[TIER_STRONG].q8 == 448);

   // the damage roll spans 92..108 %, a crit multiplies by 1.625 and pierces the target's defence stages
   const int32_t low = damage_preview(g_state, 0, 1, MOVE_SHADOW_BITE, false, 0, false);
   const int32_t high = damage_preview(g_state, 0, 1, MOVE_SHADOW_BITE, false, 16, false);
   CHECK(low < high && high * 92 <= low * 108 + 108);
   const int32_t crit_hit = damage_preview(g_state, 0, 1, MOVE_SHADOW_BITE, false, 0, true);
   CHECK(crit_hit * 8 >= low * 13 - 13 && crit_hit * 8 <= low * 13 + 16);
   g_state.battlers[1].stages[STAGE_DEFENCE] = 4;   // the target hardened: a plain hit falls, a crit does not
   CHECK(damage_preview(g_state, 0, 1, MOVE_SHADOW_BITE, false, 0, false) < low);
   CHECK(damage_preview(g_state, 0, 1, MOVE_SHADOW_BITE, false, 0, true) == crit_hit);
   g_state.battlers[1].stages[STAGE_DEFENCE] = 0;

   // EP regenerates 1 + 1/16 of what is missing; MP does not regenerate
   const uint8_t regen = kData.resources[RESOURCE_EP].regen_formula;
   CHECK(rules::resource_formula(regen, 1, 44) == 1 + 43 / 16);
   CHECK(rules::resource_formula(regen, 44, 44) == 1);

   // the escape odds out of 256: 96 + 128 x own / (own + foe) + 48 a failed attempt
   CHECK(rules::escape_odds(100, 100, 0) == 96 + 64);
   CHECK(rules::escape_odds(100, 100, 2) == 96 + 64 + 96);
   // experience: yield x foe level / 10, scaled by (foe level + 10) / (own level + 10) and x5/4 for a rival
   CHECK(rules::exp_gain(60, 10, 10, 1, 0, 0) == 60);
   CHECK(rules::exp_gain(60, 10, 30, 1, 0, 0) == 30);
   CHECK(rules::exp_gain(60, 10, 10, 1, 1, 0) == 75);
   // reward: base x (level + 8) / 2
   CHECK(rules::reward(20, 7) == 150);
}

// -- resources --------------------------------------------------------------------------------------

/// Whether a transcript holds a line.
/// @param text  transcript
/// @param line  the line (without its newline)
/// @return true when it does
bool has_line(const std::string& text, const char* line)
{
   return text.find(std::string("\n") + line + "\n") != std::string::npos;
}

/// The Starter sample's resource rules, checked on the out_of_energy and unowned_single battles.
void test_resources()
{
   // the data: three battler resources, HP faints, MP and EP persist, only EP regenerates, Strike is free
   CHECK(kData.resource_count == NUM_RESOURCES);
   CHECK(kData.faint_resource == RESOURCE_HP);
   CHECK(kData.resources[RESOURCE_MP].role != monster::kNoRole);
   CHECK(kData.resources[RESOURCE_EP].role != monster::kNoRole);
   CHECK(kData.resources[RESOURCE_MP].regen_formula == kNone8 && kData.resources[RESOURCE_MP].regen == 0);
   CHECK(kData.resources[RESOURCE_EP].regen_formula != kNone8);
   CHECK(kData.rules.fallback_move == MOVE_STRIKE);
   CHECK(kData.moves[MOVE_STRIKE].cost_count == 0);
   CHECK(kData.odds_stat == STAT_INTELLIGENCE);
   CHECK(move_slots() == kMaxMoveSlots);

   std::string out;
   play(kScenarios[4], out);   // out_of_energy
   // no MP: Cyclone is refused; 1 EP: Gale Kick (4) too, until the regen phase adds EP; spent, it is refused again
   CHECK(out.find("refused move slot 2") != std::string::npos);
   CHECK(out.find("refused move slot 0") != std::string::npos);
   CHECK(out.find("used Strike!") != std::string::npos);
   CHECK(out.find("used Gale Kick!") != std::string::npos);
   CHECK(out.find("used Cyclone!") == std::string::npos);
   // MP persisted at 0 in the record (it does not regenerate); EP was spent and regenerated
   const monster::Record& rec = g_parties[0].members[0];
   CHECK(pool_of(rec, RESOURCE_MP) == 0);
   CHECK(has_line(out, "turn 2"));

   std::string single;
   play(kScenarios[0], single);   // unowned_single: Fireball costs 8 MP each time
   const monster::Record& ember = g_parties[0].members[0];
   uint16_t st[kMaxStats];
   record_stats(ember, st);
   CHECK(pool_of(ember, RESOURCE_MP) < st[STAT_MP]);   // spent MP persisted
}

} // namespace

/// Runs every golden and check.
/// @param argc  arguments
/// @param argv  [1] the golden folder, [2] optional --record
/// @return 0 when everything matched
int main(int argc, char** argv)
{
   if (argc < 2)
   {
      std::printf("usage: %s GOLDEN_DIR [--record]\n", argv[0]);
      return EXIT_FAILURE;
   }
   g_dir = argv[1];
   const char* env = std::getenv("CAPSULE_GOLDEN_RECORD");
   g_record = (argc > 2 && std::strcmp(argv[2], "--record") == 0) || (env != nullptr && std::strcmp(env, "1") == 0);
   for (const Scenario& sc : kScenarios)
   {
      run(sc);
   }
   test_odds();
   test_rules();
   test_resources();
   std::printf("%s %d files, %d checks, %d failures\n", g_record ? "recorded" : "compared", g_files, g_checks,
               g_failures);
   return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
