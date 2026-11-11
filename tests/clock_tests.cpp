/// @file
/// M6a K1: the clocks (capsule/core/clock.h; C-05) on the engine library's headless HAL, whose real-time clock the
/// test sets (capsule/hal/rtc.h set_for_test()) and whose seconds count the frames update() ends:
///   - without an RTC: play time, the notice once, the world clock from the declared start;
///   - with one: the economy clock across reads, an RTC moved backwards (nothing credited until it catches up), a
///     jump forward;
///   - the world clock's hour and weekday, the periods, Wait (its cap, cooldown and wrap) and reset;
///   - both season modes;
///   - the save's Clock section: a round trip, an image without it, a refused offset; M6a K4: a version-1 section;
///   - M6a K4: the time events' days;
///   - the calendar arithmetic.
#include "capsule/core/clock.h"
#include "capsule/hal/rtc.h"
#include "capsule/hal/save.h"
#include "capsule/hal/system.h"
#include "capsule/monster/monster.h"
#include "capsule/save/save.h"

#include <cstdint>
#include <cstdio>

namespace
{

namespace clock = capsule::clock;
namespace rtc = capsule::hal::rtc;
namespace save = capsule::save;

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.

/// Records one check.
/// @param cond  condition that must hold
#define CHECK(cond)                                                                                                    \
   do                                                                                                                  \
   {                                                                                                                   \
      ++g_checks;                                                                                                      \
      if (!(cond))                                                                                                     \
      {                                                                                                                \
         ++g_failures;                                                                                                 \
         std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)

constexpr uint8_t kPeriods[] = {4, 10, 20};   ///< Morning from 4, day from 10, night from 20.
constexpr uint8_t kStartHour = 10;            ///< The world clock's start without an RTC ...
constexpr uint8_t kStartWeekday = 2;          ///< ... on a Tuesday.
constexpr uint8_t kMaxWait = 23;              ///< Hours a Wait adds at most.
constexpr uint32_t kCooldown = 1800;          ///< 30 minutes between Waits.
constexpr uint8_t kSeasons = 4;               ///< Four seasons ...
constexpr uint16_t kSeasonDays = 7;           ///< ... of a week each.
constexpr uint32_t kHour = clock::kSecondsPerHour;   ///< An hour's seconds.
constexpr uint32_t kDay = clock::kSecondsPerDay;     ///< A day's seconds.
constexpr int kFramesForTwoSeconds = 120;     ///< 120 GBA frames: 2.009 s (two whole seconds).

clock::Settings g_settings = {kPeriods, sizeof(kPeriods), kStartHour, kStartWeekday, kMaxWait, kCooldown, kSeasons,
                              clock::SeasonMode::FixedDays, kSeasonDays};
bool g_has_clock = true;   ///< settings() answers g_settings (else nullptr: a world without a clock).

/// Sets the RTC to some seconds after 2000.
/// @param seconds  the seconds
void set_rtc(uint32_t seconds)
{
   rtc::set_for_test(rtc::from_seconds_since_2000(seconds), true);
}

/// A date's seconds after 2000.
/// @param y   year
/// @param mo  month
/// @param d   day
/// @param h   hour
/// @param mi  minute
/// @return the seconds
uint32_t at(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi)
{
   rtc::DateTime t{};
   t.year = y;
   t.month = mo;
   t.day = d;
   t.hour = h;
   t.minute = mi;
   return rtc::seconds_since_2000(t);
}

/// Runs frames (the headless seconds count them).
/// @param n  frames
void frames(int n)
{
   for (int i = 0; i < n; ++i)
   {
      capsule::hal::system::update();
   }
}

/// The calendar: round trips, a leap day, the range check.
void test_calendar()
{
   CHECK(at(2000, 1, 1, 0, 0) == 0);
   CHECK(at(2000, 3, 1, 0, 0) == (31 + 29) * kDay);   // 2000 is a leap year
   CHECK(at(2001, 1, 1, 0, 0) == 366 * kDay);
   const uint32_t s = at(2024, 2, 29, 13, 45) + 59;
   const rtc::DateTime t = rtc::from_seconds_since_2000(s);
   CHECK(t.year == 2024 && t.month == 2 && t.day == 29 && t.hour == 13 && t.minute == 45 && t.second == 59);
   CHECK(rtc::seconds_since_2000(t) == s);
   rtc::DateTime bad = t;
   bad.day = 30;   // February 30th
   CHECK(!rtc::in_range(bad));
   bad = t;
   bad.year = 2100;
   CHECK(!rtc::in_range(bad));
}

/// No RTC: play time, the notice once, the world clock from the start.
void test_no_rtc()
{
   rtc::set_for_test(rtc::DateTime{}, false);
   clock::begin_new();
   CHECK(clock::rtc_missing() && clock::notice_pending());
   CHECK(clock::economy_seconds() == 0 && clock::hour() == kStartHour && clock::weekday() == kStartWeekday);
   frames(kFramesForTwoSeconds);
   const uint32_t played = clock::economy_seconds();
   CHECK(played >= 2 && played <= 3);   // 2.009 s: two or three whole-second steps, by where the count stood
   clock::notice_shown();
   CHECK(!clock::notice_pending());
   clock::commit();
   CHECK(clock::saved().economy_seconds == played && clock::saved().last_seen_rtc == clock::kNever);
}

/// With an RTC: the economy clock across reads, backwards and forwards; the world clock; the periods.
void test_rtc()
{
   const uint32_t start = at(2024, 3, 10, 8, 30);   // a Sunday
   set_rtc(start);
   clock::begin_new();
   CHECK(!clock::rtc_missing() && !clock::notice_pending());
   CHECK(clock::economy_seconds() == 0 && clock::hour() == 8 && clock::weekday() == 0);
   CHECK(clock::period_of_hour(8) == 0 && clock::period_of_hour(12) == 1 && clock::period_of_hour(21) == 2);
   CHECK(clock::period_of_hour(2) == 2);   // before the first start: the night past midnight
   // a power-on two days and three hours later
   set_rtc(start + 2 * kDay + 3 * kHour);
   clock::resume();
   CHECK(clock::economy_seconds() == 2 * kDay + 3 * kHour);
   CHECK(clock::hour() == 11 && clock::weekday() == 2 && clock::economy_days() == 2);
   clock::commit();
   // the RTC set back a day: nothing credited, the world clock follows the RTC
   set_rtc(start + kDay);
   clock::resume();
   CHECK(clock::economy_seconds() == 2 * kDay + 3 * kHour);
   clock::commit();
   CHECK(clock::saved().last_seen_rtc == start + 2 * kDay + 3 * kHour);   // the latest time seen stays
   // it catches up and goes an hour past
   set_rtc(start + 2 * kDay + 4 * kHour);
   clock::resume();
   CHECK(clock::economy_seconds() == 2 * kDay + 4 * kHour);
   // seconds between reads count
   frames(kFramesForTwoSeconds);
   const uint32_t later = clock::economy_seconds() - (2 * kDay + 4 * kHour);
   CHECK(later >= 2 && later <= 3);
}

/// Wait: the cap, the cooldown on the economy clock, the week's wrap, reset.
void test_wait()
{
   const uint32_t start = at(2024, 3, 10, 8, 0);   // Sunday 08:00
   set_rtc(start);
   clock::begin_new();
   CHECK(!clock::wait(0));
   CHECK(clock::wait(5) && clock::hour() == 13);
   CHECK(!clock::wait(1) && clock::hour() == 13);   // the cooldown runs
   set_rtc(start + kCooldown - 1);
   clock::resume();
   CHECK(!clock::wait(1));
   set_rtc(start + kCooldown);
   clock::resume();
   CHECK(clock::wait(30));   // capped at kMaxWait
   CHECK(clock::saved().wait_offset_hours == 5 + kMaxWait);
   CHECK(clock::hour() == (8 + 5 + kMaxWait) % clock::kHoursPerDay && clock::weekday() == 1);
   clock::saved().wait_offset_hours = clock::kWaitWrapHours - 2;
   set_rtc(start + 2 * kCooldown);
   clock::resume();
   CHECK(clock::wait(4) && clock::saved().wait_offset_hours == 2);   // wraps at a week
   clock::reset_wait();
   CHECK(clock::saved().wait_offset_hours == 0 && clock::hour() == 9);   // the RTC's hour (an hour later now)
}

/// Seasons: fixed days on the economy clock, the calendar's month; without an RTC calendar_month counts days.
void test_seasons()
{
   const uint32_t start = at(2024, 3, 10, 8, 0);
   set_rtc(start);
   clock::begin_new();
   CHECK(clock::season() == 0);
   set_rtc(start + 8 * kDay);
   clock::resume();
   CHECK(clock::season() == 1);
   set_rtc(start + kSeasons * kSeasonDays * kDay);
   clock::resume();
   CHECK(clock::season() == 0);   // the year of seasons wraps
   g_settings.season_mode = clock::SeasonMode::CalendarMonth;
   CHECK(clock::season() == (4 - 1) % kSeasons);   // 28 days after March 10th: April 7th
   g_settings.season_mode = clock::SeasonMode::FixedDays;
}

/// M6a K4: the time events' days: a first run and the same day answer 0, days later their count, Wait nothing.
void test_events()
{
   const uint32_t start = at(2024, 3, 10, 8, 0);
   set_rtc(start);
   clock::begin_new();
   CHECK(clock::saved().last_events_day == clock::kNever);
   CHECK(clock::events() == 0 && clock::saved().last_events_day == 0);   // a first run: nothing to catch up on
   CHECK(clock::events() == 0);
   CHECK(clock::wait(kMaxWait) && clock::events() == 0);                 // Wait moves no economy day
   set_rtc(start + 2 * kDay + kHour);
   clock::resume();
   CHECK(clock::events() == 2 && clock::events() == 0);
   set_rtc(start + kDay);                                                // the RTC set back: no day passes
   clock::resume();
   CHECK(clock::events() == 0);
   g_has_clock = false;
   CHECK(clock::events() == 0);
   g_has_clock = true;
}

/// Writes an image whose Clock section is version 1's (M6a K1's: without the last events day): a fresh medium's one
/// copy, then that section's last u32 dropped (the sections after it move up), its version, size and checksum and
/// the header's bytes and checksum made again.
/// @param src  the save's sources
void make_version_one(const save::Sources& src)
{
   constexpr uint32_t kDropped = save::kClockBytes - save::kClockBytesNoEvents;   // the last events day
   constexpr uint32_t kCountAt = 6;       // the header's section count
   constexpr uint32_t kBytesAt = 12;      // its bytes after the header
   constexpr uint32_t kSumAt = 16;        // its checksum
   constexpr uint32_t kU32 = 4;           // a u32's bytes
   constexpr uint32_t kVersionAt = 1;     // a table entry's version
   constexpr uint32_t kSizeAt = 4;        // its size
   constexpr uint32_t kEntrySumAt = 8;    // its checksum
   constexpr uint8_t kVersionOne = 1;     // the Clock section's first version
   constexpr uint32_t kByteBits = 8;      // bits a byte
   constexpr uint32_t kRoom = 1024;       // more than this test's image
   {
      const capsule::hal::save::Session session(true);
      CHECK(session.ok && capsule::hal::save::erase(0, capsule::hal::save::capacity()));
   }
   CHECK(save::write(src) == save::Status::Ok);   // the first copy of an empty medium
   const capsule::hal::save::Session session(true);
   uint8_t b[kRoom] = {};
   CHECK(session.ok && capsule::hal::save::read(0, b, kRoom));
   const auto get = [&b, kByteBits](uint32_t at)
   {
      uint32_t v = 0;
      for (uint32_t k = 0; k < kU32; ++k)
      {
         v |= static_cast<uint32_t>(b[at + k]) << (k * kByteBits);
      }
      return v;
   };
   const auto put = [&b, kByteBits](uint32_t at, uint32_t v)
   {
      for (uint32_t k = 0; k < kU32; ++k)
      {
         b[at + k] = static_cast<uint8_t>(v >> (k * kByteBits));
      }
   };
   const auto hash = [&b](uint32_t from, uint32_t to, uint32_t skip)
   {
      uint32_t h = capsule::monster::kFnvOffsetBasis;
      for (uint32_t i = from; i < to; ++i)
      {
         if (i < skip || i >= skip + kU32)
         {
            h = (h ^ b[i]) * capsule::monster::kFnvPrime;   // FNV-1a 32, as the save's checksums
         }
      }
      return h;
   };
   const uint32_t count = b[kCountAt];
   const uint32_t end = save::kHeaderBytes + get(kBytesAt);
   uint32_t at = save::kHeaderBytes + count * save::kSectionEntryBytes;   // the first section
   for (uint32_t e = 0; e < count; ++e)
   {
      const uint32_t entry = save::kHeaderBytes + e * save::kSectionEntryBytes;
      const uint32_t size = get(entry + kSizeAt);
      if (b[entry] == static_cast<uint8_t>(save::Section::Clock))
      {
         for (uint32_t k = at + save::kClockBytesNoEvents; k + kDropped < end; ++k)
         {
            b[k] = b[k + kDropped];   // the sections after it move up
         }
         b[entry + kVersionAt] = kVersionOne;
         put(entry + kSizeAt, save::kClockBytesNoEvents);
         put(entry + kEntrySumAt, hash(at, at + save::kClockBytesNoEvents, end));
         put(kBytesAt, get(kBytesAt) - kDropped);
         at += save::kClockBytesNoEvents;
         continue;
      }
      at += size;
   }
   put(kSumAt, hash(0, save::kHeaderBytes + count * save::kSectionEntryBytes, kSumAt));
   CHECK(capsule::hal::save::erase(0, capsule::hal::save::capacity()));
   CHECK(capsule::hal::save::program(0, b, end - kDropped));
}

/// The save's Clock section.
void test_save()
{
   CHECK(save::section_size(save::Section::Clock) == save::kClockBytes);
   set_rtc(at(2024, 3, 10, 8, 0));
   clock::begin_new();
   clock::wait(3);
   clock::notice_shown();
   const clock::Saved kept = clock::saved();
   save::Sources s{nullptr, nullptr, nullptr, nullptr};
   s.clock = &clock::saved();
   CHECK(save::write(s) == save::Status::Ok);
   clock::saved() = clock::Saved{};
   CHECK(save::read(s) == save::Status::Ok);
   const clock::Saved& back = clock::saved();
   CHECK(back.economy_seconds == kept.economy_seconds && back.last_seen_rtc == kept.last_seen_rtc &&
         back.last_wait == kept.last_wait && back.wait_offset_hours == kept.wait_offset_hours &&
         back.flags == kept.flags);
   // an offset past a week does not load (nothing changes)
   clock::saved().wait_offset_hours = clock::kWaitWrapHours;
   CHECK(save::write(s) == save::Status::Ok);
   clock::saved() = kept;
   CHECK(save::read(s) == save::Status::OtherShape && clock::saved().wait_offset_hours == kept.wait_offset_hours);
   // M6a K4: the last events day round trips; a version-1 section loads without one
   clock::saved() = kept;
   clock::saved().last_events_day = 3;
   CHECK(save::write(s) == save::Status::Ok);
   clock::saved().last_events_day = 0;
   CHECK(save::read(s) == save::Status::Ok && clock::saved().last_events_day == 3);
   make_version_one(s);
   CHECK(save::read(s) == save::Status::Ok && clock::saved().last_events_day == clock::kNever &&
         clock::saved().economy_seconds == kept.economy_seconds && clock::saved().flags == kept.flags);
   // a world without a clock: no section; a later build with one loads Saved{}
   g_has_clock = false;
   CHECK(save::section_size(save::Section::Clock) == 0);
   CHECK(save::write(s) == save::Status::Ok);
   g_has_clock = true;
   CHECK(save::read(s) == save::Status::Ok && clock::saved().economy_seconds == 0 &&
         clock::saved().last_seen_rtc == clock::kNever && clock::saved().wait_offset_hours == 0);
}

}   // namespace

/// The test world's clock.
/// @return g_settings, or nullptr while g_has_clock is false
const capsule::clock::Settings* capsule::clock::settings()
{
   return g_has_clock ? &g_settings : nullptr;
}

int main()
{
   capsule::hal::system::init();
   test_calendar();
   test_no_rtc();
   test_rtc();
   test_wait();
   test_seasons();
   test_events();
   test_save();
   std::printf("clock tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
