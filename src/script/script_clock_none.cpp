/// @file
/// M6a K1: the script Clock statements' port of a program without the engine library's clocks (the tests that build
/// the script VM themselves): every value 0, Wait refused, reset does nothing. Weak, like the engine library's
/// src/script/script_clock.cpp.
#include "capsule/core/memory.h"
#include "capsule/script/port.h"

namespace capsule::script::world
{

CAPSULE_WEAK int32_t clock_value(ClockOp kind)
{
   (void)kind;
   return 0;
}

CAPSULE_WEAK bool clock_wait(int32_t hours)
{
   (void)hours;
   return false;
}

CAPSULE_WEAK void clock_reset()
{
}

} // namespace capsule::script::world
