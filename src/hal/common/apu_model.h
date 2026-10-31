/// @file
/// Integer output model of the GBA sound hardware (D-34, M4 B1b) for the desktop and headless audio backends: the
/// four tone channels (square with sweep, square, wave, noise), their 512 Hz frame sequencer (length 256 Hz, sweep
/// 128 Hz, envelope 64 Hz), and the final mix of the tones with the two DirectSound FIFOs per side, sampled at the
/// FIFO's instants (224 a frame, one every 1,254 cycles; kHqFactor times as many in the high-quality mode).
///
/// Deterministic and integer only; it is not claimed equal to the hardware's analogue output (format §6). Register
/// writes are applied in order at the start of the frame they are rendered with (D-34 answer 5). The mix per side:
/// tones = sum of the routed channels' 4-bit levels x 2 x (NR50 volume + 1), shifted by SOUNDCNT_H's tone ratio
/// (25 / 50 / 100 %); each routed FIFO byte x 4 (full) or x 2 (half); plus SOUNDBIAS's bias, clamped to 10 bits,
/// cut to the amplitude resolution, then centred and scaled to signed 16 bits. (The hardware, and mGBA, scale a
/// full-volume FIFO by 4 so one FIFO spans the 10-bit range.)
///
/// Simplifications, documented for the parity notes: the sound master enable (NR52 bit 7) is assumed on (the core
/// never turns it off); writes to NRx2 while a channel plays take effect at its next trigger (the core retriggers on
/// every volume change); the FIFO plays the ring from the frame's start (the hardware's DMA restarts at line 150
/// with the timer's phase), and the noise register starts all ones at a trigger.
#pragma once

#include "capsule/hal/audio.h"

#include <cstdint>

namespace capsule::hal::apu
{

/// Cycles of the 2^24 Hz clock in one video frame.
inline constexpr uint32_t kCyclesPerFrame = 280896;
/// Bytes of the sound register block the model keeps (0x04000060-0x0400008F, offsets as hal::audio::ToneReg).
inline constexpr uint8_t kRegisterBytes = 0x30;
/// Offset of SOUNDCNT_H (DirectSound routing and volumes) in that block.
inline constexpr uint8_t kSoundcntH = 0x22;
/// Bytes of one wave RAM bank.
inline constexpr uint8_t kWaveBankBytes = 16;
/// Wave RAM banks.
inline constexpr uint8_t kWaveBanks = 2;
/// Tone channels.
inline constexpr uint8_t kToneChannels = 4;
/// Square channels (tone 1 with the sweep, tone 2).
inline constexpr uint8_t kSquareChannels = 2;

/// What one recorded write does.
enum class WriteKind : uint8_t
{
   Tone,     ///< A tone register byte (reg = hal::audio::ToneReg offset, value = the byte).
   Wave,     ///< One wave RAM word (reg = word index 0-3, value = the word).
   Output,   ///< SOUNDCNT_H routing (value = the halfword).
};

/// One write, in program order.
struct Write
{
   WriteKind kind;   ///< What it writes.
   uint8_t reg;      ///< Register offset or word index.
   uint16_t pad;     ///< 0.
   uint32_t value;   ///< The byte, word or halfword.
};

/// SOUNDCNT_H the backends start with: tones full, FIFO A full to the right, FIFO B full to the left.
inline constexpr uint16_t kSoundcntStereo = 0x210E;
/// SOUNDCNT_H in mono: tones full, both FIFOs at half volume to both sides.
inline constexpr uint16_t kSoundcntMono = 0x3302;
/// SOUNDCNT_H's FIFO reset bits (A bit 11, B bit 15): written once at start-up.
inline constexpr uint16_t kSoundcntFifoResets = 0x8800;
/// SOUNDBIAS at power-on (bias 0x200, 9-bit resolution), as the BIOS leaves it.
inline constexpr uint16_t kSoundbiasPowerOn = 0x0200;

/// One square channel (tone 1 has the sweep).
struct Square
{
   bool on;                  ///< Playing.
   uint8_t volume;           ///< Envelope level, 0-15.
   uint8_t envelope_timer;   ///< 64 Hz ticks to the next envelope step.
   uint8_t duty_step;        ///< Position in the 8-step duty pattern.
   uint16_t length;          ///< Length counter (counts down at 256 Hz when enabled).
   uint16_t frequency;       ///< 11-bit frequency in use (the sweep changes it).
   uint32_t timer;           ///< Cycles to the next duty step.
   uint16_t sweep_shadow;    ///< Sweep: frequency shadow.
   uint8_t sweep_timer;      ///< Sweep: 128 Hz ticks to the next step.
   bool sweep_on;            ///< Sweep: active since the trigger.
};

/// The wave channel.
struct WaveChannel
{
   bool on;             ///< Playing.
   uint8_t position;    ///< Sample index, 0-31 (0-63 with both banks).
   uint16_t length;     ///< Length counter.
   uint16_t frequency;  ///< 11-bit frequency.
   uint32_t timer;      ///< Cycles to the next sample.
};

/// The noise channel.
struct Noise
{
   bool on;                  ///< Playing.
   uint8_t volume;           ///< Envelope level, 0-15.
   uint8_t envelope_timer;   ///< 64 Hz ticks to the next envelope step.
   uint16_t length;          ///< Length counter.
   uint16_t lfsr;            ///< 15-bit shift register.
   uint32_t timer;           ///< Cycles to the next shift.
};

/// The whole model's state (what a state log prints).
struct State
{
   uint8_t regs[kRegisterBytes];                  ///< The register block as last applied.
   uint8_t wave_ram[kWaveBanks][kWaveBankBytes];  ///< Both wave banks.
   Square square[kSquareChannels];                           ///< Tones 1 and 2.
   WaveChannel wave;                              ///< Tone 3.
   Noise noise;                                   ///< Tone 4.
   uint32_t sequencer_cycles;                     ///< Cycles into the current 512 Hz step.
   uint8_t sequencer_step;                        ///< The next step, 0-7.
};

/// The model. Plain data plus functions: one instance per backend (static storage).
class Model
{
public:
   /// Power-on state (registers 0, SOUNDBIAS 0x200, channels off).
   void reset();

