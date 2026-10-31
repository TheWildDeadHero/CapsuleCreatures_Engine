/// @file
/// Battle formats (C-08a): positions, sides, reach masks and screen layout.
///
/// Part of the battle data (capsule/battle/data.h, which includes every part).
#pragma once

#include "capsule/battle/data/common.h"
#include "capsule/text/encode.h"

namespace capsule::battle
{

/// Format geometry: what happens when a single target fainted before a move.
enum class OnTargetFainted : uint8_t
{
   Redirect,
   Fail,
   NUM_ON_TARGET_FAINTED
};

/// A battle format (ROM, C-08a): positions, sides, reach and screen layout.
struct FormatData
{
   const text::Code* name;          ///< Display name.
   uint8_t positions;               ///< Positions used (<= kMaxPositions).
   uint8_t sides;                   ///< Sides (<= kMaxSides).
   uint8_t side_of[kMaxPositions];  ///< Side of each position.
   uint8_t reach[kMaxPositions];    ///< Positions each position's moves reach (bit masks).
   uint8_t x[kMaxPositions];        ///< Screen x of each position (presentation).
   uint8_t y[kMaxPositions];        ///< Screen y of each position.
   uint8_t stand_x[kMaxSides];      ///< Screen x where each side's presenters stand (presentation).
   uint8_t stand_y[kMaxSides];      ///< Screen y of each side's stand.
   uint8_t party_max;               ///< Members a party brings at most.
   OnTargetFainted on_target_fainted; ///< Single-target rule.
};

} // namespace capsule::battle
