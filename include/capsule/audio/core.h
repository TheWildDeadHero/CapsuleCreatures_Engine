/// @file
/// What an audio backend calls (D-34, M4 B1a; format document section 6): the core's mix ring and its two
/// interrupt-time steps. The backends (capsule/hal/audio.h, B1b) call these; games use capsule/audio/audio.h.
///
/// The ring is 8-bit stereo, 7 segments of one frame's samples per side (224 at 13,379 Hz) plus 16 unused bytes.
/// A backend calls them as one step, vsync() then vblank(), on the audio clock (C14a, D-34: once per game frame plus
/// once per modelled lag blank of the reference, on every target; capsule/hal/audio.h). vsync() runs the DMA counter:
/// every 7th call it tells the backend the playback wraps to the ring's start. vblank(): the players (with fades),
/// the clip players, the tone channels (register writes through hal::audio::tone_write()), then the mix of the
/// segment after the one the DMA plays (segment 0 when the counter is 1 or 0, else 8 - counter).
#pragma once

#include <cstdint>

/// The audio core's backend interface.
namespace capsule::audio::core
{

inline constexpr int kRingBytes = 1584;     ///< Bytes per side: 7 segments of 224, then 16 unused.
inline constexpr uint8_t kSegments = 7;     ///< Segments per side (at 224 samples a frame).
inline constexpr int kRingAlign = 4;        ///< The ring's alignment (32-bit DMA).

/// The mix ring: the right side (FIFO A), then the left (FIFO B). Owned by the core (IWRAM on the GBA).
struct alignas(kRingAlign) Ring
{
   int8_t right[kRingBytes];   ///< Right side.
   int8_t left[kRingBytes];    ///< Left side.
};

/// The ring (the GBA DMA reads it in place).
/// @return the core's ring
const Ring& ring();

/// The DMA counter step (the first half of a step, before vblank(); none on the first step after init()).
void vsync();

/// The second half of a step: players, clip players, tone channels, then the mix of the next segment.
void vblank();

/// The segment the latest vblank() wrote.
/// @return 0..kSegments - 1
uint8_t segment();

/// Samples mixed per side each frame (224 at the table's 13,379 Hz).
/// @return the count
uint16_t frame_samples();

}   // namespace capsule::audio::core
