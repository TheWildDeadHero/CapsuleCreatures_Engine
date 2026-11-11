/// @file
/// The game's clocks.
///
/// D-26 decision 3 (package G2) and F1 (D-27 section 2): time_of_day() and minutes() are link-time ports a game may
/// define (the engine's defaults: src/core/clock_default.cpp answers 0, src/core/clock_minutes_default.cpp never
/// moves). The script op `TimeOfDay`, the growth context `time_of_day` and the timed field effects read them.
///
/// M6a K1 (C-05, CapsuleCreatures_Design.md) adds the clocks themselves (src/core/clock.cpp), for a world that
/// declares `clock` in world.json (settings(): its data, nullptr without):
///   - the **economy clock** (economy_seconds()): seconds of real time since the save began. It is the real-time
///     clock's (capsule/hal/rtc.h) advance past the latest time the save has seen, read at begin_new(), resume() and
///     commit() and counted by hal::system::seconds() between reads. An RTC behind that time credits nothing until it
///     catches up (C-05g 1); a large jump forward counts (2); without a valid RTC the session's seconds count instead
///     (3: play time; rtc_missing(), and notice_pending() once per save);
///   - the **world clock** (hour(), weekday()): the RTC's time (without one: the start the world declares plus the
///     economy clock) plus the Wait offset, 0 to 167 hours; weekday 0 is Sunday;
///   - **Wait** (wait(), reset_wait()): hours added to the offset, at most the world's max_hours_per_use a use, refused
///     until its cooldown has passed on the economy clock;
///   - **seasons** (season()): fixed_days (economy days / length, modulo the count) or calendar_month (the RTC's
///     month, modulo the count; fixed_days without an RTC).
/// A world that declares `clock` also gets time_of_day() as the period of hour() and minutes() as economy minutes
/// (the generated world defines them weak, so a game's own definitions win).
///
/// The save keeps Saved (capsule::save Section::Clock, present when the world declares a clock).
#pragma once

#include <cstdint>

namespace capsule::clock
{

/// The time of day now, as the game counts it.
/// @return the game's period index (default: 0)
uint8_t time_of_day();

/// The game's clock in minutes: any monotonic count (it may wrap at 2^32; differences are what count).
/// @return minutes (default: always 0)
uint32_t minutes();

/// @name M6a K1: limits of the world's clock data
///@{
inline constexpr uint8_t kHoursPerDay = 24;            ///< Hours in a day.
inline constexpr uint8_t kDaysPerWeek = 7;             ///< Days in a week.
inline constexpr uint8_t kWaitWrapHours = kHoursPerDay * kDaysPerWeek;   ///< The Wait offset wraps at a week.
inline constexpr uint32_t kSecondsPerHour = 3600;      ///< Seconds in an hour.
inline constexpr uint32_t kSecondsPerDay = kSecondsPerHour * kHoursPerDay;   ///< Seconds in a day.
inline constexpr uint8_t kMaxPeriods = 8;              ///< Periods a day may have.
inline constexpr uint8_t kMaxSeasons = 12;             ///< Seasons (a 4-bit index, C-05e).
inline constexpr uint32_t kNever = 0xFFFFFFFFu;        ///< Saved::last_seen_rtc / last_wait: none yet.
///@}

/// How seasons are counted.
enum class SeasonMode : uint8_t
{
   FixedDays,       ///< (economy days / length_days) % count.
   CalendarMonth,   ///< (the RTC's month - 1) % count (FixedDays without an RTC).
   NUM_MODES        ///< Number of modes.
};

/// The world's clock data (world.json `clock`, compiled by tools/map_compiler.py).
struct Settings
{
   const uint8_t* period_starts;   ///< period_count start hours, ascending (period i from period_starts[i]).
   uint8_t period_count;           ///< 1 to kMaxPeriods (one period: always 0).
   uint8_t start_hour;             ///< The world clock without an RTC starts at this hour ...
   uint8_t start_weekday;          ///< ... of this weekday (0: Sunday).
   uint8_t wait_max_hours;         ///< Hours one Wait adds at most (1 to kWaitWrapHours - 1).
   uint32_t wait_cooldown;         ///< Economy seconds between Waits.
   uint8_t season_count;           ///< 1 to kMaxSeasons.
   SeasonMode season_mode;         ///< How seasons are counted.
   uint16_t season_length_days;    ///< FixedDays: a season's days (1 up).
};

/// What the save keeps (capsule::save Section::Clock).
struct Saved
{
   uint32_t economy_seconds = 0;          ///< The economy clock at the last commit().
   uint32_t last_seen_rtc = kNever;       ///< The latest RTC time credited (seconds since 2000), or kNever.
   uint32_t last_wait = kNever;           ///< The economy clock at the last Wait, or kNever.
   uint8_t wait_offset_hours = 0;         ///< The Wait offset (0 to kWaitWrapHours - 1).
   uint8_t flags = 0;                     ///< kNoticeShown.
   uint32_t last_events_day = kNever;     ///< M6a K4: the economy day events() last ran, or kNever.
};

/// Saved::flags: the missing-RTC notice was shown for this save.
inline constexpr uint8_t kNoticeShown = 1u << 0;

/// The world's clock data (link time: the generated world defines it; the engine's default, in src/save/save.cpp,
/// answers nullptr).
/// @return it, or nullptr when the world declares no clock
const Settings* settings();

/// The state the save keeps (the save's Sources::clock points here).
/// @return it
Saved& saved();

/// A new game: the save's clock starts now (the economy clock at 0, the RTC's time as the latest seen, no Wait).
void begin_new();

/// After a save was loaded (or at power-on): reads the RTC and counts from it.
void resume();

/// Before a save is written: folds this session's time into saved() (and counts on from it).
void commit();

/// Seconds of the economy clock (saved().economy_seconds before begin_new() / resume()).
/// @return seconds
uint32_t economy_seconds();

/// Whole days of the economy clock.
/// @return days
uint32_t economy_days();

/// The world clock's hour (0 without a clock).
/// @return 0 to 23
uint8_t hour();

/// The world clock's weekday (0 without a clock).
/// @return 0 (Sunday) to 6
uint8_t weekday();

/// The season (0 without a clock).
/// @return 0 to the world's season count - 1
uint8_t season();

/// The period an hour falls in (the last period before the first start: the night that runs past midnight).
/// @param hour  0 to 23
/// @return the period (0 without a clock)
uint8_t period_of_hour(uint8_t hour);

/// Wait: moves the world clock on.
/// @param hours  hours to add (capped at the world's wait_max_hours)
/// @return false (nothing changes) without a clock, for 0 hours or while the cooldown runs
bool wait(uint8_t hours);

/// Sets the world clock back to the RTC's time (the Wait offset to 0).
void reset_wait();

/// Whether this session has no valid RTC (the economy clock counts play time).
/// @return true without one
bool rtc_missing();

/// Whether the game should tell the player the RTC is missing (once per save: notice_shown() clears it).
/// @return true when it should
bool notice_pending();

/// The game told the player (saved with the save).
void notice_shown();

/// M6a K4: the time events' day (the script's `clock events`): the economy days passed since it last ran, and today
/// recorded. The first run of a save answers 0 (nothing to catch up on); an economy clock never runs backwards.
/// @return days passed (0: the same day, a first run, or no clock)
uint32_t events();

} // namespace capsule::clock
