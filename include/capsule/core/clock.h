/// @file
/// The game's time of day (D-26 decision 3, package G2): a context value the game supplies through a port
/// function, so carts without a real-time clock work (a game variable, an RTC, the host clock: the game decides).
/// What the values mean (morning, day, night, ...) is the game's data; transformation rules of a later package
/// (G4) and scripts (`time_of_day -> var(v);`) read the number. The engine's default
/// (src/core/clock_default.cpp) always answers 0; a game replaces it by defining clock::time_of_day() (link
/// time, like the random generator's default).
///
/// F1 (D-27 section 2): minutes() is the clock timed field effects counted in minutes run down by (a monotonic
/// count the game supplies: an RTC, a play-time counter, ...). Its default (src/core/clock_minutes_default.cpp, a
/// file of its own so a game may replace either function alone) never moves, so such effects only end when a game
/// supplies its clock.
#pragma once

#include <cstdint>

namespace capsule::clock
{

/// The time of day now, as the game counts it.
/// @return the game's period index (default: 0)
uint8_t time_of_day();

/// The game's clock in minutes: any monotonic count (it may wrap at 2^32; differences are what count).
/// @return minutes (default: always 0)
uint32_t minutes();

} // namespace capsule::clock
