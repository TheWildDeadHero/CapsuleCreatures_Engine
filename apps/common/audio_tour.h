/// @file
/// The audio tour (D-34 M4 B1b): a deterministic script over any audio table that apps/test_audio plays on the GBA
/// and desktop and apps/audio_render plays headless, so the two runs can be compared vertical blank for vertical
/// blank (format §11.4: L1 state, L2 ring segments, L3 tone writes).
///
/// It depends only on the table and the core's own state, never on the platform:
///   1. Songs: every sound number in turn: play(), then wait until its player ended (status tracks 0) or
///      kSongFrames, stop_all(), kGapFrames of silence.
///   2. Controls on the first sound with a player: fade_out (speed 2) to its end, play again, pause / resume,
///      set_volume / set_pitch / set_pan / set_tempo_scale, mono and back to stereo, fade_out_paused + fade_in,
///      and one modelled lag frame (C14a: the host spends kLagBlanks lag blanks of the reference after the frame's
///      calls, rng::spend_blanks() or the same steps itself: the driver stepping without the game).
///   A GBA host may also lag frames of its own anywhere (apps/test_audio does): they step nothing (the audio clock,
///   capsule/hal/audio.h), so the states still match the headless run step for step.
///   3. Clips: each of the first kTourClips clip voices with chorus 0, 20 and 192 and a pitch, then two at once (the
///      oldest replaced), clip_playing() polled to their ends.
/// A host calls frame() once per game frame before its vertical blank(s) and runs what it returns.
#pragma once

#include <cstdint>

namespace capsule::audio_tour
{

/// Frames a sound may play before the tour moves on (looping songs never end).
inline constexpr uint16_t kSongFrames = 180;
/// Silent frames between two sounds.
inline constexpr uint16_t kGapFrames = 2;
/// The modelled lag frame's extra blanks (C14a: more than the ring's 7 segments, so a GBA host's hold buffer and a
/// ring wrap inside one batch are both covered).
inline constexpr uint16_t kLagBlanks = 9;
/// Clip voices the tour plays (fewer when the table has fewer).
inline constexpr uint16_t kTourClips = 3;
/// Most frames a clip may take.
inline constexpr uint16_t kClipFrames = 240;

/// The tour's parts.
enum class Phase : uint8_t
{
   Songs,      ///< Every sound in turn.
   Controls,   ///< Fades, pause, volume, pitch, pan, tempo, mono, a modelled lag frame.
   Clips,      ///< Clip voices.
   Done,       ///< Finished (the host may stop).
};

/// What the host does after frame().
enum class Blanks : uint8_t
{
   One,    ///< An ordinary frame: one vertical blank.
   Lag,    ///< A modelled lag frame: kLagBlanks extra steps, then the frame's blank, nothing called between them.
   None,   ///< The tour is over.
};

/// The tour's position.
struct Tour
{
   Phase phase;        ///< The part it is in.
   uint16_t index;     ///< Sound number (Songs), control step (Controls) or clip step (Clips).
   uint16_t wait;      ///< Frames spent on the current item.
   uint32_t frame;     ///< Frames since start().
   uint16_t sounds;    ///< Sounds played so far.
   uint16_t clips;     ///< Clips played so far.
   uint16_t longest_sound;    ///< The sound that played longest in the songs part (the controls part plays it).
   uint16_t longest_frames;   ///< Its frames.
   uint8_t clip;              ///< The clip player of the clip being waited for.
};

/// Starts the tour (the core's init() must have run).
/// @param t  the tour
void start(Tour& t);

/// Runs this frame's calls.
/// @param t  the tour
/// @return how many vertical blanks the host runs now
Blanks frame(Tour& t);

/// The phase's name, for the screen.
/// @param p  the phase
/// @return ASCII text
const char* phase_name(Phase p);

} // namespace capsule::audio_tour
