/// @file
/// The compiled audio table (D-34, M4 B1a; format document section 4): what tools/audio_compiler.py writes into a
/// build's generated audio_table.cpp. Everything here is `const` data: ROM on the GBA, ordinary constants elsewhere.
/// One table per build replaces the engine library's empty default (src/audio/audio_default.cpp) at link time.
///
/// A **sample** is a 16-byte SampleHeader immediately followed by its bytes (signed 8-bit PCM, or 4-bit DPCM in
/// 33-byte blocks of 64 samples), plus the guard bytes playback reads past its end. A **voice** is a 12-byte (on the
/// GBA) instrument entry; an instrument set is a pointer to the first of 128 consecutive voices. A **song** is a
/// header plus one command stream per track (format section 3), all tracks' streams in one byte array.
#pragma once

#include <cstdint>

/// The compiled audio data.
namespace capsule::audio::rom
{

/// @name Sample formats (SampleHeader::format)
///@{
inline constexpr uint8_t kPcm8 = 0;    ///< Signed 8-bit samples.
inline constexpr uint8_t kDpcm4 = 1;   ///< 33-byte blocks of 64 samples: a raw first sample, then 4-bit deltas.
///@}

/// SampleHeader::flags: the sample loops from loop_start to length.
inline constexpr uint8_t kSampleLoop = 0x40;

/// @name Padding and envelope sizes
///@{
inline constexpr uint8_t kHeaderPad = 2;     ///< SampleHeader::reserved.
inline constexpr uint8_t kEntryPad = 3;      ///< SongEntry::reserved.
inline constexpr uint8_t kPlayerPad = 2;     ///< PlayerConfig::reserved.
inline constexpr uint8_t kEnvelopeBytes = 4; ///< VoiceEnv::adsr.
///@}

/// A sample's header; its data follows it directly (sample_data()).
struct SampleHeader
{
   uint8_t format;             ///< kPcm8 or kDpcm4.
   uint8_t reserved[kHeaderPad];   ///< 0.
   uint8_t flags;              ///< kSampleLoop.
   uint32_t pitch;             ///< Playback rate in Hz x 1024 at key 60.
   uint32_t loop_start;        ///< First sample of the loop.
   uint32_t length;            ///< Samples played; its top byte is what reverse playback reads before the data.
};

/// The bytes of a sample (they follow the header).
/// @param h  the sample
/// @return its first data byte
inline const int8_t* sample_data(const SampleHeader* h)
{
   return reinterpret_cast<const int8_t*>(h + 1);
}

/// What a voice plays (the low 4 bits of Voice::kind).
enum class VoiceKind : uint8_t
{
   None = 0,          ///< Silent: a note on it is dropped.
   Sample = 1,        ///< A sample, resampled to the note (or fixed rate).
   SquareSweep = 2,   ///< Tone channel 1: square wave with frequency sweep.
   Square = 3,        ///< Tone channel 2: square wave.
   Wave = 4,          ///< Tone channel 3: a 32-step 4-bit wave pattern.
   Noise = 5,         ///< Tone channel 4: noise.
   Split = 6,         ///< Key split: entry = instruments[keymap[key]], played at the track's key.
   Drums = 7,         ///< Drum set: entry = instruments[key], played at the entry's own key and pan.
};

/// @name Voice::kind flags
///@{
inline constexpr uint8_t kVoiceKindMask = 0x0F;   ///< The VoiceKind bits.
inline constexpr uint8_t kFixedRate = 0x10;       ///< Sample: one sample per output sample, no pitch.
inline constexpr uint8_t kReverse = 0x20;         ///< Sample: plays from the end backwards.
inline constexpr uint8_t kEvenPitch = 0x40;       ///< Tone: the frequency register is rounded to even values.
///@}

/// @name Square duty / noise period values (Voice::ref.value)
///@{
inline constexpr uint32_t kNoisePeriodLong = 0;    ///< 15-bit noise.
inline constexpr uint32_t kNoisePeriodShort = 1;   ///< 7-bit noise.
///@}

/// Bytes of one wave pattern (32 4-bit steps, the high nibble first).
inline constexpr uint8_t kWaveBytes = 16;

struct Voice;

/// What a voice points at (by kind).
union VoiceRef
{
   const SampleHeader* sample;   ///< Sample: the sample.
   const uint8_t* wave;          ///< Wave: kWaveBytes of pattern.
   const Voice* set;             ///< Split / Drums: the first voice of its 128-voice instrument set.
   uint32_t value;               ///< Square / SquareSweep: the duty 0-3; Noise: the period (kNoisePeriodLong / Short).
};

/// A voice's envelope, or a key split's keymap.
union VoiceEnv
{
   uint8_t adsr[kEnvelopeBytes];   ///< Attack, decay, sustain, release.
   const uint8_t* keymap;    ///< Split: 128 entries, key -> index into the instrument set.
};

/// @name VoiceEnv::adsr indices
///@{
inline constexpr uint8_t kAttack = 0;    ///< Attack.
inline constexpr uint8_t kDecay = 1;     ///< Decay.
inline constexpr uint8_t kSustain = 2;   ///< Sustain.
inline constexpr uint8_t kRelease = 3;   ///< Release.
///@}

/// One voice (12 bytes on the GBA).
struct Voice
{
   uint8_t kind;          ///< VoiceKind | kFixedRate | kReverse | kEvenPitch.
   uint8_t key;           ///< Base key (Sample / tone voices); drum entries play at it.
   uint8_t tone_length;   ///< Tone voices: the hardware length (0: no length).
   uint8_t pan_sweep;     ///< Through a drum set with bit 7: the pan; SquareSweep: the sweep register value.
   VoiceRef ref;          ///< What the voice plays.
   VoiceEnv env;          ///< Its envelope (Split: its keymap).
};

/// Voices in an instrument set window.
inline constexpr uint16_t kSetVoices = 128;

/// SongHeader::reverb: set the mixer's reverb to the low 7 bits when the song starts (0: leave it as it is).
inline constexpr uint8_t kReverbSet = 0x80;

/// A song: its tracks' command streams (format section 3) and the voices they select.
struct SongHeader
{
   uint8_t track_count;        ///< Tracks (0-16).
   uint8_t priority;           ///< Song priority (0-255).
   uint8_t reverb;             ///< kReverbSet | value to set it; 0 leaves it.
   uint8_t reserved;           ///< 0.
   const Voice* voices;        ///< The instrument set (first voice of its 128-voice window).
   const uint8_t* data;        ///< Every track's commands; jump targets are offsets into it.
   const uint16_t* tracks;     ///< track_count start offsets into data.
};

/// One sound number: a song and the player it plays on.
struct SongEntry
{
   const SongHeader* song;     ///< The song (the empty song for an empty slot).
   uint8_t player;             ///< Index into Table::players.
   uint8_t reserved[kEntryPad];    ///< 0.
};

/// One player of the game's player table.
struct PlayerConfig
{
   uint8_t tracks;             ///< Its tracks (1-16).
   uint8_t start_check;        ///< 1: refuses a song of lower priority than the one playing.
   uint8_t reserved[kPlayerPad];   ///< 0.
};

/// The build's audio table.
struct Table
{
   const SongEntry* songs;          ///< The song table, indexed by sound number.
   uint16_t song_count;             ///< Its entries.
   const PlayerConfig* players;     ///< The players, in table order.
   uint8_t player_count;            ///< Its entries.
   uint8_t rate_index;              ///< Mixing rate step 0-11 (3: 13,379 Hz, 224 samples a frame).
   uint8_t channels;                ///< Sampled channels (1-12).
   uint8_t master_volume;           ///< Master volume (0-15).
   uint8_t dac_bits;                ///< DAC resolution (6-9 bits).
   uint8_t stereo;                  ///< 1: stereo at start, 0: mono.
   const Voice* clips;              ///< The clip voices, in manifest order.
   uint16_t clip_count;             ///< Their number.
   const uint32_t* keys;            ///< Per sound number: its Asset Key (the entry's name, else its song's key; 0).
};

/// The build's table (generated; the engine library's default is empty).
extern const Table kTable;

}   // namespace capsule::audio::rom
