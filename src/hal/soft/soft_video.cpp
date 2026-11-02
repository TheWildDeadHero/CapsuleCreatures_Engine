/// @file
/// Software GBA video model: hal::video over emulated registers, VRAM, OAM and
/// palettes, plus the compositor (see soft_video.h).
///
/// The compositor follows the GBA's rules per pixel: the window region picks
/// which layers and whether color effects show; the two front-most visible
/// layers (by priority, sprites before backgrounds of equal priority, then
/// lower BG number) are the candidates for blending; semi-transparent sprites
/// alpha-blend over any second target regardless of the blend mode.
#include "soft_video.h"

#include "capsule/core/assert.h"
#include "capsule/core/endian.h"
#include "capsule/core/types.h"
#include "capsule/hal/vram_queue.h"

namespace capsule::hal::soft
{
namespace
{

using namespace video;   // the GBA model's layout constants

constexpr uint32_t kVramBytes = video::kBgVramBytes;      ///< BG VRAM: char blocks 0-3 / screen blocks 0-31.
constexpr uint32_t kObjVramBytes = video::kObjVramBytes;  ///< OBJ VRAM: 1024 4bpp tiles.
constexpr uint16_t kDispcntModeMask = 0x0007;     ///< DISPCNT display mode bits.
constexpr uint16_t kDispcntForcedBlank = 0x0080;  ///< DISPCNT forced-blank bit.
constexpr uint16_t kDispcntBg0 = 0x0100;          ///< DISPCNT BG0 enable; BGn is this << n.
constexpr uint16_t kDispcntObj = 0x1000;          ///< DISPCNT sprite layer enable.
constexpr uint16_t kDispcntWin0 = 0x2000;         ///< DISPCNT window 0 enable.
constexpr uint16_t kDispcntWin1 = 0x4000;         ///< DISPCNT window 1 enable.
constexpr uint16_t kDispcntObjWin = 0x8000;       ///< DISPCNT OBJ window enable.
constexpr uint16_t kDispcntHblankFree = 0x0020;   ///< FX2: DISPCNT H-blank interval free (a shorter OBJ budget).
constexpr uint16_t kForcedBlankColor = 0x7FFF;    ///< Forced blank shows white.

/// @name BGnCNT fields
///@{
constexpr int kBgcntCharBlockShift = 2;     ///< Bits 2-3: character block.
constexpr int kBgcntCharBlockMask = 3;      ///< Mask of the character block field.
constexpr int kBgcnt8bpp = 0x80;            ///< Bit 7: 8bpp tiles.
constexpr int kBgcntScreenBlockShift = 8;   ///< Bits 8-12: first screen block.
constexpr int kBgcntScreenBlockMask = 31;   ///< Mask of the screen block field.
constexpr int kBgcntWrap = 0x2000;          ///< Bit 13: affine layer wraps around.
constexpr int kBgcntSizeShift = 14;         ///< Bits 14-15: screen size.
constexpr int kBgcntSizeMask = 3;           ///< Mask of the screen size field.
///@}

/// @name BLDCNT fields (bits 0-5: first-target layer mask)
///@{
constexpr int kBldcntModeShift = 6;     ///< Bits 6-7: BlendMode.
constexpr int kBldcntModeMask = 3;      ///< Mask of the mode field.
constexpr int kBldcntSecondShift = 8;   ///< Bits 8-13: second-target layer mask.
///@}

/// @name Background geometry
///@{
constexpr int kTextBgPixels = 256;        ///< Side of a regular map of one screen block per axis.
constexpr int kTextBgLargePixels = 512;   ///< Side of a regular map of two screen blocks per axis.
constexpr int kTextBgLargeTiles = 64;     ///< Cells per side of a regular map two screen blocks long.
constexpr uint32_t kWideBlocks = 2;       ///< Screen blocks across a wide regular map.
constexpr int kAffineBgMinPixels = 128;   ///< Side of the smallest affine map (size 0); each size doubles it.
constexpr int kAffineBgMinCells = 16;     ///< Cells per side of the smallest affine map.
constexpr int kTileRowBytes4bpp = 4;      ///< Bytes in one 8-pixel row of a 4bpp tile.
constexpr int kNibbleBits = 4;            ///< Bits per 4bpp pixel; the high nibble is the right pixel.
constexpr int kNibbleMask = 15;           ///< Mask of one 4bpp pixel.
///@}

/// @name Sprites, windows and blending
///@{
constexpr int kObjXNegative = 256;    ///< attr1 x values from here up are negative (9-bit signed).
///@}

/// @name FX2 (M5): the per-line OBJ render budget (the GBA's: its OBJ unit draws a line's sprites in OAM order until
/// the line's cycles run out; the model is mGBA's, the reference the GBA builds' pictures come from)
///@{
constexpr int kObjLineCycles = 1210;           ///< Cycles a line gives the OBJ unit ...
constexpr int kObjLineCyclesHblankFree = 954;  ///< ... or with DISPCNT's H-blank interval free.
constexpr int kObjEntryCycles = 2;             ///< Cycles per OAM entry the unit steps over.
constexpr int kAffineObjCycles = 8;            ///< An affine sprite on the line: 8 + 2 per pixel of its box width.
constexpr int kAffineObjPixelCycles = 2;       ///< ...
constexpr int kNormalObjCredit = 2;            ///< A normal sprite: its width less 2 ...
constexpr int kNormalObjLeftShift = 1;         ///< ... less half of what lies left of the screen.
constexpr int kObjLinesTotal = 228;            ///< Lines of a frame (visible and blank): a sprite below the screen
                                               ///< that does not wrap back is not listed.
///@}

/// @name Sprites, windows and blending (continued)
///@{
constexpr int kNoPriority = 4;        ///< Sprite line priority where no sprite pixel is (behind priority 3).
constexpr int kDoubleSize = 2;        ///< An affine sprite with double_size set draws in a box twice its size.
constexpr int kHalf = 2;              ///< Halves a box size: its centre.
constexpr int kParamA = 0;            ///< Index of pa in an affine matrix.
constexpr int kParamB = 1;            ///< Index of pb in an affine matrix.
constexpr int kParamC = 2;            ///< Index of pc in an affine matrix.
constexpr int kParamD = 3;            ///< Index of pd in an affine matrix.
constexpr int kRectEdges = 4;         ///< Edges of a window rectangle.
constexpr int kRectX0 = 0;            ///< win_rect index: left.
constexpr int kRectY0 = 1;            ///< win_rect index: top.
constexpr int kRectX1 = 2;            ///< win_rect index: right (exclusive).
constexpr int kRectY1 = 3;            ///< win_rect index: bottom (exclusive).
constexpr int kWindowRegions = 4;     ///< Window regions with a mask.
constexpr uint16_t kWindowEdgeMask = 0xFF;   ///< N2: a scanline entry's right edge bits.
constexpr int kMaskIn0 = 0;           ///< win_mask index: inside window 0.
constexpr int kMaskIn1 = 1;           ///< win_mask index: inside window 1.
constexpr int kMaskOutside = 2;       ///< win_mask index: outside every window.
constexpr int kMaskObj = 3;           ///< win_mask index: inside the OBJ window.
constexpr int kBlendShift = 4;        ///< Divides by 16 (kBlendMax) after weighting.
constexpr int kBlendLayers = 2;       ///< Front-most layers a pixel blends: first and second target.
constexpr int kHalfwordBits = 16;     ///< Bits in a halfword: the high half of a word.
///@}

/// @name BF: register writes per scanline (I/O offsets, video::line_register_offset())
///@{
constexpr uint32_t kIoBgcnt0 = 0x08;      ///< BG0CNT; BGnCNT is 2n after it.
constexpr uint32_t kIoHofs0 = 0x10;       ///< BG0HOFS; BGnHOFS is 4n after it, BGnVOFS 4n + 2.
constexpr uint32_t kIoBg2X = 0x28;        ///< BG2X (low half; the high half is 2 after it).
constexpr uint32_t kIoBg2Y = 0x2C;        ///< BG2Y.
constexpr uint32_t kIoBg3X = 0x38;        ///< BG3X.
constexpr uint32_t kIoBg3Y = 0x3C;        ///< BG3Y.
constexpr uint32_t kIoWin0h = 0x40;       ///< WIN0H.
constexpr uint32_t kIoWin1h = 0x42;       ///< WIN1H.
constexpr uint32_t kIoBldcnt = 0x50;      ///< BLDCNT.
constexpr uint32_t kIoBldalpha = 0x52;    ///< BLDALPHA.
constexpr uint32_t kIoBldy = 0x54;        ///< BLDY.
constexpr uint32_t kBgcntStride = 2;      ///< Bytes between two layers' BGnCNT.
constexpr uint32_t kOffsetStride = 4;     ///< Bytes between two layers' HOFS.
constexpr uint32_t kHalfOffset = 2;       ///< Bytes from a register's low half to its high half (or the next).
constexpr uint16_t kBgcntMosaicBit = 0x40;   ///< BGnCNT bit 6 (the compositor's mosaic is global).
constexpr uint16_t kBldcntMask = 0x3FFF;  ///< BLDCNT's bits.
constexpr uint16_t kWeightMask = 0x1F;    ///< BLDALPHA / BLDY weight bits.
constexpr int kEvbShift = 8;              ///< BLDALPHA evb.
constexpr int kAffineRefBits = 28;        ///< BGnX / BGnY: 28-bit signed.
constexpr int kAffineRefSignShift = 32 - kAffineRefBits;   ///< Sign-extends a 28-bit value: (v << 4) >> 4.
constexpr uint32_t kLowHalfMask = 0xFFFFu;   ///< A word's low half.
///@}

/// Transform of one affine layer (BG2 or BG3).
struct Affine
{
   int16_t pa;   ///< Texture dx per screen x, Q8.
   int16_t pb;   ///< Texture dx per screen y, Q8.
   int16_t pc;   ///< Texture dy per screen x, Q8.
   int16_t pd;   ///< Texture dy per screen y, Q8.
   int32_t x;    ///< Reference x, Q8.
   int32_t y;    ///< Reference y, Q8.
};

/// The emulated display registers. "Pending" is what the engine writes;
/// "live" is latched by latch().
struct Registers
{
   uint16_t dispcnt;                  ///< DISPCNT.
   uint16_t bgcnt[video::kNumBgs];    ///< BGnCNT.
   int16_t hofs[video::kNumBgs];     ///< BGnHOFS.
   int16_t vofs[video::kNumBgs];     ///< BGnVOFS.
   Affine affine[kNumAffineBgs];    ///< BG2 / BG3 transform.
   uint16_t bldcnt;                   ///< BLDCNT: mode << 6, first targets, second targets << 8.
   uint8_t eva;                      ///< BLDALPHA first weight.
   uint8_t evb;                      ///< BLDALPHA second weight.
   uint8_t evy;                      ///< BLDY.
   uint8_t mosaic;                   ///< Mosaic block size (0 / 1: off).
   uint8_t mosaic_layers;            ///< T1: per-layer mosaic's layers (0: every layer, the whole-screen post step).
   uint8_t win_rect[kNumWindows][kRectEdges];   ///< WINnH / WINnV: x0, y0, x1, y1.
   uint8_t win_mask[kWindowRegions];            ///< WININ / WINOUT: in0, in1, outside, obj.
   const uint16_t* win_lines;                   ///< N2: window 0's edges per scanline, or nullptr (the rectangle).
   const void* line_table;                      ///< BF: the scanline table, or nullptr.
   uint8_t line_offset;                         ///< BF: its register's I/O offset.
   bool line_word;                              ///< BF: 32-bit entries.
   bool win_wrap[kNumWindows];                  ///< BF: window n's edges came from a line write (hardware rules).
   const uint16_t* irq_table;                   ///< T1: the second scanline table, or nullptr.
   uint8_t irq_count;                           ///< T1: its registers.
   uint8_t irq_offsets[video::kMaxLineTargets]; ///< T1: their I/O offsets.
};

/// OAM: 128 entries of attr0, attr1, attr2 and an interleaved affine
/// parameter, as on the GBA (matrix n = parameter of entries 4n..4n+3).
struct Oam
{
   uint16_t words[video::kNumObjs * kOamEntryHalfwords];   ///< Host-order halfwords; never byte-addressed.
};

Registers g_pending{};                 ///< Registers as last written.
Registers g_live{};                    ///< Registers the compositor uses.
const Registers* g_line = &g_live;     ///< BF: the registers of the scanline being composed (g_live, or a copy with
                                       ///< the line's table entry written).
Oam g_oam_pending{};                   ///< OAM as last written.
Oam g_oam_live{};                      ///< OAM the compositor uses.
uint8_t g_vram[kVramBytes];            ///< BG VRAM in GBA byte layout (little-endian), P-19.
uint8_t g_obj_vram[kObjVramBytes];     ///< OBJ VRAM in GBA byte layout.
video::Color g_bg_palette[kPaletteColors];    ///< BG palette.
video::Color g_obj_palette[kPaletteColors];   ///< OBJ palette.
/// FX2 (M5): the frame present() composed last (black before the first).
uint16_t g_shown[kFramePixels] = {};

/// Reads a 16-bit VRAM value (wraps inside BG VRAM).
/// @param offset  byte offset
/// @return the value
uint16_t vram_u16(uint32_t offset)
{
   // Masking with an even mask keeps the halfword aligned and inside VRAM.
   return load_le16(g_vram + (offset & (kVramBytes - kHalfwordBytes)));
}

/// Byte offset of a map cell for a layer's BGnCNT.
/// @param bgcnt  the layer's control register
/// @param tx     column, tiles
/// @param ty     row, tiles
/// @return VRAM byte offset of the cell
uint32_t cell_offset(uint16_t bgcnt, int tx, int ty)
{
   const uint32_t screen_block = (bgcnt >> kBgcntScreenBlockShift) & kBgcntScreenBlockMask;
   const uint32_t size = (bgcnt >> kBgcntSizeShift) & kBgcntSizeMask;
   const uint32_t blocks_wide = (size & 1) ? kWideBlocks : 1;

   // Screen blocks of a large map run left to right, then top to bottom.
   const uint32_t block = screen_block + (static_cast<uint32_t>(tx) >> kScreenBlockShift) +
                          (static_cast<uint32_t>(ty) >> kScreenBlockShift) * blocks_wide;
   const uint32_t cell = (static_cast<uint32_t>(ty) & kScreenBlockTileMask) * kScreenBlockTiles +
                         (static_cast<uint32_t>(tx) & kScreenBlockTileMask);
   return block * kScreenBlockBytes + cell * kHalfwordBytes;
}

/// Whether a layer is a regular (text) layer in a display mode.
/// @param mode  display mode
/// @param bg    layer
/// @return true for text layers, false for affine ones
bool is_text_bg(int mode, int bg)
{
   return mode == 0 || (mode == kModeMixed && bg < kFirstAffineBg);
}

/// Whether a layer exists in a display mode.
/// @param mode  display mode
/// @param bg    layer
/// @return false for BG3 in mode 1 and BG0-BG1 in mode 2
bool bg_exists(int mode, int bg)
{
   return mode == 0 || (mode == kModeMixed && bg < kLastAffineBg) || (mode == kModeAffine && bg >= kFirstAffineBg);
}

/// Palette index of one text-mode BG pixel.
/// @param bg  layer
/// @param x   screen x
/// @param y   screen y
/// @return BG palette entry 1-255, or 0 when transparent
int text_bg_pixel(int bg, int x, int y)
{
   const uint16_t cnt = g_line->bgcnt[bg];
   const uint32_t size = (cnt >> kBgcntSizeShift) & kBgcntSizeMask;
   const int width = (size & 1) ? kTextBgLargePixels : kTextBgPixels;
   const int height = (size & kScreenSizeTall) ? kTextBgLargePixels : kTextBgPixels;

   // T1: a per-layer mosaic takes the block's top-left screen pixel
   const int m = g_line->mosaic;
   if (g_line->mosaic_layers != 0 && m > 1 && (g_line->mosaic_layers & (1 << bg)) != 0)
   {
      x -= x % m;
      y -= y % m;
   }
   // Map sizes are powers of two: the scrolled position wraps with a mask.
   const int sx = (x + g_line->hofs[bg]) & (width - 1);
   const int sy = (y + g_line->vofs[bg]) & (height - 1);

   const uint16_t cell = vram_u16(cell_offset(cnt, sx >> kTileShift, sy >> kTileShift));
   const uint32_t tile = cell & kMapTileMask;
   const int px = (cell & kMapHflip) ? kTilePixelMask - (sx & kTilePixelMask) : (sx & kTilePixelMask);
   const int py = (cell & kMapVflip) ? kTilePixelMask - (sy & kTilePixelMask) : (sy & kTilePixelMask);
   const uint32_t char_base = ((cnt >> kBgcntCharBlockShift) & kBgcntCharBlockMask) * kCharBlockBytes;

   if (cnt & kBgcnt8bpp)
   {   // 8bpp: one byte per pixel, a row is kTileSize bytes
      const uint32_t addr = char_base + tile * kTileBytes8bpp + py * kTileSize + px;
      return addr < kVramBytes ? g_vram[addr] : 0;
   }
   const uint32_t addr = char_base + tile * kTileBytes4bpp + py * kTileRowBytes4bpp + (px >> 1);
   if (addr >= kVramBytes)
   {
      return 0;
   }   // tiles past BG VRAM are not displayed

   // Two pixels per byte, the left one in the low nibble; 4bpp pixel 0 is transparent.
   const int nibble = (px & 1) ? (g_vram[addr] >> kNibbleBits) : (g_vram[addr] & kNibbleMask);
   return nibble == 0 ? 0 : ((cell >> kMapPaletteShift) & kPaletteBankMask) * kColorsPerBank + nibble;
}

/// Palette index of one affine BG pixel.
/// @param bg     layer 2 or 3
/// @param x      screen x
/// @param ref_x  the line's internal reference x (the frame's BGnX plus pb per line before; BF: or a line's write)
/// @param ref_y  the line's internal reference y
/// @return BG palette entry 1-255, or 0 when transparent / outside the map
int affine_bg_pixel(int bg, int x, int32_t ref_x, int32_t ref_y)
{
   const uint16_t cnt = g_line->bgcnt[bg];
   const Affine& a = g_line->affine[bg - kFirstAffineBg];
   const int size = kAffineBgMinPixels << ((cnt >> kBgcntSizeShift) & kBgcntSizeMask);

   // Texture position in Q8, truncated to whole pixels.
   int tx = (ref_x + a.pa * x) >> kQ8Shift;
   int ty = (ref_y + a.pc * x) >> kQ8Shift;
   if (cnt & kBgcntWrap)
   {   // wrap
      tx &= size - 1;
      ty &= size - 1;
   }
   else if (tx < 0 || ty < 0 || tx >= size || ty >= size)
   {
      return 0;
   }
   // Affine maps are one byte per cell, row-major.
   const uint32_t map_base = ((cnt >> kBgcntScreenBlockShift) & kBgcntScreenBlockMask) * kScreenBlockBytes;
   const uint32_t cell_addr =
      map_base + static_cast<uint32_t>((ty >> kTileShift) * (size >> kTileShift) + (tx >> kTileShift));
   if (cell_addr >= kVramBytes)
   {
      return 0;
   }
   const uint32_t tile = g_vram[cell_addr];
   const uint32_t addr = ((cnt >> kBgcntCharBlockShift) & kBgcntCharBlockMask) * kCharBlockBytes +
                         tile * kTileBytes8bpp +
                         static_cast<uint32_t>((ty & kTilePixelMask) * kTileSize + (tx & kTilePixelMask));
   return addr < kVramBytes ? g_vram[addr] : 0;
}

/// Byte offset of a map cell, checked against the layer's pending size.
/// @param bg  layer
/// @param x   column, tiles
/// @param y   row, tiles
/// @return VRAM byte offset of the cell
uint32_t map_cell_offset(video::BgIndex bg, int x, int y)
{
   const uint16_t cnt = g_pending.bgcnt[bg];
   const uint32_t size = (cnt >> kBgcntSizeShift) & kBgcntSizeMask;
   const int width_tiles = (size & 1) ? kTextBgLargeTiles : kScreenBlockTiles;
   const int height_tiles = (size & kScreenSizeTall) ? kTextBgLargeTiles : kScreenBlockTiles;
   ENGINE_ASSERT(bg < video::kNumBgs && x >= 0 && y >= 0 && x < width_tiles && y < height_tiles);
   return cell_offset(cnt, x, y);
}

/// One scanline of the sprite layer.
struct ObjLine
{
   uint8_t index[video::kScreenWidth];      ///< OBJ palette entry, 0 = none.
   uint8_t priority[video::kScreenWidth];   ///< Priority of the drawn pixel.
   uint8_t semi[video::kScreenWidth];       ///< 1 if from a semi-transparent sprite.
   uint8_t window[video::kScreenWidth];     ///< 1 if inside the OBJ window.
};

/// FX2 (M5): which sprites the OBJ unit draws on a line within its cycle budget (mGBA's model): OAM order, every
/// listed entry stepped over costs kObjEntryCycles per index, and each sprite on the line its own cycles; the unit
/// stops once the budget is spent (a sprite is drawn whole or not at all).
/// @param y      scanline
/// @param drawn  receives one flag per OAM entry
void obj_line_budget(int y, bool (&drawn)[video::kNumObjs])
{
   int remaining = (g_live.dispcnt & kDispcntHblankFree) != 0 ? kObjLineCyclesHblankFree : kObjLineCycles;
   int last = 0;
   for (int i = 0; i < video::kNumObjs; ++i)
   {
      drawn[i] = false;
   }
   for (int i = 0; i < video::kNumObjs; ++i)
   {
      const uint16_t a0 = g_oam_live.words[i * kOamEntryHalfwords + kOamAttr0];
      const uint16_t a1 = g_oam_live.words[i * kOamEntryHalfwords + kOamAttr1];
      const bool affine = (a0 & kObjAffineFlag) != 0;
      const auto shape = static_cast<video::ObjShape>((a0 >> kObjShapeShift) & kObjShapeMask);
      if ((!affine && (a0 & kObjDisable)) || shape >= video::NUM_OBJ_SHAPES)
      {
         continue;   // not listed
      }
      const uint8_t size = static_cast<uint8_t>(a1 >> kObjSizeShift);
      const bool dbl = affine && (a0 & kObjDoubleSize);
      const int w = video::obj_width(shape, size) * (dbl ? kDoubleSize : 1);
      const int h = video::obj_height(shape, size) * (dbl ? kDoubleSize : 1);
      const int raw_x = a1 & kObjXMask;
      const int x = raw_x >= kObjXNegative ? raw_x - kObjXWrap : raw_x;
      int cycles = 0;
      if (affine)
      {
         cycles = kAffineObjCycles + w * kAffineObjPixelCycles + (x < 0 ? x : 0);
      }
      else
      {
         if (x + w < 0)
         {
            continue;   // wholly left of the screen: not listed
         }
         cycles = w - kNormalObjCredit + (x < 0 ? x >> kNormalObjLeftShift : 0);
      }
      const int raw_y = a0 & kObjYMask;
      if ((raw_y >= video::kScreenHeight && raw_y + h < kObjLinesTotal) ||
          (raw_x >= video::kScreenWidth && raw_x + w < kObjXWrap))
      {
         continue;   // below or right of the screen without wrapping back: not listed
      }
      remaining -= kObjEntryCycles * (i - last);
      last = i;
      if (remaining <= 0)
      {
         break;   // the budget is spent: this sprite and every later one are dropped
      }
      const int top = raw_y + h > kObjYWrap ? raw_y - kObjYWrap : raw_y;
      if (y < top || y >= top + h)
      {
         continue;
      }
      drawn[i] = true;
      remaining -= cycles;
   }
}

/// Renders every sprite's pixels on one scanline. Lower OAM entries win. FX2: only the sprites the line's OBJ
/// budget lets the unit draw (obj_line_budget()).
/// @param y     scanline
/// @param line  receives the line
void render_obj_line(int y, ObjLine& line)
{
   for (int x = 0; x < video::kScreenWidth; ++x)
   {
      line.index[x] = 0;
      line.priority[x] = kNoPriority;
      line.semi[x] = 0;
      line.window[x] = 0;
   }
   if (!(g_live.dispcnt & kDispcntObj))
   {
      return;
   }
   bool drawn[video::kNumObjs];
   obj_line_budget(y, drawn);
   for (int i = video::kNumObjs - 1; i >= 0; --i)
   {   // back to front: lower entries overwrite
      if (!drawn[i])
      {
         continue;   // FX2: past the line's budget, or not on it
      }
      const uint16_t a0 = g_oam_live.words[i * kOamEntryHalfwords + kOamAttr0];
      const uint16_t a1 = g_oam_live.words[i * kOamEntryHalfwords + kOamAttr1];
      const uint16_t a2 = g_oam_live.words[i * kOamEntryHalfwords + kOamAttr2];
      const bool affine = (a0 & kObjAffineFlag) != 0;
      if (!affine && (a0 & kObjDisable))
      {
         continue;
      }   // hidden
      const auto mode = static_cast<video::ObjMode>((a0 >> kObjModeShift) & kObjModeMask);
      const auto shape = static_cast<video::ObjShape>((a0 >> kObjShapeShift) & kObjShapeMask);
      if (shape >= video::NUM_OBJ_SHAPES || mode >= video::ObjMode::NUM_OBJ_MODES)
      {
         continue;
      }
      const uint8_t size = static_cast<uint8_t>(a1 >> kObjSizeShift);
      const int w = video::obj_width(shape, size);
      const int h = video::obj_height(shape, size);
      const bool dbl = affine && (a0 & kObjDoubleSize);
      const int box_w = dbl ? w * kDoubleSize : w;
      const int box_h = dbl ? h * kDoubleSize : h;
      int oy = a0 & kObjYMask;
      if (oy + box_h > kObjYWrap)
      {
         oy -= kObjYWrap;
      }          // wraps off the top
      if (y < oy || y >= oy + box_h)
      {
         continue;
      }
      int ox = a1 & kObjXMask;
      if (ox >= kObjXNegative)
      {
         ox -= kObjXWrap;
      }                 // 9-bit signed
      const uint32_t tile0 = a2 & kObjTileMask;
      const int prio = (a2 >> kObjPriorityShift) & kPriorityMask;
      const int bank = (a2 >> kObjPaletteShift) & kPaletteBankMask;
      const int tiles_wide = w / kTileSize;

      // Identity unless the sprite is affine; matrix parameters sit in OAM entries 4n..4n+3.
      int16_t pa = kQ8One;
      int16_t pb = 0;
      int16_t pc = 0;
      int16_t pd = kQ8One;
      if (affine)
      {
         const int group = (a1 >> kObjAffineGroupShift) & kObjAffineGroupMask;
         const int first = group * kAffineParams;
         pa = static_cast<int16_t>(g_oam_live.words[(first + kParamA) * kOamEntryHalfwords + kOamAffineHalfword]);
         pb = static_cast<int16_t>(g_oam_live.words[(first + kParamB) * kOamEntryHalfwords + kOamAffineHalfword]);
         pc = static_cast<int16_t>(g_oam_live.words[(first + kParamC) * kOamEntryHalfwords + kOamAffineHalfword]);
         pd = static_cast<int16_t>(g_oam_live.words[(first + kParamD) * kOamEntryHalfwords + kOamAffineHalfword]);
      }

      // Affine sprites transform about the centre of their box.
      const int dy = y - oy - box_h / kHalf;
      for (int bx = 0; bx < box_w; ++bx)
      {
         const int sx = ox + bx;
         if (sx < 0 || sx >= video::kScreenWidth)
         {
            continue;
         }
         int tx;
         int ty;
         if (affine)
         {
            const int dx = bx - box_w / kHalf;
            tx = ((pa * dx + pb * dy) >> kQ8Shift) + w / kHalf;
            ty = ((pc * dx + pd * dy) >> kQ8Shift) + h / kHalf;
            if (tx < 0 || ty < 0 || tx >= w || ty >= h)
            {
               continue;
            }
         }
         else
         {
            tx = (a1 & kObjHflip) ? w - 1 - bx : bx;
            ty = (a1 & kObjVflip) ? h - 1 - (y - oy) : (y - oy);
         }

         // 1D mapping: the sprite's tiles follow each other row by row.
         const uint32_t tile = tile0 + static_cast<uint32_t>((ty >> kTileShift) * tiles_wide + (tx >> kTileShift));
         const uint32_t addr =
            (tile & kObjTileMask) * kTileBytes4bpp +
            static_cast<uint32_t>((ty & kTilePixelMask) * kTileRowBytes4bpp + ((tx & kTilePixelMask) >> 1));
         const int nibble = (tx & 1) ? (g_obj_vram[addr] >> kNibbleBits) : (g_obj_vram[addr] & kNibbleMask);
         if (nibble == 0)
         {
            continue;
         }
         if (mode == video::ObjMode::Window)
         {
            line.window[sx] = 1;
            continue;
         }
         if (prio <= line.priority[sx])
         {
            line.index[sx] = static_cast<uint8_t>(bank * kColorsPerBank + nibble);
            line.priority[sx] = static_cast<uint8_t>(prio);
            line.semi[sx] = mode == video::ObjMode::SemiTransparent ? 1 : 0;
         }
      }
   }
}

/// Window mask (bits 0-4 layers, bit 5 effects) for a screen pixel.
/// @param x       screen x
/// @param y       screen y
/// @param in_obj  pixel is inside the OBJ window
/// @return the mask; every bit when no window is enabled
uint8_t window_mask(int x, int y, bool in_obj)
{
   const Registers& g = *g_line;
   const uint16_t d = g.dispcnt;
   if (!(d & (kDispcntWin0 | kDispcntWin1 | kDispcntObjWin)))
   {
      return video::kLayerAll;
   }
   for (int w = 0; w < kNumWindows; ++w)
   {
      if (!(d & (w == 0 ? kDispcntWin0 : kDispcntWin1)))
      {
         continue;
      }
      const uint8_t* r = g.win_rect[w];
      if (w == 0 && g.win_lines != nullptr)
      {
         // N2: this scanline's edges (the hardware's rule: a right edge past the screen, or left of the left edge,
         // is the screen's right edge)
         const uint16_t e = g.win_lines[y];
         const int x0 = e >> video::kWindowLineShift;
         const int raw = e & kWindowEdgeMask;
         const int x1 = raw > video::kScreenWidth || raw < x0 ? video::kScreenWidth : raw;
         if (x >= x0 && x < x1 && y >= r[kRectY0] && y < r[kRectY1])
         {
            return g.win_mask[w];
         }
         continue;
      }
      // BF: edges a scanline table wrote, as the hardware reads them (line_write() clamps them): a right edge left
      // of the left edge wraps around the screen
      const bool in_x = g.win_wrap[w] && r[kRectX1] < r[kRectX0] ? (x >= r[kRectX0] || x < r[kRectX1])
                                                                  : (x >= r[kRectX0] && x < r[kRectX1]);
      if (in_x && y >= r[kRectY0] && y < r[kRectY1])
      {
         return g.win_mask[w];
      }
   }
   if ((d & kDispcntObjWin) && in_obj)
   {
      return g.win_mask[kMaskObj];
   }
   return g.win_mask[kMaskOutside];
}

/// One channel of a BGR555 color.
/// @param c      the color
/// @param shift  0 (red), 5 (green) or 10 (blue)
/// @return 0-31
int channel(uint16_t c, int shift)
{
   return (c >> shift) & kChannelMax;
}

/// Alpha blend: a * eva / 16 + b * evb / 16 per channel, saturating at 31.
/// @param a    first-target color
/// @param b    second-target color
/// @param eva  first weight, 0-16
/// @param evb  second weight, 0-16
/// @return the blended color
uint16_t blend_alpha(uint16_t a, uint16_t b, int eva, int evb)
{
   uint16_t out = 0;
   for (int s = kRedShift; s <= kBlueShift; s += kChannelBits)
   {
      int v = (channel(a, s) * eva + channel(b, s) * evb) >> kBlendShift;
      if (v > kChannelMax)
      {
         v = kChannelMax;
      }
      out = static_cast<uint16_t>(out | (v << s));
   }
   return out;
}

/// Brightness fade toward white or black.
/// @param c      the color
/// @param evy    0-16
/// @param white  toward white instead of black
/// @return the faded color
uint16_t blend_brightness(uint16_t c, int evy, bool white)
{
   uint16_t out = 0;
   for (int s = kRedShift; s <= kBlueShift; s += kChannelBits)
   {
      const int v = channel(c, s);
      const int r = white ? v + (((kChannelMax - v) * evy) >> kBlendShift) : v - ((v * evy) >> kBlendShift);
      out = static_cast<uint16_t>(out | (r << s));
   }
   return out;
}

/// BF: a 28-bit signed affine reference value.
/// @param v  the register's bits
/// @return the value
int32_t affine_ref(uint32_t v)
{
   return static_cast<int32_t>(v << kAffineRefSignShift) >> kAffineRefSignShift;
}

/// BF: one halfword written to a register of a line's copy, as the hardware takes it.
/// @param r       the registers
/// @param offset  the I/O offset
/// @param value   the halfword
/// @param[in,out] ref_x  the affine layers' internal reference x (restarted by a BGnX write)
/// @param[in,out] ref_y  the same for y
void io_write(Registers& r, uint32_t offset, uint16_t value, int32_t* ref_x, int32_t* ref_y)
{
   if (offset >= kIoBgcnt0 && offset < kIoHofs0)
   {
      r.bgcnt[(offset - kIoBgcnt0) / kBgcntStride] = static_cast<uint16_t>(value & ~kBgcntMosaicBit);
      return;
   }
   if (offset >= kIoHofs0 && offset < kIoHofs0 + kOffsetStride * video::kNumBgs)
   {
      const uint32_t bg = (offset - kIoHofs0) / kOffsetStride;
      if ((offset - kIoHofs0) % kOffsetStride == 0)
      {
         r.hofs[bg] = static_cast<int16_t>(value);
      }
      else
      {
         r.vofs[bg] = static_cast<int16_t>(value);
      }
      return;
   }
   const uint32_t refs[] = {kIoBg2X, kIoBg2Y, kIoBg3X, kIoBg3Y};   // two per affine layer: x, y
   constexpr int kRefsPerLayer = 2;
   for (int i = 0; i < static_cast<int>(sizeof(refs) / sizeof(refs[0])); ++i)
   {
      if (offset != refs[i] && offset != refs[i] + kHalfOffset)
      {
         continue;
      }
      const int layer = i / kRefsPerLayer;
      const bool is_x = (i % kRefsPerLayer) == 0;
      Affine& a = r.affine[layer];
      int32_t& field = is_x ? a.x : a.y;
      const uint32_t bits = static_cast<uint32_t>(field);
      const uint32_t merged = offset == refs[i] ? ((bits & ~kLowHalfMask) | value)
                                                : ((bits & kLowHalfMask) | (uint32_t{value} << kHalfwordBits));
      field = affine_ref(merged);
      // a write restarts the layer's internal reference from the register
      (is_x ? ref_x : ref_y)[layer] = field;
      return;
   }
   if (offset == kIoWin0h || offset == kIoWin1h)
   {
      const int w = offset == kIoWin0h ? 0 : 1;
      int left = value >> video::kWindowLineShift;
      int right = value & kWindowEdgeMask;
      // the hardware's reading: a left edge past the screen and past the right edge starts at 0; a right edge past
      // the screen is the screen's edge (and so is a left edge past it then)
      if (left > video::kScreenWidth && left > right)
      {
         left = 0;
      }
      if (right > video::kScreenWidth)
      {
         right = video::kScreenWidth;
         if (left > video::kScreenWidth)
         {
            left = video::kScreenWidth;
         }
      }
      r.win_rect[w][kRectX0] = static_cast<uint8_t>(left);
      r.win_rect[w][kRectX1] = static_cast<uint8_t>(right);
      r.win_wrap[w] = true;
      return;
   }
   if (offset == kIoBldcnt)
   {
      r.bldcnt = static_cast<uint16_t>(value & kBldcntMask);
   }
   else if (offset == kIoBldalpha)
   {
      r.eva = static_cast<uint8_t>(value & kWeightMask);
      r.evb = static_cast<uint8_t>((value >> kEvbShift) & kWeightMask);
   }
   else if (offset == kIoBldy)
   {
      r.evy = static_cast<uint8_t>(value & kWeightMask);
   }
}

/// BF: a line's scanline table entry written to a copy of the registers.
/// @param r  the line's registers (a copy of the live ones)
/// @param y  the line
/// @param[in,out] ref_x  affine internal references x
/// @param[in,out] ref_y  affine internal references y
void line_write(Registers& r, int y, int32_t* ref_x, int32_t* ref_y)
{
   if (r.line_word)
   {
      const uint32_t v = static_cast<const uint32_t*>(r.line_table)[y];
      io_write(r, r.line_offset, static_cast<uint16_t>(v), ref_x, ref_y);
      io_write(r, r.line_offset + kHalfOffset, static_cast<uint16_t>(v >> kHalfwordBits), ref_x, ref_y);
   }
   else
   {
      io_write(r, r.line_offset, static_cast<const uint16_t*>(r.line_table)[y], ref_x, ref_y);
   }
}

/// T1: a line's second-table entry written to each of its registers.
/// @param r  the line's registers (a copy of the live ones)
/// @param y  the line
/// @param[in,out] ref_x  affine internal references x
/// @param[in,out] ref_y  affine internal references y
void line_write_irq(Registers& r, int y, int32_t* ref_x, int32_t* ref_y)
{
   const uint16_t v = r.irq_table[y];
   for (int i = 0; i < r.irq_count; ++i)
   {
      io_write(r, r.irq_offsets[i], v, ref_x, ref_y);
   }
}

} // namespace

void reset()
{
   g_pending = Registers{};
   g_pending.dispcnt = kDispcntForcedBlank;   // as on GBA: hidden until the first latch()
   for (Affine& a : g_pending.affine)
   {
      a.pa = kQ8One;   // identity transform
      a.pd = kQ8One;
   }
   g_live = g_pending;
   for (int i = 0; i < video::kNumObjs; ++i)
   {
      uint16_t* entry = g_oam_pending.words + i * kOamEntryHalfwords;
      entry[kOamAttr0] = video::kObjHidden.attr0;
      entry[kOamAttr1] = 0;
      entry[kOamAttr2] = 0;
      entry[kOamAffineHalfword] = 0;
   }
   g_oam_live = g_oam_pending;
   for (video::Color& c : g_bg_palette)
   {
      c = 0;
   }
   for (video::Color& c : g_obj_palette)
   {
      c = 0;
   }
}

void latch()
{
   g_pending.dispcnt &= static_cast<uint16_t>(~kDispcntForcedBlank);
   g_live = g_pending;
   g_oam_live = g_oam_pending;
}

const uint16_t* present()
{
   compose(g_shown);
   return g_shown;
}

const uint16_t* shown()
{
   return g_shown;
}

void compose(uint16_t* frame)
{
   if (g_live.dispcnt & kDispcntForcedBlank)
   {
      for (int i = 0; i < kFramePixels; ++i)
      {
         frame[i] = kForcedBlankColor;
      }
      return;
   }
   const int mode = g_live.dispcnt & kDispcntModeMask;
   ENGINE_ASSERT(mode <= kModeAffine);
   const uint16_t backdrop = g_bg_palette[0];

   // Affine layers' internal reference points: the frame's BGnX / BGnY, plus pb / pd after each line (BF: a line's
   // write of BGnX / BGnY restarts them from the value written).
   int32_t ref_x[kNumAffineBgs];
   int32_t ref_y[kNumAffineBgs];
   for (int i = 0; i < kNumAffineBgs; ++i)
   {
      ref_x[i] = g_live.affine[i].x;
      ref_y[i] = g_live.affine[i].y;
   }
   Registers line_regs;   // BF: the line's registers when a scanline table is set

   ObjLine obj;
   for (int y = 0; y < video::kScreenHeight; ++y)
   {
      g_line = &g_live;
      if (g_live.line_table != nullptr || g_live.irq_table != nullptr)
      {
         line_regs = g_live;
         if (g_live.line_table != nullptr)
         {
            line_write(line_regs, y, ref_x, ref_y);
         }
         if (g_live.irq_table != nullptr)
         {
            line_write_irq(line_regs, y, ref_x, ref_y);   // T1: the second table
         }
         g_line = &line_regs;
      }
      const Registers& g = *g_line;

      // Background draw order: priority 0 first, ties broken by lower BG number.
      int order[video::kNumBgs];
      int prio_of[video::kNumBgs];
      int count = 0;
      for (int prio = 0; prio < kNumPriorities; ++prio)
      {
         for (int bg = 0; bg < video::kNumBgs; ++bg)
         {
            if (!(g.dispcnt & (kDispcntBg0 << bg)) || !bg_exists(mode, bg) || (g.bgcnt[bg] & kPriorityMask) != prio)
            {
               continue;
            }
            prio_of[count] = prio;
            order[count++] = bg;
         }
      }
      const auto blend_mode = static_cast<video::BlendMode>((g.bldcnt >> kBldcntModeShift) & kBldcntModeMask);
      const uint8_t first_mask = static_cast<uint8_t>(g.bldcnt & kLayerAll);
      const uint8_t second_mask = static_cast<uint8_t>((g.bldcnt >> kBldcntSecondShift) & kLayerAll);

      // Weights above 16 count as 16, as on the GBA.
      const int eva = g.eva > kBlendMax ? kBlendMax : g.eva;
      const int evb = g.evb > kBlendMax ? kBlendMax : g.evb;
      const int evy = g.evy > kBlendMax ? kBlendMax : g.evy;

      render_obj_line(y, obj);
      for (int x = 0; x < video::kScreenWidth; ++x)
      {
         const uint8_t mask = window_mask(x, y, obj.window[x] != 0);
            // Front-most two layers: (color, layer bit). Backdrop is last.
         uint16_t color[kBlendLayers] = {backdrop, backdrop};
         uint8_t layer[kBlendLayers] = {video::kLayerBackdrop, video::kLayerBackdrop};
         bool top_semi = false;
         int found = 0;
         const bool obj_here = obj.index[x] != 0 && (mask & video::kLayerObj);
         bool obj_used = false;
         for (int i = 0; i <= count && found < kBlendLayers; ++i)
         {
            // Sprites go before any background of equal or lower priority.
            if (obj_here && !obj_used && (i == count || obj.priority[x] <= prio_of[i]))
            {
               color[found] = g_obj_palette[obj.index[x]];
               layer[found] = video::kLayerObj;
               if (found == 0)
               {
                  top_semi = obj.semi[x] != 0;
               }
               ++found;
               obj_used = true;
               if (found == kBlendLayers)
               {
                  break;
               }
            }
            if (i == count)
            {
               break;
            }
            const int bg = order[i];
            if (!(mask & (1 << bg)))
            {
               continue;
            }
            const int index = is_text_bg(mode, bg) ? text_bg_pixel(bg, x, y)
                                                   : affine_bg_pixel(bg, x, ref_x[bg - kFirstAffineBg],
                                                                     ref_y[bg - kFirstAffineBg]);
            if (index == 0)
            {
               continue;
            }
            color[found] = g_bg_palette[index];
            layer[found] = static_cast<uint8_t>(1 << bg);
            ++found;
         }
         uint16_t out = color[0];
         if (mask & video::kWindowEffects)
         {
            const bool second_ok = (second_mask & layer[1]) != 0;
            if (top_semi && second_ok)
            {
               out = blend_alpha(color[0], color[1], eva, evb);
            }
            else if (first_mask & layer[0])
            {
               if (blend_mode == video::BlendMode::Alpha && second_ok)
               {
                  out = blend_alpha(color[0], color[1], eva, evb);
               }
               else if (blend_mode == video::BlendMode::Brighten)
               {
                  out = blend_brightness(color[0], evy, true);
               }
               else if (blend_mode == video::BlendMode::Darken)
               {
                  out = blend_brightness(color[0], evy, false);
               }
            }
         }
         frame[y * video::kScreenWidth + x] = out;
      }
      for (int i = 0; i < kNumAffineBgs; ++i)
      {   // the internal reference moves by pb / pd after each line
         ref_x[i] += g.affine[i].pb;
         ref_y[i] += g.affine[i].pd;
      }
   }
   g_line = &g_live;
   // Mosaic (D-17 transitions): blocks take their top-left pixel. The GBA's
   // per-layer mosaic is aligned the same way for backgrounds.
   const int m = g_live.mosaic;
   if (m > 1 && g_live.mosaic_layers == 0)   // T1: a per-layer mosaic was applied per pixel
   {
      for (int y = 0; y < video::kScreenHeight; ++y)
      {
         for (int x = 0; x < video::kScreenWidth; ++x)
         {
            frame[y * video::kScreenWidth + x] = frame[(y - y % m) * video::kScreenWidth + (x - x % m)];
         }
      }
   }
}

} // namespace capsule::hal::soft

