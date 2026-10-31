/// @file
/// Encounter leads (D-27 section 1, package F1): the party member that leads, and the rules it brings to the
/// encounter rolls of the overworld (capsule/encounter/encounter.h) while a LeadScope lives.
///
/// **The lead** is the structure's `lead` rule (tools/battlec/field.py): the first member for which its `when`
/// expression holds, else the first member that can battle (hit points above 0; a layout without stored hit points:
/// the first member). C8: its optional `hook_when` picks the member whose passive and held item run the hooks
/// (hook_lead_of(); e.g. the first party slot even when fainted); the lead stays the global owners' holder and
/// the level the pipelines see (a repel-style check). C9: eggs: the default lead (the first that can battle) never
/// is one unless eggs may battle; the rules skip eggs unless the structure's `lead.allow_eggs` is true, and an egg
/// such a rule picks has no passive or held item that reacts (an egg in the first slot runs no passive).
///
/// **A roll with the lead** (what LeadScope attaches with encounter::set_rules()), for one table and method:
///  1. the `encounter` hook (value = the method) in a field context on the lead (capsule/battle/field_context.h):
///     the game's global owners, then the lead's passive, then its held item (C8: the hooks' member's, in a field
///     context of its own when it is another member). `target` is not a creature here: the compiler refuses
///     writes through it in `encounter` (it is the holder's own record). Their primitives change the plan:
///     `encounter_rate N D` (the odds times N / D), `encounter_levels LO HI` (the entry's bounds moved),
///     `encounter_level_bias N` (the level pipeline's input), `encounter_skip_below LEVEL`, `encounter_type TYPE N D`
///     and `encounter_field FIELD V N D` (with odds N in D the pick keeps to the entries of that type / whose species
///     field is V, when the table has any); `block` stops the roll (no encounter);
///  2. the trigger odds: the ruleset pipeline encounter_rate (the table's odds, the plan's Q8 scale, the lead's
///     level, the method), then the draw;
///  3. the pick (encounter::choose(), the bias's entries when its odds pass) and the level: the ruleset pipeline
///     encounter_level (the moved bounds, the lead's level, the bias), clamped to 1..level_max;
///  4. a creature below the plan's skip level is no encounter.
/// Without hooks and with the ruleset's default pipelines this draws exactly what encounter::roll_plain() draws.
/// The hooks' own draws (`chance()`, `odds()`) come from the roll's stream.
///
/// **The creature** (make_encounter(), after field::take_encounter()): make_monster(), then the `encounter_made`
/// hook (holder = the lead, target = the creature: `target.role(NAME) = ...` writes its record, value = its level):
/// `encounter_reroll` picks again (at most kMaxEncounterRerolls times), `block` means no encounter.
///
/// Tables, methods and conditions (D-12) are unchanged: the rules only layer on top of them. Every roll leaves a
/// trace (last_encounter(): the odds, the lead, the pick, why it ended) for debug lines.
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/battle/effect.h"
#include "capsule/core/pod.h"
#include "capsule/core/random.h"
#include "capsule/encounter/encounter.h"
#include "capsule/monster/monster.h"

#include <cstdint>

