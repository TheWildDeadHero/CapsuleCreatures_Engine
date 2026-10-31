/// @file
/// VRAM queue test (A-5) and random seeding (A-4).
///
/// The rows at the bottom show 180 tiles of character block 1 on BG1. The
/// controls queue uploads of different kinds; the statistics show how the
/// queue spreads them over VBlanks:
///   A      stream 32 KB of new tiles (from EWRAM) into character blocks 1-2
///   R      the same upload 8 times (256 KB): lands over several frames
///   B      fade the tiles' palette bank in or out (one small must-land
///          request per frame)
///   START  flood the must-land ring with 64 x 2 KB: early landings and an
///          overrun are counted
/// "Seed" shows the gameplay stream: seeded at start-up, mixed again at the
/// first key press, and otherwise left alone by this app.
#include "test_kit.h"

#include "capsule/core/memory.h"
#include "capsule/core/types.h"
#include "capsule/core/random.h"
#include "capsule/gfx/palette.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace
{

using namespace capsule;
using namespace capsule::testkit;
namespace video = hal::video;
namespace vram = hal::vram;

constexpr video::BgIndex kTileBg = 1;          ///< Layer showing the streamed tiles.
constexpr uint8_t kTileCharBlock = 1;          ///< Where they are streamed to (blocks 1-2).
constexpr uint8_t kTileScreenBlock = 30;       ///< BG1's map.
constexpr uint8_t kTileBank = kFirstAppBank;   ///< Their palette bank.
constexpr int kTileRow = 11;                   ///< First console row they cover.
constexpr int kTileRows = 6;                   ///< Rows of tiles shown.
constexpr uint32_t kWords = 8192;              ///< 32 KB: 1,024 tiles.
constexpr int kTiles = 1024;                   ///< Tiles in the pattern.
constexpr int kWordsPerTile = 8;               ///< 32-bit words in one 4bpp tile (one per pixel row).
constexpr int kBitsPerPixel = 4;               ///< Bits of one 4bpp pixel.
constexpr int kPatternColors = 15;             ///< Stripe colors: palette entries 1-15 (0 is transparent).
constexpr uint32_t kCharBlockBytes = 0x4000u;   ///< Bytes in one character block (16 KB).

/// @name Tile palette ramp
/// Red rises and green falls by kRampStep per entry; blue rises by one from kRampBlueBase.
///@{
constexpr int kRampStep = 2;         ///< Red and green step per entry.
constexpr int kChannelMax = 31;      ///< Brightest 5-bit channel value.
constexpr int kRampBlueBase = 16;    ///< Blue of entry 0.
///@}

/// @name Statistics rows
///@{
constexpr int kFrameRow = 2;      ///< Queue frame count.
constexpr int kFlushRow = 3;      ///< Cycles of the last flush.
constexpr int kBytesRow = 4;      ///< Bytes of the last flush.
constexpr int kWaitingRow = 5;    ///< Requests waiting.
constexpr int kPeakRow = 6;       ///< Peak requests waiting.
constexpr int kOverrunRow = 7;    ///< Overruns and early landings.
constexpr int kJobRow = 8;        ///< Stream job progress.
constexpr int kSeedRow = 9;       ///< Gameplay random state.
///@}
constexpr int kValueCol = 11;           ///< Column of the statistics, right of their labels.
constexpr uint32_t kPercent = 100u;     ///< Scale of a percentage.
constexpr int kSeedDigits = 8;          ///< Hex digits of the random state.

constexpr int kRepeatedUploads = 8;     ///< Uploads queued by R.
constexpr uint8_t kFadeFull = 16;       ///< Fade level of a fully faded bank.
constexpr uint32_t kFloodFactor = 2u;   ///< START floods the must-land ring with this many times its size.
constexpr int kFloodSpread = 16;        ///< Flood requests cycle over this many 2 KB pieces of the pattern.
constexpr int kFloodHalfwords = 1024;   ///< Halfwords per flood request (2 KB).
constexpr int kBytesPerHalfword = 2;    ///< Bytes in one halfword.

/// The tiles as they should land (EWRAM: too big for IWRAM, A-3).
CAPSULE_EWRAM_BSS uint32_t g_pattern[kWords];

/// One job's progress.
struct Job
{
   uint32_t started;   ///< Queue frame count when queued.
   uint32_t landed;    ///< Frames it took, or 0 while it lands.
   bool active;        ///< Waiting for it.
};

Job g_job{};                   ///< Last streaming job.
uint8_t g_phase = 0;           ///< Pattern phase (changes with A).
uint8_t g_fade = 0;            ///< Fade level 0-16.
int8_t g_fade_step = 0;        ///< +1 fading out, -1 back in, 0 still.
uint32_t g_boot_seed = 0;      ///< Gameplay state right after init().

constexpr auto kApp = text::encode("VRAM QUEUE TEST");                     ///< App title.
constexpr auto kHint1 = text::encode("A stream 32K  R x8  B fade");        ///< Controls, line 1.
constexpr auto kHint2 = text::encode("START flood must-land");             ///< Controls, line 2.

/// Blank tile: the last one of the streamed area, left empty so the rest of
/// BG1 shows nothing.
constexpr video::TileIndex kBlankTile = kWords / kWordsPerTile - 1;

/// Fills the pattern buffer: diagonal stripes, shifted by the phase.
void make_pattern()
{
   for (uint32_t t = 0; t < kBlankTile; ++t)
   {
      for (uint32_t y = 0; y < kTilePixels; ++y)
      {
         // One 4bpp row per word: pixel x is the nibble at bit x * 4; colors 1-15 cycle along the diagonal.
         uint32_t row = 0;
         for (uint32_t x = 0; x < kTilePixels; ++x)
         {
            const uint32_t v = ((x + y + t + g_phase) % kPatternColors) + 1;
            row |= v << (x * kBitsPerPixel);
         }
         g_pattern[t * kWordsPerTile + y] = row;
      }
   }
}

/// Queues the pattern once as a streaming upload.
void stream_once()
{
   vram::copy32(video::Region::BgVram, uint32_t{kTileCharBlock} * kCharBlockBytes, g_pattern, kWords,
                vram::Priority::Streaming, video::Source::Ewram);
}

/// Sets up the screen.
void enter()
{
   clear_console();
   title(kApp.codes, 0, 0, nullptr);
   controls(kHint1.codes, kHint2.codes);
   video::Color ramp[kColorsPerBank];
   for (int i = 0; i < kColorsPerBank; ++i)
   {
      ramp[i] = video::rgb5(static_cast<uint8_t>(i * kRampStep), static_cast<uint8_t>(kChannelMax - i * kRampStep),
                            static_cast<uint8_t>(kRampBlueBase + i));
   }
   gfx::palette::load_bg(static_cast<uint8_t>(kTileBank * kColorsPerBank), ramp, kColorsPerBank);
   make_pattern();
   video::load_bg_tiles(kTileCharBlock, 0, g_pattern, kTiles);   // setup: immediate
   video::configure_bg(kTileBg, true, 1, kTileCharBlock, kTileScreenBlock, false, 0);
   for (int y = 0; y < kMapCells; ++y)
   {
      for (int x = 0; x < kMapCells; ++x)
      {
         const bool shown = y >= kTileRow && y < kTileRow + kTileRows && x < kCols;
         const auto tile = static_cast<video::TileIndex>(shown ? (y - kTileRow) * kCols + x : kBlankTile);
         video::write_map_cell(kTileBg, x, y, video::map_cell(tile, shown ? kTileBank : 0));
      }
   }
   put_ascii(INK_DIM, 0, kFrameRow, "Frame");
   put_ascii(INK_DIM, 0, kFlushRow, "Last flush");
   put_ascii(INK_DIM, 0, kBytesRow, "Bytes");
   put_ascii(INK_DIM, 0, kWaitingRow, "Waiting");
   put_ascii(INK_DIM, 0, kPeakRow, "Peak");
   put_ascii(INK_DIM, 0, kOverrunRow, "Overruns");
   put_ascii(INK_DIM, 0, kJobRow, "Stream job");
   put_ascii(INK_DIM, 0, kSeedRow, "Seed");
}

/// Redraws the statistics.
void draw_stats()
{
   const vram::Stats& s = vram::stats();
   blank(kValueCol, kCols, kFrameRow);
   put_uint(INK_ON, kValueCol, kFrameRow, s.frames);

   blank(kValueCol, kCols, kFlushRow);
   int x = put_uint(INK_ON, kValueCol, kFlushRow, s.cycles_last);
   x = put_ascii(INK_DIM, x + 1, kFlushRow, "cy");
   x = put_uint(INK_ON, x + 1, kFlushRow, s.cycles_last * kPercent / vram::kVBlankCycles);
   put_ascii(INK_DIM, x, kFlushRow, "% VBL");

   blank(kValueCol, kCols, kBytesRow);
   put_uint(INK_ON, kValueCol, kBytesRow, s.bytes_last);

   blank(kValueCol, kCols, kWaitingRow);
   x = put_ascii(INK_DIM, kValueCol, kWaitingRow, "must");
   x = put_uint(INK_ON, x + 1, kWaitingRow, vram::pending(vram::Priority::MustLand));
   x = put_ascii(INK_DIM, x + 1, kWaitingRow, "strm");
   put_uint(INK_ON, x + 1, kWaitingRow, vram::pending(vram::Priority::Streaming));

   blank(kValueCol, kCols, kPeakRow);
   x = put_ascii(INK_DIM, kValueCol, kPeakRow, "must");
   x = put_uint(INK_ON, x + 1, kPeakRow, s.peak_must_land);
   x = put_ascii(INK_DIM, x + 1, kPeakRow, "strm");
   put_uint(INK_ON, x + 1, kPeakRow, s.peak_streaming);

   blank(kValueCol, kCols, kOverrunRow);
   x = put_uint(INK_ON, kValueCol, kOverrunRow, s.overruns);
   x = put_ascii(INK_DIM, x + 1, kOverrunRow, "early");
   put_uint(INK_ON, x + 1, kOverrunRow, s.forced);

   blank(kValueCol, kCols, kJobRow);
   if (g_job.active)
   {
      x = put_ascii(INK_ON, kValueCol, kJobRow, "landing,");
      put_uint(INK_ON, x + 1, kJobRow, s.frames - g_job.started);
   }
   else if (g_job.landed != 0)
   {
      x = put_ascii(INK_DIM, kValueCol, kJobRow, "landed in");
      x = put_uint(INK_ON, x + 1, kJobRow, g_job.landed);
      put_ascii(INK_DIM, x + 1, kJobRow, "frames");
   }
   else
   {
      put_ascii(INK_OFF, kValueCol, kJobRow, "none");
   }

   blank(kValueCol, kCols, kSeedRow);
   x = put_hex(INK_ON, kValueCol, kSeedRow, rng::gameplay.state, kSeedDigits);
   put_ascii(INK_DIM, x + 1, kSeedRow, rng::gameplay.state == g_boot_seed ? "boot" : "pressed");
}

/// One frame of input and queue work.
/// @param pad  the pad
void update(const Pad& pad)
{
   if (!g_job.active)
   {
      if (pad.hit(hal::input::KEY_A) || pad.hit(hal::input::KEY_R))
      {
         ++g_phase;
         make_pattern();   // only while nothing reads the buffer
         const int times = pad.hit(hal::input::KEY_R) ? kRepeatedUploads : 1;
         for (int i = 0; i < times; ++i)
         {
            stream_once();
         }
         g_job = Job{vram::stats().frames, 0, true};
      }
   }
   else if (vram::pending(vram::Priority::Streaming) == 0)
   {
      g_job.active = false;
      g_job.landed = vram::stats().frames - g_job.started;
   }
   if (pad.hit(hal::input::KEY_B))
   {
      g_fade_step = g_fade == 0 ? 1 : -1;
   }
   if (g_fade_step != 0)
   {
      g_fade = static_cast<uint8_t>(g_fade + g_fade_step);
      if (g_fade == 0 || g_fade == kFadeFull)
      {
         g_fade_step = 0;
      }
      gfx::palette::set_fade(gfx::palette::bg_bank(kTileBank), 0, g_fade);
   }
   if (pad.hit(hal::input::KEY_START) && !g_job.active)
   {
      // Twice what the ring holds: the first 32 land early to make room, and
      // the 32 left cost more than one VBlank (an overrun).
      for (uint32_t i = 0; i < kFloodFactor * vram::kMustLandSlots; ++i)
      {
         // Each request is the largest must-land size, taken from successive pieces of the pattern.
         const uint32_t at = (i % kFloodSpread) * vram::kMaxMustLandBytes;
         vram::copy16(video::Region::BgVram, uint32_t{kTileCharBlock} * kCharBlockBytes + at,
                      reinterpret_cast<const uint16_t*>(g_pattern) + at / kBytesPerHalfword, kFloodHalfwords,
                      vram::Priority::MustLand, video::Source::Ewram);
      }
   }
   draw_stats();
}

} // namespace

/// Runs the VRAM queue test until the platform quits.
/// @return 0
int main()
{
   hal::system::init();
   g_boot_seed = rng::gameplay.state;
   init_console();
   enter();
   Pad pad;
   while (hal::system::update())
   {
      pad.update();
      update(pad);
   }
   return 0;
}