namespace capsule::hal::video
{

using soft::g_pending;

void set_mode(uint8_t mode)
{
   ENGINE_ASSERT(mode <= kModeAffine);
   g_pending.dispcnt = static_cast<uint16_t>((g_pending.dispcnt & ~uint32_t{soft::kDispcntModeMask}) | mode);
}

void configure_bg(BgIndex bg, bool on, uint8_t priority, uint8_t char_block, uint8_t screen_block, bool color_8bpp,
                  uint8_t screen_size)
{
   ENGINE_ASSERT(bg < kNumBgs && priority < kNumPriorities && char_block < kNumCharBlocks &&
                 screen_block < kNumScreenBlocks && screen_size < kNumScreenSizes);
   const uint16_t wrap = static_cast<uint16_t>(g_pending.bgcnt[bg] & soft::kBgcntWrap);   // kept across configure
   g_pending.bgcnt[bg] = static_cast<uint16_t>(
      priority | (char_block << soft::kBgcntCharBlockShift) | (color_8bpp ? soft::kBgcnt8bpp : 0) |
      (screen_block << soft::kBgcntScreenBlockShift) | wrap | (screen_size << soft::kBgcntSizeShift));
   const auto enable = static_cast<uint16_t>(soft::kDispcntBg0 << bg);
   g_pending.dispcnt = static_cast<uint16_t>(on ? (g_pending.dispcnt | enable) : (g_pending.dispcnt & ~enable));
}

void set_bg_scroll(BgIndex bg, int16_t x, int16_t y)
{
   ENGINE_ASSERT(bg < kNumBgs);
   g_pending.hofs[bg] = x;
   g_pending.vofs[bg] = y;
}

int16_t bg_scroll_x(BgIndex bg)
{
   return g_pending.hofs[bg];
}
int16_t bg_scroll_y(BgIndex bg)
{
   return g_pending.vofs[bg];
}

// Tiles are 32-bit words in GBA order (8 nibbles, leftmost pixel lowest), as
// the GBA HAL copies them; stored little-endian so every host sees GBA bytes.
// (Documented in capsule/hal/video.h.)
void load_bg_tiles(uint8_t char_block, TileIndex first_tile, const uint32_t* data, uint16_t tile_count)
{
   ENGINE_ASSERT(char_block < kNumCharBlocks);
   const uint32_t offset = char_block * kCharBlockBytes + first_tile * uint32_t{kTileBytes4bpp};
   const uint32_t words = tile_count * uint32_t{kTileBytes4bpp / kWordBytes};
   ENGINE_ASSERT(offset + words * kWordBytes <= soft::kVramBytes);
   vram::before_immediate(Region::BgVram, offset, words * kWordBytes);
   for (uint32_t i = 0; i < words; ++i)
   {
      store_le32(soft::g_vram + offset + i * kWordBytes, data[i]);
   }
}

void load_bg_palette(uint8_t first_color, const Color* colors, uint16_t count)
{
   ENGINE_ASSERT(first_color + count <= kPaletteColors);
   vram::before_immediate(Region::BgPalette, first_color * kHalfwordBytes, count * kHalfwordBytes);
   for (uint16_t i = 0; i < count; ++i)
   {
      soft::g_bg_palette[first_color + i] = colors[i];
   }
}

void write_map_cell(BgIndex bg, int x, int y, MapCell cell)
{
   const uint32_t offset = soft::map_cell_offset(bg, x, y);
   vram::before_immediate(Region::BgVram, offset, kHalfwordBytes);
   store_le16(soft::g_vram + offset, cell);
}

void write_map_row(BgIndex bg, int x, int y, const MapCell* cells, int count)
{
   if (count <= 0)
   {
      return;
   }
   // One queue check from the row's first cell to its last (see the GBA HAL).
   const uint32_t first = soft::map_cell_offset(bg, x, y);
   const uint32_t last = soft::map_cell_offset(bg, x + count - 1, y);
   vram::before_immediate(Region::BgVram, first, last - first + kHalfwordBytes);
   for (int i = 0; i < count; ++i)
   {
      store_le16(soft::g_vram + soft::map_cell_offset(bg, x + i, y), cells[i]);
   }
}

MapCell read_map_cell(BgIndex bg, int x, int y)
{
   return soft::vram_u16(soft::map_cell_offset(bg, x, y));
}

void set_bg_affine(BgIndex bg, int16_t pa, int16_t pb, int16_t pc, int16_t pd, int32_t ref_x, int32_t ref_y)
{
   ENGINE_ASSERT(bg == kFirstAffineBg || bg == kLastAffineBg);
   g_pending.affine[bg - kFirstAffineBg] = soft::Affine{pa, pb, pc, pd, ref_x, ref_y};
}

void set_bg_wrap(BgIndex bg, bool wrap)
{
   ENGINE_ASSERT(bg == kFirstAffineBg || bg == kLastAffineBg);
   g_pending.bgcnt[bg] = static_cast<uint16_t>(wrap ? (g_pending.bgcnt[bg] | soft::kBgcntWrap)
                                                    : (g_pending.bgcnt[bg] & ~soft::kBgcntWrap));
}

void load_bg_tiles_8bpp(uint8_t char_block, TileIndex first_tile, const uint32_t* data, uint16_t tile_count)
{
   ENGINE_ASSERT(char_block < kNumCharBlocks);
   const uint32_t offset = char_block * kCharBlockBytes + first_tile * uint32_t{kTileBytes8bpp};
   const uint32_t words = tile_count * uint32_t{kTileBytes8bpp / kWordBytes};
   ENGINE_ASSERT(offset + words * kWordBytes <= soft::kVramBytes);
   vram::before_immediate(Region::BgVram, offset, words * kWordBytes);
   for (uint32_t i = 0; i < words; ++i)
   {
      store_le32(soft::g_vram + offset + i * kWordBytes, data[i]);
   }
}

void write_affine_cell(BgIndex bg, int x, int y, uint8_t tile)
{
   ENGINE_ASSERT(bg == kFirstAffineBg || bg == kLastAffineBg);
   const uint16_t cnt = g_pending.bgcnt[bg];
   const int cells = soft::kAffineBgMinCells << ((cnt >> soft::kBgcntSizeShift) & soft::kBgcntSizeMask);
   ENGINE_ASSERT(x >= 0 && y >= 0 && x < cells && y < cells);
   const uint32_t addr = ((cnt >> soft::kBgcntScreenBlockShift) & soft::kBgcntScreenBlockMask) * kScreenBlockBytes +
                         static_cast<uint32_t>(y * cells + x);
   ENGINE_ASSERT(addr < soft::kVramBytes);
   vram::before_immediate(Region::BgVram, addr & ~1u, kHalfwordBytes);   // the halfword holding the byte
   soft::g_vram[addr] = tile;
}

void set_obj_layer(bool on)
{
   g_pending.dispcnt =
      static_cast<uint16_t>(on ? (g_pending.dispcnt | soft::kDispcntObj) : (g_pending.dispcnt & ~soft::kDispcntObj));
}

void set_obj(uint8_t index, ObjAttr attr)
{
   ENGINE_ASSERT(index < kNumObjs);
   uint16_t* entry = soft::g_oam_pending.words + index * kOamEntryHalfwords;
   entry[kOamAttr0] = attr.attr0;
   entry[kOamAttr1] = attr.attr1;
   entry[kOamAttr2] = attr.attr2;
}

void set_obj_affine(uint8_t group, int16_t pa, int16_t pb, int16_t pc, int16_t pd)
{
   ENGINE_ASSERT(group < kNumObjAffine);
   const int16_t p[kAffineParams] = {pa, pb, pc, pd};
   for (int i = 0; i < kAffineParams; ++i)
   {   // parameter i of matrix n is in the spare halfword of OAM entry 4n + i
      soft::g_oam_pending.words[(group * kAffineParams + i) * kOamEntryHalfwords + kOamAffineHalfword] =
         static_cast<uint16_t>(p[i]);
   }
}

void load_obj_tiles(TileIndex first_tile, const uint32_t* data, uint16_t tile_count)
{
   const uint32_t offset = first_tile * uint32_t{kTileBytes4bpp};
   const uint32_t words = tile_count * uint32_t{kTileBytes4bpp / kWordBytes};
   ENGINE_ASSERT(offset + words * kWordBytes <= soft::kObjVramBytes);
   vram::before_immediate(Region::ObjVram, offset, words * kWordBytes);
   for (uint32_t i = 0; i < words; ++i)
   {
      store_le32(soft::g_obj_vram + offset + i * kWordBytes, data[i]);
   }
}

void load_obj_palette(uint8_t first_color, const Color* colors, uint16_t count)
{
   ENGINE_ASSERT(first_color + count <= kPaletteColors);
   vram::before_immediate(Region::ObjPalette, first_color * kHalfwordBytes, count * kHalfwordBytes);
   for (uint16_t i = 0; i < count; ++i)
   {
      soft::g_obj_palette[first_color + i] = colors[i];
   }
}

void set_blend(BlendMode mode, uint8_t first, uint8_t second)
{
   ENGINE_ASSERT(mode < BlendMode::NUM_BLEND_MODES);
   g_pending.bldcnt =
      static_cast<uint16_t>((first & kLayerAll) | (static_cast<uint16_t>(mode) << soft::kBldcntModeShift) |
                            ((second & kLayerAll) << soft::kBldcntSecondShift));
}

void set_blend_alpha(uint8_t eva, uint8_t evb)
{
   g_pending.eva = eva;
   g_pending.evb = evb;
}

void set_blend_brightness(uint8_t evy)
{
   g_pending.evy = evy;
}

void set_mosaic(uint8_t size)
{
   g_pending.mosaic = size > kMosaicMax ? uint8_t{kMosaicMax} : size;
}

void set_mosaic_layers(uint8_t layers)
{
   const auto bgs = static_cast<uint8_t>(layers & ((1 << kNumBgs) - 1));
   g_pending.mosaic_layers = (layers & kLayerAll) == kLayerAll ? uint8_t{0} : static_cast<uint8_t>(bgs | kLayerObj);
}

void set_window_rect(uint8_t win, uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
   ENGINE_ASSERT(win < kNumWindows && x0 <= x1 && y0 <= y1 && x1 <= kScreenWidth && y1 <= kScreenHeight);
   uint8_t* r = g_pending.win_rect[win];
   r[soft::kRectX0] = x0;
   r[soft::kRectY0] = y0;
   r[soft::kRectX1] = x1;
   r[soft::kRectY1] = y1;
}

void set_window_masks(uint8_t in0, uint8_t in1, uint8_t outside, uint8_t obj)
{
   g_pending.win_mask[soft::kMaskIn0] = static_cast<uint8_t>(in0 & kLayerAll);
   g_pending.win_mask[soft::kMaskIn1] = static_cast<uint8_t>(in1 & kLayerAll);
   g_pending.win_mask[soft::kMaskOutside] = static_cast<uint8_t>(outside & kLayerAll);
   g_pending.win_mask[soft::kMaskObj] = static_cast<uint8_t>(obj & kLayerAll);
}

void set_window_lines(const uint16_t* lines)
{
   ENGINE_ASSERT(lines == nullptr || g_pending.line_table == nullptr);   // BF: one HBlank transfer at a time
   g_pending.win_lines = lines;
}

void set_line_effect(LineRegister reg, LineWidth width, const void* table)
{
   ENGINE_ASSERT(reg < LineRegister::NUM_LINE_REGISTERS && width < LineWidth::NUM_LINE_WIDTHS);
   ENGINE_ASSERT(width == LineWidth::Half || line_word_allowed(reg));
   ENGINE_ASSERT(table == nullptr || g_pending.win_lines == nullptr);   // one HBlank transfer at a time
   g_pending.line_table = table;
   g_pending.line_offset = static_cast<uint8_t>(line_register_offset(reg));
   g_pending.line_word = width == LineWidth::Word;
}

void set_line_effect_irq(const LineRegister* regs, uint8_t count, const uint16_t* table)
{
   g_pending.irq_table = table;
   g_pending.irq_count = 0;
   if (table == nullptr)
   {
      return;
   }
   ENGINE_ASSERT(count >= 1 && count <= kMaxLineTargets);
   for (uint8_t i = 0; i < count; ++i)
   {
      ENGINE_ASSERT(regs[i] < LineRegister::NUM_LINE_REGISTERS);
      g_pending.irq_offsets[i] = static_cast<uint8_t>(line_register_offset(regs[i]));
   }
   g_pending.irq_count = count;
}

void set_windows(bool win0, bool win1, bool obj_win)
{
   uint16_t d =
      static_cast<uint16_t>(g_pending.dispcnt & ~(soft::kDispcntWin0 | soft::kDispcntWin1 | soft::kDispcntObjWin));
   if (win0)
   {
      d = static_cast<uint16_t>(d | soft::kDispcntWin0);
   }
   if (win1)
   {
      d = static_cast<uint16_t>(d | soft::kDispcntWin1);
   }
   if (obj_win)
   {
      d = static_cast<uint16_t>(d | soft::kDispcntObjWin);
   }
   g_pending.dispcnt = d;
}

namespace
{

/// The model's storage for a region.
/// @param region  the region
/// @return its first byte (palettes: host-order halfwords)
uint8_t* region_base(Region region)
{
   switch (region)
   {
   case Region::BgVram:
      return soft::g_vram;
   case Region::ObjVram:
      return soft::g_obj_vram;
   case Region::BgPalette:
      return reinterpret_cast<uint8_t*>(soft::g_bg_palette);
   case Region::ObjPalette:
      return reinterpret_cast<uint8_t*>(soft::g_obj_palette);
   case Region::NUM_REGIONS:
      break;
   }
   return nullptr;
}

/// Whether a region is a palette (stored as host-order halfwords, not GBA bytes).
/// @param region  the region
/// @return true for the palettes
bool is_palette(Region region)
{
   return region == Region::BgPalette || region == Region::ObjPalette;
}

/// Stores one halfword in a region.
/// @param region  the region
/// @param offset  byte offset (even)
/// @param value   the halfword
void store16(Region region, uint32_t offset, uint16_t value)
{
   uint8_t* base = region_base(region);
   if (is_palette(region))
   {
      reinterpret_cast<Color*>(base)[offset / kHalfwordBytes] = value;
   }
   else
   {
      store_le16(base + offset, value);
   }
}

} // namespace

// Words and halfwords keep the GBA's little-endian byte order in VRAM (P-19).
void transfer32(Region region, uint32_t offset, const uint32_t* src, uint32_t words)
{
   ENGINE_ASSERT(region < Region::NUM_REGIONS && offset % kWordBytes == 0 &&
                 offset + words * kWordBytes <= region_bytes(region));
   for (uint32_t i = 0; i < words; ++i)
   {   // low half first: the GBA's byte order
      store16(region, offset + i * kWordBytes, static_cast<uint16_t>(src[i]));
      store16(region, offset + i * kWordBytes + kHalfwordBytes, static_cast<uint16_t>(src[i] >> soft::kHalfwordBits));
   }
}

void transfer16(Region region, uint32_t offset, const uint16_t* src, uint32_t halfwords)
{
   ENGINE_ASSERT(region < Region::NUM_REGIONS && offset % kHalfwordBytes == 0 &&
                 offset + halfwords * kHalfwordBytes <= region_bytes(region));
   for (uint32_t i = 0; i < halfwords; ++i)
   {
      store16(region, offset + i * kHalfwordBytes, src[i]);
   }
}

void fill32(Region region, uint32_t offset, uint32_t value, uint32_t words)
{
   ENGINE_ASSERT(region < Region::NUM_REGIONS && offset % kWordBytes == 0 &&
                 offset + words * kWordBytes <= region_bytes(region));
   for (uint32_t i = 0; i < words; ++i)
   {
      store16(region, offset + i * kWordBytes, static_cast<uint16_t>(value));
      store16(region, offset + i * kWordBytes + kHalfwordBytes, static_cast<uint16_t>(value >> soft::kHalfwordBits));
   }
}

Source classify(const void*, Source declared)
{
   return declared;
}

} // namespace capsule::hal::video

namespace capsule::hal::soft
{

uint16_t pending_bldcnt()
{
   return g_pending.bldcnt;
}

uint8_t pending_evy()
{
   return g_pending.evy;
}

int pending_window_width(uint8_t win)
{
   ENGINE_ASSERT(win < video::kNumWindows);
   const uint8_t* r = g_pending.win_rect[win];
   return r[kRectX1] - r[kRectX0];
}

uint16_t peek16(video::Region region, uint32_t offset)
{
   ENGINE_ASSERT(region < video::Region::NUM_REGIONS && offset % video::kHalfwordBytes == 0 &&
                 offset < video::region_bytes(region));
   if (region == video::Region::BgPalette)
   {
      return g_bg_palette[offset / video::kHalfwordBytes];
   }
   if (region == video::Region::ObjPalette)
   {
      return g_obj_palette[offset / video::kHalfwordBytes];
   }
   return load_le16((region == video::Region::BgVram ? g_vram : g_obj_vram) + offset);
}

} // namespace capsule::hal::soft
