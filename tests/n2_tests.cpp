/// @file
/// N2 (D-32) tests on the fixture tests/data/n2: the shown map's cell edits (data only until drawn, replaced, dropped
/// at the next map, the table's limit), set_layout() (another map's terrain, the map's own objects), a ledge cell
/// with collision jumped in its direction only, the camera mover (spawn, a route moving the camera, removal keeping
/// the view), fixed draw orders, sprite offsets, effect sprite slots, a route's walk in place, and window 0's edges
/// per scanline in the software compositor.
#include "capsule/asset/asset.h"
#include "capsule/field/field.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/map/map.h"
#include "capsule/script/port.h"
#include "capsule/script/route.h"
#include "capsule/script/script.h"

#include "../src/hal/soft/soft_video.h"

#include "n2_world_ids.h"

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

/// The fixture has no scripts: the port does nothing.
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
/// No party.
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
/// No party.
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

/// @name The fixture (tests/data/n2)
///@{
constexpr uint16_t kRoom = 0;   ///< Map room.
constexpr uint16_t kRoomAlt = 1;   ///< Map room_alt (room's other layout: a mark at (1, 1)).
constexpr map::MetatileId kGround = 0;   ///< yard/ground.
constexpr map::MetatileId kWall = 1;   ///< yard/wall.
constexpr map::MetatileId kMark = 3;   ///< yard/mark.
constexpr map::MapCell kWallCell = 1 | 1 << map::kCellCollisionShift;   ///< behaviour wall, collision 1.
constexpr int kLedgeX = 2;   ///< The ledge (down) with a collision bit.
constexpr int kLedgeY = 4;   ///< Its row.
constexpr int kStart = 3;   ///< A free cell, column and row.
///@}

/// One frame of the field (no input) and the sprites, then the video latched and composed.
/// @param frames  how many
void run(int frames)
{
   for (int i = 0; i < frames; ++i)
   {
      field::update(field::Intent{});
      gfx::sprites::update();
      hal::vram::flush_all();
      hal::soft::latch();
   }
   hal::soft::compose(g_frame);
}

/// Shows the room with the walker on a free cell.
void enter_room()
{
   field::enter(kRoom, kStart, kStart, field::DIR_DOWN);
   run(1);
}

/// Cell edits: data only until drawn, replaced, dropped at the next enter(), the table's limit.
void cell_edits()
{
   enter_room();
   const map::Map& m = *map::current();
   const map::MetatileId layers[map::kMaxLayers] = {kWall, map::kNoMetatile, map::kNoMetatile, map::kNoMetatile};
   CHECK(map::set_cell(kStart + 1, kStart, kWallCell, layers));
   CHECK(map::cell_at(m, kStart + 1, kStart) == kWallCell);
   CHECK(map::metatile_at(m, 0, kStart + 1, kStart) == kWall);
   CHECK(map::cell_edit_count() == 1);
   bool jump = false;
   CHECK(field::check_step(kStart, kStart, field::DIR_RIGHT, 0, 0, &jump) == field::Blocked::Collision);
   const map::MetatileId other[map::kMaxLayers] = {kMark, kMark, map::kNoMetatile, map::kNoMetatile};
   CHECK(map::set_cell(kStart + 1, kStart, 0, other));   // the cell's edit replaced
   CHECK(map::cell_edit_count() == 1 && map::metatile_at(m, 1, kStart + 1, kStart) == kMark);
   map::draw_cell(kStart + 1, kStart);
   // the other map is untouched (edits are the shown map's)
   CHECK(map::metatile_at(map::kWorld.maps[kRoomAlt], 0, kStart + 1, kStart) == kGround);
   int ok = 0;
   for (int k = 0; k < map::kMaxCellEdits + 1; ++k)
   {
      ok += map::set_cell(k % 64, k / 64 + 100, 0, layers) ? 1 : 0;   // far away cells: the table fills up
   }
   CHECK(ok == map::kMaxCellEdits - 1);   // one slot was the first cell's
   field::enter(kRoom, kStart, kStart, field::DIR_DOWN);
   CHECK(map::cell_edit_count() == 0 && map::metatile_at(*map::current(), 0, kStart + 1, kStart) == kGround);
}

/// set_layout(): the other map's terrain shows, the map's own objects stay; the next enter() shows its own.
void layouts()
{
   enter_room();
   const map::Map& m = *map::current();
   CHECK(map::metatile_at(m, 0, 1, 1) == kGround);
   map::set_layout(&map::kWorld.maps[kRoomAlt]);
   CHECK(map::current() == &map::kWorld.maps[kRoom] && map::terrain() == &map::kWorld.maps[kRoomAlt]);
   CHECK(map::metatile_at(m, 0, 1, 1) == kMark);
   CHECK(field::object_count() == 1);
   map::set_layout(nullptr);
   CHECK(map::terrain() == &map::kWorld.maps[kRoom] && map::metatile_at(m, 0, 1, 1) == kGround);
   map::set_layout(&map::kWorld.maps[kRoomAlt]);
   field::enter(kRoom, kStart, kStart, field::DIR_DOWN);
   CHECK(map::terrain() == &map::kWorld.maps[kRoom]);
}

