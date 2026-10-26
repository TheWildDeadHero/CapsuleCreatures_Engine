/// @file
/// Desktop HAL (P-8), built on SDL2: window, frame pacing and keyboard input.
/// The GBA video model itself (registers, 64 KB BG VRAM, BG palette,
/// compositor) is the shared software emulation in src/hal/soft, so every
/// non-GBA target shows the same image.
#include "capsule/core/assert.h"
#include "capsule/core/random.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"

#include "../soft/soft_video.h"

/// Apps keep a plain main() on every OS; init() tells SDL that main is handled.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{

using namespace capsule::hal;

constexpr int kWindowScale = 3;   ///< Initial window size, in multiples of 240x160.

/// @name Frame clock (C-05b)
/// One GBA frame is 280896 cycles of a 2^24 Hz clock.
///@{
constexpr uint64_t kCyclesPerFrame = 280896;                   ///< Cycles per frame.
constexpr uint64_t kCyclesPerSecond = uint64_t{1} << 24;       ///< GBA clock, Hz.
///@}

constexpr int kMsPerSecond = 1000;      ///< Milliseconds per second (SDL_Delay() takes milliseconds).
constexpr int kHighWordShift = 32;      ///< Bits 32-63 of a 64-bit counter.
constexpr int kFrameEntropyShift = 16;  ///< Moves the frame count to the upper half of the entropy.

/// Owner of everything SDL hands out (one static instance): its destructor
/// runs at program exit and releases in the reverse order of creation, then
/// shuts SDL down. Constant-initialised (no start-up constructor).
struct Sdl
{
   SDL_Window* window = nullptr;       ///< The window.
   SDL_Renderer* renderer = nullptr;   ///< Its renderer.
   SDL_Texture* texture = nullptr;     ///< 240x160 ARGB8888 streaming texture.
   bool started = false;               ///< SDL_Init() succeeded.

   constexpr Sdl() = default;
   Sdl(const Sdl&) = delete;
   Sdl& operator=(const Sdl&) = delete;

   /// Releases the texture, the renderer and the window, then quits SDL.
   ~Sdl()
   {
      if (texture != nullptr)
      {
         SDL_DestroyTexture(texture);
      }
      if (renderer != nullptr)
      {
         SDL_DestroyRenderer(renderer);
      }
      if (window != nullptr)
      {
         SDL_DestroyWindow(window);
      }
      if (started)
      {
         SDL_Quit();
      }
   }
};
constinit Sdl g_sdl;   ///< The SDL objects.
uint16_t g_frame555[soft::kFramePixels];   ///< Composed frame, BGR555.
uint32_t g_frame[soft::kFramePixels];      ///< Composed frame, ARGB8888.
uint64_t g_next_frame = 0;                 ///< Performance-counter tick of the next frame.
uint64_t g_frame_remainder = 0;            ///< Carried remainder of the frame period.
input::KeyBits g_held = 0;                      ///< Keys read at the last update().
uint32_t g_frames = 0;                          ///< Frames since init().

/// One keyboard binding.
struct KeyBinding
{
   SDL_Scancode scancode;   ///< Physical key.
   input::Key key;          ///< Engine button.
};

/// Desktop is a SNES-layout target (B-11). Default keyboard layout, with the
/// four face buttons in their SNES positions (X top, A right, B bottom, Y left):
/// S = X, X = A, Z = B, A = Y, Q = L, W = R, Enter = Start, Backspace = Select,
/// arrows = D-pad. Rebinding comes with the desktop input config (B-11).
constexpr KeyBinding kBindings[] = {
   {SDL_SCANCODE_X, input::KEY_A},
   {SDL_SCANCODE_Z, input::KEY_B},
   {SDL_SCANCODE_S, input::KEY_X},
   {SDL_SCANCODE_A, input::KEY_Y},
   {SDL_SCANCODE_BACKSPACE, input::KEY_SELECT},
   {SDL_SCANCODE_RETURN, input::KEY_START},
   {SDL_SCANCODE_RIGHT, input::KEY_RIGHT},
   {SDL_SCANCODE_LEFT, input::KEY_LEFT},
   {SDL_SCANCODE_UP, input::KEY_UP},
   {SDL_SCANCODE_DOWN, input::KEY_DOWN},
   {SDL_SCANCODE_W, input::KEY_R},
   {SDL_SCANCODE_Q, input::KEY_L},
};

/// Reports an SDL failure and aborts.
/// @param what  what was being done
[[noreturn]] void fail(const char* what)
{
   std::fprintf(stderr, "capsule: %s: %s\n", what, SDL_GetError());
   std::abort();
}

