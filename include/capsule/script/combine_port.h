/// @file
/// The script port of combination (D-28 section 1, package B1): the statements `combine open NAME -> var(v);`,
/// `combine run NAME (<a>, <b>, ...) -> var(v);` and the queries `combine preview|status|fee NAME (<a>, <b>, ...)
/// -> var(v);` (capsule/script/data.h Op::Combine). The engine's defaults run a combine::Combination over the
/// State, party, boxes and stream given to attach() (src/script/script_combine.cpp): the inputs are picked from the
/// party through the text box's paged menu (script::menu), the result asked about ("A + B -> C?" with yes / no),
/// the moves picked through the menu when the method's chooser is the player's (random when the menus are off:
/// set_menus(false)), and the State's queued texts shown in the text box. A game replaces the file's functions by
/// defining them (link time). A program without battle data links src/script/script_combine_none.cpp (nothing
/// combines).
///
/// C9: `combine open` asks from the preview and draws only after yes (a random pick's result reads "?", an egg
/// delivery the eggs' name; egg inputs show the eggs' name): declining changes nothing, the stream included, so the
/// player cannot reroll a random pick by declining and opening again. The Fee query of a random pick is the highest
/// fee over its candidates (combine::Preview::fee).
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/core/random.h"
#include "capsule/monster/monster.h"
#include "capsule/script/data.h"
#include "capsule/storage/storage.h"

#include <cstdint>

namespace capsule::script::combine
{

/// @name poll() results (the statement's variable)
///@{
inline constexpr int16_t kCombineRunning = -1;   ///< Still running (a menu, a question, texts).
inline constexpr int16_t kCombineBack = 0;       ///< The player backed out (or nothing is attached).
inline constexpr int16_t kCombineDone = 1;       ///< Combined.
inline constexpr int16_t kCombineRefused = 2;    ///< The method refused (or there was no room for the result).
///@}

/// Gives the defaults what a combination needs (a game calls it once they exist; no battle may run in the State
/// meanwhile).
/// @param lent   the State lent to the combinations, or nullptr (every statement answers kCombineBack / 0)
/// @param party  the player's party
/// @param boxes  the storage boxes, or nullptr
/// @param rng    the draws' stream (rng::gameplay)
void attach(battle::State* lent, monster::Party* party, storage::Storage* boxes, rng::Stream* rng);

/// Whether the player picks moves through the menu (default true); false: a player chooser picks at random (the
/// fallback of a game without a menu).
/// @param on  menus on
void set_menus(bool on);

/// Starts the menu flow of a method (pick the inputs, preview, ask, then combine: its draws after yes).
/// @param method  method index (battle_ids.h COMBINE_METHOD_<NAME>)
void open(uint8_t method);

/// Starts a combination of given party members (no input menu, no question; the move picks when the chooser is the
/// player's).
/// @param method  method index
/// @param slots   party slots, in input order
/// @param count   how many
void run(uint8_t method, const uint8_t* slots, uint8_t count);

/// Polls the running statement (the VM calls it once per frame while it waits).
/// @return kCombineRunning, else the result
int16_t poll();

/// A query about given party members (no change, no draw).
/// @param kind    Preview, Status or Fee
/// @param method  method index
/// @param slots   party slots, in input order
/// @param count   how many
/// @return Preview: the species + 1 (0: none, kCombineRandom: a random pick); Status: combine::Status; Fee: the fee
int32_t query(CombineOp kind, uint8_t method, const uint8_t* slots, uint8_t count);

} // namespace capsule::script::combine
