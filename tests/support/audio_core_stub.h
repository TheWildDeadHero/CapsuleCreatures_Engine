/// @file
/// A stand-in for the audio core (capsule/audio/core.h) that the backend tests and the backends' smoke programs link
/// instead of the real one (D-34 M4 B1b): it owns a ring, runs the DMA counter exactly as the format says (§6: -1 at
/// each vsync(), at 0 reload 7 and restart the DMAs; the mix writes segment 0 when the counter is <= 1, else
/// 8 - counter), and each vblank() writes a known pattern into the segment plus a few tone writes, so a test can
/// predict every byte the backend captures. No players, no songs.
#pragma once

#include <cstdint>

namespace capsule::audio::stub_core
{

/// Frames between the stub's tone notes.
inline constexpr uint32_t kNoteEvery = 30;
/// The frame (1-based count of vblank() calls) at which the stub switches the output to mono.
inline constexpr uint32_t kMonoAt = 100;

/// Clears the ring and the counter and starts the backend (hal::audio::init()).
void init();

/// vblank() calls since init().
/// @return the count
uint32_t frames();

/// The byte the stub mixes for sample i of the frame-th vblank() (1-based), right side; the left side is its
/// negation (wrapping).
/// @param frame  the vblank() count
/// @param i      sample index 0-223
/// @return the byte
int8_t pattern(uint32_t frame, uint32_t i);

} // namespace capsule::audio::stub_core
