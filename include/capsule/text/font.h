/// @file
/// Fonts (C-1).
///
/// Glyphs are stored 2bpp in ROM (0 = empty, 1 = foreground, 2 = shadow) and
/// expanded to 4bpp tiles when loaded, through a 256-entry LUT. Fonts are
/// assets compiled by tools/font_compiler.py (P-1); the engine ships
/// kDefaultFont as the fallback.
#pragma once

#include "capsule/core/pod.h"
#include "capsule/hal/video.h"

#include <cstdint>

namespace capsule::text
{

/// Shape of a compiled font.
struct FontInfo
{
   uint8_t first_code;    ///< Code-page value of glyph 0.
   uint8_t glyph_count;   ///< Number of glyphs.
   uint8_t cell_width;    ///< Cell width in pixels; 8 for fixed-cell fonts.
   uint8_t cell_height;   ///< Cell height in pixels.
};
CAPSULE_POD(FontInfo, 4);

inline constexpr int kGlyphBytes2bpp = 16;   ///< One 8x8 glyph, 2 bits per pixel.
inline constexpr int kPixelsPerByte2bpp = 4; ///< Pixels in one 2bpp byte.
inline constexpr int kBits2bpp = 2;          ///< Bits of one 2bpp pixel.
inline constexpr unsigned kPixelMask2bpp = 3u;   ///< One 2bpp pixel's bits.
inline constexpr int kBits4bpp = 4;          ///< Bits of one 4bpp pixel.
inline constexpr int kRowBytes2bpp = 2;      ///< Bytes of one 8-pixel glyph row at 2bpp.
inline constexpr int kRightHalfShift4bpp = 16;   ///< Shift of a 4bpp row's right 4 pixels (its second halfword).

/// Expands one 2bpp byte (4 pixels, bits 1:0 = leftmost) to 16 bits of 4bpp
/// (4 pixels, bits 3:0 = leftmost), keeping each pixel's value as its palette
/// index.
/// @param b  four 2bpp pixels
/// @return the same four pixels at 4bpp
constexpr uint16_t expand_2bpp(uint8_t b)
{
   uint16_t out = 0;
   for (int i = 0; i < kPixelsPerByte2bpp; ++i)
   {
      // Pixel i moves from bits 2i..2i+1 to bits 4i..4i+3.
      out = static_cast<uint16_t>(out | (((b >> (kBits2bpp * i)) & kPixelMask2bpp) << (kBits4bpp * i)));
   }
   return out;
}

/// I4: a proportional font (asset kind Font; tools/font_compiler.py compile_proportional(), capsule.font/2): one
/// cell per code, drawn pixel by pixel (capsule/text/window.h) at the glyph's own advance width.
struct PropFont
{
   FontInfo info;            ///< First code, codes, cell width (a multiple of 4) and height.
   /// Per code, cell_height rows of cell_width / 4 bytes, 2bpp (bits 1:0 = leftmost): 0 empty, 1 ink, 2 shadow, 3
   /// fill (C13a: a window draws 0 and 3 alike, see capsule/text/window.h).
   const uint8_t* glyphs;
   const uint8_t* widths;    ///< Per code, its advance in pixels (0: a code the font leaves out).
   uint8_t line_height;      ///< Pixels from one line's top to the next's.
   uint8_t letter_spacing;   ///< Pixels added after each glyph.
   /// C13a: rows of a glyph a window draws (the handheld games' glyph height; 0: all cell_height rows).
   uint8_t glyph_height;
   uint8_t reserved;         ///< 0.
};

/// Bytes of one glyph of a proportional font.
/// @param f  the font
/// @return cell_width * cell_height / 4
constexpr int prop_glyph_bytes(const FontInfo& f)
{
   return f.cell_width * f.cell_height / kPixelsPerByte2bpp;
}

extern const FontInfo kDefaultFont;          ///< The built-in 8x8 font (generated).
extern const uint8_t kDefaultFontGlyphs[];  ///< Its glyphs, 2bpp (generated).

/// Expands every glyph to a 4bpp tile and loads them as consecutive tiles.
/// Glyph pixels use palette entries 1 (foreground) and 2 (shadow) of whichever
/// palette bank the map cells select.
/// @param font        the font's shape
/// @param glyphs      its 2bpp glyph data
/// @param char_block  character block to load into, 0-3
/// @param first_tile  tile that receives glyph 0
void load_font(const FontInfo& font, const uint8_t* glyphs, uint8_t char_block, hal::video::TileIndex first_tile);

} // namespace capsule::text
