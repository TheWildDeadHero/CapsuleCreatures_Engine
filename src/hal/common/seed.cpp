/// @file
/// First-press reseeding of the random streams (A-4), shared by every HAL.
#include "capsule/core/random.h"
#include "capsule/hal/system.h"

namespace capsule::hal::system
{
namespace
{

bool g_seeded_by_press = false;   ///< Whether the first press has been mixed in.

} // namespace

void seed_on_first_press(input::KeyBits held)
{
   if (g_seeded_by_press || held == 0 || !rng::press_seeding())   // U5: a game may keep its reference's seeding
   {
      return;
   }
   g_seeded_by_press = true;
   rng::seed(entropy());
}

} // namespace capsule::hal::system
