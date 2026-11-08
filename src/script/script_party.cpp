/// @file
/// C7: the engine's default party statements (capsule/script/port.h namespace party: heal(), count(),
/// find_move(); C8: give(), give_egg()) over the party the box statements were given. Weak (CAPSULE_WEAK): a game
/// that keeps its own party defines them. The stash is in src/script/script_party_stash.cpp, so a game with its own
/// does not link the default's buffer.
#include "capsule/battle/battle.h"
#include "capsule/battle/data.h"
#include "capsule/combine/data.h"
#include "capsule/combine/eggs.h"
#include "capsule/core/memory.h"
#include "capsule/core/random.h"
#include "capsule/monster/monster.h"
#include "capsule/script/ops.h"
#include "capsule/script/port.h"
#include "capsule/storage/storage.h"

namespace capsule::script::party
{
namespace
{

constexpr uint8_t kEggLevel = 1;   ///< C8: the level a given egg is built at (the lowest make_monster() accepts).

/// C8: places a new record: the party when it has room, else the attached boxes.
/// @param rec  the record
/// @return kGivenParty, kGivenBoxes or kGivenNone
int32_t place(const monster::Record& rec)
{
   monster::Party* p = boxes::party();
   if (p != nullptr && p->count < monster::kLayout.party_size && p->count < monster::kMaxPartySize)
   {
      p->members[p->count++] = rec;
      return kGivenParty;
   }
   storage::Storage* st = boxes::attached();
   return st != nullptr && storage::deposit(*st, rec) != storage::kNoSlot ? kGivenBoxes : kGivenNone;
}

/// C8: the steps of an egg cycle: the first combination method that delivers eggs, else 0.
/// @return them
uint16_t egg_cycle_steps()
{
   for (uint8_t i = 0; i < combine::kCombine.method_count; ++i)
   {
      if (combine::kCombine.methods[i].cycle_steps != 0)
      {
         return combine::kCombine.methods[i].cycle_steps;
      }
   }
   return 0;
}

} // namespace

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

CAPSULE_WEAK int32_t give(uint16_t species, uint8_t level, uint16_t item)
{
   monster::Record rec{};
   if (!battle::make_monster(rec, species, level, rng::gameplay))
   {
      return kGivenNone;
   }
   monster::set(rec, monster::Role::HeldItem, item);
   return place(rec);
}

CAPSULE_WEAK int32_t give_egg(uint16_t species)
{
   monster::Record rec{};
   if (!battle::make_monster(rec, species, kEggLevel, rng::gameplay) || !combine::make_egg(rec, egg_cycle_steps()))
   {
      return kGivenNone;
   }
   return place(rec);
}

} // namespace capsule::script::party
