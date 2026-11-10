/// @file
/// M6a K1: the script Clock statements' port (capsule/script/port.h world::clock_value(), clock_wait(),
/// clock_reset()) over the engine's clocks (capsule/core/clock.h). Weak: a game may answer from its own clock. A
/// program without the engine library links src/script/script_clock_none.cpp instead.
#include "capsule/core/clock.h"
#include "capsule/core/memory.h"
#include "capsule/script/port.h"

namespace capsule::script::world
{

namespace
{

constexpr int32_t kMaxWaitHours = clock::kWaitWrapHours - 1;   ///< The most hours a Wait can ask for.

} // namespace

CAPSULE_WEAK int32_t clock_value(ClockOp kind)
{
   switch (kind)
   {
   case ClockOp::Hour:
      return clock::hour();
   case ClockOp::Weekday:
      return clock::weekday();
   case ClockOp::Season:
      return clock::season();
   case ClockOp::Days:
      return static_cast<int32_t>(clock::economy_days());
   default:
      return 0;
   }
}

CAPSULE_WEAK bool clock_wait(int32_t hours)
{
   if (hours <= 0)
   {
      return false;
   }
   return clock::wait(static_cast<uint8_t>(hours < kMaxWaitHours ? hours : kMaxWaitHours));
}

CAPSULE_WEAK void clock_reset()
{
   clock::reset_wait();
}

} // namespace capsule::script::world
