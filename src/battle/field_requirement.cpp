/// @file
/// What a field action's requirement reads besides the growth inputs (F2, D-27 section 3): whether the member
/// knows a move and its passive (capsule/growth/data.h rules::member_knows() / member_passive(); the generated
/// rules::growth_when() calls them for `knows.MOVE` and `passive`). A battle module: every program with battle data
/// links it. The game state (`flag.NAME` / `var.NAME`) is the script bank's (src/script/field_actions.cpp).
#include "capsule/growth/data.h"
#include "capsule/growth/growth.h"

#include "internal.h"

namespace capsule::rules
{

using namespace battle::detail;

int32_t member_knows(const growth::WhenInputs& in, int32_t move)
{
   if (in.rec == nullptr || move < 0)
   {
      return 0;
   }
   for (int i = 0; i < battle::move_slots(); ++i)
   {
      if (monster::get(*in.rec, monster::Role::Moves, i) == static_cast<uint32_t>(move))
      {
         return 1;
      }
   }
   return 0;
}

int32_t member_passive(const growth::WhenInputs& in)
{
   if (in.rec == nullptr || in.species >= data().species_count)
   {
      return 0;
   }
   // as a battler gets it (state.cpp): the record's form gives the passives, the selector's low bits the slot
   const battle::SpeciesData& sp = growth::species_view(in.species, growth::record_form(*in.rec));
   const uint32_t selector = battle::record_value(*in.rec, monster::id_of(monster::Role::PassiveSelector));
   const uint8_t p = passive_for(sp, static_cast<uint8_t>(selector & data().passive_selector_mask));
   return p == battle::kNone8 ? 0 : int32_t{p} + 1;
}

} // namespace capsule::rules
