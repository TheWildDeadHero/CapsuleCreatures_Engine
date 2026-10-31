/// @file
/// Desktop sound output (capsule/hal/audio.h, D-34 M4 B1b): the integer output model (src/hal/common/apu_model.h)
/// plays the core's ring and tone writes, and each frame's signed 16-bit stereo goes to an SDL audio device at
/// 13,379 Hz (53,516 Hz in the high-quality mode, CAPSULE_AUDIO_HQ); SDL converts to the device's own rate.
///
/// The frames are queued (SDL_QueueAudio): system::update() paces them at the GBA's frame rate, so the queue stays
/// short. The device starts once kStartFrames frames are queued (it never runs dry while the program keeps pace);
/// past kMaxQueuedFrames frames (a stall) a frame is dropped instead of letting the delay grow. Without a
/// device (none present, or CAPSULE_AUDIO=0) the model still runs and the core keeps its timing: only the sound is
/// missing.
#include "capsule/audio/core.h"
#include "capsule/hal/audio.h"

#include "../common/apu_model.h"
#include "../common/audio_hook.h"

#include <SDL.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

using namespace capsule::hal;
namespace core = capsule::audio::core;

constexpr int kStereo = 2;                     ///< Output channels.
constexpr uint16_t kDeviceBufferSamples = 512;   ///< SDL's device buffer, sample frames (a power of two).
/// Frames queued before the device starts (~67 ms: more than its buffer, so the queue never runs dry and SDL never
/// pads a buffer with silence while the program keeps pace).
constexpr uint32_t kStartFrames = 4;
constexpr uint32_t kMaxQueuedFrames = 12;      ///< Frames the queue may hold before one is dropped (~200 ms).
/// The environment variable that turns the device off ("0").
constexpr const char* kAudioEnv = "CAPSULE_AUDIO";

#if defined(CAPSULE_AUDIO_HQ)
constexpr uint32_t kRateFactor = audio::kHqFactor;   ///< Device rate: the model's rate times this.
#else
constexpr uint32_t kRateFactor = 1;   ///< Device rate: the model's rate times this.
#endif

/// Owner of the SDL audio device (one static instance): closes it and its subsystem at program exit.
struct SdlAudio
{
   SDL_AudioDeviceID device = 0;   ///< The open device (0: none).
   bool subsystem = false;         ///< SDL_InitSubSystem(SDL_INIT_AUDIO) succeeded.
   bool paused = false;            ///< Opened, waiting for the first frames.

   constexpr SdlAudio() = default;
   SdlAudio(const SdlAudio&) = delete;
   SdlAudio& operator=(const SdlAudio&) = delete;

   /// Closes the device, then the subsystem.
   ~SdlAudio()
   {
      if (device != 0)
      {
         SDL_CloseAudioDevice(device);
      }
      if (subsystem)
      {
         SDL_QuitSubSystem(SDL_INIT_AUDIO);
      }
   }
};

constinit SdlAudio g_sdl_audio;   ///< The device.
apu::Player g_player;             ///< The model and the frame being recorded.

/// The next step is the first since init(): it has no vsync() (as on the GBA, see capsule/hal/audio.h).
bool g_first_step = false;
/// C14a: the steps of modelled blanks are running (spend()): their frames are rendered, not queued.
bool g_spending = false;

/// Runs one vertical blank: the core's DMA counter, its step, the frame's output (audio_hook.h).
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

/// C14a (the audio clock, capsule/hal/audio.h): the modelled blanks of rng::spend_blanks() (through audio_hook.h), one step each, now. The
/// model renders them (its state stays the GBA's) but they are not played, as on the GBA, where the DMA repeats
/// the frame it played before them: the queue does not grow by the reference's lag.
/// @param blanks  the blanks
void spend(uint32_t blanks)
{
   g_spending = true;
   for (uint32_t i = 0; i < blanks; ++i)
   {
      step();
   }
   g_spending = false;
}

/// Opens the device (none: the model runs silently).
void open_device()
{
   const char* env = std::getenv(kAudioEnv);
   if (env != nullptr && std::strcmp(env, "0") == 0)
   {
      return;
   }
   if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
   {
      std::fprintf(stderr, "capsule: no audio (SDL_InitSubSystem: %s)\n", SDL_GetError());
      return;
   }
   g_sdl_audio.subsystem = true;
   SDL_AudioSpec want{};
   want.freq = static_cast<int>(apu::output_rate(core::frame_samples()) * kRateFactor);
   want.format = AUDIO_S16SYS;
   want.channels = kStereo;
   want.samples = kDeviceBufferSamples;
   want.callback = nullptr;   // queued: frame_done() pushes each frame
   SDL_AudioSpec have{};
   // No changes allowed: SDL converts our rate and format to the device's.
   g_sdl_audio.device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
   if (g_sdl_audio.device == 0)
   {
      std::fprintf(stderr, "capsule: no audio (SDL_OpenAudioDevice: %s)\n", SDL_GetError());
      return;
   }
   g_sdl_audio.paused = true;   // until kStartFrames are queued (frame_done())
}

} // namespace

namespace capsule::hal::audio
{

void init(const core::Ring& ring)
{
   static_assert(offsetof(core::Ring, left) == sizeof(core::Ring::right), "the ring is right side, then left");
   g_player.start(reinterpret_cast<const int8_t*>(&ring), sizeof(core::Ring::right), core::frame_samples());
   open_device();
   g_first_step = true;
   hook::g_vblank_step = step;
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
   if (g_sdl_audio.device == 0 || g_spending)
   {
      return;   // no device, or a modelled blank's frame (rendered for the model's state, not played)
   }
   const uint32_t bytes = uint32_t{frame.output_samples} * apu::kOutputSides * sizeof(int16_t);
   if (SDL_GetQueuedAudioSize(g_sdl_audio.device) > kMaxQueuedFrames * bytes)
   {
      return;   // the program stalled: drop this frame rather than grow the delay
   }
   SDL_QueueAudio(g_sdl_audio.device, frame.output, bytes);
   if (g_sdl_audio.paused && SDL_GetQueuedAudioSize(g_sdl_audio.device) >= kStartFrames * bytes)
   {
      SDL_PauseAudioDevice(g_sdl_audio.device, 0);
      g_sdl_audio.paused = false;
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
