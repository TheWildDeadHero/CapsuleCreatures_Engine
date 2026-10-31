/// @file
/// The engine's default field hooks: a world without step hooks never calls
/// them. A game with step hooks defines field::game::step_hook() in its own
/// code, which keeps this file out of the link.
#include "capsule/core/assert.h"
#include "capsule/field/field.h"

namespace capsule::field::game
{

uint16_t step_hook(uint16_t hook)
{
   (void)hook;
   ENGINE_ASSERT(false && "the world has step hooks: the game must define field::game::step_hook()");
   return kNoScript;
}

} // namespace capsule::field::game