/// A ledge cell keeping a collision bit is jumped in its direction (the reference games test the ledge first) and
/// blocks the other ways.
void ledges()
{
   enter_room();
   bool jump = false;
   CHECK(map::cell_collision(map::cell_at(*map::current(), kLedgeX, kLedgeY)) != 0);
   CHECK(field::check_step(kLedgeX, kLedgeY - 1, field::DIR_DOWN, 0, 0, &jump) == field::Blocked::None && jump);
   jump = false;
   CHECK(field::check_step(kLedgeX - 1, kLedgeY, field::DIR_RIGHT, 0, 0, &jump) == field::Blocked::Ledge && !jump);
   CHECK(field::check_step(kLedgeX, kLedgeY + 1, field::DIR_UP, 0, 0, &jump) == field::Blocked::Ledge);
}

int g_rule_calls = 0;   ///< step_rules(): the rule's calls that acted (commit).
bool g_rule_refuses = true;   ///< step_rules(): the rule refuses.

/// The test's step rule: refuses steps into column kStart + 1 while g_rule_refuses.
/// @param dir     direction
/// @param x       target column
/// @param y       target row
/// @param commit  the step is taken now
/// @return true to refuse
bool test_rule(uint8_t dir, int x, int y, bool commit)
{
   (void)dir;
   (void)y;
   g_rule_calls += commit ? 1 : 0;
   return g_rule_refuses && x == kStart + 1;
}

/// The walker's step rule: asked for plain steps the cell lets through, refuses without a bump (the walker stands),
/// lets the step through when it allows it; walker_path_clear() asks it without acting.
void step_rules()
{
   enter_room();
   field::set_step_rule(test_rule);
   CHECK(!field::walker_path_clear(field::DIR_RIGHT, 1) && g_rule_calls == 0);
   field::Intent right{};
   right.direction = field::DIR_RIGHT;
   field::face_walker(field::DIR_RIGHT);
   field::update(right);
   CHECK(field::walker().last_blocked == field::Blocked::Rule && g_rule_calls == 1);
   CHECK(field::walker().motion == field::Motion::Idle);   // no bump: it stands
   g_rule_refuses = false;
   field::update(right);
   CHECK(field::walker().x == kStart + 1 && field::walker().motion == field::Motion::Walking);
   field::set_step_rule(nullptr);
   run(map::kMetatilePixels);
}

/// The camera mover: a route moves the camera (the walker stays), removal keeps the view, the next map resets it.
void camera()
{
   enter_room();
   const int32_t y0 = map::camera_y();
   CHECK(!field::object_moving(field::kCameraObject));
   static constexpr uint8_t kDown2[] = {0, static_cast<uint8_t>(route::RouteOp::Move), field::DIR_DOWN, 2,
                                        static_cast<uint8_t>(route::RouteOp::End)};
   CHECK(!field::route_object(field::kCameraObject, kDown2, true));   // no mover yet
   field::spawn_camera();
   CHECK(field::camera_spawned());
   CHECK(field::route_object(field::kCameraObject, kDown2, true));
   run(1);
   CHECK(field::object_moving(field::kCameraObject) && field::routes_busy());
   run(2 * map::kMetatilePixels + 1);
   CHECK(!field::object_moving(field::kCameraObject));
   CHECK(map::camera_y() == y0 + 2 * map::kMetatilePixels);
   CHECK(field::walker().y == kStart);
   field::remove_camera();
   run(1);
   CHECK(!field::camera_spawned() && map::camera_y() == y0 + 2 * map::kMetatilePixels);   // the view stays
   field::enter(kRoom, kStart, kStart, field::DIR_DOWN);
   run(1);
   CHECK(map::camera_y() == y0);
}

