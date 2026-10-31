/// @file
/// GBA sound output (capsule/hal/audio.h, D-34 M4 B1b): DirectSound FIFO A (right) and B (left) fed by DMA1 / DMA2
/// from the core's ring in place, timer 0 at one sample per 1,254 cycles (13,379 Hz, 224 samples a frame; other
/// table rates: 280,896 / core::frame_samples()), the tone
/// registers written directly. Linked only when the core's init() calls hal::audio::init().
///
/// Timing (C14a, D-34: the audio clock, capsule/hal/audio.h):
///   - a step is core::vsync() (the DMA counter; none on the first step after init()) then core::vblank() (players,
///     tone channels, the mix of the next segment), timed with timer 1 (free running since system::init()) and
///     VCOUNT: budget(). The VBlank interrupt runs one step on the blank that finishes a game frame (hal_gba.cpp,
///     through audio_hook.h, after the frame's commit, with nested interrupts allowed); rng::spend_blanks() runs the
///     reference's modelled lag blanks as steps at once, from the main loop (spend()). This build's own lag blanks
///     step nothing;
///   - the VCOUNT interrupt at line 150 only points the FIFO DMAs: at the segment the latest step mixed, which
///     after an ordinary frame is the one the DMA reaches anyway (it is restarted only at the ring's wrap, as the
///     reference driver restarts it: the same hardware writes). After a blank without a step (this build's lag) the
///     DMA is restarted at the segment it just played: that frame repeats. While spend() runs its steps, the DMA
///     plays a copy of the segment it was playing (the hold buffer), so the steps may write any segment of the ring:
///     their output is not played (the state stays exact, the reference's lag-time sound is skipped);
///   - timer 0 starts as line 159 begins (the reference driver waits for line 159, then starts it), so the FIFOs
///     play in phase with the frame.
///
/// Debug builds (CAPSULE_ASSERTS) also log every tone and wave RAM write, with the step it belongs to, in an EWRAM
/// ring (g_probe.tone_log) that the test runner reads each frame (format §11.4, L3).
#include "capsule/hal/audio.h"

#include "capsule/audio/core.h"
#include "capsule/core/assert.h"
#include "capsule/core/memory.h"

#include "../common/audio_hook.h"

#include <gba_interrupt.h>

#include <cstddef>
#include <cstdint>

namespace capsule::hal::audio
{

/// What a test runner reads from the GBA build (gba_run.py --read capsule::hal::audio::g_probe): the step count,
/// the budget, and in debug builds the tone write log.
struct Probe
{
   /// Entries the tone write log keeps (a power of two; a frame of the reference songs writes far fewer).
   static constexpr uint32_t kToneLogEntries = 512;

   volatile uint32_t steps;          ///< Steps (core::vblank() calls) since init(): game frames + modelled blanks.
   volatile uint32_t vsyncs;         ///< VCOUNT line-150 interrupts since init() (one a hardware frame).
   volatile uint32_t restarts;       ///< FIFO DMA restarts since init() (ring wraps, repeats, the hold buffer).
   volatile uint32_t last_cycles;    ///< Budget: the latest step, cycles.
   volatile uint32_t max_cycles;     ///< Budget: the longest step.
   volatile uint32_t total_cycles;   ///< Budget: all steps (wraps).
   volatile uint32_t tone_writes;    ///< Debug builds: log entries written so far (wraps; index = count % entries).
#if CAPSULE_ASSERTS
   /// Debug builds: (step & 0xFFFF) << 16 | register offset << 8 | byte; the step is the one the write belongs to
   /// (1-based: a write inside a step, or before it from the main loop, belongs to that step: audio_render's
   /// tones.txt frame); wave RAM bytes use offsets 0x30-0x3F, SOUNDCNT_H 0x22-0x23 (set_output()).
   volatile uint32_t tone_log[kToneLogEntries];
#endif
};

/// The probe (EWRAM: read by the runner, written once per step; 28 B, debug builds 2,076 B with the log).
CAPSULE_EWRAM_BSS Probe g_probe;

/// C14a: the audio clock's counters (gba_run.py --read capsule::hal::audio::g_clock).
struct Clock
{
   volatile uint32_t repeats;   ///< Line-150s that restarted the segment just played (blanks without a step).
   volatile uint32_t held;      ///< Line-150s that played the hold buffer (spend() under way).
   volatile uint32_t spent;     ///< Steps spend() ran (modelled blanks).
   volatile uint32_t batches;   ///< spend() calls.
};

/// The clock's counters (EWRAM, 16 B).
CAPSULE_EWRAM_BSS Clock g_clock;

} // namespace capsule::hal::audio

