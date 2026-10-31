/// @file
/// The engine's defaults of the I4 script ports (capsule/script/port.h, D-29): what imported event scripts need
/// beyond the engine's own statements. Every definition is weak (CAPSULE_WEAK): a game that has doors, weather,
/// fades, metatile changes, sound or marts defines its own, and keeps them.
///
/// - Message modes: ignored (every message types and is waited for as Message does).
/// - Locks: field::lock() / field::release() (every object; the field locks anyway while it runs a request).
/// - Removal: its template's hide flag is set and the object leaves the map (field::remove_object()): it stays
///   away until the flag clears, then comes back when the field next looks for objects (C10).
/// - Visibility: field::set_object_visible(), field::show_walker() for the walker.
/// - Templates: Pos and Here move the template's cell (field::set_template_cell()); Movement names the game's own
///   numbering, which the engine does not know: nothing.
/// - Doors (DS1): the shown map's door animations (capsule/field/doors.h): Open / Close animate the door bound to the
///   cell, SetOpen / SetClosed draw it at once, door_busy() while one animates.
/// - Override sets (DS1): map::set_override_set() on the shown map, and the set's flag or variable written when it
///   persists (so it is applied again when the field enters its map).
/// - Weather, fades, metatiles, the respawn point, remembered warps, sound, marts and the busy state: nothing, and
///   nothing to wait for.
#include "capsule/script/port.h"

#include "capsule/core/memory.h"
#include "capsule/field/doors.h"
#include "capsule/field/field.h"
#include "capsule/map/map.h"
#include "capsule/script/script.h"

namespace capsule::script::port
{

CAPSULE_WEAK void message_mode(uint8_t flags)
{
   (void)flags;
}

} // namespace capsule::script::port

namespace capsule::script::world
{

CAPSULE_WEAK void lock(uint8_t self, bool all)
{
   (void)self;
   (void)all;
   field::lock();
}

CAPSULE_WEAK void release(uint8_t self, bool all)
{
   (void)self;
   (void)all;
   field::release();
}

CAPSULE_WEAK void remove(uint8_t obj)
{
   const map::Map* m = map::current();
   if (m != nullptr && obj < m->object_count && m->objects[obj].flag != map::kNoEventCondition)
   {
      set_flag(FlagId{m->objects[obj].flag}, true);   // it stays away until the flag clears
   }
   field::remove_object(obj);   // C10: and comes back by the field's next look once it has
}

CAPSULE_WEAK void set_visible(uint8_t obj, bool on)
{
   if (obj == kObjectPlayer)
   {
      field::show_walker(on);
      return;
   }
   field::set_object_visible(obj, on);
}

CAPSULE_WEAK void set_template(uint8_t obj, TemplateOp kind, int32_t a, int32_t b)
{
   if (kind == TemplateOp::Pos)
   {
      field::set_template_cell(obj, a, b);
   }
   else if (kind == TemplateOp::Here)
   {
      const field::Object* o = field::object(obj);
      if (o != nullptr)
      {
         field::set_template_cell(obj, o->x, o->y);
      }
   }
}

CAPSULE_WEAK void door(DoorOp kind, int16_t x, int16_t y)
{
   switch (kind)
   {
   case DoorOp::Open:
      field::doors::open(x, y);
      break;
   case DoorOp::Close:
      field::doors::close(x, y);
      break;
   case DoorOp::SetOpen:
      field::doors::set_open(x, y);
      break;
   case DoorOp::SetClosed:
      field::doors::set_closed(x, y);
      break;
   default:
      break;   // Wait is the VM's (door_busy())
   }
}

CAPSULE_WEAK bool door_busy()
{
   return field::doors::busy();
}

CAPSULE_WEAK void override_set(uint16_t set, bool apply)
{
   if (set >= map::kWorld.override_set_count)
   {
      return;
   }
   const map::OverrideSet& s = map::kWorld.override_sets[set];
   if (s.persist == map::OverridePersist::Flag)
   {
      set_flag(static_cast<FlagId>(s.id), apply);
   }
   else if (s.persist == map::OverridePersist::Var)
   {
      set_var(static_cast<VarId>(s.id), apply ? s.value : 0);
   }
   map::set_override_set(set, apply);   // the shown map's: drawn now (another map's shows when it is entered)
}

CAPSULE_WEAK void weather(WeatherOp kind, int32_t value)
{
   (void)kind;
   (void)value;
}

CAPSULE_WEAK void fade(FadeOp kind, uint8_t speed, uint8_t flags)
{
   (void)kind;
   (void)speed;
   (void)flags;
}

CAPSULE_WEAK bool fading()
{
   return false;
}

CAPSULE_WEAK void set_metatile(int16_t x, int16_t y, int32_t tile, bool solid)
{
   (void)x;
   (void)y;
   (void)tile;
   (void)solid;
}

CAPSULE_WEAK void set_respawn(int32_t value)
{
   (void)value;
}

CAPSULE_WEAK void set_warp(WarpSlot slot, AssetKey map, int32_t x, int32_t y)
{
   (void)slot;
   (void)map;
   (void)x;
   (void)y;
}

} // namespace capsule::script::world

namespace capsule::script::audio
{

CAPSULE_WEAK void play(SoundOp kind, const int32_t* args, uint8_t argc)
{
   (void)kind;
   (void)args;
   (void)argc;
}

CAPSULE_WEAK bool playing(SoundWait kind)
{
   (void)kind;
   return false;
}

} // namespace capsule::script::audio

namespace capsule::script::items
{

CAPSULE_WEAK void mart_open(uint8_t kind, const uint8_t* items, uint16_t count)
{
   (void)kind;
   (void)items;
   (void)count;
}

CAPSULE_WEAK bool mart_poll()
{
   return false;
}

} // namespace capsule::script::items

namespace capsule::script::game
{

CAPSULE_WEAK bool state_busy()
{
   return false;
}

} // namespace capsule::script::game
