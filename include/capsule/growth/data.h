/// @file
/// Growth tables (D-26 section 4, package G4): each species' transformation rules and forms, the context values
/// the rules read and the training rules, compiled from the battle manifests by tools/battlec/growth.py into
/// capsule::growth::kGrowth with rules::growth_when() and rules::train_gain() (in the generated battle.cpp;
/// src/growth/growth_empty.cpp is the empty default of a program without battle data). Every rule, number and
/// name is manifest data; the engine only evaluates the generated expressions at its trigger points.
///
/// Species indices are the battle's (capsule::battle::kData.species, key order): SpeciesGrowth has one entry per
/// species. A form is a full species view (FormData::species: the species with the form's overrides), so the
/// battle reads a battler's base stats, types, passives and sprites through growth::species_view() and nothing
/// else needs to know about forms.
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/battle/data/creatures.h"
#include "capsule/core/pod.h"
#include "capsule/monster/monster.h"

#include <cstdint>

namespace capsule::growth
{

/// What made the engine look at the rules (a rule's `on`; tools/battlec/growth.py TRIGGERS, the same order).
enum class Trigger : uint8_t
{
   LevelUp,       ///< The creature levelled up (in battle: looked at once the battle is over).
   ItemUse,       ///< An item's use_field logic ran `transform` (Context::item: the item).
   Trade,         ///< The record arrived by a trade (Context::item: its held item).
   AfterBattle,   ///< A battle it was in is over.
   Script,        ///< A script asked (`transform MEMBER`).
   NUM_TRIGGERS   ///< Number of triggers.
};

/// Most context values a manifest declares (tools/battlec/growth.py MAX_CONTEXT_VALUES).
inline constexpr int kMaxContextValues = 8;
/// Most forms of one species besides the species itself (the form role's value; growth.py MAX_FORMS).
inline constexpr int kMaxForms = 15;
/// The base form (the species itself): form index and form name id 0.
inline constexpr uint8_t kBaseForm = 0;
/// No rule (a form only the effect primitive `form` chooses; no blocked expression).
inline constexpr uint16_t kNoRule = 0xFFFF;
/// No species (TransformRule::also without a second creature).
inline constexpr uint16_t kNoSpecies = 0xFFFF;
/// TransformRule::flags: the held item is used up (a trade-with-item rule).
inline constexpr uint8_t kTakesItem = 1;
/// TrainingRules::yield_fields entry of a stat without a yield.
inline constexpr uint8_t kNoField = 0xFF;

/// Where a context value comes from.
enum class ContextSource : uint8_t
{
   Game,    ///< The game's port function growth::context_value() (default: the manifest's default).
   Clock,   ///< The game's clock port, clock::time_of_day() (capsule/core/clock.h; D-26 decision 3).
};

/// The trigger context a rule is evaluated in.
struct Context
{
   int32_t values[kMaxContextValues];   ///< The context values (GrowthData::contexts order).
   uint16_t item;                       ///< ItemUse: the item used; Trade: the held item; else 0.
   Trigger trigger;                     ///< The trigger.
   uint8_t reserved;                    ///< 0.
};

/// What the generated rules::growth_when() reads.
struct WhenInputs
{
   const monster::Record* rec;   ///< The record (its roles through battle::record_value()).
   const Context* ctx;           ///< The trigger context.
   const uint16_t* stats;        ///< Its permanent stats (BattleData::stats order; its form's base stats).
   uint16_t species;             ///< Its species index.
};

/// One transformation rule of a species (`transforms[]`).
struct TransformRule
{
   uint16_t into;       ///< The species it becomes.
   uint16_t also;       ///< A species made beside it into a free party slot (a companion), or kNoSpecies.
   uint16_t rule;       ///< Its `when` (rules::growth_when() id).
   uint8_t triggers;    ///< The triggers it can hold for (bit per Trigger).
   uint8_t flags;       ///< kTakesItem.
};
CAPSULE_POD(TransformRule, 8);

/// One form of a species besides the species itself.
struct FormData
{
   battle::SpeciesData species;   ///< The species as this form gives it (base stats, types, passives, sprites).
   uint16_t rule;                 ///< Its `when` (rules::growth_when() id), or kNoRule (effect logic only).
   uint8_t name;                  ///< Its name id (battle_ids.h FORM_<NAME>; effect logic `form WHO NAME`).
   uint8_t triggers;              ///< The triggers its rule can hold for (bit per Trigger).
};

/// A species' rules and forms (slices of GrowthData::transforms and GrowthData::forms).
struct SpeciesGrowth
{
   uint16_t first_transform;   ///< First of its transformation rules.
   uint16_t first_form;        ///< First of its forms (form index 1).
   uint16_t base_rule;         ///< The rule that brings it back to its own form (forms[] "base"), or kNoRule.
   uint8_t transform_count;    ///< Its rules (first that holds wins).
   uint8_t form_count;         ///< Its forms (<= kMaxForms).
   uint8_t base_triggers;      ///< The triggers base_rule can hold for (bit per Trigger).
   uint8_t reserved[kPadAfterOneByte];   ///< 0.
};
CAPSULE_POD(SpeciesGrowth, 12);

/// One context value.
struct ContextValue
{
   int32_t initial;          ///< The manifest's default (what the default port answers).
   int32_t lo;               ///< Lowest value.
   int32_t hi;               ///< Highest value.
   ContextSource source;     ///< Where it comes from.
   uint8_t reserved[kPadAfterOneByte];   ///< 0.
};
CAPSULE_POD(ContextValue, 16);

/// Training values (structure `training`): what defeating a foe gives each stat of the ones sharing its
/// experience (exp_policy), and the caps.
struct TrainingRules
{
   uint16_t stat_cap;                         ///< Highest training value of one stat.
   uint16_t total_cap;                        ///< Highest sum of every stat's training value.
   uint8_t yield_fields[battle::kMaxStats];   ///< Per stat: the species field holding its yield, or kNoField.
   uint8_t enabled;                           ///< 1 when the manifest has training (else nothing is gained).
   /// The ruleset point each stat's gain goes through in battle (structure training.point; C6: the gainer's owners,
   /// e.g. a held item that doubles gains, change it), or 0xFF: none.
   uint8_t gain_point;
};

/// The game's growth tables.
struct GrowthData
{
   const TransformRule* transforms;   ///< transform_count rules, species by species.
   const FormData* forms;             ///< form_count forms, species by species.
   const SpeciesGrowth* species;      ///< species_count entries (the battle's species), or none (count 0).
   const ContextValue* contexts;      ///< context_count values.
   uint16_t transform_count;          ///< Rules.
   uint16_t form_count;               ///< Forms.
   uint16_t species_count;            ///< Entries of species (the battle's species_count, or 0).
   uint16_t blocked_rule;             ///< The blocked expression's rule (while it holds nothing changes), or kNoRule.
   monster::RoleSet rederive;         ///< Stored derived roles a transformation re-derives (they read the species).
   monster::RoleId form_role;         ///< The record role holding the form, or monster::kNoRole.
   uint8_t context_count;             ///< Context values (<= kMaxContextValues).
   uint8_t cancel_mask;               ///< Triggers whose transformation the player may stop (bit per Trigger).
   uint8_t learn;                     ///< 1: a transformed creature learns its new species' moves of its level.
   TrainingRules training;            ///< Training values.
};

/// The build's tables (generated with the battle data; the default has no rules, forms or training).
extern const GrowthData kGrowth;

} // namespace capsule::growth

