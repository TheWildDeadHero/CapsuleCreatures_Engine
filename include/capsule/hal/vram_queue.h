/// @file
/// VRAM upload queue (A-5).
///
/// Uploads made during a frame are queued here and land at the start of the
/// next VBlank, so the screen never shows a half-written tilemap, tile or
/// palette and the display registers and the data they point at change
/// together. On the GBA, flush() runs in the VBlank interrupt once
/// system::update() has marked the frame finished (A-2): the uploads are in
/// place before the next frame's logic starts. A frame that overran lands at
/// the following VBlank, with its registers. Other targets call flush() at
/// the end of system::update(), which is the same point in the frame.
///
/// - Two rings, O(1) to add and remove, no heap: **must-land** (palettes,
///   tilemap edits; all of it lands at the next VBlank) and **streaming**
///   (tiles, animation frames, large loads; lands in budget-sized pieces as
///   VBlank time allows, oldest first). Must-land always lands before
///   streaming.
/// - Each request's cost is estimated in CPU cycles when it is queued, from
///   its size, its access width and where its data lives (ROM, IWRAM, EWRAM).
///   The frame budget is the VBlank (68 lines x 1,232 cycles = 83,776) minus
///   the fixed VBlank work (register and OAM copies, later the audio mix).
///   A streaming request that does not fit waits for the next VBlank; large
///   ones are split into pieces when queued.
/// - The estimates are taken from the GBA's documented timings with the
///   wait states system::init() sets (ROM 3/1); they still have to be
///   measured on hardware. Every target uses the same estimates, so uploads
///   land on the same frames everywhere.
/// - Nothing is ever dropped: when a ring has no free slot, its oldest
///   requests land at once to make room (counted in Stats::forced).
///
/// Rules for callers:
/// - Queued data is read when it lands, not when it is queued: it must stay
///   valid and unchanged until then (ROM, or a static buffer whose latest
///   contents are what should land).
/// - Writes land in program order. The immediate functions in
///   capsule/hal/video.h land the queue first when they overlap a queued
///   request. Do not write the same bytes as must-land and as streaming:
///   must-land lands first.
///
/// Sizes are per-game build settings (the "build" object of the game's
/// reference/project.json, checked by tools/build_settings.py; a CMake cache
/// variable of the same name overrides every game): CAPSULE_VRAM_MUST_LAND_SLOTS
/// (default 32), CAPSULE_VRAM_STREAMING_SLOTS (64), both powers of two, and
/// CAPSULE_VRAM_BUDGET_CYCLES (VBlank minus 16,384 cycles of fixed work). The
/// queue's source is compiled into each game with that game's values, so the
/// constants below have internal linkage (each translation unit sees its
/// build's values).
#pragma once

#include "capsule/core/pod.h"
#include "capsule/hal/video.h"

#include <cstdint>

#ifndef CAPSULE_VRAM_MUST_LAND_SLOTS
/// Must-land ring size (build setting).
#define CAPSULE_VRAM_MUST_LAND_SLOTS 32
#endif
#ifndef CAPSULE_VRAM_STREAMING_SLOTS
/// Streaming ring size (build setting).
#define CAPSULE_VRAM_STREAMING_SLOTS 64
#endif
#ifndef CAPSULE_VRAM_BUDGET_CYCLES
/// Cycles of each VBlank given to the queue (build setting).
#define CAPSULE_VRAM_BUDGET_CYCLES (68 * 1232 - 16384)
#endif

