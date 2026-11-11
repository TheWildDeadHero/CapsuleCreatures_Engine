/// @file
/// M6a K1: the economy and world clocks, Wait and seasons (capsule/core/clock.h; C-05 in CapsuleCreatures_Design.md).
///
/// A session counts from one RTC read (a sync: begin_new(), resume(), commit()): the RTC's time then, and
/// hal::system::seconds() then. The RTC's time now is the read's plus the seconds since; the economy clock is the
/// saved economy seconds plus how far that is past the latest time the save has seen (an RTC behind it credits
/// nothing). Without a valid RTC the session's seconds are credited instead (play time).
#include "capsule/core/clock.h"

#include "capsule/hal/rtc.h"
#include "capsule/hal/system.h"

namespace capsule::clock
{

namespace
{

/// 2000-01-01 (the RTC's first day, seconds_since_2000() 0) was a Saturday: its weekday, Sunday counting as 0.
constexpr uint8_t kWeekdayOf2000 = 6;
/// The season of a world without one season count (none).
constexpr uint8_t kNoSeason = 0;

/// This session's clock.
struct Session
{
   bool synced = false;          ///< A sync ran.
   bool rtc_valid = false;       ///< The last read gave a valid time.
   uint32_t rtc_at_sync = 0;     ///< The RTC's time at the sync (seconds since 2000).
   uint32_t seconds_at_sync = 0; ///< hal::system::seconds() at the sync.
};

Saved g_saved{};       ///< What the save keeps.
Session g_session{};   ///< This session.

/// Reads the RTC into the session (its time and the seconds counter now).
void sync()
{
   hal::rtc::DateTime t{};
   g_session.rtc_valid = hal::rtc::read(t);
   g_session.rtc_at_sync = g_session.rtc_valid ? hal::rtc::seconds_since_2000(t) : 0;
   g_session.seconds_at_sync = hal::system::seconds();
   g_session.synced = true;
}

/// The session's seconds since its sync.
/// @return seconds
uint32_t elapsed()
{
   return g_session.synced ? hal::system::seconds() - g_session.seconds_at_sync : 0;
}

/// The RTC's time now (the sync's read plus the seconds since).
/// @return seconds since 2000 (only meaningful with a valid RTC)
uint32_t rtc_now()
{
   return g_session.rtc_at_sync + elapsed();
}

/// The world clock's hours now, Wait included, counted from a fixed origin whose weekday is weekday_base().
/// @return hours
uint32_t world_hours()
{
   const Settings* s = settings();
   uint32_t hours = 0;
   if (g_session.rtc_valid)
   {
      hours = rtc_now() / kSecondsPerHour;
   }
   else if (s != nullptr)
   {
      hours = s->start_weekday * uint32_t{kHoursPerDay} + s->start_hour + economy_seconds() / kSecondsPerHour;
   }
   return hours + g_saved.wait_offset_hours;
}

/// The weekday of world_hours()' origin.
/// @return 0 (Sunday) to 6
uint8_t weekday_base()
{
   return g_session.rtc_valid ? kWeekdayOf2000 : 0;
}

} // namespace

Saved& saved()
{
   return g_saved;
}

void begin_new()
{
   g_saved = Saved{};
   sync();
   g_saved.last_seen_rtc = g_session.rtc_valid ? g_session.rtc_at_sync : kNever;
}

void resume()
{
   sync();
   if (g_session.rtc_valid && g_saved.last_seen_rtc == kNever)
   {
      g_saved.last_seen_rtc = g_session.rtc_at_sync;   // the first valid read of this save: nothing to credit yet
   }
}

void commit()
{
   const uint32_t economy = economy_seconds();
   if (g_session.rtc_valid)
   {
      const uint32_t now = rtc_now();
      if (g_saved.last_seen_rtc == kNever || now > g_saved.last_seen_rtc)
      {
         g_saved.last_seen_rtc = now;   // an RTC still behind keeps the latest time seen (C-05g 1)
      }
   }
   g_saved.economy_seconds = economy;
   sync();
}

uint32_t economy_seconds()
{
   if (!g_session.synced)
   {
      return g_saved.economy_seconds;
   }
   if (!g_session.rtc_valid)
   {
      return g_saved.economy_seconds + elapsed();   // C-05g 3: play time
   }
   const uint32_t now = rtc_now();
   const uint32_t seen = g_saved.last_seen_rtc;
   return g_saved.economy_seconds + (seen != kNever && now > seen ? now - seen : 0);
}

uint32_t economy_days()
{
   return economy_seconds() / kSecondsPerDay;
}

uint8_t hour()
{
   return settings() != nullptr ? static_cast<uint8_t>(world_hours() % kHoursPerDay) : 0;
}

uint8_t weekday()
{
   if (settings() == nullptr)
   {
      return 0;
   }
   return static_cast<uint8_t>((world_hours() / kHoursPerDay + weekday_base()) % kDaysPerWeek);
}

uint8_t season()
{
   const Settings* s = settings();
   if (s == nullptr || s->season_count == 0)
   {
      return kNoSeason;
   }
   if (s->season_mode == SeasonMode::CalendarMonth && g_session.rtc_valid)
   {
      const hal::rtc::DateTime t = hal::rtc::from_seconds_since_2000(rtc_now());
      return static_cast<uint8_t>((t.month - 1u) % s->season_count);
   }
   const uint16_t length = s->season_length_days != 0 ? s->season_length_days : 1;
   return static_cast<uint8_t>((economy_days() / length) % s->season_count);
}

uint8_t period_of_hour(uint8_t hour)
{
   const Settings* s = settings();
   if (s == nullptr || s->period_count == 0)
   {
      return 0;
   }
   uint8_t period = static_cast<uint8_t>(s->period_count - 1);   // before the first start: the last period
   for (uint8_t i = 0; i < s->period_count; ++i)
   {
      if (s->period_starts[i] <= hour)
      {
         period = i;
      }
   }
   return period;
}

bool wait(uint8_t hours)
{
   const Settings* s = settings();
   if (s == nullptr || hours == 0)
   {
      return false;
   }
   const uint32_t now = economy_seconds();
   if (g_saved.last_wait != kNever && now - g_saved.last_wait < s->wait_cooldown)
   {
      return false;   // the cooldown runs on the economy clock: waiting cannot shorten it
   }
   const uint8_t add = hours < s->wait_max_hours ? hours : s->wait_max_hours;
   g_saved.wait_offset_hours = static_cast<uint8_t>((g_saved.wait_offset_hours + add) % kWaitWrapHours);
   g_saved.last_wait = now;
   return true;
}

void reset_wait()
{
   g_saved.wait_offset_hours = 0;
}

bool rtc_missing()
{
   return g_session.synced && !g_session.rtc_valid;
}

bool notice_pending()
{
   return rtc_missing() && (g_saved.flags & kNoticeShown) == 0;
}

void notice_shown()
{
   g_saved.flags = static_cast<uint8_t>(g_saved.flags | kNoticeShown);
}

uint32_t events()
{
   if (settings() == nullptr)
   {
      return 0;
   }
   const uint32_t today = economy_days();
   const uint32_t last = g_saved.last_events_day;
   g_saved.last_events_day = today;
   return last == kNever || today < last ? 0 : today - last;   // a first run, or (never) a clock run backwards: 0
}

} // namespace capsule::clock
