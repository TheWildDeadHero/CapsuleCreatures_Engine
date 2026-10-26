/// @file
/// The engine's sequence port (src/seq/sequence_port_hal.cpp) as seen by the
/// scene that plays a sequence: reset before a sequence, per-frame updates,
/// and what the port could not show.
///
/// The port draws with the graphics modules: backgrounds
/// (gfx::background), sprites (gfx::sprites), palettes and palette fades
/// (gfx::palette), text and the text box (text::layer), plus the video HAL's
/// fades, blending and windows. Audio is a timing stub: songs make no sound
/// but run for a configurable length, so song-synced waits and exits work.
#pragma once

#include "capsule/core/types.h"

#include <cstdint>

/// The engine sequence port's scene-side controls.
namespace capsule::seq::presenter
{

/// Features the port received calls for but cannot show yet (bit positions).
enum Missing : uint8_t
{
   MISSING_ASSET,             ///< A background, sprite or text asset was not in the table.
   MISSING_MAP_VIEW,          ///< map_show / map_camera (C-03).
   MISSING_SCANLINE,          ///< Scanline effects (A-7).
   MISSING_EMITTER,           ///< Particle emitters (A-8).
   MISSING_PALETTE_ANIM,      ///< Palette animations (A-6, A-9).
   MISSING_AUDIO,             ///< Songs and sounds (timing only, no sound).
   NUM_MISSING                ///< Number of features.
};
using MissingMask = uint8_t;   ///< Set of Missing bits.
static_assert(fits_in<MissingMask>(NUM_MISSING));

/// Clears the screen state for a new sequence: mode 0, every layer, sprite,
/// text target and palette cleared, effects and windows off, song stopped.
void reset();

/// Per-frame work after seq::step(): sprite animation and OAM, text typing,
/// the song clock.
void frame();

/// How long a song "plays" before it ends on its own.
/// @param frames  length in frames; 0 = until stopped or faded
void set_song_length(uint32_t frames);

/// Features used since reset() that the port cannot show.
/// @return Missing bits
MissingMask missing();

/// The last asset key that was not found (when MISSING_ASSET is set).
/// @return the key
AssetKey last_missing_asset();

} // namespace capsule::seq::presenter