/// The VRAM upload queue.
namespace capsule::hal::vram
{

using video::Region;
using video::Source;

/// When a request lands.
enum class Priority : uint8_t
{
   MustLand,         ///< At the next VBlank, whatever it costs.
   Streaming,        ///< As soon as VBlank time allows, oldest first.
   NUM_PRIORITIES    ///< Number of priorities.
};

/// What a request does.
enum class Mode : uint8_t
{
   Copy32,      ///< Copy 32-bit words.
   Copy16,      ///< Copy 16-bit halfwords.
   Fill32,      ///< Fill with one 32-bit value.
   NUM_MODES    ///< Number of modes.
};

/// Cycles in one VBlank (68 lines of 1,232 cycles).
inline constexpr uint32_t kVBlankCycles = 68u * 1232u;
/// Cycles of each VBlank the queue may use.
constexpr uint32_t kBudgetCycles = CAPSULE_VRAM_BUDGET_CYCLES;
/// Must-land ring size.
constexpr uint16_t kMustLandSlots = CAPSULE_VRAM_MUST_LAND_SLOTS;
/// Streaming ring size.
constexpr uint16_t kStreamingSlots = CAPSULE_VRAM_STREAMING_SLOTS;
/// Largest streaming piece: requests above this are split when queued.
inline constexpr uint32_t kPieceBytes = 4096;
/// Largest must-land request (one full 32x32 screen block).
inline constexpr uint32_t kMaxMustLandBytes = 2048;
/// Fixed cost of one request (DMA set-up, loop), cycles.
inline constexpr uint32_t kRequestCycles = 64;
/// Cycles per word of a fill: the fixed source is one IWRAM read, plus the write.
inline constexpr uint32_t kFillWordCycles = 3;

static_assert((kMustLandSlots & (kMustLandSlots - 1)) == 0 && kMustLandSlots >= 4 && kMustLandSlots <= 128,
              "CAPSULE_VRAM_MUST_LAND_SLOTS must be a power of two, 4-128 (12 B of IWRAM each)");
static_assert((kStreamingSlots & (kStreamingSlots - 1)) == 0 && kStreamingSlots >= 4 && kStreamingSlots <= 128,
              "CAPSULE_VRAM_STREAMING_SLOTS must be a power of two, 4-128 (12 B of IWRAM each)");
static_assert(kBudgetCycles > 0 && kBudgetCycles <= kVBlankCycles,
              "CAPSULE_VRAM_BUDGET_CYCLES must be within one VBlank");

/// Estimated cycles to move one unit (word or halfword) by DMA into video
/// memory: source read plus a 16-bit-bus write.
/// @param mode    the mode
/// @param source  where the data lives
/// @return cycles per unit
constexpr uint32_t unit_cycles(Mode mode, Source source)
{
   if (mode == Mode::Fill32)
   {
      return kFillWordCycles;
   }
   // Index: source; 32-bit = two 16-bit accesses on ROM/EWRAM.
   constexpr uint8_t kWord[3] = {6, 3, 8};   // ROM (S=2 per half), IWRAM, EWRAM (3 per half), + 2 write
   constexpr uint8_t kHalf[3] = {3, 2, 4};
   const auto i = static_cast<uint8_t>(source);
   return mode == Mode::Copy32 ? kWord[i] : kHalf[i];
}

/// Estimated cost of a request.
/// @param mode    the mode
/// @param source  where the data lives (ignored for fills)
/// @param bytes   bytes written
/// @return cycles
constexpr uint32_t cost(Mode mode, Source source, uint32_t bytes)
{
   const uint32_t unit = mode == Mode::Copy16 ? video::kHalfwordBytes : video::kWordBytes;
   return kRequestCycles + (bytes / unit) * unit_cycles(mode, source);
}

static_assert(cost(Mode::Copy32, Source::Rom, kPieceBytes) < kBudgetCycles / 4,
              "a streaming piece must fit well inside the frame budget");

/// One queued request (12 bytes on 32-bit targets).
struct Request
{
   union
   {
      const void* src;    ///< Copy modes: the data.
      uint32_t value;     ///< Fill32: the word.
   } data;                 ///< Source.
   uint32_t dst;           ///< Region << 24 | byte offset.
   uint32_t size_mode;     ///< Bytes (bits 0-23) | mode << 24 | source << 28.
};
CAPSULE_POD(Request, sizeof(void*) + 8);

/// Queue statistics (for the test app and budgets).
struct Stats
{
   uint32_t frames;          ///< flush() calls so far.
   uint32_t cycles_last;     ///< Estimated cycles the last flush() used.
   uint32_t bytes_last;      ///< Bytes the last flush() wrote.
   uint32_t bytes_total;     ///< Bytes landed since reset().
   uint32_t overruns;        ///< flush() calls whose must-land requests alone exceeded the budget.
   uint32_t forced;          ///< Requests landed early because their ring was full.
   uint16_t peak_must_land;  ///< Most must-land requests waiting at once.
   uint16_t peak_streaming;  ///< Most streaming requests waiting at once.
};

/// Queues a 32-bit copy of word data (tiles: 8 nibbles per word, leftmost
/// pixel lowest). The words are values, not halfword data cast to words, so
/// they land the same on every host's byte order; palettes therefore take
/// copy16().
/// @param region    destination region (VRAM; not a palette)
/// @param offset    byte offset, a multiple of 4
/// @param src       the words; must stay valid until they land
/// @param words     number of words
/// @param priority  when it lands
/// @param source    where @p src lives
void copy32(Region region, uint32_t offset, const uint32_t* src, uint32_t words, Priority priority,
            Source source = Source::Rom);

/// Queues a 16-bit copy.
/// @param region     destination region
/// @param offset     byte offset, a multiple of 2
/// @param src        the halfwords; must stay valid until they land
/// @param halfwords  number of halfwords
/// @param priority   when it lands
/// @param source     where @p src lives
void copy16(Region region, uint32_t offset, const uint16_t* src, uint32_t halfwords, Priority priority,
            Source source = Source::Rom);

/// Queues a 32-bit fill.
/// @param region    destination region
/// @param offset    byte offset, a multiple of 4
/// @param value     the word
/// @param words     number of words
/// @param priority  when it lands
void fill32(Region region, uint32_t offset, uint32_t value, uint32_t words, Priority priority);

/// Lands queued requests: every must-land request, then streaming requests
/// while the frame budget lasts. Called at the start of VBlank (GBA: by the
/// VBlank interrupt; elsewhere by system::update()); tests call it in place of
/// a frame. Never call it while the GBA VBlank interrupt may (between
/// system::update() marking a frame finished and the interrupt landing it,
/// the main loop is waiting, so engine code cannot).
void flush();

/// Lands every queued request now, ignoring the budget (scene changes).
void flush_all();

/// Called by the immediate video functions before they write: lands the
/// queue if a queued request overlaps the bytes about to be written.
/// @param region  region about to be written
/// @param offset  first byte
/// @param bytes   number of bytes
void before_immediate(Region region, uint32_t offset, uint32_t bytes);

/// Requests waiting.
/// @param priority  which ring
/// @return number of requests
uint16_t pending(Priority priority);

/// Whether nothing is waiting.
/// @return true when both rings are empty
bool idle();

/// C13c: whether a waiting copy reads from memory (debug checks: memory handed to a new owner, a screen arena block
/// or a window's tiles, must not be the source of a copy that has not landed yet).
/// @param begin  the memory's first byte
/// @param bytes  its size
/// @return true when a queued copy's source overlaps it
bool reads_from(const void* begin, uint32_t bytes);

/// The statistics.
/// @return counters since the last reset()
const Stats& stats();

/// Drops every queued request and clears the statistics (start-up, tests).
void reset();

/// C13a (R9 M4): a check of BG tile writes (offset and bytes from the start of BG memory; true: allowed), set by the
/// module that knows which screen blocks are in use (capsule/screen/layers.h, tiles_clear_of_maps()).
using TileGuard = bool (*)(uint32_t offset, uint32_t bytes);

/// C13a: sets the BG tile write check (nullptr: none).
/// @param guard  the check
void set_tile_guard(TileGuard guard);

/// C13a: whether a BG tile write passes the check (true when none is set). Writers assert it in debug builds.
/// @param offset  first byte, from the start of BG memory
/// @param bytes   bytes written
/// @return the check's answer
bool tiles_allowed(uint32_t offset, uint32_t bytes);

} // namespace capsule::hal::vram
