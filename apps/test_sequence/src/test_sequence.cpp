/// @file
/// Sequence test: runs the sequence player (capsule/seq) on its own against the
/// test manifests in tests/data/sequences, and shows what it is doing.
///
/// The engine systems a sequence drives (asset loading, sprites, effects, text
/// box, map view, audio) don't exist yet, so this app links its own recording
/// port instead of src/seq/sequence_port_hal.cpp: every port call is listed on
/// screen, and the waits a sequence depends on are simulated (a song lasts
/// kSongFrames, typed text keeps the printer busy for kTextFrames). It also
/// supplies the game hooks, so conditions and the list length can be set in the
/// menu. Because this file defines every port:: and game:: function, the
/// linker never pulls the engine's versions out of the capsule_engine archive.
///
/// - Menu: Up/Down choose, A run / toggle, Left/Right list length.
/// - Running: the sequence gets the pad as-is. L+R together pause.
/// - Paused: A one frame (other held keys go to the sequence), START resume,
///   B back to the menu.
/// - Exited: A run again, B back to the menu.
#include "test_kit.h"

#include "capsule/core/memory.h"
#include "capsule/hal/system.h"
#include "capsule/seq/sequence.h"
#include "capsule/seq/sequence_port.h"
#include "capsule/text/encode.h"

#include "seq_credits_map.inc"
#include "seq_credits_showcase.inc"
#include "seq_cutscene.inc"
#include "seq_title_song_end.inc"
#include "seq_title_timeout.inc"

#include <cstdint>

