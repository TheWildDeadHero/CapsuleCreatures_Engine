/// @file
/// Text on layers and in the text box (see capsule/text/text_layer.h).
#include "capsule/text/text_layer.h"

#include "capsule/core/assert.h"
#include "capsule/gfx/background.h"
#include "capsule/gfx/palette.h"
#include "capsule/hal/video.h"
#include "capsule/text/encode.h"
#include "capsule/text/font.h"
#include "capsule/text/placeholder.h"

#include <cstdint>

namespace capsule::text::layer
{
namespace
{

namespace video = hal::video;
namespace background = gfx::background;

/// @name Text tiles in each layer's character block
///@{
constexpr int kFrameTile = background::kTextTiles;        ///< 9 box tiles: corners, edges, interior.
constexpr int kBlankTile = background::kTextTiles + 9;    ///< Fully transparent.
constexpr int kFontTile = background::kTextTiles + 16;    ///< Glyph of code kDefaultFont.first_code.
constexpr int kBannerTile = kFontTile + kMaxFontGlyphs;   ///< 9 custom banner frame tiles (after the largest font).
///@}

/// @name Character block limits
/// The last character block (BG3's) holds the maps' screen blocks in its
/// upper tiles, so a font loaded there must end below them;
/// tools/text_encoder.py BG3_MAX_FONT_GLYPHS refuses text on bg3 at build time beyond that.
///@{
constexpr int kTilesPerCharBlock =
   static_cast<int>(video::kCharBlockBytes) / video::kTileBytes4bpp;   ///< 4bpp tiles in one character block.
constexpr int kLastCharBlock = video::kNumCharBlocks - 1;   ///< The last character block.
constexpr int kLastBlockMapTile = background::kMapScreenBlock * static_cast<int>(video::kScreenBlockBytes) /
                                     video::kTileBytes4bpp -
                                  kLastCharBlock * kTilesPerCharBlock;   ///< Its first tile under screen block 28.
constexpr int kLastBlockMaxFontGlyphs = kLastBlockMapTile - kFontTile;   ///< Most font glyphs it holds.
///@}
static_assert(kLastBlockMaxFontGlyphs == 96, "tools/text_encoder.py BG3_MAX_FONT_GLYPHS must follow");

/// @name Palette entries in a style bank
///@{
constexpr int kInk = 1;      ///< Glyph foreground.
constexpr int kShadow = 2;   ///< Glyph shadow.
constexpr int kFill = 3;     ///< Box interior.
constexpr int kFrame = 4;    ///< Box frame.
constexpr int kStyleColors = kFrame - kInk + 1;   ///< Entries a style loads (kInk..kFrame).
///@}

/// @name Box frame tiles
/// 3x3 tiles: corners, edges and the interior, row by row.
///@{
constexpr int kBoxTileSide = 3;                       ///< Frame tiles per side.
constexpr int kBoxTiles = kBoxTileSide * kBoxTileSide;   ///< Frame tiles.
static_assert(kBannerTile + kBoxTiles <= kTilesPerCharBlock, "the banner frame tiles must fit in BG0's character block");
constexpr int kLastPart = kBoxTileSide - 1;           ///< Part index of the right column / bottom row.
constexpr int kBoxInteriorTile = kFrameTile + kBoxTileSide + 1;   ///< The interior: row 1, column 1.
constexpr int kLastPixel = kTilePixels - 1;           ///< Last pixel row / column of a tile.
///@}

/// @name Box geometry, cells
///@{
constexpr int kMinBoxCells = 3;    ///< Smallest box width / height: frame and one cell inside.
constexpr int kFrameCells = 2;     ///< Cells the frame takes from a box's width or height (one per side).
///@}

/// Codes of an escape: kCodeControl or kCodePlaceholder and the code it carries.
constexpr int kEscapeCodes = 2;
/// Bits of one 4bpp pixel inside a tile row.
constexpr uint32_t kPixelMask4bpp = 15;
/// Bank of style 0: styles count down from the last bank.
constexpr uint8_t kStyleZeroBank = gfx::palette::kBanks - 1;

/// Which font variant a character block holds.
enum class Font : uint8_t
{
   None,          ///< Not loaded.
   Transparent,   ///< Glyph background transparent.
   Opaque,        ///< Glyph background = box fill (kFill).
};

/// An open text box.
struct Box
{
   uint8_t open;   ///< 1 while open.
   uint8_t x;      ///< Left, cells (frame included).
   uint8_t y;      ///< Top, cells.
   uint8_t w;      ///< Width, cells.
   uint8_t h;      ///< Height, cells.
};

/// Why typing stopped before the end.
enum class Pause : uint8_t
{
   None,     ///< Typing (or idle).
   Prompt,   ///< At a prompt (kCodePrompt): resume() goes on.
   Page,     ///< At a page break (kCodePageBreak): resume() clears the page, then goes on.
};

/// Text being typed.
struct Typing
{
   const Code* next;     ///< Next code, or nullptr when idle.
   const Code* resume;   ///< Where the text goes on after a field placeholder's expansion (next points into it), or nullptr.
   uint8_t target;       ///< Target.
   uint8_t anchor;       ///< Anchor column.
   uint8_t top;          ///< First row of the current page.
   uint8_t row;          ///< Current row.
   uint8_t col;          ///< Current column.
   Align align;          ///< Alignment.
   uint8_t bank;         ///< Palette bank.
   uint8_t timer;        ///< Frames since the last character.
   Pause pause;          ///< Stopped at a prompt or page break.
};

Font g_font[video::kNumBgs];   ///< Font variant per character block.
uint8_t g_version[video::kNumBgs];   ///< background::block_version() when the tiles were loaded.
uint8_t g_touched = 0;         ///< Layers printed on (bit per layer).
uint8_t g_style = 0;           ///< Style for the next print.
Box g_box{};                   ///< The text box.
Box g_banner{};                ///< The banner.
bool g_banner_custom = false;  ///< The banner uses its own frame tiles.
Typing g_typing{};             ///< Typing in progress.
FieldText g_field{};           ///< Expansion of the field placeholder being typed (Typing::resume).

/// @name Shared style colors
///@{
constexpr video::Color kShadowColor = video::rgb5(2, 3, 8);     ///< Glyph shadow.
constexpr video::Color kFillColor = video::rgb5(3, 5, 12);      ///< Box interior.
constexpr video::Color kFrameColor = video::rgb5(26, 28, 31);   ///< Box frame.
///@}

/// Ink colors per style; shadow, fill and frame are shared.
constexpr video::Color kStyleInk[kStyles] = {
   video::rgb5(31, 31, 31),
   video::rgb5(31, 28, 8),
   video::rgb5(12, 28, 31),
   video::rgb5(31, 16, 24),
};

uint16_t g_plan = 0;           ///< background::plan_version() when the style palettes were loaded.

/// Palette bank of a style.
/// @param style  0 to kStyles - 1
/// @return BG palette bank; style 0's when the style's bank is the map's or
///         the open banner's (kBannerBank)
uint8_t bank_of(uint8_t style)
{
   const auto bank = static_cast<uint8_t>(kStyleZeroBank - (style < kStyles ? style : 0));
   if (bank == kBannerBank && g_banner.open)
   {
      return kStyleZeroBank;
   }
   return bank < background::first_ui_bank() ? kStyleZeroBank : bank;
}

/// Loads the style palettes into the banks the UI may use.
void load_styles()
{
   g_plan = background::plan_version();
   for (int s = 0; s < kStyles; ++s)
   {
      const auto bank = static_cast<uint8_t>(kStyleZeroBank - s);
      if (bank < background::first_ui_bank())
      {
         continue;
      }
      video::Color colors[kStyleColors];   // entries kInk..kFrame
      colors[kInk - kInk] = kStyleInk[s];
      colors[kShadow - kInk] = kShadowColor;
      colors[kFill - kInk] = kFillColor;
      colors[kFrame - kInk] = kFrameColor;
      gfx::palette::load_bg(static_cast<uint8_t>(bank * gfx::palette::kBankColors + kInk), colors, kStyleColors);
   }
}

/// Whether text may go on a layer now (over a map only BG0, when the map
/// leaves it free; see gfx::background's map plan).
/// @param bg  layer
/// @return true when it may
bool usable(video::BgIndex bg)
{
   return !background::map_plan() || (bg == 0 && !background::is_reserved(0));
}

/// Forgets what was printed when a map came or went since (the layers
/// changed hands, see gfx::background's map plan) and reloads the styles.
void sync_plan()
{
   if (g_plan == background::plan_version())
   {
      return;
   }
   g_touched = 0;
   g_box = Box{};
   g_banner = Box{};
   g_typing = Typing{};
   load_styles();
}

/// Loads the box tiles, the blank tile and a font variant into a layer's block.
/// @param bg      layer
/// @param opaque  glyph background = box fill
void load_tiles(video::BgIndex bg, bool opaque)
{
   if (bg >= video::kNumBgs)
   {
      return;
   }
   const Font want = opaque ? Font::Opaque : Font::Transparent;
   if (g_version[bg] != background::block_version(bg))
   {
      g_font[bg] = Font::None;
   }
   if (g_font[bg] == want)
   {
      return;
   }
   g_version[bg] = background::block_version(bg);
   const uint8_t block = background::char_block(bg);
   if (g_font[bg] == Font::None)
   {
      // Box tiles: 1-pixel frame (kFrame) around the kFill interior.
      // Order: top-left, top, top-right, left, interior, right, bottom-left, bottom, bottom-right.
      for (int t = 0; t < kBoxTiles; ++t)
      {
         const int col = t % kBoxTileSide;
         const int row = t / kBoxTileSide;
         uint32_t tile[kTilePixels];
         for (int y = 0; y < kTilePixels; ++y)
         {
            uint32_t word = 0;
            for (int x = 0; x < kTilePixels; ++x)
            {
               const bool edge = (col == 0 && x == 0) || (col == kLastPart && x == kLastPixel) ||
                                 (row == 0 && y == 0) || (row == kLastPart && y == kLastPixel);
               word |= static_cast<uint32_t>(edge ? kFrame : kFill) << (kBits4bpp * x);
            }
            tile[y] = word;
         }
         video::load_bg_tiles(block, static_cast<video::TileIndex>(kFrameTile + t), tile, 1);
      }
      const uint32_t blank[kTilePixels] = {};
      video::load_bg_tiles(block, kBlankTile, blank, 1);
   }
   const FontInfo& f = kDefaultFont;
   // BG3's block holds the maps from tile kLastBlockMapTile up (build-checked for text on bg3).
   ENGINE_ASSERT(block != kLastCharBlock || f.glyph_count <= kLastBlockMaxFontGlyphs);
   for (int g = 0; g < f.glyph_count; ++g)
   {
      const uint8_t* src = kDefaultFontGlyphs + g * kGlyphBytes2bpp;
      uint32_t tile[kTilePixels];
      for (int row = 0; row < kTilePixels; ++row)
      {
         uint32_t word = expand_2bpp(src[row * kRowBytes2bpp]) |
                         (uint32_t{expand_2bpp(src[row * kRowBytes2bpp + 1])} << kRightHalfShift4bpp);
         if (opaque)
         {
            // Transparent pixels (0) take the box fill.
            for (int px = 0; px < kTilePixels; ++px)
            {
               if (((word >> (kBits4bpp * px)) & kPixelMask4bpp) == 0)
               {
                  word |= static_cast<uint32_t>(kFill) << (kBits4bpp * px);
               }
            }
         }
         tile[row] = word;
      }
      video::load_bg_tiles(block, static_cast<video::TileIndex>(kFontTile + g), tile, 1);
   }
   g_font[bg] = want;
}

/// Gets a layer ready for text: prepared, tiles loaded in the right variant.
/// @param bg  layer
void prepare(video::BgIndex bg)
{
   ENGINE_ASSERT(usable(bg));
   background::prepare_text(bg, video::map_cell(kBlankTile, 0));
   background::show(bg, true);
   load_tiles(bg, bg == 0 && (g_box.open || g_banner.open));
   g_touched = static_cast<uint8_t>(g_touched | (1 << bg));
}

/// Writes one glyph cell, clipped to the target.
/// @param target  0-3 or kBoxTarget
/// @param col     column within the target
/// @param row     row within the target
/// @param c       the code
/// @param bank    palette bank
void put(uint8_t target, int col, int row, Code c, uint8_t bank)
{
   const FontInfo& f = kDefaultFont;
   if (c < f.first_code || c >= f.first_code + f.glyph_count)
   {
      c = '?';
   }
   video::BgIndex bg = target;
   if (target == kBoxTarget || target == kBannerTarget)
   {
      const Box& b = target == kBoxTarget ? g_box : g_banner;
      if (!b.open || col < 0 || row < 0 || col >= b.w - kFrameCells || row >= b.h - kFrameCells)
      {
         return;
      }
      bg = 0;
      col += b.x + 1;
      row += b.y + 1;
      if (target == kBannerTarget)
      {
         bank = kBannerBank;
      }
   }
   if (col < 0 || col >= kBgMapCells)
   {
      return;
   }
   row &= kBgMapCells - 1;   // a layer's map wraps vertically
   video::write_map_cell(bg, col, row,
                         video::map_cell(static_cast<video::TileIndex>(kFontTile + (c - f.first_code)), bank));
}

/// Width of the line starting at @p codes, in cells.
/// @param codes  encoded text
/// @return cells before the next newline, page break or the end (text::line_cells())
int line_width(const Code* codes)
{
   return line_cells(codes);
}

/// First column of a line for an alignment.
/// @param anchor  anchor column
/// @param align   alignment
/// @param width   line width
/// @return the column
uint8_t line_start(uint8_t anchor, Align align, int width)
{
   int col = anchor;
   if (align == Align::Center)
   {
      col = anchor - width / kHalf;
   }
   if (align == Align::Right)
   {
      col = anchor - width + 1;
   }
   return static_cast<uint8_t>(col < 0 ? 0 : col);
}

/// Clears rows of a target's cells, leaving any typing alone.
/// @param target     0-3 or kBoxTarget (the banner is not cleared: reprint it)
/// @param first_row  first row
/// @param rows       rows to clear; 0 = all
void clear_cells(uint8_t target, uint8_t first_row, uint8_t rows);

/// Moves the typing to the next row.
/// @param t  the typing state (next: the code after the newline)
void next_row(Typing& t)
{
   ++t.row;
   t.col = line_start(t.anchor, t.align, line_width(t.next));
}

/// Types one code of the current typing state. Escapes take no time: control
/// escapes are skipped with their code (no control is known yet), and a field
/// placeholder switches to its expansion (g_field) until that ends.
/// @param instant  printing at once: prompts are skipped and a page break
///                 continues on the next row (nothing waits for a button)
/// @return false once the text has ended or stopped at a prompt or page break
bool type_one(bool instant)
{
   Typing& t = g_typing;
   if (t.next == nullptr || t.pause != Pause::None)
   {
      return false;
   }
   for (;;)
   {
      const Code c = *t.next;
      if (c == kCodeEnd && t.resume != nullptr)
      {   // the placeholder's text is typed: back to the text
         t.next = t.resume;
         t.resume = nullptr;
         continue;
      }
      if (c == kCodeControl || c == kCodePlaceholder)
      {
         if (t.next[1] == kCodeEnd)
         {   // an escape at the very end: the text ends
            ++t.next;
            continue;
         }
         // Battle placeholders are replaced before printing (battle::format()); a raw one is a bug.
         ENGINE_ASSERT(c != kCodePlaceholder || is_field_arg(t.next[1]));
         if (c == kCodePlaceholder && t.resume == nullptr && is_field_arg(t.next[1]))
         {
            expand_field(t.next[1], g_field);
            t.resume = t.next + kEscapeCodes;
            t.next = g_field.codes;
            continue;
         }
         t.next += escape_length(t.next);   // I4: a control's argument bytes too
         continue;
      }
      if (c == kCodePrompt || c == kCodePageBreak)
      {
         ++t.next;
         if (!instant)
         {   // wait for resume()
            t.pause = c == kCodePrompt ? Pause::Prompt : Pause::Page;
            return false;
         }
         if (c == kCodePageBreak)
         {
            next_row(t);
            return true;
         }
         continue;
      }
      break;
   }
   const Code c = *t.next;
   if (c == kCodeEnd)
   {
      t.next = nullptr;
      return false;
   }
   ++t.next;
   if (c == kCodeNewline)
   {
      next_row(t);
      return true;
   }
   put(t.target, t.col, t.row, c, t.bank);
   ++t.col;
   return true;
}

/// Finishes any typing at once (prompts skipped, page breaks as newlines;
/// see type_one()); a text waiting at a prompt or page break goes on too.
void finish_typing()
{
   g_typing.pause = Pause::None;
   while (type_one(true))
   {
   }
}

/// Whether a pointer is inside the stack in use (the current frame and its
/// callers). Only the GBA can tell cheaply: its stack is the top of IWRAM.
/// Elsewhere it answers false.
/// @param p  the pointer
/// @return true when @p p points into a live stack frame
[[maybe_unused]] bool on_stack(const void* p)
{
#if defined(CAPSULE_PLATFORM_GBA)
   constexpr uintptr_t kIwramEnd = 0x03008000u;   // one past the last IWRAM byte (the stacks grow down from it)
   volatile const uint8_t here = 0;   // a slot in this frame: the lowest live stack address, near enough
   const auto a = reinterpret_cast<uintptr_t>(p);
   return a >= reinterpret_cast<uintptr_t>(&here) && a < kIwramEnd;
#else
   (void)p;
   return false;
#endif
}

} // namespace

void reset()
{
   for (Font& f : g_font)
   {
      f = Font::None;
   }
   g_touched = 0;
   g_style = 0;
   g_box = Box{};
   g_banner = Box{};
   g_typing = Typing{};
   load_styles();
}

void set_style(uint8_t style)
{
   g_style = style < kStyles ? style : 0;
}

void print(uint8_t target, uint8_t x, uint8_t y, Align align, bool typed, const Code* codes)
{
   sync_plan();
   ENGINE_ASSERT(target <= kBannerTarget && codes != nullptr);
   ENGINE_ASSERT(!typed || !on_stack(codes));   // typed text is read on later frames (see text_layer.h)
   finish_typing();
   if (target == kBannerTarget)
   {
      if (!g_banner.open)
      {
         return;
      }
   }
   else if (target == kBoxTarget)
   {
      if (!g_box.open)
      {
         return;
      }
   }
   else
   {
      if (target >= video::kNumBgs)
      {
         return;
      }
      ENGINE_ASSERT(!background::is_affine(target));
      if (!usable(target))
      {   // asserted in prepare(); release builds print nothing
         ENGINE_ASSERT(false && "text layer taken by the map");
         return;
      }
      prepare(target);
   }
   g_typing = Typing{codes, nullptr, target, x, y, y, line_start(x, align, line_width(codes)), align, bank_of(g_style), 0,
                     Pause::None};
   if (!typed)
   {
      finish_typing();
   }
}

void clear(uint8_t target, uint8_t first_row, uint8_t rows)
{
   sync_plan();
   ENGINE_ASSERT(target <= kBannerTarget);
   if (target == kBannerTarget)
   {
      return;
   }   // reprint the banner instead
   if (g_typing.next != nullptr && g_typing.target == target)
   {
      g_typing = Typing{};
   }
   clear_cells(target, first_row, rows);
}

namespace
{

/// Draws a box frame and its interior on BG0, one row write per row (the
/// VRAM queue is checked once per row, not once per cell).
/// @param x      left, cells
/// @param y      top, cells
/// @param w      width, cells (at most kBgMapCells)
/// @param h      height, cells
/// @param first  the frame's first tile (9 tiles: corners, edges, interior)
/// @param bank   palette bank
void draw_frame(uint8_t x, uint8_t y, uint8_t w, uint8_t h, int first, uint8_t bank)
{
   video::MapCell cells[kBgMapCells];
   for (int row = 0; row < h; ++row)
   {
      const int r = row == 0 ? 0 : (row == h - 1 ? kLastPart : 1);
      for (int col = 0; col < w; ++col)
      {
         const int c = col == 0 ? 0 : (col == w - 1 ? kLastPart : 1);
         cells[col] = video::map_cell(static_cast<video::TileIndex>(first + r * kBoxTileSide + c), bank);
      }
      video::write_map_row(0, x, y + row, cells, w);
   }
}

void clear_cells(uint8_t target, uint8_t first_row, uint8_t rows)
{
   if (target == kBannerTarget)
   {
      return;
   }
   if (target == kBoxTarget)
   {
      if (!g_box.open)
      {
         return;
      }
      const int inner_h = g_box.h - kFrameCells;
      const int inner_w = g_box.w - kFrameCells;
      const int last = rows == 0 ? inner_h : first_row + rows;
      video::MapCell cells[kBgMapCells];
      for (int col = 0; col < inner_w; ++col)
      {
         cells[col] = video::map_cell(kBoxInteriorTile, bank_of(0));
      }
      for (int row = first_row; row < last && row < inner_h; ++row)
      {
         video::write_map_row(0, g_box.x + 1, g_box.y + 1 + row, cells, inner_w);
      }
      return;
   }
   if (!(g_touched & (1 << target)))
   {
      return;
   }
   if (rows == 0)
   {
      background::restore(target, 0, 0, kBgMapCells, kBgMapCells);
   }
   else
   {
      background::restore(target, 0, first_row, kBgMapCells, rows);
   }
   if (target == 0 && g_box.open)
   {
      open_box(g_box.x, g_box.y, g_box.w, g_box.h);
   }   // keep the box drawn
}

} // namespace

void open_box(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
   sync_plan();
   ENGINE_ASSERT(w >= kMinBoxCells && h >= kMinBoxCells && x + w <= kBgMapCells && y + h <= kBgMapCells);
   if (!usable(0))
   {   // the map uses BG0: no box (release builds stay out of the map's VRAM)
      ENGINE_ASSERT(false && "the map uses BG0; the text box needs it");
      return;
   }
   if (g_banner.open)
   {
      close_banner();
   }   // a message ends the banner (the original games close their popup too)
   g_box = Box{1, x, y, w, h};
   prepare(0);   // also switches BG0's font to the opaque variant
   draw_frame(x, y, w, h, kFrameTile, bank_of(0));
}

void close_box()
{
   sync_plan();
   if (!g_box.open)
   {
      return;
   }
   if (g_typing.next != nullptr && g_typing.target == kBoxTarget)
   {
      g_typing = Typing{};
   }
   const Box b = g_box;
   g_box = Box{};
   background::restore(0, b.x, b.y, b.w, b.h);
}

void close_banner()
{
   sync_plan();
   if (!g_banner.open)
   {
      return;
   }
   if (g_typing.next != nullptr && g_typing.target == kBannerTarget)
   {
      g_typing = Typing{};
   }
   const Box b = g_banner;
   g_banner = Box{};
   if (usable(0))
   {
      background::restore(0, b.x, b.y, b.w, b.h);
   }
   load_styles();   // bank kBannerBank back to style 2
}

void open_banner(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint32_t* frame, const video::Color* colors)
{
   sync_plan();
   ENGINE_ASSERT(w >= kMinBoxCells && h >= kMinBoxCells && x + w <= kBgMapCells && y + h <= kBgMapCells &&
                 colors != nullptr);
   if (!usable(0))
   {
      ENGINE_ASSERT(false && "the map uses BG0; the banner needs it");
      return;
   }
   if (g_banner.open)
   {
      close_banner();
   }
   g_banner = Box{1, x, y, w, h};
   prepare(0);   // the opaque font variant
   gfx::palette::load_bg(static_cast<uint8_t>(kBannerBank * gfx::palette::kBankColors + 1), colors,
                         gfx::palette::kBankColors - 1);   // color 0 is the backdrop
   g_banner_custom = frame != nullptr;
   if (frame != nullptr)
   {
      video::load_bg_tiles(background::char_block(0), kBannerTile, frame, kBoxTiles);
   }
   draw_frame(x, y, w, h, frame != nullptr ? kBannerTile : kFrameTile, kBannerBank);
}

void clear_all()
{
   sync_plan();
   g_typing = Typing{};
   close_banner();
   close_box();
   for (video::BgIndex bg = 0; bg < video::kNumBgs; ++bg)
   {
      if (g_touched & (1 << bg))
      {
         background::restore(bg, 0, 0, kBgMapCells, kBgMapCells);
      }
   }
   g_touched = 0;
}

bool busy()
{
   return g_typing.next != nullptr;
}

bool waiting()
{
   return g_typing.next != nullptr && g_typing.pause != Pause::None;
}

void resume()
{
   sync_plan();
   Typing& t = g_typing;
   if (t.next == nullptr || t.pause == Pause::None)
   {
      return;
   }
   if (t.pause == Pause::Page)
   {   // a fresh page: the rows this page used are cleared, typing starts again at the first
      clear_cells(t.target, t.top, static_cast<uint8_t>(t.row - t.top + 1));
      t.row = t.top;
      t.col = line_start(t.anchor, t.align, line_width(t.next));
   }
   t.pause = Pause::None;
   t.timer = 0;
}

bool box_open()
{
   // A map coming or going since the last call closed it (sync_plan() forgets it on the next call).
   return g_box.open != 0 && g_plan == background::plan_version();
}

bool banner_open()
{
   return g_banner.open;
}

void update()
{
   sync_plan();
   if (g_typing.next == nullptr || g_typing.pause != Pause::None)
   {
      return;
   }
   if (++g_typing.timer < kTypeFrames)
   {
      return;
   }
   g_typing.timer = 0;
   // Newlines cost no time: type through them to the next visible character.
   while (type_one(false) && g_typing.next != nullptr && g_typing.next[-1] == kCodeNewline)
   {
   }
}

} // namespace capsule::text::layer
