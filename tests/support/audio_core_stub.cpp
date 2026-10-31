/// @file
/// The audio core stand-in (audio_core_stub.h).
#include "audio_core_stub.h"

#include "capsule/audio/core.h"
#include "capsule/hal/audio.h"

namespace
{

namespace core = capsule::audio::core;
namespace hal_audio = capsule::hal::audio;
using hal_audio::ToneReg;

constexpr int8_t kDmaPeriod = 7;          ///< Frames between DMA restarts.
constexpr int8_t kSegmentZeroAt = 1;      ///< Counter values up to this mix segment 0 ...
constexpr int8_t kSegmentBase = 8;        ///< ... else segment 8 - counter.
constexpr uint32_t kFrameStep = 7;        ///< The pattern moves by this much a frame.
constexpr uint32_t kPatternMask = 0x3F;   ///< The pattern stays within -32..31.
constexpr int32_t kPatternCentre = 32;    ///< Its offset.
constexpr uint8_t kDuty50 = 0x80;         ///< NR21: 50 % duty, length 0.
constexpr uint8_t kLoudFalling = 0xF1;    ///< NR22: level 15, falling every 64 Hz tick.
constexpr uint8_t kFreqLow = 0x00;        ///< NR23: frequency 1792 (512 Hz) ...
constexpr uint8_t kFreqHighTrigger = 0x87;   ///< NR24: ... high bits 7, trigger.
constexpr uint8_t kRouteTone2Both = 0x22;    ///< NR51: tone 2 to both sides.
/// A wave pattern the stub loads at its first frame (a ramp).
constexpr uint32_t kWave[hal_audio::kWaveWords] = {0x10325476u, 0x98BADCFEu, 0xEFCDAB89u, 0x67452301u};

core::Ring g_ring;          ///< The ring (the GBA DMA would read it in place).
int8_t g_counter = 0;       ///< The DMA counter.
uint8_t g_segment = 0;      ///< The segment the latest vblank() wrote.
uint32_t g_frames = 0;      ///< vblank() calls since init().

} // namespace

namespace capsule::audio::stub_core
{

void init()
{
   g_ring = core::Ring{};
   g_counter = 0;
   g_segment = 0;
   g_frames = 0;
   hal_audio::init(g_ring);
}

uint32_t frames()
{
   return g_frames;
}

int8_t pattern(uint32_t frame, uint32_t i)
{
   return static_cast<int8_t>(static_cast<int32_t>((frame * kFrameStep + i) & kPatternMask) - kPatternCentre);
}

} // namespace capsule::audio::stub_core

namespace capsule::audio::core
{

const Ring& ring()
{
   return g_ring;
}

void vsync()
{
   g_counter = static_cast<int8_t>(g_counter - 1);
   if (g_counter <= 0)
   {
      g_counter = kDmaPeriod;
      hal_audio::restart_dma();
   }
}

void vblank()
{
   ++g_frames;
   g_segment = static_cast<uint8_t>(g_counter <= kSegmentZeroAt ? 0 : kSegmentBase - g_counter);
   const uint32_t first = uint32_t{g_segment} * hal_audio::kFrameSamples;
   for (uint32_t i = 0; i < hal_audio::kFrameSamples; ++i)
   {
      const int8_t v = stub_core::pattern(g_frames, i);
      g_ring.right[first + i] = v;
      g_ring.left[first + i] = static_cast<int8_t>(-v);
   }
   if (g_frames == 1)
   {
      hal_audio::wave_write(kWave);
   }
   if (g_frames % stub_core::kNoteEvery == 1)
   {   // a falling 512 Hz square on tone 2, both sides
      hal_audio::tone_write(ToneReg::Nr21, kDuty50);
      hal_audio::tone_write(ToneReg::Nr22, kLoudFalling);
      hal_audio::tone_write(ToneReg::Nr23, kFreqLow);
      hal_audio::tone_write(ToneReg::Nr51, kRouteTone2Both);
      hal_audio::tone_write(ToneReg::Nr24, kFreqHighTrigger);
   }
   if (g_frames == stub_core::kMonoAt)
   {
      hal_audio::set_output(false);
   }
}

uint8_t segment()
{
   return g_segment;
}

uint16_t frame_samples()
{
   return hal_audio::kFrameSamples;
}

} // namespace capsule::audio::core
