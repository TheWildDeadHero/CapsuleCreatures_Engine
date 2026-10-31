/// @file
/// Timed field effects of a program with the battle modules but without the engine library or a script bank (the
/// tests and the bench: capsule_battle_library()): there are none, so effect logic's `field_effect start` refuses and
/// its reads say "not running" (see capsule/script/field_effects.h).
#include "capsule/script/field_effects.h"

namespace capsule::script::field_effects
{

uint8_t count()
{
   return 0;
}

const FieldEffectInfo* info(uint8_t effect)
{
   (void)effect;
   return nullptr;
}

bool start(uint8_t effect, int32_t amount)
{
   (void)effect;
   (void)amount;
   return false;
}

bool active(uint8_t effect)
{
   (void)effect;
   return false;
}

int32_t remaining(uint8_t effect)
{
   (void)effect;
   return 0;
}

void stop(uint8_t effect)
{
   (void)effect;
}

void walked(uint32_t steps)
{
   (void)steps;
}

void tick()
{
}

void reset_clock()
{
}

bool take_expired(uint8_t* effect)
{
   (void)effect;
   return false;
}

} // namespace capsule::script::field_effects
