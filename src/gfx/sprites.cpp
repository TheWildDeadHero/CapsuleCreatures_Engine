/// @file
/// Sprite slots (see capsule/gfx/sprites.h).
#include "capsule/gfx/sprites.h"

#include "capsule/core/assert.h"
#include "capsule/core/memory.h"
#include "capsule/core/trig.h"
#include "capsule/gfx/palette.h"
#include "capsule/hal/vram_queue.h"

namespace capsule::gfx::sprites
{
namespace
{

namespace video = hal::video;

/// Q14 sine over Q8 scale gives Q8 matrix entries when multiplied by this: 256 * 256 / 16384.
constexpr int32_t kMatrixFactor = kQ8One * kQ8One / kQ14One;
/// Bits of a hardware draw priority (0 front to 3).
constexpr uint8_t kPriorityMask = 3;
/// Affine sprites draw in a box twice their size, so rotated corners are not clipped.
constexpr int kDoubleSize = 2;
/// Slot::uploaded: no frame in VRAM yet.
constexpr uint8_t kNoFrame = 0xFF;
/// 32-bit words of one 4bpp tile.
constexpr uint32_t kTileWords = static_cast<uint32_t>(video::kTileBytes4bpp) / sizeof(uint32_t);
/// C2b: tiles per word of the heap's allocation map.
constexpr int kBitsPerWord = 32;
/// C2b: words of the allocation map.
constexpr int kMapWords = kTiles / kBitsPerWord;
/// C2b: a tile's map word: t >> 5.
constexpr int kWordShift = 5;
/// C2b: a tile's bit in its word: t & 31.
constexpr int kBitMask = kBitsPerWord - 1;
/// C2b: frames the instances' frame cache holds (each instance shows one).
constexpr int kCachedFrames = kInstances;
/// C2b: Instance::cached: no frame.
constexpr uint8_t kNoCached = 0xFF;
static_assert(kTiles % kBitsPerWord == 0 && kCachedFrames < kNoCached);
/// C2b: words of one queued piece (the queue's largest must-land request).
constexpr uint32_t kPieceWords = hal::vram::kMaxMustLandBytes / sizeof(uint32_t);

/// One sprite slot.
struct Slot
{
   const asset::Sprite* sprite;   ///< Loaded sprite, or nullptr.
   int16_t x;                    ///< Left edge, pixels.
   int16_t y;                    ///< Top edge, pixels.
   uint16_t scale;                ///< Q8; 256 = 1.0.
   uint16_t angle;                ///< 16-bit turn.
   uint8_t shown;                ///< 1 while shown.
   video::ObjMode mode;           ///< Drawing mode.
   uint8_t animation;            ///< Playing animation.
   uint8_t step;                 ///< Position in the animation.
   uint8_t timer;                ///< Frames shown of the current step.
   uint8_t priority;             ///< 0 (front) to 3; see set_priority().
   bool paused;                  ///< I3: the animation stays on its step.
   bool half;                    ///< I3: half speed (the timer runs on every other frame).
   bool tick;                    ///< I3: half speed's frame toggle.
   uint8_t uploaded;             ///< I3: streamed sprites: the frame in the slot's tiles, or kNoFrame.
   uint8_t sub;                  ///< DS1: sub-priority among sprites of equal priority (0 front).
   int16_t top;                  ///< DS1: top edge on screen as last written (the sort's tie-break).
   uint16_t tile;                ///< C2b: its first tile in the heap, or kNoTiles.
   uint16_t tiles;               ///< C2b: tiles it holds (0: none).
   uint8_t bank;                 ///< C2b: its palette bank, or kNoBank.
};
CAPSULE_EWRAM_BSS Slot g_slots[kSlots];   ///< The slots (D-42: EWRAM, the main loop's).

/// C2: one instance (C2b: a frame of a sprite, without a slot).
struct Instance
{
   bool used;                     ///< Taken (zero-initialised: free).
   const asset::Sprite* sprite;   ///< Its sprite.
   uint8_t cached;                ///< C2b: its frame in the frame cache, or kNoCached.
   uint8_t bank;                  ///< C2b: its palette bank, or kNoBank.
   int16_t x;            ///< Left edge, pixels.
   int16_t y;            ///< Top edge, pixels.
   uint8_t flips;        ///< asset::kFlip... bits.
   uint8_t priority;     ///< 0 (front) to 3.
   uint8_t sub;          ///< Sub-priority (0 front).
   bool shown;           ///< Shown.
   int16_t top;          ///< Top edge on screen as last written (the sort's tie-break).
};
CAPSULE_EWRAM_BSS Instance g_instances[kInstances];   ///< The instances (EWRAM).

/// C2b: one frame in the instances' frame cache (tiles shared by every instance showing it).
struct CachedFrame
{
   const asset::Sprite* sprite;   ///< The sprite, or nullptr (free).
   uint16_t tile;                 ///< Its first tile in the heap.
   uint8_t frame;                 ///< The sprite's frame.
   uint8_t users;                 ///< Instances showing it.
};

/// C2b: the heap and the palette pool (EWRAM, the main loop's).
struct Memory
{
   uint32_t tiles[kMapWords];          ///< Allocated tiles (a bit per tile).
   CachedFrame frames[kCachedFrames];  ///< The instances' frames.
   uint8_t users[kBanks];              ///< Sprites drawing with each bank (0: free).
   bool own[kBanks];                   ///< A bank one slot changes (never shared).
};
CAPSULE_EWRAM_BSS Memory g_memory;   ///< The heap and the pool.

/// DS1: the sorted draw order (an order of the drawables, front first; kept from frame to frame). Drawable d is
/// slot d below kSlots, else instance d - kSlots (C2).
struct Order
{
   bool sorted;                  ///< Sorting is on.
   uint8_t items[kEntries];      ///< The drawables, front first.
   uint8_t entry[kEntries];      ///< The OAM entry each drawable was written to.
};
CAPSULE_EWRAM_BSS Order g_order;   ///< The draw order (EWRAM).
CAPSULE_EWRAM_BSS video::ObjAttr g_attrs[kEntries];   ///< Each drawable's entry in the update being written.
uint8_t g_first_entry = 0;   ///< T1: the OAM entry of the draw order's first slot (set_first_entry()).

/// Bits the priority is shifted by above the sub-priority in a sort key.
constexpr int kPriorityKeyShift = 8;

/// DS1: a drawable's sort key: priority, then sub-priority (lower in front).
/// @param d  the drawable (a slot below kSlots, else an instance)
/// @return the key
uint16_t sort_key(uint8_t d)
{
   if (d < kSlots)
   {
      return static_cast<uint16_t>(g_slots[d].priority << kPriorityKeyShift | g_slots[d].sub);
   }
   const Instance& i = g_instances[d - kSlots];
   return static_cast<uint16_t>(i.priority << kPriorityKeyShift | i.sub);
}

/// DS1: a drawable's top edge as last written.
/// @param d  the drawable
/// @return it
int16_t top_of(uint8_t d)
{
   return d < kSlots ? g_slots[d].top : g_instances[d - kSlots].top;
}

/// DS1: whether the drawable before belongs behind the drawable after (lower key in front; equal keys: the lower
/// sprite on screen in front).
/// @param before  the drawable now in front
/// @param after   the drawable now behind it
/// @return true when they swap
bool behind(uint8_t before, uint8_t after)
{
   const uint16_t a = sort_key(before);
   const uint16_t b = sort_key(after);
   return a > b || (a == b && top_of(before) < top_of(after));
}

/// DS1: sorts the draw order: an insertion sort over the last frame's order, so equal sprites keep it.
void sort_order()
{
   for (int i = 1; i < kEntries; ++i)
   {
      int j = i;
      while (j > 0 && behind(g_order.items[j - 1], g_order.items[j]))
      {
         const uint8_t t = g_order.items[j];
         g_order.items[j] = g_order.items[j - 1];
         g_order.items[j - 1] = t;
         --j;
      }
   }
}

/// C2b: whether a tile is allocated.
/// @param t  tile 0 to kTiles - 1
/// @return true when it is
bool allocated(int t)
{
   return (g_memory.tiles[t >> kWordShift] >> (t & kBitMask) & 1u) != 0;
}

/// C2b: marks tiles allocated or free.
/// @param first  first tile
/// @param count  tiles
/// @param on     allocated
void mark(int first, int count, bool on)
{
   for (int t = first; t < first + count; ++t)
   {
      const uint32_t bit = uint32_t{1} << (t & kBitMask);
      uint32_t& word = g_memory.tiles[t >> kWordShift];
      word = on ? (word | bit) : (word & ~bit);
   }
}

/// C2b: first fit over the heap (the reference games' allocator).
/// @param count  tiles
/// @return the first tile, or kNoTiles
uint16_t alloc_tiles(int count)
{
   if (count <= 0 || count > kTiles)
   {
      return kNoTiles;
   }
   int run = 0;
   for (int t = 0; t < kTiles; ++t)
   {
      run = allocated(t) ? 0 : run + 1;
      if (run == count)
      {
         const int first = t + 1 - count;
         mark(first, count, true);
         return static_cast<uint16_t>(first);
      }
   }
   return kNoTiles;
}

/// C2b: gives a slot's tiles back to the heap.
/// @param s  the slot
void free_slot_tiles(Slot& s)
{
   if (s.tile != kNoTiles && s.tiles > 0)
   {
      mark(s.tile, s.tiles, false);
   }
   s.tile = kNoTiles;
   s.tiles = 0;
}

/// C2b: whether a bank holds a palette's colors 1-15 (color 0 is transparent).
/// @param bank    the bank
/// @param colors  the palette (16 colors)
/// @return true when they match
bool same_colors(int bank, const video::Color* colors)
{
   for (int c = 1; c < palette::kBankColors; ++c)
   {
      if (palette::base(true, bank * palette::kBankColors + c) != colors[c])
      {
         return false;
      }
   }
   return true;
}

/// C2b: a free bank: the preferred one when free, else the highest free (the slots prefer their own numbers, so the
/// banks above the slots in use stay free longest).
/// @param preferred  the bank wanted (kNoBank: any)
/// @return the bank, or kNoBank
uint8_t free_bank(uint8_t preferred)
{
   if (preferred < kBanks && g_memory.users[preferred] == 0)
   {
      return preferred;
   }
   for (int b = kBanks - 1; b >= 0; --b)
   {
      if (g_memory.users[b] == 0)
      {
         return static_cast<uint8_t>(b);
      }
   }
   return kNoBank;
}

/// C2b: a bank for a palette: one holding its colors already (not a slot's own), else a free one loaded with them.
/// @param colors     the palette (16 colors)
/// @param preferred  the bank wanted when none holds them (kNoBank: any)
/// @return the bank (counted), or kNoBank when every bank is taken
uint8_t acquire_bank(const video::Color* colors, uint8_t preferred)
{
   for (int b = 0; b < kBanks; ++b)
   {
      if (g_memory.users[b] > 0 && !g_memory.own[b] && same_colors(b, colors))
      {
         ++g_memory.users[b];
         return static_cast<uint8_t>(b);
      }
   }
   const uint8_t b = free_bank(preferred);
   if (b == kNoBank)
   {
      return kNoBank;
   }
   g_memory.users[b] = 1;
   g_memory.own[b] = false;
   // color 0 is transparent and never loaded
   palette::load_obj(static_cast<uint8_t>(b * palette::kBankColors + 1), colors + 1, palette::kBankColors - 1);
   return b;
}

/// C2b: gives a bank back to the pool (kNoBank: nothing).
/// @param bank  the bank
void release_bank(uint8_t bank)
{
   if (bank < kBanks && g_memory.users[bank] > 0 && --g_memory.users[bank] == 0)
   {
      g_memory.own[bank] = false;
   }
}

/// C2b: frees a slot's tiles and bank.
/// @param s  the slot
void free_slot(Slot& s)
{
   free_slot_tiles(s);
   release_bank(s.bank);
   s.bank = kNoBank;
}

/// C2b: queues tiles into the heap (must-land, in pieces the queue takes: a sprite may now hold more tiles than one
/// request carries).
/// @param tile   the first tile
/// @param words  the tiles' words
/// @param count  tiles
void upload_tiles(uint16_t tile, const uint32_t* words, uint32_t count)
{
   const uint32_t total = count * kTileWords;
   const uint32_t base = static_cast<uint32_t>(tile) * video::kTileBytes4bpp;
   for (uint32_t done = 0; done < total; done += kPieceWords)
   {
      const uint32_t n = total - done < kPieceWords ? total - done : kPieceWords;
      hal::vram::copy32(video::Region::ObjVram, base + done * sizeof(uint32_t), words + done, n,
                        hal::vram::Priority::MustLand, video::Source::Rom);
   }
}

/// C2b: uploads a frame of a sprite into the heap (must-land).
/// @param tile    the first tile
/// @param sprite  the sprite
/// @param frame   its frame
void upload_frame(uint16_t tile, const asset::Sprite& sprite, int frame)
{
   const auto tiles = static_cast<uint32_t>(asset::tiles_per_frame(sprite));
   upload_tiles(tile, sprite.tiles + static_cast<uint32_t>(frame) * tiles * kTileWords, tiles);
}

/// C2b: a frame in the cache: the one holding it, or a new one with its tiles uploaded.
/// @param sprite  the sprite
/// @param frame   its frame
/// @return the cached frame (counted), or kNoCached (no entry or no room in the heap)
uint8_t acquire_frame(const asset::Sprite& sprite, uint8_t frame)
{
   uint8_t free = kNoCached;
   for (uint8_t k = 0; k < kCachedFrames; ++k)
   {
      CachedFrame& c = g_memory.frames[k];
      if (c.sprite == &sprite && c.frame == frame)
      {
         ++c.users;
         return k;
      }
      if (c.sprite == nullptr && free == kNoCached)
      {
         free = k;
      }
   }
   if (free == kNoCached)
   {
      return kNoCached;
   }
   const uint16_t tile = alloc_tiles(asset::tiles_per_frame(sprite));
   if (tile == kNoTiles)
   {
      return kNoCached;
   }
   g_memory.frames[free] = CachedFrame{&sprite, tile, frame, 1};
   upload_frame(tile, sprite, frame);
   return free;
}

/// C2b: gives a cached frame back (its tiles to the heap when no instance shows it).
/// @param k  the cached frame, or kNoCached
void release_frame(uint8_t k)
{
   if (k >= kCachedFrames)
   {
      return;
   }
   CachedFrame& c = g_memory.frames[k];
   if (c.users > 0 && --c.users == 0)
   {
      mark(c.tile, asset::tiles_per_frame(*c.sprite), false);
      c = CachedFrame{};
   }
}

/// C2b: the bank a sprite is drawn with (none: bank 0's colors).
/// @param bank  its bank, or kNoBank
/// @return 0 to kBanks - 1
uint8_t draw_bank(uint8_t bank)
{
   return bank < kBanks ? bank : uint8_t{0};
}


/// Writes a slot's OBJ matrix: the inverse of scale and rotation.
/// @param n  slot (= matrix)
void write_matrix(int n)
{
   const Slot& s = g_slots[n];
   const int32_t c = cos_q14(s.angle);
   const int32_t sn = sin_q14(s.angle);
   // Q14 / Q8 scale -> Q8: v * 256 / (16384 * scale / 256) = v * 4 / scale.
   const int32_t scale = s.scale == 0 ? 1 : s.scale;
   const auto pa = static_cast<int16_t>(c * kMatrixFactor / scale);
   const auto pb = static_cast<int16_t>(sn * kMatrixFactor / scale);
   video::set_obj_affine(static_cast<uint8_t>(n), pa, pb, static_cast<int16_t>(-pb), pa);
}

/// Frame the slot shows now.
/// @param s  the slot
/// @return frame index
int current_frame(const Slot& s)
{
   const asset::Animation& a = s.sprite->animations[s.animation];
   return a.frames[s.step];
}

/// The flip bits of the entry the slot shows now (I3).
/// @param s  the slot
/// @return asset::kFlip... bits
uint8_t current_flips(const Slot& s)
{
   const asset::Animation& a = s.sprite->animations[s.animation];
   return a.flips != nullptr ? a.flips[s.step] : uint8_t{0};
}

/// Whether a sprite keeps only its shown frame in VRAM (I3).
/// @param sprite  the sprite
/// @return true for a streamed sprite
bool streamed(const asset::Sprite& sprite)
{
   return (sprite.flags & asset::kSpriteStreamed) != 0;
}

/// Queues a streamed slot's frame into its tiles when it shows another one (I3).
/// @param n  the slot
/// @param frame  the frame it shows
void stream_frame(int n, int frame)
{
   Slot& s = g_slots[n];
   if (s.uploaded == frame)
   {
      return;
   }
   s.uploaded = static_cast<uint8_t>(frame);
   upload_frame(s.tile, *s.sprite, frame);
}

/// Advances a slot's animation when its entry's time is over (I3: before the entry is shown, so a caller between
/// two updates sees the entry last shown).
/// @param s  the slot
void advance(Slot& s)
{
   const asset::Animation& a = s.sprite->animations[s.animation];
   const uint8_t span = a.durations != nullptr ? a.durations[s.step] : a.period;
   if (a.count <= 1 || s.paused || s.timer < span)
   {
      return;
   }
   s.timer = 0;
   if (s.step + 1 < a.count)
   {
      ++s.step;
   }
   else if (a.loop)
   {
      s.step = 0;
   }
}

/// Counts a frame of the entry shown (half speed: every other frame); the count stops at the entry's span.
/// @param s  the slot
void count_frame(Slot& s)
{
   const asset::Animation& a = s.sprite->animations[s.animation];
   const uint8_t span = a.durations != nullptr ? a.durations[s.step] : a.period;
   s.tick = !s.tick;
   if (s.half && s.tick)
   {
      return;
   }
   if (s.timer < span)
   {
      ++s.timer;
   }
}

} // namespace

void reset()
{
   for (int n = 0; n < kSlots; ++n)
   {
      g_slots[n] = Slot{};
      g_slots[n].scale = kQ8One;
      g_slots[n].tile = kNoTiles;
      g_slots[n].bank = kNoBank;
      video::set_obj(static_cast<uint8_t>(g_first_entry + n), video::kObjHidden);
   }
   g_memory = Memory{};   // C2b: every tile and every bank free
   for (int k = kSlots; k < kEntries; ++k)
   {
      video::set_obj(static_cast<uint8_t>(g_first_entry + k), video::kObjHidden);
   }
   for (Instance& i : g_instances)
   {
      i = Instance{};
   }
   g_first_entry = 0;   // T1: entry n is slot n's again
   set_sorted(false);   // DS1: OAM entry n is slot n again
   video::set_obj_layer(true);
}

bool load(uint8_t slot, const asset::Sprite& sprite)
{
   ENGINE_ASSERT(slot < kSlots && sprite.animation_count >= 1);
   const int frame_tiles = asset::tiles_per_frame(sprite);
   const int tiles = streamed(sprite) ? frame_tiles : frame_tiles * sprite.frame_count;
   Slot& s = g_slots[slot];
   // The bank first, so a sprite of the colors before keeps its bank (counted twice for a moment)
   const uint8_t bank = acquire_bank(sprite.palette, slot);
   release_bank(s.bank);
   s.bank = bank;
   if (s.tiles != tiles)
   {
      free_slot_tiles(s);   // the tiles before stay in place when the new sprite takes as many
      s.tile = alloc_tiles(tiles);
      s.tiles = s.tile != kNoTiles ? static_cast<uint16_t>(tiles) : uint16_t{0};
   }
   s.sprite = &sprite;
   s.animation = 0;
   s.step = 0;
   s.timer = 0;
   s.paused = false;
   s.half = false;
   // a streamed sprite loads the frame its first animation starts on; update() streams the others
   const int first = streamed(sprite) ? current_frame(s) : 0;
   s.uploaded = streamed(sprite) ? static_cast<uint8_t>(first) : kNoFrame;
   if (s.tile == kNoTiles)
   {
      return false;   // no room: the slot draws nothing
   }
   video::load_obj_tiles(static_cast<video::TileIndex>(s.tile),
                         sprite.tiles + static_cast<uint32_t>(first * frame_tiles) * kTileWords,
                         static_cast<uint16_t>(tiles));
   return bank != kNoBank;
}

void unload(uint8_t slot)
{
   ENGINE_ASSERT(slot < kSlots);
   Slot& s = g_slots[slot];
   free_slot(s);
   s.sprite = nullptr;
   s.shown = 0;
   s.uploaded = kNoFrame;
}

uint8_t palette_bank(uint8_t slot)
{
   ENGINE_ASSERT(slot < kSlots);
   return g_slots[slot].bank;
}

uint8_t own_palette(uint8_t slot)
{
   ENGINE_ASSERT(slot < kSlots);
   Slot& s = g_slots[slot];
   if (s.bank >= kBanks)
   {
      return kNoBank;
   }
   if (g_memory.users[s.bank] > 1)
   {
      // Shared: the slot moves to a copy of the colors (all 16, as its users may have changed color 0)
      const uint8_t b = free_bank(slot);
      if (b == kNoBank)
      {
         return kNoBank;
      }
      video::Color colors[palette::kBankColors];
      for (int c = 0; c < palette::kBankColors; ++c)
      {
         colors[c] = palette::base(true, s.bank * palette::kBankColors + c);
      }
      palette::load_obj(static_cast<uint8_t>(b * palette::kBankColors), colors, palette::kBankColors);
      release_bank(s.bank);
      g_memory.users[b] = 1;
      s.bank = b;
   }
   g_memory.own[s.bank] = true;
   return s.bank;
}

uint16_t reserve_tiles(uint8_t slot, uint16_t count)
{
   ENGINE_ASSERT(slot < kSlots);
   Slot& s = g_slots[slot];
   s.sprite = nullptr;   // as unload(); its bank stays (reserve_palette() may have given it)
   s.shown = 0;
   s.uploaded = kNoFrame;
   free_slot_tiles(s);
   s.tile = alloc_tiles(count);
   s.tiles = s.tile != kNoTiles ? count : uint16_t{0};
   return s.tile;
}

uint8_t reserve_palette(uint8_t slot)
{
   ENGINE_ASSERT(slot < kSlots);
   Slot& s = g_slots[slot];
   release_bank(s.bank);
   s.bank = free_bank(slot);
   if (s.bank != kNoBank)
   {
      g_memory.users[s.bank] = 1;
      g_memory.own[s.bank] = true;
   }
   return s.bank;
}

uint16_t free_tiles()
{
   int n = 0;
   for (int t = 0; t < kTiles; ++t)
   {
      n += allocated(t) ? 0 : 1;
   }
   return static_cast<uint16_t>(n);
}

uint16_t largest_free_run()
{
   int best = 0;
   int run = 0;
   for (int t = 0; t < kTiles; ++t)
   {
      run = allocated(t) ? 0 : run + 1;
      best = run > best ? run : best;
   }
   return static_cast<uint16_t>(best);
}

uint8_t free_banks()
{
   int n = 0;
   for (const uint8_t users : g_memory.users)
   {
      n += users == 0 ? 1 : 0;
   }
   return static_cast<uint8_t>(n);
}

void show(uint8_t slot, bool on)
{
   ENGINE_ASSERT(slot < kSlots);
   g_slots[slot].shown = on ? 1 : 0;
}

void set_position(uint8_t slot, int16_t x, int16_t y)
{
   ENGINE_ASSERT(slot < kSlots);
   g_slots[slot].x = x;
   g_slots[slot].y = y;
}

void play(uint8_t slot, uint16_t animation)
{
   ENGINE_ASSERT(slot < kSlots);
   Slot& s = g_slots[slot];
   if (s.sprite == nullptr || animation >= s.sprite->animation_count)
   {
      return;
   }
   s.animation = static_cast<uint8_t>(animation);
   s.step = 0;
   s.timer = 0;
}

void play_at(uint8_t slot, uint16_t animation, uint8_t entry)
{
   ENGINE_ASSERT(slot < kSlots);
   Slot& s = g_slots[slot];
   if (s.sprite == nullptr || animation >= s.sprite->animation_count)
   {
      return;
   }
   const asset::Animation& a = s.sprite->animations[animation];
   s.animation = static_cast<uint8_t>(animation);
   s.step = entry < a.count ? entry : static_cast<uint8_t>(a.count - 1);
   s.timer = 0;
}

void pause(uint8_t slot, bool paused)
{
   ENGINE_ASSERT(slot < kSlots);
   g_slots[slot].paused = paused;
}

void set_half_speed(uint8_t slot, bool half)
{
   ENGINE_ASSERT(slot < kSlots);
   g_slots[slot].half = half;
}

uint16_t animation(uint8_t slot)
{
   ENGINE_ASSERT(slot < kSlots);
   return g_slots[slot].animation;
}

uint8_t entry(uint8_t slot)
{
   ENGINE_ASSERT(slot < kSlots);
   return g_slots[slot].step;
}

void set_transform(uint8_t slot, uint16_t scale_q8, uint16_t angle)
{
   ENGINE_ASSERT(slot < kAffineSlots);
   g_slots[slot].scale = scale_q8;
   g_slots[slot].angle = angle;
}

void set_priority(uint8_t slot, uint8_t priority)
{
   ENGINE_ASSERT(slot < kSlots);
   g_slots[slot].priority = static_cast<uint8_t>(priority & kPriorityMask);
}

void set_mode(uint8_t slot, video::ObjMode mode)
{
   ENGINE_ASSERT(slot < kSlots && mode < video::ObjMode::NUM_OBJ_MODES);
   g_slots[slot].mode = mode;
}

void set_order(uint8_t slot, uint8_t sub_priority)
{
   ENGINE_ASSERT(slot < kSlots);
   g_slots[slot].sub = sub_priority;
}

void set_sorted(bool on)
{
   if (on == g_order.sorted && on)
   {
      return;   // the order of the frames before is kept
   }
   g_order.sorted = on;
   for (int n = 0; n < kEntries; ++n)
   {   // a new order starts from the slots' own, the instances after them
      g_order.items[n] = static_cast<uint8_t>(n);
      g_order.entry[n] = static_cast<uint8_t>(n);
   }
}

uint8_t oam_entry(uint8_t slot)
{
   ENGINE_ASSERT(slot < kSlots);
   return g_order.entry[slot];
}

namespace
{

/// Advances a slot's animation one frame and gives its OAM entry (I3: the shown frame streamed in; DS1: its top
/// edge kept for the sort).
/// @param n  the slot
/// @return its attributes, or video::kObjHidden
video::ObjAttr slot_attr(int n)
{
   Slot& s = g_slots[n];
   if (s.sprite == nullptr || !s.shown || s.scale == 0 || s.tile == kNoTiles)
   {
      return video::kObjHidden;
   }
   // Animation: once a step has been shown for its duration (or `period`) display frames, move on (loop or hold
   // the last frame), then show the step and count the frame (I3: advancing first keeps the step last shown visible
   // to callers between updates; what shows is unchanged)
   advance(s);
   const int frame = current_frame(s);
   const uint8_t flips = current_flips(s);
   count_frame(s);
   if (streamed(*s.sprite))
   {
      stream_frame(n, frame);
   }
   const int w = video::obj_width(s.sprite->shape, s.sprite->size);
   const int h = video::obj_height(s.sprite->shape, s.sprite->size);
   const bool transformed = n < kAffineSlots && (s.scale != kQ8One || s.angle != 0);
   video::ObjDesc d{};
   d.shape = s.sprite->shape;
   d.size = s.sprite->size;
   d.palette_bank = draw_bank(s.bank);
   d.mode = s.mode;
   d.priority = s.priority;
   const int shown = streamed(*s.sprite) ? 0 : frame;   // a streamed slot's tiles hold the frame shown
   d.tile = static_cast<video::TileIndex>(s.tile + shown * asset::tiles_per_frame(*s.sprite));
   d.hflip = (flips & asset::kFlipH) != 0;
   d.vflip = (flips & asset::kFlipV) != 0;
   if (transformed)
   {
      d.affine = true;
      d.double_size = true;
      d.affine_group = static_cast<uint8_t>(n);
      // The doubled box is centred on the sprite: it starts half a sprite up and left.
      d.x = static_cast<int16_t>(s.x - w / kHalf);
      d.y = static_cast<int16_t>(s.y - h / kHalf);
      write_matrix(n);
   }
   else
   {
      d.x = s.x;
      d.y = s.y;
   }
   s.top = d.y;
   // Off-screen positions: OAM coordinates wrap, so hide what cannot show.
   const int box_w = transformed ? w * kDoubleSize : w;
   const int box_h = transformed ? h * kDoubleSize : h;
   if (d.x + box_w <= 0 || d.x >= video::kScreenWidth || d.y + box_h <= 0 || d.y >= video::kScreenHeight)
   {
      return video::kObjHidden;
   }
   return video::make_obj(d);
}

/// C2: an instance's OAM entry.
/// @param k  the instance
/// @return its attributes, or video::kObjHidden
video::ObjAttr instance_attr(int k)
{
   Instance& i = g_instances[k];
   if (!i.used || !i.shown || i.cached == kNoCached)
   {
      return video::kObjHidden;
   }
   const asset::Sprite& sprite = *i.sprite;
   const int w = video::obj_width(sprite.shape, sprite.size);
   const int h = video::obj_height(sprite.shape, sprite.size);
   i.top = i.y;
   if (i.x + w <= 0 || i.x >= video::kScreenWidth || i.y + h <= 0 || i.y >= video::kScreenHeight)
   {
      return video::kObjHidden;
   }
   video::ObjDesc d{};
   d.shape = sprite.shape;
   d.size = sprite.size;
   d.palette_bank = draw_bank(i.bank);
   d.priority = i.priority;
   d.tile = static_cast<video::TileIndex>(g_memory.frames[i.cached].tile);
   d.hflip = (i.flips & asset::kFlipH) != 0;
   d.vflip = (i.flips & asset::kFlipV) != 0;
   d.x = i.x;
   d.y = i.y;
   return video::make_obj(d);
}

/// C2: an instance's record.
/// @param instance  the instance
/// @return it
Instance& instance_at(uint8_t instance)
{
   ENGINE_ASSERT(instance < kInstances && g_instances[instance].used);
   return g_instances[instance < kInstances ? instance : 0];
}

} // namespace

uint8_t add_instance(const asset::Sprite& sprite)
{
   for (uint8_t k = 0; k < kInstances; ++k)
   {
      Instance& i = g_instances[k];
      if (!i.used)
      {
         i = Instance{};
         i.used = true;
         i.sprite = &sprite;
         i.cached = kNoCached;
         i.bank = acquire_bank(sprite.palette, kNoBank);
         return k;
      }
   }
   return kNoInstance;
}

void remove_instance(uint8_t instance)
{
   if (instance < kInstances && g_instances[instance].used)
   {
      Instance& i = g_instances[instance];   // one reset() freed already is free
      release_frame(i.cached);
      release_bank(i.bank);
      i = Instance{};
   }
}

void show_instance(uint8_t instance, bool on)
{
   instance_at(instance).shown = on;
}

void set_instance_position(uint8_t instance, int16_t x, int16_t y)
{
   Instance& i = instance_at(instance);
   i.x = x;
   i.y = y;
}

bool set_instance_frame(uint8_t instance, uint8_t frame, uint8_t flips)
{
   Instance& i = instance_at(instance);
   i.flips = flips;
   // The new frame first, so the frame shown before stays cached when it is the same
   const uint8_t cached = frame < i.sprite->frame_count ? acquire_frame(*i.sprite, frame) : kNoCached;
   release_frame(i.cached);
   i.cached = cached;
   return cached != kNoCached;
}

void set_instance_priority(uint8_t instance, uint8_t priority)
{
   instance_at(instance).priority = static_cast<uint8_t>(priority & kPriorityMask);
}

void set_instance_order(uint8_t instance, uint8_t sub_priority)
{
   instance_at(instance).sub = sub_priority;
}

uint8_t instance_entry(uint8_t instance)
{
   ENGINE_ASSERT(instance < kInstances);
   return g_order.entry[kSlots + (instance < kInstances ? instance : 0)];
}

void update()
{
   for (int n = 0; n < kSlots; ++n)
   {
      g_attrs[n] = slot_attr(n);
   }
   for (int k = 0; k < kInstances; ++k)
   {
      g_attrs[kSlots + k] = instance_attr(k);
   }
   if (g_order.sorted)
   {
      sort_order();
   }
   for (int k = 0; k < kEntries; ++k)
   {
      const uint8_t n = g_order.sorted ? g_order.items[k] : static_cast<uint8_t>(k);
      g_order.entry[n] = static_cast<uint8_t>(k);
      video::set_obj(static_cast<uint8_t>(g_first_entry + k), g_attrs[n]);
   }
}

void set_first_entry(uint8_t first)
{
   ENGINE_ASSERT(first + kEntries <= video::kNumObjs);
   if (first == g_first_entry)
   {
      return;
   }
   for (int k = 0; k < kEntries; ++k)
   {
      video::set_obj(static_cast<uint8_t>(g_first_entry + k), video::kObjHidden);   // the entries left
   }
   g_first_entry = first;
}

void reupload()
{
   for (int n = 0; n < video::kNumObjs; ++n)
   {
      video::set_obj(static_cast<uint8_t>(n), video::kObjHidden);   // a scene's entries past the slots too
   }
   for (int n = 0; n < kSlots; ++n)
   {
      const Slot& s = g_slots[n];
      if (s.sprite == nullptr || s.tile == kNoTiles)
      {
         continue;   // nothing loaded, no room, or tiles reserve_tiles() gave
      }
      const int frame_tiles = asset::tiles_per_frame(*s.sprite);
      const bool stream = streamed(*s.sprite);
      if (stream && s.uploaded == kNoFrame)
      {
         continue;
      }
      const int first = stream ? s.uploaded : 0;   // a streamed slot's tiles hold the frame it last uploaded
      upload_tiles(s.tile, s.sprite->tiles + static_cast<uint32_t>(first * frame_tiles) * kTileWords, s.tiles);
   }
   for (const CachedFrame& c : g_memory.frames)
   {
      if (c.sprite != nullptr)
      {
         upload_frame(c.tile, *c.sprite, c.frame);   // C2b: the instances' frames
      }
   }
}

} // namespace capsule::gfx::sprites
