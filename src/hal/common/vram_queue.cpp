/// @file
/// VRAM upload queue (see capsule/hal/vram_queue.h). Portable: the same code
/// runs on every target; only video::transfer32/16 and fill32 differ.
#include "capsule/hal/vram_queue.h"

#include "capsule/core/assert.h"
#include "capsule/core/memory.h"

namespace capsule::hal::vram
{
namespace
{

using video::kHalfwordBytes;
using video::kWordBytes;

/// @name Request packing (see Request)
///@{
constexpr int kRegionShift = 24;               ///< dst bits 24-31: region.
constexpr uint32_t kOffsetMask = 0x00FFFFFFu;  ///< dst bits 0-23: byte offset.
constexpr uint32_t kSizeMask = 0x00FFFFFFu;    ///< size_mode bits 0-23: bytes.
constexpr int kModeShift = 24;                 ///< size_mode bits 24-27: mode (bits 24-31: mode and source).
constexpr uint32_t kModeMask = 15;             ///< Mask of the mode field.
constexpr int kSourceShift = 28;               ///< size_mode bits 28-31: source kind.
///@}

/// Lower overlap bound of a region with nothing queued (above every offset).
constexpr uint32_t kNoLowBound = 0xFFFFFFFFu;

/// A ring of requests with a fixed power-of-two size.
/// @tparam N  slots
template <uint16_t N>
struct Ring
{
   /// Indices run over 2N so a full ring (size N) differs from an empty one.
   static constexpr int kIndexMask = 2 * N - 1;

   Request slot[N];   ///< Requests; [head, tail) are waiting.
   uint16_t head;     ///< Oldest waiting request.
   uint16_t tail;     ///< Next free slot.

