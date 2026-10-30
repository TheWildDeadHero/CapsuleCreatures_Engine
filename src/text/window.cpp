/// @file
/// Proportional text windows (see capsule/text/window.h).
#include "capsule/text/window.h"

#include "capsule/core/assert.h"
#include "capsule/core/memory.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/text/placeholder.h"

namespace capsule::text::window
{
namespace
{

namespace video = hal::video;

/// @name Tile layout (4bpp)
///@{
constexpr int kTileSide = 8;         ///< Pixels per tile side.
constexpr int kByteBits = 8;         ///< E1: the high byte of a control's 16-bit argument.
constexpr int kTileShift = 3;        ///< Pixel to tile: p >> 3.
constexpr int kPixelMask = 7;        ///< Pixel within its tile.
constexpr int kNibbleBits = 4;       ///< Bits of one 4bpp pixel.
constexpr uint32_t kNibbleMask = 15; ///< One 4bpp pixel.
///@}

/// @name Glyph pixels (2bpp)
///@{
constexpr int kPixelsPerByte = 4;    ///< Pixels of one 2bpp byte.
constexpr int kBitsPerPixel = 2;     ///< Bits of one 2bpp pixel.
constexpr unsigned kPixelBits = 3u;  ///< One 2bpp pixel.
constexpr unsigned kInkPixel = 1u;   ///< Ink.
constexpr unsigned kShadowPixel = 2u;   ///< Shadow.
///@}

/// @name C13a: word-at-a-time pixel work
///@{
constexpr int kPixelsPerWord = 8;            ///< 4bpp pixels of one 32-bit tile row.
constexpr int kWordBits = 32;                ///< Bits of a tile row.
constexpr uint32_t kAllPixels = 0xFFFFFFFFu; ///< Every pixel of a tile row.
constexpr int kLutEntries = 256;             ///< 2bpp glyph bytes (four pixels each).
constexpr int kLutPixelBits = 16;            ///< 4bpp bits of one glyph byte's four pixels.
constexpr int kGlyphBytesPerWord = 2;        ///< Glyph bytes of one 8-pixel group.
///@}

constexpr uint8_t kArrowBlinkFrames = 16;   ///< The arrow shows and hides this many frames each.
constexpr int kBlinkPhases = 2;              ///< Shown, hidden.
constexpr int kColorsBackgroundAt = kEscapeHead + 1;   ///< Control::Colors: offset of the background entry.
constexpr int kColorsShadowAt = kEscapeHead + 2;       ///< Control::Colors: offset of the shadow entry.
constexpr int kNoDirty = -1;                 ///< No tile changed.
/// C10: most codes kTypingCounter reads in one frame (a guard: a text of controls only still ends its frame).
constexpr int kMaxCodesPerFrame = 4096;

Window* g_cur = nullptr;   ///< The window the public call works on.

/// C13a: a glyph byte's four pixels at 4bpp in the window's colours and the nibbles they write (a half-row lookup
/// table, as the handheld games' printer keeps one, plus the mask of the entries that are not 0, which copying a
/// glyph into the window skips).
struct GlyphLut
{
   uint16_t value[kLutEntries];   ///< The pixels.
   uint16_t mask[kLutEntries];    ///< Their nibbles that are written.
   uint8_t ink;                   ///< The colours it was built for.
   uint8_t shadow;                ///< ...
   uint8_t background;            ///< ...
   bool built;                    ///< Built at least once.
};
CAPSULE_EWRAM_BSS GlyphLut g_lut;   ///< Built again when the colours change.

/// The window the public call works on.
/// @return it
Window& cur()
{
   return *g_cur;
}

/// Pixel width of the window.
/// @return pixels
int width_px()
{
   return cur().c.width * kTileSide;
}

/// Pixel height of the window.
/// @return pixels
int height_px()
{
   return cur().c.height * kTileSide;
}

/// Marks a tile as changed.
/// @param t  tile index in the window
void touch(int t)
{
   if (cur().dirty_lo == kNoDirty || t < cur().dirty_lo)
   {
      cur().dirty_lo = t;
   }
   if (t > cur().dirty_hi)
   {
      cur().dirty_hi = t;
   }
}

/// Sets one pixel (clipped to the window).
/// @param x      column
/// @param y      row
/// @param entry  palette entry
void put_pixel(int x, int y, uint8_t entry)
{
   if (x < 0 || y < 0 || x >= width_px() || y >= height_px())
   {
      return;
   }
   const int t = (y >> kTileShift) * cur().c.width + (x >> kTileShift);
   uint32_t& w = cur().store.words[t * kWordsPerTile + (y & kPixelMask)];
   const int shift = (x & kPixelMask) * kNibbleBits;
   w = (w & ~(kNibbleMask << shift)) | (static_cast<uint32_t>(entry & kNibbleMask) << shift);
   touch(t);
}

/// A word of eight pixels of one entry.
/// @param entry  palette entry
/// @return the word
uint32_t fill_word(uint8_t entry)
{
   uint32_t w = 0;
   for (int i = 0; i < kTileSide; ++i)
   {
      w |= static_cast<uint32_t>(entry & kNibbleMask) << (i * kNibbleBits);
   }
   return w;
}

/// Fills one pixel row of the window.
/// @param y      row
/// @param entry  palette entry
void fill_row(int y, uint8_t entry)
{
   const uint32_t w = fill_word(entry);
   const int ty = y >> kTileShift;
   for (int tx = 0; tx < cur().c.width; ++tx)
   {
      const int t = ty * cur().c.width + tx;
      cur().store.words[t * kWordsPerTile + (y & kPixelMask)] = w;
      touch(t);
   }
}

/// Copies one pixel row of the window over another.
/// @param dst  row written
/// @param src  row read
void copy_row(int dst, int src)
{
   for (int tx = 0; tx < cur().c.width; ++tx)
   {
      const int td = (dst >> kTileShift) * cur().c.width + tx;
      const int ts = (src >> kTileShift) * cur().c.width + tx;
      uint32_t* words = cur().store.words;
      words[td * kWordsPerTile + (dst & kPixelMask)] = words[ts * kWordsPerTile + (src & kPixelMask)];
      touch(td);
   }
}

/// C13a: a buffer of 4bpp tiles laid out as a window's (cells row-major, kWordsPerTile words each).
struct Canvas
{
   uint32_t* tiles;   ///< The tiles.
   int cells_w;       ///< Cells across.
   int cells_h;       ///< Cells down.
};

/// C13a: the palette entries a glyph's pixels take.
struct GlyphColors
{
   uint8_t ink;          ///< 1 pixels.
   uint8_t shadow;       ///< 2 pixels.
   uint8_t background;   ///< 0 and 3 pixels.
};

/// C13a: the tiles a drawing changed (the first and last in row-major order), or none.
struct Changed
{
   int first = kNoDirty;   ///< First tile.
   int last = kNoDirty;    ///< Last tile.
};

/// C13a: fills a rectangle of a canvas (clipped once, then a tile row's word at a time: a mask of the columns inside
/// it).
/// @param c      the canvas
/// @param x      left
/// @param y      top
/// @param w      width
/// @param h      height
/// @param entry  palette entry
/// @return the tiles changed
Changed fill_canvas(const Canvas& c, int x, int y, int w, int h, uint8_t entry)
{
   const int width = c.cells_w * kTileSide;
   const int height = c.cells_h * kTileSide;
   const int x0 = x < 0 ? 0 : x;
   const int y0 = y < 0 ? 0 : y;
   const int x1 = x + w > width ? width : x + w;     // exclusive
   const int y1 = y + h > height ? height : y + h;   // exclusive
   if (x0 >= x1 || y0 >= y1)
   {
      return Changed{};
   }
   const uint32_t value = fill_word(entry);
   const int first_tx = x0 >> kTileShift;
   const int last_tx = (x1 - 1) >> kTileShift;
   for (int tx = first_tx; tx <= last_tx; ++tx)
   {
      // the columns of this tile inside the rectangle: from lo to hi, inclusive
      const int lo = tx == first_tx ? (x0 & kPixelMask) : 0;
      const int hi = tx == last_tx ? ((x1 - 1) & kPixelMask) : kPixelMask;
      const uint32_t below_hi = hi == kPixelMask ? kAllPixels : ((1u << ((hi + 1) * kNibbleBits)) - 1u);
      const uint32_t mask = below_hi & ~((1u << (lo * kNibbleBits)) - 1u);
      for (int yy = y0; yy < y1; ++yy)
      {
         uint32_t& word = c.tiles[((yy >> kTileShift) * c.cells_w + tx) * kWordsPerTile + (yy & kPixelMask)];
         word = (word & ~mask) | (value & mask);
      }
   }
   // the changed tiles' range runs from the rectangle's first tile to its last (row-major)
   return Changed{(y0 >> kTileShift) * c.cells_w + first_tx, ((y1 - 1) >> kTileShift) * c.cells_w + last_tx};
}

/// The window the public call works on as a canvas.
/// @return it
Canvas canvas()
{
   return Canvas{cur().store.words, cur().c.width, cur().c.height};
}

/// Marks the tiles a drawing changed.
/// @param ch  them
void touch_changed(const Changed& ch)
{
   if (ch.first != kNoDirty)
   {
      touch(ch.first);
      touch(ch.last);
   }
}

/// Fills a rectangle of the window.
/// @param x      left
/// @param y      top
/// @param w      width
/// @param h      height
/// @param entry  palette entry
void fill_rect(int x, int y, int w, int h, uint8_t entry)
{
   touch_changed(fill_canvas(canvas(), x, y, w, h, entry));
}

/// A glyph's advance in a font.
/// @param font  the font
/// @param c     the code
/// @return pixels (0 for a code the font leaves out)
int advance_in(const PropFont& font, Code c)
{
   const FontInfo& f = font.info;
   if (c < f.first_code || c >= f.first_code + f.glyph_count)
   {
      return 0;
   }
   return font.widths[c - f.first_code] + font.letter_spacing;
}

/// A glyph's advance in the font glyphs come from (C10: Control::Font may have switched it).
/// @param c  the code
/// @return pixels (0 for a code the font leaves out)
int advance(Code c)
{
   return advance_in(*cur().glyphs, c);
}

/// C13a: a glyph's rows a window draws (the height of the glyph being drawn).
/// @param font  the font
/// @return PropFont::glyph_height, or the cell's height when it is 0 (or larger)
int glyph_rows(const PropFont& font)
{
   const int h = font.glyph_height;
   return h > 0 && h < font.info.cell_height ? h : font.info.cell_height;
}

/// C13a: a glyph's width (its widths[] entry, 0 for a code the font leaves out), without the letter spacing.
/// @param c  the code
/// @return pixels
int glyph_width(Code c)
{
   const PropFont& font = *cur().glyphs;
   const FontInfo& f = font.info;
   if (c < f.first_code || c >= f.first_code + f.glyph_count)
   {
      return 0;
   }
   return font.widths[c - f.first_code];
}

/// C13a: the entry a glyph pixel takes (1 ink, 2 shadow, 0 and 3 the background; the half-row table's mapping).
/// @param k  the colours
/// @param v  the 2bpp value
/// @return the entry (0: not written)
uint8_t glyph_entry(const GlyphColors& k, unsigned v)
{
   return v == kInkPixel ? k.ink : (v == kShadowPixel ? k.shadow : k.background);
}

/// C13a: the lookup table for a set of colours (built again only when they changed).
/// @param k  the colours
void ensure_lut(const GlyphColors& k)
{
   GlyphLut& l = g_lut;
   if (l.built && l.ink == k.ink && l.shadow == k.shadow && l.background == k.background)
   {
      return;
   }
   for (int b = 0; b < kLutEntries; ++b)
   {
      uint32_t value = 0;
      uint32_t mask = 0;
      for (int i = 0; i < kPixelsPerByte; ++i)
      {
         const uint8_t entry = glyph_entry(k, (static_cast<unsigned>(b) >> (i * kBitsPerPixel)) & kPixelBits);
         if (entry != 0)
         {
            value |= static_cast<uint32_t>(entry & kNibbleMask) << (i * kNibbleBits);
            mask |= kNibbleMask << (i * kNibbleBits);
         }
      }
      l.value[b] = static_cast<uint16_t>(value);
      l.mask[b] = static_cast<uint16_t>(mask);
   }
   l.ink = k.ink;
   l.shadow = k.shadow;
   l.background = k.background;
   l.built = true;
}

/// C13a: draws a glyph into a canvas as the handheld games copy one: its width's columns and its rows, clipped to
/// the canvas; entries 0 are not written.
/// @param cv    the canvas
/// @param font  the font
/// @param c     the code
/// @param x     left
/// @param y     top
/// @param k     the colours
/// @return the tiles changed
Changed glyph_canvas(const Canvas& cv, const PropFont& font, Code c, int x, int y, const GlyphColors& k)
{
   const FontInfo& f = font.info;
   if (c < f.first_code || c >= f.first_code + f.glyph_count)
   {
      return Changed{};
   }
   const uint8_t* rows = font.glyphs + (c - f.first_code) * prop_glyph_bytes(f);
   const int row_bytes = f.cell_width / kPixelsPerByte;
   const int width = font.widths[c - f.first_code];
   const int cols = width < f.cell_width ? width : f.cell_width;
   const int right = cv.cells_w * kTileSide - x;    // columns left before the right edge
   const int bottom = cv.cells_h * kTileSide - y;   // rows left before the bottom edge
   const int draw_cols = cols < right ? cols : right;
   const int draw_rows = glyph_rows(font) < bottom ? glyph_rows(font) : bottom;
   if (draw_cols <= 0 || draw_rows <= 0)
   {
      return Changed{};
   }
   Changed ch;
   if (x < 0 || y < 0)
   {
      // left of or above the canvas (a shift past its edge): pixel by pixel, clipped
      for (int r = 0; r < draw_rows; ++r)
      {
         for (int col = 0; col < draw_cols; ++col)
         {
            const unsigned v =
               (rows[r * row_bytes + col / kPixelsPerByte] >> ((col % kPixelsPerByte) * kBitsPerPixel)) & kPixelBits;
            const uint8_t entry = glyph_entry(k, v);
            const int px = x + col;
            const int py = y + r;
            if (entry == 0 || px < 0 || py < 0)
            {
               continue;
            }
            const int t = (py >> kTileShift) * cv.cells_w + (px >> kTileShift);
            uint32_t& word = cv.tiles[t * kWordsPerTile + (py & kPixelMask)];
            const int shift = (px & kPixelMask) * kNibbleBits;
            word = (word & ~(kNibbleMask << shift)) | (static_cast<uint32_t>(entry & kNibbleMask) << shift);
            ch.first = ch.first == kNoDirty || t < ch.first ? t : ch.first;
            ch.last = t > ch.last ? t : ch.last;
         }
      }
      return ch;
   }
   ensure_lut(k);
   const int shift = (x & kPixelMask) * kNibbleBits;   // the glyph's first column within its tile
   for (int r = 0; r < draw_rows; ++r)
   {
      const uint8_t* src = rows + r * row_bytes;
      const int yy = y + r;
      uint32_t* line = &cv.tiles[(yy >> kTileShift) * cv.cells_w * kWordsPerTile + (yy & kPixelMask)];
      // eight glyph pixels (two glyph bytes) at a time, written into the one or two tile rows they cover
      for (int g = 0; g * kPixelsPerWord < draw_cols; ++g)
      {
         const int b = g * kGlyphBytesPerWord;
         uint32_t value = g_lut.value[src[b]];
         uint32_t mask = g_lut.mask[src[b]];
         if (b + 1 < row_bytes)
         {
            value |= static_cast<uint32_t>(g_lut.value[src[b + 1]]) << kLutPixelBits;
            mask |= static_cast<uint32_t>(g_lut.mask[src[b + 1]]) << kLutPixelBits;
         }
         const int n = draw_cols - g * kPixelsPerWord;   // this group's columns inside the clip
         if (n < kPixelsPerWord)
         {
            mask &= (1u << (n * kNibbleBits)) - 1u;
         }
         if (mask == 0)
         {
            continue;
         }
         value &= mask;
         const int tx = (x >> kTileShift) + g;
         uint32_t& w0 = line[tx * kWordsPerTile];
         w0 = (w0 & ~(mask << shift)) | (value << shift);
         if (shift != 0)
         {
            const uint32_t spill = mask >> (kWordBits - shift);   // the group's pixels in the next tile
            if (spill != 0)
            {
               uint32_t& w1 = line[(tx + 1) * kWordsPerTile];
               w1 = (w1 & ~spill) | (value >> (kWordBits - shift));
            }
         }
      }
   }
   // the drawn box's first and last tiles bound the changed range (row-major)
   return Changed{(y >> kTileShift) * cv.cells_w + (x >> kTileShift),
                  ((y + draw_rows - 1) >> kTileShift) * cv.cells_w + ((x + draw_cols - 1) >> kTileShift)};
}

/// Draws a glyph at a pixel position of the window (glyph_canvas() in its colours).
/// @param c  the code
/// @param x  left
/// @param y  top
void draw_glyph(Code c, int x, int y)
{
   touch_changed(glyph_canvas(canvas(), *cur().glyphs, c, x, y,
                              GlyphColors{cur().ink, cur().shadow, cur().background}));
}

/// C13a: ClearTextSpan: a glyph's height of pixels at the pen in the background entry (nothing when it is 0).
/// @param width  pixels
void clear_span(int width)
{
   if (cur().background != 0)
   {
      fill_rect(cur().pen_x, cur().line_y, width, glyph_rows(*cur().glyphs), cur().background);
   }
}

/// C13a: the pen after a glyph: its advance, or with Control::MinSpacing at least that many pixels (the rest
/// cleared, as the handheld games' minimum letter spacing does).
/// @param c  the glyph's code
void pen_after_glyph(Code c)
{
   Window& w = cur();
   if (w.min_spacing == 0)
   {
      w.pen_x = static_cast<int16_t>(w.pen_x + advance(c));
      return;
   }
   const int width = glyph_width(c);
   w.pen_x = static_cast<int16_t>(w.pen_x + width);
   const int gap = w.min_spacing - width;
   if (gap > 0)
   {
      clear_span(gap);
      w.pen_x = static_cast<int16_t>(w.pen_x + gap);
   }
}

/// Clears every pixel and starts at the first line.
void clear_all()
{
   const uint32_t w = fill_word(cur().background);
   const int count = cur().c.width * cur().c.height;
   for (int i = 0; i < count * kWordsPerTile; ++i)
   {
      cur().store.words[i] = w;
   }
   if (count > 0)
   {
      touch(0);
      touch(count - 1);
   }
   cur().pen_x = cur().start_x;   // C13a: a page break: the pen back at the text's start
   cur().line_y = cur().start_y;
   cur().line = 0;
   cur().arrow_shown = false;
}

/// The arrow's position (the end of the last line's right edge).
/// @param x  left
/// @param y  top
void arrow_at(int& x, int& y)
{
   x = width_px() - advance_in(*cur().c.font, cur().c.arrow) - cur().c.left;
   y = cur().line_y;
}

/// Draws or erases the arrow.
/// @param on  drawn
void show_arrow(bool on)
{
   if (cur().c.arrow == kNoArrow || on == cur().arrow_shown)
   {
      return;
   }
   int x = 0;
   int y = 0;
   arrow_at(x, y);
   fill_rect(x, y, advance_in(*cur().c.font, cur().c.arrow), cur().c.font->info.cell_height, cur().background);
   if (on)
   {
      const PropFont* glyphs = cur().glyphs;
      cur().glyphs = cur().c.font;   // the arrow is the window font's
      draw_glyph(cur().c.arrow, x, y);
      cur().glyphs = glyphs;
   }
   cur().arrow_shown = on;
}

/// Starts a new line (scrolling when the window is full).
void new_line()
{
   cur().pen_x = cur().start_x;   // C13a: CHAR_NEWLINE: currentX = the printer's x
   if (cur().line + 1 < cur().c.lines)
   {
      ++cur().line;
      cur().line_y = static_cast<int16_t>(cur().line_y + cur().c.font->line_height);
      return;
   }
   cur().pause = Pause::Scrolling;   // full: up a line first
   cur().scrolled = 0;
}

/// Moves the window's pixels up by one frame's part of a scroll.
/// @return true when the line has scrolled all the way (nothing was moved then)
bool scroll_pixels()
{
   const int line = cur().c.font->line_height;
   if (cur().scrolled >= line)
   {
      return true;
   }
   const int step = cur().c.scroll_speed == 0 || cur().c.scroll_speed > line - cur().scrolled ? line - cur().scrolled
                                                                                    : cur().c.scroll_speed;
   const int h = height_px();
   for (int y = 0; y + step < h; ++y)
   {
      copy_row(y, y + step);
   }
   for (int y = h - step; y < h; ++y)
   {
      fill_row(y, cur().background);
   }
   cur().scrolled = static_cast<uint8_t>(cur().scrolled + step);
   return false;
}

/// One frame of a scroll (kTypingInterval: the text goes on in the frame its last row moves).
void scroll_step()
{
   scroll_pixels();
   if (cur().scrolled >= cur().c.font->line_height)
   {
      cur().pause = Pause::None;
      cur().pen_x = cur().start_x;
   }
}

/// Acts on a control escape.
/// @param p  the escape (FC)
/// @return C13a: true when it drew as a glyph does (Clear / ClearTo of a positive width count as drawn)
bool control(const Code* p)
{
   const auto k = static_cast<Control>(p[1]);
   const Code a = p[kEscapeHead];
   Window& w = cur();
   switch (k)
   {
   case Control::Color:
      w.ink = a;
      break;
   case Control::Highlight:
      w.background = a;
      break;
   case Control::Shadow:
      w.shadow = a;
      break;
   case Control::Colors:
      w.ink = a;
      w.background = p[kColorsBackgroundAt];
      w.shadow = p[kColorsShadowAt];
      break;
   case Control::Pause:
      if (!w.instant)
      {
         w.frames = a;
         w.pause = Pause::Frames;
         w.timer = 0;   // C10: kTypingCounter: the pause takes over the counter (it runs out with it)
      }
      break;
   case Control::WaitButton:
      w.pause = Pause::Button;
      break;
   case Control::Scroll:
      w.pause = Pause::Scroll;
      break;
   case Control::ShiftRight:
      w.pen_x = static_cast<int16_t>(w.start_x + a);
      break;
   case Control::ShiftDown:
      w.line_y = static_cast<int16_t>(w.start_y + a);   // C13a: currentY = y + n (from the text's start)
      break;
   case Control::Fill:
      // C13a: FillWindowPixelBuffer(PIXEL_FILL(bgColor)), the pen back to the text's start
      fill_rect(0, 0, width_px(), height_px(), w.background);
      w.pen_x = w.start_x;
      w.line_y = w.start_y;
      w.line = 0;
      break;
   case Control::Clear:
      if (a > 0)
      {
         clear_span(a);
         w.pen_x = static_cast<int16_t>(w.pen_x + a);
         return true;
      }
      break;
   case Control::SkipTo:
      w.pen_x = static_cast<int16_t>(w.start_x + a);
      break;
   case Control::ClearTo:
   {
      const int width = w.start_x + a - w.pen_x;
      if (width > 0)
      {
         clear_span(width);
         w.pen_x = static_cast<int16_t>(w.pen_x + width);
         return true;
      }
      break;
   }
   case Control::MinSpacing:
      w.min_spacing = a;
      break;
   case Control::Font:
      // C10: the style's font n draws the next glyphs (an unknown n keeps the current one)
      if (a < w.c.font_count && w.c.fonts != nullptr && w.c.fonts[a] != nullptr)
      {
         w.glyphs = w.c.fonts[a];
      }
      break;
   case Control::ResetFont:
      w.glyphs = w.c.font;
      break;
   case Control::Se:
   case Control::Bgm:
      // E1: PLAY_SE / PLAY_BGM n (two bytes, low first)
      sound::control(k, static_cast<uint16_t>(a | p[kEscapeHead + 1] << kByteBits));
      break;
   case Control::PauseMusic:
   case Control::ResumeMusic:
      sound::control(k, 0);
      break;
   case Control::WaitSe:
      w.pause = Pause::Sound;   // E1: RENDER_STATE_WAIT_SE
      break;
   default:
      break;   // nothing to draw
   }
   return false;
}

/// Types codes until a glyph is drawn (or everything up to a pause when @p all).
/// @param all  do not stop after a glyph
void type(bool all)
{
   while (cur().next != nullptr && cur().pause == Pause::None)
   {
      const Code c = *cur().next;
      if (c == kCodeEnd)
      {
         if (cur().resume != nullptr)
         {
            cur().next = cur().resume;
            cur().resume = nullptr;
            continue;
         }
         cur().next = nullptr;
         return;
      }
      const bool field = c == kCodePlaceholder && cur().next[1] != kCodeEnd && is_field_arg(cur().next[1]);
      if (field && cur().resume == nullptr)
      {
         expand_field(cur().next[1], cur().field);
         cur().resume = cur().next + kEscapeHead;
         cur().next = cur().field.codes;
         continue;
      }
      if (c == kCodeControl || c == kCodePlaceholder)
      {
         const bool drew = c == kCodeControl && cur().next[1] != kCodeEnd && control(cur().next);
         cur().next += escape_length(cur().next);
         if (drew && !all)
         {
            return;   // C13a: a Clear types as a glyph
         }
         continue;
      }
      ++cur().next;
      if (c == kCodeNewline)
      {
         new_line();
         continue;
      }
      if (c == kCodePrompt)
      {
         cur().pause = Pause::Prompt;
         return;
      }
      if (c == kCodePageBreak)
      {
         cur().pause = Pause::Page;
         return;
      }
      draw_glyph(c, cur().pen_x, cur().line_y);
      pen_after_glyph(c);
      if (!all)
      {
         return;
      }
   }
}

/// N3: one frame of the arrow image (kArrowImage) while a wait shows it: after arrow_delay frames (none for the
/// first step) the image's next window of rows is drawn at the pen over the background (its 0 pixels show the
/// background), and the step moves on.
void step_image_arrow()
{
   Window& w = cur();
   if (w.c.arrow_rows == nullptr || w.c.arrow_offsets == nullptr || w.c.arrow_steps == 0)
   {
      return;
   }
   if (w.arrow_wait > 0)
   {
      --w.arrow_wait;
      return;
   }
   fill_rect(w.pen_x, w.line_y, kArrowImageWidth, w.c.arrow_height, w.background);
   const int first = w.c.arrow_offsets[w.arrow_step % w.c.arrow_steps];
   for (int r = 0; r < w.c.arrow_height && first + r < w.c.arrow_image_rows; ++r)
   {
      const uint32_t row = w.c.arrow_rows[first + r];
      for (int x = 0; x < kArrowImageWidth; ++x)
      {
         const auto v = static_cast<uint8_t>((row >> (x * kNibbleBits)) & kNibbleMask);
         if (v != 0)
         {
            put_pixel(w.pen_x + x, w.line_y + r, v);
         }
      }
   }
   w.arrow_shown = true;
   w.arrow_wait = w.c.arrow_delay;
   w.arrow_step = static_cast<uint8_t>((w.arrow_step + 1) % w.c.arrow_steps);
}

/// N3: erases the arrow image (its rectangle at the pen back to the background).
void erase_image_arrow()
{
   Window& w = cur();
   if (w.arrow_shown)
   {
      fill_rect(w.pen_x, w.line_y, kArrowImageWidth, w.c.arrow_height, w.background);
   }
   w.arrow_shown = false;
}

/// One frame of a prompt, a page break or a scroll's wait: the arrow blinks (N3: or the arrow image steps); a press
/// goes on (a page clears the window, a scroll starts moving).
/// @param pressed  Accept or Back went down this frame
void wait_with_arrow(bool pressed)
{
   Window& w = cur();
   const bool image = w.c.arrow_kind == kArrowImage;
   if (image)
   {
      step_image_arrow();
   }
   else
   {
      w.blink = static_cast<uint8_t>(w.blink + 1);
      show_arrow((w.blink / kArrowBlinkFrames) % kBlinkPhases == 0);
   }
   if (!pressed)
   {
      return;
   }
   if (image)
   {
      erase_image_arrow();
      w.arrow_step = 0;   // the next wait starts its steps again, its first at once
      w.arrow_wait = 0;
   }
   else
   {
      show_arrow(false);
   }
   if (w.pause == Pause::Page)
   {
      clear_all();
      w.pause = Pause::None;
   }
   else if (w.pause == Pause::Scroll)
   {
      w.pause = Pause::Scrolling;
      w.scrolled = 0;
      w.pen_x = w.start_x;
   }
   else
   {
      w.pause = Pause::None;
   }
   w.blink = 0;
}

/// C10: kTypingCounter's reads in one frame (window.h): the waits first, then codes until one ends the frame (a
/// glyph drawn, a wait begun, the counter still running, the end).
/// @param pressed  Accept or Back went down this frame
/// @param held     Accept or Back is held
void counter_frame(bool pressed, bool held)
{
   Window& w = cur();
   if (w.lead > 0)
   {
      --w.lead;   // the box's own first frames: nothing is read yet
      return;
   }
   const bool rush = w.c.speedup == kSpeedupRest && held;   // held: the rest up to the next pause at once
   for (int guard = 0; guard < kMaxCodesPerFrame; ++guard)
   {
      switch (w.pause)
      {
      case Pause::Frames:
         // a Pause control: n frames, then the frame that hands back to reading
         if (w.frames != 0)
         {
            --w.frames;
         }
         else
         {
            w.pause = Pause::None;
         }
         return;
      case Pause::Button:
         if (pressed)
         {
            w.pause = Pause::None;
         }
         return;
      case Pause::Prompt:
      case Pause::Page:
      case Pause::Scroll:
         wait_with_arrow(pressed);
         return;
      case Pause::Scrolling:
         // the line moves up; the frame after its last row reads on
         if (scroll_pixels())
         {
            w.pause = Pause::None;
         }
         return;
      case Pause::Sound:
         // E1: RENDER_STATE_WAIT_SE: the frame that sees the sound ended still waits
         if (!sound::waiting())
         {
            w.pause = Pause::None;
         }
         return;
      case Pause::None:
      case Pause::NUM_PAUSES:
         break;
      }
      if (w.next == nullptr)
      {
         return;
      }
      if (w.c.speedup == kSpeedupPerFrame && held && w.sped_up)
      {
         w.timer = 0;   // sped up: held keeps the counter at 0
      }
      if (w.timer != 0 && w.c.char_delay != 0 && !rush)
      {
         --w.timer;
         if (w.c.speedup == kSpeedupPerFrame && pressed)
         {
            w.sped_up = true;   // a press while the counter runs ends it
            w.timer = 0;
         }
         return;
      }
      // the end of a placeholder's expansion or the expansion itself costs nothing (the game expands its text
      // before printing it): read on at once
      const Code c = *w.next;
      if (c == kCodeEnd && w.resume != nullptr)
      {
         w.next = w.resume;
         w.resume = nullptr;
         continue;
      }
      if (c == kCodePlaceholder && w.next[1] != kCodeEnd && is_field_arg(w.next[1]) && w.resume == nullptr)
      {
         expand_field(w.next[1], w.field);
         w.resume = w.next + kEscapeHead;
         w.next = w.field.codes;
         continue;
      }
      w.timer = w.c.char_delay;   // every code read restarts the counter
      if (c == kCodeEnd)
      {
         w.next = nullptr;
         return;
      }
      if (c == kCodeControl || c == kCodePlaceholder)
      {
         const bool drew = c == kCodeControl && w.next[1] != kCodeEnd && control(w.next);
         w.next += escape_length(w.next);
         if (w.pause == Pause::Button || w.pause == Pause::Scroll || w.pause == Pause::Sound)
         {
            w.blink = 0;
            return;   // a wait ends the frame (a Pause control's first frame counts below, at once)
         }
         if (drew && !rush)
         {
            return;   // C13a: a Clear of a positive width types as a glyph (it counts as drawn)
         }
         continue;
      }
      ++w.next;
      if (c == kCodeNewline)
      {
         new_line();
         continue;
      }
      if (c == kCodePrompt || c == kCodePageBreak)
      {
         w.pause = c == kCodePrompt ? Pause::Prompt : Pause::Page;
         w.blink = 0;
         return;
      }
      draw_glyph(c, w.pen_x, w.line_y);
      pen_after_glyph(c);
      if (!rush)
      {
         return;
      }
   }
}

/// Copies the changed tiles to VRAM: C10, queued to land at the next VBlank with the frame's other changes (the
/// handheld games copy a window to VRAM with a DMA request served in VBlank), so a glyph shows in the same picture on
/// every target (an immediate write showed on the GBA one frame before the sprites and the map, and before the
/// desktop's picture).
/// The window's tiles stay valid until then (they live in the Window); a copy too large for one must-land request is
/// split. open() copies at once instead: it writes the window's map cells at once, and its tiles must show with them
/// (a queued clear showed the last text for a frame).
/// @param now  copy at once
void upload(bool now = false)
{
   Window& w = cur();
   if (w.dirty_lo == kNoDirty)
   {
      return;
   }
   // C13a (R9 M4): a window's tiles never land in a map in use (a screen layer's, or the shown field's rings)
   ENGINE_ASSERT(hal::vram::tiles_allowed(
      w.c.char_block * video::kCharBlockBytes + static_cast<uint32_t>(w.c.first_tile + w.dirty_lo) * video::kTileBytes4bpp,
      static_cast<uint32_t>(w.dirty_hi - w.dirty_lo + 1) * video::kTileBytes4bpp));
   if (now)
   {
      video::load_bg_tiles(w.c.char_block, static_cast<video::TileIndex>(w.c.first_tile + w.dirty_lo),
                           &w.store.words[w.dirty_lo * kWordsPerTile],
                           static_cast<uint16_t>(w.dirty_hi - w.dirty_lo + 1));
      w.dirty_lo = kNoDirty;
      w.dirty_hi = kNoDirty;
      return;
   }
   constexpr int kTilesPerRequest = static_cast<int>(hal::vram::kMaxMustLandBytes / video::kTileBytes4bpp);
   for (int lo = w.dirty_lo; lo <= w.dirty_hi; lo += kTilesPerRequest)
   {
      const int count = w.dirty_hi - lo + 1 < kTilesPerRequest ? w.dirty_hi - lo + 1 : kTilesPerRequest;
      const uint32_t offset = w.c.char_block * video::kCharBlockBytes +
                              static_cast<uint32_t>(w.c.first_tile + lo) * video::kTileBytes4bpp;
      hal::vram::copy32(video::Region::BgVram, offset, &w.store.words[lo * kWordsPerTile],
                        static_cast<uint32_t>(count * kWordsPerTile), hal::vram::Priority::MustLand,
                        video::Source::Ewram);
   }
   cur().dirty_lo = kNoDirty;
   cur().dirty_hi = kNoDirty;
}

}   // namespace

void attach(Window& w, Tiles tiles)
{
   w.store = tiles;
}

void pool_init(TilePool& p, uint32_t* words, uint32_t tiles)
{
   p.words = words;
   p.capacity = tiles;
   p.high_water = 0;
   for (int i = 0; i < kMaxPoolPieces; ++i)
   {
      p.start[i] = 0;
      p.count[i] = 0;
   }
}

Tiles pool_take(TilePool& p, int tiles)
{
   if (p.words == nullptr || tiles <= 0)
   {
      return Tiles{};
   }
   int entry = -1;
   for (int i = 0; i < kMaxPoolPieces && entry < 0; ++i)
   {
      entry = p.count[i] == 0 ? i : entry;
   }
   if (entry < 0)
   {
      return Tiles{};
   }
   // first fit: from the pool's start, past every piece the stretch overlaps, until nothing overlaps it; a stretch
   // a copy still waiting for the VBlank reads (a piece given back this frame: its last owner's tiles must land as
   // they were) is passed a tile at a time
   const auto want = static_cast<uint32_t>(tiles);
   const auto bytes = want * static_cast<uint32_t>(kBytesPerTile);
   uint32_t at = 0;
   bool moved = true;
   while (moved && at + want <= p.capacity)
   {
      moved = false;
      for (int i = 0; i < kMaxPoolPieces; ++i)
      {
         const uint32_t s = p.start[i];
         const uint32_t n = p.count[i];
         if (n != 0 && s < at + want && at < s + n)
         {
            at = s + n;
            moved = true;
         }
      }
      if (!moved && at + want <= p.capacity && hal::vram::reads_from(p.words + at * kWordsPerTile, bytes))
      {
         ++at;
         moved = true;
      }
   }
   if (at + want > p.capacity)
   {
      return Tiles{};
   }
   p.start[entry] = static_cast<uint16_t>(at);
   p.count[entry] = static_cast<uint16_t>(want);
   const uint32_t used = pool_used(p);
   p.high_water = used > p.high_water ? used : p.high_water;
   return Tiles{p.words + at * kWordsPerTile, static_cast<uint16_t>(want)};
}

void pool_give_back(TilePool& p, const Tiles& t)
{
   if (t.words == nullptr || p.words == nullptr || t.words < p.words)
   {
      return;
   }
   const auto at = static_cast<uint32_t>(t.words - p.words) / kWordsPerTile;
   for (int i = 0; i < kMaxPoolPieces; ++i)
   {
      if (p.count[i] != 0 && p.start[i] == at)
      {
         p.count[i] = 0;
         return;
      }
   }
}

uint32_t pool_used(const TilePool& p)
{
   uint32_t used = 0;
   for (int i = 0; i < kMaxPoolPieces; ++i)
   {
      used += p.count[i];
   }
   return used;
}

void open(Window& w, const Config& config)
{
   g_cur = &w;
   // field by field (the storage stays the owner's: attach())
   w.c = config;
   w.next = nullptr;
   w.resume = nullptr;
   w.pause = Pause::None;
   w.frames = 0;
   w.timer = 0;
   w.lead = 0;
   w.sped_up = false;
   w.glyphs = config.font;
   w.scrolled = 0;
   w.blink = 0;
   w.arrow_step = 0;
   w.arrow_wait = 0;
   w.instant = false;
   w.start_x = static_cast<int16_t>(w.c.left);
   w.start_y = static_cast<int16_t>(w.c.top);
   w.min_spacing = 0;
   if (w.c.width * w.c.height > kMaxTiles)
   {
      w.c.height = static_cast<uint8_t>(kMaxTiles / (w.c.width > 0 ? w.c.width : 1));
   }
   // C13c: the owner's storage holds the window (a release build keeps the rows that fit; none: it stays closed)
   ENGINE_ASSERT(w.store.words == nullptr || w.c.width * w.c.height <= w.store.count);
   if (w.c.width * w.c.height > w.store.count)
   {
      w.c.height = static_cast<uint8_t>(w.store.count / (w.c.width > 0 ? w.c.width : 1));
   }
   w.open = w.c.font != nullptr && w.store.words != nullptr && w.c.width > 0 && w.c.height > 0;
   w.dirty_lo = kNoDirty;
   w.dirty_hi = kNoDirty;
   w.ink = w.c.ink;
   w.shadow = w.c.shadow;
   w.background = w.c.background;
   if (!w.open)
   {
      return;
   }
   for (int j = 0; j < w.c.height; ++j)
   {
      for (int i = 0; i < w.c.width; ++i)
      {
         const auto tile = static_cast<video::TileIndex>(w.c.first_tile + j * w.c.width + i);
         video::write_map_cell(w.c.bg, w.c.x + i, w.c.y + j, video::map_cell(tile, w.c.palette_bank));
      }
   }
   clear_all();
   upload(true);   // with the map cells just written
}

void close(Window& w)
{
   g_cur = &w;
   w.open = false;
   w.next = nullptr;
   w.pause = Pause::None;
}

bool is_open(const Window& w)
{
   return w.open;
}

void print(Window& w, const Code* codes, bool instant)
{
   g_cur = &w;
   if (!w.open)
   {
      return;
   }
   w.ink = w.c.ink;
   w.shadow = w.c.shadow;
   w.background = w.c.background;
   w.start_x = static_cast<int16_t>(w.c.left);   // C13a: the printer's x and y
   w.start_y = static_cast<int16_t>(w.c.top);
   w.min_spacing = 0;
   clear_all();
   w.next = codes;
   w.resume = nullptr;
   w.pause = Pause::None;
   w.timer = 0;
   w.lead = w.c.lead_frames;   // C10: kTypingCounter's lead frames
   w.sped_up = false;
   w.glyphs = w.c.font;        // C10: a Font control of the last text does not carry over
   w.arrow_step = 0;           // N3: the arrow image starts its steps again
   w.arrow_wait = 0;
   w.instant = instant || (w.c.typing == kTypingInterval && w.c.frames_per_char == 0);
   if (w.instant)
   {
      type(true);
   }
   upload();
}

void clear(Window& w)
{
   g_cur = &w;
   if (!w.open)
   {
      return;
   }
   w.start_x = static_cast<int16_t>(w.c.left);
   w.start_y = static_cast<int16_t>(w.c.top);
   clear_all();
   w.next = nullptr;
   w.pause = Pause::None;
   upload();
}

void update(Window& w, bool pressed, bool held)
{
   g_cur = &w;
   if (!w.open)
   {
      return;
   }
   if (w.c.typing == kTypingCounter)
   {
      counter_frame(pressed, held);
      upload();
      return;
   }
   bool sound_ended = false;   // E1: the frame a WaitSe ends types nothing
   switch (w.pause)
   {
   case Pause::Prompt:
   case Pause::Page:
   case Pause::Scroll:
      wait_with_arrow(pressed);
      break;
   case Pause::Button:
      if (pressed)
      {
         w.pause = Pause::None;
      }
      break;
   case Pause::Scrolling:
      scroll_step();
      break;
   case Pause::Sound:
      if (!sound::waiting())
      {
         w.pause = Pause::None;   // E1: as the counter's printer, the next frame types on
         sound_ended = true;
      }
      break;
   case Pause::Frames:
      if (w.frames > 0)
      {
         --w.frames;
      }
      if (w.frames == 0)
      {
         w.pause = Pause::None;
      }
      break;
   case Pause::None:
   case Pause::NUM_PAUSES:
      break;
   }
   if (w.pause == Pause::None && w.next != nullptr && !sound_ended)
   {
      if (w.instant || held)
      {
         type(true);   // the rest up to the next pause at once
      }
      else if (w.timer > 0)
      {
         --w.timer;
      }
      else
      {
         type(false);
         w.timer = static_cast<uint8_t>(w.c.frames_per_char - 1);
      }
   }
   upload();
}

bool busy(const Window& w)
{
   return w.open && (w.next != nullptr || w.pause != Pause::None);
}

bool waiting(const Window& w)
{
   return w.open && (w.pause == Pause::Prompt || w.pause == Pause::Page || w.pause == Pause::Scroll ||
                     w.pause == Pause::Button);
}

int print_at(Window& w, int x, int y, const Code* codes)
{
   g_cur = &w;
   if (!w.open || codes == nullptr)
   {
      return x;
   }
   w.pen_x = static_cast<int16_t>(x);
   w.line_y = static_cast<int16_t>(y);
   w.start_x = static_cast<int16_t>(x);   // C13a: the printer's x and y (ShiftRight / ShiftDown / SkipTo from here)
   w.start_y = static_cast<int16_t>(y);
   w.min_spacing = 0;
   w.glyphs = w.c.font;
   FieldText field;
   const Code* resume = nullptr;
   for (const Code* p = codes; p != nullptr;)
   {
      const Code c = *p;
      if (c == kCodeEnd)
      {
         p = resume;   // the end of a placeholder's text goes back to the text after it
         resume = nullptr;
         continue;
      }
      if (c == kCodePlaceholder && p[1] != kCodeEnd && is_field_arg(p[1]) && resume == nullptr)
      {
         expand_field(p[1], field);
         resume = p + kEscapeHead;
         p = field.codes;
         continue;
      }
      if (c == kCodeControl || c == kCodePlaceholder)
      {
         if (c == kCodeControl && p[1] != kCodeEnd)
         {
            static_cast<void>(control(p));
            w.pause = Pause::None;   // waits are skipped: nothing types here
         }
         p += escape_length(p);
         continue;
      }
      ++p;
      if (c == kCodeNewline)
      {
         w.pen_x = static_cast<int16_t>(x);
         w.line_y = static_cast<int16_t>(w.line_y + w.c.font->line_height);
         continue;
      }
      if (c == kCodePrompt || c == kCodePageBreak)
      {
         continue;
      }
      draw_glyph(c, w.pen_x, w.line_y);
      pen_after_glyph(c);
   }
   w.glyphs = w.c.font;
   upload();
   return w.pen_x;
}

void fill(Window& w, int x, int y, int width, int height, uint8_t entry)
{
   g_cur = &w;
   if (!w.open)
   {
      return;
   }
   fill_rect(x, y, width, height, entry);
   upload();
}

void draw_glyph_tiles(uint32_t* tiles, int width_cells, int height_cells, const PropFont& font, Code c, int x, int y,
                      uint8_t ink, uint8_t shadow, uint8_t background)
{
   static_cast<void>(glyph_canvas(Canvas{tiles, width_cells, height_cells}, font, c, x, y,
                                  GlyphColors{ink, shadow, background}));
}

void fill_tiles(uint32_t* tiles, int width_cells, int height_cells, int x, int y, int width, int height, uint8_t entry)
{
   static_cast<void>(fill_canvas(Canvas{tiles, width_cells, height_cells}, x, y, width, height, entry));
}

void set_colors(Window& w, uint8_t ink, uint8_t background, uint8_t shadow)
{
   w.ink = ink;
   w.background = background;
   w.shadow = shadow;
}

namespace
{

/// line_width()'s walk (C13a: as the handheld games measure a string: Clear, SkipTo, ClearTo and MinSpacing).
/// @param font         the font
/// @param codes        encoded text
/// @param min_spacing  the least glyph advance so far (0: off); MinSpacing in the text changes it
/// @return pixels
int width_of(const PropFont& font, const Code* codes, int& min_spacing)
{
   int w = 0;
   const FontInfo& f = font.info;
   for (const Code* p = codes; *p != kCodeEnd && *p != kCodeNewline && *p != kCodePageBreak;)
   {
      if (*p == kCodePlaceholder && p[1] != kCodeEnd && is_field_arg(p[1]))
      {
         FieldText t;
         expand_field(p[1], t);
         w += width_of(font, t.codes, min_spacing);
      }
      if (*p == kCodeControl && p[1] != kCodeEnd)
      {
         const Code a = p[kEscapeHead];
         switch (static_cast<Control>(p[1]))
         {
         case Control::Clear:
            w += a;
            break;
         case Control::SkipTo:
            w = a;
            break;
         case Control::ClearTo:
            w = a > w ? a : w;
            break;
         case Control::MinSpacing:
            min_spacing = a;
            break;
         default:
            break;
         }
      }
      if (*p == kCodeControl || *p == kCodePlaceholder)
      {
         p += escape_length(p);
         continue;
      }
      if (*p >= f.first_code && *p < f.first_code + f.glyph_count)
      {
         const int glyph = font.widths[*p - f.first_code];
         // MinSpacing: a glyph takes at least that many pixels (and no letter spacing), as the printer draws it
         w += min_spacing > 0 ? (glyph > min_spacing ? glyph : min_spacing) : glyph + font.letter_spacing;
      }
      ++p;
   }
   return w;
}

}   // namespace

int line_width(const PropFont& font, const Code* codes)
{
   int min_spacing = 0;
   return width_of(font, codes, min_spacing);
}

namespace
{

/// The frame's outer rectangle: the window's grown by the padding and the one-cell border.
struct Outer
{
   int x0;   ///< Left cell (the border's).
   int y0;   ///< Top cell.
   int x1;   ///< Right cell, inclusive.
   int y1;   ///< Bottom cell, inclusive.
};

/// The outer rectangle of a framed window.
/// @param c  the window's settings
/// @param f  the frame
/// @return the rectangle, border included
Outer outer_of(const Config& c, const Frame& f)
{
   return Outer{c.x - f.pad_left - 1, c.y - f.pad_top - 1, c.x + c.width + f.pad_right, c.y + c.height + f.pad_bottom};
}

/// Part column or row of a cell along one axis: the near edge, the inside or the far edge.
/// @param v   the cell
/// @param lo  the near border cell
/// @param hi  the far border cell
/// @return 0, 1 or 2
int part_of(int v, int lo, int hi)
{
   constexpr int kNear = 0;     // the left or top edge
   constexpr int kInside = 1;   // padding (or the window's span)
   constexpr int kFarPart = 2;  // the right or bottom edge
   return v == lo ? kNear : (v == hi ? kFarPart : kInside);
}

}   // namespace

void draw_frame(const Config& c, const Frame& f)
{
   const Outer o = outer_of(c, f);
   for (int y = o.y0; y <= o.y1; ++y)
   {
      for (int x = o.x0; x <= o.x1; ++x)
      {
         const bool in_window = x >= c.x && x < c.x + c.width && y >= c.y && y < c.y + c.height;
         if (in_window || x < 0 || y < 0)
         {
            continue;   // the window's own cell (or off the map)
         }
         const int part = part_of(y, o.y0, o.y1) * kFrameColumns + part_of(x, o.x0, o.x1);
         const auto tile = static_cast<video::TileIndex>(f.first_tile + part);
         video::write_map_cell(c.bg, x, y, video::map_cell(tile, f.palette_bank));
      }
   }
}

void clear_frame(const Config& c, const Frame& f, video::MapCell blank)
{
   const Outer o = outer_of(c, f);
   for (int y = o.y0 < 0 ? 0 : o.y0; y <= o.y1; ++y)
   {
      for (int x = o.x0 < 0 ? 0 : o.x0; x <= o.x1; ++x)
      {
         video::write_map_cell(c.bg, x, y, blank);
      }
   }
}

} // namespace capsule::text::window

namespace capsule::text::sound
{

CAPSULE_WEAK void control(Control c, uint16_t value)
{
   static_cast<void>(c);
   static_cast<void>(value);
}

CAPSULE_WEAK bool waiting()
{
   return false;
}

} // namespace capsule::text::sound
