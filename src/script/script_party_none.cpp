/// @file
/// C7: the party statements' port of a program without battle data (capsule/script/port.h namespace party):
/// heal() has no maxima to fill and does nothing; count() counts every member of the party boxes::attach() gave
/// (no eggs without battle data); find_move() finds nothing (no move table); C8's give() and give_egg() give
/// nothing (no species data); C13: lead() is slot 0, count_alive() and has_type() find nothing and member_field()
/// reads 0 (no species or eggs without battle data). Weak (CAPSULE_WEAK), as the engine
/// library's defaults (src/script/script_party.cpp), which programs with the battle modules link instead.
#include "capsule/core/memory.h"
#include "capsule/monster/monster.h"
#include "capsule/script/ops.h"
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

CAPSULE_WEAK int32_t give(uint16_t species, uint8_t level, uint16_t item)
{
   (void)species;
   (void)level;
   (void)item;
   return kGivenNone;   // no species data to build from
}

CAPSULE_WEAK int32_t give_egg(uint16_t species)
{
   (void)species;
   return kGivenNone;
}

CAPSULE_WEAK int32_t lead()
{
   return 0;
}

CAPSULE_WEAK int32_t count_alive(int32_t except)
{
   (void)except;
   return 0;   // no hit points without battle data
}

CAPSULE_WEAK bool has_type(int32_t type)
{
   (void)type;
   return false;
}

CAPSULE_WEAK int32_t member_field(int32_t slot, MemberField field)
{
   (void)slot;
   (void)field;
   return 0;
}

} // namespace capsule::script::party
