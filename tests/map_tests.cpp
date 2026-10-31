/// @file
/// Tests for capsule::map (cells, border, streaming renderer, tile animations)
/// and capsule::field (movement rules), against the world compiled from
/// tests/data/maps/world.json, drawn with the software video model, and
/// capsule::encounter against its encounter tables (tests/data/maps/encounters:
/// slot 0 grass at (1,5) always rolls, slot 1 at (2,5) never does, the water
/// at (10,0) is slot 2 with a swim table and an "old lure" table).
///
/// Fixture layout (map "field", metatile x right, y down; chunks (0,0), (1,0)
/// and (0,1) populated, (1,1) missing):
///
///     row 0  ........>.W.HHM.     > push right, W water, H elevation 4, M elevation 3
///     row 1  ..........H.B...     B bridge (elevation 15, covered layer)
///     row 2  .L.T.A..........     L ledge (down), T tree (top layer), A animated tile
///     row 3  ................
///     row 4  ..IIII#.........     I ice, # wall (collision 1)
///     row 5  .gg.............     g grass (encounter; slot 0 at x 1, slot 1 at x 2)
///     row 6  .......u........     u unpainted (border)
///
/// Events (B-3; scripts in tests/data/maps/scripts): warp 0 at (3,9) to
/// map small, a press warp (left) at (0,12) to map stack, a trigger at
/// (5,9) while var gate == 0, a sign at (2,11) read facing down until flag
/// sign_gone; small (area: field) warps back from (-4,-4); stack (its own
/// area) warps back from (1,1). Step hooks: every_step (1), even (2), four
/// (4, phase 1).
///
/// Field rules (A1, A2): map small (running not allowed, custom levels in
/// its top rows) and the world's custom elevations and swim class; see
/// field_rules().
#include "capsule/asset/asset.h"
#include "capsule/core/random.h"
#include "capsule/encounter/encounter.h"
#include "capsule/field/field.h"
#include "capsule/gfx/background.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/map/map.h"
#include "capsule/overworld/locations.h"
#include "capsule/overworld/overworld.h"
#include "capsule/script/field_actions.h"
#include "capsule/script/port.h"
#include "capsule/script/script.h"
#include "capsule/text/encode.h"
#include "capsule/text/placeholder.h"
#include "capsule/text/text_layer.h"

#include "../src/hal/soft/soft_video.h"

#include "test_encounter_ids.h"
#include "test_map_script_ids.h"
#include "test_world_ids.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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
/// Test build: no entropy (sequences seed from it).
/// @return 0
uint32_t entropy()
{
   return 0;
}
} // namespace capsule::hal::system

namespace capsule::hal::input
{
/// Test build: no keys held (sequences read them).
/// @return 0
KeyBits held()
{
   return 0;
}
} // namespace capsule::hal::input

/// The test world's scripts never wait or show anything: the port does nothing.
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
/// Number entry: answers 42.
/// @param digits  digits
void number_open(uint8_t digits)
{
   (void)digits;
}
/// The number answer.
/// @return 42
int32_t number_poll()
{
   return 42;
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
/// The map tests have no party: every role reads 0.
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
/// The map tests have no party: writes do nothing.
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
/// The test scripts call no natives.
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
uint32_t g_hook_calls[3] = {};          ///< step_hook() calls per hook.
uint32_t g_hook_steps[3] = {};          ///< Walker steps at each hook's last call.
uint16_t g_hook_script = 0xFFFF;        ///< What step_hook() asks for (field::kNoScript: nothing).
} // namespace

namespace capsule::field::game
{
/// Counts calls; asks for g_hook_script from hook "four".
/// @param hook  hook ID
/// @return a script, or kNoScript
uint16_t step_hook(uint16_t hook)
{
   if (hook < 3)
   {
      ++g_hook_calls[hook];
      g_hook_steps[hook] = field::walker().steps;
   }
   return hook == world_ids::STEP_HOOK_FOUR ? g_hook_script : field::kNoScript;
}
} // namespace capsule::field::game

