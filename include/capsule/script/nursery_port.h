/// @file
/// The script port of the nursery and of eggs (D-28 sections 2 and 3, package B2): the statements
/// `nursery pick -> var(v);` (the party menu), `nursery deposit <member> -> var(v);`,
/// `nursery withdraw <slot> [pay var(money)] -> var(v);`, the queries `nursery count|ready|odds -> var(v);` and
/// `nursery levels|fee <slot> -> var(v);`, `nursery take -> var(v);` and the nickname prompt
/// `nursery name <member> -> var(v);` (capsule/script/data.h Op::Nursery). The engine's defaults
/// (src/script/script_nursery.cpp) work on combine::nursery and the party given to attach(): the party menu and
/// the nickname prompt (a yes / no question, then a letter menu: C9: its first row the name so far, "Name: AB",
/// which ends the name, then Del and the letters) go through the text box's paged menu (script::menu). A game
/// replaces the file's functions by defining them (link time). A program without battle data, or (C9) a game without
/// eggs, links src/script/script_nursery_none.cpp (no nursery: every statement answers 0; weak: a game's own port
/// wins).
#pragma once

#include "capsule/monster/monster.h"
#include "capsule/script/data.h"

#include <cstdint>

namespace capsule::script::nursery
{

/// poll(): still running (a menu, a question).
inline constexpr int16_t kNurseryRunning = -1;

/// Gives the defaults the party (a game calls it once it exists).
/// @param party  the player's party, or nullptr (every statement answers 0)
void attach(monster::Party* party);

/// Opens the party menu (Pick).
void pick_open();

/// Opens the nickname prompt for a party member (Name).
/// @param member  party slot
void name_open(uint8_t member);

/// Polls the running Pick or Name (the VM calls it once per frame while it waits).
/// @return kNurseryRunning, else the statement's value
int16_t poll();

/// The statements that answer at once (Deposit, Count, Levels, Fee, Ready, Odds, Take).
/// @param kind  the statement
/// @param arg   its operand (a party member or a nursery slot)
/// @return its value
int32_t query(NurseryOp kind, int32_t arg);

/// Withdraw: a slot's record back to the party when the money covers its fee.
/// @param slot   nursery slot
/// @param money  the money variable's value (pays the fee), or a negative value: no fee is taken
/// @param[out] paid  the fee taken (0 unless it went)
/// @return 1 done, 0 refused (empty slot, no room), kNurseryPoor (not enough money)
int32_t withdraw(int32_t slot, int32_t money, int32_t& paid);

} // namespace capsule::script::nursery
