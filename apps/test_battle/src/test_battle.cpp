/// @file
/// Battle test: runs the battle engine (capsule/battle) on its own against
/// the Starter sample's battle data (samples/starter), one unit of work at a
/// time, and shows its state and the text of every event.
///
/// No presentation: the screen is a console. Human choices are made by a
/// fixed policy (the first usable move, its default target; replacements
/// take the first usable creature), so a scenario plays the same way for the
/// same seed; R changes the seed.
///
/// - Menu: Up/Down choose, A start.
/// - Running: A one step, START run / pause (one step per frame), R next
///   seed (restarts), B back to the menu.
#include "test_kit.h"

#include "battle_ids.h"

#include "capsule/battle/battle.h"
#include "capsule/core/memory.h"
#include "capsule/hal/system.h"
#include "capsule/monster/monster.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace
{

using namespace capsule;
using namespace capsule::testkit;
namespace bt = capsule::battle;
using namespace battle_ids;
using hal::input::KEY_A;
using hal::input::KEY_B;
using hal::input::KEY_DOWN;
using hal::input::KEY_R;
using hal::input::KEY_START;
using hal::input::KEY_UP;

constexpr int kLogTop = 9;       ///< First event-log row.
constexpr int kLogRows = 9;      ///< Event-log rows.

constexpr int kMineSlots = 4;    ///< Player creatures a scenario lists.
constexpr int kUnownedSlots = 3;    ///< Unowned creatures a scenario lists.
constexpr int kScenarioMoves = 4;   ///< Moves a scenario may give the player's first creature.

/// A scenario: who fights whom.
struct Scenario
{
   const char* name;      ///< Menu label.
   uint8_t format;        ///< Format.
   int16_t party;         ///< Opposing party index (data), or -1 (creatures without an owner: an unowned party).
   bool ai_player;        ///< The player's side is AI-controlled too.
   uint16_t mine[kMineSlots];      ///< Player species (kNone16 = none)...
   uint8_t mine_lv[kMineSlots];    ///< ...and levels.
   uint16_t unowned[kUnownedSlots];      ///< Unowned species...
   uint8_t unowned_lv[kUnownedSlots];    ///< ...and levels.
   uint16_t moves[kScenarioMoves];   ///< Moves of the player's first creature (kNone16 = learnset); other slots empty.
};

constexpr uint16_t N = bt::kNone16;   ///< Shorthand.

/// The scenarios (the Starter sample's creatures, moves and rivals).
const Scenario kScenarios[] = {
   {"Unowned single",
    FORMAT_SINGLE,
    -1,
    false,
    {SPECIES_EMBERKIT, N, N, N},
    {10, 0, 0, 0},
    {SPECIES_SPROUTLE, N, N},
    {6, 0, 0},
    {N, N, N, N}},
   {"Party single",
    FORMAT_SINGLE,
    PARTY_NOVICE_KIT,
    false,
    {SPECIES_TIDEPUP, SPECIES_CLODBUN, N, N},
    {12, 11, 0, 0},
    {N, N, N},
    {0, 0, 0},
    {N, N, N, N}},
   {"Double vs duo",
    FORMAT_DOUBLE,
    PARTY_DUO_ASH_BLY,
    false,
    {SPECIES_EMBERKIT, SPECIES_CLODBUN, SPECIES_VOLTMINK, N},
    {15, 14, 14, 0},
    {N, N, N},
    {0, 0, 0},
    {MOVE_FIREBALL, MOVE_FLARE_CLAW, N, N}},
   {"Double AI vs AI",
    FORMAT_DOUBLE,
    -1,
    true,
    {SPECIES_BREEZLE, SPECIES_DUSKOWL, SPECIES_GLIMMET, N},
    {12, 12, 12, 0},
    {SPECIES_CLODBUN, SPECIES_VOLTMINK, SPECIES_SPROUTLE},
    {12, 12, 12},
    {N, N, N, N}},
   {"Status moves",
    FORMAT_SINGLE,
    -1,
    false,
    {SPECIES_SPROUTLE, N, N, N},
    {20, 0, 0, 0},
    {SPECIES_GLIMMET, N, N},
    {20, 0, 0},
    {MOVE_TOXIC_POLLEN, MOVE_STARDUST_LULL, MOVE_VINE_LASH, N}},
   {"Elemental moves",
    FORMAT_SINGLE,
    -1,
    false,
    {SPECIES_CLODBUN, N, N, N},
    {20, 0, 0, 0},
    {SPECIES_BREEZLE, N, N},
    {18, 0, 0},
    {MOVE_QUAKE, MOVE_BOULDER_BASH, MOVE_HARDEN, N}},
};
/// Number of scenarios.
constexpr int kScenarioCount = sizeof(kScenarios) / sizeof(kScenarios[0]);

constexpr uint32_t kPartySeedMul =
   31u;   ///< Party seed = battle seed * this + kPartySeedAdd (differs from the battle's).
constexpr uint32_t kPartySeedAdd = 7u;    ///< See kPartySeedMul.
constexpr uint8_t kParticipants = 2;      ///< Participants of every scenario: the player and one opponent.
constexpr int kEventTextCodes = 96;       ///< Buffer for one event's text.

/// @name Battle view layout
///@{
constexpr int kInfoRow = 1;          ///< Row of turn, phase and seed.
constexpr int kCountsRow = 2;        ///< Row of steps, events and weather.
constexpr int kWeatherChars = 10;    ///< Letters of the weather name shown.
constexpr int kFieldTop = 3;         ///< Row of field position 0.
constexpr int kNameCol = 2;          ///< Column of a battler's name.
constexpr int kNameChars = 9;        ///< Letters of a battler's name shown.
constexpr int kHpCol = 12;           ///< Column of the current HP.
constexpr int kHpDigits = 3;         ///< Width of an HP number.
constexpr int kSlashCol = 15;        ///< Column of the "/" between the HP numbers.
constexpr int kMaxHpCol = 16;        ///< Column of the maximum HP.
constexpr int kStatusCol = 20;       ///< Column of the status.
constexpr int kStatusChars = 3;      ///< Letters of the status name shown.
constexpr int kStagesCol = 24;       ///< Column of the stat stages.
constexpr int kStagesEnd = 29;       ///< No stage starts at or after this column.
constexpr int kStageLabelLength = 3;   ///< A stage label: stat letter, sign and NUL.
///@}

/// @name Menu layout
///@{
constexpr int kMenuTop = 2;           ///< Row of the first scenario.
constexpr int kMenuNameCol = 3;       ///< Column of the scenario names, after the cursor.
constexpr int kDataRow = 10;          ///< Row of the data counts.
constexpr int kRecordRow = 11;        ///< Row of the record and state sizes.
constexpr int kStateUnitOffset = 6;   ///< Column of the state size's "B" after "state": a space and five digits.
///@}

bool g_menu = true;           ///< Showing the menu.
int g_cursor = 0;             ///< Menu row / scenario.
bool g_running = false;       ///< One step per frame.
uint32_t g_seed = 1;          ///< Battle seed.
// The battle and its parties are large and touched a few times per step: EWRAM (battle.h, "Where State lives").
CAPSULE_EWRAM_BSS bt::State g_battle;      ///< The battle.
CAPSULE_EWRAM_BSS monster::Party g_mine;   ///< Player party.
CAPSULE_EWRAM_BSS monster::Party g_unowned;   ///< Unowned party.
text::Code g_log[kLogRows][kCols + 1];   ///< Event log (oldest first).
uint32_t g_events = 0;        ///< Events seen.
uint32_t g_steps = 0;         ///< step() calls.
bt::Progress g_last = bt::Progress::Busy;   ///< Last step() result.

/// Copies codes, at most @p n.
/// @param out  buffer (n + 1)
/// @param in   codes
/// @param n    limit
void clip(text::Code* out, const text::Code* in, int n)
{
   int i = 0;
   for (; in != nullptr && in[i] != text::kCodeEnd && in[i] != text::kCodeNewline && i < n; ++i)
   {
      out[i] = in[i];
   }
   out[i] = text::kCodeEnd;
}

/// Adds a line to the event log.
/// @param codes  text
void log_line(const text::Code* codes)
{
   for (int r = 0; r + 1 < kLogRows; ++r)
   {
      for (int k = 0; k < kCols + 1; ++k)
      {
         g_log[r][k] = g_log[r + 1][k];
      }
   }
   clip(g_log[kLogRows - 1], codes, kCols);
}

/// Builds a party from species / levels.
/// @param p        party
/// @param species  species list (kNone16 ends it)
/// @param levels   levels
/// @param n        list length
void build(monster::Party& p, const uint16_t* species, const uint8_t* levels, int n)
{
   p = monster::Party{};
   rng::Stream r{g_seed * kPartySeedMul + kPartySeedAdd};
   for (int i = 0; i < n && species[i] != N; ++i)
   {
      if (bt::make_monster(p.members[p.count], species[i], levels[i], r))
      {
         ++p.count;
      }
   }
}

/// Starts the chosen scenario with the current seed.
void start_scenario()
{
   const Scenario& sc = kScenarios[g_cursor];
   build(g_mine, sc.mine, sc.mine_lv, kMineSlots);
   build(g_unowned, sc.unowned, sc.unowned_lv, kUnownedSlots);
   if (sc.moves[0] != N)
   {
      for (int m = 0; m < bt::move_slots(); ++m)
      {
         monster::set(g_mine.members[0], monster::Role::Moves, m, m < kScenarioMoves ? sc.moves[m] : N);
         bt::fill_move_slot(g_mine.members[0], m);   // full move pools (the move resources)
      }
   }
   bt::Setup su{};
   su.participants[0] =
      bt::Participant{nullptr, &g_mine, nullptr, sc.ai_player ? bt::Controller::Ai : bt::Controller::Human, 0, 0, 0};
   if (sc.party >= 0)
   {
      su.participants[1] = bt::Participant{&bt::kData.parties[sc.party], nullptr, nullptr, bt::Controller::Ai, 1, 0, 0};
   }
   else
   {
      su.participants[1] =
         bt::Participant{nullptr, &g_unowned, nullptr, bt::Controller::Ai, 1, 1u << bt::PARTY_UNOWNED, 0};
   }
   su.count = kParticipants;
   su.format = sc.format;
   su.weather = bt::kNone8;
   su.seed = g_seed;
   bt::start(g_battle, su);
   for (auto& row : g_log)
   {
      row[0] = text::kCodeEnd;
   }
   g_events = 0;
   g_steps = 0;
   g_last = bt::Progress::Busy;
   g_running = false;
}

/// Answers a pending request with the fixed policy.
void auto_choose()
{
   bt::Request r;
   if (!bt::request(g_battle, r))
   {
      return;
   }
   if (r.kind == bt::RequestKind::Replace || r.kind == bt::RequestKind::SwitchTarget)
   {
      // a replacement: the first one allowed (an effect's request takes it as its answer)
      const bt::ActionKind kind = r.kind == bt::RequestKind::Replace ? bt::ActionKind::Switch : bt::ActionKind::Answer;
      for (uint8_t i = 0; i < g_battle.battler_count; ++i)
      {
         const bt::Action a = kind == bt::ActionKind::Switch ? bt::Action{kind, 0, bt::kBench, i}
                                                             : bt::Action{kind, i, bt::kBench, 0};
         if (bt::choose(g_battle, r.battler, a))
         {
            return;
         }
      }
      return;
   }
   if (r.kind == bt::RequestKind::LearnMove || r.kind == bt::RequestKind::Choice)
   {
      bt::choose(g_battle, r.battler, bt::Action{bt::ActionKind::Answer, 0, bt::kBench, 0});
      return;
   }
   for (uint8_t k = 0; k < bt::move_slots(); ++k)
   {
      if (bt::choose(g_battle, r.battler, bt::Action{bt::ActionKind::Move, k, bt::kBench, 0}))
      {
         return;
      }
   }
   bt::choose(g_battle, r.battler, bt::Action{bt::ActionKind::Move, bt::kFallbackSlot, bt::kBench, 0});
}

/// Moves queued events into the log.
void drain()
{
   bt::Event e;
   while (bt::pop_event(g_battle, e))
   {
      ++g_events;
      text::Code buf[kEventTextCodes];
      if (bt::format_text(g_battle, e, buf, kEventTextCodes))
      {
         log_line(buf);
      }
   }
}

/// One unit of work (answering input first if needed).
void one_step()
{
   if (g_last == bt::Progress::Ended)
   {
      return;
   }
   if (g_last == bt::Progress::Input)
   {
      auto_choose();
   }
   g_last = bt::step(g_battle);
   ++g_steps;
   drain();
}

/// Short name of a phase.
/// @param p  phase
/// @return text
const char* phase_name(bt::Phase p)
{
   constexpr const char* kNames[] = {"start", "choose", "execute", "end turn", "replace", "ended"};
   return kNames[static_cast<int>(p)];
}

/// Draws the running view.
void draw_battle()
{
   clear_console();
   put_ascii(INK_TITLE, 0, 0, kScenarios[g_cursor].name);
   int x = put_ascii(INK_DIM, 0, kInfoRow, "turn");
   x = put_uint(INK_ON, x + 1, kInfoRow, g_battle.turn);
   x = put_ascii(INK_DIM, x + 1, kInfoRow, "phase");
   x = put_ascii(INK_ON, x + 1, kInfoRow, phase_name(g_battle.phase));
   x = put_ascii(INK_DIM, x + 1, kInfoRow, "seed");
   put_uint(INK_ON, x + 1, kInfoRow, g_seed);

   x = put_ascii(INK_DIM, 0, kCountsRow, "steps");
   x = put_uint(INK_ON, x + 1, kCountsRow, g_steps);
   x = put_ascii(INK_DIM, x + 1, kCountsRow, "events");
   x = put_uint(INK_ON, x + 1, kCountsRow, g_events);
   if (g_battle.weather != bt::kNone8)
   {
      text::Code w[kWeatherChars + 1];
      clip(w, bt::kData.weathers[g_battle.weather].name, kWeatherChars);
      put(INK_ON, x + 1, kCountsRow, w);
   }

   const bt::FormatData& f = bt::format(g_battle);
   for (uint8_t p = 0; p < f.positions; ++p)
   {
      const int y = kFieldTop + p;
      x = put_uint(INK_DIM, 0, y, p);
      const uint8_t i = g_battle.at[p];
      if (i == bt::kBench)
      {
         put_ascii(INK_OFF, kNameCol, y, "-");
         continue;
      }
      const bt::Battler& b = g_battle.battlers[i];
      text::Code name[kNameChars + 1];
      clip(name, b.name, kNameChars);
      put(b.hp > 0 ? INK_ON : INK_OFF, kNameCol, y, name);
      put_uint(INK_ON, kHpCol, y, b.hp, kHpDigits);
      put_ascii(INK_DIM, kSlashCol, y, "/");
      put_uint(INK_DIM, kMaxHpCol, y, bt::max_hp(b), kHpDigits);
      if (b.status != 0)
      {
         text::Code st[kStatusChars + 1];
         clip(st, bt::kData.statuses[b.status].name, kStatusChars);
         put(INK_TITLE, kStatusCol, y, st);
      }
      // Non-zero stages as e.g. "a+1 d-2": accuracy, evasion, critical, then the test data's staged stats.
      int sx = kStagesCol;
      constexpr char kStage[] = "xecadsfp";
      constexpr int kStageLetters = static_cast<int>(sizeof(kStage)) - 1;
      for (int k = 0; k < bt::kData.stage_count && k < kStageLetters && sx < kStagesEnd; ++k)
      {
         if (b.stages[k] == 0)
         {
            continue;
         }
         const char txt[kStageLabelLength] = {kStage[k], b.stages[k] > 0 ? '+' : '-', '\0'};
         sx = put_ascii(INK_DIM, sx, y, txt);
         sx = put_uint(INK_DIM, sx, y, static_cast<uint32_t>(b.stages[k] > 0 ? b.stages[k] : -b.stages[k]));
      }
   }
   for (int r = 0; r < kLogRows; ++r)
   {
      put(r == kLogRows - 1 ? INK_TITLE : INK_DIM, 0, kLogTop + r, g_log[r]);
   }
   if (g_last == bt::Progress::Ended)
   {
      constexpr const char* kOutcome[] = {"-", "victory", "defeat", "stalemate", "escaped"};
      x = put_ascii(INK_DIM, 0, kControlsTop, "outcome");
      put_ascii(INK_TITLE, x + 1, kControlsTop, kOutcome[static_cast<int>(g_battle.outcome)]);
   }
   else if (g_last == bt::Progress::Input)
   {
      put_ascii(INK_DIM, 0, kControlsTop, "waiting for a choice (A: auto)");
   }
   put_ascii(INK_OFF, 0, kControlsBottom,
             g_running ? "A step START pause R seed B menu" : "A step START run R seed B menu");
}

/// Draws the menu.
void draw_menu()
{
   clear_console();
   put_ascii(INK_TITLE, 0, 0, "BATTLE TEST");
   for (int i = 0; i < kScenarioCount; ++i)
   {
      put_ascii(i == g_cursor ? INK_ON : INK_DIM, 1, kMenuTop + i, i == g_cursor ? ">" : " ");
      put_ascii(i == g_cursor ? INK_TITLE : INK_DIM, kMenuNameCol, kMenuTop + i, kScenarios[i].name);
   }

   int x = put_ascii(INK_DIM, 1, kDataRow, "types");
   x = put_uint(INK_ON, x + 1, kDataRow, bt::kData.type_count);
   x = put_ascii(INK_DIM, x + 1, kDataRow, "moves");
   x = put_uint(INK_ON, x + 1, kDataRow, bt::kData.move_count);
   x = put_ascii(INK_DIM, x + 1, kDataRow, "species");
   put_uint(INK_ON, x + 1, kDataRow, bt::kData.species_count);

   x = put_ascii(INK_DIM, 1, kRecordRow, "record");
   x = put_uint(INK_ON, x + 1, kRecordRow, monster::kLayout.record_bytes);
   x = put_ascii(INK_DIM, x, kRecordRow, "B, state");
   put_uint(INK_ON, x + 1, kRecordRow, static_cast<uint32_t>(sizeof(bt::State)));
   put_ascii(INK_DIM, x + kStateUnitOffset, kRecordRow, "B");
   put_ascii(INK_OFF, 0, kControlsBottom, "Up/Down choose  A start");
}

} // namespace