namespace
{

using namespace capsule;
namespace video = hal::video;
using field::Blocked;
using field::Motion;

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

/// Latches and composes a frame.
void show()
{
   hal::vram::flush();   // as system::update(): queued uploads land, then the frame
   hal::soft::latch();
   hal::soft::compose(g_frame);
}

/// One pixel of the last frame.
/// @param x  column
/// @param y  row
/// @return BGR555
uint16_t pixel(int x, int y)
{
   return g_frame[y * video::kScreenWidth + x];
}

/// @name Fixture colors (tileset palette entries 1-6)
///@{
constexpr video::Color kGround = video::rgb5(0x20 >> 3, 0x80 >> 3, 0x20 >> 3);   ///< Entry 1.
constexpr video::Color kWall = video::rgb5(0x60 >> 3, 0x40 >> 3, 0x20 >> 3);     ///< Entry 2.
constexpr video::Color kTreeTop = video::rgb5(0xF8 >> 3, 0x60 >> 3, 0x60 >> 3);  ///< Entry 3.
constexpr video::Color kWater = video::rgb5(0x20 >> 3, 0x40 >> 3, 0xC0 >> 3);    ///< Entry 4.
constexpr video::Color kYellow = video::rgb5(31, 31, 0);                         ///< Entry 6.
constexpr video::Color kWhite = video::rgb5(31, 31, 31);                         ///< The walker sprite.
///@}

/// The field map.
/// @return it
const map::Map& field_map()
{
   return *map::find(asset::key_v1("map:FIELD"));
}

/// Fresh video, map shown with the camera at the origin.
void fresh()
{
   hal::soft::reset();
   hal::vram::reset();
   gfx::palette::reset();
   gfx::sprites::reset();
   map::enter(field_map(), 0, 0);
}

/// Runs the field for some frames with one intent.
/// @param frames  how many
/// @param dir     direction held, or field::kNoDirection
/// @param run     run
void hold(int frames, uint8_t dir, bool run = false)
{
   field::Intent in;
   in.direction = dir;
   in.run = run;
   for (int i = 0; i < frames; ++i)
   {
      field::update(in);
   }
}

/// Lets go and waits until the walker stands.
void settle()
{
   for (int i = 0; i < 200 && field::walker().motion != Motion::Idle; ++i)
   {
      hold(1, field::kNoDirection);
   }
}

/// Takes one step: the direction for one frame, then nothing until standing.
/// @param dir  direction
/// @param run  run
/// @return frames until standing again
int step(uint8_t dir, bool run = false)
{
   hold(1, dir, run);
   int frames = 1;
   for (; frames < 200 && field::walker().motion != Motion::Idle; ++frames)
   {
      hold(1, field::kNoDirection);
   }
   return frames;
}

/// Lookup, cells, border, negative chunk coordinates.
void cells()
{
   CHECK(map::find(asset::key_v1("map:FIELD")) != nullptr);
   CHECK(map::find(asset::key_v1("map:NOWHERE")) == nullptr);
   const map::Map& m = field_map();
   CHECK(map::cell_collision(map::cell_at(m, 6, 4)) == 1);                 // painted wall
   CHECK(map::cell_elevation(map::cell_at(m, 12, 0)) == 4);
   CHECK(map::is_painted(m, 3, 3) && !map::is_painted(m, 7, 6));
   // Unpainted cell and missing chunk: border pattern [wall, tree], phase 1, impassable.
   const map::MapCell u = map::cell_at(m, 7, 6);
   CHECK(u == map::kCellBorder && map::cell_collision(u) == 3 && map::cell_behavior(u) == 0);
   CHECK(m.layer_count == 3 && m.layers[0].bg == 3 && m.layers[2].bg == 1 && m.layers[2].priority == 1);
   bool same = true;
   bool differs = false;
   for (int l = 0; l < m.layer_count; ++l)
   {
      same = same && map::metatile_at(m, l, 7, 6) == map::metatile_at(m, l, -1, 0);   // (7+1)%2 = (-1+1)%2 = 0: wall
      differs = differs || map::metatile_at(m, l, 7, 6) != map::metatile_at(m, l, 8, 8);   // (8+1)%2 = 1: tree
   }
   CHECK(same && differs);
   CHECK(map::metatile_at(m, 2, 8, 8) != map::kNoMetatile &&
         map::metatile_at(m, 2, 7, 6) == map::kNoMetatile);   // tree top
   CHECK(map::metatile_at(m, 7, 0, 0) == map::kNoMetatile);   // no such layer
   CHECK(map::cell_encounter(map::cell_at(m, 1, 5)) && !map::cell_encounter(map::cell_at(m, 0, 0)));
   CHECK(map::behavior(map::cell_at(m, 1, 2)).kind == map::BehaviorKind::Ledge);
   const map::Map& small = *map::find(asset::key_v1("map:SMALL"));
   CHECK(map::is_painted(small, -8, -8) && map::is_painted(small, -1, -1) && !map::is_painted(small, 0, 0));
   CHECK(map::metatile_at(small, 0, 0, 0) != map::kNoMetatile);   // water border
}

/// Tile layers on any BG with their own priority: BG2 at priority 0 is drawn
/// over BG0 at priority 3, BG0's ring is screen block 31, and the field
/// map's BG1 and BG3 are switched off when this map is entered.
void layers()
{
   fresh();
   const map::Map& stack = *map::find(asset::key_v1("map:STACK"));
   CHECK(stack.layer_count == 2 && stack.layers[0].bg == 0 && stack.layers[1].priority == 0);
   CHECK(map::screen_block_of(0) == 31 && map::screen_block_of(1) == 28 && map::screen_block_of(3) == 30);
   map::enter(stack, 0, 0);
   show();
   CHECK(pixel(4, 4) == kGround);   // BG0 ground
   CHECK(pixel(16 + 4, 16 + 4) == kTreeTop);   // BG2 (priority 0) over BG0 (priority 3)
   CHECK((video::read_map_cell(0, 0, 0) & 0x3FF) != 0);   // BG0 holds tiles
   CHECK(pixel(3 * 16 + 4, 2 * 16 + 4) == kGround);   // the field's tree top (BG1, still in VRAM) is off
   map::leave();
   show();
   CHECK(pixel(4, 4) == 0 && pixel(16 + 4, 16 + 4) == 0);   // only the (black) backdrop is left
}

/// The text box over a map (map plan): BG0 moves to character block 2 and
/// screen block 31, the box covers the map without touching its tiles, and
/// closing it shows the map again; a map with a BG0 layer reserves BG0.
void text_over_map()
{
   namespace background = gfx::background;
   namespace layer = text::layer;
   fresh();
   CHECK(background::map_plan() && background::char_block(0) == 2 && background::screen_block(0) == 31);
   CHECK(background::is_reserved(1) && background::is_reserved(3) && !background::is_reserved(0));
   CHECK(background::first_ui_bank() == field_map().palette_count);
   show();
   const uint16_t ground = pixel(4, 4);
   const uint16_t under = pixel(4, 15 * 8 + 4);
   layer::reset();
   layer::open_box(0, 14, 30, 6);
   static constexpr auto kHi = text::encode("HI");
   layer::print(layer::kBoxTarget, 0, 0, layer::Align::Left, false, kHi.codes);
   show();
   CHECK(pixel(0, 14 * 8) == video::rgb5(26, 28, 31));   // frame
   CHECK(pixel(20 * 8 + 3, 17 * 8 + 3) == video::rgb5(3, 5, 12));   // interior
   CHECK(pixel(4, 4) == ground);   // the map above the box is untouched
   bool ink = false;
   for (int y = 15 * 8; y < 16 * 8; ++y)
   {
      for (int x = 8; x < 24; ++x)
      {
         ink = ink || pixel(x, y) == video::rgb5(31, 31, 31);
      }
   }
   CHECK(ink);   // "HI" in style 0's white
   CHECK((video::read_map_cell(0, 1, 15) & 0x3FF) >= background::kTextTiles);   // on screen block 31
   layer::close_box();
   show();
   CHECK(pixel(4, 15 * 8 + 4) == under && pixel(0, 14 * 8) != video::rgb5(26, 28, 31));
   map::set_camera(64, 32);   // scrolling does not touch BG0
   map::update();
   layer::open_box(0, 14, 30, 6);
   show();
   CHECK(pixel(0, 14 * 8) == video::rgb5(26, 28, 31));
   const map::Map& stack = *map::find(asset::key_v1("map:STACK"));
   map::enter(stack, 0, 0);   // BG0 is this map's
   CHECK(background::is_reserved(0) && background::char_block(0) == 0);
   show();
   CHECK(pixel(0, 14 * 8) != video::rgb5(26, 28, 31));   // the box went with the old map
   map::leave();
   CHECK(!background::map_plan() && background::char_block(0) == 0 && background::screen_block(0) == 28);
   CHECK(!background::is_reserved(0) && background::first_ui_bank() == 0);
}

/// The view draws the right colors, the tree top is on BG1, and the walker is
/// drawn under it but over the ground.
void rendering()
{
   fresh();
   show();
   CHECK(pixel(4, 4) == kGround);   // cell (0,0)
   CHECK(pixel(6 * 16 + 4, 4 * 16 + 4) == kWall);   // cell (6,4)
   CHECK(pixel(3 * 16 + 4, 2 * 16 + 4) == kTreeTop);   // tree top
   CHECK((video::read_map_cell(1, 6, 4) & 0x3FF) != 0 &&
         (video::read_map_cell(2, 6, 4) & 0x3FF) != 0);   // tree: top on BG1, ground on BG2
   // Walker under the tree: covered by the top layer.
   gfx::sprites::load(0, *asset::sprite(asset::key_v1("sprite:BLINK")));
   gfx::sprites::show(0, true);
   field::attach_sprite(0, asset::sprite(asset::key_v1("sprite:BLINK")));
   field::place(3, 2, field::DIR_DOWN);
   gfx::sprites::update();
   show();
   const int wx = 3 * 16 - map::camera_x() + 8;
   const int wy = 2 * 16 - map::camera_y() + 8;
   CHECK(pixel(wx, wy) == kTreeTop);
   field::place(3, 3, field::DIR_DOWN);   // in the open: visible
   gfx::sprites::update();
   show();
   CHECK(pixel(3 * 16 - map::camera_x() + 8, 3 * 16 - map::camera_y() + 8) == kWhite);
   field::attach_sprite(0xFF, nullptr);
   gfx::sprites::reset();
}

/// Scrolling streams new metatiles into the ring; far jumps redraw.
void streaming()
{
   fresh();
   for (int x = 0; x <= 8 * 16; x += 4)
   {
      map::set_camera(x, 0);
   }   // to x = 128
   show();
   // Screen column 0 is now map pixel 128: cell (8,0) = push arrow (yellow).
   CHECK(pixel(4, 4) == kYellow);
   CHECK(pixel(2 * 16 + 4, 4) == kWater);   // cell (10,0)
   // Missing chunk (1,1) shows the border (cells 8..15, rows 8..): wall, tree...
   map::set_camera(8 * 16, 8 * 16);
   show();
   CHECK(pixel(16 + 4, 4) == kWall);   // cell (9,8): (9+1)%2 = 0 -> wall
   map::set_camera(-200, -200);   // far jump, outside the chunks: border only
   show();
   const video::Color c = pixel(100, 100);
   CHECK(c == kWall || c == kGround);
   for (int y = -200; y <= 0; y += 8)
   {
      map::set_camera(0, y);
   }
   show();
   CHECK(pixel(6 * 16 + 4, 4 * 16 + 4) == kWall);
}

/// The animated tile alternates its frames every 4 display frames.
void animation()
{
   fresh();
   show();
   const int ax = 5 * 16 + 4;
   const int ay = 2 * 16 + 4;
   const video::Color first = pixel(ax, ay);
   CHECK(first == kWater);   // frame 0 (blue)
   bool changed = false;
   for (int i = 0; i < 8 && !changed; ++i)
   {
      map::update();
      show();
      changed = pixel(ax, ay) == kYellow;
   }
   CHECK(changed);
}

/// The map of the fixed-placement tileset (I2).
/// @param name  "map:FIXED_A" or "map:FIXED_B"
/// @return it
const map::Map& fixed_map(const char* name)
{
   return *map::find(asset::key_v1(name));
}

/// I2: a map of a fixed-placement tileset: its tiles at their VRAM numbers, raw tilemap entries (flips kept), its
/// palette banks as placed, the animation's fixed phase (first change after period - phase frames).
void fixed_tiles()
{
   hal::soft::reset();
   hal::vram::reset();
   gfx::palette::reset();
   gfx::sprites::reset();
   const map::Map& m = fixed_map("map:FIXED_A");
   CHECK(m.tile_count == 8 && m.palette_count == 2 && m.metatile_count == 4);
   CHECK(m.metatiles[1].tiles[1] == 0x402 && m.metatiles[3].tiles[0] == 0x1001);
   CHECK(m.animation_count == 1 && m.animations[0].first_tile == 3 && m.animations[0].period == 4 &&
         m.animations[0].phase == 1);
   map::enter(m, 0, 0);
   show();
   const int glow = 3 * 16 + 4;
   CHECK(pixel(4, 4) == kGround);   // tile 1, bank 0 entry 1
   CHECK(pixel(16 + 4, 16 + 4) == kWall);   // tile 2 (flipped copies), entry 2
   CHECK(pixel(5 * 16 + 4, 5 * 16 + 4) == kWhite);   // bank 1's entry 1
   CHECK(pixel(glow, glow) == kWater);   // the animation's frame 0: index 4
   map::update();
   map::update();
   show();
   CHECK(pixel(glow, glow) == kWater);
   map::update();   // the third frame: period 4 - phase 1
   show();
   CHECK(pixel(glow, glow) == kYellow);   // frame 1: index 5
   map::leave();
}

/// I2: a behavior's walls block leaving its cell through them and entering it through them (fixed_a's fence on
/// the east side of 2,4).
void walls()
{
   const map::Map& m = fixed_map("map:FIXED_A");
   map::enter(m, 0, 0);
   bool jump = false;
   CHECK(field::check_step(2, 4, field::DIR_RIGHT, 0, 0, &jump) == field::Blocked::Collision);   // out east
   CHECK(field::check_step(3, 4, field::DIR_LEFT, 0, 0, &jump) == field::Blocked::Collision);   // in from east
   CHECK(field::check_step(1, 4, field::DIR_RIGHT, 0, 0, &jump) == field::Blocked::None);   // in from west
   CHECK(field::check_step(2, 4, field::DIR_UP, 0, 0, &jump) == field::Blocked::None);   // out north
   CHECK(field::check_step(2, 4, field::DIR_LEFT, 0, 0, &jump) == field::Blocked::None);   // out west
   map::leave();
}

/// I2: field::shift_map() shows another map under a moving walker: position, the cell left and the camera move by
/// the shift, the step carries on, and an animation both maps run keeps its frame (map::enter(keep_animations)).
void shift()
{
   hal::soft::reset();
   hal::vram::reset();
   gfx::palette::reset();
   gfx::sprites::reset();
   const map::Map& a = fixed_map("map:FIXED_A");
   const map::Map& b = fixed_map("map:FIXED_B");
   field::enter(map::index_of(a), 4, 6, field::DIR_RIGHT);
   for (int i = 0; i < 3; ++i)
   {
      map::update();   // the shared animation reaches its frame 1
   }
   hold(1, field::DIR_RIGHT);
   const field::Walker before = field::walker();
   const int32_t cam_x = map::camera_x();
   const int32_t cam_y = map::camera_y();
   CHECK(before.motion == Motion::Walking && before.x == 5 && before.y == 6);
   field::shift_map(map::index_of(b), -3, 1);
   const field::Walker& w = field::walker();
   CHECK(map::current() == &b);
   CHECK(w.x == 2 && w.y == 7 && w.motion == Motion::Walking && w.remaining == before.remaining);
   CHECK(w.px == before.px - 3 * 16 && w.py == before.py + 16);
   CHECK(map::camera_x() == cam_x - 3 * 16 && map::camera_y() == cam_y + 16);
   show();
   CHECK(pixel(4 * 16 + 4 - map::camera_x(), 4 * 16 + 4 - map::camera_y()) == kYellow);   // kept: frame 1
   settle();
   CHECK(field::walker().x == 2 && field::walker().y == 7 && field::walker().motion == Motion::Idle);
   map::leave();
}

/// Walking, turning, collision, terrain, running, noclip.
void walking()
{
   fresh();
   field::set_noclip(false);
   field::place(1, 1, field::DIR_RIGHT);
   CHECK(step(field::DIR_RIGHT) == 16);
   CHECK(field::walker().x == 2 && field::walker().px == 32);
   hold(15, field::DIR_RIGHT);   // held: steps follow each other without a standing frame
   hold(1, field::DIR_RIGHT);
   CHECK(field::walker().x == 4 && field::walker().px == 48);   // arrived at 3, already heading to 4
   settle();
   // Turning: a new direction first turns in place.
   hold(1, field::DIR_UP);
   CHECK(field::walker().motion == Motion::Turning && field::walker().y == 1);
   settle();
   // Collision: (7,4) -> (6,4) is a wall.
   field::place(7, 4, field::DIR_LEFT);
   hold(4, field::DIR_LEFT);
   CHECK(field::walker().x == 7 && field::walker().last_blocked == Blocked::Collision);
   // Terrain: water is swim-only.
   field::place(9, 0, field::DIR_RIGHT);
   hold(4, field::DIR_RIGHT);
   CHECK(field::walker().x == 9 && field::walker().last_blocked == Blocked::Terrain);
   // Running: 8 frames per cell.
   field::place(0, 3, field::DIR_RIGHT);
   CHECK(step(field::DIR_RIGHT, true) == 8);
   CHECK(field::walker().x == 1 && field::walker().px == 16);
   // Noclip walks into the wall.
   field::set_noclip(true);
   field::place(7, 4, field::DIR_LEFT);
   step(field::DIR_LEFT);
   CHECK(field::walker().x == 6);
   field::set_noclip(false);
   // Border cells are impassable.
   field::place(7, 5, field::DIR_DOWN);
   hold(4, field::DIR_DOWN);
   CHECK(field::walker().y == 5 && field::walker().last_blocked == Blocked::Collision);
}

/// Ledges, ice, push tiles, elevation, encounter steps.
void behaviors()
{
   fresh();
   // Ledge (1,2) facing down: jump from (1,1) to (1,3).
   field::place(1, 1, field::DIR_DOWN);
   hold(1, field::DIR_DOWN);
   CHECK(field::walker().motion == Motion::Jumping && field::walker().y == 3);
   bool lifted = false;
   for (int i = 0; i < 20; ++i)
   {
      hold(1, field::kNoDirection);
      lifted = lifted || field::walker().lift > 0;
   }
   CHECK(field::walker().y == 3 && field::walker().py == 48 && lifted && field::walker().lift == 0);
   // Moving up into the ledge is blocked.
   field::place(1, 3, field::DIR_UP);
   hold(4, field::DIR_UP);
   CHECK(field::walker().y == 3 && field::walker().last_blocked == Blocked::Ledge);
   // Ice: from (1,4) one step right slides over (2..5,4) and stops at the wall.
   field::place(1, 4, field::DIR_RIGHT);
   hold(16, field::DIR_RIGHT);
   settle();
   CHECK(field::walker().x == 5 && field::walker().y == 4);
   // Push: stepping onto (8,0) forces a step right to (9,0).
   field::place(7, 0, field::DIR_RIGHT);
   hold(16, field::DIR_RIGHT);
   settle();
   CHECK(field::walker().x == 9);
   // Elevation: (11,0) -> (12,0) level 4 -> (13,0) -> (14,0) level 3 blocked.
   field::place(11, 0, field::DIR_RIGHT);
   step(field::DIR_RIGHT);
   CHECK(field::walker().x == 12 && field::walker().elevation == 4);
   step(field::DIR_RIGHT);
   hold(4, field::DIR_RIGHT);
   CHECK(field::walker().x == 13 && field::walker().last_blocked == Blocked::Elevation);
   // The bridge (15) is reachable from level 4 and keeps the level.
   field::place(12, 0, field::DIR_DOWN);
   CHECK(field::walker().elevation == 4);
   step(field::DIR_DOWN);
   CHECK(field::walker().y == 1 && field::walker().elevation == 4);
   // A level-0 cell (stairs) gives the walker level 0, so it can step off onto
   // another level: (13,0) level 4 -> (13,1) level 0 -> (14,1) level 0
   // -> (14,0) level 3 (a new direction turns first, so turns take a step()).
   field::place(13, 0, field::DIR_DOWN);
   CHECK(field::walker().elevation == 4);
   step(field::DIR_DOWN);
   CHECK(field::walker().y == 1 && field::walker().elevation == 0);
   step(field::DIR_RIGHT);
   step(field::DIR_RIGHT);
   CHECK(field::walker().x == 14 && field::walker().elevation == 0);
   step(field::DIR_UP);
   step(field::DIR_UP);
   CHECK(field::walker().x == 14 && field::walker().y == 0 && field::walker().elevation == 3);
   // Grass counts encounter steps. (1,5) always starts an encounter (D-12): the walker
   // waits until it is taken; (2,5) never does.
   field::place(0, 5, field::DIR_RIGHT);
   step(field::DIR_RIGHT);
   step(field::DIR_RIGHT);
   CHECK(field::walker().x == 1 && field::walker().steps == 1);
   encounter::Result r{};
   CHECK(field::take_encounter(&r) && r.method == encounter_ids::ENCOUNTER_METHOD_WALK);
   CHECK(!field::take_encounter(nullptr));
   step(field::DIR_RIGHT);
   CHECK(!field::take_encounter(nullptr));
   CHECK(field::walker().encounter_steps == 2 && field::walker().steps == 2);
   // The camera stays centred on the walker.
   CHECK(map::camera_x() == field::walker().px + 8 - 120 && map::camera_y() == field::walker().py + 8 - 80);
}

}   // namespace

