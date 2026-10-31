/// @file
/// Background loader: puts background assets on layers with a fixed VRAM plan.
///
/// VRAM plan (one layer = one character block, maps at the top of BG VRAM):
/// | Layer | Tiles | Map |
/// |---|---|---|
/// | BGn regular | character block n, tiles 0..kMaxTiles-1 | screen block 28 + n (32x32) |
/// | BG2 affine (mode 1/2) | character block 2, 8bpp tiles 0-255 | screen block 30 |
/// | BG3 affine (mode 2) | character block 3, 8bpp tiles 0-127 | screen block 31 |
///
/// Tiles kTextTiles and up in each regular layer's block are reserved for
/// text (capsule/text/text_layer.h): text box frame, then the font. Layer n
/// uses palette bank n; affine images' pixel values are shifted into bank n
/// when loaded. A layer takes the priority its image declares (default 0;
/// ties go to the lower layer number); layers prepared for text use 0.
/// Sprites use priority 0 unless gfx::sprites::set_priority() says otherwise,
/// so by default they are in front of every layer.
#pragma once

#include "capsule/asset/asset.h"
#include "capsule/hal/video.h"

#include <cstdint>

/// Background layers loaded from assets.
namespace capsule::gfx::background
{

inline constexpr int kMaxTiles = 144;          ///< Image tiles per regular layer.
inline constexpr int kTextTiles = 144;         ///< First tile reserved for text in each block.
inline constexpr int kMaxAffineTiles = 128;   ///< Image tiles per affine layer (BG3's block is shared with the maps).
inline constexpr int kMapScreenBlock = 28;   ///< Screen block of BG0's map; BGn uses 28 + n.

/// @name Map plan (C-1, B-21)
/// While a map is shown (map::enter() to map::leave()), the map owns
/// character blocks 0-1, the screen blocks of its tile layers and the
/// palette banks it loads. Its BGs are reserved: nothing here may touch
/// them. If BG0 is free, it moves to character block kMapTextCharBlock and
/// screen block kMapTextScreenBlock (the spots no map uses), so text and the
/// text box can be shown over the map; BG0 is then the only layer this
/// module may set up (load() is refused: its palette bank is the map's).
///@{
inline constexpr uint8_t kMapTextCharBlock = 2;   ///< BG0's character block while a map is shown.
inline constexpr uint8_t kMapTextScreenBlock = 31;   ///< BG0's screen block while a map is shown.

/// Starts the map plan.
/// @param map_bgs        BGs with a map tile layer (bit per BG)
/// @param palette_banks  BG palette banks the map uses (0 to n-1)
void begin_map_plan(uint8_t map_bgs, uint8_t palette_banks);

/// Ends the map plan: every layer is back on its own blocks.
void end_map_plan();

/// Whether the map plan is on.
/// @return true between begin_map_plan() and end_map_plan()
bool map_plan();

/// Whether a layer belongs to the map.
/// @param bg  layer 0-3
/// @return true for a map tile layer under the map plan
bool is_reserved(hal::video::BgIndex bg);

/// First BG palette bank free for the UI (text styles).
/// @return the map's bank count under the map plan, else 0
uint8_t first_ui_bank();

/// Counts changes of the plan, so text can reload its tiles and palettes.
/// @return a counter that changes on every begin / end
uint16_t plan_version();
///@}

/// Character block of a layer.
/// @param bg  layer 0-3
/// @return its character block: @p bg, or kMapTextCharBlock for BG0 under the map plan
uint8_t char_block(hal::video::BgIndex bg);

/// Screen block of a layer's map.
/// @param bg  layer 0-3
/// @return 28 + @p bg, or kMapTextScreenBlock for BG0 under the map plan
uint8_t screen_block(hal::video::BgIndex bg);

/// Hides every layer and forgets what was loaded (the map's reserved layers
/// and the map plan stay).
void reset();

/// Sets the display mode (0-2) and remembers it, so load() knows which layers
/// are affine.
/// @param mode  display mode
void set_display_mode(uint8_t mode);

/// Whether a layer is affine in the current display mode.
/// @param bg  layer 0-3
/// @return true for BG2 in mode 1 and BG2-BG3 in mode 2
bool layer_is_affine(hal::video::BgIndex bg);

/// Loads a background onto a layer (tiles, map, palette bank n), in the form
/// the layer needs in the current display mode (see asset::Background::other).
/// The layer keeps its shown / hidden state and position.
/// @param bg     layer 0-3
/// @param image  the background
/// @return false if the image has no form for this layer (nothing loaded)
bool load(hal::video::BgIndex bg, const asset::Background& image);

/// Makes a layer a regular 4bpp layer filled with @p blank, if nothing is
/// loaded on it, so text can be printed there.
/// @param bg     layer 0-3
/// @param blank  map cell of a transparent tile (from the text tiles)
void prepare_text(hal::video::BgIndex bg, hal::video::MapCell blank);

/// Restores map cells to what the layer's image (or blank fill) has there,
/// e.g. after text or the text box covered them.
/// @param bg  layer 0-3 (regular)
/// @param x   left column
/// @param y   top row
/// @param w   columns
/// @param h   rows
void restore(hal::video::BgIndex bg, int x, int y, int w, int h);

/// Shows or hides a layer.
/// @param bg  layer 0-3
/// @param on  show it
void show(hal::video::BgIndex bg, bool on);

/// Positions a layer: the scroll offset of a regular layer, the reference
/// point of an affine one.
/// @param bg  layer 0-3
/// @param x   pixels
/// @param y   pixels
void set_position(hal::video::BgIndex bg, int16_t x, int16_t y);

/// Whether an affine image is loaded on a layer.
/// @param bg  layer 0-3
/// @return true for an affine layer
bool is_affine(hal::video::BgIndex bg);

/// Counts loads that overwrote the layer's whole character block (affine
/// images) and changes of the map plan, so text can tell when its tiles
/// there are gone.
/// @param bg  layer 0-3
/// @return a counter that changes on every such load
uint8_t block_version(hal::video::BgIndex bg);

/// Whether anything (image or text setup) is on a layer.
/// @param bg  layer 0-3
/// @return true once load() or prepare_text() ran since reset()
bool is_set_up(hal::video::BgIndex bg);

}   // namespace capsule::gfx::background
