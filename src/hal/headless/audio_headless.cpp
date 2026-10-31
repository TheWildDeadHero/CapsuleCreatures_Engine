/// @file
/// Headless sound output (capsule/hal/audio.h, D-34 M4 B1b): the integer output model plays the core's ring and
/// tone writes; every frame is kept for tests and apps/audio_render (audio_headless.h). No device: the frames are
/// rendered and dropped unless a sink takes them.
#include "audio_headless.h"

#include "capsule/audio/core.h"
#include "capsule/hal/audio.h"

#include "../common/audio_hook.h"

#include <cstddef>

namespace
{

using namespace capsule::hal;
namespace core = capsule::audio::core;

apu::Player g_player;                              ///< The model and the frame being recorded.
bool g_started = false;                            ///< init() ran.
audio::capture::FrameSink g_sink = nullptr;        ///< Takes each finished frame.
/// The next step is the first since init(): it has no vsync() (as on the GBA, see capsule/hal/audio.h).
bool g_first_step = false;

/// C14a (the audio clock, capsule/hal/audio.h): the modelled blanks of rng::spend_blanks() (through audio_hook.h), one step each, now.
/// @param blanks  the blanks
void spend(uint32_t blanks)
{
   for (uint32_t i = 0; i < blanks; ++i)
   {
      audio::capture::step();
   }
}

} // namespace

namespace capsule::hal::audio
{

void init(const core::Ring& ring)
{
   static_assert(offsetof(core::Ring, left) == sizeof(core::Ring::right), "the ring is right side, then left");
   g_player.start(reinterpret_cast<const int8_t*>(&ring), sizeof(core::Ring::right), core::frame_samples());
   g_started = true;
   g_first_step = true;
   hook::g_vblank_step = capture::step;
   hook::g_spend_step = spend;
}

void restart_dma()
{
   g_player.restart_dma();
}

void set_output(bool stereo)
{
   g_player.set_output(stereo);
}

void tone_write(ToneReg r, uint8_t v)
{
   g_player.tone_write(static_cast<uint8_t>(r), v);
}

uint8_t tone_read(ToneReg r)
{
   return g_player.tone_read(static_cast<uint8_t>(r));
}

void wave_write(const uint32_t words[kWaveWords])
{
   g_player.wave_write(words);
}

void frame_done()
{
   const apu::FrameOut& frame = g_player.finish_frame(core::segment());
   if (g_sink != nullptr)
   {
      g_sink(frame);
   }
}

void hq_block(const int16_t* right, const int16_t* left)
{
   g_player.hq_block(right, left);
}

Budget budget()
{
   return Budget{};   // nothing to measure off the GBA
}

} // namespace capsule::hal::audio

namespace capsule::hal::audio::capture
{

bool started()
{
   return g_started;
}

const apu::FrameOut& last()
{
   return g_player.last();
}

const apu::Model& model()
{
   return g_player.model();
}

void set_sink(FrameSink sink)
{
   g_sink = sink;
}

void step()
{
   if (!g_first_step)
   {
      core::vsync();
   }
   g_first_step = false;
   core::vblank();
   frame_done();
}

} // namespace capsule::hal::audio::capture
