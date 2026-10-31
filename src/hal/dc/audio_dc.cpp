/// @file
/// Dreamcast sound output (capsule/hal/audio.h, D-34 M4 B1b): silent. The core still runs every frame (its state
/// gates game frames: waits on songs, clips and fades), but nothing is played; the tone registers are shadowed so the
/// core reads back what it wrote. A future AICA backend replaces this file.
#include "capsule/audio/core.h"
#include "capsule/hal/audio.h"

#include "../common/audio_hook.h"

namespace
{

using namespace capsule::hal;
namespace core = capsule::audio::core;

/// Bytes of the tone register block shadowed (offsets as hal::audio::ToneReg).
constexpr uint8_t kShadowBytes = 0x30;

uint8_t g_shadow[kShadowBytes];   ///< Tone registers as written.

/// The next step is the first since init(): it has no vsync() (as on the GBA, see capsule/hal/audio.h).
bool g_first_step = false;

/// Runs one vertical blank: the core's DMA counter and its step (audio_hook.h).
void step()
{
   if (!g_first_step)
   {
      core::vsync();
   }
   g_first_step = false;
   core::vblank();
   audio::frame_done();
}

/// C14a (the audio clock, capsule/hal/audio.h): the modelled blanks of rng::spend_blanks() (through audio_hook.h), one step each, now.
/// @param blanks  the blanks
void spend(uint32_t blanks)
{
   for (uint32_t i = 0; i < blanks; ++i)
   {
      step();
   }
}

} // namespace

namespace capsule::hal::audio
{

void init(const core::Ring&)
{
   g_first_step = true;
   hook::g_vblank_step = step;
   hook::g_spend_step = spend;
}

void restart_dma()
{
}

void set_output(bool)
{
}

void tone_write(ToneReg r, uint8_t v)
{
   const auto offset = static_cast<uint8_t>(r);
   if (offset < kShadowBytes)
   {
      g_shadow[offset] = v;
   }
}

uint8_t tone_read(ToneReg r)
{
   const auto offset = static_cast<uint8_t>(r);
   return offset < kShadowBytes ? g_shadow[offset] : 0;
}

void wave_write(const uint32_t[kWaveWords])
{
}

void frame_done()
{
}

void hq_block(const int16_t*, const int16_t*)
{
}

Budget budget()
{
   return Budget{};
}

} // namespace capsule::hal::audio
