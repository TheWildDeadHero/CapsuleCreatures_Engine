/// @file
/// The default time of day (capsule/core/clock.h): always the first period. A game defines clock::time_of_day()
/// in its own file instead, which keeps this one out of the link.
#include "capsule/core/clock.h"

namespace capsule::clock
{

uint8_t time_of_day()
{
   return 0;
}

} // namespace capsule::clock
