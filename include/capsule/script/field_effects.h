/// @file
/// Timed field effects (D-27 section 2, package F1): a ward that keeps weak creatures away, a lure, a light, ...
/// that run for a number of steps or minutes after an item's use_field logic, a script or game code starts them.
///
/// The effects are the structure manifest's `field_effects` (name, label, unit, stack rule, expiry message, whether
/// they survive a save). The script compiler keeps each one's state in an implicit permanent script variable
/// (`field_effect_<name>`: what it has left; capsule/script/data.h FieldEffectInfo), so the save image's Script
/// section holds it under the save shape and remap rules; a loaded save stops the effects that do not persist
/// (capsule/save/save.h). An effect with an expiry message gets an implicit script that shows it through the
/// script port; the overworld runner (capsule/overworld/overworld.h) counts the walker's steps and the minute clock
/// (capsule/core/clock.h clock::minutes()) and runs that script when an effect ends.
///
/// Who reads them: scripts (`field_effect active NAME -> var(v);`, `field_effect remaining NAME -> var(v);`),
/// effect logic (`field.effect_active(NAME)`, `field.effect_remaining(NAME)`, e.g. in an `encounter` hook) and
/// game code (these functions). Effect indices follow the structure's list (battle_ids.h FIELD_EFFECT_<NAME>).
///
/// A program without a script bank that declares the effects (no scripts, or a scripts manifest without the
/// game's battle) has none: count() is 0 and start() refuses. Programs with the battle modules but without the
/// engine library link src/script/field_effects_none.cpp instead.
#pragma once

#include "capsule/script/data.h"

#include <cstdint>

namespace capsule::script::field_effects
{

/// The effects the bank declares.
/// @return 0 to kMaxFieldEffects
uint8_t count();

/// One effect's record.
/// @param effect  index (the structure's order)
/// @return it, or nullptr past count()
const FieldEffectInfo* info(uint8_t effect);

/// Starts an effect by its stack rule: Refresh sets what it has left to @p amount, Add adds @p amount (up to its
/// variable's maximum), Refuse refuses while it runs. An effect that does not run starts with @p amount.
/// @param effect  index
/// @param amount  steps or minutes, 1 or more (clamped to its variable's range)
/// @return false when refused (it runs and refuses, the amount is below 1, or there is no such effect)
bool start(uint8_t effect, int32_t amount);

/// Whether an effect runs.
/// @param effect  index
/// @return true while it has steps or minutes left
bool active(uint8_t effect);

/// What an effect has left.
/// @param effect  index
/// @return steps or minutes (0: it does not run, or there is no such effect)
int32_t remaining(uint8_t effect);

/// Stops an effect at once, without its expiry message.
/// @param effect  index
void stop(uint8_t effect);

/// Counts the walker's steps: every running Steps effect loses @p steps; one that reaches 0 ends (take_expired()
/// reports it).
/// @param steps  steps walked since the last call
void walked(uint32_t steps);

/// Reads the minute clock: every running Minutes effect loses the minutes since the last call (the first call after
/// reset_clock() only reads the clock); one that reaches 0 ends.
void tick();

/// Forgets the minute clock's last reading (the next tick() reads it again) and every ended effect not reported
/// yet (C8: what was queued belongs to the game before): after a load (save::load()), a scene change
/// (overworld::reset()) or a long pause.
void reset_clock();

/// The next effect that ended and was not reported yet (in index order).
/// @param[out] effect  its index
/// @return false when none waits
bool take_expired(uint8_t* effect);

} // namespace capsule::script::field_effects
