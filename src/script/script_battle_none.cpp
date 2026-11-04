/// @file
/// The battle script port of a program without battles (capsule/script/battle_port.h, D-36 package 2 C1): no battle
/// starts, so each Battle op ends at once as a victory, and no party has encounter music.
/// Its definitions are weak (CAPSULE_WEAK): a game that defines its own port keeps it.
#include "capsule/script/battle_port.h"

#include "capsule/core/memory.h"

namespace capsule::script::battles
{

CAPSULE_WEAK bool start(const Request& r)
{
   (void)r;
   return false;
}

CAPSULE_WEAK bool busy()
{
   return false;
}

CAPSULE_WEAK battle::Outcome outcome()
{
   return battle::Outcome::Victory;
}

CAPSULE_WEAK int32_t encounter_music(uint16_t party)
{
   (void)party;
   return kNoMusic;
}

} // namespace capsule::script::battles