namespace
{

using namespace capsule;
using namespace capsule::testkit;
using hal::input::KEY_A;
using hal::input::KEY_B;
using hal::input::KEY_DOWN;
using hal::input::KEY_L;
using hal::input::KEY_LEFT;
using hal::input::KEY_R;
using hal::input::KEY_RIGHT;
using hal::input::KEY_START;
using hal::input::KEY_UP;

constexpr uint32_t kSongFrames = 600;   ///< Simulated song length.
constexpr uint32_t kTextFrames = 30;    ///< Simulated typing time per typed print.
constexpr int kMaxConds = 4;            ///< Conditions shown in the menu.
constexpr uint16_t kMaxListItems = 99;  ///< Largest list length the menu allows.
constexpr uint16_t kDefaultListItems = 8;   ///< List length at start-up.

/// A test manifest and its debug names.
struct Fixture
{
   const char* name;            ///< Manifest name.
   const seq::Sequence* seq;    ///< Compiled tables.
   const char* const* stages;   ///< Stage names by index.
   const char* const* exits;    ///< Exit names by code; ends with nullptr.
   const char* const* conds;    ///< Condition names, id 1 first; ends with nullptr.
   const char* const* lists;    ///< List names, id 1 first; ends with nullptr.
};

/// Fixture entry from a generated sequence symbol.
/// @param label  display name
/// @param sym    generated symbol (kSeq...)
#define CAPSULE_FIXTURE(label, sym)                                                                                    \
   {                                                                                                                   \
      label, &sym, sym##StageNames, sym##ExitNames, sym##CondNames, sym##ListNames                                     \
   }
/// The test manifests.
const Fixture kFixtures[] = {
   CAPSULE_FIXTURE("title_timeout", kSeqTitleTimeout),
   CAPSULE_FIXTURE("title_song_end", kSeqTitleSongEnd),
   CAPSULE_FIXTURE("cutscene", kSeqCutscene),
   CAPSULE_FIXTURE("credits_map", kSeqCreditsMap),
   CAPSULE_FIXTURE("credits_showcase", kSeqCreditsShowcase),
};
#undef CAPSULE_FIXTURE
constexpr int kNumFixtures = sizeof(kFixtures) / sizeof(kFixtures[0]);   ///< Fixture count.

/// Length of a nullptr-terminated name table.
/// @param names  the table
/// @return entries before nullptr
int count(const char* const* names)
{
   int n = 0;
   while (names[n] != nullptr)
   {
      ++n;
   }
   return n;
}

// --- simulated world --------------------------------------------------------

uint32_t g_clock = 0;          ///< Frames since the sequence started.
bool g_cond[kMaxConds + 1];    ///< Condition values, 1-based.
uint16_t g_list_items = kDefaultListItems;   ///< Length of every game list.

bool g_song_on = false;        ///< A song was started and not stopped.
uint32_t g_song_start = 0;     ///< g_clock when it started.
uint32_t g_song_end = 0;       ///< g_clock when it ends (length or fade).
uint32_t g_text_until = 0;     ///< g_clock until which typed text keeps the printer busy.

// --- port call log ------------------------------------------------------------

constexpr int kLogRows = 8;   ///< Rows of the on-screen call log.

constexpr int kMaxArgs = 3;               ///< Arguments a log row keeps.
constexpr uint8_t kTwoArgs = 2;           ///< Log a call with two arguments.
constexpr uint8_t kThreeArgs = 3;         ///< Log a call with three arguments.
constexpr int kMaxRepeats = 9999;         ///< Repeat count of a row saturates here (fits four columns).
constexpr uint32_t kMaxCalls = 0xFFFFFFFFu;   ///< The call counter saturates here.

/// @name Byte positions of four bytes packed into one log argument (shown in hex)
///@{
constexpr int kByte1Shift = 8;    ///< Second byte.
constexpr int kByte2Shift = 16;   ///< Third byte.
constexpr int kByte3Shift = 24;   ///< Fourth byte.
///@}
constexpr int kListKeyShift = 16;   ///< A fake list asset key: the list id from bit 16 up, the index below.

/// One row of the call log.
struct LogEntry
{
   const char* name;   ///< Port call name (string literal; compared by address).
   int32_t args[kMaxArgs];   ///< Latest arguments.
   uint8_t argc;       ///< Arguments shown.
   uint8_t hex_last;   ///< Last argument is an asset key (shown in hex).
   uint16_t repeats;   ///< Calls of this name folded into this row.
};
LogEntry g_log[kLogRows];   ///< Ring buffer of rows.
int g_log_head = 0;         ///< Next slot to write.
int g_log_size = 0;         ///< Rows in use.
uint32_t g_calls = 0;       ///< Port calls since the run started (saturating).

/// Packs four bytes into one logged argument, the first in bits 0-7. The shifts are done unsigned (a byte shifted
/// into bit 31 of an int would overflow); the log shows the value in hex, so the sign of the result does not matter.
/// @param b0  bits 0-7
/// @param b1  bits 8-15
/// @param b2  bits 16-23
/// @param b3  bits 24-31
/// @return the packed value
int32_t pack_bytes(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
   const uint32_t packed = static_cast<uint32_t>(b0) | static_cast<uint32_t>(b1) << kByte1Shift |
                           static_cast<uint32_t>(b2) << kByte2Shift | static_cast<uint32_t>(b3) << kByte3Shift;
   return static_cast<int32_t>(packed);
}

/// Records a port call; a call already on screen is folded into its row.
/// @param name      call name (string literal)
/// @param argc      arguments to show, 0-3
/// @param a0        first argument
/// @param a1        second argument
/// @param a2        third argument
/// @param hex_last  show the last argument as an asset key
void log_call(const char* name, uint8_t argc, int32_t a0 = 0, int32_t a1 = 0, int32_t a2 = 0, bool hex_last = false)
{
   g_calls = g_calls < kMaxCalls ? g_calls + 1 : g_calls;
   // A call already on screen (per-frame motion, loops) is folded into its row,
   // which moves to the bottom with the latest arguments.
   LogEntry entry{name, {a0, a1, a2}, argc, static_cast<uint8_t>(hex_last), 1};
   for (int i = 0; i < g_log_size; ++i)
   {
      const int at = (g_log_head + kLogRows - g_log_size + i) % kLogRows;
      if (g_log[at].name != name)
      {
         continue;
      }
      entry.repeats = g_log[at].repeats < kMaxRepeats ? static_cast<uint16_t>(g_log[at].repeats + 1) : kMaxRepeats;
      for (int j = i; j + 1 < g_log_size; ++j)
      {   // close the gap
         g_log[(g_log_head + kLogRows - g_log_size + j) % kLogRows] =
            g_log[(g_log_head + kLogRows - g_log_size + j + 1) % kLogRows];
      }
      g_log[(g_log_head + kLogRows - 1) % kLogRows] = entry;
      return;
   }
   g_log[g_log_head] = entry;
   g_log_head = (g_log_head + 1) % kLogRows;
   if (g_log_size < kLogRows)
   {
      ++g_log_size;
   }
}

/// Empties the call log.
void clear_log()
{
   g_log_head = 0;
   g_log_size = 0;
   g_calls = 0;
}

/// An asset key as a log argument.
/// @param k  the key
/// @return its bits
int32_t key(AssetKey k)
{
   return static_cast<int32_t>(raw(k));
}

} // namespace

