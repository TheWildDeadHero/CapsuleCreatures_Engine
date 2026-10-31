/// @file
/// Battle data basics (C-08, P-1): the battle-wide caps, the "none" index values, stage ratios and
/// odds (P-5) every other battle data header uses.
///
/// Part of the battle data (capsule/battle/data.h, which includes every part).
#pragma once

#include "capsule/core/pod.h"
#include "capsule/core/types.h"

#include <cstdint>

namespace capsule::battle
{

/// @name Caps (C-08a, REF-17)
///@{
inline constexpr int kMaxParties = 6;    ///< Parties per battle.
inline constexpr int kMaxBattlers = 24;   ///< Battler slots per battle (all parties).
inline constexpr int kMaxPositions = 6;    ///< Field positions per format.
inline constexpr int kMaxSides = 4;    ///< Sides per format.
///@}

/// "None" for 8-bit indices (type, passive, status, weather, presenter, stat).
inline constexpr uint8_t kNone8 = 0xFF;
/// "None" for 16-bit indices (move, species, item).
inline constexpr uint16_t kNone16 = 0xFFFF;

/// Stage multiplier (num / den) for one stage value.
struct Ratio
{
   uint8_t num;   ///< Numerator.
   uint8_t den;   ///< Denominator.
};
CAPSULE_POD(Ratio, 2);

/// Odds (P-5).
struct Odds
{
   uint16_t n;   ///< Successes.
   uint16_t m;   ///< Outcomes.
};
CAPSULE_POD(Odds, 4);

} // namespace capsule::battle
