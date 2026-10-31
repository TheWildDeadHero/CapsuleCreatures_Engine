/// @file
/// Map sandbox: overworld traversal (capsule::map + capsule::field) on the
/// sandbox world, without the rest of a game.
///
/// Laid out like a game: the engine library, reference/project.json, data/
/// (world.json with its tilesets and maps, assets.json with the walker
/// sprite; shapes only) and this scene code. The world's maps put their
/// tile layers on BG1-BG3 (in any priority order: the showcase map, where
/// the sandbox starts, puts its ground on BG1 behind the other two) and use
/// character blocks 0-1 and palette banks 0-11; the HUD lives on BG0
/// (character block 2, screen block 31, banks 12-15).
///
/// Controls while walking:
/// - D-pad: walk (a new direction turns first); hold B to run.
/// - A: talk to the person or read the sign in front (B-21, B-3).
/// - SELECT: HUD on / off (hidden at start). L: collision view. R: noclip (walk through anything).
/// - START: menu (Up/Down, A: go to the chosen map, night on / off (the
///   world's encounter condition "night", D-12), the next vehicle class (A2:
///   class 1, swim, crosses water at 2 pixels a frame and cannot run) or back
///   to the start; B: close; SELECT: the About page, the sample's credits,
///   which A, B, SELECT or START close).
///
/// Map events (B-3), objects (B-21) and eventing (D-17) run in the engine's
/// overworld runner (capsule/overworld/overworld.h), the way a game runs
/// them: this scene calls overworld::start() and, each frame,
/// overworld::update() with the pad, which runs scripts (the walker waits),
/// warps with their transitions, parallel object scripts, screen effects,
/// the timer and the location banner. Messages, yes / no, menus and number
/// entry show in the engine's text box (src/script/script_port_hal.cpp) on
/// BG0, which the map plan moves to character block 2 and screen block 31;
/// the HUD steps aside while a script, transition or banner uses BG0.
/// The step hook "pedometer" counts every 16th step and asks for a message
/// every 64 steps.
///
/// Field text placeholders (D-22 decision 6): the scripts manifest declares
/// {player} (a name slot) and {berries} (a variable); this scene registers
/// script::placeholder_text() as the field resolver and names the player at
/// start, so the welcome message and the hidden berry show the name (the
/// welcome message also has a page break, the berry message a prompt).
///
/// Map objects (B-21): the showcase's people talk on A (the guide also sends
/// the gatekeeper off the meadow exit), one wanders, one paces, and a
/// spotter on the path spots the walker within 4 cells, walks up and talks
/// (no battle here); afterwards its page gives it another line. Objects
/// use sprite slots 1-15; the walker has slot 0.
///
/// Eventing (D-17): a jogger on a custom move route (talk to race the timer
/// to the meadow), a box with a self switch, a show-off with shake, flash
/// and tint, a lookout who pans the camera by the number you enter, and an
/// invisible puddle (through, player touch). Locations: Showcase Park,
/// Sunny Meadow (a sign-shaped banner) and Echo Cave (a preview on the first
/// visit); the cave warps use a mosaic and a vertical wipe, the meadow warp
/// a horizontal wipe.
///
/// Encounters (D-12): grass rolls the map's tables as the walker
/// arrives; there is no battle here, so the encounter is shown on the HUD
/// (species, level, method, slot) and taken at once, and the walker goes on.
///
/// Encounter leads and timed field effects (F1, D-27): the sandbox carries the
/// Starter's party (Breezle, whose talent Quick Feet raises the encounter rate
/// by half while it leads, and Sproutle) and three Ward charms. A LeadScope
/// attaches the lead rules to the rolls; the HUD shows the lead (row 2) and
/// the Ward's steps left, and a debug line (row 16) the last roll: its odds,
/// the creature picked and why the roll ended (no trigger, skipped below the
/// Ward's level, picked). The menu uses a Ward charm (its use_field logic
/// starts the timed field effect `ward`: 120 steps, nothing below the lead's
/// level meets the walker; its expiry message shows when it ends) and swaps
/// the lead. An encounter's creature is made (make_encounter(): the lead's
/// encounter_made hooks) and shown, then dropped.
///
/// Field actions, darkness, flight points and the escape point (F2, D-27
/// section 3): the party also has Tidepup (Tide Slam: the Starter's wade
/// action) and Glimmet (Mend Light: the light action); Sproutle's Vine Lash
/// is the clear action's. A battle::ActionScope attaches the dispatcher: A
/// facing the pond asks "The water is calm. Wade across?" (yes: swim, one
/// step onto the water; stepping back onto land walks again), A facing the
/// bush at 12,9 asks to clear it (it goes until the map is entered again).
/// The cave is dark (level 3: a lit box around the walker); its sign says
/// whether anyone could light it (`field_action available`), and the menu's
/// "field action" row runs the menu actions usable on the map (Light, a
/// dark-tagged map's action). The showcase and the meadow are escape
/// points: walking into the cave records one, and the menu's "escape" row
/// returns there. Entering a map records its flight point; the menu's
/// "flight menu" row lists the recorded ones and flies to the one picked.
/// The HUD's blocked row shows DARK n on a dark map.
///
/// Step effects (F3, D-27 section 4): Sproutle starts Blighted with 6 HP. A
/// battle::StepScope attaches the party to the Starter's step effects: every 4
/// steps the blight takes 1 HP (at 0 the survive rule keeps 1 and the blight
/// fades: its message), every 64 steps each member's bond grows by 1. The
/// HUD's step row (5) shows the steps to the next blight run, Sproutle's HP
/// and status, Breezle's bond and the steps to the next bond run; rows 6-7
/// the last message a run queued. The menu's "back to start" row blights
/// Sproutle again.
///
/// Combination (B1, D-28 section 1): the menu's "merge altar" row runs the script merge_altar, which opens the
/// Starter's Merge Altar (`combine open merge_altar`) through the engine's combine port (attached to the party and
/// the lent State): the text box's menu picks two members of level 5 or more ("Combine" is not offered: the altar
/// takes exactly two), the altar asks "Breezle + Tidepup -> Glimmet?" (the type table: wind and water give light,
/// the lowest light species that is a base stage), and yes merges them: both inputs leave, the result takes the
/// first one's party slot at their average level with three of their genes, the first one's bond, its type line's
/// combine move and up to two of their moves. Sproutle (level 3) is not ready; Breezle + Glimmet (wind and light)
/// have no rule. The HUD's lead row shows the new party.
///
/// Nursery and eggs (B2, D-28 sections 2 and 3): the party also has Duskowl (wing kin, like Breezle), and Sproutle's
/// hidden talent is Nurture (eggs hatch in half the steps). The menu's "grove" row runs the script grove: with fewer
/// than two left there it opens the party menu (`nursery pick`) and leaves the one picked (`nursery deposit`); with
/// two it offers the first back (`nursery withdraw`); once a check found the pair compatible (same kin: 50% every
/// 128 steps) it gives the egg (`nursery take`). An egg in the party hatches after 5 cycles of 256 steps (640 steps
/// with Nurture in the party): the hatch script says so and asks for a nickname (`nursery name`: yes / no, then the
/// letter menu). The menu's "fast steps" row counts each step 64 times (a debug option for the screenshots). The
/// HUD's grove row (8) shows how many records the Grove holds of its slots, the steps to its next check, the odds
/// over them and whether an egg waits; C9: its held row (12) names the records it holds, slot order ("held Breezle
/// + Duskowl": the first one's base stage is the egg's species); the party rows (9-11) each member, an egg with the
/// steps it still needs.
///
/// Collision view: red = blocked (collision or border), blue = not passable
/// on foot, yellow = moves the walker (ice, ledge, push), green = encounter;
/// the hex digit is the cell's elevation.
#include "test_kit.h"

