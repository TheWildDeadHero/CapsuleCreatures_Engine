/// @file
/// Headless HAL (P-8): the software GBA video model with no display and no
/// input. Used by the headless platform, which builds the engine core and the
/// tests on any host, including cross-compiled big-endian ones (P-19), without
/// SDL. Frames are composed and dropped.
#include "capsule/core/assert.h"
#include "capsule/core/random.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/vram_queue.h"

#include "../soft/soft_video.h"

#include <cstdio>
#include <cstdlib>

namespace
{

uint32_t g_frames = 0;   ///< Frames since init().
uint16_t g_frame[capsule::hal::soft::kFramePixels];   ///< Composed frame (discarded).
} // namespace

namespace capsule::hal::system
{

void init()
{
   soft::reset();
   vram::reset();
   rng::seed(entropy());
}

uint32_t entropy()
{
   return g_frames;
}   // deterministic: tests replay exactly

bool update()
{
   vram::flush();   // A-5: queued uploads land at "VBlank", with the registers
   soft::latch();
   soft::compose(g_frame);
   ++g_frames;
   rng::frame_advance();   // N1: a game's per-frame draw at the vertical blank (off by default)
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
   return 0;
}

} // namespace capsule::hal::input
