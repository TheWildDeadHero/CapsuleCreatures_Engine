/// @file
/// The overworld runner (D-17): plays the field the way a game does, so a
/// game's overworld scene only feeds it input and draws its own HUD.
///
/// Each frame, update():
/// - advances a warp transition, if one runs (the player waits);
/// - otherwise runs the field script in the main context (the field gets no
///   input meanwhile; field::release() when it ends), or updates the field
///   with the input, then carries out what it requests: a warp (with its
///   transition) or a script (started with its object as `self`), and polls
///   the map's on_frame triggers (field::lock() while one runs);
/// - runs the scripts of Parallel objects in up to kMaxParallel contexts of
///   their own, restarted when they end and stopped when their page changes;
/// - advances the screen effects scripts start (tint, flash, shake, camera
///   pan), the countdown timer (shown on BG0 at the top right) and the
///   location banner;
/// - types text (text::layer::update()).
/// Encounters stay with the game: field::take_encounter() after update().
///
/// Warps and transitions: entering a map whose location (overworld::
/// kLocations) differs from the last one shows its banner after the
/// transition, and, when the location has a preview whose conditions hold
/// (and its rule allows), plays the preview sequence full screen while the
/// map is hidden, between the two halves of the transition. Transitions:
/// fades to black or white, mosaic, wipes and a box (script::Transition).
///
/// DS1 (D-30 section 1): a warp fired from a cell bound to a door
/// (capsule/field/doors.h) first opens the door and walks the player in, then
/// closes it behind the hidden player before the transition; arriving on a
/// door cell shows that door open, hides the player until the transition
/// ends, walks it out the door's exit direction and closes the door.
///
/// This module implements capsule::script::world, the overworld side of the
/// script VM.
#pragma once

#include "capsule/field/field.h"
#include "capsule/script/data.h"

#include <cstdint>

/// The overworld runner.
namespace capsule::overworld
{

inline constexpr int kMaxParallel = 4;          ///< Parallel object scripts running at once.
inline constexpr uint8_t kTransitionFrames = 24; ///< Frames of each half of a transition.

/// Shows the first map: field::enter() with the walker, no transition; the
/// location's banner shows. C8: a darkness level a script set (the bank's
/// map-lifetime `map_darkness`, kept by a save) counts as this map's: a
/// loaded game starts as dark as it was saved.
/// @param map     index into map::kWorld.maps
/// @param x       cell column
/// @param y       cell row
/// @param facing  field::Direction
void start(uint16_t map, int x, int y, uint8_t facing);

/// Runs one frame (see the file comment). Call after hal::system::update()
/// and the pad, before gfx::sprites::update() and map::update().
/// @param intent  the player's input (ignored while a script or transition runs)
void update(const field::Intent& intent);

/// Moves the player to a map (the same one too) with a transition, as a warp
/// event or a script's `warp` does.
/// @param map         index into map::kWorld.maps
/// @param x           cell column
/// @param y           cell row
/// @param facing      field::Direction, or script::kKeepFacing
/// @param transition  script::Transition, or script::kTransitionDefault
void warp(uint16_t map, int x, int y, uint8_t facing, uint8_t transition);

/// Starts a script in the main context, as a field request does (F2: a party menu's field action, the game's
/// own menus): the field locks until it ends.
/// @param script  ScriptId
/// @param self    its object, or field::kNoObject
/// @return false when a script, a transition or a field request is under way (nothing starts)
bool run_script(uint16_t script, uint8_t self);

/// Whether run_script() would start a script now (C9: a caller that writes the script's inputs first, such as the
/// step effects' egg_member, checks this so it never overwrites them while another script still reads them).
/// @return false while a script, a transition or a field request is under way
bool can_run_script();

/// The shown map's darkness level (F2: the map's `darkness`, changed by a script's `darkness N;`).
/// @return 0 (lit) to map::kMaxDarkness
uint8_t darkness();

/// Whether the main field script runs.
/// @return true while it does
bool script_running();

/// Whether a warp transition (or preview) runs.
/// @return true while it does
bool transitioning();

/// Whether a location preview plays.
/// @return true while one does
bool previewing();

/// What the last field request was, for HUDs and tests.
/// @return its source (field::Source::NUM_SOURCES before any)
field::Source last_source();

/// Seconds left on the countdown timer.
/// @return seconds (0 when stopped)
int32_t timer_seconds();

/// Whether the location banner shows (sliding or held).
/// @return true while it does
bool banner_shown();

/// Ends any banner, transition, effect, timer and script (a scene change).
void reset();

} // namespace capsule::overworld
