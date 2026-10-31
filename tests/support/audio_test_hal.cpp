/// @file
/// The audio core's test backend (audio_test_hal.h).
#include "audio_test_hal.h"

#include "capsule/hal/audio.h"

namespace capsule::audio::test_hal
{
namespace
{

/// Register bytes of the tone block (up to wave RAM's end).
constexpr uint8_t kRegisterBytes = 0x40;
/// Bytes of a wave word.
constexpr uint8_t kWordBytes = 4;
/// Bits per byte.
constexpr uint8_t kByteBits = 8;
/// SOUNDBIAS_H after the backend's start: 8-bit resolution (65,536 Hz PWM), bias 0x200's high bits.
constexpr uint8_t kBiasStart = 0x42;
/// NR50 after the start.
constexpr uint8_t kNr50Start = 0x77;
/// Samples of a high-quality block per side.
constexpr uint32_t kHqSamples = uint32_t(hal::audio::kHqFactor) * hal::audio::kFrameSamples;

Write g_writes[kMaxWrites];     ///< The record.
uint16_t g_count = 0;           ///< Writes since clear().
uint8_t g_regs[kRegisterBytes]; ///< Last byte of each register.
uint32_t g_restarts = 0;        ///< restart_dma() calls.
bool g_stereo = true;           ///< The routing.
int16_t g_hq[2][kHqSamples];    ///< The last high-quality block.
bool g_hq_seen = false;         ///< A block arrived.

/// Records a write.
/// @param reg    the register (or wave byte offset)
/// @param value  the byte
void record(uint8_t reg, uint8_t value)
{
   if (g_count < kMaxWrites)
   {
      g_writes[g_count] = {reg, value};
   }
   if (g_count != 0xFFFF)
   {
      ++g_count;
   }
}

}   // namespace

const Write* writes()
{
   return g_writes;
}

uint16_t write_count()
{
   return g_count;
}

void clear()
{
   g_count = 0;
}

uint32_t dma_restarts()
{
   return g_restarts;
}

bool stereo()
{
   return g_stereo;
}

const int16_t* hq(bool left)
{
   return g_hq_seen ? g_hq[left ? 1 : 0] : nullptr;
}

}   // namespace capsule::audio::test_hal

namespace capsule::hal::audio
{

using namespace capsule::audio::test_hal;

void init(const capsule::audio::core::Ring&)
{
   for (uint8_t& r : g_regs)
   {
      r = 0;
   }
   g_regs[static_cast<uint8_t>(ToneReg::SoundbiasH)] = kBiasStart;
   g_regs[static_cast<uint8_t>(ToneReg::Nr50)] = kNr50Start;
   g_count = 0;
   g_restarts = 0;
   g_stereo = true;
   g_hq_seen = false;
}

void restart_dma()
{
   ++g_restarts;
}

void set_output(bool stereo)
{
   g_stereo = stereo;
}

void tone_write(ToneReg r, uint8_t v)
{
   g_regs[static_cast<uint8_t>(r)] = v;
   record(static_cast<uint8_t>(r), v);
}

uint8_t tone_read(ToneReg r)
{
   return g_regs[static_cast<uint8_t>(r)];
}

void wave_write(const uint32_t words[kWaveWords])
{
   for (uint8_t w = 0; w < kWaveWords; ++w)
   {
      for (uint8_t b = 0; b < kWordBytes; ++b)
      {
         const uint8_t at = static_cast<uint8_t>(kWaveRecordBase + w * kWordBytes + b);
         const uint8_t v = static_cast<uint8_t>(words[w] >> (b * kByteBits));
         g_regs[at] = v;
         record(at, v);
      }
   }
}

void frame_done()
{
}

void hq_block(const int16_t* right, const int16_t* left)
{
   for (uint32_t i = 0; i < kHqSamples; ++i)
   {
      g_hq[0][i] = right[i];
      g_hq[1][i] = left[i];
   }
   g_hq_seen = true;
}

Budget budget()
{
   return {0, 0, 0, 0};
}

}   // namespace capsule::hal::audio
