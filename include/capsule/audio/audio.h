/// @file
/// The engine's audio system (D-34, M4 B1a; format document section 5): sequenced songs on players, sound effects
/// with priorities, fades, one-shot sample clips, mixed bit-exactly to the reference handheld driver's rules
/// (13,379 Hz, 8-bit wrapping output, its envelope, volume and pitch arithmetic).
///
/// Every call takes effect at the next step of the core (capsule/audio/core.h): players in table order, then the two
/// clip players, then the tone channels, then the mix. status() and the predicates read the state the latest step
/// left. Waits are polls: a game checks status each frame the way its original did.
///
/// The audio clock (C14a, D-34): the core steps once per game frame (at the frame's vertical blank, after the game's
/// calls of that frame) and once more for each lag blank the game models with rng::spend_blanks() (at that call),
/// on every target, like the gameplay generator. This build's own lag frames do not step it (the GBA repeats the
/// segment it played). So a call in game frame N lands on the same step on every target, a poll ends on the same
/// game frame, and the state is the same frame by frame (capsule/hal/audio.h has each backend's part).
///
/// Players come from the build's audio table (capsule/audio/rom.h); a sound number (SoundId) names a song and its
/// player. A player with a start check refuses a song of lower priority than the one it plays. A note's priority is
/// the song's plus its track's (at most 255); ties between channels go to the track ranked later (player table
/// order, then track index, clip tracks last).
#pragma once

#include "capsule/audio/rom.h"
#include "capsule/core/types.h"

#include <cstdint>