/// Sleeps until the next frame is due at the GBA frame rate (integer math only).
void wait_for_next_frame()
{
   const uint64_t freq = SDL_GetPerformanceFrequency();
   // Integer frame period with the remainder carried, so there is no drift.
   const uint64_t scaled = freq * kCyclesPerFrame + g_frame_remainder;
   g_next_frame += scaled / kCyclesPerSecond;
   g_frame_remainder = scaled % kCyclesPerSecond;

   const uint64_t now = SDL_GetPerformanceCounter();
   if (now >= g_next_frame)
   {
      g_next_frame = now;   // running late: resynchronize instead of catching up
      return;
   }
   const uint64_t ms = (g_next_frame - now) * kMsPerSecond / freq;
   if (ms > 1)
   {
      SDL_Delay(static_cast<Uint32>(ms - 1));
   }
   while (SDL_GetPerformanceCounter() < g_next_frame)
   {
   }
}

} // namespace

namespace capsule::hal::system
{

void init()
{
   SDL_SetMainReady();
   if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
   {
      fail("SDL_Init");
   }
   g_sdl.started = true;
   g_sdl.window =
      SDL_CreateWindow("CapsuleCreatures Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                       video::kScreenWidth * kWindowScale, video::kScreenHeight * kWindowScale, SDL_WINDOW_RESIZABLE);
   if (g_sdl.window == nullptr)
   {
      fail("SDL_CreateWindow");
   }
   g_sdl.renderer = SDL_CreateRenderer(g_sdl.window, -1, SDL_RENDERER_ACCELERATED);
   if (g_sdl.renderer == nullptr)
   {
      g_sdl.renderer = SDL_CreateRenderer(g_sdl.window, -1, 0);
   }
   if (g_sdl.renderer == nullptr)
   {
      fail("SDL_CreateRenderer");
   }
   SDL_RenderSetLogicalSize(g_sdl.renderer, video::kScreenWidth, video::kScreenHeight);
   SDL_RenderSetIntegerScale(g_sdl.renderer, SDL_TRUE);
   g_sdl.texture = SDL_CreateTexture(g_sdl.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                     video::kScreenWidth, video::kScreenHeight);
   if (g_sdl.texture == nullptr)
   {
      fail("SDL_CreateTexture");
   }

   soft::reset();
   vram::reset();
   g_next_frame = SDL_GetPerformanceCounter();
   rng::seed(entropy());
}

uint32_t entropy()
{
   if (const char* fixed = std::getenv("CAPSULE_SEED"))
   {   // reproducible runs and captures
      return static_cast<uint32_t>(std::strtoul(fixed, nullptr, 0)) ^ g_frames;
   }
   const uint64_t t = SDL_GetPerformanceCounter();
   return static_cast<uint32_t>(t) ^ static_cast<uint32_t>(t >> kHighWordShift) ^ (g_frames << kFrameEntropyShift);
}

bool update()
{
   // Start of "VBlank": latch registers, then show the frame they describe.
   vram::flush();   // A-5: queued uploads land at "VBlank", with the registers
   soft::latch();
   soft::compose(g_frame555);
   for (int i = 0; i < soft::kFramePixels; ++i)
   {
      g_frame[i] = soft::bgr555_to_argb8888(g_frame555[i]);
   }
   SDL_UpdateTexture(g_sdl.texture, nullptr, g_frame, video::kScreenWidth * sizeof(uint32_t));
   SDL_RenderClear(g_sdl.renderer);
   SDL_RenderCopy(g_sdl.renderer, g_sdl.texture, nullptr, nullptr);
   SDL_RenderPresent(g_sdl.renderer);

   SDL_Event event;
   while (SDL_PollEvent(&event))
   {
      if (event.type == SDL_QUIT)
      {
         return false;
      }
   }

   const Uint8* keys = SDL_GetKeyboardState(nullptr);
   input::KeyBits held = 0;
   for (const KeyBinding& b : kBindings)
   {
      if (keys[b.scancode])
      {
         held |= static_cast<input::KeyBits>(1u << b.key);
      }
   }
   g_held = held;
   ++g_frames;
   rng::frame_advance();   // N1: a game's per-frame draw at the vertical blank (off by default)
   seed_on_first_press(held);

   wait_for_next_frame();
   return true;
}

uint32_t vblanks()
{
   return g_frames;   // C11: one blank per update() (the frames are paced: no lag)
}

void assert_failed(const char* expr, const char* file, int line)
{
   std::fprintf(stderr, "ENGINE_ASSERT failed: %s\n  at %s:%d\n", expr, file, line);
   std::abort();
}

} // namespace capsule::hal::system

namespace capsule::hal::input
{

KeyBits held()
{
   return g_held;
}

} // namespace capsule::hal::input
