/// @file
/// Sequence sandbox: the sequence player module (title flows, cutscenes,
/// credits) playing full-screen with the engine's real port and graphics
/// modules, without the rest of a game.
///
/// Laid out like a game: the engine library, reference/project.json (what to
/// build and which sequences), data/assets.json (placeholder shape art, text
/// and the showcase list) and this scene code. The sequences are the engine's
/// test manifests (tests/data/sequences). Audio is not part of this sandbox:
/// songs are silent and run for the song length chosen in the menu, so
/// song-timed waits and exits still happen.
///
/// - Menu: Up/Down choose; A run (on a sequence), toggle (on a condition);
///   Left/Right change the song length.
/// - Playing: the sequence gets the pad as-is. L+R together pause.
/// - Paused: A one frame (other held keys go to the sequence), START resume,
///   B stop.
/// - Result: A run again, B menu. Lists what the engine could not show yet.
#include "test_kit.h"

#include "capsule/asset/asset.h"
#include "capsule/core/memory.h"
#include "capsule/hal/system.h"
#include "capsule/seq/presenter.h"
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

constexpr int kMaxConds = 4;   ///< Conditions shown in the menu.

constexpr int kKeyNameSize = 48;       ///< Buffer for a list's asset key name.
constexpr int kListPrefixLength = 5;   ///< Characters in "list:".
constexpr KeyBits kAllKeys = 0xFFFF;   ///< Every key bit: pass all keys to the sequence.

/// @name Menu layout
///@{
constexpr int kMenuHeaderRow = 2;   ///< Row of the "Sequence" header.
constexpr int kFixturesTop = 3;     ///< Row of the first sequence.
constexpr int kItemCol = 2;         ///< Column of a menu item, after the cursor.
constexpr int kCondNameCol = 6;     ///< Column of a condition's name, after its "[x]" box.
///@}

/// @name Result layout
///@{
constexpr int kExitRow = 2;           ///< Row of the exit (or "Stopped").
constexpr int kStageRow = 3;          ///< Row of the stage it ended in.
constexpr int kFramesRow = 4;         ///< Row of the frame count.
constexpr int kMissingTitleRow = 6;   ///< Row of "Not shown yet:".
constexpr int kMissingTop = 7;        ///< Row of the first missing feature.
constexpr int kMissingCol = 2;        ///< Column of the missing features (indented).
constexpr int kKeyDigits = 8;         ///< Hex digits of an asset key.
///@}

/// A sequence and its debug names.
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
/// The sequences listed in reference/project.json.
const Fixture kFixtures[] = {
   CAPSULE_FIXTURE("cutscene", kSeqCutscene),           CAPSULE_FIXTURE("title_timeout", kSeqTitleTimeout),
   CAPSULE_FIXTURE("title_song_end", kSeqTitleSongEnd), CAPSULE_FIXTURE("credits_showcase", kSeqCreditsShowcase),
   CAPSULE_FIXTURE("credits_map", kSeqCreditsMap),
};
#undef CAPSULE_FIXTURE
constexpr int kNumFixtures = sizeof(kFixtures) / sizeof(kFixtures[0]);   ///< Fixture count.

/// Song lengths offered in the menu, in frames (0 = until the sequence stops it).
constexpr uint32_t kSongLengths[] = {600, 1200, 2400, 300, 0};
constexpr int kNumSongLengths = sizeof(kSongLengths) / sizeof(kSongLengths[0]);   ///< Choices.

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

/// What the app is doing.
enum class Mode : uint8_t
{
   Menu,      ///< Choosing a sequence and its settings.
   Playing,   ///< Full-screen playback.
   Paused,    ///< Playback stopped; A steps one frame.
   Result     ///< The sequence exited or was stopped.
};

Mode g_mode = Mode::Menu;     ///< Current mode.
int g_fixture = 0;            ///< Selected sequence.
int g_cursor = 0;             ///< Menu row: sequences, conditions, song length.
int g_song = 0;               ///< Index into kSongLengths.
bool g_cond[kMaxConds + 1];   ///< Condition values, 1-based.
bool g_stopped = false;       ///< Result came from B in the pause menu.
CAPSULE_EWRAM_BSS seq::Player g_player;   ///< The player (about 1 KB: EWRAM).
seq::presenter::MissingMask g_missing = 0;   ///< presenter::missing() when the run ended.
AssetKey g_missing_key{0};    ///< presenter::last_missing_asset() when the run ended.

