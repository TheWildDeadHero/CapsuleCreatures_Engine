/// @file
/// Background video model (P-8).
///
/// Plain functions, one implementation per target. The model is the GBA's
/// own: four background layers, 64 KB of background VRAM addressed as 16 KB
/// character blocks and 2 KB screen blocks, 256 BGR555 palette entries. The
/// desktop, Dreamcast and headless HALs emulate the same model and composite it
/// in software (src/hal/soft), so every target renders identical frames from
/// identical calls (C-01b).
///
/// Register writes are latched and take effect at the next system::update()
/// (start of VBlank). The VRAM, map and palette functions here write
/// immediately: they are for scene setup (forced blank, a hidden layer or a
/// faded screen). During play, uploads go through the VRAM upload queue
/// (capsule/hal/vram_queue.h, A-5), which lands them at VBlank. An immediate
/// write that overlaps a queued one first lands the queue, so every write
/// takes effect in program order.
#pragma once

#include <cstdint>

/// Background layers, tiles, maps and palettes.
namespace capsule::hal::video
{

using BgIndex = uint8_t;    ///< Background layer, 0-3.
using TileIndex = uint16_t;   ///< Tile 0-1023 within a character block's range.
using Color = uint16_t;   ///< BGR555 color; bit 15 unused (C-02c).
using MapCell = uint16_t;   ///< Text-mode map entry; see map_cell().

inline constexpr int kScreenWidth = 240;    ///< Screen width in pixels.
inline constexpr int kScreenHeight = 160;    ///< Screen height in pixels.
inline constexpr int kNumBgs = 4;      ///< Background layers.
inline constexpr int kTileBytes4bpp = 32;    ///< Bytes in one 8x8 4bpp tile.

/// @name Display modes (DISPCNT bits 0-2)
///@{
inline constexpr uint8_t kModeMixed = 1;    ///< Mode 1: BG0-BG1 regular, BG2 affine, no BG3.
inline constexpr uint8_t kModeAffine = 2;   ///< Mode 2: BG2-BG3 affine only; the highest supported mode.
///@}

/// @name Tiles and video memory layout
///@{
inline constexpr int kTileSize = 8;                     ///< Pixels per tile side.
inline constexpr int kTileShift = 3;                    ///< Pixel to tile: x >> kTileShift.
inline constexpr int kTilePixelMask = 7;                ///< Pixel within its tile: x & kTilePixelMask.
inline constexpr int kNumCharBlocks = 4;                ///< 16 KB character blocks in BG VRAM.
inline constexpr uint32_t kCharBlockBytes = 0x4000u;    ///< Bytes in one character block.
inline constexpr int kNumScreenBlocks = 32;             ///< 2 KB screen blocks in BG VRAM.
inline constexpr uint32_t kScreenBlockBytes = 0x800u;   ///< Bytes in one screen block.
inline constexpr int kScreenBlockTiles = 32;            ///< Map cells per side of one screen block.
inline constexpr int kScreenBlockShift = 5;             ///< Cell to screen block: x >> kScreenBlockShift.
inline constexpr int kScreenBlockTileMask = 31;         ///< Cell within its screen block: x & kScreenBlockTileMask.
inline constexpr int kScreenBlockCells = 1024;          ///< Map cells (halfwords) in one screen block, 32x32.
inline constexpr int kNumScreenSizes = 4;               ///< configure_bg() screen sizes, 0-3.
inline constexpr int kScreenSizeTall = 2;   ///< Screen size bit: twice as tall (bit 0 is twice as wide).
inline constexpr int kNumPriorities = 4;   ///< Layer and sprite priorities, 0-3.
inline constexpr int kPriorityMask = 3;   ///< Mask of a priority, 0-3.
///@}

/// @name BGR555 colors and palettes
///@{
inline constexpr int kChannelBits = 5;   ///< Bits in one color channel.
inline constexpr int kChannelMax = 31;   ///< Brightest channel value; also the mask of one channel.
inline constexpr int kRedShift = 0;   ///< Bit position of red.
inline constexpr int kGreenShift = 5;   ///< Bit position of green.
inline constexpr int kBlueShift = 10;   ///< Bit position of blue.
inline constexpr int kPaletteColors = 256;   ///< Entries in the BG palette, and in the OBJ palette.
inline constexpr int kColorsPerBank = 16;   ///< Entries in one 4bpp palette bank.
inline constexpr int kPaletteBankMask = 15;   ///< Mask of a palette bank number, 0-15.
///@}

/// @name Text-mode map entry (MapCell) bits
///@{
inline constexpr int kMapTileMask = 0x3FF;   ///< Bits 0-9: tile.
inline constexpr int kMapHflip = 0x400;   ///< Bit 10: mirror horizontally.
inline constexpr int kMapVflip = 0x800;   ///< Bit 11: mirror vertically.
inline constexpr int kMapPaletteShift = 12;   ///< Bits 12-15: palette bank.
///@}

/// Packs a BGR555 color.
/// @param r  red, 0-31
/// @param g  green, 0-31
/// @param b  blue, 0-31
/// @return the color
constexpr Color rgb5(uint8_t r, uint8_t g, uint8_t b)
{
   return static_cast<Color>((r & kChannelMax) | ((g & kChannelMax) << kGreenShift) |
                             ((b & kChannelMax) << kBlueShift));
}

/// Packs a text-mode map entry.
/// @param tile          tile 0-1023
/// @param palette_bank  palette bank 0-15
/// @param hflip         mirror the tile horizontally
/// @param vflip         mirror the tile vertically
/// @return the map cell
constexpr MapCell map_cell(TileIndex tile, uint8_t palette_bank, bool hflip = false, bool vflip = false)
{
   return static_cast<MapCell>((tile & kMapTileMask) | (hflip ? kMapHflip : 0) | (vflip ? kMapVflip : 0) |
                               ((palette_bank & kPaletteBankMask) << kMapPaletteShift));
}

/// Selects the display mode (C-01d). Bitmap modes 3-5 are unsupported.
/// @param mode  0, 1 or 2
void set_mode(uint8_t mode);

/// Configures one background layer.
/// @param bg            layer 0-3
/// @param on            show the layer
/// @param priority      0 (front) to 3 (back); ties go to the lower layer index
/// @param char_block    16 KB character block holding the tiles, 0-3
/// @param screen_block  first 2 KB screen block holding the map, 0-31
/// @param color_8bpp    8bpp tiles instead of 4bpp
/// @param screen_size   0 = 256x256, 1 = 512x256, 2 = 256x512, 3 = 512x512 pixels
void configure_bg(BgIndex bg, bool on, uint8_t priority, uint8_t char_block, uint8_t screen_block, bool color_8bpp,
                  uint8_t screen_size);

/// Sets a layer's scroll offset (latched).
/// @param bg  layer 0-3
/// @param x   horizontal offset in pixels
/// @param y   vertical offset in pixels
void set_bg_scroll(BgIndex bg, int16_t x, int16_t y);

/// A layer's horizontal scroll offset, as last set.
/// @param bg  layer 0-3
/// @return offset in pixels
int16_t bg_scroll_x(BgIndex bg);

/// A layer's vertical scroll offset, as last set.
/// @param bg  layer 0-3
/// @return offset in pixels
int16_t bg_scroll_y(BgIndex bg);

/// Copies whole 4bpp tiles into a character block.
/// @param char_block  character block 0-3
/// @param first_tile  first tile to write
/// @param data        8 words per tile, pixel 0 in the low nibble; 4-byte aligned
/// @param tile_count  number of tiles
void load_bg_tiles(uint8_t char_block, TileIndex first_tile, const uint32_t* data, uint16_t tile_count);

/// Writes colors into the background palette.
/// @param first_color  first entry, 0-255 (bank n = entries 16n..16n+15)
/// @param colors       the colors
/// @param count        number of entries
void load_bg_palette(uint8_t first_color, const Color* colors, uint16_t count);

/// Writes one map cell of a layer.
/// @param bg    layer 0-3
/// @param x     column, in tiles of the layer's configured screen size
/// @param y     row, in tiles
/// @param cell  the entry; see map_cell()
void write_map_cell(BgIndex bg, int x, int y, MapCell cell);

/// Writes a run of map cells along one row of a layer: the same as one
/// write_map_cell() per cell, but the VRAM queue (A-5) is checked once for
/// the whole row instead of once per cell.
/// @param bg     layer 0-3
/// @param x      first column; x + count must stay within the layer's width
/// @param y      row, in tiles
/// @param cells  count entries, left to right
/// @param count  cells to write (0: nothing)
void write_map_row(BgIndex bg, int x, int y, const MapCell* cells, int count);

/// Reads one map cell of a layer.
/// @param bg  layer 0-3
/// @param x   column, in tiles of the layer's configured screen size
/// @param y   row, in tiles
/// @return the entry
MapCell read_map_cell(BgIndex bg, int x, int y);

/// @name Affine backgrounds (BG2 in mode 1, BG2-BG3 in mode 2)
/// An affine layer uses 8bpp tiles (64 bytes each, pixel value = BG palette
/// entry) and a map of one byte per cell. Its configure_bg() screen_size
/// selects 0 = 128, 1 = 256, 2 = 512, 3 = 1024 pixels square; scroll offsets
/// are ignored in favour of the reference point.
///@{

inline constexpr int kTileBytes8bpp = 64;   ///< Bytes in one 8x8 8bpp tile.
inline constexpr int kFirstAffineBg = 2;   ///< BG2, the first layer that can be affine.
inline constexpr int kLastAffineBg = 3;   ///< BG3, the last layer that can be affine.
inline constexpr int kNumAffineBgs = 2;   ///< Layers that can be affine (BG2, BG3).

/// Sets an affine layer's transform (latched). Texture position of screen
/// pixel (x, y) = ref + (pa*x + pb*y, pc*x + pd*y), all in Q8.
/// @param bg     2 or 3
/// @param pa     texture dx per screen x, Q8 (256 = 1:1)
/// @param pb     texture dx per screen y, Q8
/// @param pc     texture dy per screen x, Q8
/// @param pd     texture dy per screen y, Q8
/// @param ref_x  texture x at screen (0, 0), Q8 (20.8)
/// @param ref_y  texture y at screen (0, 0), Q8 (20.8)
void set_bg_affine(BgIndex bg, int16_t pa, int16_t pb, int16_t pc, int16_t pd, int32_t ref_x, int32_t ref_y);

/// Whether an affine layer wraps around or is transparent outside its map
/// (latched).
/// @param bg    2 or 3
/// @param wrap  wrap around
void set_bg_wrap(BgIndex bg, bool wrap);

/// Copies whole 8bpp tiles into a character block.
/// @param char_block  character block 0-3
/// @param first_tile  first tile (64-byte units)
/// @param data        16 words per tile, pixel 0 in the low byte; 4-byte aligned
/// @param tile_count  number of tiles
void load_bg_tiles_8bpp(uint8_t char_block, TileIndex first_tile, const uint32_t* data, uint16_t tile_count);

/// Writes one cell of an affine layer's map.
/// @param bg    2 or 3
/// @param x     column, tiles
/// @param y     row, tiles
/// @param tile  8bpp tile index, 0-255
void write_affine_cell(BgIndex bg, int x, int y, uint8_t tile);
///@}

/// @name Sprites (OBJ)
/// 128 OAM entries and 32 affine matrices, latched like the registers;
/// 32 KB of OBJ tiles in 1D mapping (tile n of a sprite follows tile n-1);
/// 256 OBJ palette entries.
///@{

inline constexpr int kNumObjs = 128;   ///< OAM entries.
inline constexpr int kNumObjAffine = 32;   ///< OBJ affine matrices.
inline constexpr int kObjTiles4bpp = 1024;   ///< 4bpp tiles in OBJ VRAM.
inline constexpr int kObjXWrap = 512;   ///< Sprite x wraps at this many pixels (9 bits).
inline constexpr int kObjYWrap = 256;   ///< Sprite y wraps at this many pixels (8 bits).

/// @name OAM layout
/// Each OAM entry is four halfwords: attr0-attr2, then one parameter of an
/// affine matrix (matrix n = parameter of entries 4n..4n+3).
///@{
inline constexpr int kOamEntryHalfwords = 4;   ///< Halfwords per OAM entry.
inline constexpr int kOamAttr0 = 0;   ///< Halfword of attr0 in an entry.
inline constexpr int kOamAttr1 = 1;   ///< Halfword of attr1 in an entry.
inline constexpr int kOamAttr2 = 2;   ///< Halfword of attr2 in an entry.
inline constexpr int kOamAffineHalfword = 3;   ///< Halfword of the affine parameter in an entry.
inline constexpr int kAffineParams = 4;   ///< Parameters of one affine matrix (pa, pb, pc, pd), one per entry.
///@}

/// @name OAM attribute bits
///@{
inline constexpr int kObjYMask = 0xFF;   ///< attr0 bits 0-7: y.
inline constexpr int kObjAffineFlag = 0x100;   ///< attr0 bit 8: affine sprite.
inline constexpr int kObjDisable = 0x200;   ///< attr0 bit 9, regular sprite: hidden.
inline constexpr int kObjDoubleSize =
   0x200;   ///< attr0 bit 9, affine sprite: box twice the size (ObjDesc::double_size).
inline constexpr int kObjModeShift = 10;   ///< attr0 bits 10-11: ObjMode.
inline constexpr int kObjModeMask = 3;   ///< Mask of an ObjMode field.
inline constexpr int kObjMosaic = 0x1000;   ///< attr0 bit 12: mosaic.
inline constexpr int kObjShapeShift = 14;   ///< attr0 bits 14-15: ObjShape.
inline constexpr int kObjShapeMask = 3;   ///< Mask of an ObjShape field.
inline constexpr int kObjXMask = 0x1FF;   ///< attr1 bits 0-8: x.
inline constexpr int kObjAffineGroupShift = 9;   ///< attr1 bits 9-13, affine sprite: matrix.
inline constexpr int kObjAffineGroupMask = 31;   ///< Mask of an affine matrix number, 0-31.
inline constexpr int kObjHflip = 0x1000;   ///< attr1 bit 12, regular sprite: mirror horizontally.
inline constexpr int kObjVflip = 0x2000;   ///< attr1 bit 13, regular sprite: mirror vertically.
inline constexpr int kObjSizeShift = 14;   ///< attr1 bits 14-15: size.
inline constexpr int kObjSizeMask = 3;   ///< Mask of a size, 0-3.
inline constexpr int kObjTileMask = 0x3FF;   ///< attr2 bits 0-9: first tile.
inline constexpr int kObjPriorityShift = 10;   ///< attr2 bits 10-11: priority.
inline constexpr int kObjPaletteShift = 12;   ///< attr2 bits 12-15: palette bank.
///@}

/// One OAM entry in GBA attribute layout.
struct ObjAttr
{
   uint16_t attr0;   ///< Y, affine / hidden, mode, shape.
   uint16_t attr1;   ///< X, flips or affine matrix, size.
   uint16_t attr2;   ///< Tile, priority, palette bank.
};

/// Sprite shape; with the size (0-3) it selects the dimensions.
enum ObjShape : uint8_t
{
   OBJ_SQUARE,   ///< 8x8, 16x16, 32x32, 64x64.
   OBJ_WIDE,   ///< 16x8, 32x8, 32x16, 64x32.
   OBJ_TALL,   ///< 8x16, 8x32, 16x32, 32x64.
   NUM_OBJ_SHAPES   ///< Number of shapes.
};

/// How a sprite is drawn.
enum class ObjMode : uint8_t
{
   Normal,   ///< Opaque pixels drawn.
   SemiTransparent,   ///< Alpha-blended onto the layer below (uses the blend weights).
   Window,   ///< Not drawn; its opaque pixels form the OBJ window.
   NUM_OBJ_MODES   ///< Number of modes.
};

/// Readable description of a sprite, packed by make_obj().
struct ObjDesc
{
   int16_t x;   ///< Left edge, pixels (wraps at 512).
   int16_t y;   ///< Top edge, pixels (wraps at 256).
   TileIndex tile;   ///< First tile in OBJ VRAM (4bpp units).
   ObjShape shape;   ///< Shape.
   uint8_t size;   ///< Size 0-3.
   uint8_t palette_bank;   ///< OBJ palette bank 0-15.
   uint8_t priority;   ///< 0 (front) to 3; a sprite is drawn over a layer of equal priority.
   ObjMode mode;   ///< Drawing mode.
   bool hflip;   ///< Mirror horizontally (non-affine only).
   bool vflip;   ///< Mirror vertically (non-affine only).
   bool affine;   ///< Transform with matrix affine_group.
   bool double_size;   ///< Affine only: draw in a box twice the size, so scaled-up pixels are not cut off.
   uint8_t affine_group;   ///< Affine matrix 0-31.
};

/// Packs a sprite description into OAM attributes.
/// @param d  the description
/// @return the attributes
constexpr ObjAttr make_obj(const ObjDesc& d)
{
   const uint16_t a0 = static_cast<uint16_t>(
      (d.y & kObjYMask) | (d.affine ? kObjAffineFlag : 0) | (d.affine && d.double_size ? kObjDoubleSize : 0) |
      (static_cast<uint16_t>(d.mode) << kObjModeShift) | (static_cast<uint16_t>(d.shape) << kObjShapeShift));

   // attr1 bits 9-13 hold the matrix of an affine sprite, the flips of a regular one.
   const uint16_t flips = d.affine
                             ? static_cast<uint16_t>((d.affine_group & kObjAffineGroupMask) << kObjAffineGroupShift)
                             : static_cast<uint16_t>((d.hflip ? kObjHflip : 0) | (d.vflip ? kObjVflip : 0));
   const uint16_t a1 = static_cast<uint16_t>((d.x & kObjXMask) | flips | ((d.size & kObjSizeMask) << kObjSizeShift));

   const uint16_t a2 =
      static_cast<uint16_t>((d.tile & kObjTileMask) | ((d.priority & kPriorityMask) << kObjPriorityShift) |
                            ((d.palette_bank & kPaletteBankMask) << kObjPaletteShift));
   return ObjAttr{a0, a1, a2};
}

/// Attributes of a hidden sprite.
inline constexpr ObjAttr kObjHidden = {kObjDisable, 0, 0};

/// Width of a sprite shape and size.
/// @param shape  the shape
/// @param size   0-3
/// @return pixels
constexpr int obj_width(ObjShape shape, uint8_t size)
{
   constexpr uint8_t kW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
   return kW[shape][size & kObjSizeMask];
}

/// Height of a sprite shape and size.
/// @param shape  the shape
/// @param size   0-3
/// @return pixels
constexpr int obj_height(ObjShape shape, uint8_t size)
{
   constexpr uint8_t kH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
   return kH[shape][size & kObjSizeMask];
}

/// Turns the sprite layer on or off (latched).
/// @param on  show sprites
void set_obj_layer(bool on);

/// Sets one OAM entry (latched).
/// @param index  entry 0-127; lower entries are drawn over higher ones
/// @param attr   the attributes; kObjHidden hides it
void set_obj(uint8_t index, ObjAttr attr);

/// Sets one OBJ affine matrix (latched). Texture offset from the sprite
/// centre = (pa*dx + pb*dy, pc*dx + pd*dy) for screen offset (dx, dy).
/// @param group  matrix 0-31
/// @param pa     Q8 (256 = 1:1)
/// @param pb     Q8
/// @param pc     Q8
/// @param pd     Q8
void set_obj_affine(uint8_t group, int16_t pa, int16_t pb, int16_t pc, int16_t pd);

/// Copies whole 4bpp tiles into OBJ VRAM.
/// @param first_tile  first tile, 0-1023
/// @param data        8 words per tile, pixel 0 in the low nibble; 4-byte aligned
/// @param tile_count  number of tiles
void load_obj_tiles(TileIndex first_tile, const uint32_t* data, uint16_t tile_count);

/// Writes colors into the OBJ palette.
/// @param first_color  first entry, 0-255 (bank n = entries 16n..16n+15)
/// @param colors       the colors
/// @param count        number of entries
void load_obj_palette(uint8_t first_color, const Color* colors, uint16_t count);
///@}

/// @name Color effects and windows
/// Layer masks use bits 0-3 for BG0-BG3, bit 4 for sprites and bit 5 for the
/// backdrop (blending) or "effects allowed" (windows).
///@{

inline constexpr uint8_t kLayerObj = 0x10;   ///< Mask bit: sprites.
inline constexpr uint8_t kLayerBackdrop = 0x20;   ///< Blend mask bit: backdrop.
inline constexpr uint8_t kWindowEffects = 0x20;   ///< Window mask bit: color effects allowed.
inline constexpr uint8_t kLayerAll = 0x3F;   ///< Every bit.
inline constexpr int kBlendMax = 16;   ///< Largest alpha weight and brightness level (1.0).
inline constexpr int kMosaicMax = 16;   ///< Largest mosaic block size, pixels.
inline constexpr int kNumWindows = 2;   ///< Rectangular windows (0 and 1).

/// Color special effect.
enum class BlendMode : uint8_t
{
   Off,   ///< None.
   Alpha,   ///< First target alpha-blended over second target.
   Brighten,   ///< First targets fade toward white by the brightness level.
   Darken,   ///< First targets fade toward black by the brightness level.
   NUM_BLEND_MODES   ///< Number of modes.
};

/// Sets the color special effect (latched).
/// @param mode    the effect
/// @param first   first-target layer mask
/// @param second  second-target layer mask (alpha only)
void set_blend(BlendMode mode, uint8_t first, uint8_t second);

/// Sets the alpha weights (latched): result = first * eva / 16 + second * evb / 16.
/// @param eva  first-target weight, 0-16
/// @param evb  second-target weight, 0-16
void set_blend_alpha(uint8_t eva, uint8_t evb);

/// Sets the brightness level for Brighten / Darken (latched).
/// @param evy  0 (none) to 16 (white / black)
void set_blend_brightness(uint8_t evy);

/// Sets the mosaic block size of every layer and sprite (latched, D-17
/// warp transitions).
/// @param size  block size in pixels, 1 (off; 0 too) to 16
void set_mosaic(uint8_t size);

/// Sets a window rectangle (latched). Right and bottom edges are exclusive.
/// @param win  window 0 or 1
/// @param x0   left, 0-240
/// @param y0   top, 0-160
/// @param x1   right, x0-240
/// @param y1   bottom, y0-160
void set_window_rect(uint8_t win, uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1);

/// Sets what shows in each window region (latched). Masks: bits 0-4 layers
/// as above, bit 5 = color effects allowed.
/// @param in0      inside window 0
/// @param in1      inside window 1 (and not window 0)
/// @param outside  outside every window
/// @param obj      inside the OBJ window (and not window 0 or 1)
void set_window_masks(uint8_t in0, uint8_t in1, uint8_t outside, uint8_t obj);

/// Turns windows on or off (latched). With every window off, everything shows
/// everywhere.
/// @param win0     window 0
/// @param win1     window 1
/// @param obj_win  the OBJ window (sprites in ObjMode::Window)
void set_windows(bool win0, bool win1, bool obj_win);

/// @name Window 0 edges per scanline (N2, D-32)
/// Window 0's left and right edges may change from one scanline to the next (a circle of light around the
/// player): set_window_lines() gives a table of kWindowLines entries, entry y the edges of scanline y as the GBA's
/// WIN0H holds them, (left << kWindowLineShift) | right (right exclusive; as the hardware reads it, a right edge past
/// the screen or left of the left edge is the screen's right edge). While a table is set, window 0 keeps its rectangle's top and bottom
/// (set_window_rect()) and takes each line's edges from the table. The table is read as the frame shows (latched
/// with it: the caller leaves it unchanged until the next frame's update(), e.g. by writing a second table and
/// switching); nullptr goes back to the rectangle. GBA: an HBlank DMA into WIN0H; other targets: the compositor.
///@{
inline constexpr int kWindowLines = kScreenHeight + 1;   ///< Entries of a table (the last one is read but not shown).
inline constexpr int kWindowLineShift = 8;               ///< Shift of an entry's left edge.

/// Sets (or clears) window 0's per-scanline edges (latched).
/// @param lines  kWindowLines entries, or nullptr
void set_window_lines(const uint16_t* lines);
///@}
///@}

/// @name Raw video memory transfers (used by the VRAM upload queue)
/// Byte offsets into one video memory region, with no layer or tile
/// arithmetic. GBA: DMA channel 3; other targets: the software video model.
/// Offsets and sizes are multiples of the access width; the call writes
/// immediately and does not land the queue (vram::flush() calls these).
///@{

inline constexpr uint32_t kBgVramBytes = 0x10000u;   ///< Bytes of BG VRAM (64 KB).
inline constexpr uint32_t kObjVramBytes = 0x8000u;   ///< Bytes of OBJ VRAM (32 KB).
inline constexpr uint32_t kPaletteBytes = 0x200u;   ///< Bytes of one palette (256 colors).
inline constexpr uint32_t kWordBytes = 4u;   ///< Bytes in a 32-bit transfer unit.
inline constexpr uint32_t kHalfwordBytes = 2u;   ///< Bytes in a 16-bit transfer unit (and in one color).

/// A video memory region.
enum class Region : uint8_t
{
   BgVram,   ///< Background tiles and maps, 64 KB.
   ObjVram,   ///< Sprite tiles, 32 KB.
   BgPalette,   ///< Background palette, 512 B.
   ObjPalette,   ///< Sprite palette, 512 B.
   NUM_REGIONS   ///< Number of regions.
};

/// Size of a region in bytes.
/// @param r  the region
/// @return bytes
constexpr uint32_t region_bytes(Region r)
{
   return r == Region::BgVram ? kBgVramBytes : r == Region::ObjVram ? kObjVramBytes : kPaletteBytes;
}

/// Where source data lives, for the cost of copying it (A-5). On the GBA the
/// three memories differ in wait states; other targets copy every kind the
/// same way but use the same estimates, so uploads land on the same frames on
/// every target.
enum class Source : uint8_t
{
   Rom,   ///< Cartridge ROM (const data).
   Iwram,   ///< Fast internal work RAM.
   Ewram,   ///< External work RAM (16-bit bus, 2 wait states).
   NUM_SOURCES   ///< Number of kinds.
};

/// Copies 32-bit words.
/// @param region  destination region
/// @param offset  byte offset, a multiple of 4
/// @param src     source words, 4-byte aligned
/// @param words   number of words
void transfer32(Region region, uint32_t offset, const uint32_t* src, uint32_t words);

/// Copies 16-bit halfwords.
/// @param region     destination region
/// @param offset     byte offset, a multiple of 2
/// @param src        source halfwords, 2-byte aligned
/// @param halfwords  number of halfwords
void transfer16(Region region, uint32_t offset, const uint16_t* src, uint32_t halfwords);

/// Fills 32-bit words with one value.
/// @param region  destination region
/// @param offset  byte offset, a multiple of 4
/// @param value   the word
/// @param words   number of words
void fill32(Region region, uint32_t offset, uint32_t value, uint32_t words);

/// Where a pointer points, as far as this target can tell (GBA: by address;
/// other targets cannot tell and answer @p declared). Used by debug checks.
/// @param p         the pointer
/// @param declared  what the caller declared
/// @return the kind of memory
Source classify(const void* p, Source declared);
///@}

}   // namespace capsule::hal::video
