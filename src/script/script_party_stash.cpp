/// @file
/// C7: the engine's default party stash (capsule/script/port.h party::stash() / restore()): one copy of the
/// attached party, kept until restored. Weak (CAPSULE_WEAK) and alone in this file: a game that keeps its own copy
/// defines both and does not link this buffer (one party: 604 B on the GBA).
#include "capsule/core/memory.h"
#include "capsule/monster/monster.h"
#include "capsule/script/port.h"

namespace capsule::script::party
{
namespace
{

/// The stash.
struct Stash
{
   monster::Party party;   ///< The copy.
   bool held;              ///< A copy is held.
};
CAPSULE_EWRAM_BSS Stash g_stash;   ///< The default stash (zero at start-up; EWRAM on the GBA).

} // namespace

CAPSULE_WEAK void stash()
{
   const monster::Party* p = boxes::party();
   if (p != nullptr)
   {
      g_stash.party = *p;
      g_stash.held = true;
   }
}

CAPSULE_WEAK void restore()
{
   monster::Party* p = boxes::party();
   if (p != nullptr && g_stash.held)
   {
      *p = g_stash.party;
   }
}

} // namespace capsule::script::party