/// Asset key of a sequence list: "list:" + the list's name in upper case with
/// spaces for underscores (e.g. credits_showcase -> list:CREDITS SHOWCASE).
/// @param list  1-based list id of the playing sequence
/// @return the key, or AssetKey{0} for an unknown id
AssetKey list_key(uint8_t list)
{
   const Fixture& f = kFixtures[g_fixture];
   if (list == 0 || list > count(f.lists))
   {
      return AssetKey{0};
   }
   char name[kKeyNameSize] = "list:";
   int n = kListPrefixLength;
   for (const char* p = f.lists[list - 1]; *p != '\0' && n < kKeyNameSize - 1; ++p)
   {
      char c = *p;
      if (c == '_')
      {
         c = ' ';
      }
      if (c >= 'a' && c <= 'z')
      {
         c = static_cast<char>(c - 'a' + 'A');
      }
      name[n++] = c;
   }
   name[n] = '\0';
   return asset::key_v1(name);
}

} // namespace

// --- game hooks (sequence_port.h; documented there) ----------------------------

namespace capsule::seq::game
{

bool cond(uint8_t id)
{
   return id >= 1 && id <= kMaxConds && g_cond[id];
}

uint16_t list_count(uint8_t list)
{
   const asset::List* l = asset::list(list_key(list));
   return l != nullptr ? l->count : 0;
}

AssetKey list_asset(uint8_t list, uint16_t index)
{
   const asset::List* l = asset::list(list_key(list));
   return (l != nullptr && index < l->count) ? l->items[index] : AssetKey{0};
}

void rng_mix_entropy(uint32_t)
{
}

} // namespace capsule::seq::game

