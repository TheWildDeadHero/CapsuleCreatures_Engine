/// @file
/// Door animations on the shown map (DS1, D-30 section 1): the player of the door assets (capsule/asset/asset.h
/// asset::Door) bound to the map's cells (map::DoorBinding, map::door_at()).
///
/// A door opens or closes through its steps: each step's frame is drawn on the step's first display frame and shows
/// for the step's ticks; the animation ends on the display frame its last step's time runs out (busy() is false
/// from then), and the last frame drawn stays. Frames show as the door says (asset::DoorShow): its tiles copied into
/// the map's tile area (map::load_tiles()) and the cells drawn over (map::set_cell_override()), or its own map
/// entries over the cells; the closed door clears the cells' overrides. field::update() runs update() at its end,
/// once per frame; field::enter() and field::shift_map() stop every door (reset()).
///
/// Up to kMaxRunning doors animate at once; a door started on a cell that already animates replaces it. Sounds:
/// starting an opening or a closing calls game::door_sound() with the door's sound key (a weak default does
/// nothing). The overworld runner's warp sequence uses these doors (capsule/overworld/overworld.h); a game with its
/// own sequence calls them itself.
#pragma once

#include "capsule/asset/asset.h"
#include "capsule/core/types.h"

#include <cstdint>

/// Door animations.
namespace capsule::field::doors
{

inline constexpr int kMaxRunning = 2;   ///< Doors animating at once.

/// The door bound to a cell of the shown map.
/// @param x  its anchor column (the door's bottom-left cell)
/// @param y  its row
/// @return the door, or nullptr (no binding, or its asset is missing)
const asset::Door* at(int x, int y);

/// Starts a door opening.
/// @param x  its anchor column
/// @param y  its row
/// @return false when no door is bound there or kMaxRunning others animate (nothing starts)
bool open(int x, int y);

/// Starts a door closing.
/// @param x  its anchor column
/// @param y  its row
/// @return false when no door is bound there or kMaxRunning others animate
bool close(int x, int y);

/// Shows a door open at once (its opening's last step), stopping its animation.
/// @param x  its anchor column
/// @param y  its row
/// @return false when no door is bound there
bool set_open(int x, int y);

/// Shows a door closed at once (the cells' own metatiles), stopping its animation.
/// @param x  its anchor column
/// @param y  its row
/// @return false when no door is bound there
bool set_closed(int x, int y);

/// Advances every running door one display frame (field::update() calls it).
void update();

/// Whether a door animates.
/// @return true while one does
bool busy();

/// Stops every door (their cells keep what was drawn until map::enter() drops the overrides).
void reset();

}   // namespace capsule::field::doors

/// The game's side of the doors.
namespace capsule::field::game
{

/// A door's sound (the weak default does nothing; a game with audio plays it).
/// @param sound  the door's open_sound or close_sound key (never AssetKey{})
void door_sound(AssetKey sound);

}   // namespace capsule::field::game
