/// @file
/// Tests for the script VM, flags, variables and map triggers (B-1, B-2)
/// against tests/data/scripts (compiled by tools/script_compiler.py), with a
/// recording port and test natives.
#include "capsule/asset/asset.h"
#include "capsule/core/clock.h"
#include "capsule/core/random.h"
#include "capsule/script/combine_port.h"
#include "capsule/script/nursery_port.h"
#include "capsule/script/field_actions.h"
#include "capsule/script/field_effects.h"
#include "capsule/script/growth_port.h"
#include "capsule/script/menu.h"
#include "capsule/script/port.h"
#include "capsule/script/script.h"

#include "test_script_ids.h"

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
} // namespace capsule::hal::system

namespace
{

using namespace capsule;
using namespace script_ids;

/// What the test port saw and will answer.
struct Port
{
   char last[64];          ///< Last message, as ASCII.
   int messages;           ///< Messages shown.
   int typing;             ///< Frames the current message keeps typing.
   int buttons;            ///< Button presses queued.
   bool box;               ///< Box open.
   int8_t yesno;           ///< Answer to give.
   int16_t choice;         ///< Answer to give.
   uint8_t choice_count;   ///< Options of the last menu.
   uint8_t choice_cancel;  ///< Cancel of the last menu.
   char option0[16];       ///< First option of the last menu.
   int slow_calls;         ///< Calls of the slow native.
   int ticks;              ///< port::tick() calls.
};
Port g_port{};   ///< The test port.

} // namespace