   /// Applies one write now.
   /// @param w  the write
   void apply(const Write& w);

   /// The register byte as the hardware would read it back.
   /// @param offset  register offset (hal::audio::ToneReg)
   /// @return the byte
   uint8_t read(uint8_t offset) const;

   /// Renders samples at the FIFO's instants across one frame, then leaves the channels at the frame's end.
   /// @param right    FIFO A bytes, one per sample (nullptr: silence)
   /// @param left     FIFO B bytes, one per sample (nullptr: silence)
   /// @param out      samples x 2 signed 16-bit values, left then right
   /// @param samples  output samples in the frame (the core's frame_samples())
   void render(const int8_t* right, const int8_t* left, int16_t* out, uint16_t samples);

   /// The high-quality mode (CAPSULE_AUDIO_HQ): the same with 16-bit DirectSound input (1/256 of a FIFO step),
   /// no 10-bit DAC: the sum is clamped to 16 bits.
   /// @param right    right DirectSound samples (nullptr: silence)
   /// @param left     left DirectSound samples (nullptr: silence)
   /// @param out      samples x 2 values, left then right
   /// @param samples  the frame's samples x kHqFactor
   void render_hq(const int16_t* right, const int16_t* left, int16_t* out, uint16_t samples);

   /// The state (for state logs and tests).
   /// @return it
   const State& state() const
   {
      return state_;
   }

private:
   /// Runs the channels and the frame sequencer for some cycles.
   /// @param cycles  2^24 Hz cycles
   void run(uint32_t cycles);
   /// One 512 Hz frame sequencer step.
   void step_sequencer();
   /// The tones' contribution to one side, before the bias.
   /// @param left  true: left side, false: right
   /// @return the 10-bit-scale value
   int32_t tone_mix(bool left) const;
   /// Starts a channel (NRx4 bit 7).
   /// @param channel  0-3
   void trigger(uint8_t channel);
   /// The sweep's next frequency from its shadow (may exceed 11 bits: overflow).
   /// @return the frequency
   uint32_t sweep_next() const;