// --- recording port (sequence_port.h; documented there) -------------------------

namespace capsule::seq::port
{

void set_video_mode(uint8_t mode)
{
   log_call("video_mode", 1, mode);
}
void bg_load(uint8_t bg, AssetKey a)
{
   log_call("bg_load", kTwoArgs, bg, key(a), 0, true);
}
void bg_show(uint8_t bg, bool on)
{
   log_call("bg_show", kTwoArgs, bg, on);
}
void bg_offset(uint8_t bg, int16_t x, int16_t y)
{
   log_call("bg_offset", kThreeArgs, bg, x, y);
}
void blend(uint8_t mode, uint8_t src, uint8_t tgt)
{
   log_call("blend", kThreeArgs, mode, src, tgt);
}
void blend_weights(uint8_t src, uint8_t tgt)
{
   log_call("blend_weights", kTwoArgs, src, tgt);
}
void brightness(uint8_t level, bool white)
{
   log_call("brightness", kTwoArgs, level, white);
}
void palette_fade(uint32_t mask, uint16_t color, uint8_t coef)
{
   log_call("palette_fade", kThreeArgs, static_cast<int32_t>(mask), color, coef);
}
void window(uint8_t win, uint16_t flags, uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
   log_call("window", kThreeArgs, win, flags, pack_bytes(x0, y0, x1, y1), true);
}

void sprite_load(uint8_t slot, AssetKey a)
{
   log_call("spr_load", kTwoArgs, slot, key(a), 0, true);
}
void sprite_show(uint8_t slot, bool on)
{
   log_call("spr_show", kTwoArgs, slot, on);
}
void sprite_pos(uint8_t slot, int16_t x, int16_t y)
{
   log_call("spr_pos", kThreeArgs, slot, x, y);
}
void sprite_anim(uint8_t slot, uint16_t anim)
{
   log_call("spr_anim", kTwoArgs, slot, anim);
}
void sprite_obj_window(uint8_t slot, bool on)
{
   log_call("spr_objwin", kTwoArgs, slot, on);
}
void sprite_affine(uint8_t slot, uint16_t scale, uint16_t angle)
{
   log_call("spr_affine", kThreeArgs, slot, scale, angle);
}
void sprite_blend(uint8_t slot, bool on)
{
   log_call("spr_blend", kTwoArgs, slot, on);
}
void sprites_clear()
{
   log_call("sprites_clear", 0);
}

void emitter(uint8_t slot, bool on, AssetKey a)
{
   log_call("emitter", kThreeArgs, slot, on, key(a), true);
}
void palette_anim(uint8_t slot, bool on, AssetKey a)
{
   log_call("pal_anim", kThreeArgs, slot, on, key(a), true);
}
void scanline(uint8_t bg, bool on, AssetKey a)
{
   log_call("scanline", kThreeArgs, bg, on, key(a), true);
}
void effects_clear()
{
   log_call("effects_clear", 0);
}

void text_box(bool open, uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
   log_call("text_box", kTwoArgs, open, pack_bytes(x, y, w, h), 0, true);
}
void text_print(uint8_t target, uint8_t x, uint8_t, uint8_t, bool typed, uint8_t, AssetKey a)
{
   if (typed)
   {
      g_text_until = g_clock + kTextFrames;
   }
   log_call("text", kThreeArgs, target, x, key(a), true);
}
void text_clear(uint8_t target, uint8_t first, uint8_t rows)
{
   log_call("text_clear", kThreeArgs, target, first, rows);
}
bool text_busy()
{
   return g_clock < g_text_until;
}
void text_clear_all()
{
   g_text_until = 0;
   log_call("text_clear_all", 0);
}

void map_show(AssetKey a)
{
   log_call("map_show", 1, key(a), 0, 0, true);
}
void map_camera(int16_t x, int16_t y)
{
   log_call("map_camera", kTwoArgs, x, y);
}

void song_play(AssetKey song, uint16_t fade_in)
{
   g_song_on = raw(song) != 0;
   g_song_start = g_clock;
   g_song_end = g_clock + kSongFrames;
   log_call("song", kTwoArgs, fade_in, key(song), 0, true);
}
void song_fade(uint16_t frames)
{
   if (g_song_on && g_clock + frames < g_song_end)
   {
      g_song_end = g_clock + frames;
   }
   log_call("song_fade", 1, frames);
}
bool song_playing()
{
   return g_song_on && g_clock < g_song_end;
}
uint32_t song_frame()
{
   return song_playing() ? g_clock - g_song_start : 0;
}
void sfx_play(AssetKey a)
{
   log_call("sfx", 1, key(a), 0, 0, true);
}

} // namespace capsule::seq::port

