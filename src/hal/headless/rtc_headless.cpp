/// @file
/// M6a K1: the headless real-time clock (capsule/hal/rtc.h): the time the tests set, invalid until they set one,
/// so a test run never depends on the host's clock.
#include "capsule/hal/rtc.h"

namespace capsule::hal::rtc
{

namespace
{

DateTime g_time{};      ///< The time reads give.
bool g_valid = false;   ///< Whether reads succeed.

} // namespace

bool read(DateTime& out)
{
   if (!g_valid)
   {
      return false;
   }
   out = g_time;
   return true;
}

void set_for_test(const DateTime& time, bool valid)
{
   g_time = time;
   g_valid = valid && in_range(time);
}

} // namespace capsule::hal::rtc
