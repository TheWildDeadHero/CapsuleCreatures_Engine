/// @file
/// Sprite slots (A-8, first part): sprite assets placed, animated, scaled and
/// rotated through fixed slots.
///
/// Slot n uses OAM entry n, OBJ palette bank n and OBJ tiles n x
/// kTilesPerSlot onward, so a sprite asset may use up to kTilesPerSlot tiles
/// across all its frames; a streamed sprite (asset::kSpriteStreamed, I3) keeps
/// only the frame shown there, uploaded (must-land) when the slot changes
/// frames, so one frame may use them all. Animation entries may be drawn
/// mirrored (asset::Animation::flips). Slots below kAffineSlots can scale and rotate (OBJ
/// matrix n); a transformed sprite is drawn in a box twice its size around its
/// centre, so enlarging it never cuts it off. Call update() once per frame: it advances
/// animations and writes OAM.
#pragma once

#include "capsule/asset/asset.h"
#include "capsule/hal/video.h"

#include <cstdint>

/// Sprite slots.
namespace capsule::gfx::sprites
{

inline constexpr int kSlots = 16;            ///< Sprite slots.
inline constexpr int kTilesPerSlot = 64;     ///< OBJ tiles per slot (e.g. 4 frames of 32x32).
inline constexpr int kAffineSlots = 16;      ///< Slots that can scale / rotate.
static_assert(kSlots * kTilesPerSlot <= hal::video::kObjTiles4bpp);
static_assert(kAffineSlots <= hal::video::kNumObjAffine && kSlots <= hal::video::kNumObjs);

/// Hides every slot, forgets what was loaded, turns the sprite layer on.
void reset();

/// Loads a sprite into a slot and starts its animation 0. Position, shown
/// state, mode and transform are kept.
/// @param slot    0 to kSlots - 1
/// @param sprite  the sprite asset
void load(uint8_t slot, const asset::Sprite& sprite);

/// C13a: forgets a slot's sprite (hidden; nothing plays in it and reupload() skips it until the next load()).
/// @param slot  0 to kSlots - 1
void unload(uint8_t slot);

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
/// order of the frame before (a stable insertion sort over the last order, as the reference games sort their
/// sprites). A slot keeps its tiles, palette bank and OBJ matrix; only its OAM entry moves.
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
/// @return 0 to kSlots - 1
uint8_t oam_entry(uint8_t slot);
///@}

/// Sets how a slot is drawn: normally, semi-transparent or as an OBJ window mask.
/// @param slot  the slot
/// @param mode  the mode
void set_mode(uint8_t slot, hal::video::ObjMode mode);

/// Advances animations one frame and writes every slot's OAM entry.
void update();

/// UC (D-33): puts every loaded slot's tiles back into OBJ memory (a streamed slot: the frame it shows) and hides
/// every OAM entry, after a full-screen scene used the sprite memory; the slots' state (sprite, position, animation,
/// order) is as it was, and the next update() writes their entries again. Palettes are the caller's
/// (gfx::palette::restore()).
void reupload();

} // namespace capsule::gfx::sprites