/// Species index by debug name.
/// @param name  species name
/// @return index, or 0xFFFF
uint16_t species(const char* name)
{
   const encounter::Bank& b = encounter::bank();
   for (uint16_t i = 0; i < b.species_name_count; ++i)
   {
      if (std::strcmp(b.species_names[i], name) == 0)
      {
         return i;
      }
   }
   return 0xFFFF;
}

/// Encounter tables: lookup by slot and method, conditions, modifiers,
/// trigger odds, step and script rolls, and the walker stopping.
void encounters()
{
   using namespace encounter_ids;
   const encounter::Bank& b = encounter::bank();
   const map::Map& m = field_map();
   CHECK(b.method_count == 3 && b.condition_count == 2 && b.table_count == 4);
   CHECK(b.methods[ENCOUNTER_METHOD_SWIM].trigger == encounter::Trigger::Step && b.methods[1].vehicles == 2);
   CHECK(b.methods[ENCOUNTER_METHOD_OLD_LURE].trigger == encounter::Trigger::Script);
   CHECK(std::strcmp(b.method_names[ENCOUNTER_METHOD_OLD_LURE], "old lure") == 0);
   CHECK(encounter::table_for(m, 0, ENCOUNTER_METHOD_WALK) != encounter::kNoTable);
   CHECK(encounter::table_for(m, 0, ENCOUNTER_METHOD_SWIM) == encounter::kNoTable);
   CHECK(map::cell_encounter_slot(map::cell_at(m, 2, 5)) == 1 && map::cell_encounter_slot(map::cell_at(m, 10, 0)) == 2);
   const uint16_t grass = encounter::table_for(m, 0, ENCOUNTER_METHOD_WALK);
   const uint16_t sprout = species("Sproutle"), owl = species("Duskowl"), volt = species("Voltmink");
   const uint16_t pup = species("Tidepup");
   CHECK(sprout != 0xFFFF && owl != 0xFFFF && volt != 0xFFFF && pup != 0xFFFF);

   // Conditions filter entries; a x0 modifier suppresses one.
   const uint32_t night = 1u << ENCOUNTER_CONDITION_NIGHT, rain = 1u << ENCOUNTER_CONDITION_RAIN;
   const encounter::Table& t = b.tables[grass];
   CHECK(t.entry_count == 3 && encounter::entry_weight(b.entries[t.entry_first], 0) == 60);
   CHECK(encounter::entry_weight(b.entries[t.entry_first], night) == 0);
   CHECK(encounter::entry_weight(b.entries[t.entry_first + 1], 0) == 0);
   CHECK(encounter::entry_weight(b.entries[t.entry_first + 1], night) == 40);
   CHECK(encounter::entry_weight(b.entries[t.entry_first + 2], rain) == 0);
   rng::Stream r{12345};
   int seen_sprout = 0, seen_owl = 0, seen_volt = 0, bad = 0;
   for (int i = 0; i < 400; ++i)
   {
      encounter::Result e{};
      const uint32_t active = (i & 1) ? night : rain;
      if (!encounter::pick(grass, active, r, &e))
      {
         ++bad;
         continue;
      }
      if (e.species == sprout)
      {
         ++seen_sprout;
         bad += (active & night) != 0 || e.level < 3 || e.level > 5;
      }
      else if (e.species == owl)
      {
         ++seen_owl;
         bad += (active & night) == 0 || e.level != 4;
      }
      else if (e.species == volt)
      {
         ++seen_volt;
         bad += (active & rain) != 0;
      }
      else
      {
         ++bad;
      }
   }
   CHECK(bad == 0 && seen_sprout > 0 && seen_owl > 0 && seen_volt > 0);
   CHECK(seen_volt < seen_owl);   // night: 40 vs 10

   // Steps: slot 0 always rolls on foot, slot 1 never, water only while swimming.
   encounter::set_conditions(0);
   encounter::Result e{};
   CHECK(encounter::roll_step(m, map::cell_at(m, 1, 5), 0, r, &e) && e.method == ENCOUNTER_METHOD_WALK);
   CHECK(!encounter::roll_step(m, map::cell_at(m, 2, 5), 0, r, &e));
   CHECK(!encounter::roll_step(m, map::cell_at(m, 0, 0), 0, r, &e));
   CHECK(!encounter::roll_step(m, map::cell_at(m, 10, 0), 0, r, &e));
   CHECK(encounter::roll_step(m, map::cell_at(m, 10, 0), 1, r, &e) && e.species == pup && e.level >= 10 &&
         e.level <= 12 && e.method == ENCOUNTER_METHOD_SWIM);
   // Script methods roll from game code for a cell.
   CHECK(encounter::roll_at(m, 10, 0, ENCOUNTER_METHOD_OLD_LURE, r, &e) && e.species == pup && e.level == 5);
   CHECK(!encounter::roll_at(m, 1, 5, ENCOUNTER_METHOD_OLD_LURE, r, &e));
   CHECK(!encounter::roll_at(m, 7, 6, ENCOUNTER_METHOD_WALK, r, &e));   // unpainted
   // Every map without encounters has no refs.
   CHECK(map::kWorld.maps[1].encounter_ref_count == 0 && map::kWorld.maps[1].encounter_refs == nullptr);

   // The walker: off, then on again.
   fresh();
   field::set_encounters(false);
   field::place(0, 5, field::DIR_RIGHT);
   step(field::DIR_RIGHT);
   CHECK(field::walker().x == 1 && !field::take_encounter(nullptr));
   field::set_encounters(true);
   field::place(0, 5, field::DIR_RIGHT);
   step(field::DIR_RIGHT);
   CHECK(field::take_encounter(&e) && (e.species == sprout || e.species == volt));
}

/// Moves one cell: turns first when facing elsewhere (a turn takes its own step()).
/// @param dir  direction
void go(uint8_t dir)
{
   if (field::walker().facing != dir)
   {
      step(dir);
   }
   step(dir);
}

/// Runs a requested script to its end (the test scripts never wait).
/// @param e  the request
void run(const field::FieldEvent& e)
{
   script::Context c{};
   script::start(c, script::ScriptId{e.script}, e.object);
   for (int i = 0; i < 10 && script::update(c); ++i)
   {
      script::frame();
   }
}

