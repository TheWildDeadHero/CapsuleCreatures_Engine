/// @file
/// The default context values (capsule/growth/growth.h growth::context_value()): what the game set last
/// (set_context_value(), B1: a moon phase and the like), else each one's manifest default. A game defines
/// growth::context_value() in its own file instead (link time, like clock::time_of_day()), which keeps this one out
/// of the link.
#include "capsule/growth/growth.h"

namespace capsule::growth
{

int32_t context_value(uint8_t index)
{
   int32_t set = 0;
   if (stored_context_value(index, set))
   {
      return set;
   }
   return index < kGrowth.context_count ? kGrowth.contexts[index].initial : 0;
}

} // namespace capsule::growth