#include "capsule/asset/asset.h"
#include "capsule/battle/battle.h"
#include "capsule/battle/data.h"
#include "capsule/battle/encounter_lead.h"
#include "capsule/battle/field_actions.h"
#include "capsule/battle/step_effects.h"
#include "capsule/core/memory.h"
#include "capsule/core/random.h"
#include "capsule/encounter/encounter.h"
#include "capsule/field/field.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/system.h"
#include "capsule/inventory/bag.h"
#include "capsule/inventory/use.h"
#include "capsule/map/map.h"
#include "capsule/overworld/overworld.h"
#include "capsule/combine/eggs.h"
#include "capsule/combine/nursery.h"
#include "capsule/script/combine_port.h"
#include "capsule/script/nursery_port.h"
#include "capsule/script/field_actions.h"
#include "capsule/script/field_effects.h"
#include "capsule/script/script.h"
#include "capsule/script/step_effects.h"
#include "capsule/text/encode.h"
#include "capsule/text/placeholder.h"

#include "battle_ids.h"
#include "encounter_ids.h"
#include "script_ids.h"
#include "world_ids.h"

#include <cstdint>

namespace
{

using namespace capsule;
using namespace capsule::testkit;
namespace video = hal::video;
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

/// HUD placement: away from the map's VRAM and palettes.
constexpr ConsoleLayout kHudLayout = {2, 31, 12, false};
constexpr video::TileIndex kOverlayTile = 128;   ///< First collision-view tile (character block 2).
constexpr uint8_t kOverlayBank = 12;          ///< Palette bank of the overlay colors (entries 3-6).
constexpr uint32_t kOverlayFirstEntry = 3u;   ///< Palette entry of the first overlay color in its bank.
constexpr uint32_t kNibbleRepeat = 0x11111111u;   ///< Times a 4-bit color fills all 8 pixels of a 4bpp row.
constexpr int32_t kNoOverlay = 0x7FFFFFFF;    ///< g_overlay_x before any overlay is drawn (forces a redraw).
constexpr video::TileIndex kFirstGlyphCode = 0x20;   ///< Character code of the font's glyph 0 (space).

/// @name Collision view geometry
/// The view draws each metatile as 2x2 BG0 tiles (elevation digit, then the
/// category color) on BG0's 32x32-tile map, which holds 16x16 metatiles and
/// scrolls with the camera.
///@{
constexpr int kMetatileShift = 4;          ///< Pixels to metatiles: shift by log2(16).
constexpr int kTilesPerMetatile = 2;       ///< BG0 tiles along a metatile side.
constexpr int32_t kRingMetatileMask = 15;  ///< Metatile position to BG0 map position (16 per side).
constexpr int kBg0PixelMask = 255;         ///< BG0 scroll wraps at 256 pixels (its 32x32-tile map).
constexpr int32_t kViewMetatileRows = 11;  ///< Metatile rows drawn: the screen's 10 and one partly shown.
constexpr int32_t kViewMetatileCols = 16;  ///< Metatile columns drawn: the screen's 15 and one partly shown.
constexpr int kLayerBg0 = 0x01;            ///< Blend mask bit: BG0 (the overlay).
constexpr int kLayerBg1To3 = 0x0E;         ///< Blend mask bits: BG1-BG3 (the map).
constexpr uint8_t kOverlayAlpha = 9;       ///< Overlay weight, in 16ths.
constexpr uint8_t kMapAlpha = 7;           ///< Map weight under the overlay, in 16ths.
///@}

/// @name HUD layout
///@{
constexpr int kCellDigits = 7;       ///< Hex digits of the cell value shown (bits 0-26 are used).
constexpr int kHudLeadRow = 2;       ///< Row of the lead (F1).
constexpr int kHudWardRow = 3;       ///< Row of the Ward's steps and the roll's skip level (F1).
constexpr int kHudRollRow = 16;      ///< Row of the last roll's debug line (F1).
constexpr int kHudStepRow = 5;       ///< Row of the step effects (F3).
constexpr int kHudStepTextRow = 6;   ///< First row of the last step message (F3).
constexpr int kStepTextRows = 2;     ///< Rows the step message may take.
constexpr int kStepTextCodes = 96;   ///< Codes a formatted step message keeps.
constexpr int kHudGroveRow = 8;      ///< Row of the Grove (B2).
constexpr int kHudPartyRow = 9;      ///< First row of the party (B2: two members a row, three rows).
constexpr int kPartyPerRow = 2;      ///< Members per party row.
constexpr int kPartyColumn = 15;     ///< Columns per member on a party row.
constexpr int kPartyNameCells = 10;  ///< Cells of a member's name on a party row.
constexpr int kPartyRows = 3;        ///< Rows of the party.
constexpr int kHudGroveHeldRow = 12;  ///< C9: row of the records the Grove holds (their names, slot order).
constexpr int kHudEventRow = 17;     ///< Row of the last event, berries and pedometer.
constexpr int kHudEncounterRow = 18;      ///< Row of the last encounter.
constexpr int kHudBlockedRow = 19;   ///< Row of the last blocked reason and the mode flags.
constexpr int kFlagGap = 2;          ///< Columns before each mode flag ("NOCLIP", "NIGHT").
///@}

/// @name Menu layout
///@{
constexpr int kExtraMenuRows = 11;   ///< Menu rows after the maps: "night", "vehicle", "ward", "lead", "field action",
                                     ///< "flight menu", "escape", "merge altar", "grove", "fast steps", "back to start".
constexpr int kVehicleMenuRow = 1;   ///< Extra row of the vehicle class (after "night").
constexpr int kWardMenuRow = 2;      ///< Extra row of "use a Ward charm" (F1).
constexpr int kLeadMenuRow = 3;      ///< Extra row of "swap the lead" (F1).
constexpr int kActionMenuRow = 4;    ///< Extra row of the first menu field action usable here (F2).
constexpr int kFlightMenuRow = 5;    ///< Extra row of "flight menu" (F2).
constexpr int kEscapeMenuRow = 6;    ///< Extra row of "escape" (F2).
constexpr int kMergeMenuRow = 7;     ///< Extra row of "merge altar" (B1).
constexpr int kGroveMenuRow = 8;     ///< Extra row of "grove" (B2).
constexpr int kFastMenuRow = 9;      ///< Extra row of "fast steps" (B2: a debug option).
constexpr int kStartMenuRow = 10;    ///< Extra row of "back to start".
constexpr int kMenuActions = 4;      ///< Menu field actions looked at (F2; the row runs the first).
constexpr int kMenuTitleRow = 1;     ///< Row of the menu title.
constexpr int kMenuTop = 3;          ///< Row of the first map (B2: one row higher, the menu grew by two).
constexpr int kMenuItemCol = 3;      ///< Column of the menu items, after the cursor.
constexpr int kShownCol = 18;        ///< Column of "(shown)".
constexpr int kMenuHintRow1 = 18;    ///< Row of the first controls hint (B2: two rows, the menu grew by two).
constexpr int kMenuHintRow2 = 19;    ///< Row of the second controls hint.
constexpr int kHintMargin = 1;       ///< D-40: columns kept free right of the "SELECT about" hint.
///@}

/// @name About page (D-40: the sample's credits, SELECT on the menu)
///@{
constexpr char kAboutHint[] = "SELECT about";   ///< The menu's hint, right-aligned on its title row.
/// Column of the hint (kAboutHint without its NUL, kHintMargin from the right edge).
constexpr int kAboutHintCol = kCols - static_cast<int>(sizeof(kAboutHint) - 1) - kHintMargin;
constexpr int kAboutTop = 3;         ///< Row of the first About line.
constexpr int kAboutCol = 1;         ///< Column of the About lines.
/// The About lines: the engine's licence and the credit libgba asks of every GBA program (as the Starter's).
constexpr const char* kAbout[] = {"The Map Sandbox sample of the",
                                  "CapsuleCreatures engine",
                                  "(MPL-2.0).",
                                  "",
                                  "Based in part on the work of",
                                  "devkitPro",
                                  "(https://devkitpro.org).",
                                  "",
                                  "Desktop builds use SDL2",
                                  "(https://libsdl.org)."};
constexpr int kAboutLines = static_cast<int>(sizeof(kAbout) / sizeof(kAbout[0]));   ///< Its lines.
static_assert(kAboutTop + kAboutLines < kMenuHintRow2, "the About lines must end above the controls hint");
///@}

/// @name The party and bag (F1: the Starter's creatures leading the encounter rolls)
///@{
constexpr uint16_t kLeadSpecies = battle_ids::SPECIES_BREEZLE;    ///< The first lead (talent Quick Feet).
constexpr uint16_t kOtherSpecies = battle_ids::SPECIES_SPROUTLE;  ///< The second member (no encounter talent).
constexpr uint8_t kLeadLevel = 5;        ///< Breezle's level: the showcase grass has levels 3 to 6 (the Ward skips 3 and 4).
constexpr uint8_t kOtherLevel = 3;       ///< Sproutle's.
constexpr uint16_t kWaderSpecies = battle_ids::SPECIES_TIDEPUP;   ///< F2: knows Tide Slam (the wade action).
constexpr uint16_t kLightSpecies = battle_ids::SPECIES_GLIMMET;   ///< F2: knows Mend Light (the light action).
constexpr uint8_t kFieldLevel = 5;       ///< Their level.
constexpr uint8_t kWaderSlot = 2;        ///< Tidepup's party slot.
constexpr uint8_t kLightSlot = 3;        ///< Glimmet's.
constexpr uint16_t kWingSpecies = battle_ids::SPECIES_DUSKOWL;    ///< B2: Breezle's kin (the Grove's pair).
constexpr uint8_t kWingSlot = 4;         ///< Duskowl's party slot.
constexpr uint8_t kHiddenSlot = 2;       ///< B2: the hidden passive slot (Sproutle's Nurture).
constexpr uint8_t kMembers = 5;          ///< Party members.
constexpr uint32_t kFastSteps = 64;      ///< B2: "fast steps" counts each step this many times.
constexpr uint32_t kPartySeed = 0x51ED;  ///< Stream seed of the members' random parts.
constexpr uint16_t kWardCharms = 3;      ///< Ward charms in the bag at start.
constexpr int kNameCells = 10;           ///< Cells of a creature or talent name on the lead row.
constexpr int kRollNameCells = 6;        ///< Cells of the creature's name on the roll row (it fits kCols).
constexpr int kTextCodes = 32;           ///< Codes of a name copied from the battle data.
constexpr uint8_t kBlightSlot = 1;       ///< F3: Sproutle, blighted at start (and by "back to start").
constexpr uint32_t kBlightHp = 6;        ///< Its hit points then.
///@}

constexpr int kChunkColMask = map::kChunkMetatiles - 1;   ///< Cell index in a chunk to its column.
constexpr int kChunkRowShift = 3;                         ///< Cell index in a chunk to its row: log2(8).
constexpr uint32_t kPedometerMessageMask = 3;             ///< Pedometer asks for a message every 4th call.

/// Collision-view categories (overlay tile kOverlayTile + n).
enum Overlay : uint8_t
{
   OV_BLOCKED,
   OV_TERRAIN,
   OV_MOVES,
   OV_ENCOUNTER,
   NUM_OVERLAYS
};

/// What the app is doing (scripts and transitions run inside Walking, in the runner).
enum class Mode : uint8_t
{
   Walking,
   Menu,
   About   ///< D-40: the About page (the sample's credits), opened from the menu.
};

/// Whether the HUD shows at start (hidden, so the sample opens on the plain map; SELECT shows it).
constexpr bool kHudAtStart = false;

Mode g_mode = Mode::Walking;   ///< Current mode.
bool g_hud = kHudAtStart;      ///< HUD shown.
bool g_collision = false;      ///< Collision view shown.
int g_map = 0;                 ///< Index of the shown map in map::kWorld.
int g_cursor = 0;              ///< Menu row.
int32_t g_overlay_x = kNoOverlay;   ///< Metatile column the overlay was drawn for.
int32_t g_overlay_y = 0;            ///< Metatile row the overlay was drawn for.
bool g_fast = false;                ///< B2: "fast steps" is on.
uint32_t g_fast_seen = 0;           ///< B2: the walker's steps fast steps counted so far.
uint32_t g_encounters = 0;          ///< Encounters met.
bool g_has_last = false;            ///< g_last holds an encounter.
encounter::Result g_last{};         ///< The last encounter.
uint8_t g_last_slot = 0;            ///< Slot of the cell it happened on.
uint32_t g_pedometer = 0;           ///< Step hook calls (every 16 steps).
bool g_hud_up = false;              ///< BG0 holds the HUD (not the runner's banner, box or timer).

CAPSULE_EWRAM_BSS battle::State g_field;   ///< The State the lead rules and item uses lend their field contexts.
CAPSULE_EWRAM_BSS monster::Party g_party;  ///< The player's party (F1: its lead leads the rolls).
CAPSULE_EWRAM_BSS monster::Party g_wild;   ///< The creature an encounter made (shown, then dropped).
CAPSULE_EWRAM_BSS inventory::Bag g_bag;    ///< The bag (Ward charms).
bool g_has_made = false;                   ///< g_wild holds an encounter's creature.
bool g_made_skipped = false;               ///< The last encounter's creature was skipped by an encounter_made hook.
const char* g_note = "";                   ///< The last menu action's note (HUD event row).
CAPSULE_EWRAM_BSS text::Code g_step_text[kStepTextCodes];   ///< F3: the last message a step run queued (fresh_party()
                                                            ///< empties it).

/// Display name of a map.
/// @param i  map index
/// @return its debug name, or "map"
const char* map_name(int i)
{
   return map::kWorld.map_names != nullptr ? map::kWorld.map_names[i] : "map";
}

/// Loads the overlay tiles and colors (solid 8x8 tiles in entries 3-6).
void load_overlay()
{
   constexpr video::Color kColors[NUM_OVERLAYS] = {video::rgb5(31, 4, 4), video::rgb5(6, 10, 31),
                                                   video::rgb5(31, 28, 0), video::rgb5(4, 28, 8)};
   gfx::palette::load_bg(static_cast<uint8_t>(kOverlayBank * kColorsPerBank + kOverlayFirstEntry), kColors,
                         NUM_OVERLAYS);
   for (uint8_t n = 0; n < NUM_OVERLAYS; ++n)
   {
      uint32_t tile[kTilePixels];   // one word per 4bpp pixel row
      const uint32_t nib = kOverlayFirstEntry + n;
      for (uint32_t& row : tile)
      {
         row = nib * kNibbleRepeat;
      }
      video::load_bg_tiles(kHudLayout.char_block, static_cast<video::TileIndex>(kOverlayTile + n), tile, 1);
   }
}

/// Collision-view category of a cell for the walker.
/// @param m  the map
/// @param x  column
/// @param y  row
/// @return Overlay category, or NUM_OVERLAYS for nothing to show
uint8_t category(const map::Map& m, int x, int y)
{
   const map::MapCell c = map::cell_at(m, x, y);
   if (map::cell_collision(c) != 0)
   {
      return OV_BLOCKED;
   }
   const map::Behavior& b = map::behavior(c);
   if ((b.passable & 1u) == 0)
   {
      return OV_TERRAIN;
   }
   if (b.kind != map::BehaviorKind::None)
   {
      return OV_MOVES;
   }
   if (map::cell_encounter(c))
   {
      return OV_ENCOUNTER;
   }
   return NUM_OVERLAYS;
}

/// Draws the collision view around the camera (whole view, when the camera
/// reached a new metatile).
void draw_overlay()
{
   const map::Map& m = *map::current();
   const int32_t mx0 = map::camera_x() >> kMetatileShift;
   const int32_t my0 = map::camera_y() >> kMetatileShift;
   video::set_bg_scroll(kConsoleBg, static_cast<int16_t>(map::camera_x() & kBg0PixelMask),
                        static_cast<int16_t>(map::camera_y() & kBg0PixelMask));
   if (mx0 == g_overlay_x && my0 == g_overlay_y)
   {
      return;
   }
   g_overlay_x = mx0;
   g_overlay_y = my0;
   constexpr char kHex[] = "0123456789ABCDEF";
   for (int32_t my = my0; my < my0 + kViewMetatileRows; ++my)
   {
      for (int32_t mx = mx0; mx < mx0 + kViewMetatileCols; ++mx)
      {
         const int tx = static_cast<int>(mx & kRingMetatileMask) * kTilesPerMetatile;
         const int ty = static_cast<int>(my & kRingMetatileMask) * kTilesPerMetatile;
         const uint8_t cat = category(m, static_cast<int>(mx), static_cast<int>(my));
         const video::MapCell fill =
            cat < NUM_OVERLAYS ? video::map_cell(static_cast<video::TileIndex>(kOverlayTile + cat), kOverlayBank)
                               : video::map_cell(0, kOverlayBank);   // glyph 0 = space
         const uint8_t e = map::cell_elevation(map::cell_at(m, static_cast<int>(mx), static_cast<int>(my)));
         const auto digit = static_cast<video::TileIndex>(kHex[e] - kFirstGlyphCode);
         video::write_map_cell(kConsoleBg, tx, ty, video::map_cell(digit, kHudLayout.first_bank + INK_TITLE));
         video::write_map_cell(kConsoleBg, tx + 1, ty, fill);
         video::write_map_cell(kConsoleBg, tx, ty + 1, fill);
         video::write_map_cell(kConsoleBg, tx + 1, ty + 1, fill);
      }
   }
}

/// Sets BG0 up for the HUD, the collision view or nothing.
void setup_bg0()
{
   init_console(kHudLayout);
   load_overlay();
   video::set_bg_scroll(kConsoleBg, 0, 0);
   g_overlay_x = kNoOverlay;
   if (g_collision && g_mode == Mode::Walking)
   {
      video::set_blend(video::BlendMode::Alpha, kLayerBg0, kLayerBg1To3 | video::kLayerObj | video::kLayerBackdrop);
      video::set_blend_alpha(kOverlayAlpha, kMapAlpha);
      draw_overlay();
   }
   else
   {
      video::set_blend(video::BlendMode::Off, 0, 0);
   }
   video::configure_bg(kConsoleBg, g_hud || g_collision || g_mode == Mode::Menu, 0, kHudLayout.char_block,
                       kHudLayout.screen_block, false, 0);
}

/// Name of a Blocked value.
/// @param b  the value
/// @return text
const char* blocked_name(field::Blocked b)
{
   switch (b)
   {
   case field::Blocked::Collision:
      return "collision";
   case field::Blocked::Elevation:
      return "elevation";
   case field::Blocked::Terrain:
      return "terrain";
   case field::Blocked::Ledge:
      return "ledge";
   default:
      return "-";
   }
}

/// Name of a Motion value.
/// @param m  the value
/// @return text
const char* motion_name(field::Motion m)
{
   switch (m)
   {
   case field::Motion::Turning:
      return "turn";
   case field::Motion::Walking:
      return "walk";
   case field::Motion::Jumping:
      return "jump";
   case field::Motion::Sliding:
      return "slide";
   case field::Motion::Pushed:
      return "pushed";
   default:
      return "stand";
   }
}

/// Name of the last request's source.
/// @return text
const char* last_event()
{
   switch (overworld::last_source())
   {
   case field::Source::Warp:
      return "warp";
   case field::Source::Trigger:
      return "trigger";
   case field::Source::Sign:
      return "sign";
   case field::Source::StepHook:
      return "step hook";
   case field::Source::Object:
      return "talk";
   case field::Source::Spotter:
      return "spotter";
   case field::Source::Touch:
      return "touch";
   case field::Source::Autorun:
      return "autorun";
   default:
      return "-";
   }
}

/// Prints a battle data name, clipped.
/// @param ink    ink
/// @param x      column
/// @param y      row
/// @param name   the name's codes, or nullptr
/// @param cells  most cells it takes
/// @return the column after it
int put_name(Ink ink, int x, int y, const text::Code* name, int cells = kNameCells)
{
   text::Code out[kTextCodes];
   int n = 0;
   for (; name != nullptr && name[n] != text::kCodeEnd && n < cells && n < kTextCodes - 1; ++n)
   {
      out[n] = name[n];
   }
   out[n] = text::kCodeEnd;
   return put(ink, x, y, out);
}

/// The name of how a roll ended.
/// @param e  the end
/// @return text
const char* end_name(battle::EncounterEnd e)
{
   switch (e)
   {
   case battle::EncounterEnd::NoTrigger:
      return "miss";
   case battle::EncounterEnd::Blocked:
      return "block";
   case battle::EncounterEnd::NoEntry:
      return "none";
   case battle::EncounterEnd::Skipped:
      return "skip";
   case battle::EncounterEnd::Picked:
      return "met";
   default:
      return "-";
   }
}

/// The lead row (the lead's species, level and talent) and the Ward row (its steps left, the last roll's skip level;
/// F1).
void draw_lead_row()
{
   clear_rows(kHudLeadRow, kHudWardRow);
   const uint8_t lead = battle::lead_of(g_party);
   int x = put_ascii(INK_DIM, 0, kHudLeadRow, "lead");
   if (lead == battle::kBench)
   {
      x = put_ascii(INK_OFF, x + 1, kHudLeadRow, "none");
   }
   else
   {
      const monster::Record& rec = g_party.members[lead];
      const uint16_t sp = battle::find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
      x = put_name(INK_TITLE, x + 1, kHudLeadRow, sp < battle::kData.species_count ? battle::kData.species[sp].name
                                                                                    : nullptr);
      x = put_ascii(INK_DIM, x + 1, kHudLeadRow, "L");
      x = put_uint(INK_ON, x, kHudLeadRow, monster::get(rec, monster::Role::Level));
      const uint8_t passive = sp < battle::kData.species_count ? battle::kData.species[sp].passives[0] : battle::kNone8;
      x = put_name(INK_DIM, x + 1, kHudLeadRow, passive < battle::kData.passive_count
                                                   ? battle::kData.passives[passive].name : nullptr);
   }
   x = put_ascii(INK_DIM, 0, kHudWardRow, "Ward");
   const int32_t left = script::field_effects::remaining(battle_ids::FIELD_EFFECT_WARD);
   if (left > 0)
   {
      x = put_int(INK_TITLE, x + 1, kHudWardRow, left);
      x = put_ascii(INK_DIM, x + 1, kHudWardRow, "steps");
   }
   else
   {
      x = put_ascii(INK_OFF, x + 1, kHudWardRow, "off");
   }
   const battle::EncounterTrace& t = battle::last_encounter();
   if (t.skip_below > 0)
   {
      x = put_ascii(INK_DIM, x + kFlagGap, kHudWardRow, "skip<");
      put_uint(INK_ON, x, kHudWardRow, t.skip_below);
   }
}

/// F3: the step row (the steps to the next blight run, Sproutle's HP and status, Breezle's bond and the steps to the
/// next bond run) and the last message a run queued (two rows).
void draw_step_rows()
{
   clear_rows(kHudStepRow, kHudStepTextRow + kStepTextRows - 1);
   int x = put_ascii(INK_DIM, 0, kHudStepRow, "blight");
   x = put_int(INK_ON, x + 1, kHudStepRow, script::step_effects::steps_left(battle_ids::STEP_EFFECT_BLIGHT));
   const monster::Record& rec = g_party.members[kBlightSlot];
   x = put_ascii(INK_DIM, x + 1, kHudStepRow, "hp");
   x = put_uint(INK_ON, x + 1, kHudStepRow, monster::get(rec, monster::Role::CurrentHp));
   // the status role's low bits are the status (the Starter keeps no counter above them)
   const bool blighted = monster::get(rec, monster::Role::PrimaryStatus) == battle_ids::STATUS_BLIGHT;
   x = put_ascii(blighted ? INK_TITLE : INK_OFF, x + 1, kHudStepRow, blighted ? "BLT" : "ok");
   x = put_ascii(INK_DIM, x + 1, kHudStepRow, "bond");
   x = put_uint(INK_ON, x + 1, kHudStepRow, monster::get(g_party.members[0], battle_ids::ROLE_BOND));
   x = put_ascii(INK_DIM, x + 1, kHudStepRow, "in");
   put_int(INK_ON, x + 1, kHudStepRow, script::step_effects::steps_left(battle_ids::STEP_EFFECT_BOND));
   // the message, kCols cells a row
   int at = 0;
   for (int row = 0; row < kStepTextRows && g_step_text[at] != text::kCodeEnd; ++row)
   {
      text::Code line[kCols + 1];
      int n = 0;
      for (; n < kCols && g_step_text[at] != text::kCodeEnd; ++n, ++at)
      {
         line[n] = g_step_text[at];
      }
      line[n] = text::kCodeEnd;
      put(INK_TITLE, 0, kHudStepTextRow + row, line);
   }
}

/// B2: a record's name on the HUD (the eggs' name for an egg, its nickname, else its species').
/// @param rec  the record
/// @return the name, or nullptr
const text::Code* shown_name(const monster::Record& rec)
{
   static text::Code nick[monster::kMaxNicknameCodes + 1];
   if (combine::is_egg(rec))
   {
      return combine::egg_name();
   }
   int n = 0;
   for (; n < monster::count(monster::Role::Nickname) && n < monster::kMaxNicknameCodes; ++n)
   {
      const auto code = static_cast<text::Code>(monster::get(rec, monster::Role::Nickname, n));
      if (code == text::kCodeEnd || code == 0)
      {
         break;
      }
      nick[n] = code;
   }
   nick[n] = text::kCodeEnd;
   if (n > 0)
   {
      return nick;
   }
   const uint16_t sp = battle::find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
   return sp < battle::kData.species_count ? battle::kData.species[sp].name : nullptr;
}

/// B2: the Grove row (how many records it holds, the steps to its next check, the method's odds over them, whether
/// an egg waits), C9: the held row (the records' names) and the party rows (each member: its name and level, an egg
/// with the steps it still needs).
void draw_grove_rows()
{
   namespace cn = combine::nursery;
   clear_rows(kHudGroveRow, kHudPartyRow + kPartyRows - 1);
   int x = put_ascii(INK_DIM, 0, kHudGroveRow, "grove");
   x = put_uint(INK_ON, x + 1, kHudGroveRow, cn::count());
   x = put_ascii(INK_DIM, x, kHudGroveRow, "/");
   x = put_uint(INK_ON, x, kHudGroveRow, cn::slot_count());
   x = put_ascii(INK_DIM, x + 1, kHudGroveRow, "chk");
   x = put_int(INK_ON, x + 1, kHudGroveRow, script::step_effects::steps_left(battle_ids::STEP_EFFECT_GROVE_CHECK));
   x = put_ascii(INK_DIM, x + 1, kHudGroveRow, "odds");
   x = put_int(INK_ON, x + 1, kHudGroveRow, cn::odds());
   put_ascii(cn::result_ready() ? INK_TITLE : INK_OFF, x + 1, kHudGroveRow, cn::result_ready() ? "EGG!" : "-");
   // C9: the records it holds by name, in slot order ("held A + B"; "-" for a free slot)
   clear_rows(kHudGroveHeldRow, kHudGroveHeldRow);
   x = put_ascii(INK_DIM, 0, kHudGroveHeldRow, "held");
   for (uint8_t s = 0; s < cn::slot_count(); ++s)
   {
      if (s > 0)
      {
         x = put_ascii(INK_DIM, x + 1, kHudGroveHeldRow, "+");
      }
      const monster::Record* held = cn::record(s);
      x = held != nullptr ? put_name(INK_ON, x + 1, kHudGroveHeldRow, shown_name(*held), kPartyNameCells)
                          : put_ascii(INK_OFF, x + 1, kHudGroveHeldRow, "-");
   }
   for (uint8_t k = 0; k < g_party.count; ++k)
   {
      const monster::Record& rec = g_party.members[k];
      const int px = (k % kPartyPerRow) * kPartyColumn;
      const int py = kHudPartyRow + k / kPartyPerRow;
      const bool egg = combine::is_egg(rec);
      int at = put_name(egg ? INK_TITLE : INK_ON, px, py, shown_name(rec), kPartyNameCells) + 1;
      if (egg)
      {
         put_uint(INK_DIM, at, py, combine::hatch_left(rec));
      }
      else
      {
         at = put_ascii(INK_DIM, at, py, "L");
         put_uint(INK_DIM, at, py, monster::get(rec, monster::Role::Level));
      }
   }
}

/// The roll row: the last roll's odds, scale, skip level, pick and end (F1's debug line).
void draw_roll_row()
{
   clear_rows(kHudRollRow, kHudRollRow);
   const battle::EncounterTrace& t = battle::last_encounter();
   int x = put_ascii(INK_DIM, 0, kHudRollRow, "roll");
   if (t.end == battle::EncounterEnd::None)
   {
      put_ascii(INK_OFF, x + 1, kHudRollRow, "-");
      return;
   }
   x = put_uint(INK_ON, x + 1, kHudRollRow, t.chance);
   x = put_ascii(INK_DIM, x, kHudRollRow, "/");
   x = put_uint(INK_ON, x, kHudRollRow, t.out_of);
   if (t.species < battle::kData.species_count)
   {
      x = put_name(INK_TITLE, x + 1, kHudRollRow, battle::kData.species[t.species].name, kRollNameCells);
      x = put_ascii(INK_DIM, x + 1, kHudRollRow, "L");
      x = put_uint(INK_ON, x, kHudRollRow, t.level);
   }
   // a picked creature the encounter_made hooks turned away reads "gone"
   const bool gone = t.end == battle::EncounterEnd::Picked && g_made_skipped;
   put_ascii(t.end == battle::EncounterEnd::Picked && !gone ? INK_TITLE : INK_DIM, x + 1, kHudRollRow,
             gone ? "gone" : end_name(t.end));
}

/// DS1: the HUD's sections, one redrawn a frame in turn (every one each kHudSections frames): redrawing all of them
/// in one frame took longer than a GBA frame, so the sandbox ran at half speed with the HUD on (E1: no input poll on
/// 232 of 600 frames).
enum class HudSection : uint8_t
{
   Position,    ///< Rows 0-1: map, cell, elevation, behaviour, motion, steps.
   Lead,        ///< The lead and Ward rows (F1).
   Steps,       ///< The step rows (F3).
   Grove,       ///< The Grove, held and party rows (B2, C9).
   Roll,        ///< The roll row (F1).
   Events,      ///< The event row, the note and the encounter row.
   Blocked,     ///< The blocked row and the mode flags.
   NUM_SECTIONS ///< Number of sections.
};
uint8_t g_hud_section = 0;   ///< DS1: the section the next draw_hud() redraws.

/// The position rows (0-1).
void draw_position_rows()
{
   const field::Walker& w = field::walker();
   const map::Map& m = *map::current();
   const map::MapCell c = map::cell_at(m, w.x, w.y);
   clear_rows(0, 1);
   int x = put_ascii(INK_TITLE, 0, 0, map_name(g_map));
   x = put_ascii(INK_DIM, x + 1, 0, "x");
   x = put_int(INK_ON, x, 0, w.x);
   x = put_ascii(INK_DIM, x + 1, 0, "y");
   x = put_int(INK_ON, x, 0, w.y);
   x = put_ascii(INK_DIM, x + 1, 0, "z");
   x = put_uint(INK_ON, x, 0, w.elevation);
   x = put_ascii(INK_DIM, x + 1, 0, "cell");
   put_hex(INK_ON, x, 0, c, kCellDigits);

   const uint8_t b = map::cell_behavior(c);
   const bool named = map::kWorld.behavior_names != nullptr && b < map::kWorld.behavior_count;
   x = put_ascii(INK_ON, 0, 1, named ? map::kWorld.behavior_names[b] : "?");
   x = put_ascii(INK_DIM, x + 1, 1, motion_name(w.motion));
   x = put_ascii(INK_DIM, x + 1, 1, "steps");
   x = put_uint(INK_ON, x, 1, w.steps);
   x = put_ascii(INK_DIM, x + 1, 1, "enc");
   put_uint(INK_ON, x, 1, w.encounter_steps);
}

/// The event, note and encounter rows.
void draw_event_rows()
{
   clear_rows(kHudEventRow, kHudEncounterRow);
   int x = put_ascii(INK_DIM, 0, kHudEventRow, "event");
   x = put_ascii(INK_ON, x + 1, kHudEventRow, last_event());
   x = put_ascii(INK_DIM, x + 1, kHudEventRow, "berries");
   x = put_int(INK_ON, x + 1, kHudEventRow, script::var(script_ids::VAR_BERRIES));
   x = put_ascii(INK_DIM, x + 1, kHudEventRow, "ped");
   put_uint(INK_ON, x + 1, kHudEventRow, g_pedometer);
   put_ascii(INK_TITLE, 0, kHudWardRow + 1, g_note);

   const encounter::Bank& eb = encounter::bank();
   x = put_ascii(INK_DIM, 0, kHudEncounterRow, "enc");
   x = put_uint(INK_ON, x + 1, kHudEncounterRow, g_encounters);
   if (g_has_last)
   {
      const bool sp_named = eb.species_names != nullptr && g_last.species < eb.species_name_count;
      x = put_ascii(INK_TITLE, x + 1, kHudEncounterRow, sp_named ? eb.species_names[g_last.species] : "?");
      x = put_ascii(INK_DIM, x + 1, kHudEncounterRow, "L");
      x = put_uint(INK_ON, x, kHudEncounterRow, g_last.level);
      const bool md_named = eb.method_names != nullptr && g_last.method < eb.method_count;
      x = put_ascii(INK_DIM, x + 1, kHudEncounterRow, md_named ? eb.method_names[g_last.method] : "?");
      x = put_ascii(INK_DIM, x + 1, kHudEncounterRow, "slot");
      put_uint(INK_ON, x, kHudEncounterRow, g_last_slot);
   }
}

/// The blocked row and the mode flags.
void draw_blocked_row()
{
   const field::Walker& w = field::walker();
   clear_rows(kHudBlockedRow, kHudBlockedRow);
   int x = put_ascii(INK_DIM, 0, kHudBlockedRow, "blocked");
   x = put_ascii(INK_ON, x + 1, kHudBlockedRow, blocked_name(w.last_blocked));
   if (w.noclip)
   {
      x = put_ascii(INK_TITLE, x + kFlagGap, kHudBlockedRow, "NOCLIP");
   }
   if ((encounter::conditions() & (1u << encounter_ids::ENCOUNTER_CONDITION_NIGHT)) != 0)
   {
      x = put_ascii(INK_TITLE, x + kFlagGap, kHudBlockedRow, "NIGHT");
   }
   if (overworld::darkness() != 0)
   {   // F2: the shown map's darkness level
      x = put_ascii(INK_TITLE, x + kFlagGap, kHudBlockedRow, "DARK");
      put_uint(INK_ON, x + 1, kHudBlockedRow, overworld::darkness());
   }
}

/// Redraws one section of the HUD (DS1: the next in turn; all of them with every_section, after the HUD's layer was
/// set up).
/// @param every_section  redraw every section now
void draw_hud(bool every_section = false)
{
   constexpr auto kSections = static_cast<uint8_t>(HudSection::NUM_SECTIONS);
   for (uint8_t k = 0; k < (every_section ? kSections : 1); ++k)
   {
      switch (static_cast<HudSection>(g_hud_section))
      {
      case HudSection::Position:
         draw_position_rows();
         break;
      case HudSection::Lead:
         draw_lead_row();
         break;
      case HudSection::Steps:
         draw_step_rows();
         break;
      case HudSection::Grove:
         draw_grove_rows();
         break;
      case HudSection::Roll:
         draw_roll_row();
         break;
      case HudSection::Events:
         draw_event_rows();
         break;
      default:
         draw_blocked_row();
         break;
      }
      g_hud_section = static_cast<uint8_t>((g_hud_section + 1) % kSections);
   }
}

/// F2: the first menu field action a member may use on the shown map.
/// @param[out] out  it
/// @return false when none
bool first_menu_action(battle::MenuAction& out)
{
   battle::MenuAction found[kMenuActions] = {};
   if (battle::menu_actions(g_party, static_cast<uint16_t>(g_map), found, kMenuActions) == 0)
   {
      return false;
   }
   out = found[0];
   return true;
}

/// F2: leaves the menu and runs a script in the overworld runner (a field action, the flight menu, the escape).
/// @param id  ScriptId
void run_from_menu(uint16_t id)
{
   g_mode = Mode::Walking;
   if (id == script::kNoScriptId || !overworld::run_script(id, field::kNoObject))
   {
      g_note = id == script::kNoScriptId ? g_note : "busy";
      setup_bg0();
      g_hud_up = true;
      return;
   }
   // BG0 is the runner's from here (a menu the script opens at once is drawn now: the HUD must not clear it later)
   clear_console();
   video::set_blend(video::BlendMode::Off, 0, 0);
   g_hud_up = false;
}

/// Menu rows: one per map, then "night", "vehicle", "use Ward charm", "swap the lead", "field action",
/// "flight menu", "escape", "merge altar" (B1), "grove", "fast steps" (B2), then "back to start".
/// @return row count
int menu_rows()
{
   return map::kWorld.map_count + kExtraMenuRows;
}

/// Draws the menu.
void draw_menu()
{
   clear_console();
   put_ascii(INK_TITLE, 1, kMenuTitleRow, "MAP SANDBOX");
   put_ascii(INK_OFF, kAboutHintCol, kMenuTitleRow, kAboutHint);   // D-40: the About page
   for (int i = 0; i < map::kWorld.map_count; ++i)
   {
      put_ascii(i == g_cursor ? INK_ON : INK_DIM, kMenuItemCol, kMenuTop + i, map_name(i));
      if (i == g_map)
      {
         put_ascii(INK_OFF, kShownCol, kMenuTop + i, "(shown)");
      }
   }

   // The extra rows follow the maps after one blank row.
   const int n = map::kWorld.map_count;
   const int night_row = kMenuTop + n + 1;
   const bool night = (encounter::conditions() & (1u << encounter_ids::ENCOUNTER_CONDITION_NIGHT)) != 0;
   put_ascii(g_cursor == n ? INK_ON : INK_DIM, kMenuItemCol, night_row, night ? "night: on" : "night: off");
   const int vehicle_x = put_ascii(g_cursor == n + kVehicleMenuRow ? INK_ON : INK_DIM, kMenuItemCol,
                                   night_row + kVehicleMenuRow, "vehicle: ");
   put_uint(g_cursor == n + kVehicleMenuRow ? INK_ON : INK_DIM, vehicle_x, night_row + kVehicleMenuRow,
            field::walker().vehicle_class);
   const int ward_x = put_ascii(g_cursor == n + kWardMenuRow ? INK_ON : INK_DIM, kMenuItemCol,
                                night_row + kWardMenuRow, "use Ward charm: ");
   put_uint(g_cursor == n + kWardMenuRow ? INK_ON : INK_DIM, ward_x, night_row + kWardMenuRow,
            inventory::count(g_bag, battle_ids::ITEM_WARD_CHARM));
   put_ascii(g_cursor == n + kLeadMenuRow ? INK_ON : INK_DIM, kMenuItemCol, night_row + kLeadMenuRow,
             "swap the lead");
   // F2: the first menu field action usable here (its label), the flight menu and the escape
   const Ink action_ink = g_cursor == n + kActionMenuRow ? INK_ON : INK_DIM;
   const int action_x = put_ascii(action_ink, kMenuItemCol, night_row + kActionMenuRow, "field action: ");
   battle::MenuAction act{};
   if (first_menu_action(act))
   {
      put_name(action_ink, action_x, night_row + kActionMenuRow,
               script::kBank.text + script::actions::info(act.action)->label);
   }
   else
   {
      put_ascii(action_ink, action_x, night_row + kActionMenuRow, "none");
   }
   put_ascii(g_cursor == n + kFlightMenuRow ? INK_ON : INK_DIM, kMenuItemCol, night_row + kFlightMenuRow,
             "flight menu");
   put_ascii(g_cursor == n + kEscapeMenuRow ? INK_ON : INK_DIM, kMenuItemCol, night_row + kEscapeMenuRow,
             "escape");
   put_ascii(g_cursor == n + kMergeMenuRow ? INK_ON : INK_DIM, kMenuItemCol, night_row + kMergeMenuRow,
             "merge altar");
   put_ascii(g_cursor == n + kGroveMenuRow ? INK_ON : INK_DIM, kMenuItemCol, night_row + kGroveMenuRow, "grove");
   put_ascii(g_cursor == n + kFastMenuRow ? INK_ON : INK_DIM, kMenuItemCol, night_row + kFastMenuRow,
             g_fast ? "fast steps: on" : "fast steps: off");
   put_ascii(g_cursor == n + kStartMenuRow ? INK_ON : INK_DIM, kMenuItemCol, night_row + kStartMenuRow,
             "back to start");
   put_ascii(INK_DIM, 1, kMenuTop + g_cursor + (g_cursor >= n ? 1 : 0), ">");

   put_ascii(INK_OFF, 1, kMenuHintRow1, "D-pad walk  B run  A read");
   put_ascii(INK_OFF, 1, kMenuHintRow2, "SELECT hud L coll R noclip");
}

/// D-40: draws the About page (the sample's credits), one line per row, on the menu's layer.
void draw_about()
{
   clear_console();
   put_ascii(INK_TITLE, 1, kMenuTitleRow, "MAP SANDBOX");
   for (int i = 0; i < kAboutLines; ++i)
   {
      put_ascii(INK_ON, kAboutCol, kAboutTop + i, kAbout[i]);
   }
   put_ascii(INK_OFF, 1, kMenuHintRow2, "B back");
}

/// First painted, unblocked cell of a map (where to put the walker).
/// @param m       the map
/// @param[out] x  column
/// @param[out] y  row
void find_spot(const map::Map& m, int& x, int& y)
{
   for (uint16_t i = 0; i < m.chunk_count; ++i)
   {
      for (int c = 0; c < map::kChunkCells; ++c)
      {
         // Cells of a chunk row by row: chunk origin plus the cell's column and row.
         x = m.chunks[i].chunk_x * map::kChunkMetatiles + (c & kChunkColMask);
         y = m.chunks[i].chunk_y * map::kChunkMetatiles + (c >> kChunkRowShift);
         if (map::is_painted(m, x, y) && map::cell_collision(map::cell_at(m, x, y)) == 0)
         {
            return;
         }
      }
   }
   x = m.chunks[0].chunk_x * map::kChunkMetatiles;
   y = m.chunks[0].chunk_y * map::kChunkMetatiles;
}

/// Shows a map with the walker on it (the runner's start: its banner, no transition).
/// @param index  map index
/// @param x      walker column
/// @param y      walker row
/// @param face   field::Direction
void go(int index, int x, int y, uint8_t face)
{
   g_map = index;
   overworld::start(static_cast<uint16_t>(index), x, y, face);   // B-2 clears, load scripts, banner
   g_hud_up = false;
}

/// Shows the world's start.
void go_start()
{
   const map::Start& s = map::kWorld.start;
   go(s.map, s.x, s.y, s.facing);
}

/// Shows another map, at its first free cell.
/// @param index  map index
void go_map(int index)
{
   if (index == map::kWorld.start.map)
   {
      go_start();
      return;
   }
   int x = 0;
   int y = 0;
   find_spot(map::kWorld.maps[index], x, y);
   go(index, x, y, field::DIR_DOWN);
}

/// Whether the HUD may use BG0 (no script, transition or banner of the runner).
/// @return true when it may
bool hud_allowed()
{
   return !overworld::script_running() && !overworld::transitioning() && !overworld::banner_shown();
}

/// B2: with fast steps on, each step the walker took counts kFastSteps times into the step effects (the nursery's
/// checks and experience, the egg cycles).
void count_fast_steps()
{
   const uint32_t steps = field::walker().steps;
   if (g_fast && steps > g_fast_seen)
   {
      script::step_effects::walked((steps - g_fast_seen) * (kFastSteps - 1));
   }
   g_fast_seen = steps;
}

/// F3: runs what the step effects made due (each member's part in turn; the texts it queued are formatted before
/// the next member's field context replaces them; the last one stays on the HUD).
void run_steps()
{
   battle::StepEvent e{};
   while (battle::step_next(e))
   {
      battle::Event ev{};
      while (battle::pop_event(g_field, ev))
      {
         text::Code out[kStepTextCodes];
         if (battle::format_text(g_field, ev, out, kStepTextCodes))
         {
            for (int k = 0; k < kStepTextCodes; ++k)
            {
               g_step_text[k] = out[k];
               if (out[k] == text::kCodeEnd)
               {
                  break;
               }
            }
         }
      }
   }
}

/// F3: Sproutle blighted with kBlightHp hit points (at start and by "back to start").
void blight_member()
{
   monster::set(g_party.members[kBlightSlot], monster::Role::PrimaryStatus, battle_ids::STATUS_BLIGHT);
   monster::set(g_party.members[kBlightSlot], monster::Role::CurrentHp, kBlightHp);
   g_step_text[0] = text::kCodeEnd;
}

/// One frame of walking (the runner plays scripts, warps and banners).
/// @param pad  input
void walk(const Pad& pad)
{
   field::Intent in;
   const bool free = hud_allowed();
   if (pad.down(KEY_DOWN))
   {
      in.direction = field::DIR_DOWN;
   }
   else if (pad.down(KEY_UP))
   {
      in.direction = field::DIR_UP;
   }
   else if (pad.down(KEY_LEFT))
   {
      in.direction = field::DIR_LEFT;
   }
   else if (pad.down(KEY_RIGHT))
   {
      in.direction = field::DIR_RIGHT;
   }
   in.run = pad.down(KEY_B);
   in.interact = pad.hit(KEY_A);
   overworld::update(in);
   if (const map::Map* m = map::current())
   {
      g_map = map::index_of(*m);
   }
   run_steps();
   encounter::Result r{};
   if (field::take_encounter(&r))
   {   // no battle in the sandbox: make its creature (the lead's encounter_made hooks), show it and walk on
      g_last = r;
      g_has_last = true;
      ++g_encounters;
      const field::Walker& w = field::walker();
      g_last_slot = map::cell_encounter_slot(map::cell_at(*map::current(), w.x, w.y));
      const battle::Made made = battle::make_encounter(r, g_wild, rng::gameplay);
      g_has_made = made == battle::Made::Battle;
      g_made_skipped = made == battle::Made::Skipped;
   }
   if (!hud_allowed())
   {   // BG0 is the runner's: the HUD steps aside
      if (g_hud_up)
      {
         clear_console();
         video::set_blend(video::BlendMode::Off, 0, 0);
         g_hud_up = false;
      }
      return;
   }
   if (!g_hud_up)
   {
      setup_bg0();
      g_hud_up = true;
   }
   if (!free)
   {
      return;
   }   // the input went to what just ended
   if (pad.hit(KEY_SELECT))
   {
      g_hud = !g_hud;
      setup_bg0();
   }
   if (pad.hit(KEY_L))
   {
      g_collision = !g_collision;
      setup_bg0();
   }
   if (pad.hit(KEY_R))
   {
      field::set_noclip(!field::walker().noclip);
   }
   if (pad.hit(KEY_START))
   {
      g_mode = Mode::Menu;
      g_cursor = g_map;
      setup_bg0();
      draw_menu();
      return;
   }
   if (g_collision)
   {
      draw_overlay();
   }
   else if (g_hud)
   {
      draw_hud();
   }
}

/// One frame of the menu.
/// @param pad  input
void menu(const Pad& pad)
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
      if (g_cursor == map::kWorld.map_count)
      {
         encounter::set_conditions(encounter::conditions() ^ (1u << encounter_ids::ENCOUNTER_CONDITION_NIGHT));
         draw_menu();
         return;
      }
      if (g_cursor == map::kWorld.map_count + kVehicleMenuRow)
      {   // the next vehicle class (A2), wrapping to on foot
         const int next = (field::walker().vehicle_class + 1) % map::kWorld.vehicle_class_count;
         field::set_vehicle(static_cast<uint8_t>(next));
         draw_menu();
         return;
      }
      if (g_cursor == map::kWorld.map_count + kWardMenuRow)
      {   // F1: the charm's use_field logic starts the timed field effect (target none: no member)
         inventory::FieldUse u(g_field, g_party, inventory::kNoMember);
         const inventory::UseResult used = u.use(g_bag, battle_ids::ITEM_WARD_CHARM);
         g_note = used == inventory::UseResult::Used ? "ward on" : "no ward";
         draw_menu();
         return;
      }
      if (g_cursor == map::kWorld.map_count + kLeadMenuRow)
      {   // F1: the other member leads (the lead rule: the first that can battle)
         const monster::Record first = g_party.members[0];
         g_party.members[0] = g_party.members[1];
         g_party.members[1] = first;
         g_note = "lead swapped";
         draw_menu();
         return;
      }
      if (g_cursor == map::kWorld.map_count + kActionMenuRow)
      {   // F2: the member writes itself to the script bank; an asking action's wrapper asks first
         battle::MenuAction act{};
         g_note = first_menu_action(act) ? "field action" : "no field action";
         run_from_menu(first_menu_action(act) ? battle::start_action(act.action, act.member) : script::kNoScriptId);
         return;
      }
      if (g_cursor == map::kWorld.map_count + kFlightMenuRow)
      {
         run_from_menu(static_cast<uint16_t>(script_ids::SCRIPT_SANDBOX_FLY));
         return;
      }
      if (g_cursor == map::kWorld.map_count + kEscapeMenuRow)
      {
         run_from_menu(static_cast<uint16_t>(script_ids::SCRIPT_SANDBOX_ESCAPE));
         return;
      }
      if (g_cursor == map::kWorld.map_count + kMergeMenuRow)
      {   // B1: the Merge Altar through the combine port (the script's `combine open merge_altar`)
         run_from_menu(static_cast<uint16_t>(script_ids::SCRIPT_MERGE_ALTAR));
         return;
      }
      if (g_cursor == map::kWorld.map_count + kGroveMenuRow)
      {   // B2: the Grove through the nursery port (the script grove)
         run_from_menu(static_cast<uint16_t>(script_ids::SCRIPT_GROVE));
         return;
      }
      if (g_cursor == map::kWorld.map_count + kFastMenuRow)
      {   // B2: the debug option
         g_fast = !g_fast;
         g_note = g_fast ? "fast steps" : "steps";
         draw_menu();
         return;
      }
      g_mode = Mode::Walking;
      if (g_cursor == map::kWorld.map_count + kStartMenuRow)
      {
         blight_member();   // F3: the blight runs again
         go_start();
      }
      else
      {
         go_map(g_cursor);
      }
   }
   else if (pad.hit(KEY_B) || pad.hit(KEY_START))
   {
      g_mode = Mode::Walking;
      setup_bg0();
      g_hud_up = true;
   }
   else if (pad.hit(KEY_SELECT))
   {   // D-40: the About page (BG0 stays the menu's: shown, no blend)
      g_mode = Mode::About;
      draw_about();
   }
}