/// Map events (B-3): bucketing, triggers with conditions, step and press
/// warps between maps and areas, signs with facing and flags, step hooks.
void events()
{
   using namespace world_ids;
   const map::Map& m = field_map();
   script::clear_all();
   // Records: buckets per chunk, local positions, landings resolved.
   CHECK(m.event_count == 4 && m.warp_count == 2 && m.area == MAP_FIELD);
   CHECK(map::kWorld.maps[MAP_SMALL].area == MAP_FIELD && map::kWorld.maps[MAP_STACK].area == MAP_STACK);
   const map::Event* w0 = map::find_event(m, 3, 9, map::EventKind::Warp, 0);
   CHECK(w0 != nullptr && w0->local == 0x13 && m.warps[w0->target].map == MAP_SMALL && m.warps[w0->target].x == -4 &&
         m.warps[w0->target].y == -4);
   CHECK(map::find_event(m, 3, 9, map::EventKind::Warp, 7) != nullptr);   // level 0: any
   CHECK(map::find_event(m, 3, 9, map::EventKind::Sign, 0) == nullptr);
   CHECK(map::find_event(m, 3, 1, map::EventKind::Warp, 0) == nullptr);   // same local cell, other chunk
   CHECK(m.chunks[0].event_count == 0 && m.chunks[2].event_count == 4);
   CHECK(map::kWorld.step_hook_count == 3 && map::kWorld.step_hooks[STEP_HOOK_FOUR].period_mask == 3 &&
         map::kWorld.step_hooks[STEP_HOOK_FOUR].phase == 1);

   fresh();
   field::enter(MAP_FIELD, 4, 9, field::DIR_LEFT);
   CHECK(script::var(script_ids::VAR_LOADS) == 1);   // on_load ran
   // Trigger at (5,9) fires while gate == 0: the walker stops and waits.
   field::FieldEvent e{};
   const uint32_t before = field::walker().steps;
   go(field::DIR_RIGHT);
   CHECK(field::walker().x == 5 && field::has_event());
   go(field::DIR_RIGHT);   // waits for the game
   CHECK(field::walker().x == 5);
   CHECK(field::take_event(&e) && e.what == field::Request::Script && e.source == field::Source::Trigger &&
         e.script == static_cast<uint16_t>(script_ids::SCRIPT_STEP_ON));
   run(e);
   CHECK(script::var(script_ids::VAR_GATE) == 1);
   go(field::DIR_LEFT);
   go(field::DIR_RIGHT);
   CHECK(field::walker().x == 5 && !field::has_event());   // gate is 1 now: no trigger
   CHECK(field::walker().steps == before + 3);

   // Sign at (2,11): only facing down from (2,10), and only until sign_gone is set.
   field::place(2, 10, field::DIR_LEFT);
   field::Intent in;
   in.interact = true;
   field::update(in);
   CHECK(!field::has_event());   // facing left
   field::place(2, 10, field::DIR_DOWN);
   field::update(in);
   CHECK(field::take_event(&e) && e.source == field::Source::Sign &&
         e.script == static_cast<uint16_t>(script_ids::SCRIPT_READ_SIGN) && e.at_x == 2 && e.at_y == 11);
   run(e);
   field::update(in);
   CHECK(!field::has_event());   // gone

   // Step warp at (3,9) to small (same area: the area variable survives, the map one clears).
   field::place(4, 9, field::DIR_LEFT);
   go(field::DIR_LEFT);
   CHECK(field::take_event(&e) && e.what == field::Request::Warp && e.map == MAP_SMALL && e.x == -4 && e.y == -4);
   const uint32_t steps = field::walker().steps;
   field::warp(e);
   CHECK(map::current() == &map::kWorld.maps[MAP_SMALL] && field::walker().x == -4 && field::walker().y == -4);
   CHECK(field::walker().facing == field::DIR_LEFT && field::walker().steps == steps && !field::has_event());
   CHECK(script::var(script_ids::VAR_GATE) == 0 && script::var(script_ids::VAR_LOADS) == 1);
   // ... and back: landing on field's warp 0 does not warp again.
   go(field::DIR_RIGHT);
   go(field::DIR_LEFT);
   CHECK(field::walker().x == -4 && field::take_event(&e) && e.map == MAP_FIELD && e.x == 3 && e.y == 9);
   field::warp(e);
   CHECK(map::current() == &field_map() && !field::has_event());
   CHECK(script::var(script_ids::VAR_LOADS) == 2);   // same area: kept, then +1

   // Press warp at (0,12): standing there, pressing left (into the border) warps to stack, another area.
   field::place(0, 12, field::DIR_DOWN);
   hold(1, field::DIR_LEFT);
   CHECK(field::take_event(&e) && e.map == MAP_STACK && e.x == 1 && e.y == 1 &&
         field::walker().facing == field::DIR_LEFT);
   hold(1, field::DIR_UP);
   CHECK(!field::has_event());   // other directions do not
   field::warp(e);
   CHECK(script::var(script_ids::VAR_LOADS) == 0);   // area left: cleared (stack has no load script)

   // Step hooks: every_step each step, even on even step counts, four when steps % 4 == 1.
   field::enter(MAP_FIELD, 0, 3, field::DIR_RIGHT);
   for (uint32_t& c : g_hook_calls)
   {
      c = 0;
   }
   const uint32_t s0 = field::walker().steps;
   for (int i = 0; i < 8; ++i)
   {
      go(field::DIR_RIGHT);
      go(field::DIR_LEFT);
   }
   CHECK(field::walker().steps == s0 + 16);
   CHECK(g_hook_calls[STEP_HOOK_EVERY_STEP] == 16 && g_hook_calls[STEP_HOOK_EVEN] == 8 &&
         g_hook_calls[STEP_HOOK_FOUR] == 4 && (g_hook_steps[STEP_HOOK_FOUR] & 3) == 1);
   // A hook asking for a script stops the walker like a trigger.
   g_hook_script = static_cast<uint16_t>(script_ids::SCRIPT_HOOK_SCRIPT);
   for (int i = 0; i < 4 && !field::has_event(); ++i)
   {
      go(field::DIR_RIGHT);
      go(field::DIR_LEFT);
   }
   CHECK(field::take_event(&e) && e.source == field::Source::StepHook &&
         e.script == static_cast<uint16_t>(script_ids::SCRIPT_HOOK_SCRIPT));
   g_hook_script = field::kNoScript;
   // Noclip skips events.
   field::set_noclip(true);
   field::place(4, 9, field::DIR_LEFT);
   go(field::DIR_LEFT);
   CHECK(!field::has_event());
   field::set_noclip(false);
}

/// Runs the field with no input.
/// @param frames  how many frames
void idle(int frames)
{
   hold(frames, field::kNoDirection);
}

/// Map objects (B-21): spawning with hide flags and overrides, collision,
/// talking, rival sight and approach, own movement inside the range,
/// scripted walks of objects and the walker, and locking.
void objects()
{
   using namespace world_ids;
   namespace ids = script_ids;
   script::clear_all();
   fresh();
   const map::Map& town = map::kWorld.maps[MAP_TOWN];
   CHECK(town.object_count == 5 && town.object_page_count == 1);
   CHECK(town.objects[OBJ_TOWN_RIVAL].sight == 3 && town.objects[OBJ_TOWN_PACER].range == 0x02 &&
         town.objects[OBJ_TOWN_WANDERER].movement == map::Movement::Wander);
   field::enter(MAP_TOWN, 2, 0, field::DIR_DOWN);
   CHECK(field::object_count() == 5 && field::object(5) == nullptr);
   const field::Object& guard = *field::object(OBJ_TOWN_GUARD);
   CHECK(guard.active && guard.x == 2 && guard.y == 2 && guard.facing == field::DIR_DOWN &&
         guard.script == static_cast<uint16_t>(ids::SCRIPT_TALK_GUARD) && guard.sprite != nullptr);
   CHECK(field::object(OBJ_TOWN_GHOST)->active && field::object(OBJ_TOWN_RIVAL)->sprite == nullptr);

   // Collision: the guard blocks the walker.
   go(field::DIR_DOWN);
   CHECK(field::walker().y == 1);
   go(field::DIR_DOWN);
   CHECK(field::walker().y == 1 && field::walker().last_blocked == Blocked::Object);

   // Talking: the guard turns to the walker; objects lock until release().
   field::Intent talk;
   talk.interact = true;
   field::update(talk);
   field::FieldEvent e{};
   CHECK(field::take_event(&e) && e.source == field::Source::Object && e.object == OBJ_TOWN_GUARD &&
         e.script == static_cast<uint16_t>(ids::SCRIPT_TALK_GUARD) && e.at_x == 2 && e.at_y == 2);
   CHECK(guard.facing == field::DIR_UP && field::locked());
   const field::Object& wanderer = *field::object(OBJ_TOWN_WANDERER);
   const int16_t wx = wanderer.x;
   const int16_t wy = wanderer.y;
   idle(400);
   CHECK(wanderer.x == wx && wanderer.y == wy && guard.facing == field::DIR_UP);   // locked: nobody moves
   field::release();
   idle(1);
   CHECK(!field::locked() && guard.facing == field::DIR_DOWN);   // a static object turns back

   // Own movement stays inside the range box, off the walker and other objects.
   bool wandered = false;
   bool paced = false;
   bool inside = true;
   const field::Object& pacer = *field::object(OBJ_TOWN_PACER);
   for (int f = 0; f < 3000; ++f)
   {
      idle(1);
      wandered = wandered || wanderer.x != 5 || wanderer.y != 6;
      paced = paced || pacer.x != 3;
      inside = inside && wanderer.x >= 4 && wanderer.x <= 6 && wanderer.y >= 5 && wanderer.y <= 7 && pacer.x >= 1 &&
               pacer.x <= 5 && pacer.y == 7 && !(wanderer.x == pacer.x && wanderer.y == pacer.y);
   }
   CHECK(wandered && paced && inside);

   // Overrides and hide flags apply when the map is entered.
   script::set_flag(ids::FLAG_GUARD_MOVED, true);
   script::set_flag(ids::FLAG_GHOST_GONE, true);
   field::enter(MAP_TOWN, 1, 5, field::DIR_DOWN);
   CHECK(guard.facing == field::DIR_LEFT && guard.script == static_cast<uint16_t>(ids::SCRIPT_GUARD_AFTER));
   CHECK(!field::object(OBJ_TOWN_GHOST)->active);
   go(field::DIR_DOWN);   // where the ghost would stand
   CHECK(field::walker().y == 6);
   field::show_object(OBJ_TOWN_GHOST, true);   // shown again by a script, on its template cell
   CHECK(field::object(OBJ_TOWN_GHOST)->active);
   field::show_object(OBJ_TOWN_GHOST, false);
   CHECK(!field::object(OBJ_TOWN_GHOST)->active);

   // Load scripts run before objects spawn: on_transition hides the ghost through its flag.
   script::clear_all();
   script::set_flag(ids::FLAG_HIDE_GHOST_ON_ENTRY, true);
   field::enter(MAP_TOWN, 0, 0, field::DIR_DOWN);
   CHECK(!field::object(OBJ_TOWN_GHOST)->active);
   // release() re-applies overrides (a flag set by the script counts at once),
   // turns Static objects back, and waits while another request is pending.
   script::clear_all();
   field::enter(MAP_TOWN, 2, 1, field::DIR_DOWN);
   field::update(talk);
   CHECK(field::locked() && field::has_event() && guard.facing == field::DIR_UP);
   script::set_flag(ids::FLAG_GUARD_MOVED, true);
   field::release();
   CHECK(field::locked() && guard.facing == field::DIR_UP);   // the talk request is still pending
   CHECK(field::take_event(&e) && e.object == OBJ_TOWN_GUARD);
   field::release();
   CHECK(!field::locked() && guard.facing == field::DIR_LEFT &&
         guard.script == static_cast<uint16_t>(ids::SCRIPT_GUARD_AFTER));
   field::face_object(OBJ_TOWN_GUARD, field::DIR_RIGHT);   // a script's facing sticks until release()
   idle(5);
   CHECK(guard.facing == field::DIR_RIGHT);
   field::lock();
   field::release();
   CHECK(guard.facing == field::DIR_LEFT && !field::locked());
   CHECK(e.what == field::Request::Script && field::FieldEvent{}.object == field::kNoObject);

   // Rival sight: out of range at 4 cells; spotted at 3; waits, walks up, the walker turns to it.
   script::clear_all();
   field::enter(MAP_TOWN, 6, 5, field::DIR_UP);
   idle(5);
   CHECK(!field::has_event() && !field::locked());
   go(field::DIR_UP);
   CHECK(field::walker().y == 4 && !field::has_event() && field::locked());   // spotted: approaching
   const field::Object& rival = *field::object(OBJ_TOWN_RIVAL);
   idle(field::kSpotFrames - 1);
   CHECK(!rival.moving && rival.y == 1);
   idle(2);
   CHECK(rival.moving);
   idle(40);
   CHECK(rival.y == 3 && !rival.moving && field::walker().facing == field::DIR_UP);
   CHECK(field::take_event(&e) && e.source == field::Source::Spotter && e.object == OBJ_TOWN_RIVAL &&
         e.script == static_cast<uint16_t>(ids::SCRIPT_RIVAL_BATTLE));
   run(e);   // sets beat_rival
   field::release();
   // Hiding an approaching rival, or placing the walker, ends the approach and its lock.
   script::set_flag(ids::FLAG_BEAT_RIVAL, false);
   field::place(6, 5, field::DIR_UP);
   go(field::DIR_UP);
   CHECK(field::locked() && !field::has_event());
   field::show_object(OBJ_TOWN_RIVAL, false);
   CHECK(!field::locked());
   field::show_object(OBJ_TOWN_RIVAL, true);
   field::place(6, 5, field::DIR_UP);
   go(field::DIR_UP);
   CHECK(field::locked());
   field::place(6, 6, field::DIR_UP);
   CHECK(!field::locked());
   idle(60);
   CHECK(!field::has_event());
   script::set_flag(ids::FLAG_BEAT_RIVAL, true);
   field::enter(MAP_TOWN, 6, 4, field::DIR_UP);
   go(field::DIR_DOWN);
   go(field::DIR_UP);
   CHECK(!field::has_event() && !field::locked());   // defeated: its sight is off

   // Scripted walks: objects and the walker go through anything, without events or step counts.
   field::enter(MAP_TOWN, 0, 0, field::DIR_DOWN);
   CHECK(field::move_object(OBJ_TOWN_GUARD, field::DIR_UP, 2) && field::object_moving(OBJ_TOWN_GUARD));
   idle(16);
   CHECK(guard.py == 16 && guard.y == 0 && field::object_moving(OBJ_TOWN_GUARD));   // on its second step
   idle(16);
   CHECK(guard.y == 0 && !field::object_moving(OBJ_TOWN_GUARD) && guard.facing == field::DIR_UP);
   field::show_object(OBJ_TOWN_GHOST, false);
   CHECK(!field::move_object(OBJ_TOWN_GHOST, field::DIR_UP, 1) && !field::object_moving(OBJ_TOWN_GHOST));
   const uint32_t steps = field::walker().steps;
   field::move_walker(field::DIR_RIGHT, 3);   // through the guard at (2,0)
   CHECK(field::walker_moving());
   idle(48);
   CHECK(field::walker().x == 3 && !field::walker_moving() && field::walker().steps == steps);
   field::face_walker(field::DIR_LEFT);
   field::face_object(OBJ_TOWN_GUARD, field::DIR_RIGHT);
   CHECK(field::walker().facing == field::DIR_LEFT && guard.facing == field::DIR_RIGHT);

   // A talk script moving its own object and the player (script::world through the field).
   field::enter(MAP_TOWN, 1, 5, field::DIR_DOWN);
   const field::Object& ghost = *field::object(OBJ_TOWN_GHOST);
   field::update(talk);
   CHECK(field::take_event(&e) && e.object == OBJ_TOWN_GHOST && ghost.facing == field::DIR_UP);
   script::Context c{};
   script::start(c, script::ScriptId{e.script}, e.object);
   int frames = 0;
   for (; frames < 100 && script::update(c); ++frames)
   {
      script::frame();
      idle(1);
   }
   CHECK(frames > 30 && frames < 40);   // two walks of 16 frames
   CHECK(!ghost.active && ghost.x == 2 && ghost.y == 6 && field::walker().y == 6 && field::walker().x == 1);
   field::release();
}

