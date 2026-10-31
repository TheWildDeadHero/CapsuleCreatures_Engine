/// @file
/// The sequence port's link to the audio system (D-34 answer 4, M4 B1a). The engine's sequence port
/// (src/seq/sequence_port_hal.cpp) plays a sequence's songs and sounds through these: the audio system defines them
/// (src/audio/audio.cpp) for every program that starts it; a program without audio links the port's defaults,
/// which return false so the port keeps its timing stub (presenter::set_song_length()).
///
/// Songs and sounds are named by Asset Key ("song:NAME", "sound:NAME"): an audio song table entry's name, else its
/// song's key (audio::find_sound()).
#pragma once

#include "capsule/core/types.h"

#include <cstdint>

/// Audio for the sequence port.
namespace capsule::seq::audio_link
{

/// Starts a song (fading in over the frames given: the reference games' fade-in, 16 steps).
/// @param song            its key
/// @param fade_in_frames  0: at full volume
/// @return false when the audio system is not started or has no such song (the port's stub plays it)
bool start_song(AssetKey song, uint16_t fade_in_frames);

/// Stops the song start_song() started (its player pauses).
void stop_song();

/// Fades the song out over the frames given (16 steps; the player ends).
/// @param frames  the fade's length
/// @return false when no song of the audio system plays
bool fade_song(uint16_t frames);

/// Whether the song start_song() started still sounds.
/// @return true while its player is neither paused nor ended
bool song_sounding();

/// Plays a sound effect.
/// @param sound  its key
/// @return false when the audio system is not started or has no such sound
bool play_sound(AssetKey sound);

}   // namespace capsule::seq::audio_link
