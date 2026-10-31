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
   uint8_t win_rect[kNumWindows][kRectEdges];   ///< WINnH / WINnV: x0, y0, x1, y1.
   uint8_t win_mask[kWindowRegions];            ///< WININ / WINOUT: in0, in1, outside, obj.
   const uint16_t* win_lines;                   ///< N2: window 0's edges per scanline, or nullptr (the rectangle).
};

/// OAM: 128 entries of attr0, attr1, attr2 and an interleaved affine
/// parameter, as on the GBA (matrix n = parameter of entries 4n..4n+3).
struct Oam
{
   uint16_t words[video::kNumObjs * kOamEntryHalfwords];   ///< Host-order halfwords; never byte-addressed.
};

Registers g_pending{};                 ///< Registers as last written.
Registers g_live{};                    ///< Registers the compositor uses.
Oam g_oam_pending{};                   ///< OAM as last written.
Oam g_oam_live{};                      ///< OAM the compositor uses.
uint8_t g_vram[kVramBytes];            ///< BG VRAM in GBA byte layout (little-endian), P-19.
uint8_t g_obj_vram[kObjVramBytes];     ///< OBJ VRAM in GBA byte layout.
video::Color g_bg_palette[kPaletteColors];    ///< BG palette.
video::Color g_obj_palette[kPaletteColors];   ///< OBJ palette.

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
   const uint16_t cnt = g_live.bgcnt[bg];
   const uint32_t size = (cnt >> kBgcntSizeShift) & kBgcntSizeMask;
   const int width = (size & 1) ? kTextBgLargePixels : kTextBgPixels;
   const int height = (size & kScreenSizeTall) ? kTextBgLargePixels : kTextBgPixels;

   // Map sizes are powers of two: the scrolled position wraps with a mask.
   const int sx = (x + g_live.hofs[bg]) & (width - 1);
   const int sy = (y + g_live.vofs[bg]) & (height - 1);

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
/// @param bg  layer 2 or 3
/// @param x   screen x
/// @param y   screen y
/// @return BG palette entry 1-255, or 0 when transparent / outside the map
int affine_bg_pixel(int bg, int x, int y)
{
   const uint16_t cnt = g_live.bgcnt[bg];
   const Affine& a = g_live.affine[bg - kFirstAffineBg];
   const int size = kAffineBgMinPixels << ((cnt >> kBgcntSizeShift) & kBgcntSizeMask);

   // Texture position in Q8, truncated to whole pixels.
   int tx = (a.x + a.pa * x + a.pb * y) >> kQ8Shift;
   int ty = (a.y + a.pc * x + a.pd * y) >> kQ8Shift;
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

/// Renders every sprite's pixels on one scanline. Lower OAM entries win.
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
   for (int i = video::kNumObjs - 1; i >= 0; --i)
   {   // back to front: lower entries overwrite
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
   const uint16_t d = g_live.dispcnt;
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
      const uint8_t* r = g_live.win_rect[w];
      if (w == 0 && g_live.win_lines != nullptr)
      {
         // N2: this scanline's edges (the hardware's rule: a right edge past the screen, or left of the left edge,
         // is the screen's right edge)
         const uint16_t e = g_live.win_lines[y];
         const int x0 = e >> video::kWindowLineShift;
         const int raw = e & kWindowEdgeMask;
         const int x1 = raw > video::kScreenWidth || raw < x0 ? video::kScreenWidth : raw;
         if (x >= x0 && x < x1 && y >= r[kRectY0] && y < r[kRectY1])
         {
            return g_live.win_mask[w];
         }
         continue;
      }
      if (x >= r[kRectX0] && x < r[kRectX1] && y >= r[kRectY0] && y < r[kRectY1])
      {
         return g_live.win_mask[w];
      }
   }
   if ((d & kDispcntObjWin) && in_obj)
   {
      return g_live.win_mask[kMaskObj];
   }
   return g_live.win_mask[kMaskOutside];
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
   // Background draw order: priority 0 first, ties broken by lower BG number.
   int order[video::kNumBgs];
   int prio_of[video::kNumBgs];
   int count = 0;
   for (int prio = 0; prio < kNumPriorities; ++prio)
   {
      for (int bg = 0; bg < video::kNumBgs; ++bg)
      {
         if (!(g_live.dispcnt & (kDispcntBg0 << bg)) || !bg_exists(mode, bg) ||
             (g_live.bgcnt[bg] & kPriorityMask) != prio)
         {
            continue;
         }
         prio_of[count] = prio;
         order[count++] = bg;
      }
   }
   const auto blend_mode = static_cast<video::BlendMode>((g_live.bldcnt >> kBldcntModeShift) & kBldcntModeMask);
   const uint8_t first_mask = static_cast<uint8_t>(g_live.bldcnt & kLayerAll);
   const uint8_t second_mask = static_cast<uint8_t>((g_live.bldcnt >> kBldcntSecondShift) & kLayerAll);

   // Weights above 16 count as 16, as on the GBA.
   const int eva = g_live.eva > kBlendMax ? kBlendMax : g_live.eva;
   const int evb = g_live.evb > kBlendMax ? kBlendMax : g_live.evb;
   const int evy = g_live.evy > kBlendMax ? kBlendMax : g_live.evy;
   const uint16_t backdrop = g_bg_palette[0];

   ObjLine obj;
   for (int y = 0; y < video::kScreenHeight; ++y)
   {
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
            const int index = is_text_bg(mode, bg) ? text_bg_pixel(bg, x, y) : affine_bg_pixel(bg, x, y);
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
   }
   // Mosaic (D-17 transitions): blocks take their top-left pixel. The GBA's
   // per-layer mosaic is aligned the same way for backgrounds.
   const int m = g_live.mosaic;
   if (m > 1)
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
   g_pending.win_lines = lines;
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
