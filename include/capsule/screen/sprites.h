/// @file
/// A screen's sprites (UC, D-33): up to kMaxSprites sprites with their own tiles and palettes, animated by
/// animation commands and drawn in the reference games' order, for full-screen scenes that show more sprites than
/// gfx::sprites' sixteen slots (a box of icons, a list's icons and cursors).
///
/// While a scene runs, this module owns sprite memory, the OBJ palette and OAM (the field's gfx::sprites slots are
/// put back by gfx::sprites::reupload() when the field returns). The model follows the reference games' sprite
/// engine, so a scene's numbers can be used as they are:
/// - Tiles: a sheet is loaded under a tag (load_sheet(): first fit over the 1024 tiles, from tile 0); a sprite of a
///   sheet shows sheet start + its animation frame's value (a tile offset). A sprite from images (Template::images)
///   gets one frame's tiles of its own; its animation frame's value is a frame index, copied into its tiles (must-
///   land, so it shows with the next frame).
/// - Palettes: a bank per palette tag (load_palette(): the first free bank); Template::palette_tag picks the bank.
/// - Positions: x, y are the sprite's centre (the box's corner is x + x2 - width / 2, twice that for a 2x-size
///   affine sprite), plus set_offset()'s for sprites with coord_offset.
/// - animate(): each sprite in use, in index order: its callback, then (if still in use) its animation: a frame
///   shows for its duration (0 counts as 1), Jump goes to a command, Loop repeats the commands since the previous
///   Loop marker count more times, End holds the frame.
/// - build_oam(): sprites sorted by priority << 8 | subpriority, equal keys the lower sprite on screen first (OAM y
///   from 160 up counted as negative, as the reference games' sort; size-3 2x-size affine square / tall sprites
///   below 128 too), a stable insertion sort over the last frame's order; kOamLimit entries are written, the rest of
///   them hidden.
#pragma once

#include "capsule/asset/asset.h"
#include "capsule/hal/video.h"

#include <cstdint>