namespace
{

using namespace capsule::hal::audio;
namespace core = capsule::audio::core;

/// @name I/O registers (offsets from kIoBase)
///@{
constexpr uint32_t kIoBase = 0x04000000;         ///< I/O registers.
constexpr uint32_t kRegDispstat = 0x04;          ///< DISPSTAT.
constexpr uint32_t kRegVcount = 0x06;            ///< VCOUNT.
constexpr uint32_t kRegSoundBlock = 0x60;        ///< First tone register (NR10): ToneReg offsets start here.
constexpr uint32_t kRegSoundcntL = 0x80;         ///< SOUNDCNT_L: NR50 / NR51.
constexpr uint32_t kRegSoundcntH = 0x82;         ///< SOUNDCNT_H: DirectSound routing.
constexpr uint32_t kRegSoundcntX = 0x84;         ///< SOUNDCNT_X: NR52.
constexpr uint32_t kRegSoundbiasH = 0x89;        ///< SOUNDBIAS, high byte.
constexpr uint32_t kRegWaveRam = 0x90;           ///< Wave RAM (the bank NR30 does not play).
constexpr uint32_t kRegFifoA = 0xA0;             ///< FIFO A.
constexpr uint32_t kRegFifoB = 0xA4;             ///< FIFO B.
constexpr uint32_t kRegDma1Sad = 0xBC;           ///< DMA1 source.
constexpr uint32_t kRegDma1Dad = 0xC0;           ///< DMA1 destination.
constexpr uint32_t kRegDma1Cnt = 0xC4;           ///< DMA1 count (low half) and control (high half).
constexpr uint32_t kRegDma1CntH = 0xC6;          ///< DMA1 control (the high half alone).
constexpr uint32_t kDmaStride = 0x0C;            ///< From DMA1's registers to DMA2's.
constexpr uint32_t kRegTm0Count = 0x100;         ///< TM0CNT_L: reload.
constexpr uint32_t kRegTm0Control = 0x102;       ///< TM0CNT_H.
constexpr uint32_t kRegTm1Count = 0x104;         ///< TM1CNT_L: timer 1, free running (system::init()).
///@}

/// @name Register values
///@{
constexpr uint16_t kTimerOn = 0x0080;            ///< TMxCNT_H: running, every cycle.
constexpr uint32_t kCyclesPerFrame = 280896;     ///< Cycles per video frame.
constexpr uint32_t kTimerOverflow = 0x10000u;    ///< Timer 0 counts up from its reload to this.
constexpr uint16_t kDmaOn = 0x8000;              ///< DMAxCNT_H bit 15.
constexpr uint16_t kDmaSpecial = 0x3000;         ///< Start timing 3: the FIFO's request.
constexpr uint16_t kDma32 = 0x0400;              ///< 32-bit units.
constexpr uint16_t kDmaRepeat = 0x0200;          ///< Repeat.
constexpr uint16_t kDmaDstFixed = 0x0040;        ///< Destination fixed.
constexpr uint32_t kDmaRepeatBit = 0x02000000u;  ///< The repeat bit in the 32-bit DMAxCNT.
/// A repeating DMA stops cleanly after a short immediate transfer: enabled, now, 32-bit, destination fixed.
constexpr uint32_t kDmaStopTransfer = (uint32_t{kDmaOn | kDma32 | kDmaDstFixed} << 16) | 4u;
/// FIFO mode: enabled, on the FIFO's request, 32-bit, repeated (the destination is fixed in this mode).
constexpr uint16_t kDmaFifo = kDmaOn | kDmaSpecial | kDma32 | kDmaRepeat;
constexpr uint16_t kSoundcntStereo = 0x210E;      ///< Tones full, FIFO A full right, FIFO B full left, timer 0.
constexpr uint16_t kSoundcntMono = 0x3302;        ///< Tones full, both FIFOs half to both sides, timer 0.
constexpr uint16_t kSoundcntFifoResets = 0x8800;  ///< FIFO A and B reset (start-up only).
constexpr uint8_t kMasterAllOn = 0x8F;            ///< SOUNDCNT_X: master enable, the tone channels' bits.
constexpr uint8_t kBiasKeep = 0x3F;               ///< SOUNDBIAS_H bits kept (the bias level).
constexpr uint8_t kBias8Bit = 0x40;               ///< SOUNDBIAS_H bits 6-7 = 1: 8-bit, 65,536 Hz PWM.
constexpr uint8_t kEnvelopeSilent = 0x08;         ///< NRx2: level 0, rising: the channel is silent but on.
constexpr uint8_t kRestart = 0x80;                ///< NRx4: trigger.
constexpr uint8_t kNr50Full = 0x77;               ///< NR50: both sides at full tone volume.
constexpr uint16_t kDispstatLineMask = 0x00FF;    ///< DISPSTAT bits kept when the VCOUNT line is set.
constexpr int kDispstatLineShift = 8;             ///< DISPSTAT bits 8-15: the VCOUNT interrupt's line.
constexpr uint16_t kVcountLine = 150;             ///< The DMA counter's line (reference driver).
constexpr uint16_t kTimerStartLine = 159;         ///< Timer 0 starts as this line begins.
constexpr uint32_t kRegIme = 0x208;               ///< IME: the interrupt master enable.
///@}

/// FIFO bytes the DMA may read past a frame's samples before the next line 150 restarts it (the ring keeps the same
/// spare bytes after its last segment).
constexpr uint32_t kFifoSlack = core::kRingBytes - core::kSegments * kFrameSamples;

/// @name The audio clock (C14a)
///@{
constexpr uint32_t kHoldBytes = kMaxFrameSamples + kFifoSlack;   ///< Bytes per side of the hold buffer.
constexpr uint8_t kNotRing = 0xFF;                ///< g_playing: the DMA plays the hold buffer.
///@}

/// @name Budget arithmetic
///@{
constexpr uint32_t kLines = 228;                  ///< Scanlines per frame.
constexpr uint32_t kCyclesPerLine = 1232;         ///< Cycles per scanline.
constexpr uint32_t kTimerRange = 0x10000u;        ///< Timer 1 wraps every 65,536 cycles.
constexpr uint32_t kTimerHalf = kTimerRange / 2;  ///< The scanline estimate is closer than this.
///@}

/// @name Debug log
///@{
constexpr int kLogStepShift = 16;                ///< Log entry bits 16-31: the step.
constexpr int kLogRegShift = 8;                  ///< Bits 8-15: the register offset.
constexpr uint32_t kLogStepMask = 0xFFFF;        ///< The step's low 16 bits.
constexpr uint8_t kLogWaveOffset = 0x30;         ///< Wave RAM byte i is logged as offset 0x30 + i.
constexpr uint8_t kLogSoundcntH = 0x22;          ///< SOUNDCNT_H's low byte (high: + 1).
constexpr int kByteBits = 8;                     ///< Bits in a byte.
constexpr uint8_t kWordBytes = 4;                ///< Bytes in a word.
///@}

/// The DMA unit of channel 1 or 2 (0: DMA1, 1: DMA2): its registers' offset from DMA1's.
/// @param n  0 or 1
/// @return the offset
constexpr uint32_t dma_offset(uint32_t n)
{
   return n * kDmaStride;
}

/// A 16-bit I/O register.
/// @param offset  offset from kIoBase
/// @return the register
volatile uint16_t& io16(uint32_t offset)
{
   return *reinterpret_cast<volatile uint16_t*>(kIoBase + offset);
}

/// A 32-bit I/O register.
/// @param offset  offset from kIoBase
/// @return the register
volatile uint32_t& io32(uint32_t offset)
{
   return *reinterpret_cast<volatile uint32_t*>(kIoBase + offset);
}

/// The hold buffer (C14a): a copy of the segment the DMA played when spend() began, played while its steps run.
struct alignas(core::kRingAlign) Hold
{
   int8_t right[kHoldBytes];   ///< Right side.
   int8_t left[kHoldBytes];    ///< Left side.
};

CAPSULE_EWRAM_BSS Hold g_hold;          ///< The hold buffer (EWRAM: the DMA reads it a few words at a time).
const core::Ring* g_core_ring = nullptr;   ///< The core's ring (init()).
volatile uint8_t g_playing = 0;         ///< The segment the DMA plays since the latest line 150 (kNotRing: hold).
volatile uint8_t g_holding = 0;         ///< spend() is running its steps: the DMA plays the hold buffer.
/// The next step is the first since init(): it has no vsync() (the timer starts at line 159, so the first vertical
/// blank comes before the first line 150, as the reference driver's).
bool g_first_step = false;

/// An 8-bit I/O register.
/// @param offset  offset from kIoBase
/// @return the register
volatile uint8_t& io8(uint32_t offset)
{
   return *reinterpret_cast<volatile uint8_t*>(kIoBase + offset);
}

#if CAPSULE_ASSERTS
/// Debug builds: one log entry for the step under way (or the next one, from the main loop).
/// @param offset  register offset (ToneReg, or the log's wave / SOUNDCNT_H offsets)
/// @param value   the byte
void log_write(uint8_t offset, uint8_t value)
{
   const uint32_t n = g_probe.tone_writes;
   g_probe.tone_log[n % Probe::kToneLogEntries] =
      (((g_probe.steps + 1) & kLogStepMask) << kLogStepShift) | (uint32_t{offset} << kLogRegShift) | value;
   g_probe.tone_writes = n + 1;
}
#endif

/// Stops a FIFO DMA (if it repeats: a short immediate transfer ends the repeat cleanly), then turns it off.
/// @param n  0: DMA1, 1: DMA2
void stop_dma(uint32_t n)
{
   if ((io32(kRegDma1Cnt + dma_offset(n)) & kDmaRepeatBit) != 0)
   {
      io32(kRegDma1Cnt + dma_offset(n)) = kDmaStopTransfer;
   }
   io16(kRegDma1CntH + dma_offset(n)) = kDma32;
}

/// Interrupts off for a scope (RAII): the previous IME comes back at its end.
struct ImeOff
{
   const uint16_t before;   ///< IME when the scope began.

