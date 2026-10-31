/// @file
/// Field actions, flight points and the escape point (D-27 section 3, package F2): the script bank's side.
///
/// **Field actions** are the structure manifest's `field_actions` (cross water, clear a bush, light a dark cave, ...;
/// tools/battlec/field.py): a requirement over a party member and the game state, a target (a world behavior, an
/// object kind, a map tag, or none for a menu action), whether it asks first, and a script. The script compiler
/// resolves the targets and scripts into the bank's FieldActionInfo records (capsule/script/data.h) and the flags and
/// variables the requirements read into FieldActionRef records, which the battle data's requirements read through
/// rules::field_state() (state()). Which member a requirement holds for is the battle side's
/// (capsule/battle/field_actions.h ActionScope), attached here (attach()) for the `field_action available` statement
/// and set_member() before an action's script runs (its implicit variable `field_action_member`).
///
/// **Flight points** are the world's `flight_points` (a label, a map, a cell, a facing): each has an implicit
/// permanent flag (recorded: the flight menu lists it). Scripts: `flight record NAME;`, `flight visit NAME;`,
/// `flight menu -> var(v);` (the menu is the port's paged choice menu, capsule/script/menu.h).
///
/// **The escape point** is where the walker stood before it last left a map with `escape_point` for one without
/// (the overworld records it with record_escape()); it lives in the bank's implicit permanent variables
/// (EscapeVars), so the save image's Script section keeps it. Scripts: `escape [transition] [-> var(ok)];`.
///
/// A program with the battle modules but without a script bank links src/script/field_actions_none.cpp (no
/// references: every flag and variable reads 0).
#pragma once

#include "capsule/script/data.h"

#include <cstdint>

namespace capsule::script::actions
{

/// available(): no member (no party attached, or the requirement holds for none).
inline constexpr uint8_t kNoMember = 0xFF;

/// The battle side's answer: the first party member an action's requirement holds for.
/// @param action  field action index (battle_ids.h FIELD_ACTION_<NAME>)
/// @return its party slot, or kNoMember
using Available = uint8_t (*)(uint8_t action);

/// The field actions the bank declares.
/// @return 0 to kMaxFieldActions
uint8_t count();

/// One field action's record.
/// @param action  index
/// @return it, or nullptr past count()
const FieldActionInfo* info(uint8_t action);

/// Attaches the battle side's requirement check (capsule/battle/field_actions.h ActionScope); nullptr detaches.
/// @param fn  the check
void attach(Available fn);

/// The first member an action is available to (through the attached check).
/// @param action  index
/// @return its party slot, or kNoMember (none attached, or none qualifies)
uint8_t available(uint8_t action);

/// Writes the member an action's script runs for (the implicit variable `field_action_member`).
/// @param slot  party slot
void set_member(uint8_t slot);

/// A game state value a requirement reads (rules::field_state() calls it).
/// @param ref  reference index (FieldActionRef order)
/// @return the flag (0 / 1) or the variable's value; 0 past the references
int32_t state(int32_t ref);

/// The flight points the bank declares.
/// @return 0 to kMaxFlightPoints
uint8_t flight_count();

/// One flight point's record.
/// @param point  index (script_ids.h FLIGHT_POINT_<NAME>)
/// @return it, or nullptr past flight_count()
const FlightPointInfo* flight_point(uint8_t point);

/// Whether a flight point is recorded.
/// @param point  index
/// @return true when its flag is set
bool recorded(uint8_t point);

/// Records a flight point (sets its flag).
/// @param point  index
void record(uint8_t point);

/// Opens the flight menu: the recorded points' labels, a page at a time (nothing opens without one: the poll
/// answers kNoFlight at once).
void menu_open();

/// menu_poll(): still waiting.
inline constexpr int32_t kFlightWaiting = -1;
/// menu_poll(): backed out, or nothing recorded.
inline constexpr int32_t kNoFlight = -2;

/// The flight menu's answer.
/// @return kFlightWaiting, kNoFlight, or the point picked (its index)
int32_t menu_poll();

/// Records the escape point (the overworld, on a warp that leaves a map with map::kMapEscapePoint for one
/// without; a bank without the escape variables keeps nothing).
/// @param map     the map's asset key (C8: a key, not an index, so a save keeps naming the same map)
/// @param x       cell column
/// @param y       cell row
/// @param facing  facing on arrival (field::Direction)
void record_escape(AssetKey map, int16_t x, int16_t y, uint8_t facing);

/// The recorded escape point.
/// @param[out] map     the map's asset key (map::find() resolves it; a map the world no longer has: none)
/// @param[out] x       cell column
/// @param[out] y       cell row
/// @param[out] facing  facing
/// @return false when none is recorded (or the bank has no escape variables)
bool escape_point(AssetKey& map, int16_t& x, int16_t& y, uint8_t& facing);

} // namespace capsule::script::actions
