/// @file
/// The field's props (C2, capsule/field/field.h place_prop()): sprites standing on cells. C2b: every prop is a
/// gfx::sprites instance showing its sprite's standing frame for its facing (no sprite slot: the frame's tiles are
/// shared with every prop showing the same frame, the palette bank with every sprite of the same colors), drawn
/// each frame like a standing object.
#include "props.h"

#include "capsule/core/assert.h"
#include "capsule/core/memory.h"
#include "capsule/core/types.h"
#include "capsule/field/field.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/video.h"
#include "capsule/map/map.h"

namespace capsule::field
{
namespace
{

/// Pixels from a cell's edge to its centre.
constexpr int kCellCentre = map::kMetatilePixels / kHalf;

/// One prop.
struct Prop
{
   bool used;                     ///< Placed (zero-initialised: free).
   uint8_t id;                    ///< The script's id.
   uint8_t instance;              ///< Its gfx::sprites instance.
   const asset::Sprite* sprite;   ///< Its sprite.
   int16_t x;                     ///< Its cell's column.
   int16_t y;                     ///< Row.
   uint8_t elevation;             ///< Its level.
   uint8_t facing;                ///< DIR_*.
};

/// The props' state (EWRAM; none of it saved).
struct State
{
   Prop props[kMaxProps];   ///< The props.
};
CAPSULE_EWRAM_BSS State g;   ///< The props.

/// A placed prop by its id.
/// @param id  the script's id
/// @return it, or nullptr
Prop* find(uint8_t id)
{
   for (Prop& p : g.props)
   {
      if (p.used && p.id == id)
      {
         return &p;
      }
   }
   return nullptr;
}

/// Frees a prop and its instance.
/// @param p  the prop
void drop(Prop& p)
{
   gfx::sprites::remove_instance(p.instance);
   p = Prop{};
}

/// Shows a prop's facing: its sprite's standing frame for it.
/// @param p  the prop
/// @return false when the heap had no room for the frame (the prop then draws nothing)
bool show_facing(const Prop& p)
{
   uint8_t frame = 0;
   uint8_t flips = 0;
   props::standing_frame(*p.sprite, p.facing, frame, flips);
   return gfx::sprites::set_instance_frame(p.instance, frame, flips);
}

} // namespace

bool place_prop(uint8_t id, const asset::Sprite& sprite, int16_t x, int16_t y, uint8_t elevation, uint8_t facing)
{
   ENGINE_ASSERT(facing < NUM_DIRECTIONS);
   if (Prop* old = find(id))
   {
      drop(*old);   // the same id again: the new prop replaces it
   }
   Prop* p = nullptr;
   for (Prop& q : g.props)
   {
      if (!q.used)
      {
         p = &q;
         break;
      }
   }
   if (p == nullptr)
   {
      return false;
   }
   const uint8_t instance = gfx::sprites::add_instance(sprite);
   if (instance == gfx::sprites::kNoInstance)
   {
      return false;
   }
   *p = Prop{true, id, instance, &sprite, x, y, elevation, static_cast<uint8_t>(facing < NUM_DIRECTIONS ? facing : 0)};
   if (!show_facing(*p))
   {
      drop(*p);   // no room for its frame
      return false;
   }
   props::draw();
   return true;
}

bool face_prop(uint8_t id, uint8_t facing)
{
   Prop* p = find(id);
   if (p == nullptr || facing >= NUM_DIRECTIONS)
   {
      return false;
   }
   p->facing = facing;
   return show_facing(*p);
}

bool remove_prop(uint8_t id)
{
   Prop* p = find(id);
   if (p == nullptr)
   {
      return false;
   }
   drop(*p);
   return true;
}

namespace props
{

void reset()
{
   for (Prop& p : g.props)
   {
      if (p.used)
      {
         drop(p);
      }
   }
   g = State{};
}

void draw()
{
   for (const Prop& p : g.props)
   {
      if (!p.used)
      {
         continue;
      }
      const asset::Sprite& sprite = *p.sprite;
      const int sw = hal::video::obj_width(sprite.shape, sprite.size);
      const int sh = hal::video::obj_height(sprite.shape, sprite.size);
      // Centred on its cell, standing on the cell's bottom edge (as a standing object)
      const int32_t sx = p.x * map::kMetatilePixels + kCellCentre - sw / kHalf - map::camera_x();
      const int32_t sy = (p.y + 1) * map::kMetatilePixels - sh - map::camera_y();
      const bool on = sx > -sw && sx < hal::video::kScreenWidth && sy > -sh && sy < hal::video::kScreenHeight;
      gfx::sprites::show_instance(p.instance, on);
      if (!on)
      {
         continue;
      }
      gfx::sprites::set_instance_position(p.instance, static_cast<int16_t>(sx), static_cast<int16_t>(sy));
      gfx::sprites::set_instance_priority(p.instance, map::elevation(p.elevation).priority);
      gfx::sprites::set_instance_order(p.instance, map::sub_priority(sy + sh, p.elevation));
   }
}

} // namespace props

} // namespace capsule::field
