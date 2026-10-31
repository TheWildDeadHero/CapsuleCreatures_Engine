/// @file
/// The keys menus read each frame (see capsule/ui/keys.h).
#include "capsule/ui/keys.h"

namespace capsule::ui
{

Keys read(KeyState& s, const Repeat& timing, hal::input::KeyBits held)
{
   Keys k;
   k.held = held;
   k.pressed = static_cast<hal::input::KeyBits>(held & ~s.held);
   k.repeated = k.pressed;
   if (held != 0 && held == s.held && timing.start != 0)
   {
      // the same keys stay held: the counter runs; when it runs out every held key repeats this frame
      if (s.counter > 0)
      {
         --s.counter;
      }
      if (s.counter == 0)
      {
         k.repeated = held;
         s.counter = timing.every;
      }
   }
   else
   {
      s.counter = timing.start;   // none held, or the keys changed: the counter starts again
   }
   s.held = held;
   return k;
}

}   // namespace capsule::ui
