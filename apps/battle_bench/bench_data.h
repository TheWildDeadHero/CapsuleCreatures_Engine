/// @file
/// What the bench knows about the game's battle data besides capsule::battle::kData: the manifest's names (a
/// scenario names species, moves, parties, ... as the manifest does) and the effect bank's debug tables (owner and
/// hook names, the pc -> source line table). Generated with the battle data by `tools/bench_scenario.py emit-data`
/// (cmake/parts/bench.cmake, capsule_battle_bench()); the bank itself carries none of it.
#pragma once

#include "capsule/battle/effect.h"

#include <cstdint>

namespace capsule::bench
{

/// One list of names (species, moves, ...), in the data's index order.
struct NameList
{
   const char* list;            ///< Its key ("species", "moves", ...).
   const char* const* names;    ///< Names (manifest spelling).
   uint16_t count;              ///< Names.
   uint16_t first;              ///< Index of the first name (1 when index 0 is "none": statuses, items).
};

/// The generated tables.
struct BenchData
{
   const char* manifest;                 ///< The manifest's file name.
   const NameList* lists;                ///< Name lists.
   int list_count;                       ///< Lists.
   const char* const* owner_names;       ///< "kind Name" per effect owner (Bank::owners).
   int owner_count;                      ///< Owners.
   const char* const* program_hooks;     ///< Hook or phase name per program ("" for a sub).
   const char* const* program_labels;    ///< The compiler's label per program.
   int program_count;                    ///< Programs.
   const char* const* op_names;          ///< Opcode names (effect::Op order).
   int op_count;                         ///< Opcodes.
   const battle::effect::LineTable* lines;   ///< The pc -> source line table.
};

/// The game's tables (defined by the generated source).
extern const BenchData kBenchData;

} // namespace capsule::bench
