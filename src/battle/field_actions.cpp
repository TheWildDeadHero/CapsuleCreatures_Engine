/// @file
/// Field actions: the member that may use one, the menu actions, and the dispatcher (see
/// capsule/battle/field_actions.h). Like encounter_lead.cpp it needs the field and script modules: the engine
/// library and the tests that list it link them.
#include "capsule/battle/field_actions.h"

#include "capsule/battle/data/field.h"
#include "capsule/combine/eggs.h"
#include "capsule/growth/growth.h"
#include "capsule/map/map.h"
#include "capsule/script/field_actions.h"

#include "internal.h"

namespace capsule::battle
{

using namespace detail;

namespace
{

/// The party an ActionScope attached (one at a time), or nullptr.
monster::Party* g_party = nullptr;

/// The actions both the battle data and the script bank declare.
/// @return their count
uint8_t action_count()
{
   const uint8_t bank = script::actions::count();
   return kFieldRules.field_action_count < bank ? kFieldRules.field_action_count : bank;
}

/// The script bank's check for `field_action available` (the attached party).
/// @param action  field action index
/// @return its member, or script::actions::kNoMember
uint8_t available_for_scripts(uint8_t action)
{
   if (g_party == nullptr)
   {
      return script::actions::kNoMember;
   }
   const uint8_t m = action_member(action, *g_party);
   return m == kBench ? script::actions::kNoMember : m;
}

} // namespace

uint8_t action_member(uint8_t action, const monster::Party& party)
{
   if (action >= action_count() || kFieldRules.first_action_rule == kNoActionRule)
   {
      return kBench;
   }
   const int32_t rule = int32_t{kFieldRules.first_action_rule} + action;
   const uint8_t n = party.count < monster::kMaxPartySize ? party.count : static_cast<uint8_t>(monster::kMaxPartySize);
   // the script trigger's context, as the lead rule's
   const growth::Context ctx = growth::make_context(growth::Trigger::Script);
   for (uint8_t k = 0; k < n; ++k)
   {
      const monster::Record& rec = party.members[k];
      const uint16_t species = find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
      // B2: an egg does no field action unless the action allows eggs (field_actions[].allow_eggs)
      if (species == kNone16 || (combine::is_egg(rec) && !combine::action_allows_eggs(action)))
      {
         continue;
      }
      uint16_t stats[kMaxStats] = {};
      record_stats(rec, stats);
      const growth::WhenInputs wi = {&rec, &ctx, stats, species};
      if (rules::growth_when(rule, wi))
      {
         return k;
      }
   }
   return kBench;
}

int menu_actions(const monster::Party& party, uint16_t map, MenuAction* out, int most)
{
   const uint16_t tags = map < map::kWorld.map_count ? map::kWorld.maps[map].tags : 0;
   int n = 0;
   for (uint8_t a = 0; a < action_count() && n < most; ++a)
   {
      const script::FieldActionInfo* info = script::actions::info(a);
      const bool here = info->target == script::ActionTarget::None ||
                        (info->target == script::ActionTarget::Tag && ((tags >> info->value) & 1u) != 0);
      if (!here)
      {
         continue;
      }
      const uint8_t member = action_member(a, party);
      if (member != kBench)
      {
         out[n++] = MenuAction{a, member};
      }
   }
   return n;
}

uint16_t start_action(uint8_t action, uint8_t member)
{
   const script::FieldActionInfo* info = action < action_count() ? script::actions::info(action) : nullptr;
   if (info == nullptr)
   {
      return script::kNoScriptId;
   }
   script::actions::set_member(member);
   return info->script;
}

uint16_t dispatch_action(const field::ActionFace& face)
{
   if (g_party == nullptr)
   {
      return field::kNoScript;
   }
   for (uint8_t a = 0; a < action_count(); ++a)
   {
      const script::FieldActionInfo* info = script::actions::info(a);
      if (info->target != face.target || info->value != face.value)
      {
         continue;
      }
      const uint8_t member = action_member(a, *g_party);
      if (member != kBench)
      {
         // the first action on this target that a member may use (the member goes to its script)
         return start_action(a, member);
      }
   }
   return field::kNoScript;
}

ActionScope::ActionScope(monster::Party& party)
   : previous_party_(g_party), previous_rules_(field::action_rules())
{
   g_party = &party;
   field::set_action_rules(dispatch_action);
   script::actions::attach(available_for_scripts);
}

ActionScope::~ActionScope()
{
   g_party = previous_party_;
   field::set_action_rules(previous_rules_);
   script::actions::attach(previous_party_ != nullptr ? available_for_scripts : nullptr);
}

} // namespace capsule::battle
