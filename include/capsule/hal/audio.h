/// @file
/// Sound output (D-34, M4 B1b): what the audio core (capsule/audio/core.h) calls to reach the hardware, one
/// implementation per target, chosen at link time like the rest of the HAL.
///
/// The core owns the mix ring (8-bit, 7 segments of one frame's samples per side) and every driver state; a backend
/// plays the ring and the four tone channels.
///
/// The audio clock (C14a, D-34; the developer's decision on R10 H1): the core steps on the reference game's clock on
/// every target, exactly as the gameplay generator does (capsule/core/random.h): once per game frame, plus once for
/// each modelled lag blank of the reference (rng::spend_blanks(), which runs them at once, in program order). A step
/// is core::vsync() (the DMA counter; none on the first step after init()) then core::vblank(). So a sound call made
/// in game frame N lands on the same step everywhere, a wait that polls the driver ends on the same game frame on
/// every target, and the state is the same frame by frame, whatever this build's own lag:
///   - GBA (`src/hal/gba/audio_gba.cpp`): DirectSound FIFO A (right) and B (left) fed by DMA1 / DMA2 from the ring
///     in place, timer 0 at one sample per 1,254 cycles, the tone registers themselves. The VBlank interrupt steps
///     once on the blank that finishes a game frame (after its video commit, hal_gba.cpp, nested interrupts
///     allowed); this build's own lag blanks step nothing, and the VCOUNT interrupt at line 150, which only points
///     the DMA, then repeats the segment just played (a 1-frame repeat on a frame the reference does not lag).
///     rng::spend_blanks() runs the modelled steps from the main loop while the DMA plays a copy of the segment it
///     was playing (the hold buffer): their output is not played, the state stays exact;
///   - desktop (`src/hal/sdl2/audio_sdl2.cpp`) and headless (`src/hal/headless/audio_headless.cpp`): the integer
///     output model of `src/hal/common/apu_model.h` (tone writes applied at the frame's start, D-34 answer 5) plays
///     the ring as the DMA would; system::update() runs one step and frame_done() after the frame's game work,
///     rng::spend_blanks() one more for each modelled blank (desktop renders those frames but does not queue them,
///     as the GBA does not play them). The first step after init() has no vsync(): the GBA backend's init() returns
///     at line 159, so there the first vertical blank comes before the first line 150. Desktop sends the model's
///     signed 16-bit stereo at 13,379 Hz to an SDL audio device (SDL converts to the device's rate); headless keeps
///     each frame for tests and apps/audio_render;
///   - Dreamcast (`src/hal/dc/audio_dc.cpp`): silent; the core still steps on the same clock, so timing is the same.
///
/// Nothing runs until the core's init() calls init(): a program that never starts audio links none of it.
#pragma once

#include <cstdint>

namespace capsule::audio::core
{
struct Ring;   // capsule/audio/core.h: the core's mix ring (right side, then left)
}

