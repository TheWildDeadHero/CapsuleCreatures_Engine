/// @file
/// Eggs (D-28 section 3, package B2): the egg role and hatch counter of a record, the refusal flags, the egg's look
/// (see capsule/combine/eggs.h).
#include "capsule/combine/eggs.h"

#include "capsule/battle/battle.h"
#include "capsule/battle/ruleset.h"

namespace capsule::combine
{

namespace
{

using monster::Role;

/// The egg rules.
/// @return them
const EggData& egg_rules()
{
   return kCombine.eggs;
}

/// A record's species.
/// @param rec  the record
/// @return its species index, or kNoSpecies
uint16_t species_of(const monster::Record& rec)
{
   const uint16_t sp = battle::find_species(AssetKey{monster::get(rec, Role::SpeciesKey)});
   return sp == battle::kNone16 ? kNoSpecies : sp;
}

/// A bit of a flag word.
/// @param index  bit index (below 32)
/// @return the bit
constexpr uint32_t bit_of(uint8_t index)
{
   return uint32_t{1} << index;
}

/// Bits of the field action and step effect masks (EggData::field_actions, step_effects).
constexpr uint8_t kActionBits = 32;

} // namespace

bool eggs_on()
{
   return egg_rules().role != monster::kNoRole && monster::has(egg_rules().role);
}

bool is_egg(const monster::Record& rec)
{
   return eggs_on() && monster::get(rec, egg_rules().role) != 0;
}

uint32_t hatch_left(const monster::Record& rec)
{
   return is_egg(rec) ? monster::get(rec, egg_rules().counter) : 0;
}

uint32_t hatch_steps(uint16_t species, uint16_t cycle_steps)
{
   if (!eggs_on() || species >= battle::kData.species_count)
   {
      return 0;
   }
   const int32_t cycles = rules::species_field_of(species, egg_rules().cycles_field);
   const uint32_t steps = static_cast<uint32_t>(cycles > 0 ? cycles : 0) * cycle_steps;
   const uint32_t top = monster::max_value(egg_rules().counter);
   return steps < top ? steps : top;
}

bool make_egg(monster::Record& rec, uint16_t cycle_steps)
{
   const uint16_t sp = species_of(rec);
   if (!eggs_on() || sp == kNoSpecies)
   {
      return false;
   }
   monster::set(rec, egg_rules().role, 0, 1);
   monster::set(rec, egg_rules().counter, 0, hatch_steps(sp, cycle_steps));
   return true;
}

void set_hatch_left(monster::Record& rec, uint32_t steps)
{
   if (!is_egg(rec))
   {
      return;
   }
   const uint32_t top = monster::max_value(egg_rules().counter);
   monster::set(rec, egg_rules().counter, 0, steps < top ? steps : top);
}

bool count_down(monster::Record& rec, uint32_t steps)
{
   if (!is_egg(rec))
   {
      return false;
   }
   const uint32_t left = monster::get(rec, egg_rules().counter);
   const uint32_t after = left > steps ? left - steps : 0;
   monster::set(rec, egg_rules().counter, 0, after);
   return after == 0;
}

bool hatch(monster::Record& rec)
{
   if (!is_egg(rec))
   {
      return false;
   }
   monster::set(rec, egg_rules().role, 0, 0);
   monster::set(rec, egg_rules().counter, 0, 0);
   // the stats follow its level and species; the pools (hit points among them) start full
   battle::fill_resources(rec);
   return true;
}

bool allowed(EggUse use)
{
   uint8_t want = kEggsBattle;
   switch (use)
   {
   case EggUse::Recruit:
      want = kEggsRecruit;
      break;
   case EggUse::Lead:
      want = kEggsLead;
      break;
   case EggUse::Transform:
      want = kEggsTransform;
      break;
   default:
      break;
   }
   return (egg_rules().flags & want) != 0;
}

bool action_allows_eggs(uint8_t action)
{
   return action < kActionBits && (egg_rules().field_actions & bit_of(action)) != 0;
}

bool step_allows_eggs(uint8_t effect)
{
   return effect < kActionBits && (egg_rules().step_effects & bit_of(effect)) != 0;
}

bool transfer_hatches_eggs()
{
   return (egg_rules().flags & kEggsTransferHatch) != 0;
}

const text::Code* egg_name()
{
   return eggs_on() ? egg_rules().name : nullptr;
}

uint32_t egg_sprite()
{
   return egg_rules().sprite;
}

int32_t egg_look(const monster::Record& rec)
{
   const uint16_t sp = species_of(rec);
   if (egg_rules().sprite_field == kNoField || sp == kNoSpecies)
   {
      return 0;
   }
   return rules::species_field_of(sp, egg_rules().sprite_field);
}

} // namespace capsule::combine
