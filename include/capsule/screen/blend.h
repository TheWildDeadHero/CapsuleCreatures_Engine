/// @file
/// Palette blends for scenes and animations (BF, D-38): blend palette banks or a sprite palette tag's bank toward a
/// colour by an amount, as the reference games' animation tasks do (BlendPalette / BlendPalettes: each channel moves
/// by ((colour - base) x amount) >> 4 from the loaded colours), fades by tag, and a stepped blend between two amounts.
///
/// A blend shows its colours at the next frame (the palette manager's visible colours: gfx::palette::blend_range());
/// it does not change a bank's fade, so the next fade step or palette load of that bank shows it anew (as the
/// reference games' fades rewrite their faded buffer).
#pragma once

#include "capsule/screen/fade.h"

#include <cstdint>

/// Palette blends.
namespace capsule::screen::blend
{

/// Blends every bank of a mask (bits 0-15 BG banks, 16-31 OBJ banks) toward a colour.
/// @param mask   the banks
/// @param coef   0 (loaded colours) to 16 (the colour)
/// @param color  the colour
void banks(uint32_t mask, uint8_t coef, hal::video::Color color);

/// Shows every bank's loaded colours, then blends the banks of a mask (the reference games' BlendPalettesUnfaded).
/// @param mask   the banks
/// @param coef   0 to 16
/// @param color  the colour
void banks_unfaded(uint32_t mask, uint8_t coef, hal::video::Color color);

/// Blends a range of palette entries (any length inside a palette).
/// @param obj    the OBJ palette (else BG)
/// @param first  first entry
/// @param count  entries
/// @param coef   0 to 16
/// @param color  the colour
void range(bool obj, int first, int count, uint8_t coef, hal::video::Color color);

/// The mask bit of a sprite palette tag's OBJ bank (capsule/screen/sprites.h load_palette()).
/// @param tag  the tag
/// @return the bit, or 0 when the tag has no bank
uint32_t tag_mask(uint16_t tag);

/// Blends a sprite palette tag's bank.
/// @param tag    the tag
/// @param coef   0 to 16
/// @param color  the colour
/// @return false when the tag has no bank
bool tag(uint16_t tag, uint8_t coef, hal::video::Color color);

/// Starts a palette fade (capsule/screen/fade.h) over the banks of some sprite palette tags and of a BG mask.
/// @param tags     the tags
/// @param count    entries of tags
/// @param bg_mask  BG banks (bits 0-15) to fade too
/// @param delay    the fade's delay
/// @param from     starting amount, 0-16
/// @param to       final amount, 0-16
/// @param color    the colour
/// @param style    how it steps
/// @return false when a fade is active (nothing starts)
bool fade_tags(const uint16_t* tags, int count, uint32_t bg_mask, int8_t delay, uint8_t from, uint8_t to,
               hal::video::Color color, const fade::FadeStyle& style = fade::kHandheldFade);

/// A blend stepped from one amount to another (data the caller keeps; step() once a frame).
struct Ramp
{
   uint32_t mask;            ///< Banks.
   hal::video::Color color;  ///< The colour.
   uint8_t coef;             ///< The next amount shown.
   uint8_t target;           ///< The last amount.
   uint8_t delay;            ///< Frames each amount stays less one.
   uint8_t wait;             ///< Frames left of the amount shown.
};

/// One frame of a ramp: when its wait is over, the next amount shown and one step toward the target taken.
/// @param r  the ramp
/// @return false once the target amount was shown (the ramp is done)
bool step(Ramp& r);

}   // namespace capsule::screen::blend
