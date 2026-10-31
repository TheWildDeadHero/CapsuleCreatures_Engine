/// @file
/// Step effects (D-27 section 4, package F3): rules that run on the party every N steps of the walker (a poison-like
/// status hurting on the walk, a friendship-like role growing, an egg-like counter running down, ...).
///
/// The effects are the structure manifest's `step_effects` (name, label, every, damage rule, messages, script). The
/// script compiler keeps each one's counter in an implicit permanent script variable (`step_effect_<name>`: the
/// steps counted towards its next run, 0 to every - 1; capsule/script/data.h StepEffectInfo), so the save image's
/// Script section holds it under the save shape and remap rules. The overworld runner (capsule/overworld/overworld.h)
/// counts the walker's steps into walked(); an effect whose counter reaches its `every` is due (take_due()), and the
/// battle side (capsule/battle/step_effects.h: the `steps` effect-logic hook on each member, the damage rule, the
/// messages, the script) runs it.
///
/// This side knows nothing of battles: it counts, reports what is due and writes the implicit member and outcome
/// variables a step effect's script reads. Effect indices follow the structure's list (battle_ids.h
/// STEP_EFFECT_<NAME>). A program without a script bank that declares the effects has none: count() is 0.
#pragma once

#include "capsule/script/data.h"

#include <cstdint>

namespace capsule::script::step_effects
{

/// The effects the bank declares.
/// @return 0 to kMaxStepEffects
uint8_t count();

/// One effect's record.
/// @param effect  index (the structure's order)
/// @return it, or nullptr past count()
const StepEffectInfo* info(uint8_t effect);

/// The steps an effect has counted towards its next run.
/// @param effect  index
/// @return 0 to every - 1 (0 for no such effect)
int32_t counted(uint8_t effect);

/// The steps until an effect's next run.
/// @param effect  index
/// @return 1 to every (0 for no such effect)
int32_t steps_left(uint8_t effect);

/// Counts the walker's steps into every effect: an effect whose count reaches its `every` starts over (the steps
/// past it count on) and is due. Several runs' worth of steps in one call make it due once.
/// @param steps  steps walked since the last call
void walked(uint32_t steps);

/// The next due effect (in index order); it is no longer due.
/// @param[out] effect  its index
/// @return false when none is due
bool take_due(uint8_t* effect);

/// Whether an effect is due.
/// @param effect  index
/// @return true when walked() made it due and take_due() did not report it yet
bool due(uint8_t effect);

/// Forgets every due effect (a new game, a loaded save: the counters stay as saved).
void clear_due();

/// Writes what an effect's script reads (its implicit member and outcome variables; nothing without them).
/// @param member   party slot
/// @param outcome  capsule::battle::StepOutcome value
void set_outcome(uint8_t member, uint8_t outcome);

/// The steps walked() counted since the last call (B2: the nursery's experience per step, claimed by the battle
/// side's step_next()).
/// @return steps
uint32_t take_walked();

/// The script that runs for each egg that hatches (B2: the structure's eggs.hatch_script).
/// @return its ScriptId, or kNoStepScript
uint16_t hatch_script();

/// Writes the party slot of the egg that hatched into the implicit variable egg_member (nothing without it).
/// @param member  party slot
void set_egg_member(uint8_t member);

} // namespace capsule::script::step_effects
