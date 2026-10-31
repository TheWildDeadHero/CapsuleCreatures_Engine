/// @file
/// The nursery (D-28 section 2, package B2): a few slots where records wait while the structure's nursery method (a
/// combination method of kind Nursery, capsule/combine/data.h NurseryData) checks them every N steps.
///
/// - deposit() takes a party member into a free slot (refused: an egg, unless the method allows eggs; the party's
///   last member that is not an egg; a full nursery). It remembers the member's level; while it is held it gains
///   the nursery_gain pipeline's experience per step (when the structure's gain is on) through walked().
/// - withdraw() gives it back to the party's end with its experience, the levels it gained (the moves of those
///   levels learned: a free slot, else the first move forgotten) and full resources; fee() is what the
///   nursery_fee pipeline charges for it (the script statement takes it from a money variable).
/// - check() is one run of the structure's check step effect: with the slots holding the method's inputs and no
///   result waiting, it draws the method's odds in NurseryData::odds_scale; a hit builds the result through a
///   combine::Combination over the held records (slot orders tried until one fits the method's slot requirements;
///   the moves a player would pick are picked at random: nothing asks while walking), makes it an egg when the method
///   delivers eggs and keeps it (consumed inputs leave their slots) until take_result() puts it into the party.
///   C9: a nursery holding more records than the method takes checks its subsets: the largest the method takes
///   first, each subset in slot order first, then its other orders; the first that fits is the one odds() shows and
///   check() draws for (the tries are bounded by every arrangement of kMaxInputs of kMaxNurserySlots; no heap).
/// - C9: an egg left there (a method that allows eggs) gains no experience.
///
/// The State is the save image's Nursery section (capsule/save/save.h: the C7 shape rules: the record hash, slots
/// kept by index, moved when the build has fewer, refused for more records than slots). It lives in EWRAM on the
/// GBA: kMaxNurserySlots + 1 records.
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/combine/combine.h"
#include "capsule/combine/data.h"
#include "capsule/core/random.h"
#include "capsule/monster/monster.h"

#include <cstdint>

namespace capsule::combine::nursery
{

/// What a nursery call says (tools/battlec/nursery.py RESULTS; the script statements' variable).
enum class Result : uint8_t
{
   Ok,          ///< Done.
   NoNursery,   ///< The build has no nursery.
   Full,        ///< No free slot.
   Empty,       ///< The slot holds nothing (or no result waits).
   NoRoom,      ///< The party has no room.
   Refused,     ///< An egg the method does not take, or the party's last member that is not an egg.
   BadSlot,     ///< No such slot or member.
   NUM_RESULTS  ///< Number of results.
};

/// The nursery's contents (the save image's Nursery section).
struct State
{
   monster::Record records[kMaxNurserySlots];   ///< The held records.
   uint32_t gained[kMaxNurserySlots];           ///< Experience each gained since it was left.
   uint8_t level[kMaxNurserySlots];             ///< Its level when it was left.
   uint8_t used;                                ///< Bit per slot holding a record.
   uint8_t pending;                             ///< 1: a result waits.
   uint8_t reserved[kPadAfterTwoBytes];         ///< 0.
   monster::Record result;                      ///< The result waiting.
};

/// The nursery's state (zero at start-up: empty).
/// @return it
State& state();

/// Empties the nursery (a new game).
void clear();

/// Whether the build has a nursery.
/// @return true when the structure declares one
bool exists();

/// Its slots.
/// @return 0 to kMaxNurserySlots
uint8_t slot_count();

/// The records it holds.
/// @return 0 to slot_count()
uint8_t count();

/// A held record.
/// @param slot  nursery slot
/// @return it, or nullptr for a free slot
const monster::Record* record(uint8_t slot);

/// Takes a party member into the first free slot.
/// @param party   the party (the member leaves it; the others move up)
/// @param member  party slot
/// @param[out] slot  the nursery slot it went to (unchanged when refused), or nullptr
/// @return Ok, NoNursery, Full, Refused or BadSlot
Result deposit(monster::Party& party, uint8_t member, uint8_t* slot = nullptr);

/// The level a held record has now (its experience with what it gained).
/// @param slot  nursery slot
/// @return the level (0 for a free slot)
uint8_t level_now(uint8_t slot);

/// The levels a held record gained since it was left.
/// @param slot  nursery slot
/// @return levels (0 for a free slot)
uint8_t levels_gained(uint8_t slot);

/// What taking a held record back costs (the nursery_fee pipeline; 0 without the structure's fee).
/// @param slot  nursery slot
/// @return the fee
int32_t fee(uint8_t slot);

/// Gives a held record back to the party's end (its experience, levels, the moves of those levels, full resources).
/// @param party  the party
/// @param slot   nursery slot
/// @return Ok, NoNursery, Empty, NoRoom or BadSlot
Result withdraw(monster::Party& party, uint8_t slot);

/// The method's odds over the held records (no draw; the first subset and slot order that fits, C9).
/// @return the odds (0: incompatible, or not enough records)
int32_t odds();

/// Whether a result waits.
/// @return true when check() made one that take_result() did not take yet
bool result_ready();

/// The result waiting.
/// @return it, or nullptr
const monster::Record* result();

/// Puts the waiting result at the party's end.
/// @param party  the party
/// @return Ok, NoNursery, Empty or NoRoom
Result take_result(monster::Party& party);

/// Counts the walker's steps into the held records' experience (nothing without the structure's gain; C9: nothing
/// for an egg).
/// @param steps  steps walked
void walked(uint32_t steps);

/// One check (the check step effect ran): see the file comment.
/// @param lent   a State lent for the combination's field context (not a battle in progress)
/// @param party  the player's party (nothing is placed into it)
/// @param rng    the draws' stream
/// @return true when a result was made
bool check(battle::State& lent, monster::Party& party, rng::Stream& rng);

} // namespace capsule::combine::nursery

namespace capsule::battle
{

/// Gives a record experience and the levels it reaches (B2: a nursery's withdraw; src/battle/combine_battle.cpp):
/// the moves of the new levels are learned (a free slot, else the first move forgotten) and its resources filled.
/// @param rec     the record
/// @param amount  experience
/// @return its level after
uint8_t grow_record(monster::Record& rec, uint32_t amount);

/// The level a record would have with more experience (no change).
/// @param rec     the record
/// @param amount  experience added
/// @return the level
uint8_t level_with(const monster::Record& rec, uint32_t amount);

} // namespace capsule::battle
