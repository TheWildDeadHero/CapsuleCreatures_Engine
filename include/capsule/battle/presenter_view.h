/// @file
/// Presenter view (optional presentation helper, D-20 B): plays the
/// presenter cues a battle queues (EventKind::Presenter) with the sprite
/// module, the video blend and the sequence player.
///
/// The battle itself never draws: it only says "presenter P of party T
/// enters / leaves / comes back". A game may present that any way it likes;
/// this helper is the engine's ready-made way, used by sandbox_battle:
///
///     presenter_view::reset();                       // battle screen set up
///     ...
///     while (presenter_view::busy()) { presenter_view::frame(); wait a frame; }
///     if (pop_event(s, e)) { presenter_view::begin(s, e); ... }
///
/// Sprite slots (gfx::sprites): 0-5 are the caller's battlers, 6-13 the
/// presenters (party t's presenter slot k uses 6 + t * kMaxPresenters + k
/// while that is below 14; later ones are text only), 14-15 the presenter
/// sequences (the battle compiler only lets them use those slots). One
/// sequence plays at a time (capsule/gfx/slots.h has the whole plan). A
/// slide or fade moves the presenter's own slot; a fade blends it with the
/// video blend (alpha, sprites over every layer) and so uses the blend while
/// it runs: the view counts its fading presenters and turns the blend off
/// when the last one ends (and in reset()). Positions: the format's
/// stand of the party's side (FormatData::stand_x / stand_y) plus the
/// presenter's offset is the sprite's centre.
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/gfx/slots.h"

#include <cstdint>

/// The presenter view (one per program: it owns sprite slots 6-15 during a battle).
namespace capsule::battle::presenter_view
{

/// @name Sprite slots
///@{
inline constexpr uint8_t kFirstPresenterSlot = gfx::slots::kFirstPresenter;   ///< First presenter sprite slot (6).
inline constexpr int kPresenterSlots = gfx::slots::kPresenters;               ///< Presenter sprite slots (6-13).
/// First slot presenter sequences use (14).
inline constexpr uint8_t kFirstSequenceSlot = gfx::slots::kFirstPresenterSequence;
///@}

// The battlers' slots hold one sprite per format position.
static_assert(gfx::slots::kBattlers == kMaxPositions, "capsule/gfx/slots.h: one battler slot per position");

/// Hides every presenter and stops any motion and sequence. Call when a
/// battle screen is set up.
void reset();

/// Starts showing an event: a Presenter cue begins its entrance, exit or
/// return; every other event is ignored.
/// @param s  the battle that queued it
/// @param e  the event
void begin(const State& s, const Event& e);

/// Whether a cue is still playing (the caller holds the next event back).
/// @return true while a slide, fade or sequence runs
bool busy();

/// Advances the running cues one frame (call once per frame, before
/// gfx::sprites::update()).
void frame();

} // namespace capsule::battle::presenter_view