/// Fixed draw orders, sprite offsets and effect slots.
void sprites_and_slots()
{
   enter_room();
   const field::Object* o = field::object(0);
   CHECK(o != nullptr && !o->fixed_order);
   field::set_fixed_order(0, true, 7);
   field::set_sprite_offset(0, 3, -3);
   field::set_fixed_order(script::kObjectPlayer, true, 9);
   run(1);
   CHECK(o->fixed_order && o->order == 7 && o->sprite_dx == 3 && o->sprite_dy == -3);
   // the fixture's map has one object (fixed slot 1 + 0): effects get slots past it
   const uint8_t a = field::claim_sprite_slot();
   CHECK(a == field::kFirstObjectSlot + 1);
   int claimed = 1;
   while (field::claim_sprite_slot() != field::kNoSpriteSlot)
   {
      ++claimed;
   }
   CHECK(claimed == map::kMaxObjects - 1);
   field::release_sprite_slot(a);
   CHECK(field::claim_sprite_slot() == a);
   field::enter(kRoom, kStart, kStart, field::DIR_DOWN);   // the next map: everything back
   CHECK(field::claim_sprite_slot() == field::kFirstObjectSlot + 1);
   CHECK(!field::object(0)->fixed_order && field::object(0)->sprite_dx == 0);
}

/// A route's InPlace step lasts its speed's frames and the route goes on after it.
void in_place()
{
   enter_room();
   static constexpr uint8_t kSpin[] = {0, static_cast<uint8_t>(route::RouteOp::InPlace), field::DIR_LEFT,
                                       route::kDefaultSpeed, static_cast<uint8_t>(route::RouteOp::End)};
   CHECK(field::route_object(0, kSpin, true));
   run(1);
   CHECK(field::object_moving(0) && field::object(0)->facing == field::DIR_LEFT);
   run(map::kMetatilePixels);
   CHECK(!field::object_moving(0));
}

/// The walker's route steps take its Speed steps (a fast step: 2 pixels a frame); the next route walks again.
void walker_speed()
{
   enter_room();
   constexpr int kFastFrames = map::kMetatilePixels / route::kSpeedDoublePixels;
   static constexpr uint8_t kFast[] = {0, static_cast<uint8_t>(route::RouteOp::Speed), route::kSpeedDouble,
                                       static_cast<uint8_t>(route::RouteOp::Move), field::DIR_RIGHT, 1,
                                       static_cast<uint8_t>(route::RouteOp::End)};
   field::route_walker(kFast);
   int frames = 0;
   run(1);   // the route's Speed and Move start the step (its first pixels)
   while (field::walker_moving() && frames < map::kMetatilePixels * 2)
   {
      run(1);
      ++frames;
   }
   CHECK(field::walker().x == kStart + 1 && frames == kFastFrames);
   static constexpr uint8_t kWalk[] = {0, static_cast<uint8_t>(route::RouteOp::Move), field::DIR_LEFT, 1,
                                       static_cast<uint8_t>(route::RouteOp::End)};
   field::route_walker(kWalk);
   frames = 0;
   run(1);
   while (field::walker_moving() && frames < map::kMetatilePixels * 2)
   {
      run(1);
      ++frames;
   }
   CHECK(field::walker().x == kStart && frames == map::kMetatilePixels);
}

/// Window 0's edges per scanline: a line's own edges, the rectangle's rows.
void window_lines()
{
   hal::soft::reset();
   static uint16_t lines[video::kWindowLines] = {};
   constexpr int kRow = 10;
   constexpr int kLeft = 20;
   constexpr int kRight = 40;
   lines[kRow] = static_cast<uint16_t>(kLeft << video::kWindowLineShift | kRight);
   const video::Color backdrop = video::rgb5(31, 0, 0);
   video::load_bg_palette(0, &backdrop, 1);
   video::set_blend(video::BlendMode::Brighten, video::kLayerBackdrop, 0);
   video::set_blend_brightness(video::kBlendMax);
   video::set_window_rect(0, 0, 0, video::kScreenWidth, video::kScreenHeight);
   video::set_window_masks(video::kLayerAll, 0, 0, 0);   // effects inside window 0 only
   video::set_windows(true, false, false);
   video::set_window_lines(lines);
   hal::soft::latch();
   hal::soft::compose(g_frame);
   const uint16_t inside = g_frame[kRow * video::kScreenWidth + kLeft + 1];
   const uint16_t outside = g_frame[kRow * video::kScreenWidth + kRight + 1];
   const uint16_t other = g_frame[(kRow + 1) * video::kScreenWidth + kLeft + 1];
   CHECK(inside != outside && outside == other);   // only row kRow's edges hold the window
   video::set_window_lines(nullptr);
   hal::soft::latch();
   hal::soft::compose(g_frame);
   CHECK(g_frame[(kRow + 1) * video::kScreenWidth + kLeft + 1] == inside);   // the rectangle again
   video::set_windows(false, false, false);
   video::set_blend(video::BlendMode::Off, 0, 0);
}

} // namespace

/// Runs the N2 tests.
/// @return 0 when every check passed
int main()
{
   hal::soft::reset();
   gfx::palette::reset();
   gfx::sprites::reset();
   cell_edits();
   layouts();
   ledges();
   step_rules();
   camera();
   sprites_and_slots();
   in_place();
   walker_speed();
   window_lines();
   std::printf("n2_tests: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