/// Runs the overworld runner for some frames with one intent.
/// @param frames  how many
/// @param in      the input
void ow(int frames, const field::Intent& in = field::Intent{})
{
   for (int i = 0; i < frames; ++i)
   {
      overworld::update(in);
   }
}

/// Runs the runner until no script or transition runs (at most @p limit frames).
/// @param limit  frame limit
/// @return frames it took
int ow_settle(int limit = 600)
{
   int n = 0;
   for (; n < limit && (overworld::script_running() || overworld::transitioning()); ++n)
   {
      overworld::update({});
   }
   return n;
}

/// One step with the runner: a direction until the walker moves, then until it stands.
/// @param dir  direction
void ow_go(uint8_t dir)
{
   field::Intent in;
   in.direction = dir;
   if (field::walker().facing != dir)
   {
      ow(1, in);
      ow(10);
   }
   ow(1, in);
   for (int i = 0; i < 40 && field::walker().motion != Motion::Idle; ++i)
   {
      ow(1);
   }
}

/// RPG Maker XP-style eventing and the runner (D-17): Autorun, Player
/// Touch, Event Touch, Custom routes, self switches and pages, Parallel
/// scripts, screen effects and the timer, banners, warps with
/// transitions and the first-visit location preview.
void eventing()
{
   using namespace world_ids;
   namespace ids = script_ids;
   script::clear_all();
   fresh();
   for (uint32_t i = 0; i < overworld::kLocations.visited_bytes; ++i)
   {
      overworld::kLocations.visited[i] = 0;
   }
   CHECK(overworld::kLocations.location_count == 2 && map::kWorld.maps[MAP_PLAZA].location == 0 &&
         map::kWorld.maps[MAP_TOWN].location == map::kNoLocation);
   // Field placeholders: the garden's name shows the name slot {hero} (see the warp below).
   text::set_field_resolver(script::placeholder_text);
   static constexpr auto kHero = text::encode("Mo");
   script::set_name(ids::PLACEHOLDER_HERO, kHero.codes);
   overworld::start(MAP_PLAZA, 4, 4, field::DIR_DOWN);
   CHECK(overworld::banner_shown());
   // Autorun: its script runs at once; its flag hides it through a page.
   ow(2);
   CHECK(overworld::last_source() == field::Source::Autorun);
   ow_settle();
   CHECK(script::flag(ids::FLAG_AUTO_DONE) && !field::object(OBJ_PLAZA_AUTO)->active &&
         field::object(OBJ_PLAZA_AUTO)->page == field::kHiddenPage && !field::locked());
   CHECK(!overworld::banner_shown());   // a script closes the banner
   // Parallel: its script runs alongside, again and again.
   const int32_t t0 = script::var(ids::VAR_TICKS);
   ow(45);
   CHECK(script::var(ids::VAR_TICKS) >= t0 + 3);
   // Custom route: the runner walks between its cells, repeating.
   bool moved = false;
   for (int f = 0; f < 120; ++f)
   {
      ow(1);
      moved = moved || field::object(OBJ_PLAZA_RUNNER)->x == 3;
   }
   CHECK(moved && field::object(OBJ_PLAZA_RUNNER)->x >= 1 && field::object(OBJ_PLAZA_RUNNER)->x <= 3);
   // Player Touch: walking into it fires its script.
   field::place(3, 3, field::DIR_UP);
   ow_go(field::DIR_UP);
   CHECK(field::walker().y == 2);
   ow_go(field::DIR_UP);
   ow_settle();
   CHECK(script::var(ids::VAR_HITS) == 1 && overworld::last_source() == field::Source::Touch);
   // Event Touch: it walks into the walker (a route).
   field::place(6, 4, field::DIR_DOWN);
   static constexpr uint8_t kUp2[] = {0, 1, field::DIR_UP, 2, 0};
   CHECK(field::route_object(OBJ_PLAZA_BUMPER, kUp2, false));
   for (int f = 0; f < 60 && script::var(ids::VAR_BUMPS) == 0; ++f)
   {
      ow(1);
   }
   ow_settle();
   CHECK(script::var(ids::VAR_BUMPS) == 1 && field::object(OBJ_PLAZA_BUMPER)->y == 5);
   // Self switches and pages: the talk sets A; page 1 takes over.
   field::place(5, 2, field::DIR_UP);
   field::Intent talk;
   talk.interact = true;
   ow(1, talk);
   ow_settle();
   CHECK(field::self_switch(OBJ_PLAZA_SW, 0) && field::object(OBJ_PLAZA_SW)->page == 1 &&
         field::object(OBJ_PLAZA_SW)->script == static_cast<uint16_t>(ids::SCRIPT_SELF_AFTER));
   CHECK(map::kWorld.self_switches != nullptr && map::kWorld.self_switch_bytes >= 1);
   // Screen effects and the timer from a sign's script; the camera pans and comes back.
   field::place(2, 6, field::DIR_DOWN);
   const int32_t cam0 = map::camera_x();
   ow(1, talk);
   bool panned = false;
   for (int f = 0; f < 200 && overworld::script_running(); ++f)
   {
      ow(1);
      panned = panned || map::camera_x() >= cam0 + 30;
   }
   CHECK(panned && map::camera_x() == cam0 && !overworld::script_running());
   CHECK(overworld::timer_seconds() >= 1 && overworld::timer_seconds() <= 2);
   ow(130);
   CHECK(overworld::timer_seconds() == 0);
   // A script's route on a Custom object: its own route takes over again when it ends.
   static constexpr uint8_t kFaceUp[] = {0, 9, field::DIR_UP, 0};
   field::place(4, 4, field::DIR_DOWN);   // out of the runner's way
   CHECK(field::route_object(OBJ_PLAZA_RUNNER, kFaceUp, true));
   for (int f = 0; f < 10 && field::routes_busy(); ++f)
   {
      ow(1);
   }
   CHECK(!field::routes_busy());
   const int16_t rx = field::object(OBJ_PLAZA_RUNNER)->x;
   bool resumed = false;
   for (int f = 0; f < 120 && !resumed; ++f)
   {
      ow(1);
      resumed = field::object(OBJ_PLAZA_RUNNER)->x != rx;
   }
   CHECK(resumed);
   // place: the script's route stops there.
   static constexpr uint8_t kWaitLong[] = {0, 8, 0xFF, 0x7F, 0};
   CHECK(field::route_object(OBJ_PLAZA_BUMPER, kWaitLong, true) && field::routes_busy());
   field::place_object(OBJ_PLAZA_BUMPER, 6, 5, field::NUM_DIRECTIONS);
   CHECK(!field::routes_busy());
   // A move asked for during a jump waits for the landing, then walks.
   static constexpr uint8_t kJump[] = {0, 7, 0, 0, 0};
   CHECK(field::route_object(OBJ_PLAZA_BUMPER, kJump, false));
   ow(4);
   CHECK(field::object(OBJ_PLAZA_BUMPER)->moving && field::object(OBJ_PLAZA_BUMPER)->lift > 0);
   CHECK(field::move_object(OBJ_PLAZA_BUMPER, field::DIR_LEFT, 1));
   for (int f = 0; f < 80 && field::object_moving(OBJ_PLAZA_BUMPER); ++f)
   {
      ow(1);
   }
   CHECK(!field::object_moving(OBJ_PLAZA_BUMPER) && field::object(OBJ_PLAZA_BUMPER)->x == 5 &&
         field::object(OBJ_PLAZA_BUMPER)->lift == 0);
   // An object that goes away no longer keeps wait_moves waiting.
   CHECK(field::route_object(OBJ_PLAZA_BUMPER, kWaitLong, true) && field::routes_busy());
   field::show_object(OBJ_PLAZA_BUMPER, false);
   CHECK(!field::routes_busy());
   // Map variables clear on a warp that plays a preview too (the map is hidden meanwhile).
   script::set_var(ids::VAR_GATE, 5);
   // A warp: transition, the garden's preview on the first visit, its banner after.
   overworld::warp(MAP_GARDEN, 3, 3, field::DIR_DOWN, script::kTransitionDefault);
   CHECK(overworld::transitioning());
   bool previewed = false;
   int frames = 0;
   for (; frames < 600 && overworld::transitioning(); ++frames)
   {
      ow(1);
      previewed = previewed || overworld::previewing();
   }
   CHECK(previewed && map::current() == &map::kWorld.maps[MAP_GARDEN] && overworld::banner_shown());
   // Its name is field text ("{hero} Garden", D-22 decision 6): the banner shows the name slot's text and is
   // sized by cells ("Mo Garden": 9 cells and the padding), not by codes.
   const auto banner_row = [](int row, char* out, int cells)
   {
      for (int x = 0; x < cells; ++x)
      {
         const int tile = (video::read_map_cell(0, x, row) & 0x3FF) - (gfx::background::kTextTiles + 16);
         out[x] = tile >= 0 && tile < 0x60 ? static_cast<char>(tile + 0x20) : '.';
      }
      out[cells] = '\0';
   };
   char shown[31];
   banner_row(1, shown, 30);
   CHECK(std::strstr(shown, "Mo Garden") != nullptr);
   CHECK((video::read_map_cell(0, 9 + 4 - 1, 0) & 0x3FF) == gfx::background::kTextTiles + 2);   // top-right corner
   CHECK(script::var(ids::VAR_GATE) == 0);
   CHECK(field::walker().x == 3 && field::walker().y == 3);
   // Back by its box warp, then to the garden again: no preview the second time.
   field::place(1, 2, field::DIR_UP);
   ow_go(field::DIR_UP);
   CHECK(overworld::transitioning());
   ow_settle();
   CHECK(map::current() == &map::kWorld.maps[MAP_PLAZA]);
   overworld::warp(MAP_GARDEN, 3, 3, script::kKeepFacing, static_cast<uint8_t>(script::Transition::Mosaic));
   previewed = false;
   for (int f = 0; f < 600 && overworld::transitioning(); ++f)
   {
      ow(1);
      previewed = previewed || overworld::previewing();
   }
   CHECK(!previewed && map::current() == &map::kWorld.maps[MAP_GARDEN]);
   // reset() mid-transition and mid-pan: nothing is left behind.
   script::world::scroll(field::DIR_RIGHT, 2, 4);
   ow(4);
   overworld::warp(MAP_GARDEN, 3, 3, script::kKeepFacing, static_cast<uint8_t>(script::Transition::WipeH));
   ow(5);
   overworld::reset();
   CHECK(!overworld::transitioning() && !field::locked());
   const int32_t cx = map::camera_x();
   field::set_camera_offset(0, 0);
   CHECK(map::camera_x() == cx);
   overworld::reset();
}

