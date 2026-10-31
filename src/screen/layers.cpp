/// @file
/// A screen's background layers (see capsule/screen/layers.h).
#include "capsule/screen/layers.h"

#include "capsule/core/assert.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/map/map.h"

namespace capsule::screen::layers
{
namespace
{

namespace video = hal::video;

/// One layer's state.
struct Layer
{
   LayerTemplate t;        ///< Its template.
   uint16_t* buffer;       ///< Its map buffer, or nullptr.
   bool used;              ///< The plan has it.
   bool affine;            ///< An affine layer in the plan's mode.
   bool scheduled;         ///< A copy is due at copy_scheduled().
};
Layer g_layers[kLayers];   ///< The layers.
uint8_t g_mode = 0;        ///< The plan's display mode.

constexpr int kWide = 1;            ///< Screen size bit: two blocks across.
constexpr int kTall = 2;            ///< Screen size bit: two blocks down.
constexpr int kBlocksAcross = 2;  ///< Screen blocks across (down) a wide (tall) map.
constexpr int kAffineBase = 16;     ///< An affine map's side, cells, at size 0 (16 << size).
constexpr int kTileField = 0x3FF;   ///< A map entry's tile bits.
constexpr int kTileFlipField = 0xFFF;   ///< Its tile and flip bits.
constexpr int kAttrField = 0xFC00;  ///< Its flip and bank bits.
constexpr uint32_t kTileBytes4 = 32;   ///< Bytes of a 4bpp tile.
constexpr uint32_t kTileBytes8 = 64;   ///< Bytes of an 8bpp tile.
/// Halfwords per queued map piece (one screen block: the queue's largest must-land request).
constexpr uint32_t kPieceHalfwords = hal::vram::kMaxMustLandBytes / video::kHalfwordBytes;
/// Words per queued tile piece.
constexpr uint32_t kPieceWords = hal::vram::kMaxMustLandBytes / video::kWordBytes;
constexpr int16_t kQ8Identity = 256;   ///< An affine layer's 1:1 matrix entry.

/// A layer by index.
/// @param bg  layer 0-3
/// @return its state
Layer& at(video::BgIndex bg)
{
   ENGINE_ASSERT(bg < kLayers);
   return g_layers[bg];
}

/// Whether a layer is affine in a display mode.
/// @param bg    the layer
/// @param mode  the mode
/// @return true for BG2 in mode 1 and BG2-BG3 in mode 2
bool affine_in(video::BgIndex bg, uint8_t mode)
{
   return (mode == video::kModeMixed && bg == video::kFirstAffineBg) ||
          (mode == video::kModeAffine && bg >= video::kFirstAffineBg);
}

/// Byte offset of a layer's map in BG memory.
/// @param l  the layer
/// @return the offset
uint32_t map_offset(const Layer& l)
{
   return static_cast<uint32_t>(l.t.screen_block) * video::kScreenBlockBytes;
}

/// Byte offset of one of a layer's tiles in BG memory.
/// @param l       the layer
/// @param offset  tile, from its first_tile
/// @return the offset
uint32_t tile_offset(const Layer& l, uint16_t offset)
{
   const uint32_t bytes = l.t.bpp8 ? kTileBytes8 : kTileBytes4;
   return static_cast<uint32_t>(l.t.char_block) * video::kCharBlockBytes +
          (static_cast<uint32_t>(l.t.first_tile) + offset) * bytes;
}

/// The reference games' entry rewrite of a rectangle copy.
/// @param src          source entry
/// @param dest         destination entry as it is
/// @param palette      a bank, kPaletteKeep or kPaletteRaw
/// @param tile_offset  added to the tile
/// @param palette_add  added to the bank
/// @return the new entry
uint16_t rewrite(uint16_t src, uint16_t dest, uint8_t palette, int16_t tile_offset, int16_t palette_add)
{
   const int moved = src + tile_offset;
   if (palette < kPaletteKeep)
   {
      return static_cast<uint16_t>((moved & kTileFlipField) + ((palette + palette_add) << video::kMapPaletteShift));
   }
   if (palette == kPaletteKeep)
   {
      return static_cast<uint16_t>(((dest & kAttrField) + (palette_add << video::kMapPaletteShift)) |
                                   (moved & kTileField));
   }
   return static_cast<uint16_t>(moved + (palette_add << video::kMapPaletteShift));
}

}   // namespace

void reset()
{
   for (int b = 0; b < kLayers; ++b)
   {
      Layer& l = g_layers[b];
      if (l.used)
      {
         video::configure_bg(static_cast<video::BgIndex>(b), false, l.t.priority, l.t.char_block, l.t.screen_block,
                             l.t.bpp8 != 0, l.t.screen_size);
      }
      l = Layer{};
   }
}

void apply(const Plan& plan)
{
   reset();
   hal::vram::set_tile_guard(tiles_clear_of_maps);   // C13a (R9 M4): text windows' tile copies are checked too
   g_mode = plan.mode;
   video::set_mode(plan.mode);
   for (int b = 0; b < kLayers; ++b)
   {
      video::configure_bg(static_cast<video::BgIndex>(b), false, 0, 0, 0, false, 0);
   }
   for (uint8_t i = 0; i < plan.count; ++i)
   {
      const LayerTemplate& t = plan.layers[i];
      Layer& l = at(t.bg);
      l.t = t;
      l.used = true;
      l.affine = affine_in(t.bg, plan.mode);
      video::configure_bg(t.bg, false, t.priority, t.char_block, t.screen_block, t.bpp8 != 0, t.screen_size);
      if (l.affine)
      {
         video::set_bg_affine(t.bg, kQ8Identity, 0, 0, kQ8Identity, 0, 0);
      }
      else
      {
         video::set_bg_scroll(t.bg, 0, 0);
      }
   }
}

void init(const LayerTemplate& t)
{
   hal::vram::set_tile_guard(tiles_clear_of_maps);
   Layer& l = at(t.bg);
   l = Layer{};
   l.t = t;
   l.used = true;
   l.affine = affine_in(t.bg, g_mode);
   video::configure_bg(t.bg, false, t.priority, t.char_block, t.screen_block, t.bpp8 != 0, t.screen_size);
   if (l.affine)
   {
      video::set_bg_affine(t.bg, kQ8Identity, 0, 0, kQ8Identity, 0, 0);
   }
   else
   {
      video::set_bg_scroll(t.bg, 0, 0);
   }
}

void set_mode(uint8_t mode)
{
   g_mode = mode;
   video::set_mode(mode);
   for (int b = 0; b < kLayers; ++b)
   {
      Layer& l = g_layers[b];
      const auto bg = static_cast<video::BgIndex>(b);
      const bool affine = affine_in(bg, mode);
      if (!l.used || affine == l.affine)
      {
         continue;
      }
      l.affine = affine;   // a layer the mode turns affine (or back): reconfigured, hidden, at its origin
      video::configure_bg(bg, false, l.t.priority, l.t.char_block, l.t.screen_block, l.t.bpp8 != 0, l.t.screen_size);
      if (affine)
      {
         video::set_bg_affine(bg, kQ8Identity, 0, 0, kQ8Identity, 0, 0);
      }
      else
      {
         video::set_bg_scroll(bg, 0, 0);
      }
   }
}

void show(video::BgIndex bg, bool on)
{
   const Layer& l = at(bg);
   if (!l.used)
   {
      return;
   }
   video::configure_bg(bg, on, l.t.priority, l.t.char_block, l.t.screen_block, l.t.bpp8 != 0, l.t.screen_size);
}

void set_scroll(video::BgIndex bg, int16_t x, int16_t y)
{
   const Layer& l = at(bg);
   if (l.affine)
   {
      video::set_bg_affine(bg, kQ8Identity, 0, 0, kQ8Identity, x, y);
   }
   else
   {
      video::set_bg_scroll(bg, x, y);
   }
}

const LayerTemplate* layer(video::BgIndex bg)
{
   const Layer& l = at(bg);
   return l.used ? &l.t : nullptr;
}

void set_buffer(video::BgIndex bg, uint16_t* buffer)
{
   at(bg).buffer = buffer;
}

uint16_t* buffer(video::BgIndex bg)
{
   return at(bg).buffer;
}

int width_cells(video::BgIndex bg)
{
   const Layer& l = at(bg);
   if (l.affine)
   {
      return kAffineBase << l.t.screen_size;
   }
   return (l.t.screen_size & kWide) != 0 ? kRowCells * kBlocksAcross : kRowCells;
}

int height_cells(video::BgIndex bg)
{
   const Layer& l = at(bg);
   if (l.affine)
   {
      return kAffineBase << l.t.screen_size;
   }
   return (l.t.screen_size & kTall) != 0 ? kRowCells * kBlocksAcross : kRowCells;
}

uint32_t map_bytes(video::BgIndex bg)
{
   const auto cells = static_cast<uint32_t>(width_cells(bg) * height_cells(bg));
   return at(bg).affine ? cells : cells * video::kHalfwordBytes;
}

int cell_index(video::BgIndex bg, int x, int y)
{
   const Layer& l = at(bg);
   const int w = width_cells(bg);
   const int h = height_cells(bg);
   x &= w - 1;
   y &= h - 1;
   if (l.affine)
   {
      return y * w + x;
   }
   // a wide map's right half is the next screen block; a 64 x 64 map's lower half the two blocks after
   if ((l.t.screen_size & kTall) != 0 && (l.t.screen_size & kWide) != 0 && y >= kRowCells)
   {
      y += kRowCells;
   }
   if ((l.t.screen_size & kWide) != 0 && x >= kRowCells)
   {
      x -= kRowCells;
      y += kRowCells;
   }
   return y * kRowCells + x;
}

namespace
{

/// C13a: whether [offset, offset + bytes) overlaps [first, first + count).
/// @param offset  the range's start
/// @param bytes   its length
/// @param first   the other's start
/// @param count   its length
/// @return true when they share a byte
bool overlaps(uint32_t offset, uint32_t bytes, uint32_t first, uint32_t count)
{
   return offset < first + count && first < offset + bytes;
}

}   // namespace

bool tiles_clear_of_maps(uint32_t offset, uint32_t bytes)
{
   for (int b = 0; b < kLayers; ++b)
   {
      const Layer& l = g_layers[b];
      if (l.used && overlaps(offset, bytes, map_offset(l), map_bytes(static_cast<video::BgIndex>(b))))
      {
         return false;   // a screen layer's map
      }
   }
   const map::Map* field = map::current();
   if (field == nullptr || map::suspended())
   {
      return true;
   }
   for (int i = 0; i < field->layer_count; ++i)
   {
      const uint32_t ring = static_cast<uint32_t>(map::screen_block_of(field->layers[i].bg)) * video::kScreenBlockBytes;
      if (overlaps(offset, bytes, ring, video::kScreenBlockBytes))
      {
         return false;   // the shown field's ring (one screen block per tile layer)
      }
   }
   return true;
}

void load_tiles(video::BgIndex bg, const asset::Tiles& tiles, uint16_t offset)
{
   const Layer& l = at(bg);
   ENGINE_ASSERT(l.used && (tiles.bpp == asset::kTiles8bpp) == (l.t.bpp8 != 0));
   const uint32_t words = static_cast<uint32_t>(tiles.count) *
                          (tiles.bpp == asset::kTiles8bpp ? asset::kTileWords8bpp : asset::kTileWords4bpp);
   ENGINE_ASSERT(tile_offset(l, offset) + words * video::kWordBytes <= video::kBgVramBytes);
   ENGINE_ASSERT(hal::vram::tiles_allowed(tile_offset(l, offset), words * video::kWordBytes));   // C13a (R9 M4)
   video::transfer32(video::Region::BgVram, tile_offset(l, offset), tiles.words, words);
}

void queue_tiles(video::BgIndex bg, const uint32_t* words, uint16_t count, uint16_t offset)
{
   const Layer& l = at(bg);
   ENGINE_ASSERT(l.used);
   const uint32_t total = static_cast<uint32_t>(count) * (l.t.bpp8 ? asset::kTileWords8bpp : asset::kTileWords4bpp);
   const uint32_t base = tile_offset(l, offset);
   ENGINE_ASSERT(hal::vram::tiles_allowed(base, total * video::kWordBytes));   // C13a (R9 M4)
   for (uint32_t done = 0; done < total; done += kPieceWords)
   {
      const uint32_t n = total - done < kPieceWords ? total - done : kPieceWords;
      hal::vram::copy32(video::Region::BgVram, base + done * video::kWordBytes, words + done, n,
                        hal::vram::Priority::MustLand, video::Source::Rom);
   }
}

void set_tilemap(video::BgIndex bg, const asset::Tilemap& map, uint16_t offset)
{
   Layer& l = at(bg);
   ENGINE_ASSERT(l.buffer != nullptr);
   const auto limit = static_cast<int>(l.affine ? map_bytes(bg) : map_bytes(bg) / video::kHalfwordBytes);
   if (l.affine)
   {
      ENGINE_ASSERT(map.affine_cells != nullptr);
      auto* bytes = reinterpret_cast<uint8_t*>(l.buffer);
      for (int i = 0; i < map.count && offset + i < limit; ++i)
      {
         bytes[offset + i] = map.affine_cells[i];
      }
      return;
   }
   ENGINE_ASSERT(map.cells != nullptr);
   for (int i = 0; i < map.count && offset + i < limit; ++i)
   {
      l.buffer[offset + i] = map.cells[i];
   }
}

void put(video::BgIndex bg, int x, int y, video::MapCell cell)
{
   Layer& l = at(bg);
   ENGINE_ASSERT(l.buffer != nullptr);
   const int i = cell_index(bg, x, y);
   if (l.affine)
   {
      reinterpret_cast<uint8_t*>(l.buffer)[i] = static_cast<uint8_t>(cell);
   }
   else
   {
      l.buffer[i] = cell;
   }
}

video::MapCell get(video::BgIndex bg, int x, int y)
{
   const Layer& l = at(bg);
   ENGINE_ASSERT(l.buffer != nullptr);
   const int i = cell_index(bg, x, y);
   return l.affine ? reinterpret_cast<const uint8_t*>(l.buffer)[i] : l.buffer[i];
}

void fill(video::BgIndex bg, video::MapCell first, int x, int y, int w, int h, uint8_t palette, int16_t delta)
{
   uint16_t entry = first;
   for (int j = y; j < y + h; ++j)
   {
      for (int i = x; i < x + w; ++i)
      {
         if (at(bg).affine)
         {
            put(bg, i, j, static_cast<uint8_t>(entry));
         }
         else
         {
            put(bg, i, j, rewrite(entry, 0, palette, 0, 0));
         }
         // the tile advances in its 10 bits; the flips and bank bits of the first entry stay
         entry = static_cast<uint16_t>((entry & kAttrField) + ((entry + delta) & kTileField));
      }
   }
}

void fill_linear(video::BgIndex bg, video::MapCell entry, int x, int y, int w, int h)
{
   Layer& l = at(bg);
   ENGINE_ASSERT(l.buffer != nullptr);
   const int stride = l.affine ? width_cells(bg) : kRowCells;
   for (int j = y; j < y + h; ++j)
   {
      for (int i = x; i < x + w; ++i)
      {
         if (l.affine)
         {
            reinterpret_cast<uint8_t*>(l.buffer)[j * stride + i] = static_cast<uint8_t>(entry);
         }
         else
         {
            l.buffer[j * stride + i] = entry;
         }
      }
   }
}

void copy_linear(video::BgIndex bg, const void* src, int x, int y, int w, int h)
{
   Layer& l = at(bg);
   ENGINE_ASSERT(l.buffer != nullptr);
   const int stride = l.affine ? width_cells(bg) : kRowCells;
   const auto* cells = static_cast<const uint16_t*>(src);
   const auto* bytes = static_cast<const uint8_t*>(src);
   for (int j = 0; j < h; ++j)
   {
      for (int i = 0; i < w; ++i)
      {
         const int to = (y + j) * stride + x + i;
         if (l.affine)
         {
            reinterpret_cast<uint8_t*>(l.buffer)[to] = bytes[j * w + i];
         }
         else
         {
            l.buffer[to] = cells[j * w + i];
         }
      }
   }
}

void copy_rect(video::BgIndex bg, const void* src, int src_x, int src_y, int src_w, int x, int y, int w, int h,
               uint8_t palette, int16_t tile_offset, int16_t palette_add)
{
   Layer& l = at(bg);
   ENGINE_ASSERT(l.buffer != nullptr);
   const auto* cells = static_cast<const uint16_t*>(src);
   const auto* bytes = static_cast<const uint8_t*>(src);
   for (int j = 0; j < h; ++j)
   {
      for (int i = 0; i < w; ++i)
      {
         const int from = (src_y + j) * src_w + src_x + i;
         if (l.affine)
         {
            const int stride = width_cells(bg);
            reinterpret_cast<uint8_t*>(l.buffer)[(y + j) * stride + x + i] =
               static_cast<uint8_t>(bytes[from] + tile_offset);
         }
         else
         {
            const int to = cell_index(bg, x + i, y + j);
            l.buffer[to] = rewrite(cells[from], l.buffer[to], palette, tile_offset, palette_add);
         }
      }
   }
}

void copy_to_vram(video::BgIndex bg)
{
   Layer& l = at(bg);
   ENGINE_ASSERT(l.used && l.buffer != nullptr);
   const uint32_t halfwords = map_bytes(bg) / video::kHalfwordBytes;
   for (uint32_t done = 0; done < halfwords; done += kPieceHalfwords)
   {
      const uint32_t n = halfwords - done < kPieceHalfwords ? halfwords - done : kPieceHalfwords;
      hal::vram::copy16(video::Region::BgVram, map_offset(l) + done * video::kHalfwordBytes, l.buffer + done, n,
                        hal::vram::Priority::MustLand, video::Source::Ewram);
   }
   l.scheduled = false;
}

void schedule_copy(video::BgIndex bg)
{
   at(bg).scheduled = true;
}

void copy_scheduled()
{
   for (int b = 0; b < kLayers; ++b)
   {
      if (g_layers[b].scheduled && g_layers[b].used && g_layers[b].buffer != nullptr)
      {
         copy_to_vram(static_cast<video::BgIndex>(b));
      }
   }
}

void put_window(video::BgIndex bg, int x, int y, int w, int h, uint16_t first_tile, uint8_t palette)
{
   uint16_t tile = first_tile;
   for (int j = 0; j < h; ++j)
   {
      for (int i = 0; i < w; ++i, ++tile)
      {
         put(bg, x + i, y + j, video::map_cell(tile, palette));
      }
   }
}

}   // namespace capsule::screen::layers
