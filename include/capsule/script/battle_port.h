/// @file
/// The script port of battles (D-36 package 2, step C, family 1): the statement
/// `battle party|rematch [-> var(v)];`, `battle unowned <species> <level> [<item>] [-> var(v)];` and
/// `battle scripted <number> [-> var(v)];` (capsule/script/ops.h Op::Battle) start a battle through it and wait for its
/// outcome, and `encounter music;` asks it for a party's encounter music. The game implements it over its own
/// battle set-up; src/script/script_battle_none.cpp is the default (no battles: each one ends at once, won), with weak
/// definitions, so a game's own port wins at link time. The encounter record's read functions are the engine's.
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/script/ops.h"

#include <cstdint>

namespace capsule::script::battles
{

/// encounter_music(): the party has no encounter music.
inline constexpr int32_t kNoMusic = -1;
/// A side of the encounter record without a party.
inline constexpr uint16_t kNoParty = 0;

/// One side of a party battle (the encounter record's side).
struct Side
{
   uint16_t party;     ///< The game's party number (kNoParty: an empty side).
   uint8_t object;     ///< Its map object's local ID, or kNoSelf.
};

/// What the Battle op asks for.
struct Request
{
   BattleKind kind;   ///< The kind of battle.
   uint8_t bits;      ///< Party / Rematch: the encounter record's mode bits (kEncounterPair ...).
   Side a;            ///< Party / Rematch: side A.
   Side b;            ///< Party / Rematch: side B (two parties when it has one).
   Side c;            ///< Party / Rematch: side C (C1b: a spotter group's third position).
   int32_t species;   ///< Unowned: the species.
   int32_t level;     ///< Unowned: the level.
   int32_t item;      ///< Unowned: the held item (0: none).
   int32_t number;    ///< Scripted: the game's number for the battle.
};

/// Starts a battle.
/// @param r  what to start
/// @return true when it runs (busy() then reports it), false when none could start (it counts as won at once)
bool start(const Request& r);

/// Whether the battle started by start() still runs (the VM asks once per frame while it waits).
/// @return true while it runs
bool busy();

/// The outcome of the last battle, once busy() is false.
/// @return it
battle::Outcome outcome();

/// The encounter music of a party (Encounter Music).
/// @param party  the game's party number
/// @return the music for audio::play(), or kNoMusic
int32_t encounter_music(uint16_t party);

/// @name The encounter record, for a game's own code (the engine's side: src/script/script.cpp)
///@{

/// A side of the encounter record.
/// @param side  kEncounterSideA, kEncounterSideB or kEncounterSideC
/// @return its party and object
Side encounter_side(uint8_t side);

/// The encounter record's current side.
/// @return kEncounterSideA, kEncounterSideB or kEncounterSideC
uint8_t encounter_current_side();

/// The encounter record's mode bits.
/// @return kEncounterPair, kEncounterRematch, kEncounterNoMusic bits
uint8_t encounter_bits();

/// The game's mode number of the last Set.
/// @return it
uint8_t encounter_mode();

///@}

} // namespace capsule::script::battles