   /// Requests waiting.
   /// @return count
   uint16_t size() const
   {
      return static_cast<uint16_t>((tail - head) & kIndexMask);
   }
   /// Free slots.
   /// @return count
   uint16_t free() const
   {
      return static_cast<uint16_t>(N - size());
   }
   /// The oldest request.
   /// @return it
   const Request& front() const
   {
      return slot[head & (N - 1)];
   }
   /// Removes the oldest request.
   void pop()
   {
      head = static_cast<uint16_t>((head + 1) & kIndexMask);
   }
   /// Adds a request at the end.
   /// @param r  the request
   void push(const Request& r)
   {
      slot[tail & (N - 1)] = r;
      tail = static_cast<uint16_t>((tail + 1) & kIndexMask);
   }
   /// The newest waiting request.
   /// @return it (the ring must not be empty)
   Request& back()
   {
      return slot[(tail - 1) & (N - 1)];
   }
   /// A waiting request by age.
   /// @param i  0 = oldest
   /// @return it
   const Request& at(uint16_t i) const
   {
      return slot[(head + i) & (N - 1)];
   }
};

Ring<kMustLandSlots> g_must{};     ///< Must-land requests.
Ring<kStreamingSlots> g_stream{};  ///< Streaming requests.
Stats g_stats{};                   ///< Statistics.

/// Lowest and one-past-highest queued byte per region; lo >= hi means none.
uint32_t g_lo[static_cast<int>(Region::NUM_REGIONS)];
/// See g_lo.
uint32_t g_hi[static_cast<int>(Region::NUM_REGIONS)];

/// Region of a request.
/// @param r  the request
/// @return the region
Region region_of(const Request& r)
{
   return static_cast<Region>(r.dst >> kRegionShift);
}
/// Byte offset of a request.
/// @param r  the request
/// @return the offset
uint32_t offset_of(const Request& r)
{
   return r.dst & kOffsetMask;
}
/// Bytes of a request.
/// @param r  the request
/// @return the size
uint32_t bytes_of(const Request& r)
{
   return r.size_mode & kSizeMask;
}
/// Mode of a request.
/// @param r  the request
/// @return the mode
Mode mode_of(const Request& r)
{
   return static_cast<Mode>((r.size_mode >> kModeShift) & kModeMask);
}
/// Source kind of a request.
/// @param r  the request
/// @return the kind
Source source_of(const Request& r)
{
   return static_cast<Source>(r.size_mode >> kSourceShift);
}
/// Estimated cycles of a request.
/// @param r  the request
/// @return cycles
uint32_t cost_of(const Request& r)
{
   return cost(mode_of(r), source_of(r), bytes_of(r));
}

/// Clears the overlap bounds.
void clear_bounds()
{
   for (int i = 0; i < static_cast<int>(Region::NUM_REGIONS); ++i)
   {
      g_lo[i] = kNoLowBound;
      g_hi[i] = 0;
   }
}

/// Widens the overlap bounds by one request.
/// @param r  the request
void add_bounds(const Request& r)
{
   const auto i = static_cast<int>(region_of(r));
   const uint32_t lo = offset_of(r);
   const uint32_t hi = lo + bytes_of(r);
   if (lo < g_lo[i])
   {
      g_lo[i] = lo;
   }
   if (hi > g_hi[i])
   {
      g_hi[i] = hi;
   }
}

/// Recomputes the overlap bounds from what is still waiting.
void rebuild_bounds()
{
   clear_bounds();
   for (uint16_t i = 0; i < g_must.size(); ++i)
   {
      add_bounds(g_must.at(i));
   }
   for (uint16_t i = 0; i < g_stream.size(); ++i)
   {
      add_bounds(g_stream.at(i));
   }
}

/// Whether a waiting request of a ring overlaps a byte range.
/// @tparam N      ring size
/// @param ring    the ring
/// @param region  region
/// @param offset  first byte
/// @param bytes   number of bytes
/// @return true when one overlaps
template <uint16_t N>
bool overlaps(const Ring<N>& ring, Region region, uint32_t offset, uint32_t bytes)
{
   for (uint16_t i = 0; i < ring.size(); ++i)
   {
      const Request& r = ring.at(i);
      if (region_of(r) == region && offset < offset_of(r) + bytes_of(r) && offset + bytes > offset_of(r))
      {
         return true;
      }
   }
   return false;
}

/// Writes one request to video memory. Runs from IWRAM on the GBA (it lands
/// every request of every VBlank).
/// @param r  the request
CAPSULE_IWRAM_CODE void land(const Request& r)
{
   const Region region = region_of(r);
   const uint32_t offset = offset_of(r);
   const uint32_t bytes = bytes_of(r);
   switch (mode_of(r))
   {
   case Mode::Copy32:
      video::transfer32(region, offset, static_cast<const uint32_t*>(r.data.src), bytes / kWordBytes);
      break;
   case Mode::Copy16:
      video::transfer16(region, offset, static_cast<const uint16_t*>(r.data.src), bytes / kHalfwordBytes);
      break;
   case Mode::Fill32:
      video::fill32(region, offset, r.data.value, bytes / kWordBytes);
      break;
   case Mode::NUM_MODES:
      break;
   }
   g_stats.bytes_total += bytes;
}

/// Lands every must-land request now.
void land_must_now()
{
   while (g_must.size() != 0)
   {
      land(g_must.front());
      g_must.pop();
      ++g_stats.forced;
   }
   rebuild_bounds();
}

/// Lands the oldest streaming requests until @p slots are free.
/// @param slots  free slots wanted
void make_stream_room(uint16_t slots)
{
   if (g_stream.free() >= slots)
   {
      return;
   }
   while (g_stream.free() < slots)
   {
      land(g_stream.front());
      g_stream.pop();
      ++g_stats.forced;
   }
   rebuild_bounds();
}

/// Builds a request.
/// @param region  destination region
/// @param offset  byte offset
/// @param bytes   size
/// @param mode    mode
/// @param source  where copy data lives
/// @return the request, data unset
Request make(Region region, uint32_t offset, uint32_t bytes, Mode mode, Source source)
{
   Request r{};
   r.dst = (static_cast<uint32_t>(region) << kRegionShift) | offset;
   r.size_mode = bytes | (static_cast<uint32_t>(mode) << kModeShift) | (static_cast<uint32_t>(source) << kSourceShift);
   return r;
}

/// Extends the newest request of a ring by @p r when @p r continues it (same
/// region, mode and source, destination and source data both contiguous, the
/// result within @p limit bytes). Adjacent small uploads (palette banks, map
/// rows) then cost one request.
/// @tparam N    ring size
/// @param ring  the ring
/// @param r     the new request
/// @param limit largest merged size
/// @return true when merged (nothing to push)
template <uint16_t N>
bool try_merge(Ring<N>& ring, const Request& r, uint32_t limit)
{
   if (ring.size() == 0)
   {
      return false;
   }
   Request& last = ring.back();
   const Mode mode = mode_of(r);
   // The top byte of size_mode is the mode and the source kind: both must match.
   if ((last.size_mode >> kModeShift) != (r.size_mode >> kModeShift) || region_of(last) != region_of(r) ||
       offset_of(last) + bytes_of(last) != offset_of(r) || bytes_of(last) + bytes_of(r) > limit)
   {
      return false;
   }
   if (mode == Mode::Fill32 ? last.data.value != r.data.value
                            : static_cast<const uint8_t*>(last.data.src) + bytes_of(last) != r.data.src)
   {
      return false;
   }
   last.size_mode += bytes_of(r);
   add_bounds(last);
   return true;
}

/// Queues one request of any mode, splitting streaming requests into pieces.
/// @param region    destination region
/// @param offset    byte offset
/// @param src       copy data, or nullptr for a fill
/// @param value     fill word
/// @param bytes     size
/// @param mode      mode
/// @param priority  ring
/// @param source    where @p src lives
void enqueue(Region region, uint32_t offset, const void* src, uint32_t value, uint32_t bytes, Mode mode,
             Priority priority, Source source)
{
   const uint32_t unit = mode == Mode::Copy16 ? kHalfwordBytes : kWordBytes;
   ENGINE_ASSERT(region < Region::NUM_REGIONS && priority < Priority::NUM_PRIORITIES);
   ENGINE_ASSERT(offset % unit == 0 && bytes % unit == 0);
   // Palettes are colors (halfwords): a word copy of them would depend on the host's byte order.
   ENGINE_ASSERT(mode != Mode::Copy32 || (region != Region::BgPalette && region != Region::ObjPalette));
   ENGINE_ASSERT(offset + bytes <= video::region_bytes(region));
   ENGINE_ASSERT(mode == Mode::Fill32 || (src != nullptr && video::classify(src, source) == source));
   if (bytes == 0)
   {
      return;
   }
   if (priority == Priority::MustLand)
   {
      ENGINE_ASSERT(bytes <= kMaxMustLandBytes);
      if (g_must.free() == 0)
      {
         land_must_now();
      }
      Request r = make(region, offset, bytes, mode, source);
      if (mode == Mode::Fill32)
      {
         r.data.value = value;
      }
      else
      {
         r.data.src = src;
      }
      if (try_merge(g_must, r, kMaxMustLandBytes))
      {
         return;
      }
      g_must.push(r);
      add_bounds(r);
      if (g_must.size() > g_stats.peak_must_land)
      {
         g_stats.peak_must_land = g_must.size();
      }
      return;
   }
   const uint32_t pieces = (bytes + kPieceBytes - 1) / kPieceBytes;
   if (pieces <= kStreamingSlots)
   {
      make_stream_room(static_cast<uint16_t>(pieces));
   }
   else
   {
      make_stream_room(kStreamingSlots);   // larger than the ring: land as it fills
   }
   uint32_t done = 0;
   while (done < bytes)
   {
      const uint32_t n = bytes - done < kPieceBytes ? bytes - done : kPieceBytes;
      if (g_stream.free() == 0)
      {
         make_stream_room(1);
      }
      Request r = make(region, offset + done, n, mode, source);
      if (mode == Mode::Fill32)
      {
         r.data.value = value;
      }
      else
      {
         r.data.src = static_cast<const uint8_t*>(src) + done;
      }
      done += n;
      if (try_merge(g_stream, r, kPieceBytes))
      {
         continue;
      }
      g_stream.push(r);
      add_bounds(r);
   }
   if (g_stream.size() > g_stats.peak_streaming)
   {
      g_stats.peak_streaming = g_stream.size();
   }
}

} // namespace

void copy32(Region region, uint32_t offset, const uint32_t* src, uint32_t words, Priority priority, Source source)
{
   enqueue(region, offset, src, 0, words * kWordBytes, Mode::Copy32, priority, source);
}

void copy16(Region region, uint32_t offset, const uint16_t* src, uint32_t halfwords, Priority priority, Source source)
{
   enqueue(region, offset, src, 0, halfwords * kHalfwordBytes, Mode::Copy16, priority, source);
}

void fill32(Region region, uint32_t offset, uint32_t value, uint32_t words, Priority priority)
{
   enqueue(region, offset, nullptr, value, words * kWordBytes, Mode::Fill32, priority, Source::Iwram);
}

// The VBlank landing loop: IWRAM on the GBA.
CAPSULE_IWRAM_CODE void flush()
{
   uint32_t spent = 0;
   const uint32_t before = g_stats.bytes_total;
   while (g_must.size() != 0)
   {
      spent += cost_of(g_must.front());
      land(g_must.front());
      g_must.pop();
   }
   if (spent > kBudgetCycles)
   {
      ++g_stats.overruns;
   }
   while (g_stream.size() != 0)
   {
      const uint32_t c = cost_of(g_stream.front());
      if (spent + c > kBudgetCycles)
      {
         break;
      }
      spent += c;
      land(g_stream.front());
      g_stream.pop();
   }
   g_stats.cycles_last = spent;
   g_stats.bytes_last = g_stats.bytes_total - before;
   ++g_stats.frames;
   rebuild_bounds();
}

void flush_all()
{
   if (idle())
   {
      clear_bounds();
      return;
   }
   while (g_must.size() != 0)
   {
      land(g_must.front());
      g_must.pop();
   }
   while (g_stream.size() != 0)
   {
      land(g_stream.front());
      g_stream.pop();
   }
   clear_bounds();
}

void before_immediate(Region region, uint32_t offset, uint32_t bytes)
{
   const auto i = static_cast<int>(region);
   if (offset >= g_hi[i] || offset + bytes <= g_lo[i])
   {
      return;
   }   // nothing queued near it
   // The span is coarse (a ring row and an animation frame are far apart):
   // land only when a queued request really overlaps, so unrelated
   // immediate writes (text on BG0) never land ring rows mid-frame.
   if (overlaps(g_must, region, offset, bytes) || overlaps(g_stream, region, offset, bytes))
   {
      flush_all();
   }
}

uint16_t pending(Priority priority)
{
   return priority == Priority::MustLand ? g_must.size() : g_stream.size();
}

bool idle()
{
   return g_must.size() == 0 && g_stream.size() == 0;
}

namespace
{

/// C13c: whether a waiting copy of a ring reads from memory.
/// @tparam N    the ring's size
/// @param ring  the ring
/// @param lo    the memory's first byte
/// @param hi    one past its last
/// @return true when one does
template <uint16_t N>
bool ring_reads(const Ring<N>& ring, uintptr_t lo, uintptr_t hi)
{
   for (uint16_t i = 0; i < ring.size(); ++i)
   {
      const Request& r = ring.at(i);
      if (mode_of(r) == Mode::Fill32)
      {
         continue;   // a fill reads no memory
      }
      const auto src = reinterpret_cast<uintptr_t>(r.data.src);
      if (src < hi && lo < src + bytes_of(r))
      {
         return true;
      }
   }
   return false;
}

}   // namespace

bool reads_from(const void* begin, uint32_t bytes)
{
   const auto lo = reinterpret_cast<uintptr_t>(begin);
   const uintptr_t hi = lo + bytes;
   return ring_reads(g_must, lo, hi) || ring_reads(g_stream, lo, hi);
}

const Stats& stats()
{
   return g_stats;
}

namespace
{

TileGuard g_tile_guard = nullptr;   ///< C13a: the BG tile write check, or nullptr.

}   // namespace

void set_tile_guard(TileGuard guard)
{
   g_tile_guard = guard;
}

bool tiles_allowed(uint32_t offset, uint32_t bytes)
{
   return g_tile_guard == nullptr || g_tile_guard(offset, bytes);
}

void reset()
{
   g_must.head = g_must.tail = 0;
   g_stream.head = g_stream.tail = 0;
   g_stats = Stats{};
   clear_bounds();
}

} // namespace capsule::hal::vram