// --- game hooks (sequence_port.h; documented there) -------------------------------

namespace capsule::seq::game
{

bool cond(uint8_t id)
{
   return id >= 1 && id <= kMaxConds && g_cond[id];
}
uint16_t list_count(uint8_t)
{
   return g_list_items;
}
AssetKey list_asset(uint8_t list, uint16_t index)
{
   return AssetKey{static_cast<uint32_t>(list) << kListKeyShift | index};   // recognisable: LLLL IIII
}
void rng_mix_entropy(uint32_t frame)
{
   log_call("rng_entropy", 1, static_cast<int32_t>(frame));
}

} // namespace capsule::seq::game

namespace
{

// --- screens ----------------------------------------------------------------------

/// @name Screen text
///@{
constexpr auto kApp = text::encode("SEQUENCE TEST");   ///< Screen text "SEQUENCE TEST".
constexpr auto kMenuHint1 =
   text::encode("Up/Down choose  A run/toggle");   ///< Screen text "Up/Down choose  A run/toggle".
constexpr auto kMenuHint2 = text::encode("Left/Right list length");   ///< Screen text "Left/Right list length".
constexpr auto kRunHint = text::encode("L+R pause");   ///< Screen text "L+R pause".
constexpr auto kPauseHint =
   text::encode("A step  START resume  B menu");   ///< Screen text "A step  START resume  B menu".
constexpr auto kExitHint = text::encode("A run again  B menu");   ///< Screen text "A run again  B menu".
///@}

/// @name Menu layout
///@{
constexpr int kMenuHeaderRow = 2;   ///< Row of the "Sequence" header.
constexpr int kFixturesTop = 3;     ///< Row of the first fixture.
constexpr int kItemCol = 2;         ///< Column of a menu item, after the cursor.
constexpr int kCondNameCol = 6;     ///< Column of a condition's name, after its "[x]" box.
///@}

/// @name Run screen layout
///@{
constexpr int kStageRow = 1;          ///< Row of the stage.
constexpr int kFrameRow = 2;          ///< Row of the frame counters.
constexpr int kLevelsRow = 3;         ///< Row of the fade, blend and palette levels.
constexpr int kTracksTop = 4;         ///< Row of track 0.
constexpr int kTracksShown = 4;       ///< Tracks shown at most.
constexpr int kPcDigits = 3;          ///< Width of a track's pc.
constexpr int kSystemsRow = 8;        ///< Row of the simulated song and text.
constexpr int kTextStateCol = 12;     ///< Column of the text printer state.
constexpr int kCallsRow = 9;          ///< Row of the "Port calls" header.
constexpr int kLogTop = 10;           ///< Row of the oldest call-log row.
constexpr int kKeyDigits = 8;         ///< Hex digits of an asset key.
constexpr KeyBits kAllKeys = 0xFFFF;  ///< Every key bit: pass all keys to the sequence.
///@}

/// What the app is doing.
enum class Mode : uint8_t
{
   Menu,      ///< Choosing a fixture and its hooks.
   Running,   ///< Playing.
   Paused,    ///< Stopped; A steps one frame.
   Exited     ///< The sequence took an exit.
};

Mode g_mode = Mode::Menu;   ///< Current mode.
int g_fixture = 0;          ///< Selected fixture.
int g_cursor = 0;           ///< Menu row: fixtures, then conditions, then list length.
CAPSULE_EWRAM_BSS seq::Player g_player;   ///< The player (about 1 KB: EWRAM).

/// Number of menu rows for the selected fixture.
/// @return fixtures + its conditions (up to kMaxConds) + 1 if it has lists
int menu_rows()
{
   const Fixture& f = kFixtures[g_fixture];
   int conds = count(f.conds);
   if (conds > kMaxConds)
   {
      conds = kMaxConds;
   }
   return kNumFixtures + conds + (count(f.lists) > 0 ? 1 : 0);
}

/// Draws the menu.
void draw_menu()
{
   clear_console();
   title(kApp.codes, 0, 0, nullptr);
   put_ascii(INK_TITLE, 0, kMenuHeaderRow, "Sequence");
   for (int i = 0; i < kNumFixtures; ++i)
   {
      const bool at = g_cursor == i;
      put_ascii(at ? INK_ON : INK_DIM, 0, kFixturesTop + i, at ? ">" : " ");
      put_ascii(i == g_fixture ? INK_ON : INK_DIM, kItemCol, kFixturesTop + i, kFixtures[i].name);
   }
   const Fixture& f = kFixtures[g_fixture];
   int row = kNumFixtures;
   int y = kFixturesTop + kNumFixtures + 1;   // one blank row after the fixtures
   const int conds = count(f.conds);
   if (conds > 0)
   {
      put_ascii(INK_TITLE, 0, y++, "Conditions");
   }
   for (int c = 0; c < conds && c < kMaxConds; ++c, ++row, ++y)
   {
      const bool at = g_cursor == row;
      put_ascii(at ? INK_ON : INK_DIM, 0, y, at ? ">" : " ");
      put_ascii(g_cond[c + 1] ? INK_ON : INK_OFF, kItemCol, y, g_cond[c + 1] ? "[x]" : "[ ]");
      put_ascii(INK_DIM, kCondNameCol, y, f.conds[c]);
   }
   if (count(f.lists) > 0)
   {
      const bool at = g_cursor == row;
      put_ascii(at ? INK_ON : INK_DIM, 0, y, at ? ">" : " ");
      const int x = put_ascii(INK_DIM, kItemCol, y, "List items:");
      put_uint(INK_ON, x + 1, y, g_list_items);
   }
   controls(kMenuHint1.codes, kMenuHint2.codes);
}

/// Starts the selected fixture from the beginning.
void start_run()
{
   g_clock = 0;
   g_song_on = false;
   g_text_until = 0;
   clear_log();
   seq::start(g_player, *kFixtures[g_fixture].seq);
   g_mode = Mode::Running;
   clear_console();
}

/// WaitKind names.
const char* const kWaitNames[] = {"fade",       "sprite", "text",     "input",  "song end",
                                  "song frame", "blend",  "pal fade", "window", "cond"};
static_assert(sizeof(kWaitNames) / sizeof(kWaitNames[0]) == static_cast<int>(seq::WaitKind::NUM_WAIT_KINDS));

/// Draws the run screen: stage, clocks, levels, tracks, simulated systems,
/// call log and status.
void draw_run()
{
   const Fixture& f = kFixtures[g_fixture];
   const seq::Player& p = g_player;
   clear_rows(0, kRows - 1);
   int x = put(INK_TITLE, 0, 0, kApp.codes);
   put_ascii(INK_ON, x + 1, 0, f.name);

   // Stage and clocks.
   x = put_ascii(INK_DIM, 0, kStageRow, "stage");
   x = put_uint(INK_ON, x + 1, kStageRow, p.stage + 1u);   // 1-based: "stage 2/7"
   x = put_ascii(INK_DIM, x, kStageRow, "/");
   x = put_uint(INK_DIM, x, kStageRow, f.seq->stage_count);
   put_ascii(INK_ON, x + 1, kStageRow, f.stages[p.stage]);
   x = put_ascii(INK_DIM, 0, kFrameRow, "frame");
   x = put_uint(INK_ON, x + 1, kFrameRow, p.stage_frames);
   x = put_ascii(INK_DIM, x + 1, kFrameRow, "total");
   x = put_uint(INK_ON, x + 1, kFrameRow, p.total_frames);

   // Effects the player is ramping.
   x = put_ascii(INK_DIM, 0, kLevelsRow, "fade");
   x = put_uint(INK_ON, x + 1, kLevelsRow, static_cast<uint32_t>(p.fade.value >> kQ8Shift));
   x = put_ascii(INK_DIM, x, kLevelsRow, p.fade_white ? "W" : "");
   x = put_ascii(INK_DIM, x + 1, kLevelsRow, "blend");
   x = put_uint(INK_ON, x + 1, kLevelsRow, static_cast<uint32_t>(p.blend_src.value >> kQ8Shift));
   x = put_ascii(INK_DIM, x, kLevelsRow, "/");
   x = put_uint(INK_ON, x, kLevelsRow, static_cast<uint32_t>(p.blend_tgt.value >> kQ8Shift));
   x = put_ascii(INK_DIM, x + 1, kLevelsRow, "pal");
   put_uint(INK_ON, x + 1, kLevelsRow, static_cast<uint32_t>(p.pal_coef.value >> kQ8Shift));

   // Tracks of the current stage.
   const seq::Stage& st = f.seq->stages[p.stage];
   for (int t = 0; t < st.track_count && t < kTracksShown; ++t)
   {
      const seq::Player::Track& tr = p.tracks[t];
      const int y = kTracksTop + t;
      x = put_ascii(INK_DIM, 0, y, "T");
      x = put_uint(INK_DIM, x, y, static_cast<uint32_t>(t));
      if (tr.done)
      {
         put_ascii(INK_OFF, x + 1, y, "done");
         continue;
      }
      x = put_ascii(INK_DIM, x + 1, y, "pc");
      x = put_uint(INK_ON, x + 1, y, static_cast<uint32_t>(tr.pc - tr.base), kPcDigits);
      if (tr.wait_kind != 0)
      {
         x = put_ascii(INK_DIM, x + 1, y, "for");
         put_ascii(INK_ON, x + 1, y, kWaitNames[tr.wait_kind - 1]);
      }
      else if (tr.wait != 0)
      {
         x = put_ascii(INK_DIM, x + 1, y, "wait");
         put_uint(INK_ON, x + 1, y, tr.wait);
      }
   }

   // Simulated systems.
   x = put_ascii(INK_DIM, 0, kSystemsRow, "song");
   if (seq::port::song_playing())
   {
      put_uint(INK_ON, x + 1, kSystemsRow, seq::port::song_frame());
   }
   else
   {
      put_ascii(INK_OFF, x + 1, kSystemsRow, "-");
   }
   x = put_ascii(INK_DIM, kTextStateCol, kSystemsRow, "text");
   put_ascii(seq::port::text_busy() ? INK_ON : INK_OFF, x + 1, kSystemsRow, seq::port::text_busy() ? "busy" : "idle");

   // Port calls, newest at the bottom.
   x = put_ascii(INK_TITLE, 0, kCallsRow, "Port calls");
   put_uint(INK_DIM, x + 1, kCallsRow, g_calls);
   for (int i = 0; i < g_log_size; ++i)
   {
      const LogEntry& e = g_log[(g_log_head + kLogRows - g_log_size + i) % kLogRows];
      const int y = kLogTop + i;
      x = put_ascii(INK_DIM, 0, y, e.name);
      for (int a = 0; a < e.argc; ++a)
      {
         const bool hex = e.hex_last && a == e.argc - 1;
         x = hex ? put_hex(INK_ON, x + 1, y, static_cast<uint32_t>(e.args[a]), kKeyDigits)
                 : put_int(INK_ON, x + 1, y, e.args[a]);
      }
      if (e.repeats > 1)
      {
         x = put_ascii(INK_DIM, x + 1, y, "x");
         put_uint(INK_DIM, x, y, e.repeats);
      }
   }

   // Status.
   switch (g_mode)
   {
   case Mode::Running:
      put_ascii(INK_ON, 0, kControlsTop, "RUNNING");
      put(INK_OFF, 0, kControlsBottom, kRunHint.codes);
      break;
   case Mode::Paused:
      put_ascii(INK_ON, 0, kControlsTop, "PAUSED");
      put(INK_OFF, 0, kControlsBottom, kPauseHint.codes);
      break;
   default:
   {
      x = put_ascii(INK_ON, 0, kControlsTop, "EXIT");
      x = put_uint(INK_ON, x + 1, kControlsTop, p.exit_code);
      const int exits = count(f.exits);
      put_ascii(INK_ON, x + 1, kControlsTop, p.exit_code < exits ? f.exits[p.exit_code] : "?");
      put(INK_OFF, 0, kControlsBottom, kExitHint.codes);
      break;
   }
   }
}

/// Advances the sequence one frame.
/// @param pad   this frame's buttons
/// @param keep  keys passed to the sequence (paused single steps drop A, the step key)
void step(const Pad& pad, KeyBits keep = kAllKeys)
{
   ++g_clock;
   const auto held = static_cast<KeyBits>(pad.held & keep);
   const auto pressed = static_cast<KeyBits>(pad.pressed & keep);
   if (seq::step(g_player, held, pressed) == seq::Status::Exited)
   {
      g_mode = Mode::Exited;
   }
}

/// Menu controls.
/// @param pad  this frame's buttons
void update_menu(const Pad& pad)
{
   const int rows = menu_rows();
   bool redraw = false;
   if (pad.repeat(KEY_UP))
   {
      g_cursor = (g_cursor + rows - 1) % rows;
      redraw = true;
   }
   if (pad.repeat(KEY_DOWN))
   {
      g_cursor = (g_cursor + 1) % rows;
      redraw = true;
   }
   if (g_cursor < kNumFixtures)
   {
      if (g_cursor != g_fixture)
      {   // choosing a fixture shows its conditions
         g_fixture = g_cursor;
         redraw = true;
      }
      if (pad.hit(KEY_A) || pad.hit(KEY_START))
      {
         start_run();
         return;
      }
   }
   else if (g_cursor < rows - 1 || count(kFixtures[g_fixture].lists) == 0)
   {
      const int c = g_cursor - kNumFixtures + 1;
      if (pad.hit(KEY_A))
      {
         g_cond[c] = !g_cond[c];
         redraw = true;
      }
   }
   else
   {
      if (pad.repeat(KEY_LEFT) && g_list_items > 0)
      {
         --g_list_items;
         redraw = true;
      }
      if (pad.repeat(KEY_RIGHT) && g_list_items < kMaxListItems)
      {
         ++g_list_items;
         redraw = true;
      }
   }
   if (redraw)
   {
      draw_menu();
   }
}

/// Whether L+R are held with one of them newly pressed.
/// @param pad  this frame's buttons
/// @return true on the frame the pair completes
bool shoulders_pressed(const Pad& pad)
{
   return pad.down(KEY_L) && pad.down(KEY_R) && (pad.hit(KEY_L) || pad.hit(KEY_R));
}

} // namespace

/// Runs the sequence test until the platform quits.
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
      switch (g_mode)
      {
      case Mode::Menu:
         update_menu(pad);
         break;
      case Mode::Running:
         if (shoulders_pressed(pad))
         {
            g_mode = Mode::Paused;
         }
         else
         {
            step(pad);
         }
         draw_run();
         break;
      case Mode::Paused:
         if (pad.hit(KEY_A))
         {
            step(pad, static_cast<KeyBits>(~hal::input::key_bit(KEY_A)));
         }
         else if (pad.hit(KEY_START))
         {
            g_mode = Mode::Running;
         }
         else if (pad.hit(KEY_B))
         {
            g_mode = Mode::Menu;
            draw_menu();
            break;
         }
         draw_run();
         break;
      case Mode::Exited:
         if (pad.hit(KEY_A))
         {
            start_run();
         }
         else if (pad.hit(KEY_B))
         {
            g_mode = Mode::Menu;
            draw_menu();
            break;
         }
         draw_run();
         break;
      }
   }
   return 0;
}
