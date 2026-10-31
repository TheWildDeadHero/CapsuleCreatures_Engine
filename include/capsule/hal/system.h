/// @file
/// Platform start-up and frame pacing (P-8).
///
/// The HAL is plain functions with exactly one implementation linked per
/// target (src/hal/gba, src/hal/sdl2, src/hal/dc or src/hal/headless). No
/// vtables or dynamic dispatch.
#pragma once

#include "capsule/hal/input.h"

#include <cstdint>

/// @namespace capsule::hal
/// Hardware abstraction layer (P-8): plain functions, one implementation per
/// target, chosen at link time.

/// Platform lifecycle: start-up, end of frame, assertion failure reporting.
namespace capsule::hal::system
{

/// Brings up the platform (GBA: interrupts, ROM wait states, Timer 1, display
/// in forced blank; desktop: SDL window and the emulated video state;
/// Dreamcast: video mode and the PowerVR), empties the VRAM upload queue and
/// seeds the random streams from entropy() (A-4).
void init();

/// Ends the current frame and waits for the next one (GBA: VBlank; desktop:
/// paced to the GBA frame rate, 2^24 / 280896 Hz, integer math only;
/// Dreamcast: the display's vertical blank).
/// @return false when the platform asks to quit (desktop window closed),
///         true otherwise.
bool update();

/// C11 (D-32 review R8 finding 6): vertical blanks since init(). GBA: counted by the VBlank interrupt, so a lag
/// frame (a frame's work longer than a vertical blank, a blocking save write) counts each blank it spans; desktop,
/// headless and Dreamcast: one per update() (no lag: the frames are paced). update() makes the per-frame RNG
/// advance once for each blank since its last call (capsule/core/random.h frame_advance()), as a reference game
/// whose vertical blank interrupt draws does.
/// @return the count (wraps at 2^32)
uint32_t vblanks();

/// Raw entropy for seeding the random streams (A-4): GBA: Timer 1 (free
/// running at 16.78 MHz from init()) as captured by the keypad interrupt at
/// the first press, and the VBlank count (before any press this is the same
/// every boot: there is nothing else to measure until the RTC driver);
/// desktop:
/// the high-resolution clock and the frame count, or the CAPSULE_SEED
/// environment variable when set (reproducible runs); Dreamcast: the
/// microsecond clock; headless: the frame count (deterministic tests). The
/// real-time clock joins in once the RTC driver exists (C-05).
/// @return a value that differs between sessions
uint32_t entropy();

/// Mixes entropy into the gameplay and cosmetic streams the first time a key
/// is held after start-up (A-4: human timing makes every session unique even
/// without an RTC). Each HAL's update() calls it with the keys it read.
/// @param held  keys held this frame
void seed_on_first_press(input::KeyBits held);

} // namespace capsule::hal::system