/// D-40: one frame of the About page: A, B, SELECT or START go back to the menu.
/// @param pad  input
void about_frame(const Pad& pad)
{
   if (pad.hit(KEY_A) || pad.hit(KEY_B) || pad.hit(KEY_SELECT) || pad.hit(KEY_START))
   {
      g_mode = Mode::Menu;
      draw_menu();
   }
}

} // namespace

namespace capsule::field::game
{

uint16_t step_hook(uint16_t hook)
{
   if (hook != world_ids::STEP_HOOK_PEDOMETER)
   {
      return kNoScript;
   }
   ++g_pedometer;
   return (g_pedometer & kPedometerMessageMask) == 0 ? static_cast<uint16_t>(script_ids::SCRIPT_PEDOMETER) : kNoScript;
}

} // namespace capsule::field::game

/// The Starter party (Breezle leads with its talent Quick Feet, Sproutle; F2: Tidepup and Glimmet for the field
/// actions) and the bag with Ward charms (F1).
void fresh_party()
{
   g_party = monster::Party{};
   rng::Stream r{kPartySeed};
   battle::make_monster(g_party.members[0], kLeadSpecies, kLeadLevel, r);
   battle::make_monster(g_party.members[1], kOtherSpecies, kOtherLevel, r);
   battle::make_monster(g_party.members[kWaderSlot], kWaderSpecies, kFieldLevel, r);
   battle::make_monster(g_party.members[kLightSlot], kLightSpecies, kFieldLevel, r);
   battle::make_monster(g_party.members[kWingSlot], kWingSpecies, kFieldLevel, r);
   g_party.count = kMembers;
   // the first passive slot for all (Breezle's is Quick Feet); B2: Sproutle's hidden one (Nurture)
   for (uint8_t k = 0; k < kMembers; ++k)
   {
      monster::set(g_party.members[k], monster::Role::PassiveSelector, k == kBlightSlot ? kHiddenSlot : 0);
   }
   combine::nursery::clear();
   inventory::clear(g_bag);
   inventory::add(g_bag, battle_ids::ITEM_WARD_CHARM, kWardCharms);
   blight_member();
}