/// Runs a script of the test world to its end (they never wait).
/// @param id  the script
void run_script(script::ScriptId id)
{
   script::Context c{};
   script::start(c, id, script::kNoSelf);
   for (int i = 0; i < 10 && script::update(c); ++i)
   {
      script::frame();
   }
}

/// Field rules from the world manifest (A1, A2): custom elevations (level 12
/// meets only itself, overpass 9 meets ramp 5 and keeps the mover's level),
/// vehicle classes (swim walks 2 pixels a frame and cannot run, set from a
/// script), terrain per class and a map that forbids running (map small:
/// its top rows paint the levels, see tests/data/maps/maps/small.json).
void field_rules()
{
   using namespace world_ids;
   namespace ids = script_ids;
   // The compiled tables.
   CHECK(map::elevation(12).meets == 0x1000 && map::levels_meet(12, 12));
   CHECK(!map::levels_meet(0, 12) && !map::levels_meet(15, 12) && map::levels_meet(0, 4) && map::levels_meet(15, 3));
   CHECK(map::levels_meet(9, 5) && map::levels_meet(5, 9) && !map::levels_meet(9, 6));
   CHECK((map::elevation(9).flags & map::kElevationKeep) != 0 && (map::elevation(15).flags & map::kElevationKeep) != 0);
   CHECK(map::elevation(4).priority == 1 && map::elevation(5).priority == 2);
   CHECK(map::may_enter(0, 3) && !map::may_enter(15, 3) && map::may_enter(15, 0) && !map::may_enter(0, 12));
   CHECK(map::arrival_level(5, 9) == 5 && map::arrival_level(5, 6) == 6 && map::placed_level(9) == 0);
   CHECK(map::kWorld.vehicle_class_count == 4);   // foot, swim, (F2) paddle and (I3) trek
   const map::VehicleClass& swim = map::vehicle_class(1);
   CHECK(swim.walk_speed == 2 && swim.run_speed == 2 && swim.first_animation == 12 &&
         (swim.flags & map::kVehicleCanRun) == 0);
   CHECK(map::vehicle_class(0).walk_speed == 1 && map::vehicle_class(0).run_speed == 2 &&
         (map::vehicle_class(0).flags & map::kVehicleCanRun) != 0);
   CHECK((map::kWorld.maps[MAP_SMALL].flags & map::kMapNoRun) != 0 && (field_map().flags & map::kMapNoRun) == 0);

   fresh();
   script::clear_all();
   field::enter(MAP_SMALL, -7, -8, field::DIR_RIGHT);
   CHECK(field::walker().vehicle_class == 0 && field::walker().elevation == 0);
   // Level 12 meets only itself: level 0 cannot step on, and a walker on it cannot step off.
   hold(4, field::DIR_RIGHT);
   CHECK(field::walker().x == -7 && field::walker().last_blocked == Blocked::Elevation);
   field::place(-6, -8, field::DIR_RIGHT);
   CHECK(field::walker().elevation == 12);
   step(field::DIR_RIGHT);
   CHECK(field::walker().x == -5 && field::walker().elevation == 12);
   hold(4, field::DIR_RIGHT);
   CHECK(field::walker().x == -5 && field::walker().last_blocked == Blocked::Elevation);
   // The overpass (9) meets the ramp (5) and keeps the level; level 6 stays out of reach.
   field::place(-8, -7, field::DIR_RIGHT);
   CHECK(field::walker().elevation == 5);
   step(field::DIR_RIGHT);
   CHECK(field::walker().x == -7 && field::walker().elevation == 5);
   step(field::DIR_RIGHT);
   CHECK(field::walker().x == -6 && field::walker().elevation == 5);
   hold(4, field::DIR_RIGHT);
   CHECK(field::walker().x == -6 && field::walker().last_blocked == Blocked::Elevation);
   field::place(-7, -7, field::DIR_DOWN);
   CHECK(field::walker().elevation == 0);   // placed on a keep level: none to keep
   // This map forbids running: a run walks.
   field::place(-8, -6, field::DIR_RIGHT);
   CHECK(!field::can_run());
   CHECK(step(field::DIR_RIGHT, true) == 16 && field::walker().x == -7 && !field::walker().running);

   // Swim from a script: 2 pixels a frame, never runs, enters water; place() keeps the class.
   run_script(ids::SCRIPT_RIDE_SWIM);
   CHECK(field::walker().vehicle_class == 1);
   field::place(-8, -6, field::DIR_RIGHT);
   CHECK(field::walker().vehicle_class == 1);
   CHECK(step(field::DIR_RIGHT) == 8 && field::walker().x == -7);
   CHECK(step(field::DIR_RIGHT, true) == 8 && field::walker().x == -6 && !field::walker().running);
   field::place(-7, -5, field::DIR_RIGHT);
   step(field::DIR_RIGHT);
   CHECK(field::walker().x == -6 && field::walker().last_blocked == Blocked::None);
   // enter() keeps the class too; on a map that allows running, swim still cannot run.
   field::enter(MAP_FIELD, 9, 0, field::DIR_RIGHT);
   CHECK(field::walker().vehicle_class == 1 && !field::can_run());
   step(field::DIR_RIGHT);
   CHECK(field::walker().x == 10);   // the water at (10,0)
   // On foot again (by class number): runs here, and water blocks.
   run_script(ids::SCRIPT_WALK_ON);
   CHECK(field::walker().vehicle_class == 0 && field::can_run());
   field::place(0, 3, field::DIR_RIGHT);
   CHECK(step(field::DIR_RIGHT, true) == 8 && field::walker().x == 1);
   field::place(9, 0, field::DIR_RIGHT);
   hold(4, field::DIR_RIGHT);
   CHECK(field::walker().x == 9 && field::walker().last_blocked == Blocked::Terrain);
   field::set_vehicle(0);
}


/// @name F2 (D-27 section 3) stand-ins
///@{
uint16_t g_action_script = field::kNoScript;   ///< What test_action_rules() answers.
field::ActionFace g_face{};                    ///< What it was last asked about.
int g_action_asks = 0;                         ///< How often it was asked.
constexpr int kBlendModeShift = 6;             ///< BLDCNT: the blend mode's bits.
constexpr uint16_t kBlendModeMask = 3;         ///< Two bits.
constexpr int kTransitionFrames = 80;          ///< Frames enough for any warp transition and banner.
///@}

/// A field action dispatcher for the field's side alone (the battle side has tests/field_actions_tests.cpp).
/// @param face  what the walker faces
/// @return g_action_script
uint16_t test_action_rules(const field::ActionFace& face)
{
   g_face = face;
   ++g_action_asks;
   return g_action_script;
}

/// Presses A once.
void press_a()
{
   field::Intent talk;
   talk.interact = true;
   field::update(talk);
}

/// Runs the overworld until its script and transitions end.
void run_overworld()
{
   for (int i = 0; i < kTransitionFrames && (overworld::script_running() || overworld::transitioning()); ++i)
   {
      overworld::update({});
   }
}

/// The overworld's blend mode as last written.
/// @return video::BlendMode as a number
uint16_t blend_mode()
{
   return static_cast<uint16_t>((hal::soft::pending_bldcnt() >> kBlendModeShift) & kBlendModeMask);
}