namespace
{

/// @name Screen text
///@{
constexpr auto kApp = text::encode("SEQUENCE SANDBOX");                    ///< Screen text "SEQUENCE SANDBOX".
constexpr auto kMenuHint1 = text::encode("Up/Down choose  A run/toggle");  ///< Screen text.
constexpr auto kMenuHint2 = text::encode("Left/Right song length");        ///< Screen text.
constexpr auto kResultHint = text::encode("A run again  B menu");          ///< Screen text.
///@}

/// Names of presenter::Missing bits.
const char* const kMissingNames[] = {"assets (see key)",  "map view",           "scanline effects",
                                     "particle emitters", "palette animations", "audio (silent)"};
static_assert(sizeof(kMissingNames) / sizeof(kMissingNames[0]) == seq::presenter::NUM_MISSING);

/// Number of menu rows for the selected sequence.
/// @return sequences + its conditions (up to kMaxConds) + the song length row
int menu_rows()
{
   int conds = count(kFixtures[g_fixture].conds);
   if (conds > kMaxConds)
   {
      conds = kMaxConds;
   }
   return kNumFixtures + conds + 1;
}

/// Switches to the console: clears the scene, sets up BG0 text.
void enter_console()
{
   seq::presenter::reset();
   init_console();
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
   int y = kFixturesTop + kNumFixtures + 1;   // one blank row after the sequences
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
   ++y;
   const bool at = g_cursor == row;
   put_ascii(at ? INK_ON : INK_DIM, 0, y, at ? ">" : " ");
   const int x = put_ascii(INK_DIM, kItemCol, y, "Song length:");
   if (kSongLengths[g_song] == 0)
   {
      put_ascii(INK_ON, x + 1, y, "until stopped");
   }
   else
   {
      const int x2 = put_uint(INK_ON, x + 1, y, kSongLengths[g_song]);
      put_ascii(INK_DIM, x2 + 1, y, "frames");
   }
   controls(kMenuHint1.codes, kMenuHint2.codes);
}

/// Draws the result screen.
void draw_result()
{
   const Fixture& f = kFixtures[g_fixture];
   const seq::Player& p = g_player;
   clear_console();
   int x = put(INK_TITLE, 0, 0, kApp.codes);
   put_ascii(INK_ON, x + 1, 0, f.name);
   if (g_stopped)
   {
      put_ascii(INK_ON, 0, kExitRow, "Stopped");
   }
   else
   {
      x = put_ascii(INK_DIM, 0, kExitRow, "Exit");
      x = put_uint(INK_ON, x + 1, kExitRow, p.exit_code);
      put_ascii(INK_ON, x + 1, kExitRow, p.exit_code < count(f.exits) ? f.exits[p.exit_code] : "?");
   }
   x = put_ascii(INK_DIM, 0, kStageRow, "in stage");
   put_ascii(INK_ON, x + 1, kStageRow, f.stages[p.stage]);
   x = put_ascii(INK_DIM, 0, kFramesRow, "after");
   x = put_uint(INK_ON, x + 1, kFramesRow, p.total_frames);
   put_ascii(INK_DIM, x + 1, kFramesRow, "frames");

   const seq::presenter::MissingMask missing = g_missing;
   put_ascii(INK_TITLE, 0, kMissingTitleRow, "Not shown yet:");
   int y = kMissingTop;
   if (missing == 0)
   {
      put_ascii(INK_DIM, kMissingCol, y++, "nothing");
   }
   for (int i = 0; i < seq::presenter::NUM_MISSING; ++i)
   {
      if (!(missing & flag_mask<seq::presenter::MissingMask>(static_cast<seq::presenter::Missing>(i))))
      {
         continue;
      }
      put_ascii(INK_DIM, kMissingCol, y++, kMissingNames[i]);
   }
   if (missing & flag_mask<seq::presenter::MissingMask>(seq::presenter::MISSING_ASSET))
   {
      x = put_ascii(INK_DIM, kMissingCol, y, "last missing key");
      put_hex(INK_ON, x + 1, y, raw(g_missing_key), kKeyDigits);
   }
   controls(kResultHint.codes, nullptr);
}

/// Starts the selected sequence full-screen.
void start_run()
{
   seq::presenter::reset();
   seq::presenter::set_song_length(kSongLengths[g_song]);
   seq::start(g_player, *kFixtures[g_fixture].seq);
   g_stopped = false;
   g_mode = Mode::Playing;
}

/// Shows the result screen.
/// @param stopped  true when stopped from the pause menu
void finish(bool stopped)
{
   g_stopped = stopped;
   g_mode = Mode::Result;
   // Keep the missing-feature record: the console reset clears it.
   g_missing = seq::presenter::missing();
   g_missing_key = seq::presenter::last_missing_asset();
   enter_console();
}

/// Advances the sequence and the presenter one frame.
/// @param pad   this frame's buttons
/// @param keep  keys passed to the sequence (paused single steps drop A)
void step(const Pad& pad, KeyBits keep = kAllKeys)
{
   const auto held = static_cast<KeyBits>(pad.held & keep);
   const auto pressed = static_cast<KeyBits>(pad.pressed & keep);
   const seq::Status status = seq::step(g_player, held, pressed);
   seq::presenter::frame();
   if (status == seq::Status::Exited)
   {
      finish(false);
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
      {   // choosing a sequence shows its conditions
         g_fixture = g_cursor;
         redraw = true;
      }
      if (pad.hit(KEY_A) || pad.hit(KEY_START))
      {
         start_run();
         return;
      }
   }
   else if (g_cursor < rows - 1)
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
      if (pad.repeat(KEY_LEFT))
      {
         g_song = (g_song + kNumSongLengths - 1) % kNumSongLengths;
         redraw = true;
      }
      if (pad.repeat(KEY_RIGHT))
      {
         g_song = (g_song + 1) % kNumSongLengths;
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

/// Runs the sandbox until the platform quits.
/// @return 0
int main()
{
   hal::system::init();
   enter_console();
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
      case Mode::Playing:
         if (shoulders_pressed(pad))
         {
            g_mode = Mode::Paused;
            break;
         }
         step(pad);
         if (g_mode == Mode::Result)
         {
            draw_result();
         }
         break;
      case Mode::Paused:
         if (pad.hit(KEY_A))
         {
            step(pad, static_cast<KeyBits>(~hal::input::key_bit(KEY_A)));
            if (g_mode == Mode::Result)
            {
               draw_result();
            }
         }
         else if (pad.hit(KEY_START))
         {
            g_mode = Mode::Playing;
         }
         else if (pad.hit(KEY_B))
         {
            finish(true);
            draw_result();
         }
         break;
      case Mode::Result:
         if (pad.hit(KEY_A))
         {
            start_run();
         }
         else if (pad.hit(KEY_B))
         {
            g_mode = Mode::Menu;
            draw_menu();
         }
         break;
      }
   }
   return 0;
}
