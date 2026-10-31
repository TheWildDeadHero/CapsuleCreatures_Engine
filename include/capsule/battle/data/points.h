/// @file
/// Modifier points (D-20 D): the named points a ruleset declares, where the pipelines run them, and the sources
/// whose handlers they run in order.
///
/// A point handler is an effect::Handler (capsule/battle/effect.h) with kHandlerPoint of a passive's, held item's,
/// status's, weather's, move's, side condition's or volatile's owner; its hook is the point id and its program names
/// the generated function (rules::point_handler()). A side or volatile source runs every condition of its kind the
/// holder has (or its side has), in data order. The point's
/// value goes through every source in the declared order, each source's handlers in their order (every
/// handler sees the value clamped to [0, 65535]), then the point's range clamps it. A point no source has a
/// handler for leaves the value as it is (battle::run_point(), capsule/battle/pipeline.h).
///
/// Part of the battle data (capsule/battle/data.h, which includes every part).
#pragma once

#include "capsule/battle/data/common.h"

namespace capsule::battle
{

/// @name Caps (D-20 D)
///@{
inline constexpr int kMaxPoints = 16;         ///< Points a ruleset declares.
inline constexpr int kMaxPointSources = 8;    ///< Sources of one point.
///@}

/// Where a point's handlers come from (POINT_SOURCES in tools/ruleset_compiler.py, same order).
enum class PointSource : uint8_t
{
   UserPassive,     ///< The user's passive.
   UserItem,        ///< The user's held item.
   UserStatus,      ///< The user's primary status.
   TargetPassive,   ///< The target's passive.
   TargetItem,      ///< The target's held item.
   TargetStatus,    ///< The target's primary status.
   Weather,         ///< The weather (holder: the user).
   Move,            ///< The pipeline's move: its own point handlers (holder: the user; E13b).
   UserSide,        ///< Each side condition on the user's side, in data order (holder: the user; E13b).
   TargetSide,      ///< Each side condition on the target's side (holder: the target; E13b).
   UserVolatile,    ///< Each volatile the user has, in data order (holder: the user; E13b).
   TargetVolatile,  ///< Each volatile the target has (holder: the target; E13b).
   NUM_SOURCES      ///< Number of sources.
};

/// A modifier point (ROM).
struct PointData
{
   int32_t lo;                                ///< The point's result is clamped to [lo, hi]...
   int32_t hi;                                ///< ...when a source has a handler.
   PointSource sources[kMaxPointSources];     ///< Sources, in the order their handlers run.
   uint8_t source_count;                      ///< Sources used.
   uint8_t reserved[kPadAfterOneByte];        ///< Padding; 0.
};
CAPSULE_POD(PointData, 20);

} // namespace capsule::battle
