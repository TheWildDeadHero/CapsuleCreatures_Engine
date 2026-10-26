/// @file
/// Encounter tables (C-03c, C-03d, C-03e, D-12): the ROM layout the map
/// compiler (tools/encounter_compiler.py) emits next to the world.
///
/// Every map assigns its encounter cells to one of kMaxSlots **slots** (the
/// 4-bit slot field of map::MapCell). A map lists, per slot and per
/// **method**, the table that slot uses: a method is a data-named way of
/// meeting creatures (walking in grass, crossing water, each tool game code
/// uses on a cell, ...). Methods triggered by steps are rolled by the field
/// module when the walker arrives on an encounter cell; the others are
/// rolled by game code (scripts, natives) through encounter::roll_at().
///
/// Tables live once in a global bank: identical tables (shared tables
/// referenced by several maps, or equal inline tables) are stored once.
/// A table is a weighted pool (C-03d): each entry has a base weight, the
/// condition bits it requires and excludes, and multipliers that apply
/// while given conditions are active. Condition bits are set by game code
/// (time of day, weather, swarms, ...); their names come from the world
/// manifest (P-3).
#pragma once

#include "capsule/core/pod.h"

#include <cstdint>

/// Encounters.
namespace capsule::encounter
{

inline constexpr int kMaxSlots = 16;              ///< Encounter slots per map (4-bit cell field).
inline constexpr int kMaxMethods = 16;            ///< Methods per world.
inline constexpr int kMaxConditions = 32;         ///< Condition bits per world.
inline constexpr int kMaxEntriesPerTable = 64;    ///< Entries per table (storage bound, not a species cap).
inline constexpr int kMaxModifiersPerEntry = 8;   ///< Weight modifiers per entry.
inline constexpr uint16_t kNoTable = 0xFFFF;      ///< No table.

/// What starts a method's roll.
enum class Trigger : uint8_t
{
   Step,          ///< Arriving on an encounter cell (field module), for the method's vehicle classes.
   Script,        ///< Game code calls roll_at() (a tool used on a cell, an interaction, ...).
   NUM_TRIGGERS   ///< Number of triggers.
};

/// One encounter method (ROM).
struct Method
{
   Trigger trigger;      ///< What rolls it.
   uint8_t vehicles;     ///< Step methods: vehicle classes it applies to (bit n = class n).
   uint8_t reserved[kPadAfterTwoBytes];  ///< 0.
};
CAPSULE_POD(Method, 4);

/// A weight multiplier while a condition is active (ROM).
struct WeightModifier
{
   uint8_t condition;        ///< Condition bit, 0-31.
   uint8_t reserved;         ///< 0.
   uint16_t multiplier_q8;   ///< Q8 (256 = x1, 768 = x3, 0 = suppressed).
};
CAPSULE_POD(WeightModifier, 4);

/// One weighted entry of a table (ROM).
struct Entry
{
   uint32_t required;         ///< Condition bits that must all be active.
   uint32_t excluded;         ///< Condition bits that must all be inactive.
   uint16_t species;          ///< Battle species index (battle_ids.h SPECIES_...).
   uint16_t weight;           ///< Base weight, 1-65535.
   uint16_t modifier_first;   ///< First of its modifiers in Bank::modifiers.
   uint8_t modifier_count;    ///< Modifiers, 0 to kMaxModifiersPerEntry.
   uint8_t min_level;         ///< Lowest level.
   uint8_t max_level;         ///< Highest level (>= min_level).
   uint8_t reserved[kPadAfterOneByte];       ///< 0.
};
CAPSULE_POD(Entry, 20);

/// A table: a run of entries plus its trigger odds (ROM).
struct Table
{
   uint16_t entry_first;   ///< First entry in Bank::entries.
   uint16_t entry_count;   ///< Entries, 0 (rolls nothing) to kMaxEntriesPerTable.
   uint16_t rate_n;        ///< Odds that a trigger starts an encounter: rate_n in rate_m (P-5).
   uint16_t rate_m;        ///< 1-65535.
};
CAPSULE_POD(Table, 8);

/// Which table a map's slot uses for a method (ROM).
struct Ref
{
   uint8_t slot;     ///< 0 to kMaxSlots - 1.
   uint8_t method;   ///< Index into Bank::methods.
   uint16_t table;   ///< Index into Bank::tables.
};
CAPSULE_POD(Ref, 4);

/// Every encounter table of the build (ROM). Generated with the world.
struct Bank
{
   const Method* methods;                ///< method_count methods.
   const Table* tables;                  ///< table_count tables.
   const Entry* entries;                 ///< entry_count entries.
   const WeightModifier* modifiers;      ///< modifier_count modifiers.
   const char* const* method_names;      ///< Debug names, or nullptr.
   const char* const* condition_names;   ///< Debug names, or nullptr.
   const char* const* species_names;     ///< Debug names by species index, or nullptr.
   uint16_t table_count;                 ///< Tables.
   uint16_t entry_count;                 ///< Entries.
   uint16_t modifier_count;              ///< Modifiers.
   uint16_t species_name_count;          ///< Entries of species_names.
   uint8_t method_count;                 ///< Methods.
   uint8_t condition_count;              ///< Condition names.
};

} // namespace capsule::encounter
