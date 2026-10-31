/// @file
/// A screen's background layers (UC, D-33): its own BG plan (character block, screen block, size, colour mode,
/// priority and first tile per layer, as data), tile loads at any tile, and each layer's map kept in a RAM buffer
/// the screen owns, edited cell by cell or by rectangles and copied to video memory at once or at the next
/// Step::CopyTilemaps (scheduled copies), as the reference games' screens keep theirs.
///
/// Addressing: a layer's buffer holds its map as video memory does (32 x 32 cells per screen block, a wide layer's
/// right half in the next block). Functions taking cell coordinates map them through the layer's size
/// (cell_index()); copy_linear() and fill_linear() index the buffer as rows of 32 cells whatever the size, as the
/// reference games' rectangle copy and plain fill do.
#pragma once

#include "capsule/asset/asset.h"
#include "capsule/hal/video.h"

#include <cstdint>

/// Screen background layers.
namespace capsule::screen::layers
{

/// One layer's setup (data; the reference games' background template).
struct LayerTemplate
{
   hal::video::BgIndex bg;     ///< Layer 0-3.
   uint8_t char_block;         ///< Its tiles' character block, 0-3.
   uint8_t screen_block;       ///< Its map's first screen block, 0-31.
   uint8_t screen_size;        ///< 0 = 32x32 cells, 1 = 64x32, 2 = 32x64, 3 = 64x64 (affine: 16 << size square).
   uint8_t bpp8;               ///< 1: 8bpp tiles (an affine layer's).
   uint8_t priority;           ///< 0 (front) to 3.
   uint16_t first_tile;        ///< The tile load_tiles() and the fills count from (the template's base tile).
};

/// A screen's layer plan (data).
struct Plan
{
   const LayerTemplate* layers;   ///< The layers it uses.
   uint8_t count;                 ///< Entries of layers.
   uint8_t mode;                  ///< Display mode 0-2.
};

/// @name Copy modes of copy_rect() (the reference games' tilemap entry copy)
///@{
inline constexpr uint8_t kPaletteKeep = 16;   ///< Keep the destination's flips and palette bits, add palette_add.
inline constexpr uint8_t kPaletteRaw = 17;    ///< The source entry + offset, palette_add added to its bank bits.
/// A palette 0-15: the entry's tile and flips + offset (12 bits), its bank = palette + palette_add.
///@}

inline constexpr int kLayers = hal::video::kNumBgs;    ///< Layers.
inline constexpr int kRowCells = 32;                    ///< Cells per row of one screen block.

/// Applies a plan: the display mode, every layer of the plan configured (hidden until show()), scroll 0, the other
/// layers hidden and forgotten. The buffers are forgotten (set_buffer()).
/// @param plan  the plan
void apply(const Plan& plan);

/// Forgets every layer (all hidden, no buffers).
void reset();

/// U6: sets up one layer from its template and keeps the others (the reference games' InitBgFromTemplate: a screen
/// that changes some layers' templates while another layer stays as it is): the layer hidden until show(), scroll 0,
/// its buffer forgotten; the display mode stays (an affine layer of the mode gets the identity matrix).
/// @param t  the template
void init(const LayerTemplate& t);

/// U6: sets the display mode and keeps every layer's template, buffer and contents (SetBgMode); a layer that turns
/// affine or regular is reconfigured (hidden; scroll 0 or the identity matrix).
/// @param mode  0-2
void set_mode(uint8_t mode);

/// Shows or hides a layer.
/// @param bg  layer 0-3
/// @param on  show it
void show(hal::video::BgIndex bg, bool on);

/// Sets a layer's scroll offset.
/// @param bg  layer 0-3
/// @param x   pixels
/// @param y   pixels
void set_scroll(hal::video::BgIndex bg, int16_t x, int16_t y);

/// A layer's template as applied.
/// @param bg  layer 0-3
/// @return it, or nullptr when the plan has no such layer
const LayerTemplate* layer(hal::video::BgIndex bg);

/// Gives a layer its map buffer (the screen's storage, e.g. a static EWRAM array of map_bytes() bytes; it must
/// outlive the screen's use of the layer). The buffer is left as it is.
/// @param bg      layer 0-3
/// @param buffer  map_bytes(bg) bytes, 2-byte aligned
void set_buffer(hal::video::BgIndex bg, uint16_t* buffer);

/// A layer's buffer.
/// @param bg  layer 0-3
/// @return it, or nullptr
uint16_t* buffer(hal::video::BgIndex bg);

/// Bytes of a layer's map (its screen size; an affine layer's one byte a cell).
/// @param bg  layer 0-3
/// @return the bytes
uint32_t map_bytes(hal::video::BgIndex bg);

/// Cells across a layer's map.
/// @param bg  layer 0-3
/// @return 32 or 64 (affine: 16 to 128)
int width_cells(hal::video::BgIndex bg);

/// Cells down a layer's map.
/// @param bg  layer 0-3
/// @return 32 or 64 (affine: 16 to 128)
int height_cells(hal::video::BgIndex bg);

/// The buffer index of a cell, through the layer's size (a wide map's right half is the next screen block).
/// @param bg  layer 0-3
/// @param x   column (wraps at the map's width)
/// @param y   row (wraps at its height)
/// @return the index
int cell_index(hal::video::BgIndex bg, int x, int y);

/// C13a (R9 M4): whether a range of BG video memory that tiles are written to stays clear of every map in use: the
/// screen blocks of each layer of the plan (layers::apply() / init()) and of the field's shown map (its rings, when
/// a map is current and not suspended: a window drawn over the field). The engine's tile writes (load_tiles(),
/// queue_tiles(), text::window's copies) assert it in debug builds; a game's own tile writes may too.
/// @param offset  first byte (from the start of BG memory)
/// @param bytes   bytes written
/// @return true when no map's screen block overlaps the range
bool tiles_clear_of_maps(uint32_t offset, uint32_t bytes);

/// Loads tiles into a layer's character block now (setup, while nothing shows them).
/// @param bg      layer 0-3
/// @param tiles   the tiles (their colour mode must be the layer's)
/// @param offset  first tile, counted from the layer's first_tile
void load_tiles(hal::video::BgIndex bg, const asset::Tiles& tiles, uint16_t offset);

/// Loads a range of raw 4bpp / 8bpp tile words into a layer's character block, through the upload queue (lands at
/// the next vertical blank; must-land). The words must stay valid until then (ROM, or the screen's static storage).
/// @param bg      layer 0-3
/// @param words   the tiles' words
/// @param count   tiles
/// @param offset  first tile, counted from the layer's first_tile
void queue_tiles(hal::video::BgIndex bg, const uint32_t* words, uint16_t count, uint16_t offset);

/// Copies a stored tilemap into a layer's buffer as stored (its entries in order from @p offset).
/// @param bg      layer 0-3
/// @param map     the tilemap (text-mode entries for a regular layer, bytes for an affine one)
/// @param offset  first buffer entry written
void set_tilemap(hal::video::BgIndex bg, const asset::Tilemap& map, uint16_t offset = 0);

/// Writes one cell of a layer's buffer.
/// @param bg    layer 0-3
/// @param x     column
/// @param y     row
/// @param cell  the entry (an affine layer: its low byte)
void put(hal::video::BgIndex bg, int x, int y, hal::video::MapCell cell);

/// Reads one cell of a layer's buffer.
/// @param bg  layer 0-3
/// @param x   column
/// @param y   row
/// @return the entry
hal::video::MapCell get(hal::video::BgIndex bg, int x, int y);

/// Fills a rectangle with a run of tiles (the reference games' sequence fill): each cell gets @p first's flips and
/// tile with palette bank @p palette, the tile advancing by @p delta (10 bits, wrapping) cell by cell, row by row.
/// @param bg       layer 0-3
/// @param first    the first cell's entry (tile + flips; its bank bits are replaced)
/// @param x        left column
/// @param y        top row
/// @param w        columns
/// @param h        rows
/// @param palette  bank 0-15
/// @param delta    tile step per cell (0: one tile everywhere)
void fill(hal::video::BgIndex bg, hal::video::MapCell first, int x, int y, int w, int h, uint8_t palette,
          int16_t delta = 0);

/// Fills a rectangle with one raw entry, indexing the buffer as rows of 32 cells.
/// @param bg     layer 0-3
/// @param entry  the entry
/// @param x      left column
/// @param y      top row
/// @param w      columns
/// @param h      rows
void fill_linear(hal::video::BgIndex bg, hal::video::MapCell entry, int x, int y, int w, int h);

/// Copies a w x h block of raw entries (row by row) into a rectangle, indexing the buffer as rows of 32 cells.
/// @param bg   layer 0-3
/// @param src  w x h entries (an affine layer: bytes)
/// @param x    left column
/// @param y    top row
/// @param w    columns
/// @param h    rows
void copy_linear(hal::video::BgIndex bg, const void* src, int x, int y, int w, int h);

/// Copies part of a source map into a rectangle (the reference games' rectangle copy with a palette rule): the
/// source is src_w entries a row; its (src_x, src_y) cell goes to (x, y); each entry is rewritten by @p palette
/// (a bank 0-15, kPaletteKeep or kPaletteRaw), @p tile_offset and @p palette_add.
/// @param bg           layer 0-3
/// @param src          the source entries (an affine layer: bytes)
/// @param src_x        source column
/// @param src_y        source row
/// @param src_w        source entries a row
/// @param x            destination column
/// @param y            destination row
/// @param w            columns copied
/// @param h            rows copied
/// @param palette      bank, kPaletteKeep or kPaletteRaw
/// @param tile_offset  added to each tile
/// @param palette_add  added to the bank
void copy_rect(hal::video::BgIndex bg, const void* src, int src_x, int src_y, int src_w, int x, int y, int w, int h,
               uint8_t palette, int16_t tile_offset, int16_t palette_add);

/// Copies a layer's whole buffer to video memory through the upload queue (must-land: it shows at the next vertical
/// blank; the buffer must keep its contents until then).
/// @param bg  layer 0-3
void copy_to_vram(hal::video::BgIndex bg);

/// Asks for a layer's copy at the next copy_scheduled() (several asks: one copy).
/// @param bg  layer 0-3
void schedule_copy(hal::video::BgIndex bg);

/// Copies the scheduled layers (the screen's Step::CopyTilemaps).
void copy_scheduled();

/// Mirrors a window's cells into a layer's buffer (a text::window opened on the layer wrote them to video memory;
/// the buffer keeps them for its next copy): w x h tiles from @p first_tile, row by row, in bank @p palette.
/// @param bg          layer 0-3
/// @param x           left column
/// @param y           top row
/// @param w           columns
/// @param h           rows
/// @param first_tile  the window's first tile (in the layer's character block)
/// @param palette     bank 0-15
void put_window(hal::video::BgIndex bg, int x, int y, int w, int h, uint16_t first_tile, uint8_t palette);

}   // namespace capsule::screen::layers
