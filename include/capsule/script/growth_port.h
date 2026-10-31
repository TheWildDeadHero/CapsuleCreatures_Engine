/// @file
/// The script port of growth (D-26 section 4, package G4): the script statement `transform(<slot>) -> var(v);`
/// (trigger script) and the presentation callbacks a transformation asks through. The engine's defaults run a
/// growth::Change over the State and party given to attach() (src/script/script_growth.cpp) and present it with
/// the text box's choice menu (src/script/script_growth_ask.cpp; the learn question is the items port's
/// learn_open() / learn_poll()); a game replaces any file's functions by defining them (link time). A program
/// without battle data links src/script/script_growth_none.cpp (nothing ever transforms).
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/monster/monster.h"

#include <cstdint>

namespace capsule::script::growth
{

/// @name transform_poll() results (the statement's variable)
///@{
inline constexpr int16_t kTransformRunning = -1;   ///< Still running (presentation, a question).
inline constexpr int16_t kTransformNone = 0;       ///< No rule held (or nothing attached).
inline constexpr int16_t kTransformChanged = 1;    ///< The member transformed or changed form.
inline constexpr int16_t kTransformStopped = 2;    ///< The player stopped it.
///@}

/// @name ask_poll() answers
///@{
inline constexpr int8_t kAskWaiting = -1;   ///< No answer yet.
inline constexpr int8_t kAskStop = 0;       ///< Stop it.
inline constexpr int8_t kAskGo = 1;         ///< Let it happen.
///@}

/// Gives the defaults the State they lend to a Change (no battle may run in it meanwhile) and the party (a game
/// calls it once both exist).
/// @param lent   the State, or nullptr (every transform answers kTransformNone)
/// @param party  the party, or nullptr
void attach(battle::State* lent, monster::Party* party);

/// Starts a script's transformation of a party member (the script trigger's rules).
/// @param slot  party slot
void transform_open(uint8_t slot);

/// Polls it (the VM calls it once per frame while the statement waits).
/// @return kTransformRunning, else the result
int16_t transform_poll();

/// Shows that a party member is about to transform and asks whether to go on when it may be stopped (the
/// presentation callback of a Change in the Asking stage).
/// @param slot         party slot
/// @param from         its species
/// @param into         the species it becomes
/// @param cancellable  whether the player may stop it (transformation.cancel)
void ask_open(uint8_t slot, uint16_t from, uint16_t into, bool cancellable);

/// Polls the question.
/// @return kAskWaiting, kAskStop or kAskGo (never kAskStop when it was not cancellable)
int8_t ask_poll();

} // namespace capsule::script::growth
