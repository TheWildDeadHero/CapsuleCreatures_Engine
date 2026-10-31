/// @file
/// Background loader (see capsule/gfx/background.h).
#include "capsule/gfx/background.h"

#include "capsule/core/assert.h"
#include "capsule/gfx/palette.h"

namespace capsule::gfx::background
{
namespace
{

namespace video = hal::video;

/// @name Display modes (set_display_mode())
///@{
constexpr uint8_t kModeOneAffine = 1;   ///< BG0-1 regular, BG2 affine.
constexpr uint8_t kModeTwoAffine = 2;   ///< BG0-1 off, BG2-3 affine; also the highest mode used.
///@}

constexpr video::BgIndex kFirstAffineBg = 2;   ///< Lowest layer that can be affine.
constexpr uint8_t kAllBgsMask = 15;            ///< One bit per BG.
constexpr uint8_t kPriorityMask = 3;           ///< Bits of a hardware draw priority (0 front to 3).

/// @name Affine images
///@{
constexpr int kAffineSmallCells = 16;           ///< Cells per side of a small (128x128) affine map.
constexpr int kAffineLargeCells = 32;           ///< Cells per side of a large (256x256) affine map.
constexpr int kAffineSmallSize = 0;         ///< configure_bg() screen size of a small affine map.
constexpr int kAffineLargeSize = 1;         ///< configure_bg() screen size of a large affine map.
constexpr int kMaxBg2AffineTiles = 256;         ///< 8bpp tiles BG2 may load (its character block is its own).
constexpr int kTileWords8bpp = video::kTileBytes8bpp / kWordBytes;   ///< 32-bit words in one 8bpp tile.
constexpr int kBits8bpp = 8;   ///< Bits of one 8bpp pixel.
constexpr uint32_t kPixelMask8bpp = 0xFF;       ///< One 8bpp pixel (one byte of a tile word).
///@}

/// @name Regular map entries
///@{
constexpr int kMapCellBankShift = 12;                ///< Shift of the palette bank field.
constexpr video::MapCell kMapCellTileAndFlips = 0x0FFF;   ///< Tile index and flip bits (bank cleared).
///@}

/// What is on one layer.
struct Layer
{
   const asset::Background* image;   ///< Loaded image, or nullptr.
   video::MapCell blank;             ///< Fill of a layer prepared for text.
   uint8_t set_up;                   ///< 1 once loaded or prepared for text.
   uint8_t affine;                   ///< 1 for an affine image.
   uint8_t shown;                    ///< 1 while shown.
   uint8_t size;                     ///< configure_bg() screen size.
   uint8_t version;                  ///< See block_version().
   uint8_t priority;                 ///< Layer priority, 0-3.
};
Layer g_layers[video::kNumBgs];   ///< Per-layer state.
uint8_t g_mode = 0;               ///< Display mode set with set_display_mode().

/// The map plan (see begin_map_plan()).
struct Plan
{
   uint8_t on;         ///< 1 while a map is shown.
   uint8_t map_bgs;    ///< BGs with a map tile layer.
   uint8_t banks;      ///< BG palette banks the map uses.
   uint16_t version;   ///< plan_version().
};
Plan g_plan{};   ///< The map plan.

/// Bumps every layer's block version (their character blocks changed hands).
void bump_versions()
{
   for (Layer& l : g_layers)
   {
      l.version = static_cast<uint8_t>(l.version + 1);
   }
}

/// Whether this module may set a layer up now.
/// @param bg  layer
/// @return false for any layer but BG0 (when free) under the map plan
bool may_set_up(video::BgIndex bg)
{
   return !g_plan.on || (bg == 0 && !is_reserved(0));
}

/// Applies a layer's configuration to the HAL.
/// @param bg  layer
void configure(video::BgIndex bg)
{
   ENGINE_ASSERT(!is_reserved(bg));
   const Layer& l = g_layers[bg];
   video::configure_bg(bg, l.shown != 0, l.priority, char_block(bg), screen_block(bg), l.affine != 0, l.size);
}

} // namespace

void begin_map_plan(uint8_t map_bgs, uint8_t palette_banks)
{
   ENGINE_ASSERT(palette_banks <= palette::kBanks);
   g_plan.on = 1;
   g_plan.map_bgs = static_cast<uint8_t>(map_bgs & kAllBgsMask);
   g_plan.banks = palette_banks;
   g_plan.version = static_cast<uint16_t>(g_plan.version + 1);
   bump_versions();
   for (video::BgIndex bg = 0; bg < video::kNumBgs; ++bg)
   {
      if (is_reserved(bg))
      {
         continue;
      }
      // Whatever was set up here used blocks the map now owns.
      g_layers[bg].image = nullptr;
      g_layers[bg].set_up = 0;
      g_layers[bg].affine = 0;
      g_layers[bg].shown = 0;
      configure(bg);
   }
}

void end_map_plan()
{
   if (!g_plan.on)
   {
      return;
   }
   const uint8_t map_bgs = g_plan.map_bgs;
   g_plan.on = 0;
   g_plan.map_bgs = 0;
   g_plan.banks = 0;
   g_plan.version = static_cast<uint16_t>(g_plan.version + 1);
   bump_versions();
   for (video::BgIndex bg = 0; bg < video::kNumBgs; ++bg)
   {
      Layer& l = g_layers[bg];
      if ((map_bgs & (1 << bg)) != 0 || bg == 0)
      {   // the map's layers, and BG0's moved blocks
         l.image = nullptr;
         l.set_up = 0;
         l.affine = 0;
         l.shown = 0;
      }
      configure(bg);
   }
}

bool map_plan()
{
   return g_plan.on != 0;
}

bool is_reserved(video::BgIndex bg)
{
   return g_plan.on && bg < video::kNumBgs && (g_plan.map_bgs & (1 << bg)) != 0;
}

uint8_t first_ui_bank()
{
   return g_plan.on ? g_plan.banks : 0;
}

uint16_t plan_version()
{
   return g_plan.version;
}

uint8_t char_block(video::BgIndex bg)
{
   return g_plan.on && bg == 0 && !is_reserved(0) ? kMapTextCharBlock : bg;
}

uint8_t screen_block(video::BgIndex bg)
{
   return g_plan.on && bg == 0 && !is_reserved(0) ? kMapTextScreenBlock : static_cast<uint8_t>(kMapScreenBlock + bg);
}

void reset()
{
   set_display_mode(0);
   for (video::BgIndex bg = 0; bg < video::kNumBgs; ++bg)
   {
      if (is_reserved(bg))
      {
         continue;
      }   // the map's
      const uint8_t version = g_layers[bg].version;
      g_layers[bg] = Layer{};
      g_layers[bg].version = static_cast<uint8_t>(version + 1);
      video::configure_bg(bg, false, 0, char_block(bg), screen_block(bg), false, 0);
      video::set_bg_scroll(bg, 0, 0);
   }
   for (video::BgIndex bg = kFirstAffineBg; bg < video::kNumBgs; ++bg)
   {
      if (is_reserved(bg))
      {
         continue;
      }
      video::set_bg_affine(bg, kQ8One, 0, 0, kQ8One, 0, 0);   // identity matrix
      video::set_bg_wrap(bg, false);
   }
}

void set_display_mode(uint8_t mode)
{
   ENGINE_ASSERT(mode <= kModeTwoAffine);
   g_mode = mode;
   video::set_mode(mode);
}

bool layer_is_affine(video::BgIndex bg)
{
   return (g_mode == kModeOneAffine && bg == kFirstAffineBg) || (g_mode == kModeTwoAffine && bg >= kFirstAffineBg);
}

bool load(video::BgIndex bg, const asset::Background& asset_image)
{
   ENGINE_ASSERT(bg < video::kNumBgs && !g_plan.on);   // its palette bank and blocks would be the map's
   const bool want_affine = layer_is_affine(bg);
   const asset::Background* pick = &asset_image;
   if ((pick->affine != 0) != want_affine)
   {
      pick = pick->other;
   }
   if (pick == nullptr || (pick->affine != 0) != want_affine)
   {
      return false;
   }
   const asset::Background& image = *pick;
   Layer& l = g_layers[bg];
   l.image = &image;
   l.set_up = 1;
   l.affine = image.affine;
   l.priority = image.priority & kPriorityMask;
   // Layer n uses BG bank n; color 0 is the backdrop and never loaded.
   palette::load_bg(static_cast<uint8_t>(bg * palette::kBankColors + 1), image.palette + 1, palette::kBankColors - 1);
   if (image.affine)
   {
      ENGINE_ASSERT(bg >= kFirstAffineBg && image.width == image.height &&
                    (image.width == kAffineSmallCells || image.width == kAffineLargeCells));
      ENGINE_ASSERT(image.tile_count <= (bg == kFirstAffineBg ? kMaxBg2AffineTiles : kMaxAffineTiles));
      l.size = image.width == kAffineSmallCells ? kAffineSmallSize : kAffineLargeSize;
      ++l.version;   // 8bpp tiles fill the block, text tiles included
      configure(bg);
      // Pixel values 1-15 move into this layer's palette bank.
      const uint32_t shift = static_cast<uint32_t>(bg) * palette::kBankColors;
      uint32_t tile[kTileWords8bpp];
      for (uint16_t t = 0; t < image.tile_count; ++t)
      {
         for (int w = 0; w < kTileWords8bpp; ++w)
         {
            const uint32_t v = image.tiles[t * kTileWords8bpp + w];
            uint32_t out = 0;
            // One pixel per byte; 0 stays transparent.
            for (int b = 0; b < kWordBytes; ++b)
            {
               const uint32_t px = (v >> (kBits8bpp * b)) & kPixelMask8bpp;
               out |= (px == 0 ? 0 : px + shift) << (kBits8bpp * b);
            }
            tile[w] = out;
         }
         video::load_bg_tiles_8bpp(char_block(bg), t, tile, 1);
      }
      for (int y = 0; y < image.height; ++y)
      {
         for (int x = 0; x < image.width; ++x)
         {
            video::write_affine_cell(bg, x, y, image.affine_map[y * image.width + x]);
         }
      }
      return true;
   }
   ENGINE_ASSERT(image.width == kBgMapCells && image.height == kBgMapCells && image.tile_count <= kMaxTiles);
   l.size = 0;
   configure(bg);
   video::load_bg_tiles(char_block(bg), 0, image.tiles, image.tile_count);
   restore(bg, 0, 0, kBgMapCells, kBgMapCells);
   return true;
}

void prepare_text(video::BgIndex bg, video::MapCell blank)
{
   ENGINE_ASSERT(bg < video::kNumBgs && may_set_up(bg));
   Layer& l = g_layers[bg];
   l.blank = blank;
   if (l.set_up && !l.affine)
   {
      return;
   }
   l.image = nullptr;
   l.set_up = 1;
   l.affine = 0;
   l.size = 0;
   l.priority = 0;
   configure(bg);
   restore(bg, 0, 0, kBgMapCells, kBgMapCells);
}

void restore(video::BgIndex bg, int x, int y, int w, int h)
{
   ENGINE_ASSERT(bg < video::kNumBgs && !g_layers[bg].affine && !is_reserved(bg));
   const Layer& l = g_layers[bg];
   const auto bank = static_cast<video::MapCell>(bg << kMapCellBankShift);   // layer n's palette bank is n
   for (int row = y; row < y + h; ++row)
   {
      for (int col = x; col < x + w; ++col)
      {
         if (row < 0 || row >= kBgMapCells || col < 0 || col >= kBgMapCells)
         {
            continue;
         }
         const video::MapCell cell =
            l.image != nullptr
               ? static_cast<video::MapCell>((l.image->map[row * kBgMapCells + col] & kMapCellTileAndFlips) | bank)
               : l.blank;
         video::write_map_cell(bg, col, row, cell);
      }
   }
}

void show(video::BgIndex bg, bool on)
{
   ENGINE_ASSERT(bg < video::kNumBgs && !is_reserved(bg));
   g_layers[bg].shown = on ? 1 : 0;
   configure(bg);
}

void set_position(video::BgIndex bg, int16_t x, int16_t y)
{
   ENGINE_ASSERT(bg < video::kNumBgs && !is_reserved(bg));
   if (g_layers[bg].affine)
   {
      video::set_bg_affine(bg, kQ8One, 0, 0, kQ8One, int32_t{x} * kQ8One, int32_t{y} * kQ8One);   // identity, Q8 offset
   }
   else
   {
      video::set_bg_scroll(bg, x, y);
   }
}

bool is_affine(video::BgIndex bg)
{
   return bg < video::kNumBgs && g_layers[bg].affine != 0;
}

uint8_t block_version(video::BgIndex bg)
{
   return bg < video::kNumBgs ? g_layers[bg].version : 0;
}

bool is_set_up(video::BgIndex bg)
{
   return bg < video::kNumBgs && g_layers[bg].set_up != 0;
}

}   // namespace capsule::gfx::background
