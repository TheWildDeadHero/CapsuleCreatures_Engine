/// @file
/// The game-supplied context values (B1, D-28 section 1; capsule/growth/growth.h set_context_value()): a game sets a
/// named value such as a moon phase and the default port (src/growth/growth_port_default.cpp) answers it. Its own
/// file, so a game that defines growth::context_value() itself still links the setter without a second port.
#include "capsule/growth/growth.h"

#include "capsule/core/memory.h"

namespace capsule::growth
{

namespace
{

/// What the game set, and which values it set (bit per context value).
struct Store
{
   int32_t values[kMaxContextValues];   ///< The values set.
   uint32_t set;                        ///< Bit per value: set.
};

/// The store (zero at start-up: nothing set; EWRAM on the GBA: not speed-critical).
CAPSULE_EWRAM_BSS Store g_store;

} // namespace

void set_context_value(uint8_t index, int32_t value)
{
   if (index < kMaxContextValues)
   {
      g_store.values[index] = value;
      g_store.set |= 1u << index;
   }
}

bool stored_context_value(uint8_t index, int32_t& out)
{
   if (index >= kMaxContextValues || ((g_store.set >> index) & 1u) == 0)
   {
      return false;
   }
   out = g_store.values[index];
   return true;
}

} // namespace capsule::growth
