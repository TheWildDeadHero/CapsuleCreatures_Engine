/// @file
/// C13b engine tests: the world's cell interaction rules (capsule::map::Interaction, field::update()'s A press) on
/// the fixture tests/data/c13b: a rule per behavior, its facing, flag, vehicle class and level conditions, the
/// first rule that holds winning, an object and a sign before the rules, the field actions (F2) asked for a cell on
/// another level (water below a bank) and the rules kept for after them; the faced position's level.
#include "capsule/asset/asset.h"
#include "capsule/core/random.h"
#include "capsule/field/field.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/map/map.h"
#include "capsule/script/port.h"
#include "capsule/script/script.h"
#include "capsule/text/encode.h"

#include "../src/hal/soft/soft_video.h"

#include "c13b_script_ids.h"
#include "c13b_world_ids.h"

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
/// The fixture's doors make no sound.
/// @param sound  the key
void door_sound(AssetKey sound)
{
   (void)sound;
}
} // namespace capsule::field::game

namespace
{

using namespace capsule;

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

/// @name The fixture's behaviors (tests/data/c13b/world.json's order)
///@{
constexpr uint8_t kWater = 6;   ///< Water (only the swim class enters it; on level 1 below the bank).
constexpr uint8_t kFall = 7;    ///< A fall (swim enters it).
constexpr uint8_t kSwim = 1;    ///< The swim vehicle class.
///@}

/// A script's number (the requests carry numbers).
/// @param s  the script
/// @return its ScriptId value
constexpr uint16_t id(script::ScriptId s)
{
   return static_cast<uint16_t>(s);
}

uint16_t g_action_script = field::kNoScript;   ///< What the test's field actions answer for water and the fall.
uint32_t g_actions_asked = 0;                  ///< Times the dispatcher was asked.

/// The test's field actions: g_action_script for water and the fall, nothing for anything else.
/// @param face  what the walker faces
/// @return a script, or kNoScript
uint16_t actions(const field::ActionFace& face)
{
   ++g_actions_asked;
   const bool wet = face.target == script::ActionTarget::Behavior && (face.value == kWater || face.value == kFall);
   return wet ? g_action_script : field::kNoScript;
}

/// Presses A on a cell: places the walker, presses, takes the request (and releases the field).
/// @param x       the walker's column
/// @param y       its row
/// @param facing  its facing
/// @param[out] source  the request's source
/// @return the requested script, or kNoScript
uint16_t press(int x, int y, uint8_t facing, field::Source& source)
{
   field::place(x, y, facing);
   field::Intent a;
   a.interact = true;
   field::update(a);
   field::FieldEvent e{};
   uint16_t s = field::kNoScript;
   source = field::Source::NUM_SOURCES;
   if (field::take_event(&e) && e.what == field::Request::Script)
   {
      s = e.script;
      source = e.source;
   }
   field::release();
   return s;
}

/// Presses A and returns the script only.
/// @param x       the walker's column
/// @param y       its row
/// @param facing  its facing
/// @return the requested script, or kNoScript
uint16_t press(int x, int y, uint8_t facing)
{
   field::Source source{};
   return press(x, y, facing, source);
}

/// The rules: behavior, facing, flags, first match, objects and signs first, the level rule.
void rules()
{
   script::clear_all();
   hal::soft::reset();
   hal::vram::reset();
   gfx::sprites::reset();
   field::set_action_rules(nullptr);
   field::enter(world_ids::MAP_DEN, 2, 3, field::DIR_UP);
   field::Source source{};
   // the PC's cell holds an object without a script: it leaves the press to the cell's rule
   CHECK(press(1, 2, field::DIR_UP, source) == id(script_ids::SCRIPT_USE_PC) && source == field::Source::Interaction);
   CHECK(press(2, 2, field::DIR_UP) == id(script_ids::SCRIPT_WATCH));   // the screen, facing up
   CHECK(press(3, 1, field::DIR_LEFT) == field::kNoScript);   // ... not facing left
   CHECK(press(3, 2, field::DIR_UP) == id(script_ids::SCRIPT_READ));   // the shelf's flag rule fails: the next
   script::set_flag(static_cast<script::FlagId>(script_ids::FLAG_SHELF_OPEN), true);
   CHECK(press(3, 2, field::DIR_UP) == id(script_ids::SCRIPT_READ_OPEN));   // now the first rule holds
   CHECK(press(5, 2, field::DIR_UP, source) == id(script_ids::SCRIPT_TALK) && source == field::Source::Object);
   CHECK(press(6, 2, field::DIR_UP, source) == id(script_ids::SCRIPT_SIGN_READ) && source == field::Source::Sign);
   // the level rule: from the level-3 cell below the base it holds; from a level-0 cell beside it (level 0 meets
   // every level: the faced position takes it) it does not
   CHECK(press(4, 2, field::DIR_UP) == id(script_ids::SCRIPT_BASE_PC));
   CHECK(press(5, 1, field::DIR_LEFT) == field::kNoScript);
   CHECK(press(2, 3, field::DIR_DOWN) == field::kNoScript);   // a plain floor cell: nothing
}

/// The field actions between the two groups of rules: water on another level asks them; the fall's rules come after.
void after_actions()
{
   script::clear_all();
   hal::soft::reset();
   hal::vram::reset();
   gfx::sprites::reset();
   field::set_action_rules(actions);
   field::enter(world_ids::MAP_DEN, 2, 3, field::DIR_UP);
   // from the bank (level 3) the water below (level 1) is on another level: the actions are asked
   g_action_script = id(script_ids::SCRIPT_SWIM);
   g_actions_asked = 0;
   field::Source source{};
   CHECK(press(1, 4, field::DIR_DOWN, source) == id(script_ids::SCRIPT_SWIM) && source == field::Source::Action);
   CHECK(g_actions_asked == 1);
   // the fall on foot: the actions first; with none, the rule kept for after them
   CHECK(press(4, 4, field::DIR_DOWN) == id(script_ids::SCRIPT_SWIM));
   g_action_script = field::kNoScript;
   CHECK(press(4, 4, field::DIR_DOWN, source) == id(script_ids::SCRIPT_CANNOT_CLIMB) &&
         source == field::Source::Interaction);
   // swimming up at the fall (the class enters it: no actions) its first rule needs the flag
   field::set_vehicle(kSwim);
   g_actions_asked = 0;
   CHECK(press(4, 6, field::DIR_UP) == id(script_ids::SCRIPT_CANNOT_CLIMB) && g_actions_asked == 0);
   script::set_flag(static_cast<script::FlagId>(script_ids::FLAG_CLIMB), true);
   CHECK(press(4, 6, field::DIR_UP) == id(script_ids::SCRIPT_CLIMB_UP));
   field::set_vehicle(0);
   field::set_action_rules(nullptr);
}

} // namespace

/// Runs the tests.
/// @return 0 when all pass
int main()
{
   rules();
   after_actions();
   std::printf("c13b_tests: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
