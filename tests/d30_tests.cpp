/// @file
/// D-30 tests (DS1) on the fixture tests/data/d30: tile animation targets at frame offsets, cell override sets
/// (applied, cleared, kept through a flag, from scripts), door animations (frame steps, tiles and override doors,
/// bindings by warp and by metatile, sounds), the overworld's door warp sequence, direction-grid sprite sheets and
/// the object draw order (sub-priorities from the world's rule, sorted OAM).
#include "capsule/asset/asset.h"
#include "capsule/core/random.h"
#include "capsule/field/doors.h"
#include "capsule/field/field.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/map/map.h"
#include "capsule/overworld/overworld.h"
#include "capsule/script/port.h"
#include "capsule/script/script.h"
#include "capsule/text/encode.h"

#include "../src/hal/soft/soft_video.h"

#include "d30_script_ids.h"
#include "d30_world_ids.h"

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
/// Test build: no entropy.
/// @return 0
uint32_t entropy()
{
   return 0;
}
} // namespace capsule::hal::system

namespace capsule::hal::input
{
/// Test build: no keys held.
/// @return 0
KeyBits held()
{
   return 0;
}
} // namespace capsule::hal::input

/// The fixture's scripts show nothing: the port does nothing.
namespace capsule::script::port
{
/// Shows nothing.
/// @param codes  text
void message(const text::Code* codes)
{
   (void)codes;
}
/// Never busy.
/// @return false
bool message_busy()
{
   return false;
}
/// Nothing to close.
void close_message()
{
}
/// No presses.
/// @return false
bool button()
{
   return false;
}
/// Nothing to open.
void yesno_open()
{
}
/// Answers yes.
/// @return 1
int8_t yesno_poll()
{
   return 1;
}
/// Nothing to open.
/// @param options  options
/// @param count    count
/// @param cancel   cancel index
void choice_open(const text::Code* const* options, uint8_t count, uint8_t cancel)
{
   (void)options;
   (void)count;
   (void)cancel;
}
/// Answers 0.
/// @return 0
int16_t choice_poll()
{
   return 0;
}
/// Nothing to advance.
void tick()
{
}
/// Box position (ignored).
/// @param pos  position
void message_position(uint8_t pos)
{
   (void)pos;
}
/// Number entry (ignored).
/// @param digits  digits
void number_open(uint8_t digits)
{
   (void)digits;
}
/// The number answer.
/// @return 0
int32_t number_poll()
{
   return 0;
}
/// Key input: A.
/// @return 1
int8_t key_poll()
{
   return 1;
}
} // namespace capsule::script::port

namespace capsule::script::party
{
/// No party: every role reads 0.
/// @param slot     party slot
/// @param role     role
/// @param element  element
/// @return 0
int32_t role_get(uint8_t slot, uint8_t role, uint8_t element)
{
   (void)slot;
   (void)role;
   (void)element;
   return 0;
}
/// No party: writes do nothing.
/// @param slot     party slot
/// @param role     role
/// @param element  element
/// @param value    value
void role_set(uint8_t slot, uint8_t role, uint8_t element, int32_t value)
{
   (void)slot;
   (void)role;
   (void)element;
   (void)value;
}
} // namespace capsule::script::party

namespace capsule::script::game
{
/// The fixture calls no natives.
/// @param id      native
/// @param args    arguments
/// @param argc    count
/// @param result  result
/// @return Done
NativeStatus native(uint16_t id, const int32_t* args, uint8_t argc, int32_t& result)
{
   (void)id;
   (void)args;
   (void)argc;
   result = 0;
   return NativeStatus::Done;
}
} // namespace capsule::script::game

namespace
{
uint32_t g_sounds = 0;            ///< door_sound() calls.
capsule::AssetKey g_last_sound{};   ///< Its last key.
} // namespace