/// F2 (D-27 section 3), the field's and the overworld's side: the compiled world (object kind, darkness, tags,
/// escape point, a class that dismounts), A on water and on an object of a kind asks the attached dispatcher (not
/// when the walker can enter the cell, not without one), the paddle class stepping off onto land, a dark map's
/// window and blend, `darkness`, the escape point recorded by a warp out of an escape_point map and `escape`, and
/// `flight record` / `flight visit`.
void field_actions()
{
   using namespace world_ids;
   namespace ids = script_ids;
   CHECK(map::kWorld.maps[MAP_SMALL].darkness == 2 && map::kWorld.maps[MAP_SMALL].tags == 1);
   CHECK((field_map().flags & map::kMapEscapePoint) != 0 && (map::kWorld.maps[MAP_TOWN].flags & map::kMapEscapePoint) == 0);
   CHECK(map::kWorld.maps[MAP_TOWN].objects[OBJ_TOWN_PACER].kind == 1 &&
         map::kWorld.maps[MAP_TOWN].objects[OBJ_TOWN_GUARD].kind == map::kNoObjectKind);
   CHECK((map::vehicle_class(2).flags & map::kVehicleDismount) != 0 && (map::vehicle_class(1).flags & map::kVehicleDismount) == 0);

   fresh();
   script::clear_all();
   field::enter(MAP_FIELD, 9, 0, field::DIR_RIGHT);
   field::set_vehicle(0);
   field::FieldEvent e{};
   // No dispatcher: A on the water does nothing.
   press_a();
   CHECK(!field::take_event(&e));
   // With one: it is asked about the water's behavior and its script is requested.
   field::set_action_rules(test_action_rules);
   g_action_script = static_cast<uint16_t>(ids::SCRIPT_GO_DARK);
   press_a();
   CHECK(field::take_event(&e) && e.source == field::Source::Action && e.what == field::Request::Script &&
         e.script == g_action_script && e.at_x == 10 && e.at_y == 0 && e.object == field::kNoObject);
   CHECK(g_face.target == script::ActionTarget::Behavior && g_face.value == 1 && g_face.facing == field::DIR_RIGHT);
   field::release();
   g_action_script = field::kNoScript;
   press_a();
   CHECK(!field::take_event(&e));
   // Swimming, the water can be entered: nobody is asked.
   field::set_vehicle(1);
   const int asked = g_action_asks;
   press_a();
   CHECK(!field::take_event(&e) && g_action_asks == asked);
   // The paddle class steps off the water as on foot.
   field::set_vehicle(2);
   field::place(10, 0, field::DIR_LEFT);
   step(field::DIR_LEFT);
   CHECK(field::walker().x == 9 && field::walker().vehicle_class == 0);
   field::place(9, 0, field::DIR_RIGHT);
   hold(4, field::DIR_RIGHT);
   CHECK(field::walker().x == 9 && field::walker().last_blocked == Blocked::Terrain);   // on foot again
   // An object of a kind without a script of its own: the dispatcher is asked with its kind; it becomes self.
   field::enter(MAP_TOWN, 3, 6, field::DIR_DOWN);
   field::lock();   // the pacer stands still
   g_action_script = static_cast<uint16_t>(ids::SCRIPT_GO_LIT);
   press_a();
   CHECK(field::take_event(&e) && e.source == field::Source::Action && e.object == OBJ_TOWN_PACER &&
         e.at_x == 3 && e.at_y == 7);
   CHECK(g_face.target == script::ActionTarget::Object && g_face.value == 1 && g_face.object == OBJ_TOWN_PACER);
   field::release();
   field::set_action_rules(nullptr);
   g_action_script = field::kNoScript;

   // A dark map: window 0 and the Darken blend each frame; `darkness 0;` lights it until the next map.
   overworld::start(MAP_SMALL, -8, -6, field::DIR_RIGHT);
   CHECK(overworld::darkness() == 2);
   overworld::update({});
   CHECK(blend_mode() == static_cast<uint16_t>(hal::video::BlendMode::Darken));
   CHECK(overworld::run_script(static_cast<uint16_t>(ids::SCRIPT_GO_LIT), field::kNoObject));
   CHECK(!overworld::run_script(static_cast<uint16_t>(ids::SCRIPT_GO_LIT), field::kNoObject));   // one at a time
   run_overworld();
   overworld::update({});
   CHECK(overworld::darkness() == 0 && blend_mode() == static_cast<uint16_t>(hal::video::BlendMode::Off));
   CHECK(overworld::run_script(static_cast<uint16_t>(ids::SCRIPT_GO_DARK), field::kNoObject));
   run_overworld();
   CHECK(overworld::darkness() == 3);
   // C8: the levels are the world's (tests/data/maps world.json darkness_levels): level 3 is a 16-pixel half box,
   // brightness 16; level 2 (map small's own) 32 and 12
   overworld::update({});
   CHECK(hal::soft::pending_evy() == 16 && hal::soft::pending_window_width(0) == 32);
   // C8: a script-set level is kept (the map-lifetime map_darkness): a game started again on the map (a load) keeps
   // it; another map clears it, and coming back shows the map's own level
   CHECK(script::var(script::VarId{script::kBank.darkness_var}) == 3 + 1);
   overworld::start(MAP_SMALL, -8, -6, field::DIR_RIGHT);
   CHECK(overworld::darkness() == 3);
   overworld::warp(MAP_TOWN, 2, 0, field::DIR_DOWN, 0);
   run_overworld();
   CHECK(overworld::darkness() == 0 && script::var(script::VarId{script::kBank.darkness_var}) == 0);
   overworld::warp(MAP_SMALL, -8, -6, field::DIR_RIGHT, 0);
   run_overworld();
   overworld::update({});
   CHECK(overworld::darkness() == 2 && hal::soft::pending_evy() == 12 && hal::soft::pending_window_width(0) == 64);
   CHECK(overworld::run_script(static_cast<uint16_t>(ids::SCRIPT_GO_LIT), field::kNoObject));
   run_overworld();
   overworld::start(MAP_SMALL, -8, -6, field::DIR_RIGHT);
   CHECK(overworld::darkness() == 0);   // lit by a script, still lit after a new start on it

   // The escape point: a warp from field (escape_point) to town records the cell behind the walker.
   overworld::start(MAP_FIELD, 2, 2, field::DIR_UP);
   CHECK(overworld::darkness() == 0);
   AssetKey m{};
   int16_t x = 0;
   int16_t y = 0;
   uint8_t facing = 0;
   CHECK(!script::actions::escape_point(m, x, y, facing));
   overworld::warp(MAP_TOWN, 2, 0, field::DIR_DOWN, 0);
   run_overworld();
   CHECK(map::index_of(*map::current()) == MAP_TOWN);
   // C8: the escape point keeps the map's asset key (not its index)
   CHECK(script::actions::escape_point(m, x, y, facing) && m == map::key_of(MAP_FIELD) && x == 2 &&
         y == 3 && facing == field::DIR_DOWN);
   CHECK(overworld::run_script(static_cast<uint16_t>(ids::SCRIPT_RUN_AWAY), field::kNoObject));
   run_overworld();
   CHECK(map::index_of(*map::current()) == MAP_FIELD && field::walker().x == 2 && field::walker().y == 3);
   CHECK(script::var(ids::VAR_ESCAPED) == 1);
   // C8: a recorded map the world does not have (a save from a build with another map list) is no escape point
   overworld::warp(MAP_TOWN, 2, 0, field::DIR_DOWN, 0);
   run_overworld();
   script::actions::record_escape(AssetKey{0x12345678u}, 2, 3, field::DIR_DOWN);
   CHECK(map::find(AssetKey{0x12345678u}) == nullptr);
   CHECK(overworld::run_script(static_cast<uint16_t>(ids::SCRIPT_RUN_AWAY), field::kNoObject));
   run_overworld();
   CHECK(map::index_of(*map::current()) == MAP_TOWN && script::var(ids::VAR_ESCAPED) == 0);
   overworld::warp(MAP_FIELD, 2, 3, field::DIR_DOWN, 0);
   run_overworld();
   // A flight point: recorded, then visited.
   overworld::warp(MAP_TOWN, 2, 0, field::DIR_DOWN, 0);
   run_overworld();
   CHECK(!script::actions::recorded(ids::FLIGHT_POINT_HOME));
   CHECK(overworld::run_script(static_cast<uint16_t>(ids::SCRIPT_FLY_HOME), field::kNoObject));
   run_overworld();
   CHECK(script::actions::recorded(ids::FLIGHT_POINT_HOME));
   CHECK(map::index_of(*map::current()) == MAP_FIELD && field::walker().x == 1 && field::walker().y == 1);
   overworld::reset();
}

