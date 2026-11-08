/// @file
/// C7: the party statements' port of a program without battle data (capsule/script/port.h namespace party):
/// heal() has no maxima to fill and does nothing; count() counts every member of the party boxes::attach() gave
/// (no eggs without battle data); find_move() finds nothing (no move table). Weak (CAPSULE_WEAK), as the engine
/// library's defaults (src/script/script_party.cpp), which programs with the battle modules link instead.
#include "capsule/core/memory.h"
#include "capsule/monster/monster.h"
#include "capsule/script/port.h"

namespace capsule::script::party
{

CAPSULE_WEAK void heal()
{
}

CAPSULE_WEAK int32_t count(bool hatched)
{
   (void)hatched;
   const monster::Party* p = boxes::party();
   return p != nullptr ? p->count : 0;
}

CAPSULE_WEAK bool find_move(uint16_t move, int32_t& slot, int32_t& species)
{
   (void)move;
   (void)species;
   slot = monster::kLayout.party_size;
   return false;
}

} // namespace capsule::script::party