namespace capsule::field::game
{
/// The fixture has no step hooks.
/// @param hook  hook ID
/// @return kNoScript
uint16_t step_hook(uint16_t hook)
{
   (void)hook;
   return kNoScript;
}
/// Counts the doors' sounds.
/// @param sound  the key
void door_sound(AssetKey sound)
{
   ++g_sounds;
   g_last_sound = sound;
}
} // namespace capsule::field::game

namespace
{

using namespace capsule;
namespace video = hal::video;

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

uint16_t g_frame[hal::soft::kFramePixels];   ///< Last composed frame.

constexpr int kCell = map::kMetatilePixels;    ///< Pixels per cell.
constexpr int kInside = 4;                     ///< Pixels into a cell where its colour is read.
constexpr int kTileBytes = 32;                 ///< Bytes of a 4bpp tile.
constexpr uint8_t kBgGround = 2;               ///< The yard's layer 0 BG.
constexpr uint8_t kBgTop = 1;                  ///< The yard's layer 1 BG.
constexpr int kEntryTiles = 2;                 ///< Map entries per cell side.

/// @name Fixture colors (tests/data/d30/tilesets/yard.json, entries 1-10)
///@{
constexpr video::Color kGround = video::rgb5(0x20 >> 3, 0x80 >> 3, 0x20 >> 3);   ///< Entry 1.
constexpr video::Color kWall = video::rgb5(0x60 >> 3, 0x40 >> 3, 0x20 >> 3);     ///< Entry 2.
constexpr video::Color kMark = video::rgb5(31, 0, 31);                           ///< Entry 4.
constexpr video::Color kFlower = video::rgb5(0xF8 >> 3, 0xF8 >> 3, 0xF8 >> 3);   ///< Entry 5 (the targets' own).
/// The glow animation's frames 0-3 (entries 7-10).
constexpr video::Color kGlow[] = {video::rgb5(0, 31, 0), video::rgb5(0, 0, 31), video::rgb5(31, 31, 0),
                                  video::rgb5(0, 31, 31)};
constexpr int kGlowFrames = 4;                 ///< Frames of the glow.
constexpr int kGlowPeriod = 4;                 ///< Display frames per glow frame.
///@}

/// @name The gate door (tests/data/d30/assets.json door:GATE)
///@{
constexpr uint16_t kGateLoadAt = 1024 - 8;     ///< Its frames load at the tile area's last 8 tiles.
constexpr uint16_t kGateFrame0Pixels = 0x8888; ///< Frame 0's tiles (palette index 8), one halfword.
constexpr uint16_t kGateFrame1Pixels = 0x9999; ///< Frame 1's (index 9).
constexpr int kGateSteps = 8;                  ///< Display frames of its opening (2 closed, 3 + 3).
///@}

/// Latches and composes a frame (the queued uploads land first, as in system::update()).
void show()
{
   hal::vram::flush();
   hal::soft::latch();
   hal::soft::compose(g_frame);
}

/// The colour in a cell of the last frame (camera at the map's origin).
/// @param x  cell column
/// @param y  cell row
/// @return BGR555
uint16_t cell_color(int x, int y)
{
   return g_frame[(y * kCell + kInside) * video::kScreenWidth + x * kCell + kInside];
}

/// The yard map.
/// @return it
const map::Map& yard()
{
   return map::kWorld.maps[world_ids::MAP_YARD];
}

/// Fresh video, the yard shown at the origin.
void fresh()
{
   hal::soft::reset();
   hal::vram::reset();
   gfx::palette::reset();
   gfx::sprites::reset();
   map::enter(yard(), 0, 0);
}

/// Runs the field for some frames standing.
/// @param frames  how many
void idle(int frames)
{
   for (int i = 0; i < frames; ++i)
   {
      field::update(field::Intent{});
   }
}

/// Tile animation targets: each target shows frame ((clock + phase) / period + offset) mod frames, its own tiles.
void animation_targets()
{
   fresh();
   const map::TileAnimation& a = yard().animations[0];
   CHECK(yard().animation_count == 1 && a.targets == 2 && a.frame_count == kGlowFrames);
   CHECK(yard().anim_targets != nullptr && yard().anim_targets[0].offset == 2 && yard().anim_targets[1].offset == 3 &&
         yard().anim_targets[1].phase == 1);
   // the three flower cells (1, 6), (2, 6), (3, 6): offsets 0, 2, 3; phases 0, 0, 1
   constexpr int kOffsets[] = {0, 2, 3};
   constexpr int kPhases[] = {0, 0, 1};
   constexpr int kTargets = 3;
   constexpr int kRow = 6;
   constexpr int kFrames = 24;
   bool all = true;
   for (int clock = 0; clock < kFrames; ++clock)
   {
      show();
      for (int k = 0; k < kTargets; ++k)
      {
         const int frame = ((clock + kPhases[k]) / kGlowPeriod + kOffsets[k]) % kGlowFrames;
         all = all && cell_color(1 + k, kRow) == kGlow[frame];
      }
      map::update();
   }
   CHECK(all);
   CHECK(cell_color(0, kRow) == kGround);
   static_cast<void>(kFlower);
}

/// Override sets: applied and cleared on the shown map, a set of entries, a set kept through a flag, scripts.
void override_sets()
{
   fresh();
   show();
   CHECK(cell_color(1, 1) == kGround && video::read_map_cell(kBgTop, 2 * kEntryTiles, 1 * kEntryTiles) == 0);
   CHECK(map::set_override_set(world_ids::OVERRIDE_BRIDGE, true) && map::override_set_on(world_ids::OVERRIDE_BRIDGE));
   show();
   CHECK(cell_color(1, 1) == kMark);
   constexpr uint16_t kEntries[] = {4101, 5125, 6149, 7173};   // the set's raw entries on layer 1 of (2, 1)
   CHECK(video::read_map_cell(kBgTop, 2 * kEntryTiles, 1 * kEntryTiles) == kEntries[0] &&
         video::read_map_cell(kBgTop, 2 * kEntryTiles + 1, 1 * kEntryTiles) == kEntries[1] &&
         video::read_map_cell(kBgTop, 2 * kEntryTiles, 1 * kEntryTiles + 1) == kEntries[2] &&
         video::read_map_cell(kBgTop, 2 * kEntryTiles + 1, 1 * kEntryTiles + 1) == kEntries[3]);
   map::set_override_set(world_ids::OVERRIDE_BRIDGE, false);
   show();
   CHECK(cell_color(1, 1) == kGround && !map::override_set_on(world_ids::OVERRIDE_BRIDGE));
   CHECK(!map::set_override_set(map::kWorld.override_set_count, true));   // no such set
   // the camera moving away and back keeps an applied set
   map::set_override_set(world_ids::OVERRIDE_BRIDGE, true);
   map::set_camera(kCell * map::kChunkMetatiles * 2, 0);
   map::set_camera(0, 0);
   show();
   CHECK(cell_color(1, 1) == kMark);

   // a set kept through its flag: entering the map applies it; the script port writes the flag
   script::clear_all();
   script::set_flag(static_cast<script::FlagId>(script_ids::FLAG_GATE_OPEN), true);
   hal::soft::reset();
   hal::vram::reset();
   field::enter(world_ids::MAP_YARD, 4, 4, field::DIR_DOWN);
   show();
   CHECK(map::override_set_on(world_ids::OVERRIDE_GATE_OPEN) && !map::override_set_on(world_ids::OVERRIDE_BRIDGE));
   const int32_t cam_x = map::camera_x();
   const int32_t cam_y = map::camera_y();
   const auto cell_at = [&](int x, int y)
   {
      return g_frame[((y * kCell - cam_y) + kInside) * video::kScreenWidth + (x * kCell - cam_x) + kInside];
   };
   CHECK(cell_at(3, 2) == kGround && cell_at(2, 2) == kWall);
   script::run_now(script::ScriptId{script_ids::SCRIPT_GATE_OFF});
   show();
   CHECK(!map::override_set_on(world_ids::OVERRIDE_GATE_OPEN) && cell_at(3, 2) == kWall &&
         !script::flag(static_cast<script::FlagId>(script_ids::FLAG_GATE_OPEN)));
   script::run_now(script::ScriptId{script_ids::SCRIPT_BRIDGE_ON});
   CHECK(map::override_set_on(world_ids::OVERRIDE_BRIDGE));
   script::run_now(script::ScriptId{script_ids::SCRIPT_GATE_ON});
   CHECK(map::override_set_on(world_ids::OVERRIDE_GATE_OPEN) &&
         script::flag(static_cast<script::FlagId>(script_ids::FLAG_GATE_OPEN)));
   // leaving and entering again: the flag's set comes back, the map-lifetime set does not
   field::enter(world_ids::MAP_YARD, 4, 4, field::DIR_DOWN);
   CHECK(map::override_set_on(world_ids::OVERRIDE_GATE_OPEN) && !map::override_set_on(world_ids::OVERRIDE_BRIDGE));
}

/// The entries a layer of a cell shows (its top-left one).
/// @param bg  the layer's BG
/// @param x   cell column
/// @param y   cell row
/// @return the map entry
uint16_t entry_at(uint8_t bg, int x, int y)
{
   return video::read_map_cell(bg, (x % map::kRingMetatiles) * kEntryTiles, (y % map::kRingMetatiles) * kEntryTiles);
}

/// Doors: bindings, the opening's steps frame by frame, the closing reversed, set open / closed, sounds.
void doors()
{
   script::clear_all();
   hal::soft::reset();
   hal::vram::reset();
   gfx::sprites::reset();
   field::enter(world_ids::MAP_YARD, 4, 4, field::DIR_DOWN);
   const asset::Door* gate = asset::door(asset::key_v1("door:GATE"));
   CHECK(gate != nullptr && gate->show == asset::DoorShow::Tiles && gate->width == 1 && gate->height == 2);
   CHECK(field::doors::at(4, 2) == gate);   // the warp's door
   CHECK(field::doors::at(6, 2) == gate);   // bound by its metatile
   CHECK(field::doors::at(5, 2) == nullptr && field::doors::at(4, 1) == nullptr);
   const uint16_t own = entry_at(kBgGround, 4, 2);
   const uint32_t sounds = g_sounds;
   CHECK(field::doors::open(4, 2) && field::doors::busy());
   CHECK(g_sounds == sounds + 1 && g_last_sound == asset::key_v1("sound:DOOR_OPEN"));
   // steps: closed 2, frame 0 for 3, frame 1 for 3 (each drawn on its step's first display frame)
   bool steps_ok = true;
   for (int n = 1; n <= kGateSteps; ++n)
   {
      idle(1);
      show();
      const uint16_t bottom = entry_at(kBgGround, 4, 2);
      const uint16_t top = entry_at(kBgGround, 4, 1);
      const uint16_t pixels = hal::soft::peek16(video::Region::BgVram, uint32_t{kGateLoadAt} * kTileBytes);
      if (n <= 2)
      {
         steps_ok = steps_ok && bottom == own;
      }
      else
      {
         steps_ok = steps_ok && top == kGateLoadAt && bottom == kGateLoadAt + 4 &&
                    pixels == (n <= 5 ? kGateFrame0Pixels : kGateFrame1Pixels);
      }
      steps_ok = steps_ok && field::doors::busy() == (n < kGateSteps);
   }
   CHECK(steps_ok);
   CHECK(entry_at(kBgTop, 4, 2) == 0);   // layer 1: the door's own (blank) entries
   idle(3);
   show();
   CHECK(entry_at(kBgGround, 4, 2) == kGateLoadAt + 4);   // the open frame stays
   CHECK(field::doors::close(4, 2));
   CHECK(g_last_sound == asset::key_v1("sound:DOOR_SHUT"));
   idle(kGateSteps);
   show();
   CHECK(!field::doors::busy() && entry_at(kBgGround, 4, 2) == own);   // closed: the cell's own metatile again
   CHECK(field::doors::set_open(6, 2) && !field::doors::busy());
   show();
   CHECK(entry_at(kBgGround, 6, 2) == kGateLoadAt + 4);
   CHECK(field::doors::set_closed(6, 2));
   show();
   CHECK(entry_at(kBgGround, 6, 2) == entry_at(kBgGround, 4, 2));
   CHECK(!field::doors::open(5, 2));   // no door there
   // two doors at once, a third refused
   CHECK(field::doors::open(4, 2) && field::doors::open(6, 2));
   idle(kGateSteps);
   CHECK(!field::doors::busy());
}

/// The overworld's door warp: the door opens, the walker walks in, hides, the door closes, the screen fades, the
/// destination's door shows open, the walker walks out and that door closes.
void door_warp()
{
   script::clear_all();
   hal::soft::reset();
   hal::vram::reset();
   gfx::sprites::reset();
   const asset::Sprite* walker = asset::sprite(asset::key_v1("sprite:WALKER"));
   gfx::sprites::load(field::kWalkerSlot, *walker);
   gfx::sprites::show(field::kWalkerSlot, true);
   field::attach_sprite(field::kWalkerSlot, walker);
   overworld::start(world_ids::MAP_YARD, 4, 3, field::DIR_UP);
   field::Intent up;
   up.direction = field::DIR_UP;
   overworld::update(up);
   CHECK(overworld::transitioning() && field::doors::busy());
   bool hidden = false;
   bool entered = false;
   bool open_shown = false;
   constexpr int kMaxFrames = 600;
   int frames = 0;
   for (; frames < kMaxFrames && overworld::transitioning(); ++frames)
   {
      overworld::update(field::Intent{});
      show();
      hidden = hidden || !field::walker().visible;
      if (map::current() == &map::kWorld.maps[world_ids::MAP_HALL] && !entered)
      {
         entered = true;
         const asset::Door* hatch = asset::door(asset::key_v1("door:HATCH"));
         constexpr uint16_t kHatchOpen = 8193;   // its open frame's raw entries
         open_shown = hatch != nullptr && entry_at(kBgGround, 2, 5) == kHatchOpen && !field::walker().visible;
      }
   }
   CHECK(frames < kMaxFrames && hidden && entered && open_shown);
   CHECK(field::walker().x == 2 && field::walker().y == 6 && field::walker().visible);
   CHECK(!field::doors::busy() && entry_at(kBgGround, 2, 5) != 8193);   // closed behind it
}

/// Direction-grid sprites: the right column mirrors the left one, identical frames are merged.
void direction_grids()
{
   const asset::Sprite* g3 = asset::sprite(asset::key_v1("sprite:GRID3"));
   const asset::Sprite* g4 = asset::sprite(asset::key_v1("sprite:GRID4"));
   CHECK(g3 != nullptr && g4 != nullptr);
   if (g3 == nullptr || g4 == nullptr)
   {
      return;
   }
   constexpr int kSets = 2;
   constexpr int kDirections = 4;
   constexpr uint8_t kLeft = 2;
   constexpr uint8_t kRight = 3;
   CHECK(g3->animation_count == kSets * kDirections);
   CHECK(g3->frame_count == 5);   // six cells, up's step frame equal to its stand frame
   const asset::Animation& left = g3->animations[kLeft];
   const asset::Animation& right = g3->animations[kRight];
   CHECK(right.frames[0] == left.frames[0] && right.flips != nullptr && right.flips[0] == asset::kFlipH &&
         left.flips == nullptr);
   const asset::Animation& walk_right = g3->animations[kDirections + kRight];
   CHECK(walk_right.count == 2 && walk_right.flips != nullptr && walk_right.flips[0] == asset::kFlipH &&
         walk_right.flips[1] == asset::kFlipH && walk_right.period == 8);
   CHECK(g3->animations[1].frames[0] == g3->animations[kDirections + 1].frames[0]);   // merged up frames
   CHECK(g4->frame_count == kDirections && g4->animations[kRight].flips == nullptr &&
         g4->animations[kRight].frames[0] == kRight);
}

/// The draw order: the world's rule, sorted OAM entries (lower on screen in front; an elevation's base).
void draw_order()
{
   // the default rule: rows of 16 pixels from the screen's top, two steps each
   CHECK(map::draw_order_of(map::kDefaultDrawOrder, 80, 0) == 23);
   CHECK(map::draw_order_of(map::kDefaultDrawOrder, 96, 0) == 21);
   CHECK(map::draw_order_of(map::kDefaultDrawOrder, -8, 0) == 33);
   // the fixture's rule: elevation 3's base is 60, the others' 100
   CHECK(map::sub_priority(80, 3) == 83 && map::sub_priority(80, 1) == 123);

   script::clear_all();
   hal::soft::reset();
   hal::vram::reset();
   gfx::sprites::reset();
   const asset::Sprite* walker = asset::sprite(asset::key_v1("sprite:WALKER"));
   gfx::sprites::load(field::kWalkerSlot, *walker);
   gfx::sprites::show(field::kWalkerSlot, true);
   field::attach_sprite(field::kWalkerSlot, walker);
   field::enter(world_ids::MAP_YARD, 4, 4, field::DIR_DOWN);
   const field::Object* front = field::object(world_ids::OBJ_YARD_FRONT);
   const field::Object* raised = field::object(world_ids::OBJ_YARD_RAISED);
   CHECK(front != nullptr && raised != nullptr);
   if (front == nullptr || raised == nullptr)
   {
      return;
   }
   idle(1);
   gfx::sprites::update();
   // the walker at (4, 4) stands above the object at (4, 5): the object is in front (a lower OAM entry)
   CHECK(gfx::sprites::oam_entry(front->slot) < gfx::sprites::oam_entry(field::kWalkerSlot));
   field::place(4, 6, field::DIR_DOWN);   // below it: the walker in front
   idle(1);
   gfx::sprites::update();
   CHECK(gfx::sprites::oam_entry(field::kWalkerSlot) < gfx::sprites::oam_entry(front->slot));
   // the raised object at (5, 5) (elevation 3: base 60) stays in front of a walker below it at (5, 6)
   field::place(5, 6, field::DIR_DOWN);
   idle(1);
   gfx::sprites::update();
   CHECK(gfx::sprites::oam_entry(raised->slot) < gfx::sprites::oam_entry(field::kWalkerSlot));
   // composed: where the walker and the object overlap, the front one's pixels show
   field::place(4, 4, field::DIR_DOWN);
   idle(1);
   gfx::sprites::update();
   show();
   const int sx = 4 * kCell - map::camera_x() + kCell / 2;   // the object's middle column
   const int sy = 5 * kCell - map::camera_y() + kInside;      // inside both sprites
   constexpr video::Color kWalkerColor = video::rgb5(31, 31, 31);
   CHECK(g_frame[sy * video::kScreenWidth + sx] != kWalkerColor);   // the object's body, not the walker's
   // unsorted (another screen): slot order again
   gfx::sprites::set_sorted(false);
   gfx::sprites::update();
   CHECK(gfx::sprites::oam_entry(field::kWalkerSlot) == field::kWalkerSlot);
}

} // namespace

/// Runs the checks.
/// @return 0 when every check passed
int main()
{
   animation_targets();
   override_sets();
   doors();
   door_warp();
   direction_grids();
   draw_order();
   std::printf("d30_tests: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
