/// @file
/// The default minute clock (capsule/core/clock.h): it never moves, so timed field effects counted in minutes only
/// run down once a game defines clock::minutes() in its own file (which keeps this one out of the link).
#include "capsule/core/clock.h"

namespace capsule::clock
{

uint32_t minutes()
{
   return 0;
}

} // namespace capsule::clock
