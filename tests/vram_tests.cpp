/// @file
/// Tests for the VRAM upload queue (A-5) on the software video model, the
/// palette manager's use of it, and the random-stream seeding (A-4).
#include "capsule/core/random.h"
#include "capsule/gfx/palette.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"

#include "../src/hal/soft/soft_video.h"

#include <cstdio>
#include <cstdlib>

namespace capsule::hal::system
{
/// Test build: report and abort.
/// @param expr  failed condition
/// @param file  file
/// @param line  line
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   std::printf("ASSERT %s:%d: %s\n", file, line, expr);
   std::abort();
}

uint32_t g_test_entropy = 0;   ///< What entropy() returns in this test.

/// Test build: a value the test controls.
/// @return g_test_entropy
uint32_t entropy()
{
   return g_test_entropy;
}
} // namespace capsule::hal::system

namespace
{

using namespace capsule;
namespace video = hal::video;
namespace vram = hal::vram;
using video::Region;
using video::Source;
using vram::Priority;

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.

/// Records one check.
/// @param cond  condition that must hold
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

/// A halfword of video memory.
/// @param r       region
/// @param offset  byte offset
/// @return value
uint16_t at(Region r, uint32_t offset)
{
   return hal::soft::peek16(r, offset);
}

/// A word of video memory.
/// @param r       region
/// @param offset  byte offset
/// @return value
uint32_t word(Region r, uint32_t offset)
{
   return at(r, offset) | (uint32_t{at(r, offset + 2)} << 16);
}

/// Fresh model and queue.
void fresh()
{
   hal::soft::reset();
   vram::reset();
}

alignas(4) uint32_t g_big[16384];   ///< 64 KB of test words.

void test_must_land()
{
   fresh();
   static const uint16_t kColors[4] = {0x1111, 0x2222, 0x3333, 0x4444};
   vram::copy16(Region::BgPalette, 32, kColors, 4, Priority::MustLand, Source::Rom);
   CHECK(vram::pending(Priority::MustLand) == 1 && !vram::idle());
   CHECK(at(Region::BgPalette, 32) == 0);   // nothing lands before VBlank
   vram::flush();
   CHECK(at(Region::BgPalette, 32) == 0x1111 && at(Region::BgPalette, 38) == 0x4444);
   CHECK(vram::idle() && vram::stats().frames == 1);
   CHECK(vram::stats().bytes_last == 8);
   CHECK(vram::stats().cycles_last == vram::cost(vram::Mode::Copy16, Source::Rom, 8));
}

void test_merge()
{
   fresh();
   static const uint16_t kRow[64] = {};
   // Two halves of one run, queued separately, become one request.
   vram::copy16(Region::BgVram, 0xE000, kRow, 32, Priority::MustLand, Source::Iwram);
   vram::copy16(Region::BgVram, 0xE040, kRow + 32, 32, Priority::MustLand, Source::Iwram);
   CHECK(vram::pending(Priority::MustLand) == 1);
   // Not contiguous in the source: stays separate.
   vram::copy16(Region::BgVram, 0xE080, kRow, 32, Priority::MustLand, Source::Iwram);
   CHECK(vram::pending(Priority::MustLand) == 2);
   // Different source kind: separate.
   vram::copy16(Region::BgVram, 0xE0C0, kRow + 32, 32, Priority::MustLand, Source::Ewram);
   CHECK(vram::pending(Priority::MustLand) == 3);
   vram::flush();
}

void test_fill()
{
   fresh();
   vram::fill32(Region::ObjVram, 64, 0xA5A55A5Au, 8, Priority::MustLand);
   vram::flush();
   CHECK(word(Region::ObjVram, 64) == 0xA5A55A5Au && word(Region::ObjVram, 92) == 0xA5A55A5Au);
   CHECK(word(Region::ObjVram, 96) == 0);
}

void test_streaming_budget()
{
   fresh();
   for (uint32_t i = 0; i < 16384; ++i)
   {
      g_big[i] = i * 2654435761u;
   }
   // 64 KB from ROM: 16 pieces of 4 KB; each costs ~6 KB of cycles.
   vram::copy32(Region::BgVram, 0, g_big, 16384, Priority::Streaming, Source::Rom);
   CHECK(vram::pending(Priority::Streaming) == 16);
   const uint32_t piece = vram::cost(vram::Mode::Copy32, Source::Rom, vram::kPieceBytes);
   const uint32_t per_frame = vram::kBudgetCycles / piece;
   vram::flush();
   CHECK(vram::pending(Priority::Streaming) == 16 - per_frame);
   CHECK(vram::stats().cycles_last <= vram::kBudgetCycles);
   CHECK(word(Region::BgVram, 0) == g_big[0]);
   CHECK(word(Region::BgVram, per_frame * vram::kPieceBytes) == 0);   // next piece still waiting
   int frames = 1;
   while (!vram::idle() && frames < 100)
   {
      vram::flush();
      ++frames;
   }
   CHECK(frames == static_cast<int>((16 + per_frame - 1) / per_frame));
   bool same = true;
   for (uint32_t i = 0; i < 16384; i += 97)
   {
      same = same && word(Region::BgVram, i * 4) == g_big[i];
   }
   CHECK(same);
   CHECK(vram::stats().bytes_total == 65536);
}

void test_must_land_first()
{
   fresh();
   for (uint32_t i = 0; i < 4096; ++i)
   {
      g_big[i] = 0x11111111u;
   }
   vram::copy32(Region::BgVram, 0, g_big, 4096, Priority::Streaming, Source::Rom);   // 16 KB, 4 pieces
   static const uint16_t kC = 0x7C00;
   vram::copy16(Region::BgPalette, 0, &kC, 1, Priority::MustLand, Source::Rom);
   vram::flush();
   CHECK(at(Region::BgPalette, 0) == 0x7C00);
   CHECK(vram::pending(Priority::MustLand) == 0);
}

void test_overrun_and_forced()
{
   fresh();
   // Must-land beyond the budget still lands, and is counted.
   static uint16_t rows[1024];
   for (int i = 0; i < 32; ++i)
   {
      vram::copy16(Region::BgVram, static_cast<uint32_t>(i) * 2048u, rows, 1024, Priority::MustLand, Source::Ewram);
   }
   CHECK(vram::pending(Priority::MustLand) == 32);
   vram::flush();
   CHECK(vram::stats().overruns == 1 && vram::idle());
   // A full ring lands its oldest requests early instead of dropping anything.
   static const uint16_t kOne = 7;
   for (uint32_t i = 0; i < vram::kMustLandSlots; ++i)
   {
      vram::copy16(Region::BgVram, i * 64u, &kOne, 1, Priority::MustLand, Source::Rom);   // gaps: no merging
   }
   CHECK(vram::pending(Priority::MustLand) == vram::kMustLandSlots && vram::stats().forced == 0);
   vram::copy16(Region::BgVram, 0x8000, &kOne, 1, Priority::MustLand, Source::Rom);
   CHECK(vram::stats().forced == vram::kMustLandSlots && vram::pending(Priority::MustLand) == 1);
   CHECK(at(Region::BgVram, 64) == 7);   // landed early
   vram::flush();
   CHECK(at(Region::BgVram, 0x8000) == 7);
}

void test_program_order()
{
   fresh();
   // Queued then an overlapping immediate write: the immediate one wins.
   static const uint16_t kQueued = 0x0011;
   vram::copy16(Region::BgPalette, 2, &kQueued, 1, Priority::MustLand, Source::Rom);
   const video::Color immediate = 0x0022;
   video::load_bg_palette(1, &immediate, 1);
   CHECK(vram::idle());   // the overlap landed the queue first
   vram::flush();
   CHECK(at(Region::BgPalette, 2) == 0x0022);
   // A write elsewhere does not land the queue early.
   vram::copy16(Region::BgPalette, 2, &kQueued, 1, Priority::MustLand, Source::Rom);
   video::load_bg_palette(9, &immediate, 1);
   CHECK(vram::pending(Priority::MustLand) == 1 && at(Region::BgPalette, 2) == 0x0022);
   video::load_obj_palette(1, &immediate, 1);   // same offset, other region
   CHECK(vram::pending(Priority::MustLand) == 1);
   vram::flush();
   CHECK(at(Region::BgPalette, 2) == 0x0011);
   // Two requests far apart (an animation frame and a ring row): a write
   // between them is inside their span but overlaps neither, so nothing lands.
   static const uint32_t kTile[8] = {1, 2, 3, 4, 5, 6, 7, 8};
   static const uint16_t kRow[32] = {};
   vram::copy32(Region::BgVram, 0, kTile, 8, Priority::Streaming, Source::Rom);
   vram::copy16(Region::BgVram, 0xE000, kRow, 32, Priority::MustLand, Source::Rom);
   const uint32_t blank[8] = {};
   video::load_bg_tiles(2, 0, blank, 1);   // 0x8000
   CHECK(vram::pending(Priority::MustLand) == 1 && vram::pending(Priority::Streaming) == 1);
   video::load_bg_tiles(0, 0, blank, 1);   // over the queued tile: lands first
   CHECK(vram::idle() && word(Region::BgVram, 0) == 0);
}

void test_palette_manager()
{
   fresh();
   gfx::palette::reset();
   const video::Color red = video::rgb5(31, 0, 0);
   gfx::palette::load_bg(16 + 1, &red, 1);
   CHECK(at(Region::BgPalette, 34) == 0);   // queued, not landed
   vram::flush();
   CHECK(at(Region::BgPalette, 34) == red);
   gfx::palette::set_fade(gfx::palette::bg_bank(1), 0, 16);
   vram::flush();
   CHECK(at(Region::BgPalette, 34) == 0);   // faded to black
   gfx::palette::set_fade(0, 0, 0);
   vram::flush();
   CHECK(at(Region::BgPalette, 34) == red);   // base colors kept
   CHECK(vram::stats().bytes_last == 32);     // one bank, not the whole palette
}

void test_seed()
{
   using hal::system::g_test_entropy;
   rng::Stream a = rng::gameplay;
   rng::seed(1234);
   const rng::Stream after = rng::gameplay;
   CHECK(after.state != a.state);
   // Same start + same entropy = same result (deterministic).
   rng::gameplay = a;
   rng::seed(1234);
   CHECK(rng::gameplay.state == after.state);
   // Different entropy, different stream.
   rng::gameplay = a;
   rng::seed(1235);
   CHECK(rng::gameplay.state != after.state);
   CHECK(rng::gameplay.state != rng::cosmetic.state);
   // U5: with press seeding off, a press leaves the streams as they are (and the next press still seeds once on).
   const rng::Stream unseeded = rng::gameplay;
   CHECK(rng::press_seeding());
   rng::set_press_seeding(false);
   hal::system::seed_on_first_press(1);
   CHECK(rng::gameplay.state == unseeded.state);
   rng::set_press_seeding(true);
   // First press reseeds once, only on a press.
   const rng::Stream before = rng::gameplay;
   g_test_entropy = 99;
   hal::system::seed_on_first_press(0);
   CHECK(rng::gameplay.state == before.state);
   hal::system::seed_on_first_press(1);
   const rng::Stream pressed = rng::gameplay;
   CHECK(pressed.state != before.state);
   hal::system::seed_on_first_press(1);
   CHECK(rng::gameplay.state == pressed.state);
}

} // namespace

int main()
{
   test_must_land();
   test_merge();
   test_fill();
   test_streaming_budget();
   test_must_land_first();
   test_overrun_and_forced();
   test_program_order();
   test_palette_manager();
   test_seed();
   std::printf("vram_tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