/// Runs the sandbox.
/// @return 0
int main()
{
   hal::system::init();
   gfx::palette::reset();
   gfx::sprites::reset();
   // Field placeholders: the script bank resolves {player} and {berries}.
   text::set_field_resolver(script::placeholder_text);
   static constexpr auto kPlayerName = text::encode("Robin");
   script::set_name(script_ids::PLACEHOLDER_PLAYER, kPlayerName.codes);
   const asset::Sprite* walker = asset::sprite(asset::key_v1("sprite:WALKER"));
   if (walker != nullptr)
   {
      gfx::sprites::load(field::kWalkerSlot, *walker);
      gfx::sprites::show(field::kWalkerSlot, true);
      field::attach_sprite(field::kWalkerSlot, walker);
   }
   fresh_party();
   // F1: the lead rules ride on every encounter roll while the sandbox runs
   battle::LeadScope leads(g_field, g_party);
   // F2: the field actions' dispatcher and the scripts' `field_action available` on the party
   battle::ActionScope actions(g_party);
   // F3: the step effects run on the party (their field contexts on the same lent State, draws from gameplay)
   battle::StepScope steps(g_field, g_party, rng::gameplay);
   // B1: the combine statements combine party members in the same lent State (no boxes here), draws from gameplay
   script::combine::attach(&g_field, &g_party, nullptr, &rng::gameplay);
   // B2: the nursery statements and the nickname prompt work on the party
   script::nursery::attach(&g_party);
   go_start();
   Pad pad;
   while (hal::system::update())
   {
      pad.update();
      rng::next16(rng::gameplay);   // A-4: the gameplay stream advances every frame
      if (g_mode == Mode::Walking)
      {
         walk(pad);
      }
      else if (g_mode == Mode::Menu)
      {
         menu(pad);
      }
      else
      {
         about_frame(pad);
      }
      count_fast_steps();
      gfx::sprites::update();
      map::update();
   }
   return 0;
}