   /// Turns interrupts off.
   ImeOff() : before(io16(kRegIme))
   {
      io16(kRegIme) = 0;
   }
   ImeOff(const ImeOff&) = delete;              ///< Not copyable.
   ImeOff& operator=(const ImeOff&) = delete;   ///< Not copyable.

   /// Restores IME.
   ~ImeOff()
   {
      io16(kRegIme) = before;
   }
};

/// Starts both FIFO DMAs at a segment: each is stopped (a 4-word immediate transfer ends a repeating one), its
/// source set, and enabled again in FIFO mode (enabling reloads the source). At the ring's start this is the
/// reference driver's restart.
/// @param right  the right side's first byte
/// @param left   the left side's first byte
void start_fifo_dma(const int8_t* right, const int8_t* left)
{
   stop_dma(0);
   stop_dma(1);
   io32(kRegDma1Sad) = reinterpret_cast<uint32_t>(right);
   io32(kRegDma1Sad + dma_offset(1)) = reinterpret_cast<uint32_t>(left);
   io16(kRegDma1CntH) = kDmaFifo;
   io16(kRegDma1CntH + dma_offset(1)) = kDmaFifo;
   g_probe.restarts = g_probe.restarts + 1;
}

/// VCOUNT interrupt (line 150, IME off): points the FIFO DMAs at what this hardware frame plays (the file's
/// "Timing"): the hold buffer while spend() runs; else the segment the latest step mixed, restarting the DMA only
/// when that is not the segment it reaches by itself (the ring's wrap, a repeat after a blank without a step).
void on_vcount()
{
   g_probe.vsyncs = g_probe.vsyncs + 1;
   if (g_holding != 0)
   {
      start_fifo_dma(g_hold.right, g_hold.left);
      g_clock.held = g_clock.held + 1;
      g_playing = kNotRing;
      return;
   }
   const uint8_t target = core::segment();
   const uint8_t playing = g_playing;
   if (playing == kNotRing || target != playing + 1)
   {
      if (target == playing)
      {
         g_clock.repeats = g_clock.repeats + 1;
      }
      const uint32_t at = uint32_t{target} * core::frame_samples();
      start_fifo_dma(g_core_ring->right + at, g_core_ring->left + at);
   }
   g_playing = target;
}

/// One step of the audio clock (audio_hook.h; hal_gba.cpp runs it after a game frame's commit with IME on, spend()
/// from the main loop): core::vsync() (not on the first step), core::vblank(), timed.
void vblank_step()
{
   const uint16_t t0 = io16(kRegTm1Count);
   const uint16_t l0 = io16(kRegVcount);
   if (!g_first_step)
   {
      core::vsync();
   }
   g_first_step = false;
   core::vblank();
   const uint16_t t1 = io16(kRegTm1Count);
   const uint16_t l1 = io16(kRegVcount);

   // Timer 1 counts 16 bits; the scanlines (1,232 cycles each) tell how many times it wrapped.
   const uint32_t coarse = ((l1 + kLines - l0) % kLines) * kCyclesPerLine;
   uint32_t cycles = static_cast<uint16_t>(t1 - t0);
   while (cycles + kTimerHalf < coarse)
   {
      cycles += kTimerRange;
   }
   g_probe.last_cycles = cycles;
   if (cycles > g_probe.max_cycles)
   {
      g_probe.max_cycles = cycles;
   }
   g_probe.total_cycles = g_probe.total_cycles + cycles;
   g_probe.steps = g_probe.steps + 1;
}

/// Starts the hold (IME off): the segment the DMA plays now is copied into the hold buffer, with the spare bytes the
/// FIFO may read after it, and the DMA plays the copy from here on. A second spend() before the next line 150 finds
/// the hold playing already.
void hold_begin()
{
   const ImeOff quiet;
   const uint8_t playing = g_playing;
   if (playing != kNotRing)
   {
      const uint32_t from = uint32_t{playing} * core::frame_samples();
      uint32_t bytes = core::frame_samples() + kFifoSlack;
      if (from + bytes > uint32_t{core::kRingBytes})
      {
         bytes = core::kRingBytes - from;   // a rate whose segments fill the ring: no spare bytes after the last one
      }
      // Word copies (segments start on words; every rate's frame and the spare bytes are whole words)
      const auto* right = reinterpret_cast<const uint32_t*>(g_core_ring->right + from);
      const auto* left = reinterpret_cast<const uint32_t*>(g_core_ring->left + from);
      auto* hold_right = reinterpret_cast<uint32_t*>(g_hold.right);
      auto* hold_left = reinterpret_cast<uint32_t*>(g_hold.left);
      for (uint32_t i = 0; i < bytes / kWordBytes; ++i)
      {
         hold_right[i] = right[i];
         hold_left[i] = left[i];
      }
      start_fifo_dma(g_hold.right, g_hold.left);
      g_playing = kNotRing;
   }
   g_holding = 1;
}

/// The modelled blanks of rng::spend_blanks() (capsule/hal/audio.h): one step each, now, from the main loop, while
/// the DMA plays the hold buffer (their output is not played). The VBlank interrupt runs no step meanwhile: it
/// steps only on the blank that finishes a game frame, and the main loop finishes none while it is here.
/// @param blanks  the blanks
void spend(uint32_t blanks)
{
   hold_begin();
   for (uint32_t i = 0; i < blanks; ++i)
   {
      vblank_step();
   }
   g_clock.spent = g_clock.spent + blanks;
   g_clock.batches = g_clock.batches + 1;
   asm volatile("" ::: "memory");   // every step's stores land before the line-150 handler may read the segment
   g_holding = 0;
}

} // namespace

