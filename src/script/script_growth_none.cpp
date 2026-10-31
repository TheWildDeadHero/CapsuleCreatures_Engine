/// @file
/// The growth script port of a program without battle data (capsule/script/growth_port.h, G4): nothing is
/// attached and nothing ever transforms (`transform(...) -> var(v)` gives kTransformNone at once).
#include "capsule/script/growth_port.h"

namespace capsule::script::growth
{

void attach(battle::State* lent, monster::Party* party)
{
   (void)lent;
   (void)party;
}

void transform_open(uint8_t slot)
{
   (void)slot;
}

int16_t transform_poll()
{
   return kTransformNone;
}

void ask_open(uint8_t slot, uint16_t from, uint16_t into, bool cancellable)
{
   (void)slot;
   (void)from;
   (void)into;
   (void)cancellable;
}

int8_t ask_poll()
{
   return kAskGo;
}

} // namespace capsule::script::growth
