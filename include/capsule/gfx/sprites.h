/// @file
/// Sprite slots (A-8, first part): sprite assets placed, animated, scaled and
/// rotated through fixed slots.
///
/// Slot n uses OAM entry n (C2: the slots, then the instances, from set_first_entry()'s first). C2b: OBJ tile
/// memory is a heap (first fit over the kTiles tiles, as the reference games' sprite engine) and the OBJ palette
/// banks a pool: a slot takes the tiles it shows (a streamed sprite, asset::kSpriteStreamed (I3), one frame's,
/// uploaded (must-land) when the slot changes frames; any other sprite all its frames') and a bank shared with every
/// sprite of the same 15 colors (bank n first for slot n, when free). Animation entries may be drawn mirrored
/// (asset::Animation::flips). Instances (C2) draw further sprites without a slot: one frame each, its tiles shared
/// with every instance showing the same frame. Slots below kAffineSlots can scale and rotate (OBJ matrix n); a
/// transformed sprite is drawn in a box twice its size around its centre, so enlarging it never cuts it off. Call
/// update() once per frame: it advances animations and writes OAM.
#pragma once

#include "capsule/asset/asset.h"
#include "capsule/gfx/palette.h"
#include "capsule/hal/video.h"

#include <cstdint>

/// Sprite slots.
namespace capsule::gfx::sprites
{

inline constexpr int kSlots = 16;            ///< Sprite slots.
inline constexpr int kAffineSlots = 16;      ///< Slots that can scale / rotate.
inline constexpr int kInstances = 48;        ///< C2: instances (OAM entries after the slots').
inline constexpr int kTiles = hal::video::kObjTiles4bpp;   ///< C2b: OBJ tiles in the heap.
inline constexpr int kMaxSpriteTiles = kTiles;   ///< C2b: tiles one sprite may take (the asset compiler checks).
inline constexpr int kBanks = palette::kBanks;   ///< C2b: OBJ palette banks in the pool.
inline constexpr uint8_t kNoInstance = 0xFF; ///< C2: add_instance(): none free.
inline constexpr uint16_t kNoTiles = 0xFFFF; ///< C2b: reserve_tiles(): no room.
inline constexpr uint8_t kNoBank = 0xFF;     ///< C2b: palette_bank() / own_palette() / reserve_palette(): none.
/// C2: OAM entries the slots and the instances take (from set_first_entry()'s first).
inline constexpr int kEntries = kSlots + kInstances;
static_assert(kAffineSlots <= hal::video::kNumObjAffine && kEntries <= hal::video::kNumObjs);

/// Hides every slot, forgets what was loaded, frees every instance, every tile and every bank, turns the sprite
/// layer on.
void reset();

/// Loads a sprite into a slot and starts its animation 0. Position, shown
/// state, mode and transform are kept. C2b: the slot's tiles come from the heap (kept in place when the sprite
/// before took as many) and its palette bank from the pool.
/// @param slot    0 to kSlots - 1
/// @param sprite  the sprite asset
/// @return false when the heap had no room for its tiles (the slot then draws nothing until a load that fits;
///         animation calls still work) or no bank was free (it draws with bank 0's colors)
bool load(uint8_t slot, const asset::Sprite& sprite);

/// C13a: forgets a slot's sprite (hidden; nothing plays in it and reupload() skips it until the next load()).
/// C2b: its tiles and its bank go back to the heap and the pool.
/// @param slot  0 to kSlots - 1
void unload(uint8_t slot);

/// @name Tiles and palette banks (C2b)
///@{

/// The palette bank a slot draws with.
/// @param slot  the slot
/// @return 0 to kBanks - 1, or kNoBank (nothing loaded, or no bank was free: it draws with bank 0's)
uint8_t palette_bank(uint8_t slot);

/// Makes a slot's palette bank its own, so its colors can be changed without changing other sprites' (a sprite of
/// the same colors shares a bank): a shared bank is copied into a free one first. The bank stays the slot's until
/// it is unloaded or loaded again.
/// @param slot  a loaded slot
/// @return its bank, or kNoBank (nothing loaded, or no bank free to copy into)
uint8_t own_palette(uint8_t slot);

/// Reserves tiles for a slot whose tiles its user writes itself (a scene drawing its own sprites with the field's
/// slots): the slot's sprite is forgotten (as unload()) and count tiles are its own until it is unloaded or loaded.
/// reupload() does not restore them.
/// @param slot   the slot
/// @param count  1 to kTiles tiles
/// @return the first tile, or kNoTiles when the heap has no room
uint16_t reserve_tiles(uint8_t slot, uint16_t count);

/// Reserves a palette bank of its own for a slot whose colors its user loads itself (with reserve_tiles()); it is
/// the slot's until it is unloaded or loaded.
/// @param slot  the slot
/// @return the bank (bank slot first, when free), or kNoBank when every bank is taken
uint8_t reserve_palette(uint8_t slot);

/// Tiles the heap has free (tests, debug views).
/// @return 0 to kTiles
uint16_t free_tiles();

/// The longest run of free tiles (the largest sprite frame set that can load; tests, debug views).
/// @return 0 to kTiles
uint16_t largest_free_run();

/// Palette banks the pool has free (tests, debug views).
/// @return 0 to kBanks
uint8_t free_banks();
///@}

/// Shows or hides a slot.
/// @param slot  the slot
/// @param on    show it
void show(uint8_t slot, bool on);

/// Positions a slot.
/// @param slot  the slot
/// @param x     left edge of the untransformed sprite, pixels
/// @param y     top edge, pixels
void set_position(uint8_t slot, int16_t x, int16_t y);

/// Starts one of the slot's animations from its first frame.
/// @param slot       the slot
/// @param animation  animation index; out-of-range indices are ignored
void play(uint8_t slot, uint16_t animation);

/// I3: switches a slot to an animation at one of its entries (a step animation resuming); the entry shows for its
/// whole duration from this frame.
/// @param slot       the slot
/// @param animation  animation index; out-of-range indices are ignored
/// @param entry      entry of its frames (clamped to the last)
void play_at(uint8_t slot, uint16_t animation, uint8_t entry);

/// I3: pauses or resumes a slot's animation (a paused animation stays on its entry; the entry's time still runs
/// out, so a resumed animation whose entry is over moves on at once).
/// @param slot    the slot
/// @param paused  pause it
void pause(uint8_t slot, bool paused);

/// I3: plays a slot's animation at half speed (each entry for twice its duration) or normally.
/// @param slot  the slot
/// @param half  half speed
void set_half_speed(uint8_t slot, bool half);

/// I3: the animation a slot plays.
/// @param slot  the slot
/// @return its index
uint16_t animation(uint8_t slot);

/// I3: the entry of its animation a slot shows (the one shown last; it changes in update()).
/// @param slot  the slot
/// @return the entry
uint8_t entry(uint8_t slot);

/// Sets a slot's scale and rotation.
/// @param slot      below kAffineSlots
/// @param scale_q8  scale, Q8 (256 = 1.0; 0 hides the sprite)
/// @param angle     16-bit turn (counter-clockwise)
void set_transform(uint8_t slot, uint16_t scale_q8, uint16_t angle);

/// Sets a slot's priority against the background layers: a sprite is drawn
/// over every layer of equal or lower (larger number) priority. Default 0, in
/// front of everything; reset() restores it.
/// @param slot      the slot
/// @param priority  0 (front) to 3
void set_priority(uint8_t slot, uint8_t priority);

/// @name Draw order among sprites (DS1, D-30 section 4)
/// Unsorted (after reset()), OAM entry n is slot n, so a lower slot covers a higher one. Sorted (set_sorted(true),
/// the field's screens), OAM entries go out each update() in the order of the slots' priority, then their
/// sub-priority (set_order(); lower in front), then the lower sprite on screen in front (its top edge), then the
/// order of the frame before (C2: instances sort among them; unsorted, they follow the slots in instance order) (a
/// stable insertion sort over the last order, as the reference games sort their sprites). A slot keeps its tiles,
/// palette bank and OBJ matrix; only its OAM entry moves.
///@{

/// Sets a slot's sub-priority among the sprites of equal priority.
/// @param slot          the slot
/// @param sub_priority  0 (front) to 255
void set_order(uint8_t slot, uint8_t sub_priority);

/// Turns the sorted draw order on or off (off: OAM entry n is slot n).
/// @param on  sort
void set_sorted(bool on);

/// The OAM entry a slot was written to by the last update() (tests, debug views).
/// @param slot  the slot
/// @return 0 to kEntries - 1
uint8_t oam_entry(uint8_t slot);
///@}

/// Sets how a slot is drawn: normally, semi-transparent or as an OBJ window mask.
/// @param slot  the slot
/// @param mode  the mode
void set_mode(uint8_t slot, hal::video::ObjMode mode);

/// Advances animations one frame and writes every slot's OAM entry.
void update();

/// T1 (M5): moves the slots' (C2: and the instances') OAM entries to first ... first + kEntries - 1 (0 by default and after reset()), so a
/// scene's own sprites drawn over the slots can take the entries before them (the hardware draws lower entries in
/// front: the reference games sort a battle transition's sprites ahead of the field's objects). The entries left
/// are hidden; the next update() writes the new ones. The matrices stay 0 ... kAffineSlots - 1.
/// @param first  the first entry (first + kEntries <= hal::video::kNumObjs)
void set_first_entry(uint8_t first);

/// @name Instances (C2)
/// An instance draws one frame of a sprite with no slot of its own (C2b): a crowd of many sprites takes no slots.
/// Its frame's tiles are shared with every instance showing the same frame of the same sprite, its palette bank with
/// every sprite of the same colors. It does not animate, scale or rotate; it has its own position, shown state,
/// frame and flips, priority and sub-priority, and sorts with the slots (set_sorted()).
///@{

/// Adds a hidden instance of a sprite, showing no frame until set_instance_frame() (no flips, priority 0,
/// sub-priority 0).
/// @param sprite  the sprite
/// @return the instance, or kNoInstance when every one is taken
uint8_t add_instance(const asset::Sprite& sprite);

/// Frees an instance, its frame's tiles and its bank when no other sprite uses them (one reset() freed already
/// stays free).
/// @param instance  the instance
void remove_instance(uint8_t instance);

/// Shows or hides an instance.
/// @param instance  the instance
/// @param on        show it
void show_instance(uint8_t instance, bool on);

/// Positions an instance.
/// @param instance  the instance
/// @param x         left edge, pixels
/// @param y         top edge, pixels
void set_instance_position(uint8_t instance, int16_t x, int16_t y);

/// Sets the frame an instance shows (its tiles uploaded, must-land, unless an instance shows that frame already).
/// @param instance  the instance
/// @param frame     a frame of its sprite
/// @param flips     asset::kFlip... bits
/// @return false when the frame is not the sprite's or the heap had no room (the instance then draws nothing)
bool set_instance_frame(uint8_t instance, uint8_t frame, uint8_t flips);

/// Sets an instance's priority against the background layers (as set_priority()).
/// @param instance  the instance
/// @param priority  0 (front) to 3
void set_instance_priority(uint8_t instance, uint8_t priority);

/// Sets an instance's sub-priority among the sprites of equal priority (as set_order()).
/// @param instance      the instance
/// @param sub_priority  0 (front) to 255
void set_instance_order(uint8_t instance, uint8_t sub_priority);

/// The OAM entry an instance was written to by the last update() (tests, debug views).
/// @param instance  the instance
/// @return 0 to kEntries - 1
uint8_t instance_entry(uint8_t instance);
///@}

/// UC (D-33): puts every loaded slot's tiles back into OBJ memory (a streamed slot: the frame it shows; C2b: at the
/// heap's places, and every instance frame's) and hides every OAM entry, after a full-screen scene used the sprite
/// memory; the slots' state (sprite, position, animation, order) is as it was, and the next update() writes their
/// entries again. Tiles reserve_tiles() gave are not restored. Palettes are the caller's
/// (gfx::palette::restore()).
void reupload();

} // namespace capsule::gfx::sprites
