/// @file
/// Field rules (D-27 sections 1 to 4, packages F1 to F3): the structure's `lead` rule, the count of its timed field
/// effects, of its field actions with their requirements' first growth rule and of its step effects, as data
/// (tools/battlec/field.py). The lead rules themselves (the encounter hooks, the encounter_rate and
/// encounter_level pipelines, the encounter_made hook) are capsule/battle/encounter_lead.h; the timed field effects'
/// records and state live with the script bank (capsule/script/field_effects.h).
///
/// Not part of BattleData: the table is the build's capsule::battle::kFieldRules (generated with the battle data;
/// the engine's default has no lead rule and no field effects).
#pragma once

#include "capsule/core/pod.h"

#include <cstdint>

namespace capsule::battle
{

/// FieldRules::lead_rule: no rule (the lead is the first member that can battle).
inline constexpr uint16_t kNoLeadRule = 0xFFFF;
/// FieldRules::first_action_rule: no field actions.
inline constexpr uint16_t kNoActionRule = 0xFFFF;

/// The build's field rules (ROM).
struct FieldRules
{
   uint16_t lead_rule;          ///< The lead's growth rule (rules::growth_when() id; capsule/growth/data.h), or
                                ///< kNoLeadRule.
   uint8_t field_effect_count;  ///< The structure's timed field effects (battle_ids.h FIELD_EFFECT_<NAME>).
   uint8_t field_action_count;  ///< F2: the structure's field actions (battle_ids.h FIELD_ACTION_<NAME>).
   uint16_t first_action_rule;  ///< F2: field action k's requirement is growth rule first_action_rule + k, or
                                ///< kNoActionRule.
   uint8_t step_effect_count;   ///< F3: the structure's step effects (battle_ids.h STEP_EFFECT_<NAME>; their records
                                ///< live with the script bank, capsule/script/step_effects.h).
   uint8_t reserved;            ///< 0.
   uint16_t hook_lead_rule;     ///< C8: the encounter hooks' member's growth rule (`lead.hook_when`: its passive and
                                ///< held item run them), or kNoLeadRule: the lead runs them.
   uint16_t reserved2;          ///< 0.
};
CAPSULE_POD(FieldRules, 12);

/// The build's field rules (generated; the engine's default: kNoLeadRule, no field effects, no field actions).
extern const FieldRules kFieldRules;

} // namespace capsule::battle
