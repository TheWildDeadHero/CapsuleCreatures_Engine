/// @file
/// C7: the engine's default party statements (capsule/script/port.h namespace party: heal(), count(),
/// find_move()) over the party attach() gave. Weak (CAPSULE_WEAK): a game that keeps its own party defines them.
/// The stash is in src/script/script_party_stash.cpp, so a game with its own does not link the default's buffer.
#include "capsule/battle/battle.h"
#include "capsule/battle/data.h"
#include "capsule/combine/eggs.h"
#include "capsule/core/memory.h"
#include "capsule/monster/monster.h"
#include "capsule/script/port.h"

namespace capsule::script::party
{

CAPSULE_WEAK void heal()
{
   monster::Party* p = boxes::party();
   for (int i = 0; p != nullptr && i < p->count && i < monster::kMaxPartySize; ++i)
   {
      // hit points and move points to their maxima, then no status
      battle::fill_resources(p->members[i]);
      monster::set(p->members[i], monster::Role::PrimaryStatus, 0);
   }
}

CAPSULE_WEAK int32_t count(bool hatched)
{
   const monster::Party* p = boxes::party();
   int32_t n = 0;
   for (int i = 0; p != nullptr && i < p->count && i < monster::kMaxPartySize; ++i)
   {
      n += !hatched || !combine::is_egg(p->members[i]) ? 1 : 0;
   }
   return n;
}

CAPSULE_WEAK bool find_move(uint16_t move, int32_t& slot, int32_t& species)
{
   const monster::Party* p = boxes::party();
   const int slots = monster::count(monster::Role::Moves);
   for (int i = 0; p != nullptr && i < p->count && i < monster::kMaxPartySize; ++i)
   {
      const monster::Record& rec = p->members[i];
      if (combine::is_egg(rec))
      {
         continue;   // an egg knows no move yet
      }
      for (int s = 0; s < slots; ++s)
      {
         if (move != monster::kNoMove && monster::get(rec, monster::Role::Moves, s) == move)
         {
            slot = i;
            species = battle::find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
            return true;
         }
      }
   }
   slot = monster::kLayout.party_size;
   return false;
}

} // namespace capsule::script::party
