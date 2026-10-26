/// @file
/// Asset table (A-5, first part): compiled assets looked up by AssetKey.
///
/// tools/asset_compiler.py turns a capsule.assets/1 manifest (P-1) into ROM
/// tables: backgrounds (tiles, map, palette), sprites (frames, palette,
/// animations) and text (encoded strings). A build links exactly one table,
/// asset::kTable; the engine library carries an empty default that a build's
/// generated table replaces at link time (the same pattern as the sequence
/// game hooks). Lookups are a binary search over keys sorted at build time.
#pragma once

#include "capsule/core/pod.h"
#include "capsule/core/types.h"
#include "capsule/hal/video.h"
#include "capsule/text/encode.h"
#include "capsule/text/font.h"
#include "capsule/text/window.h"
#include "capsule/ui/menu.h"

#include <cstdint>

/// Compiled assets and their lookup.
namespace capsule::asset
{

/// What an asset is.
enum class Kind : uint8_t
{
   Background,   ///< A background layer image.
   Sprite,       ///< A sprite with frames and animations.
   Text,         ///< An encoded string.
   List,         ///< An ordered list of other assets' keys.
   Font,         ///< I4: a proportional font (text::PropFont).
   Door,         ///< DS1: a door animation (Door).
   TextBox,      ///< C10: a text box style (TextBox).
   Tiles,        ///< UC: tiles a screen loads where its plan says (Tiles).
   Tilemap,      ///< UC: a tilemap as stored (Tilemap).
   Palette,      ///< UC: palette colours, banks of 16 (Palette).
   NUM_KINDS     ///< Number of kinds.
};

/// One row of the sorted key index (ROM).
struct Entry
{
   AssetKey key;       ///< Asset Key v1 (C-02d).
   Kind kind;      ///< Which table index points into.
   uint8_t reserved;  ///< Padding; 0.
   uint16_t index;     ///< Index into that kind's table.
};
CAPSULE_POD(Entry, 8);

/// A background image (ROM). Regular layers use 4bpp tiles and a 32x32 map
/// whose cells carry palette bank 0 (the loader sets the real bank); affine
/// layers use 8bpp tiles (pixel values 0-15) and a byte map. An image may come
/// in both forms (`other`), so the same asset works on a regular layer and on
/// an affine one.
struct Background
{
   const Background* other;             ///< The same image in the other form, or nullptr.
   const uint32_t* tiles;               ///< 8 words per 4bpp tile, or 16 per 8bpp tile.
   const uint16_t* map;                 ///< Regular: width x height map cells; nullptr for affine.
   const uint8_t* affine_map;           ///< Affine: width x height tile indices; nullptr otherwise.
   const hal::video::Color* palette;    ///< 16 colors; entry 0 is never shown.
   uint16_t tile_count;                 ///< Tiles in tiles.
   uint8_t width;                      ///< Map width, cells.
   uint8_t height;                     ///< Map height, cells.
   uint8_t affine;                     ///< 1 for an affine (8bpp) image.
   uint8_t priority;                   ///< Layer priority the image wants, 0 (front) to 3 (back).
};

/// One sprite animation (ROM).
struct Animation
{
   const uint8_t* frames;      ///< Frame indices in order.
   const uint8_t* durations;   ///< Display frames per entry of frames (1-255 each), or nullptr to use period.
   uint8_t count;              ///< Number of frames.
   uint8_t period;             ///< Display frames per animation frame (1-255); unused when durations is set.
   uint8_t loop;               ///< 1 to loop, 0 to hold the last frame.
   const uint8_t* flips = nullptr;   ///< I3: kFlip... bits per entry of frames, or nullptr (none flipped).
};

/// @name Animation::flips bits (I3)
///@{
inline constexpr uint8_t kFlipH = 1;   ///< The entry's frame is drawn mirrored horizontally.
inline constexpr uint8_t kFlipV = 2;   ///< The entry's frame is drawn mirrored vertically.
///@}

/// Sprite::flags (I3): only the frame shown is in VRAM (gfx::sprites uploads a frame when the slot changes to it),
/// so frames x tiles may exceed a slot's tiles; one frame must fit.
inline constexpr uint8_t kSpriteStreamed = 1;

/// @name Sprite::steps entries (I3: step animations)
/// A walking sprite's step table, as the reference games keep one: the walk animations pause on one of two standing
/// entries between steps, and a new step starts at the entry after the other one, so the feet alternate across
/// stops (capsule/field/field.h).
///@{
inline constexpr int kStepFirstStand = 0;    ///< The first standing entry.
inline constexpr int kStepSecondStand = 1;   ///< The second standing entry.
inline constexpr int kStepAfterSecond = 2;   ///< Where a step starts after a pause on the second standing entry.
inline constexpr int kStepAfterFirst = 3;    ///< Where a step starts after a pause on the first standing entry.
inline constexpr int kStepEntries = 4;       ///< Entries of a step table.
///@}

/// A sprite (ROM): equally sized frames in 1D tile order, one 16-color palette.
struct Sprite
{
   const uint32_t* tiles;               ///< frame_count x tiles_per_frame 4bpp tiles.
   const hal::video::Color* palette;    ///< 16 colors; entry 0 is transparent.
   const Animation* animations;         ///< At least one; animation 0 plays on load.
   hal::video::ObjShape shape;          ///< OBJ shape.
   uint8_t size;                        ///< OBJ size 0-3.
   uint8_t frame_count;                 ///< Frames.
   uint8_t animation_count;             ///< Animations.
   uint8_t flags = 0;                   ///< I3: kSpriteStreamed.
   const uint8_t* steps = nullptr;      ///< I3: its step table (kStepEntries entries), or nullptr.
};

/// @name Door animations (DS1, D-30 section 1)
/// A door is a sheet of frames, each covering width x height metatile cells (its anchor, the cell a map binds it to,
/// is the bottom-left one), and the steps that open and close it. Each step shows a frame for some display frames;
/// the closed door (kDoorClosed) is the cells' own metatiles. How frames show (Door::show):
/// - DoorShow::Tiles: the frame's tiles are copied into the map's tile area at load_at (map::load_tiles(), the
///   reference games' way) and the cells are drawn with entries that name those tiles (map entries, the map's tile
///   0 at index 0);
/// - DoorShow::Override: the frames are map entries drawn over the door's cells (map::set_cell_override()), naming
///   tiles the map has.
///
/// Door::entries holds, per entry set, the cells top row first, left to right, each with Door::layers tile layers
/// of four map entries (top-left, top-right, bottom-left, bottom-right); a map's layers past Door::layers keep the
/// cell's own metatiles. Tiles mode has one entry set for all frames (entry_frames 1), Override one per frame.
///@{
inline constexpr uint8_t kDoorClosed = 0xFF;   ///< DoorStep::frame of the closed door.

/// How a door's frames show.
enum class DoorShow : uint8_t
{
   Tiles,      ///< Frame tiles loaded into the map's tile area; fixed entries name them.
   Override,   ///< Each frame's own map entries over the cells.
   NUM_DOOR_SHOWS   ///< Number of kinds.
};

/// One step of a door's opening or closing (ROM).
struct DoorStep
{
   uint8_t frame;   ///< Frame shown, or kDoorClosed.
   uint8_t ticks;   ///< Display frames it shows (1-255).
};
CAPSULE_POD(DoorStep, 2);

/// A door animation (ROM).
struct Door
{
   const uint32_t* tiles;        ///< Tiles: frame_count x frame_tiles 4bpp tiles; Override: nullptr.
   const uint16_t* entries;      ///< entry_frames x width x height x layers x 4 map entries.
   const DoorStep* open;         ///< Its opening, open_count steps (the last one shows the open door).
   const DoorStep* close;        ///< Its closing, close_count steps.
   AssetKey open_sound;          ///< Sound played when it starts opening ("sound:NAME"), or AssetKey{} (none).
   AssetKey close_sound;         ///< Sound played when it starts closing, or AssetKey{}.
   uint16_t load_at;             ///< Tiles: the first map tile a frame's tiles load into.
   uint8_t frame_count;          ///< Frames.
   uint8_t frame_tiles;          ///< Tiles: tiles per frame (0 for Override).
   uint8_t width;                ///< Cells across.
   uint8_t height;               ///< Cells down.
   uint8_t layers;               ///< Tile layers each cell's entries cover (1-4).
   uint8_t entry_frames;         ///< Entry sets: 1 (shared by every frame) or frame_count.
   uint8_t open_count;           ///< Steps of open.
   uint8_t close_count;          ///< Steps of close.
   DoorShow show;                ///< How frames show.
   uint8_t exit;                 ///< The direction a walker arriving on it walks out (field::Direction).
};
///@}

/// C10: a text box style (ROM): a game's message box or menu box as data (capsule.assets/1 kind "text_box"): the
/// window's settings (text::window::Config, its font and the fonts its Font control selects resolved to the
/// table's fonts) and its frame (text::window::Frame). A game opens a window with a copy of `window` (it may set
/// what its screen decides, e.g. the character block when char_block is kLayerCharBlock) and draws `frame`. N3: a
/// style may carry a menu (ui::MenuStyle: capsule/ui/menu.h) and its window an arrow image (Config::arrow_kind).
struct TextBox
{
   text::window::Config window;   ///< Where and how the text types.
   text::window::Frame frame;     ///< The frame around it.
   const ui::MenuStyle* menu;     ///< N3: how a menu in it behaves (the style's "menu" object), or nullptr.
   const hal::video::Color* palette;         ///< N3: the window's bank (16 colors, "palette"), or nullptr.
   const uint32_t* frame_tiles;              ///< N3: the frame's nine tiles (its "image"), or nullptr.
   const hal::video::Color* frame_palette;   ///< N3: the frame's bank (16 colors), or nullptr.
};
/// N3: tiles of a style's frame_tiles (text::window::kFrameParts).
inline constexpr int kFrameTileCount = text::window::kFrameParts;
/// TextBox::window.char_block of a style that leaves it to the layer's character block (the game's screen setup).
inline constexpr uint8_t kLayerCharBlock = 0xFF;

/// @name Screen graphics (UC, D-33)
/// What a full-screen scene (capsule/screen/screen.h) loads into the places its own plan gives: blocks of tiles at
/// any tile of a character block or of sprite memory, tilemaps copied into a layer's map as stored (each entry keeps
/// its tile, flips and palette bank), and palettes of any number of banks loaded at any bank. Unlike a Background
/// (one image, one bank, the loader's fixed VRAM plan), the three are separate, as a game's screens keep them.
///@{
inline constexpr uint8_t kTiles4bpp = 4;   ///< Tiles::bpp of 4bpp tiles (8 words each).
inline constexpr uint8_t kTiles8bpp = 8;   ///< Tiles::bpp of 8bpp tiles (16 words each).
inline constexpr int kTileWords4bpp = 8;    ///< 32-bit words of one 4bpp tile.
inline constexpr int kTileWords8bpp = 16;   ///< 32-bit words of one 8bpp tile.

/// UC: a block of tiles (ROM), in the order a screen loads them (row-major tiles of the source image, or its
/// frames' blocks one after the other).
struct Tiles
{
   const uint32_t* words;   ///< count tiles: kTileWords4bpp words each (4bpp) or kTileWords8bpp (8bpp).
   uint16_t count;          ///< Tiles.
   uint8_t bpp;             ///< kTiles4bpp or kTiles8bpp.
   uint8_t columns;         ///< Tiles per row of the source image (0: not an image's rows).
};

/// UC: a tilemap as stored (ROM): text-mode map entries (hal::video::MapCell: tile, flips, palette bank) in the
/// order a layer's map memory holds them (32 x 32 per screen block, the blocks one after the other), or one tile
/// byte per cell of an affine layer's map.
struct Tilemap
{
   const uint16_t* cells;         ///< count text-mode entries, or nullptr (an affine map).
   const uint8_t* affine_cells;   ///< count affine entries, or nullptr (a text-mode map).
   uint16_t count;                ///< Entries.
   uint8_t columns;               ///< Entries per row as stored (32 for a text-mode map in screen-block order).
   uint8_t reserved;              ///< Padding; 0.
};

/// UC: palette colours (ROM): count colours, bank by bank (16 a bank; the last bank may be shorter). Entry 0 of a
/// bank is kept as given (a background's bank 0 entry 0 is the backdrop).
struct Palette
{
   const hal::video::Color* colors;   ///< The colours.
   uint16_t count;                    ///< Colours.
   uint16_t reserved;                 ///< Padding; 0.
};
///@}

/// A list of asset keys (ROM), e.g. what a showcase loops over.
struct List
{
   const AssetKey* items;   ///< The keys, in order.
   uint16_t count;          ///< Number of keys.
};

/// The linked asset table.
struct Table
{
   const Entry* entries;                ///< Sorted by key.
   const Background* backgrounds;       ///< Background table.
   const Sprite* sprites;               ///< Sprite table.
   const text::Code* const* texts;      ///< Text table: strings ending with kCodeEnd.
   const List* lists;                   ///< List table.
   uint16_t count;                      ///< Entries.
   const text::PropFont* fonts = nullptr;   ///< I4: font table (a table without fonts leaves it out).
   const Door* doors = nullptr;             ///< DS1: door table (a table without doors leaves it out).
   const TextBox* text_boxes = nullptr;     ///< C10: text box styles (a table without them leaves it out).
   const Tiles* tiles = nullptr;            ///< UC: tile blocks (a table without them leaves them out).
   const Tilemap* tilemaps = nullptr;       ///< UC: tilemaps.
   const Palette* palettes = nullptr;       ///< UC: palettes.
};

/// FNV-1a 32 offset basis (the hash of no bytes).
inline constexpr uint32_t kFnvOffsetBasis = 0x811C9DC5u;
/// FNV-1a 32 prime, multiplied in after each byte.
inline constexpr uint32_t kFnvPrime = 0x01000193u;

/// Asset Key v1 (C-02d) of a name already in canonical form: FNV-1a 32 over
/// "namespace:NAME" with NAME upper case and single spaces, as the asset and
/// sequence compilers produce. For ASCII names only.
/// @param canonical  e.g. "sprite:SHOWCASE 01"
/// @return the key
constexpr AssetKey key_v1(const char* canonical)
{
   uint32_t h = kFnvOffsetBasis;
   for (const char* p = canonical; *p != '\0'; ++p)
   {
      h = (h ^ static_cast<uint8_t>(*p)) * kFnvPrime;
   }
   return AssetKey{h};
}

/// The build's asset table (generated; the engine's default is empty).
extern const Table kTable;

/// Looks a key up.
/// @param key  the asset key
/// @return its entry, or nullptr if the table has no such asset
const Entry* find(AssetKey key);

/// A background by key.
/// @param key  the asset key
/// @return the background, or nullptr if missing or not a background
const Background* background(AssetKey key);

/// A sprite by key.
/// @param key  the asset key
/// @return the sprite, or nullptr if missing or not a sprite
const Sprite* sprite(AssetKey key);

/// A text by key.
/// @param key  the asset key
/// @return the codes (ending with kCodeEnd), or nullptr if missing or not text
const text::Code* text(AssetKey key);

/// A list by key.
/// @param key  the asset key
/// @return the list, or nullptr if missing or not a list
const List* list(AssetKey key);

/// I4: a proportional font by key.
/// @param key  the asset key
/// @return the font, or nullptr if missing or not a font
const text::PropFont* font(AssetKey key);

/// DS1: a door animation by key.
/// @param key  the asset key
/// @return the door, or nullptr if missing or not a door
const Door* door(AssetKey key);
/// C10: a text box style by key.
/// @param key  the asset key
/// @return the style, or nullptr if missing or not a text box
const TextBox* text_box(AssetKey key);

/// UC: a block of tiles by key.
/// @param key  the asset key
/// @return the tiles, or nullptr if missing or not tiles
const Tiles* tiles(AssetKey key);

/// UC: a tilemap by key.
/// @param key  the asset key
/// @return the tilemap, or nullptr if missing or not a tilemap
const Tilemap* tilemap(AssetKey key);

/// UC: a palette by key.
/// @param key  the asset key
/// @return the palette, or nullptr if missing or not a palette
const Palette* palette(AssetKey key);

/// Tiles in one frame of a sprite.
/// @param s  the sprite
/// @return (width / 8) x (height / 8)
constexpr int tiles_per_frame(const Sprite& s)
{
   return (hal::video::obj_width(s.shape, s.size) / kTilePixels) *
          (hal::video::obj_height(s.shape, s.size) / kTilePixels);
}

} // namespace capsule::asset
