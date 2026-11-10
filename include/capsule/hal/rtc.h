/// @file
/// M6a K1 (C-05a): the real-time clock, read only when the game's clock asks (at power-on, at a save, after a
/// wake), never polled. One implementation per platform, chosen at link time (P-8):
///   - GBA (src/hal/gba/rtc_gba.cpp): the cartridge's S-3511A over the GPIO port (0x080000C4 data, C6 direction,
///     C8 read enable), in 24-hour mode; a chip that answers nothing, reports a power loss (its battery ran out) or
///     gives digits out of range reads as invalid;
///   - desktop and Dreamcast (src/hal/common/rtc_host.cpp): the host's local time;
///   - headless (src/hal/headless/rtc_headless.cpp): a time the tests set (set_for_test()), invalid until set.
#pragma once

#include <cstdint>

namespace capsule::hal::rtc
{

/// A date and time as the clock keeps them (no time zone; the world clock's hour is this hour).
struct DateTime
{
   uint16_t year = 0;     ///< 2000 to 2099.
   uint8_t month = 0;     ///< 1 to 12.
   uint8_t day = 0;       ///< 1 to the month's days.
   uint8_t hour = 0;      ///< 0 to 23.
   uint8_t minute = 0;    ///< 0 to 59.
   uint8_t second = 0;    ///< 0 to 59.
};

/// @name The range a clock may give (the S-3511A keeps two-digit years from 2000)
///@{
inline constexpr uint16_t kFirstYear = 2000;   ///< The earliest year.
inline constexpr uint16_t kLastYear = 2099;    ///< The latest year.
inline constexpr uint8_t kMonths = 12;         ///< Months in a year.
inline constexpr uint8_t kHours = 24;          ///< Hours in a day.
inline constexpr uint8_t kMinutes = 60;        ///< Minutes in an hour (and seconds in a minute).
///@}

/// Reads the clock.
/// @param out  receives the date and time when valid
/// @return false when there is no clock, it lost its power or it gave a date out of range (out is then unchanged)
bool read(DateTime& out);

/// Headless only: the time the next reads give (the tests' clock).
/// @param time   the date and time
/// @param valid  false: reads fail (no clock)
void set_for_test(const DateTime& time, bool valid);

/// Whether a date and time lie in the range above (each field, the day within its month).
/// @param t  the date and time
/// @return true when they do
bool in_range(const DateTime& t);

/// Seconds from 2000-01-01 00:00:00 to a date and time (no leap seconds).
/// @param t  a date and time in range
/// @return the seconds
uint32_t seconds_since_2000(const DateTime& t);

/// The date and time some seconds after 2000-01-01 00:00:00 (seconds_since_2000()'s inverse).
/// @param seconds  the seconds
/// @return the date and time
DateTime from_seconds_since_2000(uint32_t seconds);

} // namespace capsule::hal::rtc