namespace capsule::hal::audio
{

/// Samples mixed per side each frame at the reference rate (13,379 Hz: 280,896 cycles / 1,254). The table's rate
/// may be another of the 12 steps (core::frame_samples(): 96-704); every backend follows the core's count.
inline constexpr uint16_t kFrameSamples = 224;
/// The most samples a frame can have (the 42,048 Hz step).
inline constexpr uint16_t kMaxFrameSamples = 704;
/// Output rate of the desktop and headless model at the reference rate, Hz (the GBA timer gives 2^24 / 1,254 =
/// 13,378.96 Hz; other rates: (597,275 x samples + 5,000) / 10,000, the reference driver's rounding).
inline constexpr uint32_t kOutputRate = 13379;
/// The optional high-quality desktop mode (CAPSULE_AUDIO_HQ) mixes this many times the samples per frame.
inline constexpr uint16_t kHqFactor = 4;
/// 32-bit words of one wave pattern (32 4-bit samples).
inline constexpr uint8_t kWaveWords = 4;

/// A tone (PSG) register: its offset from 0x04000060, the GBA's sound register block. The core writes them in the
/// original order; GBA writes the register, the model records it for the frame's start.
enum class ToneReg : uint8_t
{
   Nr10 = 0x00,         ///< SOUND1CNT_L: tone 1 sweep.
   Nr11 = 0x02,         ///< SOUND1CNT_H low: tone 1 duty and length.
   Nr12 = 0x03,         ///< SOUND1CNT_H high: tone 1 envelope.
   Nr13 = 0x04,         ///< SOUND1CNT_X low: tone 1 frequency, low 8 bits.
   Nr14 = 0x05,         ///< SOUND1CNT_X high: tone 1 frequency high 3 bits, length enable, trigger.
   Nr21 = 0x08,         ///< SOUND2CNT_L low: tone 2 duty and length.
   Nr22 = 0x09,         ///< SOUND2CNT_L high: tone 2 envelope.
   Nr23 = 0x0C,         ///< SOUND2CNT_H low: tone 2 frequency, low 8 bits.
   Nr24 = 0x0D,         ///< SOUND2CNT_H high: tone 2 frequency high bits, length enable, trigger.
   Nr30 = 0x10,         ///< SOUND3CNT_L: wave on, bank size, bank number.
   Nr31 = 0x12,         ///< SOUND3CNT_H low: wave length.
   Nr32 = 0x13,         ///< SOUND3CNT_H high: wave volume.
   Nr33 = 0x14,         ///< SOUND3CNT_X low: wave frequency, low 8 bits.
   Nr34 = 0x15,         ///< SOUND3CNT_X high: wave frequency high bits, length enable, trigger.
   Nr41 = 0x18,         ///< SOUND4CNT_L low: noise length.
   Nr42 = 0x19,         ///< SOUND4CNT_L high: noise envelope.
   Nr43 = 0x1C,         ///< SOUND4CNT_H low: noise shift, width, divisor.
   Nr44 = 0x1D,         ///< SOUND4CNT_H high: noise length enable, trigger.
   Nr50 = 0x20,         ///< SOUNDCNT_L low: tone master volume right (bits 0-2) and left (bits 4-6).
   Nr51 = 0x21,         ///< SOUNDCNT_L high: tone channel routing, right (bits 0-3) and left (bits 4-7).
   Nr52 = 0x24,         ///< SOUNDCNT_X: master enable (bit 7), channel status (bits 0-3, read only).
   SoundbiasH = 0x29,   ///< SOUNDBIAS high byte: amplitude resolution (bits 6-7), bias bits 7-8.
};

/// Starts the output (called by the core's init(), once, after the ring is cleared). GBA: DMA1 -> FIFO A from
/// ring.right, DMA2 -> FIFO B from ring.left, SOUNDCNT_H stereo (A right, B left, both full, tones full),
/// SOUNDCNT_X master on, SOUNDBIAS 8-bit (65,536 Hz PWM), the tone channels silenced with NR50 0x77, the VCOUNT
/// interrupt at line 150 and the VBlank step installed; timer 0 (reload -280,896 / core::frame_samples(): -1,254 at
/// the reference rate) starts as line 159 begins, as the
/// reference driver starts it. Desktop: the SDL audio device; headless: the capture. Every backend then runs the
/// core each vertical blank.
/// @param ring  the core's ring (it stays where it is: the GBA DMA reads it in place)
void init(const capsule::audio::core::Ring& ring);

/// The core's DMA counter wrapped (it calls this from vsync() every 7th step): the playback goes back to the ring's
/// start. The model: its read position goes back to 0. GBA (C14a): nothing at once; the VCOUNT interrupt at line
/// 150 starts the FIFO DMAs at the segment the core mixed last, which restarts them at the ring's start here (each
/// repeating DMA is stopped, a 4-word immediate transfer then off, and restarted in FIFO mode).
void restart_dma();

/// Routes the sampled output (SOUNDCNT_H). Stereo: FIFO A right, FIFO B left, both at full volume; mono: both
/// FIFOs to both sides at half volume. The tone ratio stays full. (The core centres the tone pan itself.)
/// @param stereo  true: stereo, false: mono
void set_output(bool stereo);

/// Writes a tone register, in program order. GBA: the register; the model applies the frame's writes at its start.
/// @param r  the register
/// @param v  the byte
void tone_write(ToneReg r, uint8_t v);

/// Reads a tone register as the hardware returns it (the core reads NRx2, NR10, NR43, NR51 and SOUNDBIAS_H back).
/// GBA: the register; the model: the last byte written (NR52: bit 7 as written, bits 0-3 the channel status).
/// @param r  the register
/// @return its value
uint8_t tone_read(ToneReg r);

/// Writes wave RAM (the bank the CPU sees: the one NR30 does not select for playback). Ordered with tone_write().
/// @param words  the pattern, kWaveWords little-endian words (first sample in the high nibble of the first byte)
void wave_write(const uint32_t words[kWaveWords]);

/// Desktop / headless: the segment vblank() just mixed and this frame's tone writes are final; the model renders the
/// frame's output (it plays the ring from its read position as the DMA does). GBA: nothing (the hardware plays).
void frame_done();

/// High-quality desktop mode (CAPSULE_AUDIO_HQ, desktop and headless only): the core's HQ mixer hands its frame here
/// before frame_done(), kHqFactor * core::frame_samples() samples per side, in 1/256 of an 8-bit ring step (16-bit
/// accumulation, clamped). frame_done() then plays these instead of the ring, and the tone model runs at the same
/// rate. Never in parity runs. GBA and Dreamcast ignore it.
/// @param right  the right side's samples
/// @param left   the left side's samples
void hq_block(const int16_t* right, const int16_t* left);

/// The cost of the core's step (GBA: measured with timer 1, 16.78 MHz, around each step, the modelled blanks'
/// included; other targets: zero). For the budget report (apps/test_audio, docs).
struct Budget
{
   uint32_t last_cycles;    ///< The latest step, cycles.
   uint32_t max_cycles;     ///< The longest step since init(), cycles.
   uint32_t total_cycles;   ///< All steps since init(), cycles (wraps).
   uint32_t steps;          ///< Steps measured since init() (game frames + modelled blanks).
};

/// The measured cost so far.
/// @return the counters
Budget budget();

} // namespace capsule::hal::audio