/// The audio system.
namespace capsule::audio
{

using SoundId = uint16_t;      ///< A sound number: the song table's index.
using PlayerId = uint8_t;      ///< A player: index into the player table.
using ClipHandle = uint8_t;    ///< A clip player (0 or 1).

inline constexpr uint16_t kAllTracks = 0xFFFF;    ///< Every track of a player.
inline constexpr uint16_t kVolumeFull = 256;      ///< Per-track volume scale 1.0.
inline constexpr uint16_t kTempoNormal = 256;     ///< Tempo scale 1.0.
inline constexpr SoundId kNoSound = 0xFFFF;       ///< find_sound(): no such sound.
inline constexpr PlayerId kNoPlayer = 0xFF;       ///< player_of(): no such sound.

/// @name Limits
///@{
inline constexpr uint8_t kMaxPlayers = 8;           ///< Game players in a table.
inline constexpr uint8_t kClipPlayers = 2;          ///< Clip players (after the game's).
inline constexpr uint8_t kClipTracks = 2;           ///< Tracks per clip player.
inline constexpr uint8_t kMaxTracks = 40;           ///< Tracks of every player together, clip players' included.
inline constexpr uint8_t kMaxPlayerTracks = 16;     ///< Tracks of one player.
inline constexpr uint8_t kMaxSampledChannels = 12;  ///< Sampled channels (Table::channels is 1-12).
inline constexpr uint8_t kToneChannels = 4;         ///< Tone channels: square + sweep, square, wave, noise.
inline constexpr uint8_t kVarBytes = 16;            ///< The songs' shared variable area.
///@}

/// Starts the audio system from rom::kTable: clears every state, opens the players in table order and the two clip
/// players, sets the mixer (channels, master volume, rate) and the output, then starts the backend
/// (hal::audio::init()). Call once, before anything else here.
void init();

/// @name Songs
///@{

/// Starts a sound on its player (refused by a start-check player playing a song of higher priority).
/// @param id  the sound number
void play(SoundId id);

/// Starts a sound unless its player already plays that song and has not ended or paused.
/// @param id  the sound number
void play_or_keep(SoundId id);

/// Pauses the sound's player, only if its current song is that sound's.
/// @param id  the sound number
void stop_song(SoundId id);

/// Pauses every player and both clip players.
void stop_all();

/// Unpauses every player and both clip players.
void resume_all();
///@}

/// @name Players
///@{

/// Pauses a player: its notes stop, its position is kept (resume() goes on from there, silent until the next notes).
/// @param p  the player
void pause(PlayerId p);

/// Unpauses a player.
/// @param p  the player
void resume(PlayerId p);

/// Fades a player out in 16 steps of frames_per_step frames each; at the end its tracks are cleared (ended).
/// @param p                the player
/// @param frames_per_step  frames per step (1 or more)
void fade_out(PlayerId p, uint16_t frames_per_step);

/// Fades a player out like fade_out(), but keeps its tracks and pauses it at the end (resume() or fade_in() go on).
/// @param p                the player
/// @param frames_per_step  frames per step
void fade_out_paused(PlayerId p, uint16_t frames_per_step);

/// Unpauses a player and fades it in from silence in 16 steps.
/// @param p                the player
/// @param frames_per_step  frames per step
void fade_in(PlayerId p, uint16_t frames_per_step);

/// Runs the start reset of the tracks of a song started since the last step now (so a following set_volume() or
/// set_pan() is not undone by it).
/// @param p  the player
void apply_start(PlayerId p);

/// Sets the volume scale of some tracks.
/// @param p       the player
/// @param tracks  bit per track (kAllTracks)
/// @param volume  0..kVolumeFull
void set_volume(PlayerId p, uint16_t tracks, uint16_t volume);

/// Shifts the pitch of some tracks.
/// @param p       the player
/// @param tracks  bit per track
/// @param pitch   1/256 semitone (negative: lower)
void set_pitch(PlayerId p, uint16_t tracks, int16_t pitch);

/// Pans some tracks.
/// @param p       the player
/// @param tracks  bit per track
/// @param pan     -128 (left) .. 127 (right), added twice the track's own pan
void set_pan(PlayerId p, uint16_t tracks, int8_t pan);

/// Scales a player's tempo.
/// @param p      the player
/// @param scale  kTempoNormal = 1.0
void set_tempo_scale(PlayerId p, uint16_t scale);

/// Sets the modulation depth of some tracks (0 clears their modulation).
/// @param p       the player
/// @param tracks  bit per track
/// @param depth   the depth
void set_mod_depth(PlayerId p, uint16_t tracks, uint8_t depth);

/// Sets the modulation speed of some tracks (0 clears their modulation).
/// @param p       the player
/// @param tracks  bit per track
/// @param speed   the LFO speed
void set_lfo_speed(PlayerId p, uint16_t tracks, uint8_t speed);
///@}

/// @name State
///@{

/// A player's status after the latest step.
struct PlayerStatus
{
   uint16_t tracks;   ///< Bit per track that existed when the latest tick ran it (0: the song ended).
   bool paused;       ///< Paused (pause(), the end of a fade, or every track ended).
};

/// A player's status.
/// @param p  the player
/// @return its status
PlayerStatus status(PlayerId p);

/// The song a player last started.
/// @param p  the player
/// @return its header, or nullptr before the first one
const rom::SongHeader* current_song(PlayerId p);

/// Whether a player plays.
/// @param p  the player
/// @return true when not paused and some track exists
bool playing(PlayerId p);

/// Whether a player's song ended.
/// @param p  the player
/// @return true when no track exists
bool ended(PlayerId p);

/// Whether a player is paused or its song ended.
/// @param p  the player
/// @return true when paused or no track exists
bool paused_or_ended(PlayerId p);

/// Players in the table (game players; the clip players are not counted).
/// @return the count
uint8_t player_count();

/// Sound numbers in the table.
/// @return the count
uint16_t sound_count();

/// The player a sound plays on.
/// @param id  the sound number
/// @return the player, or kNoPlayer for a number past the table
PlayerId player_of(SoundId id);

/// The sound number named by an Asset Key (a song table entry's name, else its song's key; the first match).
/// @param key  the key
/// @return the number, or kNoSound
SoundId find_sound(AssetKey key);
///@}

/// @name Clips (one-shot sample cues)
///@{

/// A clip's settings.
struct ClipParams
{
   uint16_t clip;          ///< Index into the table's clips.
   uint8_t volume;         ///< 0..127 (bit 7 ignored).
   int8_t pan;             ///< -64..63.
   int16_t pitch;          ///< key x 256 + fine x 2 - 128 (60 x 256: the voice's base pitch).
   uint16_t hold_ticks;    ///< Ticks (one a frame) before the release.
   uint8_t release;        ///< Envelope release.
   uint8_t chorus;         ///< 0: one track; else a second track tuned (chorus + tune) & 0x7F.
   uint8_t priority;       ///< Note priority.
   uint32_t start_offset;  ///< Samples skipped.
};

/// Plays a clip on a free clip player (its first track has ended and lost its channel), else on the one that has
/// played longest.
/// @param c  the settings
/// @return the clip player
ClipHandle play_clip(const ClipParams& c);

/// Whether a clip still sounds.
/// @param h  the clip player
/// @return true while its first track still has a channel
bool clip_playing(ClipHandle h);

/// Pauses a clip player.
/// @param h  the clip player
void stop_clip(ClipHandle h);

/// Clears both clip sequences: a clip track still running ends at its next command.
void clear_clips();
///@}

/// @name Output
///@{

/// Output routing.
enum class Output : uint8_t
{
   Stereo,   ///< Sampled channels: right and left sides; tone channels panned.
   Mono,     ///< Both sides of both, at half volume; tone channels centred.
};

/// Switches stereo / mono (the routing at once, the tone pan from the next volume change).
/// @param o  the routing
void set_output(Output o);

/// Stops the output: the mix ring is cleared and the core does not step until vsync_on() (around a reset or a
/// program that takes the hardware).
void vsync_off();

/// Starts the output again after vsync_off(): the DMA counter restarts.
void vsync_on();

/// A side of the mix.
enum class Side : uint8_t
{
   Right,   ///< The right side (FIFO A).
   Left,    ///< The left side (FIFO B).
};

/// The segment mixed at the latest step (core::frame_samples() samples), for a waveform display.
/// @param side  the side
/// @return its first sample
const int8_t* last_block(Side side);
///@}

/// @name Song variables
///@{

/// A byte of the variable area the songs' VAR commands use (shared by every player).
/// @param index  0..kVarBytes - 1
/// @return its value
uint8_t var(uint8_t index);

/// Sets a byte of the variable area.
/// @param index  0..kVarBytes - 1
/// @param value  the byte
void set_var(uint8_t index, uint8_t value);
///@}

}   // namespace capsule::audio
