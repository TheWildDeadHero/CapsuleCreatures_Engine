/// @file
/// The screen arena (see capsule/screen/arena.h).
#include "capsule/screen/arena.h"

#include "capsule/core/memory.h"
#include "capsule/hal/vram_queue.h"
#include "arena_storage.h"

#include <cstring>

namespace capsule::screen::arena
{
namespace
{

/// One live block.
struct Entry
{
   uint32_t start;     ///< Its first byte's offset in the storage (after alignment).
   uint32_t end;       ///< One past its last byte (used() while it is the newest block).
   uint16_t serial;    ///< Its serial.
};

constexpr uint16_t kFirstSerial = 1;   ///< Serials start here (0 is an empty Block's).

CAPSULE_EWRAM_BSS Entry g_blocks[kMaxBlocks];   ///< The live blocks, oldest first.
int g_count = 0;                                ///< Entries of g_blocks.
uint32_t g_used = 0;                            ///< Bytes in use.
uint32_t g_high = 0;                            ///< The most bytes in use at once.
uint16_t g_serial = kFirstSerial;               ///< The next block's serial.

/// An offset rounded up to an alignment.
/// @param at     the offset
/// @param align  a power of two
/// @return the aligned offset
constexpr uint32_t align_up(uint32_t at, uint32_t align)
{
   return (at + align - 1u) & ~(align - 1u);   // a power of two: clearing the low bits rounds down
}

/// The alignment a request gets.
/// @param align  the request's (a power of two)
/// @return at least kAlign
constexpr uint32_t effective(uint32_t align)
{
   return align < kAlign ? kAlign : align;
}

/// Halts: the arena (or its live list) is too small for what the game opens at once.
/// @param what  which limit
[[noreturn]] void full(const char* what)
{
   // a configuration error of the game's build (screen_arena_bytes), reported in release builds too: a silent failure
   // would hand a screen memory that another screen owns
   hal::system::assert_failed(what, __FILE__, __LINE__);
}

/// Gives back the entries from one on (newest last). used() is always the end of the newest live block (0 when none:
/// last in, first out), so it goes back to the end of the block before the first one given back.
/// @param first  the first entry given back
void drop_from(int first)
{
   if (first >= g_count)
   {
      return;
   }
   g_count = first;
   g_used = g_count > 0 ? g_blocks[g_count - 1].end : 0u;
}

}   // namespace

uint32_t capacity()
{
   return detail::g_storage_bytes;
}

uint32_t used()
{
   return g_used;
}

uint32_t high_water()
{
   return g_high;
}

int blocks()
{
   return g_count;
}

bool fits(uint32_t bytes, uint32_t align)
{
   const uint32_t start = align_up(g_used, effective(align));
   return g_count < kMaxBlocks && start <= detail::g_storage_bytes && bytes <= detail::g_storage_bytes - start;
}

void* take(uint32_t bytes, uint32_t align, uint16_t& index, uint16_t& serial)
{
   ENGINE_ASSERT((align & (align - 1u)) == 0u);   // a power of two
   ENGINE_ASSERT(align <= kStorageAlign);         // the storage's own alignment bounds every block's
   ENGINE_ASSERT(bytes > 0u);                     // every block has a size (release_to() relies on it)
   if (g_count >= kMaxBlocks)
   {
      full("screen arena: more than kMaxBlocks blocks at once");
   }
   if (!fits(bytes, align))
   {
      full("screen arena full: raise the build setting screen_arena_bytes");
   }
   const uint32_t start = align_up(g_used, effective(align));
   Entry& e = g_blocks[g_count];
   e.start = start;
   e.end = start + bytes;
   e.serial = g_serial;
   index = static_cast<uint16_t>(g_count);
   serial = g_serial;
   ++g_count;
   ++g_serial;
   if (g_serial == 0)   // wrapped: 0 stays an empty Block's
   {
      g_serial = kFirstSerial;
   }
   g_used = e.end;
   if (g_used > g_high)
   {
      g_high = g_used;
   }
   uint8_t* p = detail::g_storage + start;
   // C13c: memory given back while a copy from it still waits for the VBlank (a window's tiles) is not handed out
   // again before it lands: the new owner's zeroes would land in its place
   ENGINE_ASSERT(!hal::vram::reads_from(p, bytes));
   std::memset(p, 0, bytes);   // the reference games' AllocZeroed
   return p;
}

void used_after_release()
{
   hal::system::assert_failed("a screen arena Block used while not taken (released, or its screen left)", __FILE__,
                              __LINE__);
}

bool live(uint16_t index, uint16_t serial)
{
   return index < g_count && serial != 0 && g_blocks[index].serial == serial;
}

void release(uint16_t index, uint16_t serial)
{
   if (live(index, serial))
   {
      drop_from(index);
   }
}

void release_to(uint32_t mark)
{
   // a block taken after used() returned the mark starts at or above it; one taken before ends at or below it (every
   // block has a size, so it also starts below it)
   int first = g_count;
   while (first > 0 && g_blocks[first - 1].start >= mark)
   {
      --first;
   }
   drop_from(first);
}

void reset()
{
   g_count = 0;
   g_used = 0;
   g_high = 0;
}

}   // namespace capsule::screen::arena
