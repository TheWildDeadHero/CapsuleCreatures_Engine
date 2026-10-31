/// @file
/// The sprite slot plan: which module owns which gfx::sprites slots on each
/// kind of screen. One table, so the overworld, the battle screen and the
/// sequences never hand out the same slot twice.
///
/// | Screen                 | Slots  | Owner                                          |
/// |------------------------|--------|------------------------------------------------|
/// | Overworld              | 0      | the walker (field::attach_sprite())            |
/// | Overworld              | 1-15   | map objects (object n uses slot 1 + n; I3: on  |
/// |                        |        | a map with an object view, a free one)         |
/// | Battle                 | 0-5    | battlers, one per format position (the game)   |
/// | Battle                 | 6-13   | presenters (battle::presenter_view)            |
/// | Battle                 | 14-15  | presenter sequences (battle::presenter_view)   |
/// | Full-screen sequence   | 0-15   | the sequence player (seq, the map is hidden)   |
///
/// The modules that use a range check it against their own limits with
/// static_assert (map::kMaxObjects in capsule/field/field.h,
/// battle::kMaxPositions in capsule/battle/presenter_view.h,
/// seq::kMaxSprites in src/seq/sequence_port_hal.cpp).
#pragma once

#include "capsule/gfx/sprites.h"

#include <cstdint>

/// The sprite slot plan (constants only).
namespace capsule::gfx::slots
{

/// @name Overworld
///@{
inline constexpr uint8_t kWalker = 0;        ///< The walker's slot.
inline constexpr uint8_t kFirstObject = 1;   ///< Slot of map object 0 (object n uses kFirstObject + n).
inline constexpr int kObjects = sprites::kSlots - kFirstObject;   ///< Slots for map objects.
///@}

/// @name Battle screen
///@{
inline constexpr uint8_t kFirstBattler = 0;   ///< Slot of format position 0.
inline constexpr int kBattlers = 6;           ///< One per format position.
/// First presenter slot.
inline constexpr uint8_t kFirstPresenter = kFirstBattler + kBattlers;
inline constexpr int kPresenters = 8;         ///< Presenter slots.
/// First slot of the presenter sequences.
inline constexpr uint8_t kFirstPresenterSequence = kFirstPresenter + kPresenters;
/// Slots of the presenter sequences.
inline constexpr int kPresenterSequences = sprites::kSlots - kFirstPresenterSequence;
///@}

/// @name Full-screen sequences (title, cutscenes, credits, location previews)
///@{
inline constexpr uint8_t kFirstSequence = 0;          ///< First slot a full-screen sequence uses.
inline constexpr int kSequences = sprites::kSlots;    ///< Every slot (nothing else is on screen).
///@}

// Each screen's ranges are contiguous, in order and inside the sprite module's slots.
static_assert(kWalker < kFirstObject && kFirstObject + kObjects == sprites::kSlots);
static_assert(kFirstBattler + kBattlers == kFirstPresenter && kFirstPresenter + kPresenters == kFirstPresenterSequence);
static_assert(kPresenterSequences > 0 && kFirstPresenterSequence + kPresenterSequences == sprites::kSlots);
static_assert(kFirstSequence + kSequences == sprites::kSlots);

}   // namespace capsule::gfx::slots