namespace capsule::script::port
{

/// Records the message.
/// @param codes  text
void message(const text::Code* codes)
{
   int n = 0;
   while (codes[n] != text::kCodeEnd && n < 63)
   {
      g_port.last[n] = static_cast<char>(codes[n]);
      ++n;
   }
   g_port.last[n] = '\0';
   ++g_port.messages;
   g_port.typing = 2;
   g_port.box = true;
}
/// Types for two frames.
/// @return true while typing
bool message_busy()
{
   return g_port.typing > 0;
}
/// Closes.
void close_message()
{
   g_port.box = false;
}
/// One queued press.
/// @return true when one was queued
bool button()
{
   if (g_port.buttons == 0)
   {
      return false;
   }
   --g_port.buttons;
   return true;
}
/// Opens (nothing to draw).
void yesno_open()
{
}
/// The queued answer.
/// @return it
int8_t yesno_poll()
{
   return g_port.yesno;
}
/// Records the menu.
/// @param options  options
/// @param count    count
/// @param cancel   cancel index
void choice_open(const text::Code* const* options, uint8_t count, uint8_t cancel)
{
   g_port.choice_count = count;
   g_port.choice_cancel = cancel;
   int n = 0;
   while (options[0][n] != text::kCodeEnd && n < 15)
   {
      g_port.option0[n] = static_cast<char>(options[0][n]);
      ++n;
   }
   g_port.option0[n] = '\0';
}
/// The queued answer.
/// @return it
int16_t choice_poll()
{
   return g_port.choice;
}
/// Advances typing; counts frames.
void tick()
{
   ++g_port.ticks;
   if (g_port.typing > 0)
   {
      --g_port.typing;
   }
}

/// I4: message modes (ignored).
/// @param flags  the kMessage... bits
void message_mode(uint8_t flags)
{
   (void)flags;
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

namespace
{

/// What the test world saw.
struct World
{
   char log[256];     ///< Calls, as text.
   int walking;       ///< Frames the current walk keeps going.
   bool missing;      ///< Object 9 does not exist.
   bool blocked;      ///< C8: path_clear() says a step is blocked.
   uint32_t self;     ///< Self switches (bit obj * 4 + letter).
};
World g_world{};   ///< The test world.

/// Appends to the log.
/// @param text  what happened
void log(const char* text)
{
   std::strncat(g_world.log, text, sizeof g_world.log - std::strlen(g_world.log) - 1);
}

/// An object as text ("P" for the player).
/// @param obj  local ID or kObjectPlayer
/// @return text
const char* obj_name(uint8_t obj)
{
   static char buf[8];
   if (obj == capsule::script::kObjectPlayer)
   {
      return "P";
   }
   std::snprintf(buf, sizeof buf, "%u", obj);
   return buf;
}

} // namespace

namespace capsule::script::world
{

/// Records a walk of three frames; object 9 is missing.
/// @param obj    object
/// @param dir    direction
/// @param steps  cells
/// @return false for object 9
bool move(uint8_t obj, uint8_t dir, uint16_t steps)
{
   char b[32];
   std::snprintf(b, sizeof b, "move %s %u %u;", obj_name(obj), dir, steps);
   log(b);
   if (obj == 9)
   {
      return false;
   }
   g_world.walking = 3;
   return true;
}
/// C8: records the check of a walk.
/// @param obj    object
/// @param dir    direction
/// @param steps  cells
/// @return false when the world is blocked
bool path_clear(uint8_t obj, uint8_t dir, uint16_t steps)
{
   char b[32];
   std::snprintf(b, sizeof b, "clear %s %u %u;", obj_name(obj), dir, steps);
   log(b);
   return !g_world.blocked;
}
/// Counts down the walk.
/// @param obj  object
/// @return true while walking
bool moving(uint8_t obj)
{
   (void)obj;
   return g_world.walking-- > 0;
}
/// Records a turn.
/// @param obj  object
/// @param dir  direction
void face(uint8_t obj, uint8_t dir)
{
   char b[32];
   std::snprintf(b, sizeof b, "face %s %u;", obj_name(obj), dir);
   log(b);
}
/// Records a turn towards another.
/// @param obj    object
/// @param other  the other
void face_toward(uint8_t obj, uint8_t other)
{
   char b[32];
   std::snprintf(b, sizeof b, "toward %s ", obj_name(obj));
   log(b);
   std::snprintf(b, sizeof b, "%s;", obj_name(other));
   log(b);
}
/// Records showing or hiding.
/// @param obj  object
/// @param on   shown
void show(uint8_t obj, bool on)
{
   char b[32];
   std::snprintf(b, sizeof b, "%s %s;", on ? "show" : "hide", obj_name(obj));
   log(b);
}

/// Records a route (its length).
/// @param obj          object
/// @param code         route
/// @param len          bytes
/// @param from_script  from a script
/// @return true
bool route(uint8_t obj, const uint8_t* code, uint16_t len, bool from_script)
{
   char b[40];
   std::snprintf(b, sizeof b, "route %s %u %u;", obj_name(obj), len, code[0]);
   log(b);
   (void)from_script;
   g_world.walking = 2;
   return true;
}
/// Counts down the walk.
/// @return true while walking
bool routes_busy()
{
   return g_world.walking-- > 0;
}
/// Self switches: bit per object and letter.
/// @param obj     object
/// @param letter  letter
/// @return its state
bool self_switch(uint8_t obj, uint8_t letter)
{
   return ((g_world.self >> (obj * 4 + letter)) & 1) != 0;
}
/// Sets a self switch.
/// @param obj     object
/// @param letter  letter
/// @param on      set
void set_self_switch(uint8_t obj, uint8_t letter, bool on)
{
   const uint32_t bit = 1u << (obj * 4 + letter);
   g_world.self = on ? (g_world.self | bit) : (g_world.self & ~bit);
}
/// Queries: x = 10 + obj, y = 20 + obj, facing 3, map 7, steps 99, timer 5.
/// @param q    query
/// @param obj  object
/// @return the value
int32_t query(Query q, uint8_t obj)
{
   switch (q)
   {
   case Query::PosX:
      return obj == kObjectPlayer ? 1 : 10 + obj;
   case Query::PosY:
      return obj == kObjectPlayer ? 2 : 20 + obj;
   case Query::Facing:
      return 3;
   case Query::MapIndex:
      return 7;
   case Query::Steps:
      return 99;
   default:
      return 5;
   }
}
/// Records a warp; it takes two frames.
/// @param map         key
/// @param x           column
/// @param y           row
/// @param facing      facing
/// @param transition  transition
void warp(AssetKey map, int16_t x, int16_t y, uint8_t facing, uint8_t transition)
{
   char b[64];
   std::snprintf(b, sizeof b, "warp %08X %d %d %u %u;", static_cast<unsigned>(map), x, y, facing, transition);
   log(b);
   g_world.walking = 2;
}
/// Counts down the warp.
/// @return true while it runs
bool warping()
{
   return g_world.walking-- > 0;
}
/// Records a place.
/// @param obj     object
/// @param x       column
/// @param y       row
/// @param facing  facing
void place(uint8_t obj, int16_t x, int16_t y, uint8_t facing)
{
   char b[40];
   std::snprintf(b, sizeof b, "place %s %d %d %u;", obj_name(obj), x, y, facing);
   log(b);
}
/// Records a scroll.
/// @param dir    direction
/// @param cells  cells
/// @param speed  speed
void scroll(uint8_t dir, int16_t cells, uint8_t speed)
{
   char b[40];
   std::snprintf(b, sizeof b, "scroll %u %d %u;", dir, cells, speed);
   log(b);
}
/// Never scrolling.
/// @return false
bool scrolling()
{
   return false;
}
/// Records a tint.
/// @param color     color
/// @param strength  strength
/// @param frames    frames
void tint(hal::video::Color color, uint8_t strength, uint16_t frames)
{
   char b[40];
   std::snprintf(b, sizeof b, "tint %04X %u %u;", color, strength, frames);
   log(b);
}
/// Records a flash.
/// @param color   color
/// @param frames  frames
void flash(hal::video::Color color, uint16_t frames)
{
   char b[40];
   std::snprintf(b, sizeof b, "flash %04X %u;", color, frames);
   log(b);
}
/// Records a shake.
/// @param power   power
/// @param frames  frames
void shake(uint8_t power, uint16_t frames)
{
   char b[40];
   std::snprintf(b, sizeof b, "shake %u %u;", power, frames);
   log(b);
}
/// Records the timer.
/// @param seconds  seconds
void timer_start(int32_t seconds)
{
   char b[40];
   std::snprintf(b, sizeof b, "timer %d;", static_cast<int>(seconds));
   log(b);
}
/// Records the timer stop.
void timer_stop()
{
   log("timer stop;");
}
/// Records encounters on / off.
/// @param on  on
void encounters(bool on)
{
   log(on ? "enc on;" : "enc off;");
}
/// Records a vehicle class change.
/// @param vehicle  the class
void set_vehicle(uint8_t vehicle)
{
   char b[24];
   std::snprintf(b, sizeof b, "vehicle %u;", static_cast<unsigned>(vehicle));
   log(b);
}
/// Records a darkness level (F2).
/// @param level  the level
void set_darkness(uint8_t level)
{
   char b[24];
   std::snprintf(b, sizeof b, "darkness %u;", static_cast<unsigned>(level));
   log(b);
}
/// Records an escape (F2): the stub warps only when the bank holds a recorded escape point.
/// @param transition  the transition
/// @return whether it warped
bool escape(uint8_t transition)
{
   AssetKey to{};
   int16_t x = 0;
   int16_t y = 0;
   uint8_t facing = 0;
   if (!actions::escape_point(to, x, y, facing))
   {
      log("escape none;");
      return false;
   }
   char b[48];
   std::snprintf(b, sizeof b, "escape %08X %d %d %u;", static_cast<unsigned>(to), x, y,
                 static_cast<unsigned>(transition));
   log(b);
   g_world.walking = 2;
   return true;
}

/// @name I4 stubs: each records its call
///@{

/// Records a lock.
/// @param self  the context's object
/// @param all   every object
void lock(uint8_t self, bool all)
{
   (void)self;
   log(all ? "lockall;" : "lock;");
}
/// Records a release.
/// @param self  the context's object
/// @param all   every object
void release(uint8_t self, bool all)
{
   (void)self;
   log(all ? "releaseall;" : "release;");
}
/// Records a removal.
/// @param obj  object
void remove(uint8_t obj)
{
   char b[24];
   std::snprintf(b, sizeof b, "remove %s;", obj_name(obj));
   log(b);
}
/// Records a visibility change.
/// @param obj  object
/// @param on   shown
void set_visible(uint8_t obj, bool on)
{
   char b[24];
   std::snprintf(b, sizeof b, "visible %s %d;", obj_name(obj), on ? 1 : 0);
   log(b);
}
/// Records a template change.
/// @param obj   object
/// @param kind  kind
/// @param a     first value
/// @param b     second value
void set_template(uint8_t obj, TemplateOp kind, int32_t a, int32_t b)
{
   char t[40];
   std::snprintf(t, sizeof t, "template %s %u %d %d;", obj_name(obj), static_cast<unsigned>(kind),
                 static_cast<int>(a), static_cast<int>(b));
   log(t);
}
/// Records a door.
/// @param kind  kind
/// @param x     column
/// @param y     row
void door(DoorOp kind, int16_t x, int16_t y)
{
   char b[32];
   std::snprintf(b, sizeof b, "door %u %d %d;", static_cast<unsigned>(kind), x, y);
   log(b);
}
/// No door runs.
/// @return false
bool door_busy()
{
   return false;
}
/// Records the weather.
/// @param kind   kind
/// @param value  value
void weather(WeatherOp kind, int32_t value)
{
   char b[32];
   std::snprintf(b, sizeof b, "weather %u %d;", static_cast<unsigned>(kind), static_cast<int>(value));
   log(b);
}
/// Records a fade (it runs for two polls).
/// @param kind   kind
/// @param speed  speed
/// @param flags  flags
void fade(FadeOp kind, uint8_t speed, uint8_t flags)
{
   char b[32];
   std::snprintf(b, sizeof b, "fade %u %u %u;", static_cast<unsigned>(kind), static_cast<unsigned>(speed),
                 static_cast<unsigned>(flags));
   log(b);
   g_world.walking = 2;
}
/// The fade runs while the walk counter does.
/// @return true while it runs
bool fading()
{
   return g_world.walking-- > 0;
}
/// Records a metatile change.
/// @param x      column
/// @param y      row
/// @param tile   metatile
/// @param solid  collision
void set_metatile(int16_t x, int16_t y, int32_t tile, bool solid)
{
   char b[40];
   std::snprintf(b, sizeof b, "metatile %d %d %d %d;", x, y, static_cast<int>(tile), solid ? 1 : 0);
   log(b);
}
/// Records the respawn point.
/// @param value  value
void set_respawn(int32_t value)
{
   char b[24];
   std::snprintf(b, sizeof b, "respawn %d;", static_cast<int>(value));
   log(b);
}
/// Records a remembered warp.
/// @param slot  slot
/// @param map   map
/// @param x     column
/// @param y     row
void set_warp(WarpSlot slot, AssetKey map, int32_t x, int32_t y)
{
   char b[48];
   std::snprintf(b, sizeof b, "setwarp %u %08X %d %d;", static_cast<unsigned>(slot), static_cast<unsigned>(map),
                 static_cast<int>(x), static_cast<int>(y));
   log(b);
}
/// DS1: records an override set's apply or clear.
/// @param set    the set
/// @param apply  applied
void override_set(uint16_t set, bool apply)
{
   char b[32];
   std::snprintf(b, sizeof b, "override %u %d;", static_cast<unsigned>(set), apply ? 1 : 0);
   log(b);
}
///@}

} // namespace capsule::script::world

namespace capsule::script::audio
{

/// Records a sound.
/// @param kind  kind
/// @param args  operands
/// @param argc  how many
void play(SoundOp kind, const int32_t* args, uint8_t argc)
{
   char b[40];
   std::snprintf(b, sizeof b, "sound %u %u %d;", static_cast<unsigned>(kind), static_cast<unsigned>(argc),
                 argc > 0 ? static_cast<int>(args[0]) : -1);
   log(b);
}
/// No sound plays.
/// @param kind  kind
/// @return false
bool playing(SoundWait kind)
{
   (void)kind;
   return false;
}

} // namespace capsule::script::audio

namespace
{
/// One party record element the test port holds.
struct RoleCell
{
   uint8_t slot;      ///< Party slot.
   uint8_t role;      ///< Role ID.
   uint8_t element;   ///< Element.
   int32_t value;     ///< Its value.
};
constexpr int kRoleCells = 32;    ///< Elements the test party holds.
constexpr int kPartySlots = 3;    ///< Slots of the test party (the layout's party_size).
RoleCell g_roles[kRoleCells];     ///< Written elements.
int g_role_count = 0;             ///< Used cells.
int g_role_gets = 0;              ///< role_get() calls.

/// The cell of an element, made when @p make.
/// @param slot     party slot
/// @param role     role
/// @param element  element
/// @param make     add it when missing
/// @return the cell, or nullptr
RoleCell* role_cell(uint8_t slot, uint8_t role, uint8_t element, bool make)
{
   for (int i = 0; i < g_role_count; ++i)
   {
      if (g_roles[i].slot == slot && g_roles[i].role == role && g_roles[i].element == element)
      {
         return &g_roles[i];
      }
   }
   if (!make || g_role_count >= kRoleCells || slot >= kPartySlots)
   {
      return nullptr;
   }
   g_roles[g_role_count] = RoleCell{slot, role, element, 0};
   return &g_roles[g_role_count++];
}
} // namespace

namespace capsule::script::party
{

/// A written element, or 0.
/// @param slot     party slot
/// @param role     role
/// @param element  element
/// @return value
int32_t role_get(uint8_t slot, uint8_t role, uint8_t element)
{
   ++g_role_gets;
   const RoleCell* c = role_cell(slot, role, element, false);
   return c != nullptr ? c->value : 0;
}

/// Records an element (slots past the party's are ignored).
/// @param slot     party slot
/// @param role     role
/// @param element  element
/// @param value    value
void role_set(uint8_t slot, uint8_t role, uint8_t element, int32_t value)
{
   if (RoleCell* c = role_cell(slot, role, element, true))
   {
      c->value = value;
   }
}

} // namespace capsule::script::party

namespace capsule::script::game
{

/// add(a, b) returns a + b; slow(n) waits until called n times.
/// @param id      native
/// @param args    arguments
/// @param argc    argument count
/// @param result  result
/// @return Done or Wait
NativeStatus native(uint16_t id, const int32_t* args, uint8_t argc, int32_t& result)
{
   if (id == NATIVE_ADD && argc == 2)
   {
      result = args[0] + args[1];
      return NativeStatus::Done;
   }
   if (id == NATIVE_SLOW && argc == 1)
   {
      return ++g_port.slow_calls < args[0] ? NativeStatus::Wait : NativeStatus::Done;
   }
   std::printf("unexpected native %u\n", id);
   std::abort();
}

/// The stub game is never busy.
/// @return false
bool state_busy()
{
   return false;
}

} // namespace capsule::script::game

namespace
{
constexpr int kStubItems = 16;        ///< Item ids the items stub counts.
constexpr int kBagPolls = 2;          ///< bag_poll() calls before the stub's bag menu answers.
constexpr int32_t kBagChoice = 5;     ///< The item the stub's bag menu answers.
constexpr uint8_t kStubTime = 2;      ///< The stub's time of day.
int32_t g_items[kStubItems] = {};     ///< Counts per item id (G2).
int g_bag_polls = 0;                  ///< bag_poll() calls since bag_open().
} // namespace

namespace capsule::script::items
{

/// @param b  ignored (the stub counts by id)
void attach(inventory::Bag* b)
{
   (void)b;
}

/// @return nullptr (no bag)
inventory::Bag* bag()
{
   return nullptr;
}

/// @param item  id
/// @param n     count
/// @return true for an id the stub counts
bool give(uint16_t item, int32_t n)
{
   if (item >= kStubItems || n <= 0)
   {
      return false;
   }
   g_items[item] += n;
   return true;
}

/// @param item  id
/// @param n     count
/// @return true when the stub holds that many
bool take(uint16_t item, int32_t n)
{
   if (item >= kStubItems || n <= 0 || g_items[item] < n)
   {
      return false;
   }
   g_items[item] -= n;
   return true;
}

/// @param item  id
/// @return its count
int32_t count(uint16_t item)
{
   return item < kStubItems ? g_items[item] : 0;
}

/// Starts the stub menu.
void bag_open()
{
   g_bag_polls = 0;
}

/// @return -1 for kBagPolls calls, then kBagChoice
int32_t bag_poll()
{
   return ++g_bag_polls <= kBagPolls ? -1 : kBagChoice;
}

/// @param shop   ignored
/// @param money  ignored
void shop_open(uint8_t shop, VarId money)
{
   (void)shop;
   (void)money;
}

/// @return false (closed at once)
bool shop_poll()
{
   return false;
}

/// @param member  ignored
/// @param move    ignored
void learn_open(uint8_t member, uint16_t move)
{
   (void)member;
   (void)move;
}

/// @return 0 (forget the first move)
int16_t learn_poll()
{
   return 0;
}

/// Records a mart (it closes at once).
/// @param kind   kind
/// @param items  items
/// @param count  how many
void mart_open(uint8_t kind, const uint8_t* items, uint16_t count)
{
   (void)items;
   char b[24];
   std::snprintf(b, sizeof b, "mart %u %u;", static_cast<unsigned>(kind), static_cast<unsigned>(count));
   log(b);
}

/// The mart closes at once.
/// @return false
bool mart_poll()
{
   return false;
}

} // namespace capsule::script::items

namespace
{
constexpr int32_t kStubMembers = 3;   ///< Party members the boxes stub may deposit (G3).
constexpr int kBoxPolls = 1;          ///< poll() calls before the stub's boxes menu answers.
constexpr int32_t kBoxChoice = 2;     ///< The box (+ 1) the stub's boxes menu answers.
int32_t g_boxed = 0;                  ///< Records the stub holds.
int g_box_polls = 0;                  ///< poll() calls since open().
} // namespace

namespace capsule::script::boxes
{

/// @param st     ignored (the stub only counts)
/// @param party  ignored
void attach(storage::Storage* st, monster::Party* party)
{
   (void)st;
   (void)party;
}

/// @return nullptr (no storage)
storage::Storage* attached()
{
   return nullptr;
}

/// @return the records the stub holds
int32_t count()
{
   return g_boxed;
}

/// @param member  a member below kStubMembers goes in
/// @return true when it did
bool deposit(int32_t member)
{
   if (member < 0 || member >= kStubMembers)
   {
      return false;
   }
   ++g_boxed;
   return true;
}

/// @param slot  a slot below the records held comes out
/// @return true when it did
bool withdraw(int32_t slot)
{
   if (slot < 0 || slot >= g_boxed)
   {
      return false;
   }
   --g_boxed;
   return true;
}

/// Starts the stub menu.
void open()
{
   g_box_polls = 0;
}

/// @return -1 for kBoxPolls calls, then kBoxChoice
int32_t poll()
{
   return ++g_box_polls <= kBoxPolls ? -1 : kBoxChoice;
}

} // namespace capsule::script::boxes

/// The growth port's stub (G4: the statement `transform(<member>) -> var(v)`).
namespace capsule::script::growth
{

namespace
{

constexpr int kGrowPolls = 1;   ///< transform_poll() calls before the stub answers.
uint8_t g_grow_slot = 0;        ///< The member transform_open() was given.
int g_grow_polls = 0;           ///< transform_poll() calls since it opened.

} // namespace

/// @param slot  the member (1 changes, any other is stopped)
void transform_open(uint8_t slot)
{
   g_grow_slot = slot;
   g_grow_polls = 0;
}

/// @return kTransformRunning for kGrowPolls calls, then member 1's kTransformChanged or kTransformStopped
int16_t transform_poll()
{
   if (++g_grow_polls <= kGrowPolls)
   {
      return kTransformRunning;
   }
   return g_grow_slot == 1 ? kTransformChanged : kTransformStopped;
}

} // namespace capsule::script::growth

/// The combine port's stub (B1: the statements `combine open|run|preview|status|fee NAME ... -> var(v)`).
namespace capsule::script::combine
{

namespace
{

constexpr int kCombinePolls = 1;      ///< poll() calls before the stub answers.
constexpr int32_t kKindScale = 100;   ///< query(): kind * this + the members' sum + 1.
uint8_t g_combine_slots[kMaxCombineMembers] = {};   ///< The members run() was given.
uint8_t g_combine_count = 0;         ///< How many.
uint8_t g_combine_method = 0xFF;     ///< The method open() / run() was given.
bool g_combine_ran = false;          ///< run() (true) or open() (false) started the running statement.
int g_combine_polls = 0;             ///< poll() calls since it started.

} // namespace

/// @param lent   unused
/// @param party  unused
/// @param boxes  unused
/// @param rng    unused
void attach(battle::State* lent, monster::Party* party, storage::Storage* boxes, rng::Stream* rng)
{
   (void)lent;
   (void)party;
   (void)boxes;
   (void)rng;
}

/// @param on  unused
void set_menus(bool on)
{
   (void)on;
}

/// @param method  the method
void open(uint8_t method)
{
   g_combine_method = method;
   g_combine_ran = false;
   g_combine_polls = 0;
}

/// @param method  the method
/// @param slots   the members
/// @param count   how many
void run(uint8_t method, const uint8_t* slots, uint8_t count)
{
   g_combine_method = method;
   g_combine_count = count;
   for (uint8_t k = 0; k < count && k < kMaxCombineMembers; ++k)
   {
      g_combine_slots[k] = slots[k];
   }
   g_combine_ran = true;
   g_combine_polls = 0;
}

/// @return kCombineRunning for kCombinePolls calls, then kCombineDone (run) or kCombineBack (open)
int16_t poll()
{
   if (++g_combine_polls <= kCombinePolls)
   {
      return kCombineRunning;
   }
   return g_combine_ran ? kCombineDone : kCombineBack;
}

/// @param kind    the query
/// @param method  the method (0 expected)
/// @param slots   the members
/// @param count   how many
/// @return kind * kKindScale + method + the members' sum + 1
int32_t query(CombineOp kind, uint8_t method, const uint8_t* slots, uint8_t count)
{
   int32_t sum = 0;
   for (uint8_t k = 0; k < count; ++k)
   {
      sum += slots[k];
   }
   return static_cast<int32_t>(kind) * kKindScale + method + sum + 1;
}

} // namespace capsule::script::combine

/// The nursery port's stub (B2: the statement `nursery name <member> -> var(v)`, the only one a script bank without a
/// nursery may use).
namespace capsule::script::nursery
{

namespace
{

constexpr int kNamePolls = 1;      ///< poll() calls before the stub answers.
uint8_t g_name_member = 0xFF;      ///< The member name_open() was given.
int g_name_polls = 0;              ///< poll() calls since it opened.

} // namespace

/// @param party  unused
void attach(monster::Party* party)
{
   (void)party;
}

/// The party menu (not used by the fixture).
void pick_open()
{
   g_name_polls = 0;
}

/// @param member  the member
void name_open(uint8_t member)
{
   g_name_member = member;
   g_name_polls = 0;
}

/// @return kNurseryRunning for kNamePolls calls, then 1 (named)
int16_t poll()
{
   return ++g_name_polls <= kNamePolls ? kNurseryRunning : int16_t{1};
}

/// @param kind  unused
/// @param arg   unused
/// @return 0
int32_t query(NurseryOp kind, int32_t arg)
{
   (void)kind;
   (void)arg;
   return 0;
}

/// @param slot   unused
/// @param money  unused
/// @param paid   0
/// @return 0
int32_t withdraw(int32_t slot, int32_t money, int32_t& paid)
{
   (void)slot;
   (void)money;
   paid = 0;
   return 0;
}

} // namespace capsule::script::nursery

namespace capsule::clock
{

/// @return kStubTime
uint8_t time_of_day()
{
   return kStubTime;
}

uint32_t g_minutes = 0;   ///< The stub minute clock (F1: test_field_effects moves it).

/// @return g_minutes
uint32_t minutes()
{
   return g_minutes;
}

} // namespace capsule::clock

namespace
{

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

/// One frame of one context: frame(), then update().
/// @param c  context
/// @return update()'s result
bool step(script::Context& c)
{
   script::frame();
   return script::update(c);
}

/// Runs a context to its end (at most @p frames frames).
/// @param c       context
/// @param frames  frame limit
/// @return frames it took
int run(script::Context& c, int frames = 100)
{
   int n = 0;
   while (step(c) && n < frames)
   {
      ++n;
   }
   return n;
}

void test_flow()
{
   script::clear_all();
   script::Context c{};
   script::start(c, SCRIPT_FLOW);
   CHECK(run(c) == 0);   // never waits: done in one update
   CHECK(script::var(VAR_V8) == 7);
   CHECK(script::flag(FLAG_F_PERM));
   CHECK(script::var(VAR_V16) == 1000);   // 70 + 2000 saturates at the declared 1000 (P-18)
   CHECK(script::var(VAR_OUT) == 123456);
   CHECK(script::flag(FLAG_F_DAILY));     // when not game(other): this build is "base"
}

void test_vars_and_banks()
{
   script::clear_all();
   CHECK(script::var(VAR_VMIN) == 5);   // a range above 0 starts at its minimum
   script::set_var(VAR_VMIN, 200);
   CHECK(script::var(VAR_VMIN) == 10);
   script::add_var(VAR_VMIN, -100);
   CHECK(script::var(VAR_VMIN) == 5);
   script::set_var(VAR_V8, 300);
   CHECK(script::var(VAR_V8) == 255);
   script::set_var(VAR_OUT, -5);
   CHECK(script::var(VAR_OUT) == 0);
   script::add_var(VAR_OUT, 0x7FFFFFFF);
   script::add_var(VAR_OUT, 10);
   CHECK(script::var(VAR_OUT) == 0x7FFFFFFF);
   script::set_flag(FLAG_F_PERM, true);
   script::set_flag(FLAG_F_DAILY, true);
   script::set_flag(FLAG_F_AREA, true);
   script::set_flag(FLAG_F_MAP, true);
   script::set_var(VAR_ANSWER, 3);
   CHECK(script::lifetime_of(FLAG_F_AREA) == script::Lifetime::Area);
   script::change_map(1, 1);   // same area: only the map bank clears
   CHECK(!script::flag(FLAG_F_MAP) && script::flag(FLAG_F_AREA) && script::var(VAR_ANSWER) == 0);
   script::change_map(1, 2);
   CHECK(!script::flag(FLAG_F_AREA) && script::flag(FLAG_F_DAILY));
   script::new_day();
   CHECK(!script::flag(FLAG_F_DAILY) && script::flag(FLAG_F_PERM));
   CHECK(script::save_size() == script_ids::kScriptSaveBytes);
}

void test_talk()
{
   script::clear_all();
   g_port = Port{};
   g_port.yesno = -1;
   g_port.choice = -1;
   script::Context c{};
   script::start(c, SCRIPT_TALK);
   step(c);
   CHECK(g_port.messages == 1 && std::strcmp(g_port.last, "Hello") == 0 && c.wait == script::Wait::Typing);
   step(c);
   step(c);
   step(c);
   CHECK(c.wait == script::Wait::Button);   // typed; waits for a press
   g_port.buttons = 1;
   step(c);
   CHECK(c.wait == script::Wait::YesNo);
   g_port.yesno = 1;
   step(c);
   CHECK(script::var(VAR_ANSWER) == 1);
   CHECK(g_port.messages == 2 && std::strcmp(g_port.last, "From an asset.") == 0);   // text:key
   for (int i = 0; i < 4; ++i)
   {
      step(c);
   }
   g_port.buttons = 1;
   step(c);
   CHECK(c.wait == script::Wait::Choice && g_port.choice_count == 3 && g_port.choice_cancel == 2);
   CHECK(std::strcmp(g_port.option0, "One") == 0);
   g_port.choice = 1;
   step(c);
   CHECK(script::var(VAR_ANSWER) == 1 && !g_port.box && !script::running(c));
}

void test_natives_and_delay()
{
   script::clear_all();
   g_port = Port{};
   script::set_var(VAR_V8, 5);
   script::Context c{};
   script::start(c, SCRIPT_NATIVES);
   CHECK(step(c));
   CHECK(script::var(VAR_OUT) == 7 && c.wait == script::Wait::Native);
   const int frames = run(c);
   CHECK(g_port.slow_calls == 3);
   CHECK(frames == 3);   // slow done on its 3rd call; delay 2 resumes two updates later
   CHECK(script::var(VAR_OUT) == 8);
}

void test_call_table()
{
   script::clear_all();
   script::Context c{};
   script::set_var(VAR_V8, 1);
   script::start(c, SCRIPT_TABLE);
   run(c);
   CHECK(script::var(VAR_OUT) == 11);
   script::set_var(VAR_V8, 0);
   script::start(c, SCRIPT_TABLE);
   run(c);
   CHECK(script::var(VAR_OUT) == 10);
   script::start(c, SCRIPT_DEEP1);   // depth 3 of the fixture's stack_depth 4
   run(c);
   CHECK(script::var(VAR_OUT) == 3);
}

void test_ops_cap()
{
   script::clear_all();
   script::Context c{};
   script::start(c, SCRIPT_SPIN);
   CHECK(step(c));   // still running: yielded at the instruction cap
   const int32_t after_one = script::var(VAR_V16);
   CHECK(after_one > 0 && after_one < script::kOpsPerFrame);
   step(c);
   CHECK(script::var(VAR_V16) > after_one);
   script::stop(c);
   CHECK(!script::running(c));
}

void test_triggers()
{
   script::clear_all();
   script::ScriptId fired{};
   script::enter_map(asset::key_v1("map:FIELD"));
   CHECK(script::flag(FLAG_F_MAP));   // on_load ran
   script::set_var(VAR_ANSWER, 9);
   CHECK(script::poll_triggers(fired) && fired == SCRIPT_FIRE);
   CHECK(script::poll_triggers(fired));   // still true: fires again, as the reference games' polling
   script::Context c{};
   script::start(c, fired);
   run(c);
   CHECK(!script::flag(FLAG_F_MAP) && script::var(VAR_ANSWER) == 0);
   CHECK(!script::poll_triggers(fired));   // false now
   script::set_var(VAR_V8, 4);             // not a dependency: stays quiet without re-checking
   CHECK(!script::poll_triggers(fired));
   script::set_flag(FLAG_F_MAP, true);
   CHECK(script::poll_triggers(fired));
   script::enter_map(asset::key_v1("map:SMALL"));   // no scripts: nothing armed
   CHECK(!script::poll_triggers(fired));
}

void test_two_contexts()
{
   script::clear_all();
   g_port = Port{};
   script::Context a{};
   script::Context b{};
   script::Context idle{};
   script::start(a, SCRIPT_SPIN);
   script::start(b, SCRIPT_SPIN);
   script::frame();
   script::update(a);
   script::update(b);
   script::update(idle);
   CHECK(g_port.ticks == 1);   // the port advances once per frame, however many contexts run
   CHECK(script::running(a) && script::running(b) && !script::running(idle));
}

/// Map object commands: self resolves to the context's object, moves wait
/// until the world says the walk is over, missing objects do not wait.
void test_objects()
{
   script::clear_all();
   g_world = World{};
   script::set_var(VAR_V8, 5);
   script::Context c{};
   script::start(c, SCRIPT_OBJECTS, 7);
   CHECK(c.self == 7);
   CHECK(step(c) && c.wait == script::Wait::Object);
   CHECK(std::strcmp(g_world.log, "toward 7 P;move 7 2 2;") == 0);
   const int frames = run(c);
   CHECK(frames == 7);   // waits three frames per walk; the second walk ends with the script
   CHECK(std::strcmp(g_world.log, "toward 7 P;move 7 2 2;move P 1 1;toward P 7;face 3 3;hide 3;show 5;"
                                  "move 9 0 0;") == 0);
   // Without an object of its own, self commands do nothing.
   g_world = World{};
   script::start(c, SCRIPT_OBJECTS);
   CHECK(c.self == script::kNoSelf);
   run(c);
   CHECK(std::strcmp(g_world.log, "move P 1 1;face 3 3;hide 3;show 5;move 9 0 0;") == 0);
}

/// C8: `move ... checked`: the world's check first; a clear path walks (waits, ok = 1), a blocked one does not move
/// (ok = 0, no wait).
void test_checked_move()
{
   script::clear_all();
   g_world = World{};
   script::Context c{};
   script::start(c, SCRIPT_CHECKED_WALK);
   CHECK(step(c) && c.wait == script::Wait::Object);
   CHECK(std::strcmp(g_world.log, "clear P 1 2;move P 1 2;") == 0 && script::var(VAR_WALK_OK) == 1);
   run(c);
   g_world = World{};
   g_world.blocked = true;
   script::start(c, SCRIPT_CHECKED_WALK);
   run(c);
   CHECK(!script::running(c) && std::strcmp(g_world.log, "clear P 1 2;") == 0 && script::var(VAR_WALK_OK) == 0);
}

/// Eventing commands (D-17): routes, self switches, queries, variable ops,
/// entry, screen effects and the warp, against the recording world.
void test_eventing()
{
   script::clear_all();
   g_world = World{};
   g_port = Port{};
   script::Context c{};
   script::start(c, SCRIPT_EVENTING, 3);
   const int frames = run(c, 50);
   CHECK(!script::running(c) && frames > 3);
   CHECK(std::strncmp(g_world.log, "route 3 8 2;route 2 5 1;", 24) == 0);   // skip flag; repeat flag
   CHECK((g_world.self >> (3 * 4 + 1)) & 1);
   CHECK(script::var(VAR_OUT) == 13 * 3);           // pos_x(self) = 10 + 3, then *= 3
   CHECK(script::var(VAR_V16) == 2 + 99);           // pos_y(player) + steps
   CHECK(script::var(VAR_V8) == 3);                 // 7 % 4
   CHECK(script::var(VAR_NUM) == 42 && script::var(VAR_KEY) == 1);
   CHECK(script::var(VAR_ANSWER) == 0);             // self is gone after the warp
   CHECK(std::strstr(g_world.log, "place P 4 5 1;scroll 3 2 2;tint 001F 8 30;flash 7C00 10;shake 2 20;"
                                  "timer 90;timer stop;enc off;vehicle 1;vehicle 0;warp ") != nullptr);
   CHECK(std::strstr(g_world.log, " 1 2 0 6;") != nullptr);   // x 1, y 2, down, box
}

/// F2 (D-27 section 3): `darkness`, the world's flight points (`flight record` sets the implicit flag, `flight
/// visit` warps with its transition, `flight menu` lists the recorded ones and warps to the one picked) and
/// `escape` (nothing recorded: no warp, 0; recorded: the warp, 1), against the recording world.
void test_f2()
{
   namespace act = script::actions;
   script::clear_all();
   g_world = World{};
   g_port = Port{};
   CHECK(act::flight_count() == 2 && !act::recorded(FLIGHT_POINT_HOME));
   const script::FlightPointInfo* gate = act::flight_point(FLIGHT_POINT_TOWN_GATE);
   CHECK(gate != nullptr && gate->x == 2 && gate->y == 0 && gate->facing == 0);
   script::Context c{};
   script::start(c, SCRIPT_F2);
   run(c, 50);
   CHECK(!script::running(c) && act::recorded(FLIGHT_POINT_HOME) && !act::recorded(FLIGHT_POINT_TOWN_GATE));
   char want[96];
   std::snprintf(want, sizeof want, "darkness 2;warp %08X 2 0 0 4;warp %08X 1 1 0 255;escape none;",
                 static_cast<unsigned>(gate->map), static_cast<unsigned>(act::flight_point(FLIGHT_POINT_HOME)->map));
   CHECK(std::strcmp(g_world.log, want) == 0);
   CHECK(std::strcmp(g_port.option0, "Home") == 0 && g_port.choice_count == 1);   // only the recorded one
   CHECK(script::var(VAR_OUT) == 1 && script::var(VAR_ANSWER) == 0);
   // a recorded escape point: the warp, 1
   g_world = World{};
   // C8: the map's asset key in both halves of the key (a key over 2^31 included)
   act::record_escape(AssetKey{0xC0DE1234u}, 5, -6, 1);
   script::start(c, SCRIPT_F2_ESCAPE);
   run(c, 50);
   CHECK(std::strcmp(g_world.log, "escape C0DE1234 5 -6 1;") == 0 && script::var(VAR_ANSWER) == 1);
}

/// Field placeholders (D-22 decision 6): the bank's table, placeholder_text()
/// for each binding, name slots (cut, cleared by clear_all(), saved), and the
/// message bytes the port gets (FD 80+ placeholders, FA, FB, terms expanded).
void test_placeholders()
{
   script::clear_all();
   g_port = Port{};
   g_role_count = 0;
   CHECK(script::kBank.placeholder_count == 5);
   CHECK(script::placeholder(PLACEHOLDER_LEAD) != nullptr && script::placeholder(5) == nullptr);
   CHECK(script::placeholder(PLACEHOLDER_HERO)->kind == script::PlaceholderKind::Name);
   CHECK(script::placeholder(PLACEHOLDER_LEAD)->kind == script::PlaceholderKind::Record);
   CHECK(script::placeholder(PLACEHOLDER_LEAD)->param == 2);   // party slot
   text::Code out[text::kMaxPlaceholderCodes];
   CHECK(script::placeholder_text(PLACEHOLDER_HERO, out, text::kMaxPlaceholderCodes) == 0);   // empty slot
   const text::Code name[] = {'R', 'o', 'w', 'a', 'n', 'n', 'a', text::kCodeEnd};
   script::set_name(PLACEHOLDER_HERO, name);
   CHECK(script::placeholder_text(PLACEHOLDER_HERO, out, text::kMaxPlaceholderCodes) == 5);   // cut at 5
   CHECK(std::memcmp(out, "Rowan", 5) == 0);
   CHECK(script::placeholder_text(PLACEHOLDER_HERO, out, 2) == 2);   // the caller's capacity
   script::set_var(VAR_V16, 907);
   CHECK(script::placeholder_text(PLACEHOLDER_COUNT, out, text::kMaxPlaceholderCodes) == 3);
   CHECK(std::memcmp(out, "907", 3) == 0);
   script::set_var(VAR_V16, 0);
   CHECK(script::placeholder_text(PLACEHOLDER_COUNT, out, text::kMaxPlaceholderCodes) == 1 && out[0] == '0');
   CHECK(script::placeholder_text(PLACEHOLDER_TOWN, out, text::kMaxPlaceholderCodes) == 10);
   CHECK(std::memcmp(out, "Elm Hollow", 10) == 0);
   CHECK(script::placeholder_text(PLACEHOLDER_LEAD, out, text::kMaxPlaceholderCodes) == 0);   // no nickname
   // record bindings read the party through party::role_get(): a text role's codes, a number in decimal
   CHECK(script::placeholder(PLACEHOLDER_LEAD)->index == ROLE_NICKNAME && script::placeholder(PLACEHOLDER_LEAD)->text == 8);
   CHECK(script::placeholder(PLACEHOLDER_MET)->index == ROLE_MET_LEVEL && script::placeholder(PLACEHOLDER_MET)->text == 0);
   const char* nick = "Blobby";
   for (int i = 0; nick[i] != '\0'; ++i)
   {
      script::party::role_set(2, ROLE_NICKNAME, static_cast<uint8_t>(i), nick[i]);
   }
   CHECK(script::placeholder_text(PLACEHOLDER_LEAD, out, text::kMaxPlaceholderCodes) == 6);
   CHECK(std::memcmp(out, "Blobby", 6) == 0);
   script::party::role_set(1, ROLE_MET_LEVEL, 0, 105);
   CHECK(script::placeholder_text(PLACEHOLDER_MET, out, text::kMaxPlaceholderCodes) == 3);
   CHECK(std::memcmp(out, "105", 3) == 0);
   CHECK(script::placeholder_text(9, out, text::kMaxPlaceholderCodes) == 0);
   CHECK(script::save_size() == kScriptSaveBytes && script::kBank.name_bytes == 6);
   script::clear_all();
   CHECK(script::placeholder_text(PLACEHOLDER_HERO, out, text::kMaxPlaceholderCodes) == 0);
   // The message as the port receives it.
   script::Context c{};
   script::start(c, SCRIPT_GREET);
   step(c);
   const char expected[] = {'H', 'i', ' ', '\xFD', '\x80', '!', ' ', '\xFD', '\x81', ' ', 'i', 'n', ' ',
                            '\xFD', '\x82', '.', '\xFA', '\xFB', '\xFD', '\x83', ' ', 'E', 'l', 'm', ' ',
                            'H', 'o', 'l', 'l', 'o', 'w', 's', '\0'};
   CHECK(g_port.messages == 1 && std::memcmp(g_port.last, expected, sizeof expected) == 0);
   script::stop(c);
}

/// role_get / role_set (D-22 decision 2): custom and engine roles by name, ID and constant, through the port.
void test_records()
{
   script::clear_all();
   g_role_count = 0;
   g_role_gets = 0;
   script::party::role_set(2, ROLE_LEVEL, 0, 33);
   script::party::role_set(0, ROLE_NICKNAME, 7, 'z');
   script::Context c{};
   script::start(c, SCRIPT_RECORDS);
   CHECK(run(c) == 0);   // never waits
   CHECK(g_role_gets == 4);
   CHECK(script::var(VAR_MET) == 42);
   CHECK(role_cell(0, ROLE_MET_LEVEL, 0, false) != nullptr && role_cell(0, ROLE_MET_LEVEL, 0, false)->value == 42);
   CHECK(role_cell(1, ROLE_RIBBONS, 31, false) != nullptr && script::var(VAR_V8) == 1);
   CHECK(script::var(VAR_V16) == 33);   // an engine role (level) of slot 2
   const RoleCell* gender = role_cell(1, ROLE_GENDER, 0, false);   // slot var(v8) = 1
   CHECK(gender != nullptr && gender->value == 3);
   CHECK(role_cell(0, ROLE_RIBBONS, 0, false) != nullptr && role_cell(0, ROLE_RIBBONS, 0, false)->value == 1);
   CHECK(role_cell(0, ROLE_RIBBONS, 1, false) != nullptr && role_cell(0, ROLE_RIBBONS, 1, false)->value == 1);
   CHECK(script::var(VAR_ANSWER) == 'z');
   CHECK(ROLE_RIBBONS == 129 && ROLE_GENDER == 0x80 && ROLE_LEVEL == 5);
}

/// The bag statements (G2): give / take / count through the items port, the bag menu waits, the time of day.
void test_bag()
{
   script::clear_all();
   script::Context c{};
   script::start(c, SCRIPT_BAG);
   CHECK(run(c) == kBagPolls + 1);   // the frame that opens the bag menu, then one per poll while it is open
   CHECK(script::var(VAR_BAG_OK) == 1 && script::var(VAR_V8) == 2 && script::var(VAR_ANSWER) == 1);
   CHECK(g_items[3] == 1 && g_items[2] == 1);   // took one of three's two; gave one of id var(v8) = 2
   CHECK(script::var(VAR_V16) == kStubTime && script::var(VAR_MET) == kBagChoice);
}

/// The box statements (G3) through the stub boxes port: a deposit, the count, a withdrawal of slot var(v8) = 1 (the
/// stub holds one record, at slot 0: it fails), a deposit the stub refuses, the boxes menu after one poll.
void test_boxes()
{
   script::clear_all();
   g_boxed = 0;
   script::Context c{};
   script::start(c, SCRIPT_BOXES);
   CHECK(run(c) == kBoxPolls + 1);   // the frame that opens the boxes menu, then one per poll while it is open
   CHECK(script::var(VAR_BAG_OK) == 1 && script::var(VAR_V8) == 1 && script::var(VAR_ANSWER) == 0);
   CHECK(g_boxed == 1 && script::var(VAR_MET) == kBoxChoice);
}

/// The transform statement (G4) through the stub growth port: member 1 changes, member 0 is stopped; each waits
/// one poll.
void test_transform()
{
   script::clear_all();
   script::Context c{};
   script::start(c, SCRIPT_GROW);
   CHECK(run(c) == 2 * (script::growth::kGrowPolls + 1));   // each statement: its opening frame, then one per poll
   CHECK(script::var(VAR_V8) == script::growth::kTransformChanged);
   CHECK(script::var(VAR_ANSWER) == script::growth::kTransformStopped);
}

/// The combine statements (B1) through the stub combine port: the queries' operands (kind, method, members), run's
/// members (one an operand read from a variable) and the waits of run and open.
void test_combine()
{
   namespace cp = script::combine;
   script::clear_all();
   script::Context c{};
   script::start(c, SCRIPT_MIX);
   CHECK(run(c) == 2 * (cp::kCombinePolls + 1));   // run and open: their opening frame, then one per poll
   constexpr int32_t kPreview = 0 + 1 + 1;   // CombineOp::Preview * 100, members 0 + 1, + 1
   CHECK(script::var(VAR_V8) == static_cast<int32_t>(script::CombineOp::Preview) * cp::kKindScale + kPreview);
   CHECK(script::var(VAR_V16) == static_cast<int32_t>(script::CombineOp::Status) * cp::kKindScale + kPreview);
   CHECK(script::var(VAR_BAG_OK) == 0xFF);   // Fee * 100 + 2 is clamped to the u8 variable
   CHECK(cp::g_combine_method == 0 && cp::g_combine_count == 2 && cp::g_combine_slots[0] == 1);
   CHECK(cp::g_combine_slots[1] == script::var(VAR_V8) || cp::g_combine_slots[1] == monster::kMaxPartySize);
   CHECK(script::var(VAR_ANSWER) == cp::kCombineDone && script::var(VAR_OUT) == cp::kCombineBack);
}

/// F1 (D-27 section 2): the timed field effects of tests/data/battle_stats3 (ward by steps, refused while it runs;
/// glow by minutes, added up, not saved; lure by steps, started over): their implicit variables and expiry scripts,
/// the statements, the stack rules, steps, minutes and the expiry reports.
void test_field_effects()
{
   namespace fx = script::field_effects;
   script::clear_all();
   CHECK(fx::count() == 3);
   const script::FieldEffectInfo* ward = fx::info(0);
   CHECK(ward != nullptr && ward->var == raw(VAR_FIELD_EFFECT_WARD) &&
         ward->expiry_script == raw(SCRIPT_FIELD_EFFECT_END_WARD) && ward->unit == script::FieldEffectUnit::Steps &&
         ward->stack == script::FieldEffectStack::Refuse && (ward->flags & script::kFieldEffectPersists) != 0);
   const script::FieldEffectInfo* glow = fx::info(1);
   CHECK(glow != nullptr && glow->expiry_script == script::kNoExpiryScript &&
         glow->unit == script::FieldEffectUnit::Minutes && (glow->flags & script::kFieldEffectPersists) == 0);
   CHECK(fx::info(3) == nullptr && !fx::start(3, 1));
   // the statements: ward starts, the second start is refused; active / remaining; glow adds
   script::Context c{};
   script::start(c, SCRIPT_FX);
   run(c);
   CHECK(script::var(VAR_BAG_OK) == 1 && script::var(VAR_ANSWER) == 0);
   CHECK(script::var(VAR_V8) == 1 && script::var(VAR_V16) == 3);
   CHECK(fx::remaining(1) == 5 && fx::start(1, 7) && fx::remaining(1) == 12);
   CHECK(!fx::start(2, 0) && fx::start(2, 4) && fx::start(2, 2) && fx::remaining(2) == 2);   // lure starts over
   CHECK(script::var(VAR_FIELD_EFFECT_LURE) == 2);   // its implicit variable holds it
   // steps: lure ends first (reported once), then ward; glow does not count steps
   uint8_t which = 0xFF;
   fx::walked(2);
   CHECK(fx::remaining(0) == 1 && !fx::active(2) && fx::remaining(1) == 12);
   CHECK(fx::take_expired(&which) && which == 2 && !fx::take_expired(&which));
   fx::walked(5);
   CHECK(!fx::active(0) && fx::take_expired(&which) && which == 0);
   CHECK(fx::start(0, 3));   // a ward that ended starts again
   // minutes: the first tick reads the clock, the next ones count; a clock that wraps still counts forward
   clock::g_minutes = 0xFFFFFFFEu;
   fx::reset_clock();
   fx::tick();
   CHECK(fx::remaining(1) == 12);
   clock::g_minutes = 3;   // five minutes later, past the wrap
   fx::tick();
   CHECK(fx::remaining(1) == 7 && fx::remaining(0) == 3);
   clock::g_minutes = 20;
   fx::tick();
   CHECK(!fx::active(1) && fx::take_expired(&which) && which == 1);
   // stop: no report
   fx::stop(0);
   CHECK(!fx::active(0) && !fx::take_expired(&which));
}

} // namespace

/// A paged menu's entry text: "E" and the entry's number (two digits).
/// @param index  entry
/// @param out    codes
void paged_entry(int index, text::Code* out)
{
   out[0] = 'E';
   out[1] = static_cast<text::Code>('0' + index / 10);
   out[2] = static_cast<text::Code>('0' + index % 10);
   out[3] = text::kCodeEnd;
}

/// C6 (review4 finding 12): a list longer than a menu goes page by page (script::kMaxChoices - 1 entries and "More..."),
/// the last page wraps to the first, Back answers kBack; a short list shows whole.
void test_paged_menu()
{
   script::menu::Paged m{};
   constexpr int kTotal = 20;
   script::menu::open(m, kTotal, paged_entry);
   CHECK(g_port.choice_count == script::kMaxChoices && std::strcmp(g_port.option0, "E00") == 0);
   g_port.choice = script::kMaxChoices - 1;   // "More..."
   CHECK(script::menu::poll(m) == script::menu::kWaiting && std::strcmp(g_port.option0, "E07") == 0);
   g_port.choice = 2;
   CHECK(script::menu::poll(m) == 9);
   script::menu::open(m, kTotal, paged_entry);
   g_port.choice = script::kMaxChoices - 1;
   CHECK(script::menu::poll(m) == script::menu::kWaiting);
   CHECK(script::menu::poll(m) == script::menu::kWaiting && std::strcmp(g_port.option0, "E14") == 0);
   CHECK(g_port.choice_count == kTotal - 2 * (script::kMaxChoices - 1) + 1);   // six entries and "More..."
   g_port.choice = static_cast<int16_t>(g_port.choice_count - 1);
   CHECK(script::menu::poll(m) == script::menu::kWaiting && std::strcmp(g_port.option0, "E00") == 0);   // wrapped
   g_port.choice = static_cast<int16_t>(g_port.choice_cancel);
   CHECK(script::menu::poll(m) == script::menu::kBack && script::menu::poll(m) == script::menu::kBack);
   script::menu::open(m, script::kMaxChoices, paged_entry);
   CHECK(g_port.choice_count == script::kMaxChoices);   // fits: no "More..."
   g_port.choice = script::kMaxChoices - 1;
   CHECK(script::menu::poll(m) == script::kMaxChoices - 1);
   g_port.choice = 0;
}


/// I4 (D-29): the statements imported event scripts need, against the recording world: locks, a message that does
/// not wait, a route waited for by kObjectLast, removal, visibility, templates, doors, weather, fades, metatiles,
/// the respawn point, the dynamic warp, sound, marts, the busy state; a loose call that recurses past the stack goes
/// on as a jump; the map scripts I4 added (on_resume: every entry; on_warp_into: the first whose condition holds).
void test_i4()
{
   script::clear_all();
   g_world = World{};
   g_port = Port{};
   script::Context c{};
   script::start(c, SCRIPT_I4);
   run(c, 200);
   CHECK(!script::running(c));
   char want[256];
   std::snprintf(want, sizeof want,
                 "lockall;route 2 5 0;remove 2;visible 2 0;template 2 0 3 4;door 0 1 2;weather 0 3;fade 0 0 0;"
                 "metatile 1 2 5 1;respawn 4;setwarp 1 %08X 1 2;sound 6 1 9;mart 0 2;releaseall;",
                 static_cast<unsigned>(asset::key_v1("map:TOWN")));
   CHECK(std::strcmp(g_world.log, want) == 0);
   if (std::strcmp(g_world.log, want) != 0)
   {
      std::printf("  got  %s\n  want %s\n", g_world.log, want);
   }
   CHECK(script::var(VAR_OUT) == 7);    // the loose call returned
   CHECK(script::var(VAR_V8) == 30);    // the loop went on as jumps once the stack was full
   // the map scripts I4 added
   script::set_var(VAR_V8, 0);
   script::set_var(VAR_ANSWER, 0);
   CHECK(script::run_map_scripts(asset::key_v1("map:TOWN"), script::TriggerKind::OnResume));
   CHECK(script::var(VAR_ANSWER) == 5);
   CHECK(script::run_map_scripts(asset::key_v1("map:TOWN"), script::TriggerKind::OnWarpInto));
   CHECK(script::var(VAR_ANSWER) == 6 && script::var(VAR_OUT) == 7);   // only the first entry ran
   script::set_var(VAR_V8, 1);
   CHECK(!script::run_map_scripts(asset::key_v1("map:TOWN"), script::TriggerKind::OnWarpInto));
   CHECK(!script::run_map_scripts(asset::key_v1("map:SMALL"), script::TriggerKind::OnResume));
   CHECK(!script::immediate());
}

int main()
{
   test_flow();
   test_vars_and_banks();
   test_talk();
   test_natives_and_delay();
   test_call_table();
   test_ops_cap();
   test_triggers();
   test_two_contexts();
   test_objects();
   test_checked_move();
   test_eventing();
   test_placeholders();
   test_records();
   test_bag();
   test_boxes();
   test_transform();
   test_combine();
   test_field_effects();
   test_f2();
   test_paged_menu();
   test_i4();
   std::printf("script tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
