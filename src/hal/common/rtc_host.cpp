/// @file
/// M6a K1: the real-time clock on desktop and Dreamcast (capsule/hal/rtc.h): the host's local time.
#include "capsule/hal/rtc.h"

#include <ctime>

namespace capsule::hal::rtc
{

namespace
{

constexpr int kTmYearBase = 1900;   ///< std::tm counts years from 1900 ...
constexpr int kTmMonthBase = 1;     ///< ... and months from 0.

} // namespace

bool read(DateTime& out)
{
   const std::time_t now = std::time(nullptr);
   std::tm local{};
#if defined(_WIN32)
   if (localtime_s(&local, &now) != 0)
   {
      return false;
   }
#else
   if (localtime_r(&now, &local) == nullptr)
   {
      return false;
   }
#endif
   DateTime t{};
   t.year = static_cast<uint16_t>(local.tm_year + kTmYearBase);
   t.month = static_cast<uint8_t>(local.tm_mon + kTmMonthBase);
   t.day = static_cast<uint8_t>(local.tm_mday);
   t.hour = static_cast<uint8_t>(local.tm_hour);
   t.minute = static_cast<uint8_t>(local.tm_min);
   t.second = static_cast<uint8_t>(local.tm_sec < kMinutes ? local.tm_sec : kMinutes - 1);   // a leap second
   if (!in_range(t))
   {
      return false;   // a host clock outside 2000-2099
   }
   out = t;
   return true;
}

void set_for_test(const DateTime&, bool)
{
}

} // namespace capsule::hal::rtc