namespace capsule::rules
{

/// Whether a growth rule holds (generated from the manifest's `when` expressions; G4).
/// @param rule  rule id (TransformRule::rule, FormData::rule, GrowthData::blocked_rule)
/// @param in    the record, its stats and species, the trigger context
/// @return true when the expression is not 0 (false for an unknown id)
bool growth_when(int32_t rule, const growth::WhenInputs& in);

/// The training a stat gains from one defeated foe (structure training.gain, a named ruleset formula).
/// @param yield  the foe species' yield of the stat (> 0)
/// @param value  the stat's training value now
/// @param total  the sum of every stat's training value now
/// @param level  the receiver's level
/// @return the gain before the caps (0 without training)
int32_t train_gain(int32_t yield, int32_t value, int32_t total, int32_t level);

/// @name What a field action's requirement reads besides the growth inputs (F2, D-27 section 3; tools/battlec/field.py)
///@{

/// Whether the member knows a move (`knows.MOVE`; src/battle/field_requirement.cpp).
/// @param in    the member (its record)
/// @param move  move index
/// @return 1 when one of its move slots holds it, else 0
int32_t member_knows(const growth::WhenInputs& in, int32_t move);

/// The member's passive (`passive`; its species' slot named by the passive selector, as a battler gets it).
/// @param in  the member
/// @return 0 for none, else the passive index + 1 (the words passive.NAME)
int32_t member_passive(const growth::WhenInputs& in);

/// A game state value (`flag.NAME` / `var.NAME`): the script bank's field action reference k
/// (capsule/script/field_actions.h; a program without a script bank links src/script/field_actions_none.cpp: 0).
/// @param ref  reference index (tools/battlec/field.py game_refs() order)
/// @return the flag (0 / 1) or the variable's value
int32_t field_state(int32_t ref);
///@}

} // namespace capsule::rules
