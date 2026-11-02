/// @file
/// The scene kit's sandbox (BF, D-38; simple shapes): plays the scenes of scenes.h in turn, with no input, so every
/// target shows the same frames (tools: the viewer capsule_anim_view renders them on the software video model).
///
/// GBA: the cost of each scene's frame (CPU cycles, the largest seen, setup frames left out) is kept in
/// g_anim_bench, and after the first round the bench measures what the scanline transfer takes from the CPU each
/// frame (a fixed loop timed with a table off, with a 16-bit and with a 32-bit table), then the showcase goes on. Read g_anim_bench with
/// gba_run.py --read g_anim_bench.
#include "scenes.h"

#include "capsule/hal/system.h"

#if CAPSULE_PLATFORM_GBA
#include "capsule/core/memory.h"
#include "capsule/hal/video.h"

/// Words of g_anim_bench.
constexpr int kBenchWords = 10;
/// GBA bench results: per scene the largest frame cost in cycles, its setup frame left out (0-3), then the loop's
/// cycles with no table, a 16-bit and a 32-bit table (4-6), the frames each loop took (7), a done flag (8) and the showcase frame (9).
extern "C" volatile uint32_t g_anim_bench[kBenchWords];
volatile uint32_t g_anim_bench[kBenchWords] = {};

namespace
{

constexpr uint32_t kIo = 0x04000000u;            ///< I/O registers.
constexpr uint32_t kTm1Count = 0x104;            ///< Timer 1 counter (free running at the CPU clock: system::init()).
constexpr uint32_t kTm2Count = 0x108;            ///< Timer 2 counter.
constexpr uint32_t kTm2Control = 0x10A;          ///< Timer 2 control.
constexpr uint16_t kTimerOnCascade = 0x0084;     ///< Enabled, counting timer 1's overflows.
constexpr int kHalfBits = 16;                    ///< Timer 2 is the high half of the count.
constexpr uint32_t kSpinLoops = 200000;          ///< The bench's loop (a few frames).
constexpr int kSettleFrames = 2;                 ///< Frames before a bench loop (the table latched).
constexpr int kBenchNoTable = 4;                 ///< g_anim_bench index: loop cycles, no table.
constexpr int kBenchHalf = 5;                    ///< ... a 16-bit table.
constexpr int kBenchWord = 6;                    ///< ... a 32-bit table.
constexpr int kBenchFrames = 7;                  ///< ... frames the loop took.
constexpr int kBenchDone = 8;                    ///< ... set when the bench ran.
constexpr int kBenchFrame = 9;                   ///< ... the showcase frame running.

CAPSULE_EWRAM_BSS uint32_t g_table[capsule::hal::video::kLineEntries];   ///< The bench's table (zeros).

/// A 16-bit I/O register.
/// @param offset  its offset
/// @return the register
volatile uint16_t& io16(uint32_t offset)
{
   return *reinterpret_cast<volatile uint16_t*>(kIo + offset);
}

/// A 32-bit cycle count: timer 2 (cascaded) above timer 1.
/// @return cycles
uint32_t cycles()
{
   uint16_t hi = io16(kTm2Count);
   uint16_t lo = io16(kTm1Count);
   const uint16_t again = io16(kTm2Count);
   if (again != hi)
   {   // timer 1 overflowed between the reads
      hi = again;
      lo = io16(kTm1Count);
   }
   return (uint32_t{hi} << kHalfBits) | lo;
}

/// A loop of fixed work.
/// @param n  iterations
void spin(uint32_t n)
{
   volatile uint32_t i = 0;
   while (i < n)
   {
      i = i + 1;
   }
}

/// Times the loop with a table set (or none).
/// @param width  the table's width
/// @param on     a table at all
/// @return the loop's cycles
uint32_t timed(capsule::hal::video::LineWidth width, bool on)
{
   namespace video = capsule::hal::video;
   video::set_line_effect(video::LineRegister::Bg3Hofs, width, on ? g_table : nullptr);
   for (int i = 0; i < kSettleFrames; ++i)
   {
      capsule::hal::system::update();
   }
   const uint32_t blanks = capsule::hal::system::vblanks();
   const uint32_t start = cycles();
   spin(kSpinLoops);
   const uint32_t taken = cycles() - start;
   g_anim_bench[kBenchFrames] = capsule::hal::system::vblanks() - blanks;
   return taken;
}

/// The bench (once, after the first round).
void bench()
{
   namespace video = capsule::hal::video;
   g_anim_bench[kBenchNoTable] = timed(video::LineWidth::Half, false);
   g_anim_bench[kBenchHalf] = timed(video::LineWidth::Half, true);
   g_anim_bench[kBenchWord] = timed(video::LineWidth::Word, true);
   video::set_line_effect(video::LineRegister::Bg3Hofs, video::LineWidth::Half, nullptr);
   g_anim_bench[kBenchDone] = 1;
}

}   // namespace
#endif

/// Runs the showcase until the platform quits.
/// @return 0
int main()
{
   capsule::hal::system::init();
#if CAPSULE_PLATFORM_GBA
   io16(kTm2Count) = 0;
   io16(kTm2Control) = kTimerOnCascade;
#endif
   uint32_t f = 0;
   while (capsule::hal::system::update())
   {
#if CAPSULE_PLATFORM_GBA
      if (f == sandbox_anim::kTotalFrames && g_anim_bench[kBenchDone] == 0)
      {
         bench();
      }
      g_anim_bench[kBenchFrame] = f;
      const uint32_t start = cycles();
      sandbox_anim::frame(f);
      const uint32_t cost = cycles() - start;
      const int scene = static_cast<int>(f / sandbox_anim::kSceneFrames) % sandbox_anim::kScenes;
      const bool setup = f % sandbox_anim::kSceneFrames == 0;   // a scene's first frame sets it up
      if (!setup && cost > g_anim_bench[scene])
      {
         g_anim_bench[scene] = cost;
      }
#else
      sandbox_anim::frame(f);
#endif
      ++f;
   }
   return 0;
}