/// I3 (map crowd): spawning by the view, the reference movement types, a counter, door / arrow / none / dynamic
/// warps, a vehicle class that turns in 8 frames, bumps and keeps moving, and step animations (sprite:STEPPER).
void reference_motion()
{
   using namespace world_ids;
   namespace ids = script_ids;
   script::clear_all();
   fresh();
   const map::Map& crowd = map::kWorld.maps[MAP_CROWD];
   CHECK(crowd.object_count == 31 && map::spawns_by_view(crowd) && crowd.object_view.right == 10);
   CHECK(crowd.objects[OBJ_CROWD_TURNER].movement == map::Movement::Turn &&
         crowd.objects[OBJ_CROWD_TURNER].movement_flags == (map::kMoveTwoChoices | map::kMoveDelaysShort << 4));
   CHECK(crowd.objects[OBJ_CROWD_STACK_A].x == crowd.objects[OBJ_CROWD_STACK_B].x);   // stacked_objects

   // The view: the far ones (x 20-28) are not spawned near the start; jumping there spawns them and drops the
   // near ones; walking spawns a template as its cell comes within 10 cells to the right.
   field::enter(MAP_CROWD, 2, 8, field::DIR_DOWN);
   CHECK(field::object(OBJ_CROWD_CLERK)->present && !field::object(OBJ_CROWD_FAR0)->present);
   field::enter(MAP_CROWD, 19, 5, field::DIR_DOWN);
   CHECK(field::object(OBJ_CROWD_FAR0)->present && !field::object(OBJ_CROWD_CLERK)->present);
   int shown = 0;
   for (uint8_t i = 0; i < field::object_count(); ++i)
   {
      shown += field::object(i)->present ? 1 : 0;
   }
   CHECK(shown > 0 && shown <= map::kMaxObjects);   // never more than the sprite slots
   field::enter(MAP_CROWD, 8, 1, field::DIR_RIGHT);
   CHECK(!field::object(OBJ_CROWD_FAR0)->present);
   hold(1, field::DIR_RIGHT);   // a step to 9: far0 is 11 cells away
   CHECK(!field::object(OBJ_CROWD_FAR0)->present);
   hold(16, field::DIR_RIGHT);   // the next step (to 10) spawns it on its first frame
   CHECK(field::walker().x == 10 && field::object(OBJ_CROWD_FAR0)->present);
   settle();

   // Rotate (clockwise, facing down): faces at once, turns 48 frames later on the frame after its wait ends.
   field::enter(MAP_CROWD, 12, 6, field::DIR_UP);
   idle(49);
   CHECK(field::object(OBJ_CROWD_ROTOR)->facing == field::DIR_DOWN);
   idle(1);
   CHECK(field::object(OBJ_CROWD_ROTOR)->facing == field::DIR_LEFT);
   // Patrol (facing right, range 2): one cell each 16 frames, the first pixel in its first frame; at the range's
   // end it turns back.
   field::enter(MAP_CROWD, 14, 8, field::DIR_UP);
   idle(1);
   const field::Object& pat = *field::object(OBJ_CROWD_PATROLLER);
   CHECK(pat.moving && pat.x == 15 && pat.px == 14 * 16 + 1);
   idle(15);
   CHECK(!pat.moving && pat.px == 15 * 16);
   idle(16);
   CHECK(pat.x == 16);
   idle(16);
   CHECK(pat.x == 15 && pat.facing == field::DIR_LEFT);   // back from the range's end
   // A fence stops an object as it stops the walker: the fenced patroller walks in place.
   field::enter(MAP_CROWD, 2, 8, field::DIR_UP);
   idle(40);
   CHECK(field::object(OBJ_CROWD_FENCED)->x == 2 && field::object(OBJ_CROWD_FENCED)->facing == field::DIR_RIGHT);
   // Circuit (right, down, left, up within 1): around its home square.
   field::enter(MAP_CROWD, 16, 8, field::DIR_UP);
   idle(16);
   CHECK(field::object(OBJ_CROWD_CIRCLER)->x == 17 && field::object(OBJ_CROWD_CIRCLER)->y == 3);
   idle(16);
   CHECK(field::object(OBJ_CROWD_CIRCLER)->y == 4);
   // Invisible: present and solid, never drawn; InPlace: never leaves its cell.
   field::enter(MAP_CROWD, 9, 10, field::DIR_DOWN);
   idle(20);
   CHECK(field::object(OBJ_CROWD_GHOST)->invisible && field::object(OBJ_CROWD_GHOST)->present);
   CHECK(field::object(OBJ_CROWD_JOGGER)->x == 8 && field::object(OBJ_CROWD_JOGGER)->facing == field::DIR_LEFT);
   hold(1, field::DIR_DOWN);
   settle();
   CHECK(field::walker().y == 11);   // the ghost (9,12) blocks the next step
   hold(1, field::DIR_DOWN);
   settle();
   CHECK(field::walker().y == 11);
   // Turn: only its two choices; Roam: stays in its range box.
   field::enter(MAP_CROWD, 7, 8, field::DIR_UP);
   bool turned_up = false;
   for (int f = 0; f < 600; ++f)
   {
      idle(1);
      const uint8_t d = field::object(OBJ_CROWD_TURNER)->facing;
      CHECK(d == field::DIR_DOWN || d == field::DIR_UP);
      turned_up = turned_up || d == field::DIR_UP;
      const field::Object& r = *field::object(OBJ_CROWD_ROAMER);
      CHECK(r.x >= 8 && r.x <= 12 && r.y >= 1 && r.y <= 5);
   }
   CHECK(turned_up);
   // Mirror (reference facing up; the walker faced down at spawn): it steps the opposite way.
   field::enter(MAP_CROWD, 4, 9, field::DIR_DOWN);
   idle(2);
   hold(1, field::DIR_RIGHT);   // a turn first: the mirror turns too
   settle();
   CHECK(field::object(OBJ_CROWD_COPIER)->facing == field::DIR_LEFT);
   hold(1, field::DIR_RIGHT);
   CHECK(field::object(OBJ_CROWD_COPIER)->moving && field::object(OBJ_CROWD_COPIER)->x == 3);
   settle();

   // A counter: A facing it talks to the object beyond.
   field::enter(MAP_CROWD, 4, 8, field::DIR_RIGHT);
   field::Intent a;
   a.interact = true;
   field::update(a);
   field::FieldEvent e{};
   CHECK(field::take_event(&e) && e.source == field::Source::Object && e.object == OBJ_CROWD_CLERK);
   field::release();
   // keep_facing: a talked-to Static object keeps facing the walker after release().
   field::enter(MAP_CROWD, 6, 10, field::DIR_RIGHT);
   field::update(a);
   CHECK(field::take_event(&e) && e.object == OBJ_CROWD_STAYER);
   field::release();
   CHECK(field::object(OBJ_CROWD_STAYER)->facing == field::DIR_LEFT);

   // Warps: a door (up from the cell below it), an arrow (facing first), a none warp, a dynamic warp.
   field::enter(MAP_CROWD, 3, 7, field::DIR_UP);
   hold(1, field::DIR_UP);
   CHECK(field::take_event(&e) && e.what == field::Request::Warp && e.at_x == 3 && e.at_y == 6 &&
         e.map == MAP_SMALL && field::walker().y == 7);
   field::enter(MAP_CROWD, 1, 15, field::DIR_UP);
   hold(1, field::DIR_DOWN);
   CHECK(!field::has_event() && field::walker().motion == Motion::Turning);   // facing up: it turns first
   settle();
   hold(1, field::DIR_DOWN);
   CHECK(field::take_event(&e) && e.what == field::Request::Warp && e.at_y == 15);
   field::enter(MAP_CROWD, 2, 14, field::DIR_DOWN);
   step(field::DIR_DOWN);
   CHECK(!field::has_event() && field::walker().y == 15);   // a none warp never fires
   field::enter(MAP_CROWD, 3, 14, field::DIR_DOWN);
   step(field::DIR_DOWN);
   CHECK(field::take_event(&e) && e.map == map::kDynamicWarp && e.x == 5);

   // Vehicle class trek: turns in 8 frames, bumps into the counter (a step's walk in place), keeps moving into a
   // new direction on arrival.
   field::enter(MAP_CROWD, 4, 6, field::DIR_UP);
   field::set_vehicle(3);
   hold(1, field::DIR_RIGHT);
   hold(7, field::DIR_RIGHT);
   CHECK(field::walker().motion == Motion::Turning && field::walker().x == 4);
   hold(1, field::DIR_RIGHT);
   CHECK(field::walker().motion == Motion::Walking && field::walker().x == 5 && field::walker().px == 4 * 16 + 1);
   hold(14, field::DIR_RIGHT);
   hold(1, field::DIR_DOWN);   // arrives at 5,6 with down held: steps down without a turn
   CHECK(field::walker().motion == Motion::Walking && field::walker().move_dir == field::DIR_DOWN);
   hold(15, field::DIR_DOWN);
   hold(1, field::DIR_DOWN);   // 5,7 -> 5,8 is the counter: a bump
   CHECK(field::walker().motion == Motion::Turning && field::walker().y == 7 &&
         field::walker().last_blocked == Blocked::Collision);
   settle();
   field::set_vehicle(0);

   // Step animations: standing, the walk animation paused on a standing entry; steps alternate the feet.
   gfx::sprites::reset();
   const asset::Sprite* stepper = asset::sprite(asset::key_v1("sprite:STEPPER"));
   CHECK(stepper != nullptr && stepper->steps != nullptr && (stepper->flags & asset::kSpriteStreamed) != 0);
   gfx::sprites::load(field::kWalkerSlot, *stepper);
   gfx::sprites::show(field::kWalkerSlot, true);
   field::attach_sprite(field::kWalkerSlot, stepper);
   field::enter(MAP_CROWD, 2, 9, field::DIR_DOWN);
   idle(1);
   CHECK(gfx::sprites::animation(field::kWalkerSlot) == 4 && gfx::sprites::entry(field::kWalkerSlot) == 1);
   hold(1, field::DIR_DOWN);
   CHECK(gfx::sprites::entry(field::kWalkerSlot) == 2);   // after the first standing entry: the second step
   settle();
   idle(1);
   CHECK(gfx::sprites::entry(field::kWalkerSlot) == 3);   // stopped past the first stand: the second one
   hold(1, field::DIR_DOWN);
   CHECK(gfx::sprites::entry(field::kWalkerSlot) == 0);   // and the next step starts on the other foot
   settle();
   field::attach_sprite(0xFF, nullptr);
   map::leave();
}

/// Runs every check.
/// @return 0 when all pass

constexpr int kInPlaceCheckFrames = 20;   ///< I4: frames past one step's time at walking speed (16).

/// I4 (D-29): template changes on the shown map (a cell, a movement and its facing) hold for its spawns until the next
/// map; an object's sprite hides where it stands (it stays active); a route walks in place for one step's time and
/// turns back to its page's facing.
void i4_field()
{
   using namespace world_ids;
   script::clear_all();
   fresh();
   field::enter(MAP_TOWN, 2, 0, field::DIR_DOWN);
   field::set_template_cell(OBJ_TOWN_GUARD, 4, 4);
   field::set_template_movement(OBJ_TOWN_GUARD, map::Movement::Static, 0, 0, field::DIR_LEFT);
   field::spawn_objects();
   const field::Object* guard = field::object(OBJ_TOWN_GUARD);
   CHECK(guard->x == 4 && guard->y == 4 && guard->facing == field::DIR_LEFT &&
         guard->movement == map::Movement::Static);
   field::set_object_visible(OBJ_TOWN_GUARD, false);
   CHECK(!guard->visible && guard->active);
   field::enter(MAP_TOWN, 2, 0, field::DIR_DOWN);   // the next map forgets the changes
   CHECK(guard->x == 2 && guard->y == 2 && guard->facing == field::DIR_DOWN && guard->visible);
   static constexpr uint8_t kRoute[] = {0, static_cast<uint8_t>(route::RouteOp::InPlace), field::DIR_LEFT,
                                        route::kDefaultSpeed, static_cast<uint8_t>(route::RouteOp::FaceOriginal),
                                        static_cast<uint8_t>(route::RouteOp::End)};
   CHECK(field::route_object(OBJ_TOWN_GUARD, kRoute, true));
   idle(1);
   CHECK(guard->facing == field::DIR_LEFT && guard->x == 2 && field::routes_busy());
   idle(kInPlaceCheckFrames);
   CHECK(guard->facing == field::DIR_DOWN && !field::routes_busy());
}

/// C12 (map crowd): a script's move of a template (set_template_cell(), the reference games' edited template copy)
/// decides spawning by the view as it decides the spawn cell: moved into the view it spawns there, moved out it
/// stays off; its spawn cell (home) centres its range and keeps it spawned until both it and its cell leave the view.
void c12_view_override()
{
   using namespace world_ids;
   constexpr int kStartX = 2;         // the walker's cell (a free row): far0's compiled cell (20,1) is out of the view
   constexpr int kStartY = 7;
   constexpr int kNearX = 4;          // far0 moved here: inside the view, off the walker's row
   constexpr int kNearY = 9;
   constexpr int kAwayX = 26;         // the clerk moved here: outside it
   constexpr int kAwayY = 1;
   constexpr int kRoamX = 12;         // the roamer moved here (range 2 x 2), off the walker's row
   constexpr int kRoamY = 12;
   constexpr int kRoamRange = 2;
   constexpr int kRoamFrames = 600;
   constexpr int kWalkTo = 14;        // far0's cell 10 to the left (view 9), its compiled cell still in the view
   constexpr int kWalkFrames = 600;   // far more than the walk takes
   script::clear_all();
   fresh();
   field::enter(MAP_CROWD, kStartX, kStartY, field::DIR_RIGHT);
   CHECK(!field::object(OBJ_CROWD_FAR0)->present && field::object(OBJ_CROWD_CLERK)->present);

   field::set_template_cell(OBJ_CROWD_FAR0, kNearX, kNearY);
   field::set_template_cell(OBJ_CROWD_CLERK, kAwayX, kAwayY);
   field::set_template_cell(OBJ_CROWD_ROAMER, kRoamX, kRoamY);
   field::spawn_objects();
   const field::Object& far0 = *field::object(OBJ_CROWD_FAR0);
   CHECK(far0.present && far0.x == kNearX && far0.y == kNearY && far0.home_x == kNearX && far0.home_y == kNearY);
   CHECK(!field::object(OBJ_CROWD_CLERK)->present);

   // The roamer's range box is around its spawn cell, not the compiled one.
   const field::Object& roamer = *field::object(OBJ_CROWD_ROAMER);
   CHECK(roamer.present && roamer.x == kRoamX && roamer.y == kRoamY);
   bool roamed = false;
   for (int f = 0; f < kRoamFrames; ++f)
   {
      idle(1);
      CHECK(roamer.x >= kRoamX - kRoamRange && roamer.x <= kRoamX + kRoamRange && roamer.y >= kRoamY - kRoamRange &&
            roamer.y <= kRoamY + kRoamRange);
      roamed = roamed || roamer.x != kRoamX || roamer.y != kRoamY;
   }
   CHECK(roamed);

   // Walking away: far0 goes when its cell and spawn cell leave the view (the compiled cell does not keep it).
   for (int f = 0; f < kWalkFrames && field::walker().x < kWalkTo; ++f)
   {
      hold(1, field::DIR_RIGHT);
   }
   settle();
   CHECK(field::walker().x == kWalkTo && !far0.present);
   map::leave();
}

int main()
{
   cells();
   layers();
   text_over_map();
   rendering();
   streaming();
   animation();
   walking();
   behaviors();
   encounters();
   events();
   objects();
   eventing();
   field_rules();
   field_actions();
   fixed_tiles();
   walls();
   shift();
   reference_motion();
   i4_field();
   c12_view_override();
   std::printf("map tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
