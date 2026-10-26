/// @file
/// Video test: the background layer model on its own (hal::video), page by
/// page, with no game code. L / R change page.
///   1 Palettes  BG palette banks 4-15, all 16 entries (entry 0 = transparent)
///   2 Flips     one tile and one 2x2 block in every flip combination
///   3 Layers    BG1-BG3 overlapping: scroll, priority, on/off per layer
///   4 Map size  all four screen sizes, scrolling across screen-block seams
/// The console (BG0, test_kit.h) is always on top.
#include "test_kit.h"

#include "capsule/core/types.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace
{

using namespace capsule;
using namespace capsule::testkit;
namespace video = hal::video;
using hal::input::Key;

constexpr uint8_t kTileBlock = 1;                  ///< Character block of the generated pattern tiles.
constexpr uint8_t kSizeBlockBase = 24;             ///< Page 4: up to four screen blocks 24-27.
constexpr uint8_t kLayerBlocks[] = {28, 29, 30};   ///< Page 3: screen blocks of BG1, BG2, BG3.

constexpr int kBitsPerPixel = 4;                  ///< Bits of one 4bpp pixel.
constexpr uint32_t kNibbleRepeat = 0x11111111u;   ///< Times a 4-bit color fills all 8 pixels of a 4bpp row.
constexpr int kQuadrants = 4;                     ///< Tiles of a 2x2 block.
constexpr int kQuadTopRight = 1;                  ///< Quadrant index of the top-right tile.
constexpr int kQuadBottomLeft = 2;                ///< Quadrant index of the bottom-left tile.
constexpr int kQuadBottomRight = 3;               ///< Quadrant index of the bottom-right tile.
constexpr int kOneCharLength = 2;                 ///< One character and its terminator.
constexpr text::Code kFirstGlyphCode = 0x20;      ///< Character code of the font's glyph 0 (space).

/// @name Shared page layout
///@{
constexpr int kHeaderRow = 2;   ///< Row of a page's column headers or first label.
constexpr int kNoteRow = 16;    ///< Row of a page's note.
///@}

/// Pattern tiles in character block 1.
enum Tile : video::TileIndex
{
   TILE_EMPTY = 0,      ///< Transparent.
   TILE_SOLID = 1,      ///< 1-15: solid color index 1-15.
   TILE_ARROW = 16,     ///< Asymmetric 8x8 arrow, colors 1, 5, 9.
   TILE_CHECKER = 17,   ///< 4x4 checker of color 1 and transparent.
   TILE_STRIPES = 18,   ///< 2-pixel horizontal stripes of color 2.
   TILE_FRAME = 19,     ///< 1-pixel frame of color 3, transparent inside.
   TILE_BIG = 20,       ///< 20-23: one 16x16 "F", quadrants TL, TR, BL, BR.
   NUM_TILES = 24       ///< Tiles generated.
};

// --- tile generation --------------------------------------------------------

/// One 4bpp tile row from a pattern string: pixel 0 is the low nibble
/// (leftmost on screen).
/// @param pixels  8 characters: '1'-'9' = color index, anything else = 0
/// @return the row
uint32_t row_of(const char* pixels)
{
   uint32_t r = 0;
   for (int x = 0; x < kTilePixels; ++x)
   {
      const char c = pixels[x];
      const uint32_t v = (c >= '1' && c <= '9') ? static_cast<uint32_t>(c - '0') : 0u;
      r |= v << (kBitsPerPixel * x);
   }
   return r;
}

/// Loads one tile from 8 pattern rows (see row_of()).
/// @param tile  tile index in kTileBlock
/// @param rows  8 pattern strings
void load_pattern(video::TileIndex tile, const char* const rows[kTilePixels])
{
   uint32_t t[kTilePixels];
   for (int y = 0; y < kTilePixels; ++y)
   {
      t[y] = row_of(rows[y]);
   }
   video::load_bg_tiles(kTileBlock, tile, t, 1);
}

/// Generates and loads every Tile.
void load_tiles()
{
   uint32_t t[kTilePixels];
   for (int i = 0; i < kTilePixels; ++i)
   {
      t[i] = 0;
   }
   video::load_bg_tiles(kTileBlock, TILE_EMPTY, t, 1);
   for (uint32_t c = 1; c < kColorsPerBank; ++c)
   {
      for (int i = 0; i < kTilePixels; ++i)
      {
         t[i] = c * kNibbleRepeat;
      }
      video::load_bg_tiles(kTileBlock, static_cast<video::TileIndex>(TILE_SOLID + c - 1), t, 1);
   }
   static const char* const kArrow[kTilePixels] = {
      "...1....", "..111...", ".11111..", "1.555.1.", "..555...", "..555...", "..59999.", "..5.....",
   };
   static const char* const kChecker[kTilePixels] = {
      "1111....", "1111....", "1111....", "1111....", "....1111", "....1111", "....1111", "....1111",
   };
   static const char* const kStripes[kTilePixels] = {
      "22222222", "22222222", "........", "........", "22222222", "22222222", "........", "........",
   };
   static const char* const kFrame[kTilePixels] = {
      "33333333", "3......3", "3......3", "3......3", "3......3", "3......3", "3......3", "33333333",
   };
   // 16x16 "F" with a dot in the lower right, split into four tiles.
   static const char* const kBigTL[kTilePixels] = {
      "........", ".1111111", ".1111111", ".11.....", ".11.....", ".11.....", ".111111.", ".111111.",
   };
   static const char* const kBigTR[kTilePixels] = {
      "........", "111111..", "111111..", "........", "........", "........", "........", "........",
   };
   static const char* const kBigBL[kTilePixels] = {
      ".11.....", ".11.....", ".11.....", ".11.....", ".11.....", ".11.....", ".11.....", "........",
   };
   static const char* const kBigBR[kTilePixels] = {
      "........", "........", "........", "........", "....99..", "....99..", "........", "........",
   };
   load_pattern(TILE_ARROW, kArrow);
   load_pattern(TILE_CHECKER, kChecker);
   load_pattern(TILE_STRIPES, kStripes);
   load_pattern(TILE_FRAME, kFrame);
   load_pattern(TILE_BIG + 0, kBigTL);
   load_pattern(TILE_BIG + kQuadTopRight, kBigTR);
   load_pattern(TILE_BIG + kQuadBottomLeft, kBigBL);
   load_pattern(TILE_BIG + kQuadBottomRight, kBigBR);
}

constexpr int kAppBanks = 12;         ///< Palette banks free for the app (4-15).
constexpr int kChannelMask = 31;      ///< One 5-bit channel of a color.
constexpr int kGreenShift = 5;        ///< Position of green in a color.
constexpr int kBlueShift = 10;        ///< Position of blue in a color.
constexpr int kBrightestLevel = 16;   ///< Brightness of entry 1, in 16ths.

/// Loads banks 4-15: one hue each, entries 1-15 from full brightness down to
/// 1/16, so low entries (used by the 2-color patterns and the font) are bright.
void load_palettes()
{
   constexpr video::Color kHue[kAppBanks] = {
      video::rgb5(31, 0, 0), video::rgb5(31, 16, 0), video::rgb5(31, 31, 0), video::rgb5(16, 31, 0),
      video::rgb5(0, 31, 0), video::rgb5(0, 31, 16), video::rgb5(0, 31, 31), video::rgb5(0, 16, 31),
      video::rgb5(0, 0, 31), video::rgb5(16, 0, 31), video::rgb5(31, 0, 31), video::rgb5(31, 31, 31),
   };
   for (int b = 0; b < kAppBanks; ++b)
   {
      video::Color bank[kColorsPerBank];
      bank[0] = 0;   // never shown: entry 0 is transparent

      // Split the hue into its 5-bit channels.
      const int r = kHue[b] & kChannelMask;
      const int g = (kHue[b] >> kGreenShift) & kChannelMask;
      const int bl = (kHue[b] >> kBlueShift) & kChannelMask;
      for (int i = 1; i < kColorsPerBank; ++i)
      {
         const int level = kBrightestLevel + 1 - i;   // 16/16 .. 2/16
         bank[i] = video::rgb5(static_cast<uint8_t>(r * level / kBrightestLevel),
                               static_cast<uint8_t>(g * level / kBrightestLevel),
                               static_cast<uint8_t>(bl * level / kBrightestLevel));
      }
      video::load_bg_palette(static_cast<uint8_t>((kFirstAppBank + b) * kColorsPerBank), bank, kColorsPerBank);
   }
}

/// Fills the top-left w x h cells of a layer.
/// @param bg    layer
/// @param w     columns
/// @param h     rows
/// @param cell  map entry
void fill(video::BgIndex bg, int w, int h, video::MapCell cell)
{
   for (int y = 0; y < h; ++y)
   {
      for (int x = 0; x < w; ++x)
      {
         video::write_map_cell(bg, x, y, cell);
      }
   }
}

constexpr uint8_t kLowestPriority = 3;   ///< BG priority drawn behind the others.

/// Turns BG1-BG3 off and resets their scroll.
void hide_app_layers()
{
   for (video::BgIndex bg = 1; bg < video::kNumBgs; ++bg)
   {
      video::configure_bg(bg, false, kLowestPriority, kTileBlock, kLayerBlocks[bg - 1], false, 0);
      video::set_bg_scroll(bg, 0, 0);
   }
}

constexpr auto kApp = text::encode("VIDEO TEST");         ///< App title.
constexpr auto kPageHint = text::encode("L/R: page");    ///< Controls hint.

// --- page 1: palettes -------------------------------------------------------

constexpr auto kPalettesName = text::encode("Palettes");                    ///< Page name.
constexpr auto kPalettesNote = text::encode("Entry 0 shows the backdrop");  ///< Page note.

constexpr int kSwatchCol = 4;    ///< Column of entry 0's swatch.
constexpr int kSwatchTop = 3;    ///< Row of the first bank's swatches.
constexpr int kBankDigits = 2;   ///< Width of a bank number.

/// Page 1: a swatch for every entry of banks 4-15.
void enter_palettes()
{
   video::configure_bg(1, true, 1, kTileBlock, kLayerBlocks[0], false, 0);
   fill(1, kMapCells, kMapCells, video::map_cell(TILE_EMPTY, 0));
   constexpr char kHex[] = "0123456789ABCDEF";
   for (int i = 0; i < kColorsPerBank; ++i)
   {
      const char s[kOneCharLength] = {kHex[i], '\0'};
      put_ascii(INK_DIM, kSwatchCol + i, kHeaderRow, s);
   }
   for (int b = 0; b < kAppBanks; ++b)
   {
      const int y = kSwatchTop + b;
      const auto bank = static_cast<uint8_t>(kFirstAppBank + b);
      put_ascii(INK_DIM, 0, y, "B");
      put_uint(INK_DIM, 1, y, bank, kBankDigits);
      for (int i = 1; i < kColorsPerBank; ++i)
      {
         video::write_map_cell(1, kSwatchCol + i, y,
                               video::map_cell(static_cast<video::TileIndex>(TILE_SOLID + i - 1), bank));
      }
   }
   put(INK_DIM, 0, kNoteRow, kPalettesNote.codes);
   controls(kPageHint.codes, nullptr);
}

// --- page 2: flips ----------------------------------------------------------

/// @name Page 2 text
///@{
constexpr auto kFlipsName = text::encode("Flips");   ///< Screen text "Flips".
constexpr auto kFlipNone = text::encode("none");   ///< Screen text "none".
constexpr auto kFlipH = text::encode("H");   ///< Screen text "H".
constexpr auto kFlipV = text::encode("V");   ///< Screen text "V".
constexpr auto kFlipHV = text::encode("HV");   ///< Screen text "HV".
constexpr auto kTileRow = text::encode("8x8 tile:");   ///< Screen text "8x8 tile:".
constexpr auto kBlockRow =
   text::encode("2x2 block (cells swap too):");   ///< Screen text "2x2 block (cells swap too):".
constexpr auto kFlipsNote =
   text::encode("Each row must mirror cleanly");   ///< Screen text "Each row must mirror cleanly".
///@}

/// @name Page 2 layout
///@{
constexpr int kFlipCombos = 4;      ///< Flip combinations: none, H, V, HV (bit 0 = H, bit 1 = V).
constexpr int kFlipVBit = 2;        ///< Bit of a combination that flips vertically.
constexpr int kFlipCol0 = 2;        ///< Column of the first combination.
constexpr int kFlipColStep = 7;     ///< Columns between combinations.
constexpr int kFlipBankBase = 2;    ///< App bank of the first combination.
constexpr int kFlipBankStep = 2;    ///< App banks between combinations.
constexpr int kArrowRow = 4;        ///< Row of the arrow tiles.
constexpr int kArrowNameRow = 5;    ///< Row of their names.
constexpr int kBlockLabelRow = 7;   ///< Row of the 2x2 block label.
constexpr int kBlockTop = 9;        ///< Top row of the 2x2 blocks.
constexpr int kBlockNameRow = 12;   ///< Row of their names.
constexpr int kFlipsNoteRow = 15;   ///< Row of the page note.
///@}

/// Page 2: the arrow tile and the 2x2 "F" in every flip combination.
void enter_flips()
{
   video::configure_bg(1, true, 1, kTileBlock, kLayerBlocks[0], false, 0);
   fill(1, kMapCells, kMapCells, video::map_cell(TILE_EMPTY, 0));
   const text::Code* const names[kFlipCombos] = {kFlipNone.codes, kFlipH.codes, kFlipV.codes, kFlipHV.codes};
   put(INK_DIM, 0, kHeaderRow, kTileRow.codes);
   put(INK_DIM, 0, kBlockLabelRow, kBlockRow.codes);
   for (int f = 0; f < kFlipCombos; ++f)
   {
      const bool h = (f & 1) != 0;
      const bool v = (f & kFlipVBit) != 0;
      const int x = kFlipCol0 + f * kFlipColStep;
      const auto bank = static_cast<uint8_t>(kFirstAppBank + kFlipBankBase + f * kFlipBankStep);
      video::write_map_cell(1, x, kArrowRow, video::map_cell(TILE_ARROW, bank, h, v));
      put(INK_DIM, x, kArrowNameRow, names[f]);
      // A flipped 2x2 block swaps its cells as well as flipping each one.
      for (int q = 0; q < kQuadrants; ++q)
      {
         const int qx = q & 1;
         const int qy = q >> 1;
         const int cx = x + (h ? 1 - qx : qx);
         const int cy = kBlockTop + (v ? 1 - qy : qy);
         video::write_map_cell(1, cx, cy, video::map_cell(static_cast<video::TileIndex>(TILE_BIG + q), bank, h, v));
      }
      put(INK_DIM, x, kBlockNameRow, names[f]);
   }
   put(INK_DIM, 0, kFlipsNoteRow, kFlipsNote.codes);
   controls(kPageHint.codes, nullptr);
}

// --- page 3: layers ---------------------------------------------------------

/// @name Page 3 text
///@{
constexpr auto kLayersName = text::encode("Layers");   ///< Screen text "Layers".
constexpr auto kLayersHint1 = text::encode("L/R page  D-pad scroll");   ///< Screen text "L/R page  D-pad scroll".
constexpr auto kLayersHint2 =
   text::encode("A layer  B priority  START on");   ///< Screen text "A layer  B priority  START on".
constexpr auto kOn = text::encode("on ");   ///< Screen text "on ".
constexpr auto kOff = text::encode("off");   ///< Screen text "off".
///@}

/// Page 3 settings of one layer.
struct Layer
{
   bool on;            ///< Shown.
   uint8_t priority;   ///< 0-3.
   int16_t x;          ///< Scroll x.
   int16_t y;          ///< Scroll y.
};
constexpr int kAppLayers = 3;   ///< Layers the page controls: BG1-BG3.

/// @name Page 3 layout
///@{
constexpr int kLayerTop = 3;                           ///< Row of BG1's line in the table.
constexpr int kBgNumCol = 3;                           ///< Column of the layer number.
constexpr int kOnCol = 5;                              ///< Column of on / off.
constexpr int kPriCol = 9;                             ///< Column of the priority.
constexpr int kPriDigits = 3;                          ///< Width of the priority.
constexpr int kXCol = 13;                              ///< Column of the scroll x.
constexpr int kYCol = 19;                              ///< Column of the scroll y.
constexpr int kScrollDigits = 6;                       ///< Width of a scroll value.
constexpr int kXHeadCol = kXCol + kScrollDigits - 1;   ///< "x" over the last digit of scroll x.
constexpr int kYHeadCol = kYCol + kScrollDigits - 1;   ///< "y" over the last digit of scroll y.
constexpr int kPanelCols = 12;                         ///< Columns of a layer's panel.
constexpr int kPanelRows = 6;                          ///< Rows of a layer's panel.
constexpr int kPriorityMask = 3;                       ///< BG priorities 0-3.
///@}

Layer g_layers[kAppLayers];   ///< BG1-BG3.
int g_selected = 0;   ///< Layer the controls apply to, 0-2.

/// Applies one layer's settings to the HAL.
/// @param i  0-2 for BG1-BG3
void apply_layer(int i)
{
   const auto bg = static_cast<video::BgIndex>(i + 1);
   const Layer& l = g_layers[i];
   video::configure_bg(bg, l.on, l.priority, kTileBlock, kLayerBlocks[i], false, 0);
   video::set_bg_scroll(bg, l.x, l.y);
}

/// Draws the layer table.
void draw_layers()
{
   for (int i = 0; i < kAppLayers; ++i)
   {
      const int y = kLayerTop + i;
      const Layer& l = g_layers[i];
      const Ink ink = i == g_selected ? INK_ON : INK_DIM;
      blank(0, kCols, y);
      put_ascii(ink, 0, y, i == g_selected ? ">BG" : " BG");
      put_uint(ink, kBgNumCol, y, static_cast<uint32_t>(i + 1));
      put(l.on ? ink : INK_OFF, kOnCol, y, l.on ? kOn.codes : kOff.codes);
      put_uint(ink, kPriCol, y, l.priority, kPriDigits);
      put_int(ink, kXCol, y, l.x, kScrollDigits);
      put_int(ink, kYCol, y, l.y, kScrollDigits);
   }
}

/// Page 3: three overlapping layers.
void enter_layers()
{
   // Three overlapping 12x6 panels, so priority changes show where they meet.
   struct Panel
   {
      video::MapCell cell;
      int x, y;
   };
   constexpr Panel kPanels[kAppLayers] = {
      {video::map_cell(TILE_CHECKER, kFirstAppBank + 0), 2, 7},    // BG1: red checker
      {video::map_cell(TILE_STRIPES, kFirstAppBank + 4), 8, 9},    // BG2: green stripes
      {video::map_cell(TILE_FRAME, kFirstAppBank + 8), 14, 11},    // BG3: blue frames
   };
   for (int i = 0; i < kAppLayers; ++i)
   {
      const auto bg = static_cast<video::BgIndex>(i + 1);
      g_layers[i] = Layer{true, static_cast<uint8_t>(i + 1), 0, 0};
      apply_layer(i);
      fill(bg, kMapCells, kMapCells, video::map_cell(TILE_EMPTY, 0));
      for (int y = 0; y < kPanelRows; ++y)
      {
         for (int x = 0; x < kPanelCols; ++x)
         {
            video::write_map_cell(bg, kPanels[i].x + x, kPanels[i].y + y, kPanels[i].cell);
         }
      }
   }
   g_selected = 0;
   put_ascii(INK_TITLE, kOnCol, kHeaderRow, "on");
   put_ascii(INK_TITLE, kPriCol, kHeaderRow, "pri");
   put_ascii(INK_TITLE, kXHeadCol, kHeaderRow, "x");
   put_ascii(INK_TITLE, kYHeadCol, kHeaderRow, "y");
   draw_layers();
   controls(kLayersHint1.codes, kLayersHint2.codes);
}

/// Page 3 controls: scroll, select, priority, on/off.
/// @param pad  this frame's buttons
void update_layers(const Pad& pad)
{
   Layer& l = g_layers[g_selected];
   bool changed = false;
   if (pad.down(hal::input::KEY_LEFT))
   {
      l.x = static_cast<int16_t>((l.x + 1) & kScrollMask);
      changed = true;
   }
   if (pad.down(hal::input::KEY_RIGHT))
   {
      l.x = static_cast<int16_t>((l.x - 1) & kScrollMask);
      changed = true;
   }
   if (pad.down(hal::input::KEY_UP))
   {
      l.y = static_cast<int16_t>((l.y + 1) & kScrollMask);
      changed = true;
   }
   if (pad.down(hal::input::KEY_DOWN))
   {
      l.y = static_cast<int16_t>((l.y - 1) & kScrollMask);
      changed = true;
   }
   if (pad.hit(hal::input::KEY_B))
   {
      l.priority = static_cast<uint8_t>((l.priority + 1) & kPriorityMask);
      changed = true;
   }
   if (pad.hit(hal::input::KEY_START))
   {
      l.on = !l.on;
      changed = true;
   }
   if (pad.hit(hal::input::KEY_A))
   {
      g_selected = (g_selected + 1) % kAppLayers;
      changed = true;
   }
   if (changed)
   {
      apply_layer(g_selected);
      draw_layers();
   }
}

// --- page 4: map sizes ------------------------------------------------------

/// @name Page 4 text
///@{
constexpr auto kSizesName = text::encode("Map size");   ///< Screen text "Map size".
constexpr auto kSizesHint1 =
   text::encode("L/R page  D-pad scroll (A fast)");   ///< Screen text "L/R page  D-pad scroll (A fast)".
constexpr auto kSizesHint2 = text::encode("B next size");   ///< Screen text "B next size".
constexpr auto kSizeLbl = text::encode("BG1 size");   ///< Screen text "BG1 size".
constexpr auto kScrollLbl = text::encode("scroll");   ///< Screen text "scroll".
constexpr auto kSizesNote = text::encode("Digit = screen block 24+n");   ///< Screen text "Digit = screen block 24+n".
///@}
/// @name Page 4 screen sizes and layout
/// Size code bit 0 doubles the width, bit 1 the height.
///@{
constexpr int kNumSizes = 4;   ///< Screen size codes.
constexpr uint8_t kSizeWide = 1;   ///< 512x256: two screen blocks side by side.
constexpr uint8_t kSizeTall = 2;   ///< 256x512: two screen blocks stacked.
constexpr uint8_t kSizeBig = 3;   ///< 512x512: four screen blocks.
constexpr uint8_t kSizeMask = 3;   ///< Size codes 0-3.
constexpr int kBigMapCells = 64;   ///< Cells along a doubled map side.
constexpr int kBlocksPerRow = 2;   ///< Screen blocks per row of a 512x512 map.
constexpr int kLabelSpacing = 4;   ///< A block digit every 4 cells...
constexpr int kLabelOffset = 2;   ///< ...starting 2 cells in.
constexpr int kBankStepPerBlock = 3;   ///< App banks between the colors of adjacent screen blocks.
constexpr int kSizeRow = 2;   ///< Row of the size readout.
constexpr int kSizeScrollRow = 3;   ///< Row of the scroll readout.
constexpr int kSizeScrollDigits = 4;   ///< Width of a scroll value.
constexpr int kFastStep = 8;   ///< Scroll step with A held.
///@}

/// Screen size names by size code.
const char* const kSizeNames[kNumSizes] = {"256x256", "512x256", "256x512", "512x512"};

uint8_t g_size = 0;   ///< Current screen size code, 0-3.
int16_t g_size_x = 0;   ///< BG1 scroll x.
int16_t g_size_y = 0;   ///< BG1 scroll y.

/// Draws the size and scroll readout.
void draw_sizes()
{
   blank(0, kCols, kSizeRow);
   int x = put(INK_DIM, 0, kSizeRow, kSizeLbl.codes);
   put_ascii(INK_ON, x + 1, kSizeRow, kSizeNames[g_size]);
   blank(0, kCols, kSizeScrollRow);
   x = put(INK_DIM, 0, kSizeScrollRow, kScrollLbl.codes);
   x = put_int(INK_ON, x + 1, kSizeScrollRow, g_size_x, kSizeScrollDigits);
   put_int(INK_ON, x + 1, kSizeScrollRow, g_size_y, kSizeScrollDigits);
}

/// Configures BG1 for g_size and fills its whole map.
void apply_size()
{
   // Font tiles (character block 0) so every cell can show its screen block.
   video::configure_bg(1, true, 1, kConsoleCharBlock, kSizeBlockBase, false, g_size);
   video::set_bg_scroll(1, g_size_x, g_size_y);
   const int w = (g_size & kSizeWide) ? kBigMapCells : kMapCells;
   const int h = (g_size & kSizeTall) ? kBigMapCells : kMapCells;
   for (int y = 0; y < h; ++y)
   {
      for (int x = 0; x < w; ++x)
      {
         // Which screen block the cell is in: blocks run left to right, then top to bottom.
         int block = 0;
         if (g_size == kSizeWide)
         {
            block = x / kMapCells;
         }
         if (g_size == kSizeTall)
         {
            block = y / kMapCells;
         }
         if (g_size == kSizeBig)
         {
            block = x / kMapCells + kBlocksPerRow * (y / kMapCells);
         }
         const bool edge = (x % kMapCells == 0) || (y % kMapCells == 0) || (x % kMapCells == kMapCells - 1) ||
                           (y % kMapCells == kMapCells - 1);
         const bool label = (x % kLabelSpacing == kLabelOffset) && (y % kLabelSpacing == kLabelOffset);
         const text::Code c = edge ? '#' : (label ? static_cast<text::Code>('0' + block) : ' ');
         // Font glyph tile = code - 0x20; banks 4, 7, 10, 13 per block.
         video::write_map_cell(1, x, y,
                               video::map_cell(static_cast<video::TileIndex>(c - kFirstGlyphCode),
                                               static_cast<uint8_t>(kFirstAppBank + kBankStepPerBlock * block)));
      }
   }
   draw_sizes();
}

/// Page 4: one layer in each screen size.
void enter_sizes()
{
   g_size = 0;
   g_size_x = 0;
   g_size_y = 0;
   apply_size();
   put(INK_DIM, 0, kNoteRow, kSizesNote.codes);
   controls(kSizesHint1.codes, kSizesHint2.codes);
}

/// Page 4 controls: scroll, next size.
/// @param pad  this frame's buttons
void update_sizes(const Pad& pad)
{
   const int step = pad.down(hal::input::KEY_A) ? kFastStep : 1;
   bool moved = false;
   if (pad.down(hal::input::KEY_LEFT))
   {
      g_size_x = static_cast<int16_t>((g_size_x - step) & kScrollMask);
      moved = true;
   }
   if (pad.down(hal::input::KEY_RIGHT))
   {
      g_size_x = static_cast<int16_t>((g_size_x + step) & kScrollMask);
      moved = true;
   }
   if (pad.down(hal::input::KEY_UP))
   {
      g_size_y = static_cast<int16_t>((g_size_y - step) & kScrollMask);
      moved = true;
   }
   if (pad.down(hal::input::KEY_DOWN))
   {
      g_size_y = static_cast<int16_t>((g_size_y + step) & kScrollMask);
      moved = true;
   }
   if (pad.hit(hal::input::KEY_B))
   {
      g_size = static_cast<uint8_t>((g_size + 1) & kSizeMask);
      apply_size();
   }
   else if (moved)
   {
      video::set_bg_scroll(1, g_size_x, g_size_y);
      draw_sizes();
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
   {kPalettesName.codes, enter_palettes, no_update},
   {kFlipsName.codes, enter_flips, no_update},
   {kLayersName.codes, enter_layers, update_layers},
   {kSizesName.codes, enter_sizes, update_sizes},
};
constexpr int kNumPages = sizeof(kPages) / sizeof(kPages[0]);   ///< Page count.

/// Switches to a page.
/// @param page  0-based page
void enter(int page)
{
   clear_console();
   hide_app_layers();
   title(kApp.codes, page, kNumPages, kPages[page].name);
   kPages[page].enter();
}

}   // namespace

/// Runs the video test until the platform quits.
/// @return 0
int main()
{
   hal::system::init();
   init_console();
   load_tiles();
   load_palettes();

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
