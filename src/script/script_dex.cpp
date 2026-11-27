/// @file
/// D-36 §3.1 G3: the Dex statement's port (capsule/script/port.h, capsule::script::dex) over the engine's records
/// (capsule/dex/dex.h). Weak: a game with its own rules (a regional list, a completion rule, ids that are not its dex
/// numbers) defines these. Without a regional list the regional count is every entry's; completion and the national
/// mode are game rules, so they answer 0 and do nothing.
#include "capsule/core/memory.h"
#include "capsule/dex/dex.h"
#include "capsule/script/port.h"

namespace capsule::script::dex
{

CAPSULE_WEAK int32_t count(bool caught, bool regional)
{
   (void)regional;   // no regional list in the engine: every entry
   return static_cast<int32_t>(capsule::dex::count(caught, 1, capsule::dex::kMaxEntries));
}

CAPSULE_WEAK int32_t species(bool caught, int32_t id)
{
   // the id as the entry (a game whose species ids are not its dex numbers replaces this)
   if (id <= 0 || id > capsule::dex::kMaxEntries)
   {
      return 0;
   }
   return capsule::dex::has(caught, static_cast<uint16_t>(id)) ? 1 : 0;
}

CAPSULE_WEAK int32_t complete(bool regional)
{
   (void)regional;
   return 0;
}

CAPSULE_WEAK int32_t national()
{
   return 0;
}

CAPSULE_WEAK void enable_national()
{
}

} // namespace capsule::script::dex
