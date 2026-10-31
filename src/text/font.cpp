/// @file
/// Font loading: 2bpp glyphs expanded to 4bpp tiles through a ROM table (C-1).
#include "capsule/text/font.h"

#include "capsule/core/types.h"

#include <array>

namespace capsule::text
{

namespace
{

constexpr int kByteValues = 256;     ///< Values of one byte (entries of the expansion table).

/// Builds the 2bpp-to-4bpp expansion table at compile time.
/// @return expand_2bpp() of every byte value
constexpr std::array<uint16_t, kByteValues> make_lut()
{
   std::array<uint16_t, kByteValues> lut{};
   for (int b = 0; b < kByteValues; ++b)
   {
      lut[b] = expand_2bpp(static_cast<uint8_t>(b));
   }
   return lut;
}

/// 2bpp byte -> 4bpp halfword. C-1: 256 x 2 B = 512 B in ROM.
constexpr std::array<uint16_t, kByteValues> kExpandLut = make_lut();
static_assert(sizeof(kExpandLut) == 512);

} // namespace

void load_font(const FontInfo& font, const uint8_t* glyphs, uint8_t char_block, hal::video::TileIndex first_tile)
{
   uint32_t tile[kTilePixels];   // one 4bpp tile: 8 rows of 8 pixels
   for (int g = 0; g < font.glyph_count; ++g)
   {
      const uint8_t* src = glyphs + g * kGlyphBytes2bpp;
      for (int row = 0; row < kTilePixels; ++row)
      {
         // Each 2bpp byte expands to 4 pixels (a halfword); the row's left half is the low halfword.
         tile[row] = kExpandLut[src[row * kRowBytes2bpp]] |
                     (uint32_t{kExpandLut[src[row * kRowBytes2bpp + 1]]} << kRightHalfShift4bpp);
      }
      hal::video::load_bg_tiles(char_block, static_cast<hal::video::TileIndex>(first_tile + g), tile, 1);
   }
}

} // namespace capsule::text