/// Runs the test app.
/// @return 0
int main()
{
   hal::system::init();
   init_console();
   draw_menu();
   Pad pad;
   while (hal::system::update())
   {
      pad.update();
      if (g_menu)
      {
         if (pad.hit(KEY_UP) || pad.repeat(KEY_UP))
         {
            g_cursor = (g_cursor + kScenarioCount - 1) % kScenarioCount;
            draw_menu();
         }
         else if (pad.hit(KEY_DOWN) || pad.repeat(KEY_DOWN))
         {
            g_cursor = (g_cursor + 1) % kScenarioCount;
            draw_menu();
         }
         else if (pad.hit(KEY_A))
         {
            g_menu = false;
            start_scenario();
            draw_battle();
         }
         continue;
      }
      bool redraw = false;
      if (pad.hit(KEY_B))
      {
         g_menu = true;
         draw_menu();
         continue;
      }
      if (pad.hit(KEY_R))
      {
         ++g_seed;
         start_scenario();
         redraw = true;
      }
      if (pad.hit(KEY_START))
      {
         g_running = !g_running;
         redraw = true;
      }
      if (pad.hit(KEY_A) || g_running)
      {
         one_step();
         redraw = true;
         if (g_last == bt::Progress::Ended)
         {
            g_running = false;
         }
      }
      if (redraw)
      {
         draw_battle();
      }
   }
   return 0;
}
