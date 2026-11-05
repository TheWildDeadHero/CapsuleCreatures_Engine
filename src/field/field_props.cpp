/// @file
/// The field's props (C2, capsule/field/field.h place_prop()): sprites standing on cells. Each distinct sprite takes
/// one claimed sprite slot holding its standing frames (gfx::sprites::load_frames(), the slot itself hidden); every
/// prop is a gfx::sprites instance of it, drawn each frame like a standing object.
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

/// The most distinct prop sprites at once (each takes a sprite slot; the claimable slots bound them further).
constexpr int kMaxPropSprites = map::kMaxObjects;
/// PropSprite::slot / Prop::sprite: none.
constexpr uint8_t kNone = 0xFF;
/// Pixels from a cell's edge to its centre.
constexpr int kCellCentre = map::kMetatilePixels / kHalf;

/// One prop sprite: its slot and the standing frames it holds.
struct PropSprite
{
   const asset::Sprite* sprite;              ///< The sprite, or nullptr (free).
   uint8_t slot;                             ///< Its claimed sprite slot.
   uint8_t users;                            ///< Props showing it.
   uint8_t frames[NUM_DIRECTIONS];           ///< Each facing's frame among the slot's (load_frames() order).
   uint8_t flips[NUM_DIRECTIONS];            ///< Each facing's flips.
};

/// One prop.
struct Prop
{
   bool used;             ///< Placed (zero-initialised: free).
   uint8_t id;            ///< The script's id.
   uint8_t sprite;        ///< Its PropSprite.
   uint8_t instance;      ///< Its gfx::sprites instance.
   int16_t x;             ///< Its cell's column.
   int16_t y;             ///< Row.
   uint8_t elevation;     ///< Its level.
   uint8_t facing;        ///< DIR_*.
};

/// The props' state (EWRAM; none of it saved).
struct State
{
   PropSprite sprites[kMaxPropSprites];   ///< The prop sprites.
   Prop props[kMaxProps];                 ///< The props.
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

/// Frees a prop: its instance, and its sprite's slot when no prop shows that sprite any more.
/// @param p  the prop
void drop(Prop& p)
{
   gfx::sprites::remove_instance(p.instance);
   PropSprite& s = g.sprites[p.sprite];
   if (--s.users == 0)
   {
      gfx::sprites::unload(s.slot);
      release_sprite_slot(s.slot);
      s = PropSprite{};
   }
   p = Prop{};
}

/// The prop sprite of a sprite: the one in use, or a new one in a newly claimed slot with its standing frames.
/// @param sprite  the sprite
/// @return its index, or kNone (no slot free, or no record)
uint8_t sprite_of(const asset::Sprite& sprite)
{
   uint8_t free = kNone;
   for (uint8_t k = 0; k < kMaxPropSprites; ++k)
   {
      if (g.sprites[k].sprite == &sprite)
      {
         return k;
      }
      if (g.sprites[k].sprite == nullptr && free == kNone)
      {
         free = k;
      }
   }
   if (free == kNone)
   {
      return kNone;
   }
   const uint8_t slot = claim_sprite_slot();
   if (slot == kNoSpriteSlot)
   {
      return kNone;
   }
   // Each facing's standing frame, every distinct frame once in the slot
   PropSprite& s = g.sprites[free];
   uint8_t set[NUM_DIRECTIONS] = {};
   uint8_t count = 0;
   for (uint8_t dir = 0; dir < NUM_DIRECTIONS; ++dir)
   {
      uint8_t frame = 0;
      props::standing_frame(sprite, dir, frame, s.flips[dir]);
      uint8_t at = 0;
      while (at < count && set[at] != frame)
      {
         ++at;
      }
      if (at == count)
      {
         set[count++] = frame;
      }
      s.frames[dir] = at;
   }
   gfx::sprites::load_frames(slot, sprite, set, count);
   gfx::sprites::show(slot, false);   // the props are its instances; the slot draws nothing itself
   s.sprite = &sprite;
   s.slot = slot;
   s.users = 0;
   return free;
}

/// Shows a prop's facing.
/// @param p  the prop
void show_facing(const Prop& p)
{
   const PropSprite& s = g.sprites[p.sprite];
   gfx::sprites::set_instance_frame(p.instance, s.frames[p.facing], s.flips[p.facing]);
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
   const uint8_t k = p != nullptr ? sprite_of(sprite) : kNone;
   if (k == kNone)
   {
      return false;
   }
   PropSprite& s = g.sprites[k];
   const uint8_t instance = gfx::sprites::add_instance(s.slot);
   if (instance == gfx::sprites::kNoInstance)
   {
      if (s.users == 0)
      {
         gfx::sprites::unload(s.slot);   // claimed for this prop alone
         release_sprite_slot(s.slot);
         s = PropSprite{};
      }
      return false;
   }
   ++s.users;
   *p = Prop{true, id, k, instance, x, y, elevation, static_cast<uint8_t>(facing < NUM_DIRECTIONS ? facing : 0)};
   show_facing(*p);
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
   show_facing(*p);
   return true;
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
      const asset::Sprite& sprite = *g.sprites[p.sprite].sprite;
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
