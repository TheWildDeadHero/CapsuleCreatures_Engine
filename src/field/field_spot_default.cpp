/// @file
/// The engine's default spotter group limit (C1b, capsule/field/field.h game::spot_positions()): none beyond the
/// world's own (map::World::spotter_positions). A game whose battle formats or party limit a group defines
/// field::game::spot_positions() itself; this definition is weak (CAPSULE_WEAK), so the game's own one is kept.
#include "capsule/core/memory.h"
#include "capsule/field/field.h"

namespace capsule::field::game
{

CAPSULE_WEAK uint8_t spot_positions()
{
   return map::kMaxSpotPositions;
}

} // namespace capsule::field::game
