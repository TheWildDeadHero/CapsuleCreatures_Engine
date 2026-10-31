/// @file
/// Step effects, the battle side (D-27 section 4, package F3): what runs on the party when a step effect is due
/// (capsule/script/step_effects.h counts the walker's steps and says which).
///
/// **One run** of a due effect goes over the party in slot order. For each member with a species and a handler for
/// the `steps` hook, a field context opens on it (capsule/battle/field_context.h; its record as battler
/// kFieldBattler, with the StepScope's stream) and the handlers run with value = the effect's index
/// (`field.step_effect == NAME`): the game's global owners, then the member's status, its passive, its held item.
/// Their primitives and reads work as in any field context: `damage_hp holder 1 quiet no_faint_check`, `cure`,
/// `restore`, `holder.role(NAME) = ...` (written into the record), `field_effect start ...`, `text`, `say`, `cue`.
/// Then the engine applies the effect's rule:
///  - a member whose hit points went from above 0 to 0: the damage rule Survive gives it 1 back (outcome
///    Survived), Faint leaves it at 0 (outcome Fainted);
///  - one that lost hit points and kept some: outcome Hurt;
/// queues the effect's message for that outcome (the battle's message, {user} = the member) and closes the context
/// (the member is written back: hit points, status, resources, moves). The texts queued by the handlers and the
/// message stay in the StepScope's State until the next step_next(): a game shows them in between (pop_event(),
/// format_text()).
///
/// After the last member, a run with an outcome writes the worst one and its member (C8: the first member in party
/// order that had it) to the implicit variables and
/// starts the effect's script, if it names one, in the overworld runner (overworld::run_script(); while the
/// runner is busy the run waits and step_next() tries again).
///
/// B2 (D-28 sections 2 and 3): after a run's members and before its script, the run of the nursery's check effect
/// runs combine::nursery::check() and the run of the eggs' step effect is an egg cycle: every party member that is
/// not an egg answers the `hatch` hook (value = the effect's every; the largest answer counts), every egg's counter
/// loses that many steps, an egg at 0 hatches (combine::hatch()) and the eggs' hatch script starts in the overworld
/// runner, one egg at a time, with its party slot in egg_member (step_next() waits while the runner is busy). Each
/// step_next() first counts the steps walked since the last call into the nursery's experience
/// (script::step_effects::take_walked(), combine::nursery::walked()).
///
/// Engine library only (it needs the script and overworld modules, as encounter_lead.cpp does).
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/core/pod.h"
#include "capsule/core/random.h"
#include "capsule/monster/monster.h"

#include <cstdint>

namespace capsule::battle
{

/// A member's outcome of one run (tools/battlec/field.py STEP_OUTCOMES; the outcome variable's values).
enum class StepOutcome : uint8_t
{
   None,       ///< Its hit points did not go down.
   Hurt,       ///< It lost hit points and kept some.
   Survived,   ///< The run took them to 0 and the survive rule kept 1.
   Fainted,    ///< The run took them to 0.
   NUM_OUTCOMES   ///< Number of outcomes.
};

/// What one member's part of a run did (step_next()).
struct StepEvent
{
   uint8_t effect;       ///< The step effect (battle_ids.h STEP_EFFECT_<NAME>).
   uint8_t member;       ///< The member's party slot.
   StepOutcome outcome;  ///< What happened to its hit points.
   uint8_t handlers;     ///< Owners whose handlers ran.
   uint16_t hp_before;   ///< Its hit points before the handlers.
   uint16_t hp_after;    ///< After the rule.
};
CAPSULE_POD(StepEvent, 8);

/// Attaches a party, a State and a stream to the step effects while it lives (RAII; one at a time). The State is
/// lent for the field contexts (not a battle in progress); the stream gives the handlers' draws (`chance()`).
class StepScope
{
public:
   /// Attaches them; nothing is due yet (script::step_effects::clear_due()).
   /// @param s      a State the field contexts use
   /// @param party  the player's party
   /// @param rng    the draws' stream (rng::gameplay)
   StepScope(State& s, monster::Party& party, rng::Stream& rng);
   /// Detaches them (a run under way is dropped).
   ~StepScope();
   StepScope(const StepScope&) = delete;              ///< Not copied.
   StepScope& operator=(const StepScope&) = delete;   ///< Not assigned.
};

/// Runs the next member of the run under way, or starts the next due effect's run. Members without a handler for
/// the hook are passed over without a field context.
/// @param[out] out  what the member's part did
/// @return true when a member's handlers ran (out is set; their texts are queued in the State), false when nothing
///         is due or under way (or the run's script waits for the overworld runner)
bool step_next(StepEvent& out);

/// Whether a run is under way or an effect is due.
/// @return true while step_next() has work
bool step_busy();

/// What the last member's part did (sandboxes and debug lines).
/// @return the last event (effect kNone8 before any)
const StepEvent& last_step();

} // namespace capsule::battle