/// Screen sprites.
namespace capsule::screen::sprites
{

inline constexpr int kMaxSprites = 64;                   ///< Sprites.
inline constexpr uint8_t kNoSprite = kMaxSprites;        ///< create() when none is free.
inline constexpr int kOamLimit = 64;                     ///< OAM entries build_oam() writes.
inline constexpr int kTiles = hal::video::kObjTiles4bpp;   ///< 4bpp tiles of sprite memory.
inline constexpr int kSheets = kMaxSprites;              ///< Tile tags at once.
inline constexpr int kBanks = 16;                        ///< Palette banks.
inline constexpr int kMatrices = hal::video::kNumObjAffine;   ///< Affine matrices.
inline constexpr uint16_t kNoTag = 0xFFFF;               ///< No tag (Template: images / its own bank).
inline constexpr uint16_t kNoTiles = 0xFFFF;             ///< sheet_start() of an unknown tag.
inline constexpr uint8_t kNoBank = 0xFF;                 ///< A palette that could not be loaded.
inline constexpr int kSpriteData = 8;                    ///< Data words of a sprite (its callback's).

/// @name Affine modes (Template::affine)
///@{
inline constexpr uint8_t kAffineOff = 0;      ///< A plain sprite.
inline constexpr uint8_t kAffineNormal = 1;   ///< Transformed by its matrix.
inline constexpr uint8_t kAffineDouble = 3;   ///< Transformed, drawn in a box twice its size.
///@}

/// An animation command's kind.
enum class AnimOp : uint8_t
{
   Frame,   ///< Show value (a tile offset into the sheet, or a frame of the images) for duration frames.
   End,     ///< Hold the frame shown; the animation has ended.
   Jump,    ///< Go to command value.
   Loop,    ///< value 0: a loop's start marker; n: repeat back to the marker n more times.
   NUM_OPS  ///< Number of kinds.
};

/// @name AnimCmd::flips bits
///@{
inline constexpr uint8_t kFlipH = 1;   ///< Mirror horizontally.
inline constexpr uint8_t kFlipV = 2;   ///< Mirror vertically.
///@}

/// One animation command (data).
struct AnimCmd
{
   AnimOp op;          ///< Kind.
   uint8_t duration;   ///< Frame: frames shown.
   uint8_t flips;      ///< Frame: kFlip bits.
   uint8_t reserved;   ///< Padding; 0.
   int16_t value;      ///< Frame: image; Jump: command; Loop: count.
};

/// A sprite's frames as images: each frame frame_tiles 4bpp tiles, one after the other (ROM).
struct Images
{
   const uint32_t* words;   ///< frames x frame_tiles tiles.
   uint16_t frame_tiles;    ///< Tiles of one frame.
   uint16_t frames;         ///< Frames.
};

struct Sprite;
/// A sprite's per-frame function (animate() calls it before its animation).
using Callback = void (*)(Sprite& s);

/// What a sprite is made from (data; the reference games' sprite template).
struct Template
{
   uint16_t tile_tag;                    ///< Its sheet's tag, or kNoTag (images).
   uint16_t palette_tag;                 ///< Its palette's tag, or kNoTag (bank).
   hal::video::ObjShape shape;           ///< OBJ shape.
   uint8_t size;                         ///< OBJ size 0-3.
   uint8_t priority;                     ///< 0 (front) to 3.
   uint8_t bank;                         ///< The bank when palette_tag is kNoTag.
   uint8_t affine;                       ///< kAffineOff, kAffineNormal or kAffineDouble.
   hal::video::ObjMode mode;             ///< Drawing mode.
   uint8_t matrix;                       ///< Affine: its matrix 0-31.
   const AnimCmd* const* anims;          ///< Its animations (each ends with End or Jump), or nullptr (frame 0).
   const Images* images;                 ///< tile_tag kNoTag: its frames.
   Callback callback;                    ///< Per-frame function, or nullptr.
};

/// One sprite (the scene may change any field between frames).
struct Sprite
{
   const Template* tmpl;      ///< Its template.
   Callback callback;         ///< Per-frame function (the template's at create()).
   int16_t x;                 ///< Centre x.
   int16_t y;                 ///< Centre y.
   int16_t x2;                ///< Added to x.
   int16_t y2;                ///< Added to y.
   int16_t data[kSpriteData]; ///< The callback's words.
   uint16_t tile;             ///< First tile shown.
   uint16_t sheet_start;      ///< Its sheet's first tile (or its own tiles).
   uint8_t bank;              ///< Palette bank.
   uint8_t priority;          ///< 0 (front) to 3.
   uint8_t subpriority;       ///< Lower in front among equal priorities.
   uint8_t anim;              ///< Animation playing.
   uint8_t cmd;               ///< Its command.
   uint8_t delay;             ///< Frames left of the command's frame.
   uint8_t loop_left;         ///< Loop repeats left.
   uint8_t oam_y;             ///< Corner y as last built (the sort's key).
   uint8_t flips;             ///< kFlip bits of the frame shown.
   uint8_t mirror;            ///< kFlip bits the sprite adds (exclusive-or with the frame's).
   int8_t center_x;           ///< Centre to corner, x.
   int8_t center_y;           ///< Centre to corner, y.
   bool in_use;               ///< The entry holds a sprite.
   bool invisible;            ///< Not drawn (still animated).
   bool anim_beginning;       ///< Its animation starts at the next animate().
   bool anim_ended;           ///< It reached End.
   bool anim_paused;          ///< Its frames stay.
   bool coord_offset;         ///< set_offset() moves it.
};

/// Frees every sprite, sheet and palette; hides every OAM entry; the sort order and the offset start over.
void reset();

/// Loads a sheet's tiles under a tag (first fit; at once: the scene's setup).
/// @param tag    its tag
/// @param tiles  4bpp tiles
/// @return its first tile, or kNoTiles when they do not fit
uint16_t load_sheet(uint16_t tag, const asset::Tiles& tiles);

/// Loads raw 4bpp tile words under a tag (first fit; through the upload queue, must-land).
/// @param tag    its tag
/// @param words  count x 8 words (valid until they land)
/// @param count  tiles
/// @return its first tile, or kNoTiles
uint16_t load_sheet_words(uint16_t tag, const uint32_t* words, uint16_t count);

/// U6: copies raw 4bpp tile words into sprite memory from a tile on (a sheet's tiles changed in place, the reference
/// games' RequestDma3Copy to OBJ VRAM; through the upload queue, must-land: the words stay valid until it lands).
/// @param first_tile  the first tile written
/// @param words       count x 8 words
/// @param count       tiles
void copy_tiles(uint16_t first_tile, const uint32_t* words, uint16_t count);

/// U3 (D-33): takes a sheet's tiles under a tag without loading anything (first fit; the screen writes the tiles
/// itself, e.g. a table of icons it fills as they show, as the reference games' reserved tiles are used).
/// @param tag    its tag
/// @param count  tiles
/// @return its first tile, or kNoTiles when they do not fit
uint16_t reserve_sheet(uint16_t tag, uint16_t count);

/// Frees a sheet's tiles.
/// @param tag  its tag
void free_sheet(uint16_t tag);

/// A sheet's first tile.
/// @param tag  its tag
/// @return the tile, or kNoTiles
uint16_t sheet_start(uint16_t tag);

/// Loads 16 colours as a tagged palette (a tag already loaded keeps its bank).
/// @param tag     its tag
/// @param colors  16 colours (entry 0 is transparent but loaded)
/// @return its bank, or kNoBank when every bank is taken
uint8_t load_palette(uint16_t tag, const hal::video::Color* colors);

/// Takes a bank for a tag without loading colours.
/// @param tag  its tag
/// @return its bank, or kNoBank
uint8_t alloc_palette(uint16_t tag);

/// A tag's bank.
/// @param tag  its tag
/// @return the bank, or kNoBank
uint8_t palette_bank(uint16_t tag);

/// Frees a tag's bank (its colours stay).
/// @param tag  its tag
void free_palette(uint16_t tag);

/// Creates a sprite in the first free entry.
/// @param t            its template
/// @param x            centre x
/// @param y            centre y
/// @param subpriority  lower in front
/// @return its entry, or kNoSprite
uint8_t create(const Template& t, int16_t x, int16_t y, uint8_t subpriority);

/// Creates a sprite in the last free entry.
/// @param t            its template
/// @param x            centre x
/// @param y            centre y
/// @param subpriority  lower in front
/// @return its entry, or kNoSprite
uint8_t create_at_end(const Template& t, int16_t x, int16_t y, uint8_t subpriority);

/// Destroys a sprite (an images sprite's tiles are freed).
/// @param id  its entry
void destroy(uint8_t id);

/// A sprite.
/// @param id  0 to kMaxSprites - 1
/// @return its entry
Sprite& sprite(uint8_t id);

/// Starts one of a sprite's animations at the next animate().
/// @param id    its entry
/// @param anim  the animation
void start_anim(uint8_t id, uint8_t anim);

/// Runs every sprite's callback and animation once (the scene's Step::Animate).
void animate();

/// Sorts the sprites and writes OAM (the scene's Step::BuildOam).
void build_oam();

/// Sets the offset added to sprites with coord_offset.
/// @param x  pixels
/// @param y  pixels
void set_offset(int16_t x, int16_t y);

/// Sets an affine matrix (Q8).
/// @param matrix  0-31
/// @param pa      texture dx per screen x
/// @param pb      texture dx per screen y
/// @param pc      texture dy per screen x
/// @param pd      texture dy per screen y
void set_matrix(uint8_t matrix, int16_t pa, int16_t pb, int16_t pc, int16_t pd);

/// Sprites in use.
/// @return the count
int count();

/// The sprite the last build_oam() wrote to an OAM entry (tests, debug views).
/// @param entry  0 to kOamLimit - 1
/// @return its entry, or kNoSprite (hidden)
uint8_t drawn(int entry);

/// Tiles allocated.
/// @return the count
int tiles_used();

}   // namespace capsule::screen::sprites
