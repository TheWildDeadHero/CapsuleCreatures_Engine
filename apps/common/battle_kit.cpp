/// @file
/// The battle sandbox kit (battle_kit.h): a game's battle data played with a minimal presentation. The engine's
/// apps/starter_battle and the reference repo's sandbox_battle run it with their own battle_kit::GameConfig.
///
/// The presentation plays the engine's events at its own pace (text, HP bars, sprites; presenter cues through
/// battle::presenter_view); the engine never waits for it. Menus follow the data: battler pools besides the hit
/// points are shown in the party and command menus, the move menu pages four moves at a time and shows the cursor
/// move's first move resource pool, its battler costs, type and power; a move whose cost cannot be paid is greyed
/// out (with nothing payable, FIGHT uses the data's free fallback move). G3: BAG uses an item of the player's bag
/// (GameConfig::bag) on its target kind; a recruit's checks wobble the creature's sprite and a recruited creature
/// joins the party, else the storage boxes, which the menu's "Storage boxes" page shows.
///
/// Host-free: no heap (fixed tables sized by engine caps), State and parties in EWRAM (CAPSULE_EWRAM_BSS).
#include "test_kit.h"

#include "battle_kit.h"

#include "capsule/asset/asset.h"
#include "capsule/battle/battle.h"
#include "capsule/battle/presenter_view.h"
#include "capsule/core/memory.h"
#include "capsule/core/random.h"
#include "capsule/core/types.h"
#include "capsule/gfx/background.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/slots.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/monster/monster.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace
{

using namespace capsule;
using namespace capsule::testkit;
namespace video = hal::video;
namespace bt = capsule::battle;
using hal::input::KEY_A;
using hal::input::KEY_B;
using hal::input::KEY_DOWN;
using hal::input::KEY_L;
using hal::input::KEY_LEFT;
using hal::input::KEY_R;
using hal::input::KEY_RIGHT;
using hal::input::KEY_SELECT;
using hal::input::KEY_START;
using hal::input::KEY_UP;

/// Text console: BG0, font in character block 0, map in screen block 28,
/// inks in banks 8-11. (The backdrop on BG3 uses character block 3, which
/// covers screen blocks 24-26 for its tiles, and screen block 31 for its map.)
constexpr ConsoleLayout kText = {0, 28, 8, false};
constexpr video::BgIndex kPanelBg = 1;       ///< Panels layer.
constexpr uint8_t kPanelChar = 1;            ///< Its character block.
constexpr uint8_t kPanelScreen = 29;         ///< Its screen block.
constexpr uint8_t kPanelBank = 13;           ///< Panel colors (1 fill, 2 border).
constexpr video::TileIndex kBarTile = 128;   ///< First HP-bar tile (char block 0): 9 fills.
constexpr uint8_t kBarGreen = 12;            ///< HP bar banks (1 fill, 2 empty).
constexpr uint8_t kBarYellow = 14;           ///< ...
constexpr uint8_t kBarRed = 15;              ///< ...
constexpr int kBarTiles = 6;                 ///< HP bar width in tiles (48 px).
constexpr int kBoxTop = 15;                  ///< Text box first row.
constexpr int kTextFrames = 48;              ///< Frames a message stays (normal speed).
constexpr int kFastFrames = 12;              ///< ... in fast mode.

/// @name Panels and HP bars
/// Panel tiles: 0 transparent, 1 fill, 2 border. Panel and bar banks use
/// entries 1 and 2 (fill / border, or fill / empty).
///@{
constexpr video::TileIndex kPanelFillTile = 1;     ///< Panel fill tile.
constexpr video::TileIndex kPanelBorderTile = 2;   ///< Panel border tile.
constexpr int kPanelTiles = 3;                     ///< Panel tiles loaded (empty, fill, border).
constexpr uint32_t kFillRow = 0x11111111u;         ///< A 4bpp tile row all in color 1.
constexpr uint32_t kBorderRow = 0x22222222u;       ///< A 4bpp tile row all in color 2.
constexpr int kUiColors = 2;                       ///< Colors per panel or bar bank (entries 1-2).
constexpr int kBitsPerPixel = 4;                   ///< Bits of one 4bpp pixel.
constexpr int kBarTopRow = 2;                      ///< First pixel row of the bar inside its tile.
constexpr int kBarBottomRow = 5;                   ///< Last pixel row of the bar inside its tile.
constexpr int kBarEmptyColor = 2;                  ///< Bar color index of the empty part.
constexpr int kBarPixels = kBarTiles * kTilePixels;   ///< HP bar width in pixels.
constexpr uint32_t kBarFifths = 5;                 ///< Bar colors change at fifths of the maximum HP...
constexpr uint32_t kGreenFifths = 2;               ///< ...green above 2/5, yellow above 1/5, red below.
///@}

/// @name Screen setup
///@{
constexpr video::BgIndex kBackdropBg = 3;    ///< Backdrop layer.
constexpr uint8_t kBackdropPriority = 3;     ///< Backdrop priority (behind everything).
constexpr uint8_t kSpritePriority = 2;       ///< Creature sprite priority (behind the panels).
constexpr uint32_t kPartySeed = 0xC0FFEEu;   ///< Random seed of the starting party.
constexpr uint32_t kBattleSeedMix = 0xB4771Eu;   ///< Mixed into the gameplay state to seed a battle.
constexpr uint8_t kParticipants = 2;         ///< Participants of every battle: the player and one opponent.
constexpr int kUnownedDouble = 2;               ///< Unowned creatures in a 2 vs 2 battle.
constexpr int kEventTextCodes = 96;          ///< Buffer for one event's text.
constexpr int kRefusalTextCodes = 64;        ///< Buffer for the "no running" message.
///@}

/// @name Scenario menu layout
///@{
constexpr int kMenuTop = 2;               ///< Row of the first scenario.
constexpr int kMenuNameCol = 3;           ///< Column of the scenario names, after the cursor.
constexpr int kPartyNameBuffer = 20;    ///< Buffer for a party name.
constexpr int kPartyNameChars = 18;     ///< Letters of a party name shown.
constexpr int kSpeciesNameBuffer = 13;    ///< Buffer for a species name.
constexpr int kSpeciesNameChars = 10;     ///< Letters of a species name shown.
constexpr int kPartyLvCol = 12;           ///< Column of "Lv".
constexpr int kPartyLevelCol = 14;        ///< Column of the level.
constexpr int kPartyHpCol = 18;           ///< Column of the current HP.
constexpr int kPartySlashCol = 21;        ///< Column of the "/" between the HP numbers.
constexpr int kPartyMaxHpCol = 22;        ///< Column of the maximum HP.
constexpr int kPoolsRowOffset = 1;        ///< A member's pools line (MP, EP, ...) is the row below its HP line.
constexpr int kPoolsCol = 3;              ///< Column of a member's pools line.
constexpr int kHpDigits = 3;              ///< Width of an HP number.
///@}

/// @name Battle HUD layout
/// A HUD box has 3 rows (name, bar, HP numbers on the player's side), or 2 when
/// a side has more than kCompactSideSize positions.
///@{
constexpr int kCompactSideSize = 2;   ///< Positions per side that still get full boxes.
constexpr int kFullHudRows = 3;       ///< Rows of a full box.
constexpr int kCompactHudRows = 2;    ///< Rows of a compact box.
constexpr int kNearHudCol = 16;       ///< Column of the player's side boxes.
constexpr int kHudCols = 14;          ///< Columns of a box.
constexpr int kHudNameBuffer = 11;    ///< Buffer for a battler name.
constexpr int kHudNameChars = 9;      ///< Letters of a battler name shown.
constexpr int kHudLvCol = 10;         ///< Column of "L" in a box.
constexpr int kHudLevelCol = 11;      ///< Column of the level in a box.
constexpr int kHudStatusCol = 8;      ///< Column of the status tag in a box.
constexpr int kStatusChars = 3;       ///< Letters of a status tag.
constexpr int kHudHpRow = 2;          ///< Row of the HP numbers in a box.
constexpr int kHudHpCol = 2;          ///< Column of the current HP in a box.
constexpr int kHudSlashCol = 5;       ///< Column of the "/" in a box.
constexpr int kHudMaxHpCol = 6;       ///< Column of the maximum HP in a box.
constexpr int kWeatherCol = 17;       ///< Column of the weather label.
constexpr int kWeatherChars = 12;     ///< Letters of the weather name shown.
///@}

/// @name Text box and menus
///@{
constexpr int kBoxRows = 5;                             ///< Rows of the text box.
constexpr int kBoxBottom = kBoxTop + kBoxRows - 1;      ///< Last row of the text box.
constexpr int kBoxLineChars = 28;                       ///< Characters per text box line.
constexpr int kBoxLines = 2;                            ///< Lines of a message.
constexpr int kBoxLineSpacing = 2;                      ///< Rows from one message line to the next.
constexpr int kBoxLine1 = kBoxTop + 1;                  ///< Row of the first box line.
constexpr int kBoxLine2 = kBoxLine1 + kBoxLineSpacing;  ///< Row of the second box line.
constexpr int kCommandItems = 4;                        ///< FIGHT, BAG, PARTY, RUN.
/// @name The command menu's entries (indices into it)
///@{
constexpr int kCommandFight = 0;   ///< FIGHT.
constexpr int kCommandBag = 1;     ///< BAG.
constexpr int kCommandParty = 2;   ///< PARTY.
///@}
constexpr int kCommandNameChars = 12;                   ///< Letters of the battler name in "What will ... do?".
constexpr int kCommandCol0 = 2;                         ///< Column of the first command's cursor.
constexpr int kCommandColStep = 7;                      ///< Columns between commands.
constexpr int kMoveCols = 2;                            ///< Moves and targets are shown two per row.
constexpr int kMoveCol0 = 2;                            ///< Column of the left move or target.
constexpr int kMoveColStep = 14;                        ///< Columns to the right one.
constexpr int kMoveNameChars = 12;                      ///< Letters of a move name shown.
constexpr int kTypeNameBuffer = 10;                     ///< Buffer for a type name.
constexpr int kTypeNameChars = 8;                       ///< Letters of a type name shown.
constexpr int kFieldGap = 2;                            ///< Columns between the cost, type and power fields.
constexpr int kMovesPerPage = 4;                        ///< Moves the move menu shows at once (two rows of two).
constexpr int kPageCol = 25;                            ///< Column of the move page number ("1/2").
constexpr int kCommandPoolsRow = kBoxLine1 + 1;         ///< Row of the battler's pools in the command menu.
constexpr int kTargetTop = kBoxTop + 2;                 ///< Row of the first target row.
constexpr int kTargetNameChars = 10;                    ///< Letters of a target name shown.
constexpr int kAllyTagOffset = 11;                      ///< Column of "(a)" after a target's cursor.
///@}

/// @name Party (switch) menu layout
///@{
constexpr int kSwitchPanelOrigin = 1;     ///< Left column and top row of the panel.
constexpr int kSwitchPanelCols = 28;    ///< Columns of the panel.
constexpr int kSwitchExtraRows = 2;     ///< Panel rows besides the list: title and bottom border.
constexpr int kSwitchTextCol = 2;       ///< Column of the title and the cursor.
constexpr int kSwitchListTop = 2;       ///< Row of the first member.
constexpr int kSwitchNameCol = 3;       ///< Column of a member's name.
constexpr int kSwitchNameChars = 10;    ///< Letters of a member's name shown.
constexpr int kSwitchLCol = 14;         ///< Column of "L".
constexpr int kSwitchLevelCol = 15;     ///< Column of the level.
constexpr int kSwitchHpCol = 19;        ///< Column of the current HP.
constexpr int kSwitchSlashCol = 22;     ///< Column of the "/".
constexpr int kSwitchMaxHpCol = 23;     ///< Column of the maximum HP.
constexpr int kSwitchActiveCol = 27;    ///< Column of the "*" of a member on the field.
///@}

/// @name Bag menu layout (G3; a panel over the field, as the party menu)
///@{
constexpr int kBagRows = 8;               ///< Items the bag menu lists at most.
constexpr int kBagNameCol = 3;            ///< Column of an item's name.
constexpr int kBagNameChars = 14;         ///< Letters of an item's name shown.
constexpr int kBagCountCol = 19;          ///< Column of "x" and its count.
constexpr int kBagNameBuffer = 15;        ///< Buffer for an item's name.
///@}

/// @name Storage boxes page layout (G3)
///@{
constexpr int kBoxesTop = 3;              ///< Row of the first slot.
constexpr int kBoxesRows = 15;            ///< Slots per column.
constexpr int kBoxesColStep = 15;         ///< Columns from one column of slots to the next.
constexpr int kBoxesNameChars = 9;        ///< Letters of a boxed creature's name shown.
constexpr int kBoxesLevelOffset = 11;     ///< Column of its level in its column.
constexpr int kBoxNameChars = 16;         ///< Letters of a box name shown.
///@}

/// @name Item and recruit presentation (G3)
///@{
constexpr int kUseTextCodes = 48;         ///< Codes of "NAME used ITEM!".
constexpr int kWobbleFrames = 24;         ///< Frames a recruit check wobbles the sprite.
///@}

/// @name Sprite motion and pacing
///@{
constexpr int kSpriteCenterOffset = 32;   ///< Format positions are sprite centers; sprites are 64x64.
constexpr int kNudgePixels = 6;           ///< How far an attacker moves forward.
constexpr int8_t kNudgeFrames = 10;       ///< Frames an attacker stays forward.
constexpr int kDropPixelsPerFrame = 4;    ///< Faint slide speed.
constexpr int kFaintFrames = 16;          ///< Frames of a faint slide (the event waits as long).
constexpr int kSwitchFrames = 8;          ///< Frames a switch in or out waits.
constexpr int kTextSpeedUp = 4;           ///< Text timer runs this many times faster with A held.
constexpr int kMaxEventsPerFrame = 64;    ///< Events and steps one frame handles at most.
///@}

/// Scenario menu entries after the parties.
enum Extra : uint8_t
{
   EX_UNOWNED,
   EX_UNOWNED_DOUBLE,
   EX_EXHIBITION,
   EX_BOXES,      ///< The storage boxes page (G3).
   EX_HEAL,
   NUM_EXTRAS
};

/// What the app is doing.
enum class Mode : uint8_t
{
   Menu,
   Battle,
   Boxes,     ///< The storage boxes page (G3).
};

/// What the battle presentation is doing.
enum class Ui : uint8_t
{
   Events,    ///< Playing events / stepping the engine.
   Command,   ///< Fight, Party or Run.
   Fight,     ///< Move choice.
   Target,    ///< Target choice.
   Party,     ///< Switch / replacement choice (or an item's member, G3).
   Bag,       ///< Item choice (G3).
   ItemTarget,   ///< A battler target of the chosen item (G3).
   Result,    ///< Battle over: A returns to the menu.
};

Mode g_mode = Mode::Menu;          ///< Mode.
Ui g_ui = Ui::Events;              ///< Battle presentation state.
int g_cursor = 0;                  ///< Menu cursor.
bool g_fast = false;               ///< Fast text.
const battle_kit::GameConfig* g_game = nullptr;   ///< The game (run()).
// The battle and its parties are large and touched a few times per step: EWRAM (battle.h, "Where State lives").
CAPSULE_EWRAM_BSS monster::Party g_party;      ///< The player's party (kept between battles).
CAPSULE_EWRAM_BSS monster::Party g_unowned;       ///< Unowned creatures of the current battle.
CAPSULE_EWRAM_BSS monster::Party g_ai_party;   ///< AI-controlled "player" party (exhibition).
CAPSULE_EWRAM_BSS bt::State g_battle;          ///< The battle.
CAPSULE_EWRAM_BSS inventory::Bag g_bag;        ///< The player's bag (G3; GameConfig::bag).
storage::Storage g_no_boxes{};                 ///< The empty view of a game without storage boxes.
storage::Storage* g_boxes = &g_no_boxes;       ///< The storage boxes recruited creatures go to (GameConfig::boxes).
uint16_t g_item = 0;               ///< The item being aimed (Bag, ItemTarget, Party states), or 0.
int g_box = 0;                     ///< The box the storage page shows.
bt::Request g_request{};           ///< Pending human choice.
uint8_t g_move_slot = 0;           ///< Chosen move (Target state).
int g_text_timer = 0;              ///< Frames left on the current message.
uint16_t g_shown_hp[bt::kMaxBattlers];       ///< HP the HUD shows.
uint16_t g_target_hp[bt::kMaxBattlers];      ///< HP it moves toward.
uint8_t g_shown_status[bt::kMaxBattlers];    ///< Status the HUD shows.
int8_t g_nudge[bt::kMaxPositions];           ///< Attack nudge per position (frames left).
int8_t g_drop[bt::kMaxPositions];            ///< Faint slide per position (frames done, -1 = none).

/// Sprite slot of a format position's battler (capsule/gfx/slots.h: slots 0-5).
/// @param pos  position
/// @return its slot
constexpr uint8_t battler_slot(uint8_t pos)
{
   return static_cast<uint8_t>(gfx::slots::kFirstBattler + pos);
}
static_assert(gfx::slots::kBattlers >= bt::kMaxPositions, "one battler sprite slot per position");

// -- drawing helpers ------------------------------------------------------------------------------

/// Writes one panel cell.
/// @param x     column
/// @param y     row
/// @param tile  0 transparent, 1 fill, 2 border
void panel_cell(int x, int y, video::TileIndex tile)
{
   video::write_map_cell(kPanelBg, x, y, video::map_cell(tile, kPanelBank));
}

/// Fills a panel rectangle (border on the edges).
/// @param x0  left column
/// @param y0  top row
/// @param w   width
/// @param h   height
void panel(int x0, int y0, int w, int h)
{
   for (int y = y0; y < y0 + h; ++y)
   {
      for (int x = x0; x < x0 + w; ++x)
      {
         const bool edge = y == y0 || y == y0 + h - 1 || x == x0 || x == x0 + w - 1;
         panel_cell(x, y, edge ? kPanelBorderTile : kPanelFillTile);
      }
   }
}

/// Clears every panel.
void clear_panels()
{
   for (int y = 0; y < kMapCells; ++y)
   {
      for (int x = 0; x < kMapCells; ++x)
      {
         panel_cell(x, y, 0);
      }
   }
}

/// Loads the panel and HP-bar tiles and colors.
void load_ui_graphics()
{
   uint32_t tiles[kPanelTiles][kTilePixels] = {};
   for (int r = 0; r < kTilePixels; ++r)
   {
      tiles[kPanelFillTile][r] = kFillRow;
      tiles[kPanelBorderTile][r] = kBorderRow;
   }
   video::load_bg_tiles(kPanelChar, 0, &tiles[0][0], kPanelTiles);
   constexpr video::Color kPanelColors[kUiColors] = {video::rgb5(4, 6, 12), video::rgb5(18, 20, 26)};
   gfx::palette::load_bg(static_cast<uint8_t>(kPanelBank * kColorsPerBank + 1), kPanelColors, kUiColors);

   // HP bar tiles: rows 2-5 filled with 0..8 pixels of color 1, the rest color 2.
   for (int fill = 0; fill <= kTilePixels; ++fill)
   {
      uint32_t t[kTilePixels] = {};
      for (int r = kBarTopRow; r <= kBarBottomRow; ++r)
      {
         uint32_t row = 0;
         for (int px = 0; px < kTilePixels; ++px)
         {
            row |= static_cast<uint32_t>(px < fill ? 1 : kBarEmptyColor) << (px * kBitsPerPixel);
         }
         t[r] = row;
      }
      video::load_bg_tiles(kText.char_block, static_cast<video::TileIndex>(kBarTile + fill), t, 1);
   }

   constexpr video::Color kEmpty = video::rgb5(6, 8, 10);
   constexpr video::Color kGreens[kUiColors] = {video::rgb5(6, 28, 10), kEmpty};
   constexpr video::Color kYellows[kUiColors] = {video::rgb5(30, 26, 4), kEmpty};
   constexpr video::Color kReds[kUiColors] = {video::rgb5(30, 8, 6), kEmpty};
   gfx::palette::load_bg(static_cast<uint8_t>(kBarGreen * kColorsPerBank + 1), kGreens, kUiColors);
   gfx::palette::load_bg(static_cast<uint8_t>(kBarYellow * kColorsPerBank + 1), kYellows, kUiColors);
   gfx::palette::load_bg(static_cast<uint8_t>(kBarRed * kColorsPerBank + 1), kReds, kUiColors);
}

/// Sets every layer up for the menu or a battle.
/// @param battle  battle layout (backdrop on)
void setup_screen(bool battle)
{
   init_console(kText);   // mode 0, BG0 text
   video::configure_bg(kPanelBg, true, 1, kPanelChar, kPanelScreen, false, 0);
   video::set_bg_scroll(kPanelBg, 0, 0);
   load_ui_graphics();
   clear_panels();
   const asset::Background* field = asset::background(asset::key_v1("image:BATTLE FIELD"));
   if (battle && field != nullptr)
   {
      gfx::background::load(kBackdropBg, *field);
      gfx::background::show(kBackdropBg, true);
      gfx::background::set_position(kBackdropBg, 0, 0);
      video::configure_bg(kBackdropBg, true, kBackdropPriority, gfx::background::char_block(kBackdropBg),
                          gfx::background::screen_block(kBackdropBg), false, 0);
   }
   else
   {
      gfx::background::show(kBackdropBg, false);
      constexpr video::Color kBackdrop = video::rgb5(3, 6, 12);
      video::load_bg_palette(0, &kBackdrop, 1);
   }
   for (uint8_t pos = 0; pos < bt::kMaxPositions; ++pos)
   {
      gfx::sprites::show(battler_slot(pos), false);
   }
   bt::presenter_view::reset();   // presenter slots 6-15
}

/// Copies codes into a buffer, at most @p n.
/// @param out  buffer (n + 1 codes)
/// @param in   codes
/// @param n    limit
void clip(text::Code* out, const text::Code* in, int n)
{
   int i = 0;
   for (; in != nullptr && in[i] != text::kCodeEnd && i < n; ++i)
   {
      out[i] = in[i];
   }
   out[i] = text::kCodeEnd;
}

// -- party ---------------------------------------------------------------------------------------

/// Builds the starting party (GameConfig::start).
void make_party()
{
   g_party = monster::Party{};
   rng::Stream r{kPartySeed};
   for (int k = 0; k < g_game->start_count && g_party.count < monster::kMaxPartySize; ++k)
   {
      const battle_kit::StartMember& s = g_game->start[k];
      if (bt::make_monster(g_party.members[g_party.count], s.species, s.level, r))
      {
         ++g_party.count;
      }
   }
}

/// Restores every member's resources (every persisted pool full) and status.
/// @param party  the party
void heal(monster::Party& party)
{
   for (int i = 0; i < party.count; ++i)
   {
      monster::Record& rec = party.members[i];
      bt::fill_resources(rec);
      monster::set(rec, monster::Role::PrimaryStatus, 0);
   }
}

/// Whether a resource is a battler pool besides the hit points (MP, EP, ...: what the menus show).
/// @param r  BattleData::resources index
/// @return true for such a pool
bool shown_pool(uint8_t r)
{
   const bt::ResourceData& d = bt::kData.resources[r];
   return d.scope == static_cast<uint8_t>(bt::ResourceScope::Battler) && r != bt::kData.faint_resource;
}

/// Draws a battler's pools besides the hit points: "MP 12/20  EP 5/18" (nothing without such pools).
/// @param b  the battler
/// @param x  first column
/// @param y  row
void put_pools(const bt::Battler& b, int x, int y)
{
   for (uint8_t r = 0; r < bt::kData.resource_count; ++r)
   {
      if (!shown_pool(r))
      {
         continue;
      }
      x = put(INK_DIM, x, y, bt::kData.resources[r].label);
      x = put_uint(INK_ON, x + 1, y, static_cast<uint32_t>(bt::resource_value(b, r)));
      x = put_ascii(INK_DIM, x, y, "/");
      x = put_uint(INK_ON, x, y, static_cast<uint32_t>(bt::resource_max(b, r)));
      x += kFieldGap;
   }
}

/// The first move resource (the pool the move menu shows for the cursor's slot, e.g. PP), or kNone8.
/// @return its BattleData::resources index
uint8_t shown_move_resource()
{
   for (uint8_t r = 0; r < bt::kData.resource_count; ++r)
   {
      if (bt::kData.resources[r].scope == static_cast<uint8_t>(bt::ResourceScope::Move))
      {
         return r;
      }
   }
   return bt::kNone8;
}

/// Rows a party member takes in the menu: its HP line, and the pools line when a pool besides the hit points is
/// persisted.
/// @return 1 or 2
int member_rows()
{
   for (uint8_t r = 0; r < bt::kData.resource_count; ++r)
   {
      if (shown_pool(r) && bt::kData.resources[r].role != monster::kNoRole)
      {
         return 1 + kPoolsRowOffset;
      }
   }
   return 1;
}

/// Fills the bag with the starting items (GameConfig::bag).
void restock_bag()
{
   inventory::clear(g_bag);
   for (int k = 0; g_game->bag != nullptr && k < g_game->bag_count; ++k)
   {
      inventory::add(g_bag, g_game->bag[k].item, g_game->bag[k].count);
   }
}

/// Restores the player's party and restocks the bag.
void heal_party()
{
   heal(g_party);
   restock_bag();
}

/// Whether any party member can fight.
/// @return true if one has HP
bool party_ready()
{
   for (int i = 0; i < g_party.count; ++i)
   {
      if (monster::get(g_party.members[i], monster::Role::CurrentHp) > 0)
      {
         return true;
      }
   }
   return false;
}

// -- scenario menu ---------------------------------------------------------------------------------

/// Entries in the scenario menu.
/// @return parties + extras
int menu_rows()
{
   return bt::kData.party_count + NUM_EXTRAS;
}

/// Row of "PARTY": right below the scenario entries.
/// @return the row
int party_title_row()
{
   return kMenuTop + menu_rows();
}

/// Draws the scenario menu.
void draw_menu()
{
   clear_console();
   clear_panels();
   put_ascii(INK_TITLE, 1, 0, g_game->title);
   for (int i = 0; i < menu_rows(); ++i)
   {
      const Ink ink = i == g_cursor ? INK_ON : INK_DIM;
      put_ascii(ink, 1, kMenuTop + i, i == g_cursor ? ">" : " ");
      if (i < bt::kData.party_count)
      {
         text::Code name[kPartyNameBuffer];
         clip(name, bt::kData.parties[i].name, kPartyNameChars);
         int x = put(ink, kMenuNameCol, kMenuTop + i, name);
         put_ascii(INK_OFF, x + 1, kMenuTop + i, i == g_game->double_party ? "(2 vs 2)" : "");
      }
      else
      {
         const char* const extras[NUM_EXTRAS] = {g_game->unowned_label, g_game->unowned_double_label,
                                                 g_game->exhibition_label, "Storage boxes", "Heal party"};
         put_ascii(ink, kMenuNameCol, kMenuTop + i, extras[i - bt::kData.party_count]);
      }
   }

   put_ascii(INK_TITLE, 1, party_title_row(), "PARTY");
   // a member's pools line only while every member fits above the controls (G3: a recruit grows the party)
   const int rows = party_title_row() + 1 + g_party.count * member_rows() <= kControlsTop ? member_rows() : 1;
   for (int i = 0; i < g_party.count; ++i)
   {
      const monster::Record& rec = g_party.members[i];
      const uint16_t sp = bt::find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
      uint16_t st[bt::kMaxStats] = {};
      bt::record_stats(rec, st);
      text::Code name[kSpeciesNameBuffer];
      clip(name, sp != bt::kNone16 ? bt::kData.species[sp].name : nullptr, kSpeciesNameChars);
      const int y = party_title_row() + 1 + i * rows;
      put(INK_ON, 1, y, name);
      put_ascii(INK_DIM, kPartyLvCol, y, "Lv");
      put_uint(INK_ON, kPartyLevelCol, y, monster::get(rec, monster::Role::Level));
      put_uint(INK_ON, kPartyHpCol, y, monster::get(rec, monster::Role::CurrentHp), kHpDigits);
      put_ascii(INK_DIM, kPartySlashCol, y, "/");
      put_uint(INK_ON, kPartyMaxHpCol, y, st[bt::kData.pool_stat]);
      // the persisted pools besides the hit points (MP, EP, ...), from the record
      int x = kPoolsCol;
      for (uint8_t r = 0; rows > 1 && r < bt::kData.resource_count; ++r)
      {
         if (shown_pool(r) && bt::kData.resources[r].role != monster::kNoRole)
         {
            x = put(INK_DIM, x, y + kPoolsRowOffset, bt::kData.resources[r].label);
            x = put_uint(INK_ON, x + 1, y + kPoolsRowOffset, monster::get(rec, bt::kData.resources[r].role));
            x += kFieldGap;
         }
      }
   }
   controls(text::encode("Up/Down choose  A start").codes, text::encode("").codes);
}

// -- battle HUD -------------------------------------------------------------------------------------

/// Whether a position is on the player's side of the screen (bottom-left).
/// @param pos  position
/// @return true for the player's side
bool near_side(uint8_t pos)
{
   return bt::format(g_battle).side_of[pos] == bt::player_side(g_battle);
}

/// Rows per HUD box: 3 (name, bar, HP numbers on the player's side), or 2
/// when a side has more than two positions (compact, BUDGET-4).
/// @return rows
int hud_rows()
{
   const bt::FormatData& f = bt::format(g_battle);
   int per_side = 0;
   for (uint8_t p = 0; p < f.positions; ++p)
   {
      per_side += f.side_of[p] == f.side_of[0] ? 1 : 0;
   }
   return per_side > kCompactSideSize ? kCompactHudRows : kFullHudRows;
}

/// HUD box origin of a position.
/// @param pos  position
/// @param x    column
/// @param y    row
void hud_origin(uint8_t pos, int& x, int& y)
{
   const bt::FormatData& f = bt::format(g_battle);
   int index = 0;
   int count = 0;
   for (uint8_t p = 0; p < f.positions; ++p)
   {
      if (f.side_of[p] != f.side_of[pos])
      {
         continue;
      }
      if (p < pos)
      {
         ++index;
      }
      ++count;
   }
   const int rows = hud_rows();
   if (near_side(pos))
   {
      x = kNearHudCol;   // stacked upward from the text box
      y = kBoxTop - rows * (count - index);
   }
   else
   {
      x = 0;
      y = rows * index;
   }
}

/// Draws one HUD box.
/// @param pos  position
void draw_hud(uint8_t pos)
{
   int x0 = 0;
   int y0 = 0;
   hud_origin(pos, x0, y0);
   const uint8_t i = g_battle.at[pos];
   const int rows = hud_rows();
   for (int y = y0; y < y0 + rows; ++y)
   {
      blank(x0, x0 + kHudCols, y);
      for (int x = x0; x < x0 + kHudCols; ++x)
      {
         panel_cell(x, y, 0);
      }
   }
   if (i == bt::kBench || g_drop[pos] >= 0)
   {
      return;
   }
   panel(x0, y0, kHudCols, rows);
   const bt::Battler& b = g_battle.battlers[i];
   text::Code name[kHudNameBuffer];
   clip(name, b.name, kHudNameChars);
   put(INK_TITLE, x0 + 1, y0, name);
   put_ascii(INK_DIM, x0 + kHudLvCol, y0, "L");
   put_uint(INK_ON, x0 + kHudLevelCol, y0, b.level);
   // HP bar.
   const uint32_t hp = g_shown_hp[i];
   const uint32_t max = bt::max_hp(b) != 0 ? bt::max_hp(b) : 1;
   // Filled pixels, rounded up so any HP left shows; then per tile 0-8 pixels.
   const uint32_t px = (hp * kBarTiles * kTilePixels + max - 1) / max;
   const uint8_t bank =
      hp * kBarFifths > max * kGreenFifths ? kBarGreen : (hp * kBarFifths > max ? kBarYellow : kBarRed);
   for (int t = 0; t < kBarTiles; ++t)
   {
      const int fill = static_cast<int>(px) - t * kTilePixels;
      const int f = fill < 0 ? 0 : (fill > kTilePixels ? kTilePixels : fill);
      video::write_map_cell(kConsoleBg, x0 + 1 + t, y0 + 1,
                            video::map_cell(static_cast<video::TileIndex>(kBarTile + f), bank));
   }
   if (g_shown_status[i] != 0 && g_shown_status[i] < bt::kData.status_count)
   {
      text::Code tag[kStatusChars + 1];
      clip(tag, bt::kData.statuses[g_shown_status[i]].name, kStatusChars);
      put(INK_ON, x0 + kHudStatusCol, y0 + 1, tag);
   }
   if (near_side(pos) && rows == kFullHudRows)
   {
      put_uint(INK_ON, x0 + kHudHpCol, y0 + kHudHpRow, hp, kHpDigits);
      put_ascii(INK_DIM, x0 + kHudSlashCol, y0 + kHudHpRow, "/");
      put_uint(INK_ON, x0 + kHudMaxHpCol, y0 + kHudHpRow, max);
   }
}

/// Draws every HUD box and the weather label.
void draw_huds()
{
   for (uint8_t p = 0; p < bt::format(g_battle).positions; ++p)
   {
      draw_hud(p);
   }
   blank(kNearHudCol, kCols, 0);
   if (g_battle.weather != bt::kNone8 && g_battle.weather < bt::kData.weather_count)
   {
      text::Code w[kWeatherChars + 1];
      clip(w, bt::kData.weathers[g_battle.weather].name, kWeatherChars);
      put(INK_DIM, kWeatherCol, 0, w);
   }
}

/// Clears the text box.
void clear_box()
{
   clear_rows(kBoxTop, kBoxBottom);
   panel(0, kBoxTop, kCols, kBoxRows);
}

/// Shows a message in the text box (wrapped to 28 columns, two lines).
/// @param codes  text
void show_message(const text::Code* codes)
{
   clear_box();
   text::Code line[kBoxLineChars + 1];
   const text::Code* from[kBoxLineChars];   // where each cell's code is in the text (to re-read a cut word)
   int row = 0;
   int n = 0;
   int last_space = -1;
   for (const text::Code* c = codes; row < kBoxLines; ++c)
   {
      // Codes that take no cell are dropped (this box knows no control and does not wait):
      // control escapes with their code, prompts and page breaks (battle::format() replaced the placeholders).
      if (*c == text::kCodeControl && c[1] != text::kCodeEnd)
      {
         c += text::escape_length(c) - 1;   // I4: with its argument bytes
         continue;
      }
      if (*c == text::kCodeControl || *c == text::kCodePrompt || *c == text::kCodePageBreak)
      {
         continue;
      }
      const bool end = *c == text::kCodeEnd;
      if (!end && *c != text::kCodeNewline)
      {
         if (*c == ' ')
         {
            last_space = n;
         }
         from[n] = c;
         line[n++] = *c;
      }
      if (end || *c == text::kCodeNewline || n == kBoxLineChars)
      {
         int cut = n;
         if (!end && *c != text::kCodeNewline && n == kBoxLineChars && last_space > 0)
         {
            cut = last_space;
            c = from[last_space];   // re-read the rest of the word next row (the loop steps past the space)
         }
         line[cut] = text::kCodeEnd;
         put(INK_TITLE, 1, kBoxLine1 + row * kBoxLineSpacing, line);
         ++row;
         n = 0;
         last_space = -1;
         if (end)
         {
            break;
         }
      }
   }
}

/// Positions a sprite for its battler.
/// @param pos  position
void place_sprite(uint8_t pos)
{
   const bt::FormatData& f = bt::format(g_battle);
   int x = f.x[pos] - kSpriteCenterOffset;
   int y = f.y[pos] - kSpriteCenterOffset;
   if (g_nudge[pos] > 0)
   {
      x += near_side(pos) ? kNudgePixels : -kNudgePixels;   // toward the other side
   }
   if (g_drop[pos] >= 0)
   {
      y += g_drop[pos] * kDropPixelsPerFrame;
   }
   gfx::sprites::set_position(battler_slot(pos), static_cast<int16_t>(x), static_cast<int16_t>(y));
}

/// Loads the sprite of the battler on a position.
/// @param pos  position
void load_sprite(uint8_t pos)
{
   const uint8_t i = g_battle.at[pos];
   if (i == bt::kBench)
   {
      gfx::sprites::show(battler_slot(pos), false);
      return;
   }
   const bt::SpeciesData& sp = bt::kData.species[g_battle.battlers[i].species];
   const asset::Sprite* s = asset::sprite(near_side(pos) ? sp.back : sp.front);
   if (s == nullptr)
   {
      gfx::sprites::show(battler_slot(pos), false);
      return;
   }
   gfx::sprites::load(battler_slot(pos), *s);
   gfx::sprites::set_priority(battler_slot(pos), kSpritePriority);
   g_drop[pos] = -1;
   g_nudge[pos] = 0;
   place_sprite(pos);
   gfx::sprites::show(battler_slot(pos), true);
}

// -- starting battles ---------------------------------------------------------------------------------

/// Starts a battle and switches to the battle screen.
/// @param su  setup
void begin_battle(const bt::Setup& su)
{
   if (!bt::start(g_battle, su))
   {
      return;
   }
   g_mode = Mode::Battle;
   g_ui = Ui::Events;
   g_text_timer = 0;
   setup_screen(true);
   for (uint8_t i = 0; i < bt::kMaxBattlers; ++i)
   {
      g_shown_hp[i] = g_battle.battlers[i].hp;
      g_target_hp[i] = g_battle.battlers[i].hp;
      g_shown_status[i] = g_battle.battlers[i].status;
   }
   for (uint8_t p = 0; p < bt::kMaxPositions; ++p)
   {
      g_drop[p] = -1;
      g_nudge[p] = 0;
   }
   clear_box();
}

void draw_boxes();

/// Starts the chosen scenario.
/// @param row  menu row
void start_scenario(int row)
{
   if (row == bt::kData.party_count + EX_HEAL)
   {
      heal_party();
      draw_menu();
      return;
   }
   if (row == bt::kData.party_count + EX_BOXES)
   {
      g_mode = Mode::Boxes;
      g_box = 0;
      draw_boxes();
      return;
   }
   if (!party_ready())
   {
      heal_party();
   }
   bt::Setup su{};
   su.seed = rng::gameplay.state ^ kBattleSeedMix;
   su.weather = bt::kNone8;
   su.participants[0] = bt::Participant{nullptr, &g_party, nullptr, bt::Controller::Human, 0, 0, 0};
   su.participants[0].bag = &g_bag;   // G3: the bag's items, and room in the boxes for a recruit
   su.storage = g_boxes != &g_no_boxes ? g_boxes : nullptr;
   su.count = kParticipants;
   if (row < bt::kData.party_count)
   {
      su.format = row == g_game->double_party ? g_game->double_format : g_game->single_format;
      su.participants[1] = bt::Participant{&bt::kData.parties[row], nullptr, nullptr, bt::Controller::Ai, 1, 0, 0};
   }
   else
   {
      g_unowned = monster::Party{};
      const int extra = row - bt::kData.party_count;
      const int count = extra == EX_UNOWNED          ? 1
                        : extra == EX_UNOWNED_DOUBLE ? kUnownedDouble
                                                     : static_cast<int>(g_game->exhibition_count);
      for (int k = 0; k < count; ++k)
      {
         const auto sp = static_cast<uint16_t>(rng::below(rng::gameplay, bt::kData.species_count));
         const auto lv =
            static_cast<uint8_t>(rng::between(rng::gameplay, g_game->unowned_min_level, g_game->unowned_max_level));
         if (bt::make_monster(g_unowned.members[g_unowned.count], sp, lv, rng::gameplay))
         {
            ++g_unowned.count;
         }
      }
      su.format = extra == EX_UNOWNED          ? g_game->single_format
                  : extra == EX_UNOWNED_DOUBLE ? g_game->double_format
                                               : g_game->exhibition_format;
      su.participants[1] =
         bt::Participant{nullptr, &g_unowned, nullptr, bt::Controller::Ai, 1, 1u << bt::PARTY_UNOWNED, 0};
      if (extra == EX_EXHIBITION)
      {
         // Exhibition: a copy of the party fought by the AI (the real party is untouched).
         g_ai_party = g_party;
         heal(g_ai_party);
         su.participants[0] = bt::Participant{nullptr, &g_ai_party, nullptr, bt::Controller::Ai, 0, 0, 0};
      }
   }
   begin_battle(su);
}

// -- battle: events -------------------------------------------------------------------------------

/// Shows "NAME used ITEM!" for an Item action (G3; the item's own logic says what it does).
/// @param e  the UseItem event
void show_use(const bt::Event& e)
{
   text::Code line[kUseTextCodes];
   int n = 0;
   const auto append = [&line, &n](const text::Code* codes)
   {
      for (int k = 0; codes != nullptr && codes[k] != text::kCodeEnd && n < kUseTextCodes - 1; ++k)
      {
         line[n++] = codes[k];
      }
   };
   append(e.battler < g_battle.battler_count ? g_battle.battlers[e.battler].name : nullptr);
   append(text::encode(" used ").codes);
   append(e.a < bt::kData.item_count ? bt::kData.items[e.a].name : nullptr);
   append(text::encode("!").codes);
   line[n] = text::kCodeEnd;
   show_message(line);
}

/// A recruit moment (G3): a check wobbles the creature's sprite, a recruit takes it off the field.
/// @param e  the Recruit event
/// @return frames to wait
int present_recruit(const bt::Event& e)
{
   const uint8_t pos = e.battler < g_battle.battler_count ? g_battle.battlers[e.battler].position : bt::kBench;
   if (pos >= bt::kMaxPositions)
   {
      return 0;
   }
   if (e.extra == static_cast<uint8_t>(bt::RecruitMoment::Check))
   {
      g_nudge[pos] = static_cast<int8_t>(kNudgeFrames);
      return kWobbleFrames;
   }
   if (e.extra == static_cast<uint8_t>(bt::RecruitMoment::Recruited))
   {
      gfx::sprites::show(battler_slot(pos), false);
      draw_hud(pos);
   }
   return 0;
}

/// Applies one presentation event.
/// @param e  event
/// @return frames to wait before the next event (0 = go on at once)
int present(const bt::Event& e)
{
   switch (e.kind)
   {
   case bt::EventKind::Message:
   case bt::EventKind::StatusText:
   case bt::EventKind::WeatherText:
   case bt::EventKind::PresenterText:
   case bt::EventKind::Text:
   {
      text::Code buf[kEventTextCodes];
      if (!bt::format_text(g_battle, e, buf, kEventTextCodes))
      {
         return 0;
      }
      show_message(buf);
      return g_fast ? kFastFrames : kTextFrames;
   }
   case bt::EventKind::UseMove:
   {
      const uint8_t pos = g_battle.battlers[e.battler].position;
      if (pos < bt::kMaxPositions)
      {
         g_nudge[pos] = kNudgeFrames;
      }
      return 0;
   }
   case bt::EventKind::Hp:
      g_target_hp[e.battler] = e.a;
      return 0;
   case bt::EventKind::Status:
      g_shown_status[e.battler] = static_cast<uint8_t>(e.a);
      draw_huds();
      return 0;
   case bt::EventKind::Faint:
      if (e.target < bt::kMaxPositions)
      {
         g_drop[e.target] = 0;
      }
      return kFaintFrames;
   case bt::EventKind::SwitchIn:
      g_shown_hp[e.battler] = g_battle.battlers[e.battler].hp;
      g_target_hp[e.battler] = g_shown_hp[e.battler];
      g_shown_status[e.battler] = g_battle.battlers[e.battler].status;
      if (e.target < bt::kMaxPositions)
      {
         load_sprite(e.target);
      }
      draw_huds();
      return kSwitchFrames;
   case bt::EventKind::SwitchOut:
      if (e.target < bt::kMaxPositions)
      {
         gfx::sprites::show(battler_slot(e.target), false);
      }
      return kSwitchFrames;
   case bt::EventKind::Weather:
   case bt::EventKind::Level:
      draw_huds();
      return 0;
   case bt::EventKind::Presenter:
      bt::presenter_view::begin(g_battle, e);   // battle_frame() waits while it plays
      return 0;
   case bt::EventKind::UseItem:
      show_use(e);
      return g_fast ? kFastFrames : kTextFrames;
   case bt::EventKind::Recruit:
      return present_recruit(e);
   default:
      return 0;
   }
}

/// Moves shown HP toward the engine's values.
/// @return true while a bar is still moving
bool animate_bars()
{
   bool moving = false;
   for (uint8_t i = 0; i < g_battle.battler_count; ++i)
   {
      if (g_shown_hp[i] == g_target_hp[i])
      {
         continue;
      }
      const int max = bt::max_hp(g_battle.battlers[i]);
      const int stepv = max / kBarPixels > 1 ? max / kBarPixels : 1;   // about one bar pixel per frame
      if (g_shown_hp[i] < g_target_hp[i])
      {
         g_shown_hp[i] =
            static_cast<uint16_t>(g_shown_hp[i] + stepv > g_target_hp[i] ? g_target_hp[i] : g_shown_hp[i] + stepv);
      }
      else
      {
         g_shown_hp[i] =
            static_cast<uint16_t>(g_shown_hp[i] - stepv < g_target_hp[i] ? g_target_hp[i] : g_shown_hp[i] - stepv);
      }
      moving = true;
   }
   if (moving)
   {
      draw_huds();
   }
   return moving;
}

/// Sprite motion: nudges and faint slides.
void animate_sprites()
{
   for (uint8_t p = 0; p < bt::format(g_battle).positions; ++p)
   {
      if (g_nudge[p] > 0)
      {
         --g_nudge[p];
      }
      if (g_drop[p] >= 0 && g_drop[p] < kFaintFrames)
      {
         ++g_drop[p];
         if (g_drop[p] == kFaintFrames)
         {
            gfx::sprites::show(battler_slot(p), false);
            draw_hud(p);
         }
      }
      if (g_battle.at[p] != bt::kBench)
      {
         place_sprite(p);
      }
   }
}

// -- battle: menus ------------------------------------------------------------------------------------

/// Draws the command menu.
void draw_command()
{
   clear_box();
   text::Code name[kCommandNameChars + 1];
   clip(name, g_battle.battlers[g_request.battler].name, kCommandNameChars);
   int x = put_ascii(INK_DIM, 1, kBoxLine1, "What will");
   x = put(INK_TITLE, x + 1, kBoxLine1, name);
   put_ascii(INK_DIM, x, kBoxLine1, " do?");
   put_pools(g_battle.battlers[g_request.battler], kCommandCol0, kCommandPoolsRow);
   constexpr const char* kItems[kCommandItems] = {"FIGHT", "BAG", "PARTY", "RUN"};
   for (int k = 0; k < kCommandItems; ++k)
   {
      const int cx = kCommandCol0 + k * kCommandColStep;
      put_ascii(k == g_cursor ? INK_ON : INK_DIM, cx, kBoxLine2, k == g_cursor ? ">" : " ");
      put_ascii(k == g_cursor ? INK_TITLE : INK_DIM, cx + 1, kBoxLine2, kItems[k]);
   }
}

/// Draws the move menu: the cursor's page of four moves, then the cursor's move (its slot's first move resource
/// pool, its battler costs, type and power).
void draw_fight()
{
   clear_box();
   const bt::Battler& b = g_battle.battlers[g_request.battler];
   const int first = (g_cursor / kMovesPerPage) * kMovesPerPage;
   for (int k = first; k < first + kMovesPerPage && k < bt::move_slots(); ++k)
   {
      const int x = kMoveCol0 + (k % kMoveCols) * kMoveColStep;
      const int y = kBoxLine1 + ((k - first) / kMoveCols);
      if (b.moves[k] == bt::kNone16)
      {
         put_ascii(INK_OFF, x + 1, y, "-");
         continue;
      }
      text::Code name[kMoveNameChars + 1];
      clip(name, bt::kData.moves[b.moves[k]].name, kMoveNameChars);
      put_ascii(INK_ON, x, y, k == g_cursor ? ">" : " ");
      put(bt::move_usable(g_battle, g_request.battler, static_cast<uint8_t>(k)) ? (k == g_cursor ? INK_TITLE : INK_DIM)
                                                                                : INK_OFF,
          x + 1, y, name);
   }
   if (bt::move_slots() > kMovesPerPage)
   {
      int x = put_uint(INK_DIM, kPageCol, kBoxLine1, static_cast<uint32_t>(first / kMovesPerPage + 1));
      x = put_ascii(INK_DIM, x, kBoxLine1, "/");
      put_uint(INK_DIM, x, kBoxLine1, static_cast<uint32_t>((bt::move_slots() + kMovesPerPage - 1) / kMovesPerPage));
   }
   if (g_cursor < bt::move_slots() && b.moves[g_cursor] != bt::kNone16)
   {
      const bt::MoveData& m = bt::kData.moves[b.moves[g_cursor]];
      text::Code ty[kTypeNameBuffer];
      clip(ty, bt::kData.types[m.type].name, kTypeNameChars);
      // the slot's first move resource pool ("PP 10/15"), then the move's battler costs (D-25: "EP 5", "MP 8"; a
      // cost given by a formula shows "*"), or "free" for a move without costs
      int x = kMoveCol0 - kFieldGap;
      const uint8_t pool = shown_move_resource();
      if (pool != bt::kNone8)
      {
         x = put(INK_DIM, x + kFieldGap, kBoxLine2, bt::kData.resources[pool].label);
         x = put_uint(INK_ON, x + 1, kBoxLine2, static_cast<uint32_t>(bt::resource_value(b, pool, g_cursor)));
         x = put_ascii(INK_DIM, x, kBoxLine2, "/");
         x = put_uint(INK_ON, x, kBoxLine2, static_cast<uint32_t>(bt::resource_max(b, pool, g_cursor)));
      }
      for (int c = 0; c < m.cost_count; ++c)
      {
         const bt::MoveCost& cost = bt::kData.move_costs[m.cost_first + c];
         if (bt::kData.resources[cost.resource].scope == static_cast<uint8_t>(bt::ResourceScope::Move))
         {
            continue;   // a move resource's cost: its pool is shown above
         }
         x = put(INK_DIM, x + kFieldGap, kBoxLine2, bt::kData.resources[cost.resource].label);
         x = cost.formula == bt::kNone8 ? put_uint(INK_ON, x + 1, kBoxLine2, cost.amount)
                                        : put_ascii(INK_ON, x + 1, kBoxLine2, "*");
      }
      if (m.cost_count == 0)
      {
         x = put_ascii(INK_DIM, x + kFieldGap, kBoxLine2, "free");
      }
      x = put(INK_ON, x + kFieldGap, kBoxLine2, ty);
      if (m.power != 0)
      {
         x = put_ascii(INK_DIM, x + kFieldGap, kBoxLine2, "pow");
         put_uint(INK_ON, x + 1, kBoxLine2, m.power);
      }
   }
}

/// Positions a target menu row stands for.
/// @param mask  valid positions
/// @param row   row
/// @return position, or kBench
uint8_t target_at(uint8_t mask, int row)
{
   for (uint8_t p = 0; p < bt::kMaxPositions; ++p)
   {
      if (((mask >> p) & 1) != 0 && row-- == 0)
      {
         return p;
      }
   }
   return bt::kBench;
}

/// Number of valid targets.
/// @param mask  valid positions
/// @return count
int target_count(uint8_t mask)
{
   int n = 0;
   for (uint8_t p = 0; p < bt::kMaxPositions; ++p)
   {
      n += (mask >> p) & 1;
   }
   return n;
}

/// The target mask of the chosen move.
/// @return mask
uint8_t current_targets()
{
   const bt::Battler& b = g_battle.battlers[g_request.battler];
   return bt::target_choices(g_battle, g_request.battler, b.moves[g_move_slot]);
}

/// Draws the target menu.
void draw_target()
{
   clear_box();
   const uint8_t mask = current_targets();
   put_ascii(INK_DIM, 1, kBoxLine1, "Target:");
   for (int r = 0; r < target_count(mask) && r < bt::kMaxPositions; ++r)
   {
      const uint8_t p = target_at(mask, r);
      text::Code name[kTargetNameChars + 1];
      clip(name, g_battle.battlers[g_battle.at[p]].name, kTargetNameChars);
      const int x = kMoveCol0 + (r % kMoveCols) * kMoveColStep;
      const int y = kTargetTop + r / kMoveCols;
      put_ascii(INK_ON, x, y, r == g_cursor ? ">" : " ");
      put(r == g_cursor ? INK_TITLE : INK_DIM, x + 1, y, name);
      put_ascii(INK_OFF, x + kAllyTagOffset, y, near_side(p) ? "(a)" : "");
   }
}

/// Battlers the party menu lists (the requesting battler's party).
/// @param out       battler indices
/// @param capacity  entries of out (C13a: the list never runs past it, whatever the party's count says)
/// @return count
int party_list(uint8_t* out, int capacity)
{
   const bt::PartyState& t = g_battle.parties[g_battle.battlers[g_request.battler].party];
   int n = 0;
   for (int k = 0; k < t.count && n < capacity; ++k)
   {
      out[n++] = static_cast<uint8_t>(t.first_battler + k);
   }
   return n;
}

/// Draws the party menu (over the field).
void draw_party()
{
   uint8_t list[bt::kMaxBattlers];
   const int n = party_list(list, bt::kMaxBattlers);
   panel(kSwitchPanelOrigin, kSwitchPanelOrigin, kSwitchPanelCols, kSwitchExtraRows + n);
   for (int y = kSwitchPanelOrigin; y < kSwitchPanelOrigin + kSwitchExtraRows + n; ++y)
   {
      blank(kSwitchPanelOrigin, kSwitchPanelOrigin + kSwitchPanelCols, y);
   }
   put_ascii(INK_TITLE, kSwitchTextCol, kSwitchPanelOrigin,
             g_item != 0 ? "Use on which?"
             : g_request.kind == bt::RequestKind::Replace ? "Send in which?" : "Switch to which?");
   for (int k = 0; k < n; ++k)
   {
      const bt::Battler& b = g_battle.battlers[list[k]];
      const bool ok = b.position == bt::kBench && b.hp > 0;
      text::Code name[kSwitchNameChars + 1];
      clip(name, b.name, kSwitchNameChars);
      const int y = kSwitchListTop + k;
      put_ascii(INK_ON, kSwitchTextCol, y, k == g_cursor ? ">" : " ");
      put(ok ? (k == g_cursor ? INK_TITLE : INK_DIM) : INK_OFF, kSwitchNameCol, y, name);
      put_ascii(INK_DIM, kSwitchLCol, y, "L");
      put_uint(INK_ON, kSwitchLevelCol, y, b.level);
      put_uint(ok ? INK_ON : INK_OFF, kSwitchHpCol, y, b.hp, kHpDigits);
      put_ascii(INK_DIM, kSwitchSlashCol, y, "/");
      put_uint(INK_ON, kSwitchMaxHpCol, y, bt::max_hp(b));
      if (b.position != bt::kBench)
      {
         put_ascii(INK_OFF, kSwitchActiveCol, y, "*");
      }
   }
   clear_box();
   put_ascii(INK_DIM, 1, kBoxLine1, "A: choose  B: back");
}

/// Hides the party menu again.
void close_party()
{
   for (int y = kSwitchPanelOrigin; y < kSwitchPanelOrigin + kSwitchExtraRows + bt::kMaxBattlers && y < kBoxTop; ++y)
   {
      blank(0, kCols, y);
      for (int x = 0; x < kCols; ++x)
      {
         panel_cell(x, y, 0);
      }
   }
   draw_huds();
}

/// Opens the menu for the pending request.
void open_request()
{
   g_cursor = 0;
   g_item = 0;
   if (g_request.kind == bt::RequestKind::LearnMove || g_request.kind == bt::RequestKind::Choice)
   {
      // an effect's question this sandbox has no menu for: the first answer
      bt::choose(g_battle, g_request.battler, bt::Action{bt::ActionKind::Answer, 0, bt::kBench, 0});
      g_ui = Ui::Events;
      return;
   }
   if (g_request.kind == bt::RequestKind::Replace || g_request.kind == bt::RequestKind::SwitchTarget)
   {
      g_ui = Ui::Party;
      draw_party();
   }
   else
   {
      g_ui = Ui::Command;
      draw_command();
   }
}

/// Sends a choice to the engine; shows a refusal message if it is not allowed.
/// @param a  choice
/// @return true if accepted
bool send(const bt::Action& a)
{
   if (bt::choose(g_battle, g_request.battler, a))
   {
      g_ui = Ui::Events;
      clear_box();
      return true;
   }
   if (a.kind == bt::ActionKind::Run && !bt::can_escape(g_battle, g_request.battler))
   {
      const bt::Event e{
         bt::EventKind::Message, g_request.battler, bt::kBench, 0, static_cast<uint16_t>(bt::Msg::EscapeBlocked), 0};
      text::Code buf[kRefusalTextCodes];
      if (bt::format_text(g_battle, e, buf, kRefusalTextCodes))
      {
         show_message(buf);
      }
      g_text_timer = kTextFrames;
      g_ui = Ui::Events;   // the request is still pending: it reopens
   }
   return false;
}

// -- battle: the bag (G3) ----------------------------------------------------------------------------

/// The battle-usable items of the bag, in pocket order (at most kBagRows).
/// @param out  item ids
/// @return count
int bag_list(uint16_t* out)
{
   int n = 0;
   for (uint8_t p = 0; p < inventory::kInventory.pocket_count; ++p)
   {
      int used = 0;
      const inventory::BagSlot* slots = inventory::pocket_slots(g_bag, p, used);
      for (int k = 0; k < used && n < kBagRows; ++k)
      {
         const inventory::ItemInfo* in = inventory::info(slots[k].item);
         if (in != nullptr && (in->use == inventory::Use::Battle || in->use == inventory::Use::Both))
         {
            out[n++] = slots[k].item;
         }
      }
   }
   return n;
}

/// Positions with a battler on them (an item's battler target).
/// @return mask
uint8_t occupied_positions()
{
   uint8_t mask = 0;
   for (uint8_t p = 0; p < bt::format(g_battle).positions; ++p)
   {
      mask = static_cast<uint8_t>(g_battle.at[p] != bt::kBench ? mask | (1u << p) : mask);
   }
   return mask;
}

/// The Item action of the aimed item.
/// @param target   a battler target's position (kBench: none)
/// @param battler  a member target's battler (else the user)
/// @return the action
bt::Action item_action(uint8_t target, uint8_t battler)
{
   bt::Action a{bt::ActionKind::Item, 0, target, battler};
   a.item = g_item;
   return a;
}

/// Draws the bag menu (over the field): the usable items and their counts.
void draw_bag()
{
   uint16_t list[kBagRows];
   const int n = bag_list(list);
   const int rows = n > 0 ? n : 1;
   panel(kSwitchPanelOrigin, kSwitchPanelOrigin, kSwitchPanelCols, kSwitchExtraRows + rows);
   for (int y = kSwitchPanelOrigin; y < kSwitchPanelOrigin + kSwitchExtraRows + rows; ++y)
   {
      blank(kSwitchPanelOrigin, kSwitchPanelOrigin + kSwitchPanelCols, y);
   }
   put_ascii(INK_TITLE, kSwitchTextCol, kSwitchPanelOrigin, "Use which item?");
   if (n == 0)
   {
      put_ascii(INK_OFF, kBagNameCol, kSwitchListTop, "(nothing to use)");
   }
   for (int k = 0; k < n; ++k)
   {
      const bool ok = bt::item_usable(g_battle, g_request.battler, list[k]);
      text::Code name[kBagNameBuffer];
      clip(name, bt::kData.items[list[k]].name, kBagNameChars);
      const int y = kSwitchListTop + k;
      put_ascii(INK_ON, kSwitchTextCol, y, k == g_cursor ? ">" : " ");
      put(ok ? (k == g_cursor ? INK_TITLE : INK_DIM) : INK_OFF, kBagNameCol, y, name);
      const int x = put_ascii(INK_DIM, kBagCountCol, y, "x");
      put_uint(INK_ON, x, y, inventory::count(g_bag, list[k]));
   }
   clear_box();
   put_ascii(INK_DIM, 1, kBoxLine1, "A: use  B: back");
}

/// Draws the battler target menu of the aimed item (every battler on the field).
void draw_item_target()
{
   close_party();
   clear_box();
   const uint8_t mask = occupied_positions();
   put_ascii(INK_DIM, 1, kBoxLine1, "Use on:");
   for (int r = 0; r < target_count(mask) && r < bt::kMaxPositions; ++r)
   {
      const uint8_t p = target_at(mask, r);
      text::Code name[kTargetNameChars + 1];
      clip(name, g_battle.battlers[g_battle.at[p]].name, kTargetNameChars);
      const int x = kMoveCol0 + (r % kMoveCols) * kMoveColStep;
      const int y = kTargetTop + r / kMoveCols;
      put_ascii(INK_ON, x, y, r == g_cursor ? ">" : " ");
      put(r == g_cursor ? INK_TITLE : INK_DIM, x + 1, y, name);
      put_ascii(INK_OFF, x + kAllyTagOffset, y, near_side(p) ? "(a)" : "");
   }
}

/// One frame of the bag menu: choose an item, then its target by its kind (a battler on the field, a party member,
/// nothing: the user).
/// @param pad   input
/// @param up    Up this frame
/// @param down  Down this frame
void bag_input(const Pad& pad, bool up, bool down)
{
   uint16_t list[kBagRows];
   const int n = bag_list(list);
   if (up && g_cursor > 0)
   {
      --g_cursor;
   }
   if (down && g_cursor + 1 < n)
   {
      ++g_cursor;
   }
   if (up || down)
   {
      draw_bag();
   }
   if (pad.hit(KEY_B))
   {
      close_party();
      g_ui = Ui::Command;
      g_cursor = 1;
      draw_command();
      return;
   }
   if (!pad.hit(KEY_A) || n == 0 || !bt::item_usable(g_battle, g_request.battler, list[g_cursor]))
   {
      return;
   }
   g_item = list[g_cursor];
   const inventory::ItemInfo* in = inventory::info(g_item);
   g_cursor = 0;
   if (in->target == inventory::Target::Battler)
   {
      g_ui = Ui::ItemTarget;
      draw_item_target();
   }
   else if (in->target == inventory::Target::Member || in->target == inventory::Target::MoveSlot)
   {
      // a move slot target uses the member's first slot (the sandbox's simplification)
      close_party();
      g_ui = Ui::Party;
      draw_party();
   }
   else if (send(item_action(bt::kBench, g_request.battler)))
   {
      close_party();
   }
}

/// One frame of a menu.
/// @param pad  input
void menu_input(const Pad& pad)
{
   const bool up = pad.hit(KEY_UP) || pad.repeat(KEY_UP);
   const bool down = pad.hit(KEY_DOWN) || pad.repeat(KEY_DOWN);
   const bool left = pad.hit(KEY_LEFT) || pad.repeat(KEY_LEFT);
   const bool right = pad.hit(KEY_RIGHT) || pad.repeat(KEY_RIGHT);
   switch (g_ui)
   {
   case Ui::Command:
      if (left && g_cursor > 0)
      {
         --g_cursor;
      }
      if (right && g_cursor < kCommandItems - 1)
      {
         ++g_cursor;
      }
      if (left || right)
      {
         draw_command();
      }
      if (pad.hit(KEY_A))
      {
         if (g_cursor == kCommandFight)
         {
            // No usable move: struggle straight away.
            bool any = false;
            for (uint8_t k = 0; k < bt::move_slots(); ++k)
            {
               any = any || bt::move_usable(g_battle, g_request.battler, k);
            }
            if (!any)
            {
               send(bt::Action{bt::ActionKind::Move, bt::kFallbackSlot, bt::kBench, 0});
               return;
            }
            g_ui = Ui::Fight;
            g_cursor = 0;
            draw_fight();
         }
         else if (g_cursor == kCommandBag)
         {
            g_ui = Ui::Bag;
            g_cursor = 0;
            draw_bag();
         }
         else if (g_cursor == kCommandParty)
         {
            g_ui = Ui::Party;
            g_item = 0;
            g_cursor = 0;
            draw_party();
         }
         else
         {
            send(bt::Action{bt::ActionKind::Run, 0, bt::kBench, 0});
         }
      }
      break;
   case Ui::Fight:
      // Moves are a grid two wide (four to a page): up / down move a row (past a page's edge to the next page),
      // left / right a column.
      if (up && g_cursor >= kMoveCols)
      {
         g_cursor -= kMoveCols;
      }
      if (down && g_cursor + kMoveCols < bt::move_slots())
      {
         g_cursor += kMoveCols;
      }
      if (left && (g_cursor & 1) != 0)
      {
         --g_cursor;
      }
      if (right && (g_cursor & 1) == 0)
      {
         ++g_cursor;
      }
      if (up || down || left || right)
      {
         draw_fight();
      }
      if (pad.hit(KEY_B))
      {
         g_ui = Ui::Command;
         g_cursor = 0;
         draw_command();
      }
      else if (pad.hit(KEY_A) && bt::move_usable(g_battle, g_request.battler, static_cast<uint8_t>(g_cursor)))
      {
         g_move_slot = static_cast<uint8_t>(g_cursor);
         const uint8_t mask = current_targets();
         if (target_count(mask) > 1)
         {
            g_ui = Ui::Target;
            g_cursor = 0;
            draw_target();
         }
         else
         {
            send(bt::Action{bt::ActionKind::Move, g_move_slot, target_at(mask, 0), 0});
         }
      }
      break;
   case Ui::Target:
   {
      const int n = target_count(current_targets());
      if ((left || up) && g_cursor > 0)
      {
         --g_cursor;
      }
      if ((right || down) && g_cursor + 1 < n)
      {
         ++g_cursor;
      }
      if (up || down || left || right)
      {
         draw_target();
      }
      if (pad.hit(KEY_B))
      {
         g_ui = Ui::Fight;
         g_cursor = g_move_slot;
         draw_fight();
      }
      else if (pad.hit(KEY_A))
      {
         send(bt::Action{bt::ActionKind::Move, g_move_slot, target_at(current_targets(), g_cursor), 0});
      }
      break;
   }
   case Ui::Party:
   {
      uint8_t list[bt::kMaxBattlers];
      const int n = party_list(list, bt::kMaxBattlers);
      if (up && g_cursor > 0)
      {
         --g_cursor;
      }
      if (down && g_cursor + 1 < n)
      {
         ++g_cursor;
      }
      if (up || down)
      {
         draw_party();
      }
      if (pad.hit(KEY_B) && g_request.kind == bt::RequestKind::Action)
      {
         close_party();
         g_ui = g_item != 0 ? Ui::Bag : Ui::Command;
         g_cursor = g_item != 0 ? 0 : kCommandParty;
         if (g_item != 0)
         {
            draw_bag();
         }
         else
         {
            draw_command();
         }
      }
      else if (pad.hit(KEY_A))
      {
         // an item's member (G3); an effect's SwitchTarget request takes the replacement as its answer
         bt::Action a = g_request.kind == bt::RequestKind::SwitchTarget
                           ? bt::Action{bt::ActionKind::Answer, list[g_cursor], bt::kBench, 0}
                           : bt::Action{bt::ActionKind::Switch, 0, bt::kBench, list[g_cursor]};
         if (g_item != 0)
         {
            a = item_action(bt::kBench, list[g_cursor]);
         }
         if (send(a))
         {
            close_party();
         }
      }
      break;
   }
   case Ui::Bag:
      bag_input(pad, up, down);
      break;
   case Ui::ItemTarget:
   {
      const int n = target_count(occupied_positions());
      if ((left || up) && g_cursor > 0)
      {
         --g_cursor;
      }
      if ((right || down) && g_cursor + 1 < n)
      {
         ++g_cursor;
      }
      if (up || down || left || right)
      {
         draw_item_target();
      }
      if (pad.hit(KEY_B))
      {
         g_ui = Ui::Bag;
         g_cursor = 0;
         draw_bag();
      }
      else if (pad.hit(KEY_A))
      {
         send(item_action(target_at(occupied_positions(), g_cursor), g_request.battler));
      }
      break;
   }
   default:
      break;
   }
}

/// One frame of a battle.
/// @param pad  input
void battle_frame(const Pad& pad)
{
   if (pad.hit(KEY_SELECT))
   {
      g_fast = !g_fast;
   }
   animate_sprites();
   if (g_ui == Ui::Result)
   {
      if (pad.hit(KEY_A))
      {
         g_mode = Mode::Menu;
         g_cursor = 0;
         setup_screen(false);
         draw_menu();
      }
      return;
   }
   if (g_ui != Ui::Events)
   {
      if (pad.hit(KEY_START))
      {   // leave the battle (sandbox convenience)
         g_mode = Mode::Menu;
         setup_screen(false);
         draw_menu();
         return;
      }
      menu_input(pad);
      return;
   }
   if (g_text_timer > 0)
   {
      g_text_timer -= pad.down(KEY_A) ? kTextSpeedUp : 1;
      if (pad.hit(KEY_A))
      {
         g_text_timer = 0;
      }
      return;
   }
   if (animate_bars() || bt::presenter_view::busy())
   {
      return;
   }
   for (int guard = 0; guard < kMaxEventsPerFrame; ++guard)
   {
      bt::Event e;
      if (bt::pop_event(g_battle, e))
      {
         g_text_timer = present(e);
         if (g_text_timer > 0 || bt::presenter_view::busy())
         {
            return;
         }
         continue;
      }
      const bt::Progress p = bt::step(g_battle);
      if (p == bt::Progress::Ended && g_battle.ev_count == 0)
      {
         g_ui = Ui::Result;
         constexpr const char* kOutcome[] = {"", "Victory!", "Defeat...", "Draw.", "Escaped.", "Recruited!"};
         static_assert(sizeof(kOutcome) / sizeof(kOutcome[0]) == static_cast<int>(bt::Outcome::NUM_OUTCOMES));
         clear_box();
         put_ascii(INK_TITLE, 1, kBoxLine1, kOutcome[static_cast<int>(g_battle.outcome)]);
         put_ascii(INK_DIM, 1, kBoxLine2, "A: back to the menu");
         if (!party_ready())
         {
            heal_party();
         }
         return;
      }
      if (p == bt::Progress::Input && g_battle.ev_count == 0)
      {
         if (bt::request(g_battle, g_request))
         {
            open_request();
         }
         return;
      }
   }
}

// -- storage boxes page (G3) -------------------------------------------------------------------------

/// Draws the storage boxes page: the box's name and its records (name and level), two columns of slots.
void draw_boxes()
{
   clear_console();
   clear_panels();
   put_ascii(INK_TITLE, 1, 0, "STORAGE BOXES");
   if (storage::kStorage.box_count == 0)
   {
      put_ascii(INK_OFF, 1, kBoxesTop, "(this game has no boxes)");
      controls(text::encode("B back").codes, text::encode("").codes);
      return;
   }
   text::Code name[kBoxNameChars + 1];
   clip(name, storage::box_name(g_box), kBoxNameChars);
   int x = put(INK_ON, 1, 1, name);
   x = put_ascii(INK_DIM, x + 1, 1, "(");
   x = put_uint(INK_ON, x, 1, static_cast<uint32_t>(storage::box_count(*g_boxes, g_box)));
   x = put_ascii(INK_DIM, x, 1, "/");
   x = put_uint(INK_ON, x, 1, storage::kStorage.box_size);
   put_ascii(INK_DIM, x, 1, ")");
   for (int k = 0; k < storage::kStorage.box_size && k < kBoxesRows * kMoveCols; ++k)
   {
      const int slot = g_box * storage::kStorage.box_size + k;
      const int cx = 1 + (k / kBoxesRows) * kBoxesColStep;
      const int cy = kBoxesTop + k % kBoxesRows;
      monster::Record rec{};
      if (!storage::read(*g_boxes, slot, rec))
      {
         put_ascii(INK_OFF, cx, cy, "-");
         continue;
      }
      const uint16_t sp = bt::find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
      text::Code sname[kSpeciesNameBuffer];
      clip(sname, sp != bt::kNone16 ? bt::kData.species[sp].name : nullptr, kBoxesNameChars);
      put(INK_ON, cx, cy, sname);
      const int lx = put_ascii(INK_DIM, cx + kBoxesLevelOffset, cy, "L");
      put_uint(INK_ON, lx, cy, monster::get(rec, monster::Role::Level));
   }
   controls(text::encode("L/R box  B back").codes, text::encode("").codes);
}

/// One frame of the storage boxes page.
/// @param pad  input
void boxes_frame(const Pad& pad)
{
   const int boxes = storage::kStorage.box_count;
   if (boxes > 0 && (pad.hit(KEY_R) || pad.hit(KEY_RIGHT)))
   {
      g_box = (g_box + 1) % boxes;
      draw_boxes();
   }
   else if (boxes > 0 && (pad.hit(KEY_L) || pad.hit(KEY_LEFT)))
   {
      g_box = (g_box + boxes - 1) % boxes;
      draw_boxes();
   }
   else if (pad.hit(KEY_B))
   {
      g_mode = Mode::Menu;
      draw_menu();
   }
}

/// One frame of the scenario menu.
/// @param pad  input
void menu_frame(const Pad& pad)
{
   if (pad.hit(KEY_UP) || pad.repeat(KEY_UP))
   {
      g_cursor = (g_cursor + menu_rows() - 1) % menu_rows();
      draw_menu();
   }
   else if (pad.hit(KEY_DOWN) || pad.repeat(KEY_DOWN))
   {
      g_cursor = (g_cursor + 1) % menu_rows();
      draw_menu();
   }
   else if (pad.hit(KEY_A))
   {
      start_scenario(g_cursor);
   }
}

} // namespace

int capsule::battle_kit::run(const GameConfig& game)
{
   g_game = &game;
   hal::system::init();
   gfx::palette::reset();
   gfx::sprites::reset();
   gfx::background::reset();
   make_party();
   restock_bag();
   g_boxes = game.boxes != nullptr ? game.boxes : &g_no_boxes;
   storage::clear(*g_boxes);
   setup_screen(false);
   draw_menu();
   Pad pad;
   while (hal::system::update())
   {
      rng::next16(rng::gameplay);   // A-4: the gameplay stream advances every frame
      pad.update();
      if (g_mode == Mode::Menu)
      {
         menu_frame(pad);
      }
      else if (g_mode == Mode::Boxes)
      {
         boxes_frame(pad);
      }
      else
      {
         battle_frame(pad);
         bt::presenter_view::frame();
      }
      gfx::sprites::update();
   }
   return 0;
}