namespace capsule::hal::audio
{

void init(const core::Ring& ring)
{
   static_assert(offsetof(core::Ring, left) == sizeof(core::Ring::right), "the ring is right side, then left");

   // DirectSound: both FIFO DMAs stopped, the routing and the 8-bit PWM, the ring as the DMAs' sources.
   stop_dma(0);
   stop_dma(1);
   io8(kRegSoundcntX) = kMasterAllOn;
   io16(kRegSoundcntH) = kSoundcntStereo | kSoundcntFifoResets;
   io8(kRegSoundbiasH) = static_cast<uint8_t>((io8(kRegSoundbiasH) & kBiasKeep) | kBias8Bit);
   g_core_ring = &ring;
   g_playing = 0;
   g_holding = 0;
   g_first_step = true;
   io32(kRegDma1Sad) = reinterpret_cast<uint32_t>(ring.right);
   io32(kRegDma1Dad) = kIoBase + kRegFifoA;
   io32(kRegDma1Sad + dma_offset(1)) = reinterpret_cast<uint32_t>(ring.left);
   io32(kRegDma1Dad + dma_offset(1)) = kIoBase + kRegFifoB;

   // The tone channels silent and on, both sides at full tone volume, nothing routed yet (the reference driver's
   // start-up writes).
   io16(kRegSoundcntL) = 0;
   io8(kRegSoundBlock + static_cast<uint32_t>(ToneReg::Nr12)) = kEnvelopeSilent;
   io8(kRegSoundBlock + static_cast<uint32_t>(ToneReg::Nr22)) = kEnvelopeSilent;
   io8(kRegSoundBlock + static_cast<uint32_t>(ToneReg::Nr42)) = kEnvelopeSilent;
   io8(kRegSoundBlock + static_cast<uint32_t>(ToneReg::Nr14)) = kRestart;
   io8(kRegSoundBlock + static_cast<uint32_t>(ToneReg::Nr24)) = kRestart;
   io8(kRegSoundBlock + static_cast<uint32_t>(ToneReg::Nr44)) = kRestart;
   io8(kRegSoundBlock + static_cast<uint32_t>(ToneReg::Nr30)) = 0;
   io8(kRegSoundBlock + static_cast<uint32_t>(ToneReg::Nr50)) = kNr50Full;

   // Timer 0 stopped and loaded (one sample per 280,896 / samples cycles: 1,254 at 224 a frame), both DMAs in
   // FIFO mode, then the timer starts as line 159 begins.
   io16(kRegTm0Control) = 0;
   io16(kRegTm0Count) = static_cast<uint16_t>(kTimerOverflow - kCyclesPerFrame / core::frame_samples());
   io16(kRegDma1CntH) = kDmaFifo;
   io16(kRegDma1CntH + dma_offset(1)) = kDmaFifo;
   while (io16(kRegVcount) == kTimerStartLine)
   {
   }
   while (io16(kRegVcount) != kTimerStartLine)
   {
   }
   io16(kRegTm0Control) = kTimerOn;

   // Line 150 points the DMA; the step runs at the blank that finishes a game frame, and in spend().
   io16(kRegDispstat) =
      static_cast<uint16_t>((io16(kRegDispstat) & kDispstatLineMask) | (kVcountLine << kDispstatLineShift));
   irqSet(IRQ_VCOUNT, on_vcount);
   irqEnable(IRQ_VCOUNT);
   hook::g_vblank_step = vblank_step;
   hook::g_spend_step = spend;
}

void restart_dma()
{
   // C14a: the core's DMA counter runs on the audio clock (in the step); the hardware DMA follows the segment the
   // core mixed at each line 150 (on_vcount()), which restarts it at the ring's start where the counter wraps.
}

void set_output(bool stereo)
{
   const uint16_t value = stereo ? kSoundcntStereo : kSoundcntMono;
   io16(kRegSoundcntH) = value;
#if CAPSULE_ASSERTS
   log_write(kLogSoundcntH, static_cast<uint8_t>(value & UINT8_MAX));
   log_write(kLogSoundcntH + 1, static_cast<uint8_t>(value >> kByteBits));
#endif
}

void tone_write(ToneReg r, uint8_t v)
{
   io8(kRegSoundBlock + static_cast<uint32_t>(r)) = v;
#if CAPSULE_ASSERTS
   log_write(static_cast<uint8_t>(r), v);
#endif
}

uint8_t tone_read(ToneReg r)
{
   return io8(kRegSoundBlock + static_cast<uint32_t>(r));
}

void wave_write(const uint32_t words[kWaveWords])
{
   volatile uint32_t* ram = &io32(kRegWaveRam);
   for (uint8_t i = 0; i < kWaveWords; ++i)
   {
      ram[i] = words[i];
#if CAPSULE_ASSERTS
      for (uint8_t b = 0; b < kWordBytes; ++b)
      {
         log_write(static_cast<uint8_t>(kLogWaveOffset + i * kWordBytes + b),
                   static_cast<uint8_t>((words[i] >> (b * kByteBits)) & UINT8_MAX));
      }
#endif
   }
}

void frame_done()
{
   // The hardware plays the ring and the tone registers itself.
}

void hq_block(const int16_t*, const int16_t*)
{
   // Desktop only.
}

Budget budget()
{
   return Budget{g_probe.last_cycles, g_probe.max_cycles, g_probe.total_cycles, g_probe.steps};
}

} // namespace capsule::hal::audio
