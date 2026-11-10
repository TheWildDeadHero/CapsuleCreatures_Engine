/// @file
/// M6a K1: the calendar arithmetic of capsule/hal/rtc.h (every platform): the range check and the conversion
/// between a date and time and the seconds since 2000-01-01, Gregorian (2000 to 2099: every fourth year is a leap
/// year, 2000 included).
#include "capsule/hal/rtc.h"

namespace capsule::hal::rtc
{

namespace
{

constexpr uint32_t kSecondsPerMinute = kMinutes;                       ///< Seconds in a minute.
constexpr uint32_t kSecondsPerHour = kSecondsPerMinute * kMinutes;     ///< Seconds in an hour.
constexpr uint32_t kSecondsPerDay = kSecondsPerHour * kHours;          ///< Seconds in a day.
constexpr uint16_t kDaysPerYear = 365;                                 ///< Days in a common year.
constexpr uint16_t kLeapEvery = 4;                                     ///< Every fourth year is a leap year here.
constexpr uint8_t kFebruary = 2;                                       ///< February's month number.
constexpr uint8_t kLeapFebruaryDays = 29;                              ///< February's days in a leap year.
/// Days of each month in a common year (January first).
constexpr uint8_t kMonthDays[kMonths] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

/// Whether a year in range is a leap year.
/// @param year  the year
/// @return true for a leap year
constexpr bool leap(uint16_t year)
{
   return (year - kFirstYear) % kLeapEvery == 0;
}

/// Days in a month.
/// @param year   the year
/// @param month  the month (1 to 12)
/// @return its days
constexpr uint8_t month_days(uint16_t year, uint8_t month)
{
   return month == kFebruary && leap(year) ? kLeapFebruaryDays : kMonthDays[month - 1];
}

/// Days in a year.
/// @param year  the year
/// @return its days
constexpr uint16_t year_days(uint16_t year)
{
   return leap(year) ? kDaysPerYear + 1 : kDaysPerYear;
}

} // namespace

bool in_range(const DateTime& t)
{
   if (t.year < kFirstYear || t.year > kLastYear || t.month < 1 || t.month > kMonths)
   {
      return false;
   }
   return t.day >= 1 && t.day <= month_days(t.year, t.month) && t.hour < kHours && t.minute < kMinutes &&
          t.second < kMinutes;
}

uint32_t seconds_since_2000(const DateTime& t)
{
   uint32_t days = 0;
   for (uint16_t y = kFirstYear; y < t.year; ++y)
   {
      days += year_days(y);
   }
   for (uint8_t m = 1; m < t.month; ++m)
   {
      days += month_days(t.year, m);
   }
   days += t.day - 1u;
   return days * kSecondsPerDay + t.hour * kSecondsPerHour + t.minute * kSecondsPerMinute + t.second;
}

DateTime from_seconds_since_2000(uint32_t seconds)
{
   DateTime t{};
   uint32_t days = seconds / kSecondsPerDay;
   uint32_t rest = seconds % kSecondsPerDay;
   t.year = kFirstYear;
   while (days >= year_days(t.year))
   {
      days -= year_days(t.year);
      ++t.year;
   }
   t.month = 1;
   while (days >= month_days(t.year, t.month))
   {
      days -= month_days(t.year, t.month);
      ++t.month;
   }
   t.day = static_cast<uint8_t>(days + 1);
   t.hour = static_cast<uint8_t>(rest / kSecondsPerHour);
   rest %= kSecondsPerHour;
   t.minute = static_cast<uint8_t>(rest / kSecondsPerMinute);
   t.second = static_cast<uint8_t>(rest % kSecondsPerMinute);
   return t;
}

} // namespace capsule::hal::rtc
