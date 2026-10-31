/// @file
/// C10 tests (the R7 review's engine findings) on the fixture tests/data/c10: the script VM's options (the next
/// instruction on the frame after a wait ends; `random` as a modulo draw taken even for a bound of 0; u16 variables
/// that wrap while narrower ones saturate), the selected object and the face opposite the walker's facing, session
/// flags and variables (never saved), an immediate trigger (no request, the walker keeps moving, the cell's next
/// trigger still checked), on_resume before the objects spawn, an object's level after each step, a removed object
/// that comes back once its flag clears, a spotter that stops spotting, and a text box style (asset::TextBox) with
/// the fonts its Font control selects; text tables of two languages (the build took "en"). C11: a saved game's
/// map entry (field::enter_saved(): on_load only, the game's view and object hooks, on_return).
#include "capsule/asset/asset.h"
#include "capsule/core/assert.h"
#include "capsule/core/random.h"
#include "capsule/field/field.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/map/map.h"
#include "capsule/script/data.h"
#include "capsule/script/port.h"
#include "capsule/script/script.h"
#include "capsule/text/encode.h"
#include "capsule/text/window.h"

#include "../src/hal/soft/soft_video.h"

#include "c10_script_ids.h"
#include "c10_world_ids.h"

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

/// Test build: no keys held.
/// @return 0
capsule::hal::input::KeyBits capsule::hal::input::held()
{
   return 0;
}

// The fixture's scripts show nothing: the port does nothing (qualified definitions, as R7 finding 12 asks of ports).

/// Shows nothing.
/// @param codes  text
void capsule::script::port::message(const text::Code* codes)
{
   (void)codes;
}
/// Never busy.
/// @return false
bool capsule::script::port::message_busy()
{
   return false;
}
/// Nothing to close.
void capsule::script::port::close_message()
{
}
/// No presses.
/// @return false
bool capsule::script::port::button()
{
   return false;
}
/// Nothing to open.
void capsule::script::port::yesno_open()
{
}
/// Answers yes.
/// @return 1
int8_t capsule::script::port::yesno_poll()
{
   return 1;
}
/// Nothing to open.
/// @param options  options
/// @param count    count
/// @param cancel   cancel index
void capsule::script::port::choice_open(const text::Code* const* options, uint8_t count, uint8_t cancel)
{
   (void)options;
   (void)count;
   (void)cancel;
}
/// Answers 0.
/// @return 0
int16_t capsule::script::port::choice_poll()
{
   return 0;
}
/// Nothing to advance.
void capsule::script::port::tick()
{
}
/// Box position (ignored).
/// @param pos  position
void capsule::script::port::message_position(uint8_t pos)
{
   (void)pos;
}
/// Number entry (ignored).
/// @param digits  digits
void capsule::script::port::number_open(uint8_t digits)
{
   (void)digits;
}
/// The number answer.
/// @return 0
int32_t capsule::script::port::number_poll()
{
   return 0;
}
/// Key input: A.
/// @return 1
int8_t capsule::script::port::key_poll()
{
   return 1;
}

/// No party: every role reads 0.
/// @param slot     party slot
/// @param role     role
/// @param element  element
/// @return 0
int32_t capsule::script::party::role_get(uint8_t slot, uint8_t role, uint8_t element)
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
void capsule::script::party::role_set(uint8_t slot, uint8_t role, uint8_t element, int32_t value)
{
   (void)slot;
   (void)role;
   (void)element;
   (void)value;
}

/// The fixture calls no natives.
/// @param id      native
/// @param args    arguments
/// @param argc    count
/// @param result  result
/// @return Done
capsule::script::game::NativeStatus capsule::script::game::native(uint16_t id, const int32_t* args, uint8_t argc,
                                                                  int32_t& result)
{
   (void)id;
   (void)args;
   (void)argc;
   result = 0;
   return NativeStatus::Done;
}

/// The fixture has no step hooks.
/// @param hook  hook ID
/// @return kNoScript
uint16_t capsule::field::game::step_hook(uint16_t hook)
{
   (void)hook;
   return kNoScript;
}