   State state_{};   ///< Everything.
};

/// Writes one frame can record (more are dropped and counted; the core writes a few dozen at most).
inline constexpr uint16_t kMaxWrites = 512;
/// Output samples per side of one frame at most (the high-quality mode).
inline constexpr uint16_t kMaxFrameOutput = audio::kMaxFrameSamples * audio::kHqFactor;
/// Output values per sample (left, right).
inline constexpr uint16_t kOutputSides = 2;

inline constexpr uint32_t kFrameRateX10000 = 597275;   ///< Frames a second x 10,000 (59.7275).
inline constexpr uint32_t kRateRounding = 5000;        ///< Half of the divider: rounds to the nearest.
inline constexpr uint32_t kRateDivider = 10000;        ///< The divider.
/// The model's output rate for a frame of this many samples, Hz: (597,275 x samples + 5,000) / 10,000, the
/// reference driver's rounding of 59.7275 frames a second (224: 13,379).
/// @param samples  samples per frame
/// @return the rate
constexpr uint32_t output_rate(uint16_t samples)
{
   return (kFrameRateX10000 * samples + kRateRounding) / kRateDivider;
}
static_assert(output_rate(audio::kFrameSamples) == audio::kOutputRate, "the reference rate");

/// One rendered frame: what the backends play and what headless captures (format §11: L2 segment, L3 writes, L4
/// output).
struct FrameOut
{
   uint32_t index;                              ///< Frames rendered since start() (1: the first).
   uint8_t segment;                             ///< The segment the core mixed this frame (core::segment()).
   bool restarted;                              ///< restart_dma() ran this frame.
   bool hq;                                     ///< The output came from hq_block() (high-quality mode).
   uint16_t read_position;                      ///< The DMA's position (bytes into a side) the output played from.
   uint16_t segment_samples;                    ///< Samples per side in right / left (the frame's samples).
   int8_t right[audio::kMaxFrameSamples];       ///< That segment's right side.
   int8_t left[audio::kMaxFrameSamples];        ///< Its left side.
   uint16_t write_count;                        ///< Writes recorded this frame.
   uint16_t dropped_writes;                     ///< Writes past kMaxWrites (applied, not recorded).
   Write writes[kMaxWrites];                    ///< The writes, in program order.
   uint16_t output_samples;                     ///< Samples per side in output.
   int16_t output[kOutputSides * kMaxFrameOutput];   ///< Left, right, left, right ...
};

/// The model with what a desktop or headless backend needs around it: the register shadow the core reads back, the
/// frame's writes (applied at the next frame's start), the DMA's read position in the ring, the HQ block.
class Player
{
public:
   /// Starts: model reset, the start-up register state the GBA backend writes (master on, NR50 0x77, 8-bit
   /// resolution, stereo routing), read position 0.
   /// @param ring        the core's ring: right side, then left, contiguous
   /// @param side_bytes  bytes of one side
   /// @param samples     samples per frame (the core's frame_samples(), at most audio::kMaxFrameSamples)
   void start(const int8_t* ring, uint16_t side_bytes, uint16_t samples);

   /// The DMA restart: the read position goes back to the ring's start.
   void restart_dma();

   /// Records SOUNDCNT_H routing.
   /// @param stereo  stereo or mono
   void set_output(bool stereo);

   /// Records a tone register write (the shadow changes now; the model at the frame's start).
   /// @param offset  ToneReg offset
   /// @param value   the byte
   void tone_write(uint8_t offset, uint8_t value);

   /// A register as the core reads it back: the byte last written (NR52: its bit 7 and the model's status).
   /// @param offset  ToneReg offset
   /// @return the byte
   uint8_t tone_read(uint8_t offset) const;

   /// Records a wave RAM write.
   /// @param words  kWaveWords words
   void wave_write(const uint32_t* words);

   /// Takes the frame's high-quality block (copied).
   /// @param right  the frame's samples x kHqFactor
   /// @param left   as many
   void hq_block(const int16_t* right, const int16_t* left);

   /// Finishes the frame: applies its writes, captures the segment, renders the output from the read position (or
   /// the HQ block), advances the read position by one frame.
   /// @param segment  the segment the core mixed this frame
   /// @return the frame (valid until the next finish_frame())
   const FrameOut& finish_frame(uint8_t segment);

   /// The last finished frame.
   /// @return it
   const FrameOut& last() const
   {
      return last_;
   }

   /// The model.
   /// @return it
   const Model& model() const
   {
      return model_;
   }

private:
   /// Records one write for this frame.
   /// @param w  the write
   void record(const Write& w);
   /// A FIFO byte the DMA reads (right side: past its end it reads on into the left side, as the hardware's DMA does;
   /// past the ring: 0).
   /// @param left      which side
   /// @param position  bytes into the side
   /// @return the byte
   int8_t fifo_byte(bool left, uint32_t position) const;

   Model model_{};                             ///< The output model.
   const int8_t* ring_ = nullptr;              ///< The core's ring.
   uint16_t side_bytes_ = 0;                   ///< Bytes of one side.
   uint16_t samples_ = audio::kFrameSamples;   ///< Samples per frame.
   uint32_t read_position_ = 0;                ///< The DMA's position, bytes into a side.
   uint8_t shadow_[kRegisterBytes] = {};       ///< Bytes as written (read back by the core).
   FrameOut pending_{};                        ///< This frame's writes so far.
   FrameOut last_{};                           ///< The last finished frame.
   bool hq_pending_ = false;                   ///< hq_block() came this frame.
   int16_t hq_right_[kMaxFrameOutput] = {};    ///< The HQ block's right side.
   int16_t hq_left_[kMaxFrameOutput] = {};     ///< Its left side.
   uint32_t frames_ = 0;                       ///< Frames finished since start().
};

} // namespace capsule::hal::apu
