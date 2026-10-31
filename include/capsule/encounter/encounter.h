/// @file
/// Encounter rolls (C-03d, D-12): which table a cell uses, the trigger
/// odds, the weighted pick and the level.
///
/// The tables come from map::kWorld.encounters (see data.h). Game code
/// keeps the active condition bits up to date with set_conditions() (time
/// of day, weather, swarms, ...); a roll only offers entries whose required
/// bits are all active and whose excluded bits are all inactive, with each
/// active modifier's multiplier applied to the weight (truncating, like the
/// originals). A table whose entries all drop out gives no encounter.
///
/// All randomness comes from the stream the caller passes (normally
/// rng::gameplay, A-4), so recorded play replays identically.
///
/// **Rules (D-27 section 1, package F1).** A table's roll (its trigger odds,
/// then the pick) is roll_plain() unless a game attached rules with
/// set_rules(): the battle side's lead rules (capsule/battle/encounter_lead.h:
/// the party's lead, its passive's and held item's `encounter` hooks, the
/// ruleset's encounter_rate / encounter_level pipelines) replace it while
/// their scope lives. The tables, methods and conditions stay as they are:
/// rules layer on top of them through choose() (a pick restricted to some
/// entries) and the public helpers below.
#pragma once

#include "capsule/core/random.h"
#include "capsule/encounter/data.h"
#include "capsule/map/map.h"

#include <cstdint>

namespace capsule::encounter
{

/// A creature to battle (an encounter rolled).
struct Result
{
   uint16_t species;   ///< Battle species index.
   uint8_t level;      ///< Level.
   uint8_t method;     ///< Method that rolled it.
   uint16_t table;     ///< Bank table index.
   uint16_t entry;     ///< Bank entry index.
};
CAPSULE_POD(Result, 8);

/// Bits per word of a KeepMask.
inline constexpr int kKeepWordBits = 32;
/// Words of a KeepMask (a bit per entry of a table).
inline constexpr int kKeepWords = kMaxEntriesPerTable / kKeepWordBits;
static_assert(kMaxEntriesPerTable % kKeepWordBits == 0, "a keep mask holds whole 32-bit words");

/// Which entries of a table a pick may choose (bit n = the table's entry n).
struct KeepMask
{
   uint32_t bits[kKeepWords];   ///< Bit n % kKeepWordBits of word n / kKeepWordBits: entry n may be chosen.
};

/// Whether a keep mask lets entry @p n be chosen.
/// @param k  the mask
/// @param n  entry of the table (0 to kMaxEntriesPerTable - 1)
/// @return true when its bit is set
constexpr bool keeps(const KeepMask& k, int n)
{
   return ((k.bits[n / kKeepWordBits] >> (n % kKeepWordBits)) & 1u) != 0;
}

/// Sets entry @p n's bit of a keep mask.
/// @param k  the mask
/// @param n  entry of the table (0 to kMaxEntriesPerTable - 1)
constexpr void keep(KeepMask& k, int n)
{
   k.bits[n / kKeepWordBits] |= 1u << (n % kKeepWordBits);
}

/// A game's rules for one table's roll (the trigger odds, then the pick): set_rules().
/// @param table   bank table index
/// @param method  method index (stored in the result)
/// @param r       random stream
/// @param out     the encounter
/// @return true when an encounter starts
using RollRules = bool (*)(uint16_t table, uint8_t method, rng::Stream& r, Result* out);

/// The build's encounter bank.
/// @return the bank set_bank() gave, else map::kWorld.encounters, or an empty bank
const Bank& bank();

/// Uses another bank than the world's (tests and tools; the world's maps still name the tables by index).
/// @param b  the bank, or nullptr for the world's again
void set_bank(const Bank* b);

/// Attaches a game's rules to every table roll (roll_step(), roll_at()); nullptr: roll_plain() again.
/// @param rules  the rules (capsule/battle/encounter_lead.h's LeadScope attaches the lead rules)
void set_rules(RollRules rules);

/// The attached rules.
/// @return them, or nullptr for roll_plain()
RollRules rules();

/// Sets the active condition bits.
/// @param bits  bit n = condition n of the world's encounter_conditions
void set_conditions(uint32_t bits);

/// The active condition bits.
/// @return bits
uint32_t conditions();

/// The table a map uses for a slot and method.
/// @param m       the map
/// @param slot    0 to kMaxSlots - 1
/// @param method  method index
/// @return bank table index, or kNoTable
uint16_t table_for(const map::Map& m, uint8_t slot, uint8_t method);

/// The weight an entry has under some condition bits.
/// @param e       the entry
/// @param active  condition bits
/// @return 0 when filtered out, else the base weight times every active modifier
uint32_t entry_weight(const Entry& e, uint32_t active);

/// Chooses an entry of a table by weight (no trigger odds, no level): one 32-bit draw (two 16-bit draws).
/// @param table   bank table index
/// @param active  condition bits
/// @param allowed  the entries it may choose, or nullptr for every entry
/// @param r        random stream
/// @param[out] entry  the table's entry index (0 to its entry_count - 1)
/// @return false when no entry it may choose has a weight under @p active (no draw then)
bool choose(uint16_t table, uint32_t active, const KeepMask* allowed, rng::Stream& r, uint16_t* entry);

/// Picks an entry and level from a table (no trigger odds): choose(), then the level between the entry's lowest
/// and highest.
/// @param table   bank table index
/// @param active  condition bits
/// @param r       random stream
/// @param[out] out  the pick (method left 0)
/// @return false when no entry is valid under @p active
bool pick(uint16_t table, uint32_t active, rng::Stream& r, Result* out);

/// One table's roll without rules: its trigger odds (rate_n in rate_m), then pick() under the active conditions.
/// @param table   bank table index
/// @param method  method index, stored in the result
/// @param r       random stream
/// @param[out] out  the encounter
/// @return true when an encounter starts
bool roll_plain(uint16_t table, uint8_t method, rng::Stream& r, Result* out);

/// Rolls the step methods of a cell the walker just arrived on: for the
/// first step method whose vehicle classes include @p vehicle and for which
/// the map has a table in the cell's slot, the table's roll (the attached
/// rules, or roll_plain()). Cells without the encounter flag never roll.
/// @param m        the map
/// @param c        the cell arrived on
/// @param vehicle  the walker's vehicle class
/// @param r        random stream
/// @param[out] out  the encounter
/// @return true when an encounter starts
bool roll_step(const map::Map& m, map::MapCell c, uint8_t vehicle, rng::Stream& r, Result* out);

/// Rolls a method for a cell from game code (a tool used on the faced
/// cell, an object the walker interacts with, ...): the map's table for the cell's slot and
/// @p method, then its roll (the attached rules, or roll_plain()). The cell needs the encounter
/// flag.
/// @param m       the map
/// @param x       cell column
/// @param y       cell row
/// @param method  method index (any trigger)
/// @param r       random stream
/// @param[out] out  the encounter
/// @return true when an encounter starts
bool roll_at(const map::Map& m, int x, int y, uint8_t method, rng::Stream& r, Result* out);

} // namespace capsule::encounter
