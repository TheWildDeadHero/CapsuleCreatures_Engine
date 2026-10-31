/// @file
/// The audio core's state for tests and parity recordings (D-34 decision 2a; format document section 5): every
/// player, track and channel as the latest vertical blank step left it. The field names follow the reference
/// driver's state so a recorder can compare them with the original's (track positions are offsets into the song's
/// command data; commands run are counted per track).
#pragma once

#include "capsule/audio/audio.h"

#include <cstdint>

namespace capsule::audio
{

/// CALL levels of a track.
inline constexpr uint8_t kCallLevels = 3;
/// TrackState::flags: the track exists (it runs; its END clears the bit at once, the player's status bits follow at
/// the next tick).
inline constexpr uint8_t kTrackExists = 0x80;

/// One player (game players first, then the two clip players).
struct PlayerState
{
   uint32_t status;        ///< Bits 0-15: tracks that existed at the latest tick; bit 31: paused.
   uint32_t clock;         ///< Ticks since the song started.
   uint16_t sound;         ///< The sound number last started (clip players: the clip), or kNoSound.
   uint16_t tempo_d;       ///< Tempo: twice the song's TEMPO value (150 = one tick a frame).
   uint16_t tempo_u;       ///< Tempo scale (kTempoNormal = 1.0).
   uint16_t tempo_i;       ///< Tempo increment per frame: tempo_d x tempo_u >> 8.
   uint16_t tempo_c;       ///< Tempo accumulator (a tick per 150).
   uint16_t fade_oi;       ///< Fade: frames per step (0: no fade).
   uint16_t fade_oc;       ///< Fade: frames until the next step.
   uint16_t fade_ov;       ///< Fade: volume x 4 (bit 0: keep the tracks, bit 1: fading in).
   uint8_t priority;       ///< The song's priority.
   uint8_t track_count;    ///< Tracks of the player.
   uint8_t first_track;    ///< Its first track in Snapshot::tracks.
   uint8_t start_check;    ///< 1: refuses lower-priority songs.
};

/// One track.
struct TrackState
{
   uint32_t events;           ///< Commands run since the song started.
   uint32_t start_offset;     ///< Samples a note's sample skips (EXT 4).
   uint16_t offset;           ///< Next command: offset into the song's data.
   uint16_t hold;             ///< EXT 3 hold counter.
   uint16_t stack[kCallLevels];   ///< CALL return offsets.
   uint8_t flags;             ///< 0x80 exists, 0x40 start, 0x03 volume change, 0x0C pitch change.
   uint8_t wait;              ///< Ticks to wait.
   uint8_t call_depth;        ///< CALL depth 0-3.
   uint8_t repeat;            ///< REPEAT counter.
   uint8_t gate;              ///< Gate of the next note.
   uint8_t key;               ///< Current key.
   uint8_t velocity;          ///< Current velocity.
   uint8_t running;           ///< Running command byte.
   uint8_t program;           ///< The last PROGRAM number.
   uint8_t voice_kind;        ///< The track's voice: Voice::kind.
   uint8_t voice_key;         ///< The track's voice: Voice::key.
   int8_t key_m;              ///< Resulting key offset (pitch >> 8).
   uint8_t pit_m;             ///< Resulting fine pitch (pitch & 0xFF).
   int8_t key_shift;          ///< TRANSPOSE.
   int8_t key_shift_x;        ///< set_pitch() >> 8.
   int8_t tune;               ///< FINE_TUNE - 64.
   uint8_t pit_x;             ///< set_pitch() & 0xFF.
   int8_t bend;               ///< BEND - 64.
   uint8_t bend_range;        ///< BEND_RANGE.
   uint8_t vol_mr;            ///< Resulting right volume.
   uint8_t vol_ml;            ///< Resulting left volume.
   uint8_t vol;               ///< VOLUME.
   uint8_t vol_x;             ///< Volume scale (64 = 1.0; set_volume() / 4, fades).
   int8_t pan;                ///< PAN - 64.
   int8_t pan_x;              ///< set_pan().
   int8_t mod_m;              ///< Current modulation value.
   uint8_t mod;               ///< MOD_DEPTH.
   uint8_t mod_type;          ///< MOD_TYPE: 0 pitch, 1 volume, 2 pan.
   uint8_t lfo_speed;         ///< LFO_SPEED.
   uint8_t lfo_phase;         ///< LFO phase.
   uint8_t lfo_delay;         ///< LFO_DELAY.
   uint8_t lfo_delay_count;   ///< LFO delay counter.
   uint8_t priority;          ///< PRIORITY.
   uint8_t echo_volume;       ///< EXT 0.
   uint8_t echo_length;       ///< EXT 1.
   uint8_t channel;           ///< First channel of its list (index into Snapshot::channels), 0xFF none.
};

/// One channel: Snapshot::channels holds the sampled channels (kMaxSampledChannels), then the 4 tone channels.
struct ChannelState
{
   uint32_t frequency;     ///< Sampled: the rate (Hz); tone: the register value.
   int32_t position;       ///< Sampled: the current sample index.
   int32_t count;          ///< Sampled: samples left before the end.
   uint32_t fraction;      ///< Sampled: the position's fraction (23 bits).
   uint8_t status;         ///< 0x80 start, 0x40 stop, 0x20 decoder set, 0x10 loop, 0x04 echo, 0x03 envelope phase.
   uint8_t kind;           ///< The voice's Voice::kind.
   uint8_t track;          ///< Rank of its track (1-based; 0 none).
   uint8_t key;            ///< Key played.
   uint8_t midi_key;       ///< The note's key (NOTE_OFF matches it).
   uint8_t velocity;       ///< Velocity.
   uint8_t priority;       ///< Priority.
   uint8_t gate;           ///< Ticks to the release (0: held).
   int8_t rhythm_pan;      ///< Drum pan.
   uint8_t right;          ///< Right volume.
   uint8_t left;           ///< Left volume.
   uint8_t env;            ///< Envelope level.
   uint8_t env_right;      ///< Sampled: the right output volume; tone: the envelope goal.
   uint8_t env_left;       ///< Sampled: the left output volume; tone: the envelope counter.
   uint8_t attack;         ///< Envelope attack.
   uint8_t decay;          ///< Envelope decay.
   uint8_t sustain;        ///< Envelope sustain.
   uint8_t release;        ///< Envelope release.
   uint8_t echo_volume;    ///< Pseudo-echo volume.
   uint8_t echo_length;    ///< Pseudo-echo length.
   uint8_t sustain_goal;   ///< Tone: the sustain level.
   uint8_t pan;            ///< Tone: the routing bits.
   uint8_t n4;             ///< Tone: the NRx4 value.
   uint8_t length;         ///< Tone: the length.
   uint8_t sweep;          ///< Tone 1: the sweep.
   uint8_t next;           ///< Next channel of the track's list, 0xFF none.
};

/// The whole state.
struct Snapshot
{
   PlayerState players[kMaxPlayers + kClipPlayers];          ///< player_count, then the 2 clip players.
   TrackState tracks[kMaxTracks];                            ///< track_count tracks (rank = index + 1).
   ChannelState channels[kMaxSampledChannels + kToneChannels];   ///< Sampled, then tone.
   uint8_t var[kVarBytes];     ///< The variable area.
   uint8_t player_count;       ///< Game players (the clip players follow).
   uint8_t track_count;        ///< Tracks in use.
   uint8_t sampled_channels;   ///< Sampled channels mixed.
   uint8_t reverb;             ///< Reverb 0-127 (global, sticky).
   uint8_t dma_counter;        ///< The DMA counter.
   uint8_t segment;            ///< The segment the latest step mixed.
   uint8_t master_volume;      ///< Master volume 0-15.
   uint8_t mono;               ///< 1: mono output.
   uint8_t envelope_clock;     ///< Tone envelope frame counter (14..0).
};

/// Copies the state the latest vertical blank step left.
/// @param out  the snapshot
void snapshot(Snapshot& out);

}   // namespace capsule::audio