namespace capsule::battle
{

/// Most extra picks encounter_reroll makes for one encounter.
inline constexpr int kMaxEncounterRerolls = 3;
/// EncounterPlan::rate_scale of an unchanged roll (x1 in Q8).
inline constexpr int32_t kRateScaleOne = 256;
/// Highest EncounterPlan::rate_scale (x16; ruleset_compiler MAX_RATE_SCALE).
inline constexpr int32_t kMaxRateScale = 4096;

/// What an encounter plan's pick keeps to.
enum class EncounterBias : uint8_t
{
   None,     ///< Every entry.
   Type,     ///< Entries whose species has the type EncounterPlan::bias_id.
   Field,    ///< Entries whose species field bias_id is bias_value.
   NUM_BIASES   ///< Number of kinds.
};

/// What the encounter hooks decided for one roll (the Encounter op's changes; EncounterOp).
struct EncounterPlan
{
   int32_t rate_scale;      ///< Trigger odds scale, Q8 (kRateScaleOne = x1), 0 to kMaxRateScale.
   int16_t lo_add;          ///< Added to the entry's lowest level.
   int16_t hi_add;          ///< Added to its highest.
   uint16_t bias_value;     ///< Field: the species field's value.
   uint8_t level_bias;      ///< The encounter_level pipeline's level_bias input.
   uint8_t skip_below;      ///< No encounter below this level (0: none).
   EncounterBias bias;      ///< What the pick keeps to.
   uint8_t bias_id;         ///< Type or species field index.
   uint8_t bias_n;          ///< The bias's odds: bias_n...
   uint8_t bias_d;          ///< ...in bias_d.
   uint8_t reroll;          ///< encounter_made asked for another pick.
   uint8_t reserved[kPadAfterOneByte];   ///< 0.
};
CAPSULE_POD(EncounterPlan, 20);

/// How the last roll ended (EncounterTrace::end).
enum class EncounterEnd : uint8_t
{
   None,        ///< No roll yet.
   NoTrigger,   ///< The trigger odds did not pass.
   Blocked,     ///< An encounter hook said block (before or after the pick).
   NoEntry,     ///< No entry could be picked under the active conditions.
   Skipped,     ///< The creature was below the plan's skip level.
   Picked,      ///< An encounter starts.
   NUM_ENDS     ///< Number of ends.
};

/// What the last roll did (sandboxes and debug lines).
struct EncounterTrace
{
   uint32_t chance;      ///< Trigger odds drawn with: chance...
   uint32_t out_of;      ///< ...in out_of (up to 65536).
   uint16_t species;     ///< The creature picked (kNone16 before a pick).
   uint16_t table;       ///< The table rolled.
   int32_t rate_scale;   ///< The plan's odds scale.
   uint8_t lead;         ///< The lead's party slot, or kBench.
   uint8_t level;        ///< The level picked (0 before a pick).
   uint8_t skip_below;   ///< The plan's skip level.
   uint8_t method;       ///< The method rolled.
   uint8_t rerolls;      ///< Extra picks encounter_reroll made.
   uint8_t handlers;     ///< Encounter hook owners run.
   EncounterEnd end;     ///< How it ended.
   uint8_t reserved;     ///< 0.
};
CAPSULE_POD(EncounterTrace, 24);

/// What make_encounter() made.
enum class Made : uint8_t
{
   Battle,    ///< The creature is in the party given: start the battle.
   Skipped,   ///< No encounter (an encounter_made hook blocked it, or a re-pick fell below the skip level).
   NUM_MADE   ///< Number of results.
};

/// The party's lead (the structure's lead rule).
/// @param party  the party
/// @return its party slot, or kBench when no member leads
uint8_t lead_of(const monster::Party& party);

/// The member whose passive and held item run the encounter hooks (C8: the structure's `lead.hook_when`, else the
/// lead).
/// @param party  the party
/// @return its party slot, or kBench
uint8_t hook_lead_of(const monster::Party& party);

/// Attaches the lead rules to the encounter rolls while it lives (RAII: encounter::set_rules(), then back to what
/// was attached before). One at a time; the State is lent for the field contexts of the hooks (not a battle in
/// progress while a roll runs).
class LeadScope
{
public:
   /// Attaches the rules.
   /// @param s      a State the hooks' field contexts use
   /// @param party  the player's party (its lead)
   LeadScope(State& s, monster::Party& party);
   /// Detaches them.
   ~LeadScope();
   LeadScope(const LeadScope&) = delete;              ///< Not copied.
   LeadScope& operator=(const LeadScope&) = delete;   ///< Not assigned.

private:
   encounter::RollRules previous_;   ///< What was attached before.
};

/// Makes the creature of an encounter (after field::take_encounter() / encounter::roll_at()): make_monster(), then
/// the lead's `encounter_made` hook (with a LeadScope attached; else just the creature).
/// @param r     the encounter
/// @param made  receives the creature as its only member (count 1)
/// @param rng   stream for the creature's random parts and the hooks' draws (rng::gameplay)
/// @return Battle, or Skipped
Made make_encounter(const encounter::Result& r, monster::Party& made, rng::Stream& rng);

/// What the last roll (and make_encounter()) did.
/// @return the trace
const EncounterTrace& last_encounter();

/// Effect primitive Op::Encounter (effect_host.cpp): changes the plan of the encounter hook running.
/// @param a  the action (id = EncounterOp, flags = type / species field, args = its operands)
/// @return the outcome (fail: no encounter hook is running, or encounter_reroll outside encounter_made)
effect::Outcome encounter_primitive(const effect::Action& a);

} // namespace capsule::battle
