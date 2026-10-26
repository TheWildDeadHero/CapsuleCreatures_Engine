/// @file
/// Text test: the font and printer on their own (capsule/text), page by page.
/// L / R change page.
///   1 Glyphs  every glyph of the app's font by code: its code page
///             (data/text/test.codepage.json) is ASCII (0x20-0x7E) plus a
///             diamond at 0x80, a non-ASCII glyph (0x7F is a blank gap)
///   2 Inks    one line in each palette bank 4-15
///   3 Layout  newlines, columns 0 and 29, and a scrollable 32x32 text layer
///             with markers on its edges to check wrapping
#include "test_kit.h"

#include "capsule/core/types.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/text/encode.h"
#include "capsule/text/font.h"
#include "capsule/text/print.h"

#include <cstdint>

namespace
{

using namespace capsule;
using namespace capsule::testkit;
namespace video = hal::video;

constexpr video::BgIndex kLayoutBg = 1;          ///< Page 3's scrollable text layer.
constexpr uint8_t kLayoutScreenBlock = 28;       ///< Its map.

constexpr auto kApp = text::encode("TEXT TEST");        ///< App title.
constexpr auto kPageHint = text::encode("L/R: page");   ///< Controls hint.

/// Turns the page 3 layer off and resets its scroll.
void hide_layout_layer()
{
   video::configure_bg(kLayoutBg, false, 1, kConsoleCharBlock, kLayoutScreenBlock, false, 0);
   video::set_bg_scroll(kLayoutBg, 0, 0);
}

// --- page 1: glyphs ---------------------------------------------------------

constexpr auto kGlyphsName = text::encode("Glyphs");   ///< Page name.

constexpr int kGlyphsPerRow = 16;    ///< Glyphs per grid row: one per low hex digit.
constexpr int kGridCol = 6;          ///< Column of the grid's first glyph.
constexpr int kHexHeaderRow = 2;     ///< Row of the low-digit header above the grid.
constexpr int kGridTop = 3;          ///< Row of the grid's first glyph row.
constexpr int kRowHexCol = 2;        ///< Column of a row label's high digit (after "0x").
constexpr int kRowZeroCol = 3;       ///< Column of a row label's trailing "0".
constexpr int kGlyphCountRow = 11;   ///< Row of the glyph count.
constexpr int kCellSizeRow = 12;     ///< Row of the cell size.
constexpr int kOneCharLength = 2;    ///< One character and its terminator.

/// Page 1: every glyph of the app's font (kDefaultFont, compiled with its code page) by code.
void enter_glyphs()
{
   constexpr char kHex[] = "0123456789ABCDEF";
   const text::FontInfo& f = text::kDefaultFont;
   for (int i = 0; i < kGlyphsPerRow; ++i)
   {
      const char s[kOneCharLength] = {kHex[i], '\0'};
      put_ascii(INK_DIM, kGridCol + i, kHexHeaderRow, s);
   }
   const int first = f.first_code;
   const int last = f.first_code + f.glyph_count - 1;

   // One grid row per high hex digit, labelled "0x<digit>0".
   for (int row = first / kGlyphsPerRow; row <= last / kGlyphsPerRow; ++row)
   {
      const int y = kGridTop + row - first / kGlyphsPerRow;
      put_ascii(INK_DIM, 0, y, "0x");
      put_hex(INK_DIM, kRowHexCol, y, static_cast<uint32_t>(row), 1);
      put_ascii(INK_DIM, kRowZeroCol, y, "0");
      for (int col = 0; col < kGlyphsPerRow; ++col)
      {
         const int code = row * kGlyphsPerRow + col;
         if (code < first || code > last)
         {
            continue;
         }
         const text::Code one[kOneCharLength] = {static_cast<text::Code>(code), text::kCodeEnd};
         put(INK_TITLE, kGridCol + col, y, one);
      }
   }
   int x = put_ascii(INK_DIM, 0, kGlyphCountRow, "Glyphs:");
   put_uint(INK_ON, x + 1, kGlyphCountRow, f.glyph_count);
   x = put_ascii(INK_DIM, 0, kCellSizeRow, "Cell:");
   x = put_uint(INK_ON, x + 1, kCellSizeRow, f.cell_width);
   x = put_ascii(INK_ON, x, kCellSizeRow, "x");
   put_uint(INK_ON, x, kCellSizeRow, f.cell_height);
   controls(kPageHint.codes, nullptr);
}

// --- page 2: inks -----------------------------------------------------------

constexpr auto kInksName = text::encode("Inks");                  ///< Page name.
constexpr auto kSample = text::encode("Sphinx of black quartz!");   ///< Sample line.

constexpr int kAppBanks = 12;     ///< Palette banks free for the app (4-15).
constexpr int kInksTop = 3;       ///< Row of the first bank's line.
constexpr int kBankDigits = 2;    ///< Width of the bank number.
constexpr int kSampleCol = 5;     ///< Column of the sample line.

/// @name Ink spread
/// Each channel steps by its own amount per bank (mod 32, the 5-bit range) so
/// the hues spread around the wheel; kMinChannel is OR-ed in to keep them bright.
///@{
constexpr int kChannelLevels = 32;   ///< Levels of a 5-bit color channel.
constexpr int kMinChannel = 8;       ///< Bit OR-ed into each channel so no ink is too dark.
constexpr int kRedStep = 7;          ///< Red step per bank.
constexpr int kGreenStep = 13;       ///< Green step per bank.
constexpr int kGreenOffset = 10;     ///< Green start.
constexpr int kBlueStep = 19;        ///< Blue step per bank.
constexpr int kBlueOffset = 20;      ///< Blue start.
///@}

/// Page 2: the sample line in each palette bank 4-15.
void enter_inks()
{
   constexpr video::Color kShadow = video::rgb5(1, 2, 5);
   for (int b = 0; b < kAppBanks; ++b)
   {
      // A spread of inks: hue around the wheel, alternating bright and pale.
      const auto r = static_cast<uint8_t>((b * kRedStep) % kChannelLevels | kMinChannel);
      const auto g = static_cast<uint8_t>((b * kGreenStep + kGreenOffset) % kChannelLevels | kMinChannel);
      const auto bl = static_cast<uint8_t>((b * kBlueStep + kBlueOffset) % kChannelLevels | kMinChannel);
      const video::Color ink = video::rgb5(r, g, bl);
      const auto bank = static_cast<uint8_t>(kFirstAppBank + b);
      video::load_bg_palette(static_cast<uint8_t>(bank * kColorsPerBank + kInkEntry), &ink, 1);
      video::load_bg_palette(static_cast<uint8_t>(bank * kColorsPerBank + kShadowEntry), &kShadow, 1);

      const int y = kInksTop + b;
      put_ascii(INK_DIM, 0, y, "B");
      put_uint(INK_DIM, 1, y, bank, kBankDigits);
      text::print(text::PrintTarget{kConsoleBg, 0, bank}, text::kDefaultFont, kSampleCol, y, kSample.codes);
   }
   controls(kPageHint.codes, nullptr);
}

// --- page 3: layout ---------------------------------------------------------

/// @name Page 3 text
///@{
constexpr auto kLayoutName = text::encode("Layout");   ///< Screen text "Layout".
constexpr auto kMultiline =
   text::encode("Newlines:\nline two\n  indented three");   ///< Screen text "Newlines:\nline two\n  indented three".
constexpr auto kLeftEdge = text::encode("<col 0");   ///< Screen text "<col 0".
constexpr auto kRightEdge = text::encode("col 29>");   ///< Screen text "col 29>".
constexpr auto kLayoutHint1 =
   text::encode("L/R page  D-pad scroll BG1");   ///< Screen text "L/R page  D-pad scroll BG1".
constexpr auto kCorner = text::encode("+");   ///< Screen text "+".
constexpr auto kTopEdge = text::encode("TOP EDGE OF THE 32x32 MAP");   ///< Screen text "TOP EDGE OF THE 32x32 MAP".
constexpr auto kBottomEdge = text::encode("BOTTOM EDGE (ROW 31)");   ///< Screen text "BOTTOM EDGE (ROW 31)".
constexpr auto kVert = text::encode("|");   ///< Screen text "|".
///@}

constexpr int kMultilineRow = 2;   ///< Row of the newline sample.
constexpr int kEdgeRow = 5;   ///< Row of the edge-column markers.
constexpr int kRightEdgeLength = 7;   ///< Characters in kRightEdge, which ends in column 29.
constexpr int kLayoutTopRow = 7;   ///< Screen row BG1's top edge starts on.
constexpr int kMapLast = kMapCells - 1;   ///< Last row and column of BG1's map.
constexpr int kEdgeTextCol = 2;   ///< Column of the top and bottom edge labels.
constexpr int kScrollDigits = 4;   ///< Width of each scroll readout.

int16_t g_scroll_x = 0;   ///< BG1 scroll x.
int16_t g_scroll_y = 0;   ///< BG1 scroll y.

/// Draws the scroll readout in the second controls row.
void draw_scroll()
{
   blank(0, kCols, kControlsBottom);
   int x = put_ascii(INK_DIM, 0, kControlsBottom, "BG1 scroll");
   x = put_int(INK_ON, x + 1, kControlsBottom, g_scroll_x, kScrollDigits);
   put_int(INK_ON, x + 1, kControlsBottom, g_scroll_y, kScrollDigits);
}

/// Page 3: newlines, edge columns and a framed 32x32 layer.
void enter_layout()
{
   put(INK_ON, 0, kMultilineRow, kMultiline.codes);
   put(INK_ON, 0, kEdgeRow, kLeftEdge.codes);
   put(INK_ON, kCols - kRightEdgeLength, kEdgeRow, kRightEdge.codes);

   // BG1: its own 32x32 map, framed, under the console; starts scrolled so
   // its top edge is on screen row 7.
   video::configure_bg(kLayoutBg, true, 1, kConsoleCharBlock, kLayoutScreenBlock, false, 0);
   const text::PrintTarget t{kLayoutBg, 0, INK_OFF};
   text::clear(t, text::kDefaultFont);
   const text::PrintTarget mark{kLayoutBg, 0, INK_ON};
   for (int i = 1; i < kMapLast; ++i)
   {
      text::print(mark, text::kDefaultFont, 0, i, kVert.codes);
      text::print(mark, text::kDefaultFont, kMapLast, i, kVert.codes);
   }
   text::print(mark, text::kDefaultFont, 0, 0, kCorner.codes);
   text::print(mark, text::kDefaultFont, kMapLast, 0, kCorner.codes);
   text::print(mark, text::kDefaultFont, 0, kMapLast, kCorner.codes);
   text::print(mark, text::kDefaultFont, kMapLast, kMapLast, kCorner.codes);
   text::print(mark, text::kDefaultFont, kEdgeTextCol, 0, kTopEdge.codes);
   text::print(mark, text::kDefaultFont, kEdgeTextCol, kMapLast, kBottomEdge.codes);

   // Scrolling up by kLayoutTopRow rows (wrapped into the 512-pixel range) puts the map's top on that row.
   g_scroll_x = 0;
   g_scroll_y = static_cast<int16_t>(-kLayoutTopRow * kTilePixels & kScrollMask);
   video::set_bg_scroll(kLayoutBg, g_scroll_x, g_scroll_y);
   controls(kLayoutHint1.codes, nullptr);
   draw_scroll();
}

/// Page 3 controls: scroll BG1.
/// @param pad  this frame's buttons
void update_layout(const Pad& pad)
{
   bool moved = false;
   if (pad.down(hal::input::KEY_LEFT))
   {
      g_scroll_x = static_cast<int16_t>((g_scroll_x - 1) & kScrollMask);
      moved = true;
   }
   if (pad.down(hal::input::KEY_RIGHT))
   {
      g_scroll_x = static_cast<int16_t>((g_scroll_x + 1) & kScrollMask);
      moved = true;
   }
   if (pad.down(hal::input::KEY_UP))
   {
      g_scroll_y = static_cast<int16_t>((g_scroll_y - 1) & kScrollMask);
      moved = true;
   }
   if (pad.down(hal::input::KEY_DOWN))
   {
      g_scroll_y = static_cast<int16_t>((g_scroll_y + 1) & kScrollMask);
      moved = true;
   }
   if (moved)
   {
      video::set_bg_scroll(kLayoutBg, g_scroll_x, g_scroll_y);
      draw_scroll();
   }
}

// --- pages ------------------------------------------------------------------

/// One page of the app.
struct Page
{
   const text::Code* name;   ///< Shown in the title row.
   void (*enter)();   ///< Draws the page.
   void (*update)(const Pad&);   ///< Runs each frame while the page is shown.
};

/// Update for pages without controls.
void no_update(const Pad&)
{
}

/// The pages, in order.
const Page kPages[] = {
   {kGlyphsName.codes, enter_glyphs, no_update},
   {kInksName.codes, enter_inks, no_update},
   {kLayoutName.codes, enter_layout, update_layout},
};
constexpr int kNumPages = sizeof(kPages) / sizeof(kPages[0]);   ///< Page count.

/// Switches to a page.
/// @param page  0-based page
void enter(int page)
{
   clear_console();
   hide_layout_layer();
   title(kApp.codes, page, kNumPages, kPages[page].name);
   kPages[page].enter();
}

}   // namespace

/// Runs the text test until the platform quits.
/// @return 0
int main()
{
   hal::system::init();
   init_console();

   int page = 0;
   enter(page);
   Pad pad;
   while (hal::system::update())
   {
      pad.update();
      if (pad.hit(hal::input::KEY_R))
      {
         page = (page + 1) % kNumPages;
         enter(page);
         continue;
      }
      if (pad.hit(hal::input::KEY_L))
      {
         page = (page + kNumPages - 1) % kNumPages;
         enter(page);
         continue;
      }
      kPages[page].update(pad);
   }
   return 0;
}
