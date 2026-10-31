/// @file
/// Engine default natives: none. A game builds its own file defining
/// game::native() instead of this one (capsule/script/port.h).
#include "capsule/script/port.h"

#include "capsule/core/assert.h"

namespace capsule::script::game
{

NativeStatus native(uint16_t, const int32_t*, uint8_t, int32_t& result)
{
   ENGINE_ASSERT(false && "script native called, but the game defines no natives");
   result = 0;
   return NativeStatus::Done;
}

} // namespace capsule::script::game