namespace
{

using namespace capsule;
using namespace script_ids;

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

constexpr int kStepFrames = 64;   ///< Enough frames for one walked step.
constexpr uint8_t kStepper = 1;   ///< plain's objects: the stepper, the watcher, the removable one.
constexpr uint8_t kWatcher = 2;   ///< The spotter.
constexpr uint8_t kRemovable = 3; ///< Removed and brought back.
constexpr uint8_t kHidden = 0;    ///< Hidden by on_resume.
constexpr uint8_t kRaised = 3;    ///< The level of plain's raised cell (3, 2).
constexpr int32_t kU16Top = 65535;   ///< A u16 variable's highest value.
constexpr uint32_t kRollBound = 10;  ///< rolls: random(10).
constexpr uint8_t kSmallTop = 9;     ///< The narrow variable's highest value.

/// Fresh video and scripts, plain entered with the walker at a cell.
/// @param x       column
/// @param y       row
/// @param facing  the walker's facing
void enter_plain(int x, int y, uint8_t facing)
{
   hal::soft::reset();
   hal::vram::reset();
   gfx::palette::reset();
   gfx::sprites::reset();
   script::clear_all();
   field::leave();
   field::enter(world_ids::MAP_PLAIN, x, y, facing);
}

/// Walks the walker one cell (the field's own movement), then lets it stand.
/// @param d  the direction
void walk(uint8_t d)
{
   const int16_t x = field::walker().x;
   const int16_t y = field::walker().y;
   field::Intent in;
   in.direction = d;
   for (int i = 0; i < kStepFrames && field::walker().x == x && field::walker().y == y; ++i)
   {
      field::update(in);
   }
   for (int i = 0; i < kStepFrames && field::walker().motion != field::Motion::Idle; ++i)
   {
      field::update(field::Intent{});   // the step's last frames: it arrives
   }
}

/// Runs a context frame by frame.
/// @param c       the context
/// @param frames  how many frames
void frames(script::Context& c, int frames)
{
   for (int i = 0; i < frames; ++i)
   {
      script::update(c);
   }
}

/// R7 finding 2: with "resume": "next_frame" the instruction after a delay runs on the frame after the wait ends.
void resume_next_frame()
{
   script::clear_all();
   CHECK((script::kBank.vm_flags & script::kVmResumeNextFrame) != 0);
   script::Context c;
   script::start(c, SCRIPT_PAUSES, script::kNoSelf);
   frames(c, 3);   // the delay's frame, then two frames: the wait ends on the third
   CHECK(script::var(VAR_AFTER) == 0 && script::running(c));
   frames(c, 1);   // the next instruction: one frame later
   CHECK(script::var(VAR_AFTER) == 1);
}

/// R7 findings 4 and 8: `random` is next16() % n (drawn for n = 0 too: 0); u16 variables wrap, narrower ones
/// saturate.
void random_and_wrap()
{
   script::clear_all();
   rng::gameplay.state = 0x1234u;
   rng::Stream s = rng::gameplay;
   const uint32_t first = rng::next16(s) % kRollBound;
   rng::next16(s);   // random(var(zero)): drawn, 0
   script::run_now(SCRIPT_ROLLS);
   CHECK(script::var(VAR_ROLL) == static_cast<int32_t>(first) && script::var(VAR_ZERO) == 0);
   CHECK(rng::gameplay.state == s.state);   // two draws
   script::run_now(SCRIPT_WRAPS);
   CHECK(script::var(VAR_HITS) == 0);        // 65535 + 1
   CHECK(script::var(VAR_GATE) == kU16Top);  // 0 - 1
   CHECK(script::var(VAR_SMALL) == kSmallTop);   // 9 + 5 in 0..9: saturates
}

/// R7 finding 13: session flags and variables live through map changes and are never saved; a new game clears them.
void session_lifetime()
{
   script::clear_all();
   script::set_flag(FLAG_PASSING, true);
   script::set_var(VAR_VISITS, 3);
   CHECK(script::lifetime_of(FLAG_PASSING) == script::Lifetime::Session);
   script::change_map(0, 1);   // a map change and an area change
   CHECK(script::flag(FLAG_PASSING) && script::var(VAR_VISITS) == 3);
   CHECK(script::kSaveSubBanks == 2 * script::kSavedLifetimes + 1 && script::kSavedLifetimes == script::kNumLifetimes - 1);
   script::clear_all();
   CHECK(!script::flag(FLAG_PASSING) && script::var(VAR_VISITS) == 0);
}

/// R7 finding 10: `face selected opposite_player`: the script's object, or the walker without one, turns opposite
/// the walker's facing (not towards it).
void face_opposite()
{
   enter_plain(1, 1, field::DIR_RIGHT);
   script::Context c;
   script::start(c, SCRIPT_TURNS, kStepper);   // the stepper (at 2, 2) talked to while the walker faces right
   frames(c, 1);
   CHECK(field::object(kStepper) != nullptr && field::object(kStepper)->facing == field::DIR_LEFT);
   script::start(c, SCRIPT_TURNS, script::kNoSelf);   // a trigger's script: the walker itself turns around
   frames(c, 1);
   CHECK(field::walker().facing == field::DIR_LEFT);
}

/// R7 finding 3: an immediate trigger runs at once and the walker keeps going; the cell's plain trigger after it is
/// still checked (it fires while gate is 0).
void immediate_triggers()
{
   enter_plain(1, 4, field::DIR_RIGHT);
   script::set_var(VAR_GATE, 1);   // the plain trigger stays quiet
   walk(field::DIR_RIGHT);
   walk(field::DIR_RIGHT);   // onto (3, 4)
   CHECK(field::walker().x == 3 && script::var(VAR_HITS) == 1 && !field::has_event());
   walk(field::DIR_RIGHT);   // it keeps walking
   CHECK(field::walker().x == 4);
   enter_plain(2, 4, field::DIR_RIGHT);   // gate 0: the immediate one runs, then the plain one asks for its script
   walk(field::DIR_RIGHT);
   field::FieldEvent e{};
   CHECK(script::var(VAR_HITS) == 1 && field::take_event(&e) && e.what == field::Request::Script &&
         e.script == raw(SCRIPT_STOP_HERE));
}

/// R7 finding 6: on_resume runs before the objects spawn (it hides one by its flag: never spawned).
void resume_before_spawn()
{
   enter_plain(1, 1, field::DIR_DOWN);
   CHECK(script::flag(FLAG_RESUME_HIDES));
   CHECK(field::object(kHidden) != nullptr && !field::object(kHidden)->present);
}

/// C11 (R8 finding 4): what enter_saved()'s hooks saw (the order its scripts and steps ran in).
struct SavedSeen
{
   int view_calls = 0;          ///< restore_view() calls.
   int object_calls = 0;        ///< restore_objects() calls.
   bool view_before_load = false;      ///< restore_view() ran before on_load.
   bool objects_after_resume = false;  ///< restore_objects() ran after on_resume and the spawn ...
   bool objects_before_return = false; ///< ... and before on_return.
};

SavedSeen g_seen;   ///< The hooks' record.

constexpr int kEditX = 2;   ///< The cell the view hook edits.
constexpr int kEditY = 2;   ///< Its row.

/// The view hook: a cell edit before the load scripts.
void restore_view()
{
   ++g_seen.view_calls;
   g_seen.view_before_load = script::var(VAR_LOADS) == 0;
   const map::Map& m = map::kWorld.maps[world_ids::MAP_PLAIN];
   map::MetatileId layers[map::kMaxLayers];
   for (int l = 0; l < map::kMaxLayers; ++l)
   {
      layers[l] = map::metatile_at(m, l, 0, 0);
   }
   map::set_cell(kEditX, kEditY, map::cell_at(m, 0, 0), layers);
}

/// The objects hook: after on_resume and the spawn, before on_return; it moves the stepper.
void restore_objects()
{
   ++g_seen.object_calls;
   g_seen.objects_after_resume = script::flag(FLAG_RESUME_HIDES) && field::object(kStepper) != nullptr &&
                                 field::object(kStepper)->present;
   g_seen.objects_before_return = script::var(VAR_RETURNS) == 0;
   field::place_object(kStepper, kEditX, kEditY + 1, field::DIR_UP);
}

/// C11 (R8 finding 4): a saved game's map entry (field::enter_saved()): no on_transition, the view hook before
/// on_load, on_resume, the objects, the objects hook, on_return; enter() runs on_transition and no on_return.
void saved_entry()
{
   hal::soft::reset();
   hal::vram::reset();
   gfx::palette::reset();
   gfx::sprites::reset();
   script::clear_all();
   field::leave();
   g_seen = SavedSeen{};
   const field::SavedEntry hooks{restore_view, restore_objects};
   field::enter_saved(world_ids::MAP_PLAIN, 1, 1, field::DIR_DOWN, hooks);
   CHECK(script::var(VAR_TRANSITIONS) == 0);
   CHECK(script::var(VAR_LOADS) == 1);
   CHECK(script::var(VAR_RETURNS) == 1);
   CHECK(g_seen.view_calls == 1 && g_seen.view_before_load);
   CHECK(g_seen.object_calls == 1 && g_seen.objects_after_resume && g_seen.objects_before_return);
   CHECK(map::is_edited(kEditX, kEditY) && !map::is_edited(0, 0));
   const field::Object* o = field::object(kStepper);
   CHECK(o != nullptr && o->x == kEditX && o->y == kEditY + 1 && o->facing == field::DIR_UP);
   // a warp's entry
   enter_plain(1, 1, field::DIR_DOWN);
   CHECK(script::var(VAR_TRANSITIONS) == 1 && script::var(VAR_LOADS) == 1 && script::var(VAR_RETURNS) == 0);
   CHECK(!map::is_edited(kEditX, kEditY));
}

/// R7 finding 11: an object's level follows the cell it steps onto.
void object_levels()
{
   enter_plain(1, 6, field::DIR_DOWN);
   CHECK(field::object(kStepper)->elevation == 1);
   CHECK(field::move_object(kStepper, field::DIR_RIGHT, 1));
   for (int i = 0; i < kStepFrames && field::object_moving(kStepper); ++i)
   {
      field::update(field::Intent{});
   }
   CHECK(field::object(kStepper)->x == 3 && field::object(kStepper)->elevation == kRaised);
}

/// R7 finding 16: a removed object stays away while its flag is set and comes back on the next look once it clears.
void removal_and_return()
{
   enter_plain(1, 6, field::DIR_UP);
   CHECK(field::object(kRemovable)->present);
   script::run_now(SCRIPT_REMOVE_IT);   // the engine's removal: its flag set, off the map
   CHECK(script::flag(FLAG_REMOVABLE_GONE) && !field::object(kRemovable)->present);
   walk(field::DIR_UP);
   CHECK(!field::object(kRemovable)->present);   // the flag holds it away
   script::run_now(SCRIPT_BRING_BACK);
   CHECK(!field::object(kRemovable)->present);   // not before the field looks again
   walk(field::DIR_UP);
   CHECK(field::object(kRemovable)->present);
}

/// C10 (R7 finding 15's engine part): a spotter that stops spotting asks for nothing.
void stopped_sight()
{
   enter_plain(1, 3, field::DIR_RIGHT);
   field::stop_sight(kWatcher);
   walk(field::DIR_RIGHT);
   walk(field::DIR_RIGHT);   // into its line of sight (row 3, three cells)
   for (int i = 0; i < kStepFrames; ++i)
   {
      field::update(field::Intent{});
   }
   CHECK(!field::has_event() && field::object(kWatcher)->blind && field::walker().x == 3);
   enter_plain(1, 3, field::DIR_RIGHT);   // a new map entry: it spots again
   CHECK(!field::object(kWatcher)->blind);
   walk(field::DIR_RIGHT);
   walk(field::DIR_RIGHT);
   for (int i = 0; i < kStepFrames && !field::has_event(); ++i)
   {
      field::update(field::Intent{});
   }
   CHECK(field::has_event());   // the spotter's approach and request
}

/// Text box styles as data: the window's settings, its fonts and its frame from the asset; the build's language.
void text_box_style()
{
   const asset::TextBox* s = asset::text_box(asset::key_v1("text_box:C10_MESSAGE"));
   const text::PropFont* font = asset::font(asset::key_v1("font:C10_BOX"));
   CHECK(s != nullptr && font != nullptr);
   if (s == nullptr)
   {
      return;
   }
   const text::window::Config& c = s->window;
   CHECK(c.font == font && c.bg == 1 && c.char_block == asset::kLayerCharBlock && c.first_tile == 200);
   CHECK(c.x == 2 && c.y == 15 && c.width == 6 && c.height == 2 && c.top == 1 && c.lines == 2);
   CHECK(c.typing == text::window::kTypingCounter && c.char_delay == 3 && c.speedup == text::window::kSpeedupPerFrame &&
         c.lead_frames == 2 && c.scroll_speed == 2 && c.arrow == text::window::kNoArrow);
   CHECK(c.font_count == 4 && c.fonts != nullptr && c.fonts[3] == font && c.fonts[0] == nullptr);
   CHECK(s->frame.first_tile == 300 && s->frame.palette_bank == 9 && s->frame.pad_left == 1);
   // the English table's texts (the French table's same key is not in this build)
   const text::Code* hello = asset::text(asset::key_v1("text:C10.HELLO"));
   CHECK(hello != nullptr && hello[0] == 'A' && hello[1] == 'B');
}

}   // namespace

int main()
{
   resume_next_frame();
   random_and_wrap();
   session_lifetime();
   face_opposite();
   immediate_triggers();
   resume_before_spawn();
   object_levels();
   removal_and_return();
   stopped_sight();
   text_box_style();
   saved_entry();
   std::printf("c10 tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
