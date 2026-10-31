// Tests of the audio backends that do not need a GBA (D-34 M4 B1b): the headless backend's capture (segments, DMA
// read position and restarts, ordered tone writes applied at the next frame's start, read-back, mono routing, the
// high-quality block) driven by the core stub (tests/support/audio_core_stub.h), and the integer output model
// (src/hal/common/apu_model.h): FIFO scaling, bias and resolution, the tone channels' period, duty, envelope, length,
// sweep, wave and noise.
#include "capsule/audio/core.h"
#include "capsule/hal/audio.h"

#include "../src/hal/common/apu_model.h"
#include "../src/hal/common/audio_hook.h"
#include "../src/hal/headless/audio_headless.h"
#include "support/audio_core_stub.h"

#include <cstdio>
#include <cstdlib>

namespace capsule::hal::system
{
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   std::printf("ASSERT %s:%d: %s\n", file, line, expr);
   std::abort();
}
} // namespace capsule::hal::system

namespace
{

using namespace capsule::hal;
namespace stub_core = capsule::audio::stub_core;
using audio::ToneReg;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                                    \
   do                                                                                                                  \
   {                                                                                                                   \
      ++g_checks;                                                                                                      \
      if (!(cond))                                                                                                     \
      {                                                                                                                \
         ++g_failures;                                                                                                 \
         std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)

constexpr uint16_t kN = audio::kFrameSamples;
constexpr int kDmaPeriod = 7;

/// Runs one blank through the installed hook, as system::update() does.
void blank()
{
   audio::hook::vblank();
}

// The hook is empty until init(); init() installs the step. The first blank runs no vsync() (as on the GBA, where
// init() returns at line 159): it mixes segment 0 (counter 0) and plays from 0; the DMA then restarts at blanks 2,
// 9, 16 ...; the read position goes 0, 224, ... and back; the captured segment is the one the core mixed.
void capture_follows_the_dma_counter()
{
   CHECK(audio::hook::g_vblank_step == nullptr);
   CHECK(audio::hook::g_spend_step == nullptr);
   audio::hook::spend(3);   // before init(): nothing to step
   CHECK(stub_core::frames() == 0);
   CHECK(!audio::capture::started());
   stub_core::init();
   CHECK(audio::capture::started());
   CHECK(audio::hook::g_vblank_step != nullptr);
   CHECK(audio::hook::g_spend_step != nullptr);

   for (uint32_t frame = 1; frame <= 3 * kDmaPeriod; ++frame)
   {
      blank();
      const apu::FrameOut& f = audio::capture::last();
      CHECK(f.index == frame);
      if (frame == 1)
      {
         CHECK(!f.restarted && f.read_position == 0 && f.segment == 0);
      }
      else
      {
         // counter 7 right after a restart: segment 1, then 2 ... 6, then 0 (counter 1)
         const uint32_t k = (frame - 2) % kDmaPeriod;
         CHECK(f.restarted == (k == 0));
         CHECK(f.read_position == k * kN);
         CHECK(f.segment == (k == kDmaPeriod - 1 ? 0 : k + 1));
      }
      bool bytes_ok = true;
      for (uint32_t i = 0; i < kN; ++i)
      {
         bytes_ok = bytes_ok && f.right[i] == stub_core::pattern(frame, i) &&
                    f.left[i] == static_cast<int8_t>(-stub_core::pattern(frame, i));
      }
      CHECK(bytes_ok);
      CHECK(f.output_samples == kN && !f.hq);
   }
}

// Each output sample of a stereo frame with no tone routed is the played FIFO byte x 4 (10-bit) x 64 (16-bit): the
// right side plays FIFO A, the left FIFO B. The DMA plays the segment the previous blank mixed (also across a
// restart: segment 0 is mixed when the counter is 1, the frame before the restart).
void output_plays_the_previous_segment()
{
   constexpr int32_t kScale = 4 * 64;
   for (int n = 0; n < kDmaPeriod + 1; ++n)
   {
      const apu::FrameOut before = audio::capture::last();
      blank();
      const apu::FrameOut& f = audio::capture::last();
      CHECK(f.read_position == uint32_t{before.segment} * kN);
      bool ok = true;
      for (uint32_t i = 0; i < kN; ++i)
      {
         ok = ok && f.output[2 * i + 1] == before.right[i] * kScale && f.output[2 * i] == before.left[i] * kScale;
      }
      CHECK(ok);
   }
}

// C14a (the audio clock): modelled lag blanks (rng::spend_blanks() -> hook::spend()) are steps run at once, each a
// full blank (vsync(), vblank(), the captured frame): the DMA counter, the segments, the read position and every
// byte go on exactly as the same number of ordinary blanks would take them.
void modelled_blanks_step_now()
{
   constexpr uint32_t kSpent = 9;   // more than the ring's segments: a restart inside the batch
   const uint32_t frames = stub_core::frames();
   const uint32_t index = audio::capture::last().index;
   audio::hook::spend(0);
   CHECK(stub_core::frames() == frames);
   audio::hook::spend(kSpent);
   CHECK(stub_core::frames() == frames + kSpent);
   CHECK(audio::capture::last().index == index + kSpent);
   for (uint32_t n = 0; n < kDmaPeriod; ++n)
   {
      blank();
      const uint32_t frame = stub_core::frames();
      const apu::FrameOut& f = audio::capture::last();
      CHECK(f.index == frame);
      const uint32_t k = (frame - 2) % kDmaPeriod;   // the sequence capture_follows_the_dma_counter() checks
      CHECK(f.restarted == (k == 0));
      CHECK(f.read_position == k * kN);
      CHECK(f.segment == (k == kDmaPeriod - 1 ? 0 : k + 1));
      bool bytes_ok = true;
      for (uint32_t i = 0; i < kN; ++i)
      {
         bytes_ok = bytes_ok && f.right[i] == stub_core::pattern(frame, i);
      }
      CHECK(bytes_ok);
   }
}

// Tone writes: read back at once, recorded in order, applied at the next frame's start (the channel's status bit
// shows in NR52 only after the frame that applied them).
void tone_writes_apply_at_the_frame_start()
{
   // Run to the frame before the stub's next note (it writes at frames 1, 31, 61 ...).
   while (stub_core::frames() % stub_core::kNoteEvery != 0)
   {
      blank();
   }
   blank();   // the note's frame: 5 writes
   const apu::FrameOut& f = audio::capture::last();
   CHECK(f.write_count == 5);
   CHECK(f.writes[0].kind == apu::WriteKind::Tone && f.writes[0].reg == static_cast<uint8_t>(ToneReg::Nr21));
   CHECK(f.writes[4].reg == static_cast<uint8_t>(ToneReg::Nr24) && f.writes[4].value == 0x87);
   CHECK(audio::tone_read(ToneReg::Nr22) == 0xF1);
   CHECK(audio::tone_read(ToneReg::Nr51) == 0x22);
   CHECK((audio::tone_read(ToneReg::Nr52) & 0x02) != 0);   // applied in this frame's render: tone 2 on
   const apu::State& s = audio::capture::model().state();
   CHECK(s.square[1].on && s.square[1].frequency == 1792);
}

// Mono: both FIFOs at half volume to both sides.
void mono_mixes_both_fifos_at_half()
{
   while (stub_core::frames() < stub_core::kMonoAt)
   {
      blank();
   }
   CHECK(audio::tone_read(static_cast<ToneReg>(apu::kSoundcntH)) == (apu::kSoundcntMono & 0xFF));
   blank();   // the first frame rendered with the mono routing
   // pattern + its negation: the FIFO sum is 0 (and the tone of the frame-91 note has decayed: 15 ticks of 64 Hz)
   const apu::FrameOut& f = audio::capture::last();
   bool equal = true;
   for (uint32_t i = 0; i < kN; ++i)
   {
      equal = equal && f.output[2 * i] == f.output[2 * i + 1];
   }
   CHECK(equal);
}

// The high-quality block replaces the ring for one frame: 896 samples per side, the block's values at full
// volume (the stub has switched to mono: half).
void hq_block_replaces_the_ring()
{
   // past the decay of the stub's latest note (15 ticks of 64 Hz from its frame): no tone in the output
   constexpr uint32_t kQuiet = 20;
   while (stub_core::frames() % stub_core::kNoteEvery != kQuiet)
   {
      blank();
   }
   int16_t right[apu::kMaxFrameOutput];
   int16_t left[apu::kMaxFrameOutput];
   for (uint32_t i = 0; i < apu::kMaxFrameOutput; ++i)
   {
      right[i] = static_cast<int16_t>(i * 8);
      left[i] = 0;
   }
   audio::hq_block(right, left);
   blank();
   const apu::FrameOut& f = audio::capture::last();
   CHECK(f.hq && f.output_samples == audio::kFrameSamples * audio::kHqFactor);
   CHECK(f.output[2 * 100] == (100 * 8) / 2);
   blank();
   CHECK(!audio::capture::last().hq);
}

// --- the model alone ------------------------------------------------------------------------------------------

apu::Model g_model;
int16_t g_out[2 * kN];

/// A model with master on, NR50 full, 8-bit resolution, stereo routing (the backends' start state).
void fresh_model()
{
   g_model.reset();
   g_model.apply({apu::WriteKind::Tone, static_cast<uint8_t>(ToneReg::Nr52), 0, 0x80});
   g_model.apply({apu::WriteKind::Tone, static_cast<uint8_t>(ToneReg::Nr50), 0, 0x77});
   g_model.apply({apu::WriteKind::Tone, static_cast<uint8_t>(ToneReg::SoundbiasH), 0, 0x42});
   g_model.apply({apu::WriteKind::Output, 0, 0, apu::kSoundcntStereo});
}

void tone(ToneReg r, uint8_t v)
{
   g_model.apply({apu::WriteKind::Tone, static_cast<uint8_t>(r), 0, v});
}

// The 8-bit resolution cuts the 10-bit sum's 2 low bits; the DAC clamps at the 10-bit range.
void model_bias_resolution_and_clamp()
{
   fresh_model();
   int8_t a[kN];
   for (uint32_t i = 0; i < kN; ++i)
   {
      a[i] = static_cast<int8_t>(i < 128 ? i : -128);
   }
   g_model.render(a, nullptr, g_out, kN);
   CHECK(g_out[2 * 3 + 1] == 3 * 4 * 64);
   CHECK(g_out[2 * 127 + 1] == 127 * 4 * 64);
   CHECK(g_out[2 * 200 + 1] == -128 * 4 * 64);   // -512: 0 at the DAC, the lowest output
   CHECK(g_out[2 * 3] == 0);                      // FIFO A is not routed left in stereo
   // 9-bit resolution keeps the bit an 8-bit mode cuts: with tones only (odd 10-bit values) the two differ
}

/// Counts level changes of one side over a frame (a square's edges).
int edges(int side)
{
   int n = 0;
   for (uint32_t i = 1; i < kN; ++i)
   {
      n += g_out[2 * i + static_cast<uint32_t>(side)] != g_out[2 * (i - 1) + static_cast<uint32_t>(side)] ? 1 : 0;
   }
   return n;
}

// A 50 % square at frequency 1536 lasts (2048 - 1536) x 16 x 8 = 65,536 cycles a period: 4.3 periods a frame,
// about 8 edges, routed right only; level 15 x 2 x 8 = 240 on the 10-bit scale (x 64).
void model_square_period_duty_and_routing()
{
   fresh_model();
   tone(ToneReg::Nr51, 0x02);   // tone 2 right
   tone(ToneReg::Nr21, 0x80);
   tone(ToneReg::Nr22, 0xF0);
   tone(ToneReg::Nr23, 0x00);
   tone(ToneReg::Nr24, 0x86);
   g_model.render(nullptr, nullptr, g_out, kN);
   const int e = edges(1);
   CHECK(e >= 7 && e <= 10);
   CHECK(edges(0) == 0);
   int16_t high = 0;
   for (uint32_t i = 0; i < kN; ++i)
   {
      high = g_out[2 * i + 1] > high ? g_out[2 * i + 1] : high;
   }
   CHECK(high == 15 * 2 * 8 * 64);
}

// An envelope falling every tick (64 Hz) from 15 is silent after 15 ticks (about 14 frames); a length of 1 (63
// written) ends the channel within one 256 Hz tick when enabled.
void model_envelope_and_length()
{
   fresh_model();
   tone(ToneReg::Nr51, 0x22);
   tone(ToneReg::Nr21, 0x80);
   tone(ToneReg::Nr22, 0xF1);
   tone(ToneReg::Nr24, 0x87);
   for (int f = 0; f < 16; ++f)
   {
      g_model.render(nullptr, nullptr, g_out, kN);
   }
   CHECK(g_model.state().square[1].volume == 0);

   fresh_model();
   tone(ToneReg::Nr21, 0x80 | 63);
   tone(ToneReg::Nr22, 0xF0);
   tone(ToneReg::Nr24, 0xC7);   // length enable + trigger
   CHECK(g_model.state().square[1].on);
   g_model.render(nullptr, nullptr, g_out, kN);
   CHECK(!g_model.state().square[1].on);
   CHECK((g_model.read(static_cast<uint8_t>(ToneReg::Nr52)) & 0x02) == 0);
}

// A rising sweep (time 1, shift 1): from 1500 the trigger's own check overflows 2047 (off at once); from 1200 the
// first step gives 1800, whose next check overflows: tone 1 stops within a frame (two 128 Hz steps). A sweep with
// no time keeps the frequency.
void model_sweep()
{
   fresh_model();
   tone(ToneReg::Nr10, 0x11);
   tone(ToneReg::Nr12, 0xF0);
   tone(ToneReg::Nr13, 0xDC);
   tone(ToneReg::Nr14, 0x85);   // 0x5DC = 1500
   CHECK(!g_model.state().square[0].on);
   tone(ToneReg::Nr13, 0xB0);
   tone(ToneReg::Nr14, 0x84);   // 0x4B0 = 1200
   CHECK(g_model.state().square[0].on);
   g_model.render(nullptr, nullptr, g_out, kN);
   CHECK(!g_model.state().square[0].on && g_model.state().square[0].sweep_shadow == 1800);

   fresh_model();
   tone(ToneReg::Nr10, 0x08);   // no sweep time
   tone(ToneReg::Nr12, 0xF0);
   tone(ToneReg::Nr13, 0x00);
   tone(ToneReg::Nr14, 0x84);
   g_model.render(nullptr, nullptr, g_out, kN);
   CHECK(g_model.state().square[0].on && g_model.state().square[0].frequency == 0x400);
}

// Wave RAM goes to the bank NR30 does not play; at 100 % the samples are the nibbles, high first.
void model_wave()
{
   fresh_model();
   tone(ToneReg::Nr30, 0x40);   // bank 1 plays: the CPU writes bank 0
   const uint32_t words[audio::kWaveWords] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0};
   for (uint8_t i = 0; i < audio::kWaveWords; ++i)
   {
      g_model.apply({apu::WriteKind::Wave, i, 0, words[i]});
   }
   CHECK(g_model.state().wave_ram[0][0] == 0xFF && g_model.state().wave_ram[1][0] == 0);
   tone(ToneReg::Nr30, 0x80);   // bank 0 plays, DAC on
   tone(ToneReg::Nr32, 0x20);   // 100 %
   tone(ToneReg::Nr51, 0x44);
   tone(ToneReg::Nr34, 0x87);
   g_model.render(nullptr, nullptr, g_out, kN);
   CHECK(g_model.state().wave.on);
   int16_t high = 0;
   for (uint32_t i = 0; i < kN; ++i)
   {
      high = g_out[2 * i] > high ? g_out[2 * i] : high;
   }
   CHECK(high == 15 * 2 * 8 * 64);
}

// Noise: the register moves from all ones; 7-bit mode repeats every 127 steps.
void model_noise()
{
   fresh_model();
   tone(ToneReg::Nr42, 0xF0);
   tone(ToneReg::Nr43, 0x08);   // 7-bit, shift 0, divisor 0: 32 cycles a step
   tone(ToneReg::Nr44, 0x80);
   CHECK(g_model.state().noise.lfsr == 0x7FFF);
   g_model.render(nullptr, nullptr, g_out, kN);
   CHECK(g_model.state().noise.on && g_model.state().noise.lfsr != 0x7FFF);
}

} // namespace

int main()
{
   capture_follows_the_dma_counter();
   output_plays_the_previous_segment();
   tone_writes_apply_at_the_frame_start();
   mono_mixes_both_fifos_at_half();
   hq_block_replaces_the_ring();
   modelled_blanks_step_now();
   model_bias_resolution_and_clamp();
   model_square_period_duty_and_routing();
   model_envelope_and_length();
   model_sweep();
   model_wave();
   model_noise();
   std::printf("audio hal tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
