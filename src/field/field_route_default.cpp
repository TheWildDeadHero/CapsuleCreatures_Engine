/// @file
/// The engine's default game route steps (I4, capsule/script/route.h RouteOp::Game): every step is done at once. A
/// game whose scripts declare `step` words defines field::game::route_step() itself; this definition is weak
/// (CAPSULE_WEAK), so the game's own one is kept.
#include "capsule/core/memory.h"
#include "capsule/field/field.h"

namespace capsule::field::game
{

CAPSULE_WEAK bool route_step(uint8_t mover, uint8_t step, uint8_t arg)
{
   (void)mover;
   (void)step;
   (void)arg;
   return true;
}

} // namespace capsule::field::game
