/// @file
/// The audio core's state and the functions its files share (D-34, M4 B1a). Not a public header: games use
/// capsule/audio/audio.h, backends capsule/audio/core.h, tests capsule/audio/state.h.
///
/// Layout follows the reference handheld driver's state (inventory sections 2-6): players with tempo and fade
/// counters, tracks with their command position, volume/pitch inputs and LFO, channels with their envelope, sample
/// position and track links. Tracks are ranked by their index (+1) in g_tracks: player table order, then track
/// index, the clip players' tracks last; the rank replaces the original's track addresses in every tie. Channels
/// 0-11 are the sampled channels, 12-15 the tone channels (square + sweep, square, wave, noise). Each track keeps a
/// list of its channels (head + next / prev indices); a channel names its track by rank (0: none).
///
/// Placement on the GBA: the ring and the decoder cache in IWRAM (the mixer writes them every sample), everything
/// else in EWRAM.
#pragma once

#include "capsule/audio/audio.h"
#include "capsule/audio/core.h"
#include "capsule/audio/rom.h"

#include <cstdint>

namespace capsule::audio::detail
{

/// @name Command bytes (format section 3)
///@{
inline constexpr uint8_t kCmdNote = 0x80;        ///< NOTE i: 0x80 + i, i = 0..48.
inline constexpr uint8_t kCmdNoteLast = 0xB0;    ///< The last NOTE byte.
inline constexpr uint8_t kCmdWait = 0xB1;        ///< WAIT i: 0xB1 + i.
inline constexpr uint8_t kCmdWaitLast = 0xE1;    ///< The last WAIT byte.
inline constexpr uint8_t kCmdEnd = 0xE2;         ///< END.
inline constexpr uint8_t kCmdJump = 0xE3;        ///< JUMP u16.
inline constexpr uint8_t kCmdCall = 0xE4;        ///< CALL u16.
inline constexpr uint8_t kCmdReturn = 0xE5;      ///< RETURN.
inline constexpr uint8_t kCmdRepeat = 0xE6;      ///< REPEAT n u16.
inline constexpr uint8_t kCmdVar = 0xE7;         ///< VAR op addr data [u16].
inline constexpr uint8_t kCmdPriority = 0xE8;    ///< PRIORITY v.
inline constexpr uint8_t kCmdTempo = 0xE9;       ///< TEMPO v.
inline constexpr uint8_t kCmdTranspose = 0xEA;   ///< TRANSPOSE s8.
inline constexpr uint8_t kCmdProgram = 0xEB;     ///< PROGRAM n (the first of the running commands after NOTE).
inline constexpr uint8_t kCmdVolume = 0xEC;      ///< VOLUME v.
inline constexpr uint8_t kCmdPan = 0xED;         ///< PAN v.
inline constexpr uint8_t kCmdBend = 0xEE;        ///< BEND v.
inline constexpr uint8_t kCmdBendRange = 0xEF;   ///< BEND_RANGE v.
inline constexpr uint8_t kCmdLfoSpeed = 0xF0;    ///< LFO_SPEED v.
inline constexpr uint8_t kCmdLfoDelay = 0xF1;    ///< LFO_DELAY v.
inline constexpr uint8_t kCmdModDepth = 0xF2;    ///< MOD_DEPTH v.
inline constexpr uint8_t kCmdModType = 0xF3;     ///< MOD_TYPE v.
inline constexpr uint8_t kCmdFineTune = 0xF4;    ///< FINE_TUNE v.
inline constexpr uint8_t kCmdExt = 0xF5;         ///< EXT sub args.
inline constexpr uint8_t kCmdNoteOff = 0xF6;     ///< NOTE_OFF [key].
inline constexpr uint8_t kCmdFirst = 0x80;       ///< Bytes below are arguments.
///@}

/// @name EXT sub-commands
///@{
inline constexpr uint8_t kExtEchoVolume = 0;   ///< Pseudo-echo volume.
inline constexpr uint8_t kExtEchoLength = 1;   ///< Pseudo-echo length.
inline constexpr uint8_t kExtRelease = 2;      ///< The track voice's release.
inline constexpr uint8_t kExtHold = 3;         ///< Hold u16 ticks (re-runs itself with a wait of 1).
inline constexpr uint8_t kExtStartOffset = 4;  ///< Samples skipped by the next notes (u32).
inline constexpr uint8_t kExtAttack = 5;       ///< The track voice's attack.
inline constexpr uint8_t kExtDecay = 6;        ///< The track voice's decay.
inline constexpr uint8_t kExtSustain = 7;      ///< The track voice's sustain.
inline constexpr uint8_t kExtToneLength = 8;   ///< The track voice's tone length.
inline constexpr uint8_t kExtSweep = 9;        ///< The track voice's sweep (pan_sweep).
///@}

/// VAR operations: 0-5 assign, 6-17 jump when the comparison holds.
inline constexpr uint8_t kVarOps = 18;
/// The first VAR operation that jumps.
inline constexpr uint8_t kVarFirstJump = 6;

/// @name Track flags
///@{
inline constexpr uint8_t kTrackVolSet = 0x01;      ///< Recompute the volume.
inline constexpr uint8_t kTrackVolChange = 0x03;   ///< Recompute the volume and push it to the channels.
inline constexpr uint8_t kTrackPitSet = 0x04;      ///< Recompute the pitch.
inline constexpr uint8_t kTrackPitChange = 0x0C;   ///< Recompute the pitch and push it to the channels.
inline constexpr uint8_t kTrackChanges = 0x0F;     ///< Every change bit.
inline constexpr uint8_t kTrackStart = 0x40;       ///< Reset at the next tick.
inline constexpr uint8_t kTrackExist = 0x80;       ///< The track runs.
inline constexpr uint8_t kTrackKeep = 0xF0;        ///< The bits a volume / pitch pass keeps.
///@}

/// @name Channel status
///@{
inline constexpr uint8_t kChanStart = 0x80;     ///< Started; the envelope begins at the next mix.
inline constexpr uint8_t kChanStop = 0x40;      ///< Releasing.
inline constexpr uint8_t kChanDecoder = 0x20;   ///< The reverse / compressed position is set.
inline constexpr uint8_t kChanLoop = 0x10;      ///< The sample loops.
inline constexpr uint8_t kChanEcho = 0x04;      ///< Pseudo-echo.
inline constexpr uint8_t kChanEnv = 0x03;       ///< The envelope phase bits.
inline constexpr uint8_t kEnvAttack = 3;        ///< Attack.
inline constexpr uint8_t kEnvDecay = 2;         ///< Decay.
inline constexpr uint8_t kEnvSustain = 1;       ///< Sustain.
inline constexpr uint8_t kEnvRelease = 0;       ///< Release.
inline constexpr uint8_t kChanOn = kChanStart | kChanStop | kChanEcho | kChanEnv;   ///< Any of these: sounding.
///@}

/// @name Tone channel modify bits
///@{
inline constexpr uint8_t kModifyVolume = 0x01;   ///< Write the envelope and routing.
inline constexpr uint8_t kModifyPitch = 0x02;    ///< Write the frequency.
///@}

/// No channel (list ends, a track without channels).
inline constexpr uint8_t kNoChannel = 0xFF;
/// The first tone channel's index in g_channels.
inline constexpr uint8_t kFirstTone = kMaxSampledChannels;
/// Every channel slot.
inline constexpr uint8_t kChannelSlots = kMaxSampledChannels + kToneChannels;
/// Every player slot: the game's, then the clip players.
inline constexpr uint8_t kPlayerSlots = kMaxPlayers + kClipPlayers;

/// @name Fixed-point constants of the original driver
///@{
inline constexpr uint16_t kTempoTick = 150;           ///< Tempo accumulator per tick.
inline constexpr uint16_t kTempoStart = 150;          ///< Tempo when a song starts (one tick a frame).
inline constexpr uint32_t kPaused = 0x80000000u;      ///< Player status: paused.
inline constexpr uint32_t kTrackBits = 0x0000FFFFu;   ///< Player status: the track bits.
inline constexpr uint16_t kFadeFull = 64 << 2;        ///< Fade volume at full (volume x 4).
inline constexpr uint16_t kFadeStep = 4 << 2;         ///< Fade volume change per step.
inline constexpr uint16_t kFadeKeep = 0x0001;         ///< Fade flag: keep the tracks (pause at the end).
inline constexpr uint16_t kFadeIn = 0x0002;           ///< Fade flag: fading in.
inline constexpr uint8_t kFadeShift = 2;              ///< volX = fade volume >> 2.
inline constexpr uint8_t kStartBendRange = 2;         ///< Track reset: bend range.
inline constexpr uint8_t kStartVolX = 64;             ///< Track reset: volume scale 1.0.
inline constexpr uint8_t kStartLfoSpeed = 22;         ///< Track reset: LFO speed.
inline constexpr uint8_t kCentre = 64;                ///< PAN / BEND / FINE_TUNE centre.
inline constexpr uint8_t kMaxCallDepth = 3;           ///< CALL levels.
inline constexpr uint32_t kFractionBits = 23;         ///< Sample position fraction bits.
inline constexpr uint32_t kFixedStep = 1u << kFractionBits;   ///< One sample per output sample.
inline constexpr uint32_t kFractionClear = 0x3F800000u;      ///< The bits a step clears (23-29; 30-31 stay).
inline constexpr uint8_t kRhythmPanCentre = 0xC0;     ///< Drum pan: (pan_sweep - 0xC0) x 2.
inline constexpr uint8_t kPanSweepPan = 0x80;         ///< pan_sweep bit 7: a drum entry's pan / sweep off.
inline constexpr uint8_t kSweepTimeBits = 0x70;       ///< NR10 sweep time bits.
inline constexpr uint8_t kSweepOff = 8;               ///< NR10 value with no sweep.
inline constexpr uint8_t kMaxEnvelope = 255;          ///< Sampled envelope maximum.
inline constexpr uint8_t kSampleLoopBits = 0xC0;      ///< Sample flags that mean "loops".
inline constexpr uint8_t kDpcmBlockSamples = 64;      ///< Samples per DPCM block.
inline constexpr uint8_t kDpcmBlockBytes = 33;        ///< Bytes per DPCM block.
inline constexpr uint8_t kDpcmBlockShift = 6;         ///< log2(kDpcmBlockSamples).
inline constexpr uint8_t kToneEnvelopeFrames = 14;    ///< Tone envelope clock: counts 14..0.
inline constexpr uint8_t kSustainRefresh = 7;         ///< Tone sustain: frames between volume refreshes.
///@}

/// One track.
struct Track
{
   uint32_t events;          ///< Commands run since the song started.
   uint32_t start_offset;    ///< EXT 4: samples a note skips.
   uint16_t offset;          ///< Next command (offset into the player's song data).
   uint16_t hold;            ///< EXT 3 counter.
   uint16_t stack[kMaxCallDepth];   ///< CALL return offsets.
   uint8_t flags;            ///< kTrack* bits.
   uint8_t wait;             ///< Ticks to wait.
   uint8_t call_depth;       ///< CALL depth.
   uint8_t repeat;           ///< REPEAT counter.
   uint8_t gate;             ///< Gate of the next note.
   uint8_t key;              ///< Current key.
   uint8_t velocity;         ///< Current velocity.
   uint8_t running;          ///< Running command byte.
   int8_t key_m;             ///< Pitch result: key offset.
   uint8_t pit_m;            ///< Pitch result: fine.
   int8_t key_shift;         ///< TRANSPOSE.
   int8_t key_shift_x;       ///< set_pitch() key part.
   int8_t tune;              ///< FINE_TUNE - 64.
   uint8_t pit_x;            ///< set_pitch() fine part.
   int8_t bend;              ///< BEND - 64.
   uint8_t bend_range;       ///< BEND_RANGE.
   uint8_t vol_mr;           ///< Volume result: right.
   uint8_t vol_ml;           ///< Volume result: left.
   uint8_t vol;              ///< VOLUME.
   uint8_t vol_x;            ///< Volume scale (64 = 1.0).
   int8_t pan;               ///< PAN - 64.
   int8_t pan_x;             ///< set_pan().
   int8_t mod_m;             ///< Modulation value.
   uint8_t mod;              ///< MOD_DEPTH.
   uint8_t mod_type;         ///< MOD_TYPE.
   uint8_t lfo_speed;        ///< LFO_SPEED.
   uint8_t lfo_phase;        ///< LFO phase.
   uint8_t lfo_delay;        ///< LFO_DELAY.
   uint8_t lfo_delay_count;  ///< LFO delay counter.
   uint8_t priority;         ///< PRIORITY.
   uint8_t echo_volume;      ///< EXT 0.
   uint8_t echo_length;      ///< EXT 1.
   uint8_t chan;             ///< First channel of its list, kNoChannel.
   uint8_t program;          ///< The last PROGRAM number.
   uint8_t cleared;          ///< clear_clips(): ends at the next command.
   rom::Voice voice;         ///< Its voice (a copy of the PROGRAM's).
};

/// One channel (sampled or tone).
struct Channel
{
   const rom::SampleHeader* sample;   ///< Sampled: its sample.
   const uint8_t* loaded_wave;        ///< Tone 3: the pattern now in wave RAM.
   rom::VoiceRef ref;                 ///< Tone: duty / period / wave pattern; sampled: the sample.
   uint32_t frequency;     ///< Sampled: rate in Hz; tone: the register value.
   int32_t position;       ///< Sampled: the current sample.
   int32_t count;          ///< Sampled: samples left (at a start: the start offset).
   uint32_t fraction;      ///< Sampled: position fraction (kFractionBits).
   uint8_t status;         ///< kChan* bits.
   uint8_t kind;           ///< Voice::kind of its voice.
   uint8_t right;          ///< Right volume.
   uint8_t left;           ///< Left volume.
   uint8_t attack;         ///< Envelope attack.
   uint8_t decay;          ///< Envelope decay.
   uint8_t sustain;        ///< Envelope sustain.
   uint8_t release;        ///< Envelope release.
   uint8_t key;            ///< Key played.
   uint8_t env;            ///< Envelope level.
   uint8_t env_right;      ///< Sampled: right output volume.
   uint8_t env_left;       ///< Sampled: left output volume.
   uint8_t env_goal;       ///< Tone: envelope goal.
   uint8_t env_counter;    ///< Tone: envelope step counter.
   uint8_t echo_volume;    ///< Pseudo-echo volume.
   uint8_t echo_length;    ///< Pseudo-echo length.
   uint8_t gate;           ///< Ticks to the release.
   uint8_t midi_key;       ///< The note's key.
   uint8_t velocity;       ///< Velocity.
   uint8_t priority;       ///< Priority.
   int8_t rhythm_pan;      ///< Drum pan.
   uint8_t sustain_goal;   ///< Tone: sustain level.
   uint8_t n4;             ///< Tone: NRx4 value.
   uint8_t pan;            ///< Tone: routing bits.
   uint8_t pan_mask;       ///< Tone: this channel's routing bits.
   uint8_t modify;         ///< Tone: kModify* bits.
   uint8_t length;         ///< Tone: length.
   uint8_t sweep;          ///< Tone 1: sweep.
   uint8_t track;          ///< Its track's rank (0: none).
   uint8_t prev;           ///< Previous channel of the track's list.
   uint8_t next;           ///< Next channel of the track's list.
};

/// One player.
struct Player
{
   const rom::SongHeader* song;   ///< The song last started.
   const rom::Voice* voices;      ///< Its instrument set.
   const uint8_t* data;           ///< Its command data.
   uint32_t status;               ///< kTrackBits | kPaused.
   uint32_t clock;                ///< Ticks since the start.
   uint16_t sound;                ///< The sound number (clip players: the clip).
   uint16_t tempo_d;              ///< Tempo.
   uint16_t tempo_u;              ///< Tempo scale.
   uint16_t tempo_i;              ///< Tempo increment.
   uint16_t tempo_c;              ///< Tempo accumulator.
   uint16_t fade_oi;              ///< Fade frames per step (0: none).
   uint16_t fade_oc;              ///< Fade counter.
   uint16_t fade_ov;              ///< Fade volume x 4 | kFadeKeep | kFadeIn.
   uint8_t track_count;           ///< Tracks.
   uint8_t priority;              ///< Song priority.
   uint8_t start_check;           ///< Refuses lower priorities.
   uint8_t first_track;           ///< Index of its first track in g_tracks.
   volatile uint8_t busy;         ///< An API call is changing it (the vertical blank step skips it).
};

/// The driver.
struct Driver
{
   uint32_t div_freq;         ///< Step per Hz of channel frequency (627 at 13,379 Hz).
   uint16_t frame_samples;    ///< Samples per frame and side.
   uint8_t dma_counter;       ///< Frames to the DMA restart.
   uint8_t dma_period;        ///< Segments in the ring.
   uint8_t reverb;            ///< Reverb 0-127.
   uint8_t channels;          ///< Sampled channels in use.
   uint8_t master_volume;     ///< 0-15.
   uint8_t mono;              ///< 1: mono.
   uint8_t envelope_clock;    ///< Tone envelope clock 14..0.
   uint8_t segment;           ///< The segment written last.
   uint8_t player_count;      ///< Game players.
   uint8_t track_count;       ///< Tracks in use.
   uint8_t output_off;        ///< vsync_off(): the core is idle.
   volatile uint8_t busy;     ///< A driver-wide change (the reverb) is under way: the step skips the frame.
   uint8_t var[kVarBytes];    ///< The songs' variable area.
};

/// Bytes of a clip sequence (31 used).
inline constexpr uint8_t kClipDataBytes = 32;

/// The clip sequence of one clip player (format section 5: two tracks, the second only with chorus).
struct ClipSong
{
   rom::SongHeader header;   ///< Its header.
   uint16_t tracks[kClipTracks];   ///< Track offsets.
   uint8_t data[kClipDataBytes];   ///< The commands.
};

extern core::Ring g_ring;                  ///< The mix ring (IWRAM on the GBA).
extern Driver g_driver;                    ///< The driver.
extern Player g_players[kPlayerSlots];     ///< Players: the game's, then the clip players.
extern Track g_tracks[kMaxTracks];         ///< Tracks.
extern Channel g_channels[kChannelSlots];  ///< Channels: sampled, then tone.
extern ClipSong g_clip_songs[kClipPlayers];   ///< The clip sequences.

/// @name Tables (src/audio/tables.cpp, generated by tools/audio/gen_tables.py)
///@{
inline constexpr uint8_t kLengthCount = 49;          ///< NOTE / WAIT lengths.
inline constexpr uint8_t kNotes = 12;                ///< Notes per octave.
inline constexpr uint8_t kNoiseCount = 60;           ///< Noise register values.
inline constexpr uint8_t kWaveVolumeCount = 16;      ///< Wave volume table (envelope levels 0-15).
inline constexpr uint8_t kDeltaCount = 16;           ///< DPCM deltas.
inline constexpr uint8_t kRateCount = 12;            ///< Mixing rates.
extern const uint8_t kLengths[kLengthCount];         ///< Ticks of NOTE i / WAIT i.
extern const uint32_t kOctave[kNotes];               ///< round(2^31 x 2^(i/12)).
extern const int16_t kToneOctave[kNotes];            ///< round(-131072 / f(36 + i)).
extern const uint8_t kNoise[kNoiseCount];            ///< Noise NR43 values by key - 21.
extern const uint8_t kWaveVolume[kWaveVolumeCount];  ///< NR32 value by envelope level.
extern const int8_t kDeltas[kDeltaCount];            ///< DPCM deltas.
extern const uint16_t kRateSamples[kRateCount];      ///< Samples per frame by rate index.
///@}

/// Rank of a track (1-based).
/// @param t  the track
/// @return its rank
inline uint8_t rank_of(const Track& t)
{
   return static_cast<uint8_t>(&t - g_tracks + 1);
}

/// The track of a rank.
/// @param rank  1-based
/// @return the track
inline Track& track_of(uint8_t rank)
{
   return g_tracks[rank - 1];
}

/// The kind bits of a voice / channel.
/// @param kind  Voice::kind
/// @return its VoiceKind
inline rom::VoiceKind kind_of(uint8_t kind)
{
   return static_cast<rom::VoiceKind>(kind & rom::kVoiceKindMask);
}

/// Whether a kind is a tone voice.
/// @param kind  Voice::kind
/// @return true for square + sweep, square, wave and noise
inline bool is_tone(uint8_t kind)
{
   const rom::VoiceKind k = kind_of(kind);
   return k == rom::VoiceKind::SquareSweep || k == rom::VoiceKind::Square || k == rom::VoiceKind::Wave ||
          k == rom::VoiceKind::Noise;
}

/// The tone channel number (1-4) of a tone kind.
/// @param kind  Voice::kind
/// @return 1 square + sweep, 2 square, 3 wave, 4 noise
inline uint8_t tone_number(uint8_t kind)
{
   return static_cast<uint8_t>((kind & rom::kVoiceKindMask) - static_cast<uint8_t>(rom::VoiceKind::SquareSweep) + 1);
}

/// A compiler barrier (C14a, R10 M3): no memory access moves across it. `volatile` orders only the volatile
/// accesses among themselves, so the busy flags are set and cleared around one of these: the plain stores of an API
/// call (player, track and channel state) can then not be moved before the lock is taken or after it is released,
/// whatever the inlining (LTO). It costs no instruction.
[[gnu::always_inline]] inline void compiler_barrier()
{
   asm volatile("" ::: "memory");
}

/// RAII lock of a player while an API call changes it (the vertical blank step skips a busy player).
class PlayerLock
{
public:
   /// Takes the lock.
   /// @param p  the player
   explicit PlayerLock(Player& p);
   /// Releases it.
   ~PlayerLock();
   PlayerLock(const PlayerLock&) = delete;              ///< Not copyable.
   PlayerLock& operator=(const PlayerLock&) = delete;   ///< Not copyable.

private:
   Player& m_player;   ///< The locked player.
};

// --- Sequencer (sequencer.cpp) ---

/// One frame of a player: its fade, its ticks, then the volume / pitch pass.
/// @param p  the player
void player_main(Player& p);
/// Starts a song on a player (with the start check).
/// @param p      the player
/// @param song   the song
/// @param sound  the sound number for the state
void start_song(Player& p, const rom::SongHeader* song, uint16_t sound);
/// Pauses a player and stops its tracks (the original's stop).
/// @param p  the player
void pause_player(Player& p);
/// The track reset of a START (everything but the command position and the call stack).
/// @param t  the track
void reset_track(Track& t);
/// Recomputes a track's volume and pitch results.
/// @param t  the track
void track_vol_pitch(Track& t);
/// Clears a track's modulation (and flags its change).
/// @param t  the track
void clear_mod(Track& t);

// --- Notes and channels (notes.cpp) ---

/// NOTE i: reads the optional key, velocity and gate extra, then allocates and starts a channel.
/// @param p      the player
/// @param t      the track
/// @param index  the length index (0: held)
void note_on(const Player& p, Track& t, uint8_t index);
/// NOTE_OFF: releases the first sounding, non-releasing channel of the track playing the key.
/// @param p  the player
/// @param t  the track (its command position at the optional key)
void note_off(const Player& p, Track& t);
/// END: releases the track's channels, unlinks them and clears the track.
/// @param t  the track
void track_end(Track& t);
/// Stops a track's channels at once (tone channels silenced) and empties its list.
/// @param t  the track
void track_stop(Track& t);
/// Unlinks a channel from its track's list.
/// @param c  the channel index
void unlink(uint8_t c);
/// Sets a channel's volumes from its track's.
/// @param ch  the channel
/// @param t   its track
void channel_volume(Channel& ch, const Track& t);
/// A sampled channel's frequency.
/// @param sample  the sample
/// @param key     the key (clamped to 178)
/// @param fine    the fine pitch, 1/256 semitone
/// @return the rate in Hz
uint32_t key_to_freq(const rom::SampleHeader* sample, uint8_t key, uint8_t fine);
/// A tone channel's frequency register value.
/// @param number  1-4
/// @param key     the key
/// @param fine    the fine pitch
/// @return the 11-bit value + 2048 (square / wave) or the NR43 value (noise)
uint32_t key_to_tone(uint8_t number, uint8_t key, uint8_t fine);

// --- Envelopes (envelope.cpp) ---

/// The sampled channel's envelope step of a frame (also its start: position, count, loop).
/// @param ch  the channel
/// @return false when the channel stopped
bool sampled_envelope(Channel& ch);

// --- Tone channels (tone.cpp) ---

/// The tone channels' frame: envelopes and register writes (the reference driver's order).
void tone_step();
/// Silences a tone channel.
/// @param number  1-4
void tone_off(uint8_t number);

// --- Mixer (mixer.cpp) ---

/// Mixes the frame's segment: reverb or silence, then every sampled channel.
void mix();
/// Clears the ring (and the high-quality history).
void clear_ring();

}   // namespace capsule::audio::detail
