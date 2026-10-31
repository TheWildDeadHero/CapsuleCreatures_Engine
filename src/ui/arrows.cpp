/// @file
/// A list's scroll arrows (see capsule/ui/arrows.h).
#include "capsule/ui/arrows.h"

#include "capsule/asset/asset.h"
#include "capsule/core/trig.h"
#include "capsule/gfx/sprites.h"

namespace capsule::ui
{
namespace
{

constexpr int kSineUnit = 256;        ///< The bounce's sine unit (1.0) and the divisor of amplitude * sine.
constexpr int kQ14PerUnit = kQ14One / kSineUnit;   ///< Q14 steps per 1/256 sine unit.

/// The arrows' sprite.
/// @param a  the arrows
/// @return it, or nullptr when the style names none
const asset::Sprite* sprite_of(const Arrows& a)
{
   if (a.style == nullptr || static_cast<uint32_t>(a.style->arrow_sprite) == 0)
   {
      return nullptr;
   }
   return asset::sprite(a.style->arrow_sprite);
}

}   // namespace

int bounce(uint8_t amplitude, uint8_t phase)
{
   // the sine at 256 steps a turn, in 1/256 units cut towards zero, then scaled and cut towards zero again
   const int32_t sine = sin_q14(static_cast<uint16_t>(phase << kAngleStepShift)) / kQ14PerUnit;
   return static_cast<int>(sine * amplitude / kSineUnit);
}

void open(Arrows& a, const MenuStyle& style, uint8_t up_slot, uint8_t down_slot, int16_t x, int16_t up_y,
          int16_t down_y)
{
   a.style = &style;
   a.up_slot = up_slot;
   a.down_slot = down_slot;
   a.x = x;
   a.up_y = up_y;
   a.down_y = down_y;
   a.up_phase = 0;
   a.down_phase = 0;
   a.started = false;
   const asset::Sprite* s = sprite_of(a);
   a.open = s != nullptr;
   if (!a.open)
   {
      return;
   }
   gfx::sprites::load(up_slot, *s);
   gfx::sprites::load(down_slot, *s);
   gfx::sprites::show(up_slot, false);
   gfx::sprites::show(down_slot, false);
   gfx::sprites::set_position(up_slot, x, up_y);
   gfx::sprites::set_position(down_slot, x, down_y);
}

void update(Arrows& a, uint8_t scroll, uint8_t last_scroll)
{
   if (!a.open)
   {
      return;
   }
   if (!a.started)
   {
      // the first frame starts the animations; the arrows stay hidden and unmoved until the next
      gfx::sprites::play(a.up_slot, a.style->arrow_up);
      gfx::sprites::play(a.down_slot, a.style->arrow_down);
      a.started = true;
      return;
   }
   gfx::sprites::show(a.up_slot, scroll != 0);
   gfx::sprites::show(a.down_slot, scroll != last_scroll);
   gfx::sprites::set_position(a.up_slot, a.x, static_cast<int16_t>(a.up_y + bounce(a.style->arrow_amplitude,
                                                                                    a.up_phase)));
   gfx::sprites::set_position(a.down_slot, a.x, static_cast<int16_t>(a.down_y + bounce(a.style->arrow_amplitude,
                                                                                        a.down_phase)));
   a.up_phase = static_cast<uint8_t>(a.up_phase + a.style->arrow_speed);
   a.down_phase = static_cast<uint8_t>(a.down_phase - a.style->arrow_speed);   // the down arrow runs backwards
}

void close(Arrows& a)
{
   if (a.open)
   {
      gfx::sprites::show(a.up_slot, false);
      gfx::sprites::show(a.down_slot, false);
   }
   a.open = false;
}

}   // namespace capsule::ui
