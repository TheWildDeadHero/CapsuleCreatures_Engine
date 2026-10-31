/// @file
/// Types and the type chart (C-08h, D-22 A6): type entries, the chart's tiers (manifest `chart.tiers`), how the
/// tiers of a defender's types combine (`chart.combine`) and the types a battler carries.
///
/// A matchup (attacking type against one defending type) names a tier. A scaling tier multiplies by its Q8
/// factor; a decisive tier ends the matchup at once (Zero: no effect, Ko: knocks out), the first decisive tier in
/// the defender's type order winning. The scaling tiers of the defender's types combine by BattleData::combine.
///
/// Part of the battle data (capsule/battle/data.h, which includes every part).
#pragma once

#include "capsule/battle/data/common.h"
#include "capsule/text/encode.h"

namespace capsule::battle
{

/// @name Caps (C-08a, REF-17)
///@{

/// Types per battler: the species' types (BattleData::species_types) and the types effects add
/// (BattleData::added_types) together. The effect primitive SET_TYPES takes this many operands.
inline constexpr int kMaxTypesPer = 3;
inline constexpr int kMaxTiers = 8;   ///< Tiers of a type chart (manifest chart.tiers).
///@}

/// What a tier does (TierData::kind).
enum class TierKind : uint8_t
{
   Scale,   ///< Multiplies by TierData::q8 (combined over the defender's types).
   Zero,    ///< Decisive: no effect.
   Ko,      ///< Decisive: knocks out at once.
   NUM_KINDS
};

/// How the scaling tiers of a defender's types combine (BattleData::combine, manifest chart.combine).
enum class Combine : uint8_t
{
   Exponent,     ///< Sum the tiers' exponents (every scaling tier is 2^exp; the pipelines' type_exp, pow2()).
   Multiply,     ///< Multiply the tiers' Q8 factors into one factor (truncating each step), applied once.
   Sequential,   ///< Apply each tier's factor to the value in turn, truncating after each (type_scale()).
   NUM_MODES
};

/// One tier of the type chart (ROM). Manifest `{name, q8 | decisive: zero | ko, message}`; the name is the
/// manifest's only.
struct TierData
{
   uint16_t q8;       ///< Factor of a scaling tier (Q8: 256 = x1); 0 for a decisive one.
   int8_t exp;        ///< Exponent of a scaling tier whose factor is 2^exp (combine Exponent); 0 otherwise.
   uint8_t kind;      ///< TierKind.
   uint8_t message;   ///< Msg said when this decisive tier applies, or kNone8 (scaling tiers: kNone8).
   uint8_t reserved;  ///< Padding; 0.
};
CAPSULE_POD(TierData, 6);

/// A type (ROM).
struct TypeData
{
   const text::Code* name;   ///< Display name.
   uint8_t color;            ///< Presentation color index.
   uint8_t reserved[kPadAfterOneByte];      ///< Padding; 0.
};

} // namespace capsule::battle
