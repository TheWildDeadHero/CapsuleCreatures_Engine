/// @file
/// Palette manager with software fade (A-6, first part).
///
/// Every palette write that should take part in fades goes through here: the
/// manager keeps the loaded ("base") colors and the visible colors, blended
/// toward a target color for the banks a fade covers. Fading a bank out and
/// back in therefore never loses its colors.
///
/// Visible colors reach palette RAM through the VRAM upload queue (A-5,
/// must-land): only the banks a call changes are queued, and they land at the
/// next VBlank together with the display registers.
#pragma once

#include "capsule/hal/video.h"

#include <cstdint>

/// Graphics modules built on the video HAL: palettes, backgrounds, sprites.
namespace capsule::gfx
{
}

/// Palettes with software fade.
namespace capsule::gfx::palette
{

/// @name Palette geometry
///@{
inline constexpr int kBanks = 16;              ///< Palette banks per palette (BG or OBJ).
inline constexpr int kBankColors = 16;         ///< Colors per bank.
inline constexpr int kColors = kBanks * kBankColors;   ///< Colors per palette (256).
inline constexpr int kObjBankShift = 16;       ///< Bank mask bit of OBJ bank 0 (BG banks use bits 0-15).
inline constexpr int kChannelBits = 5;         ///< Bits per color channel (BGR555).
inline constexpr int kColorBits = 15;          ///< Bits of a whole color (three channels).
inline constexpr int kChannelMask = 31;        ///< One channel's bits.
inline constexpr uint8_t kFadeFull = 16;       ///< Blend coefficient that gives the target color.
///@}

/// Bank mask bit for a BG palette bank.
/// @param bank  0-15
/// @return the mask bit (bits 0-15)
constexpr uint32_t bg_bank(int bank)
{
   return uint32_t{1} << bank;
}

/// Bank mask bit for an OBJ palette bank.
/// @param bank  0-15
/// @return the mask bit (bits 16-31)
constexpr uint32_t obj_bank(int bank)
{
   return uint32_t{1} << (kObjBankShift + bank);
}

/// Blends one color toward another.
/// @param base    the color
/// @param target  the color to blend toward
/// @param coef    0 (base) to 16 (target)
/// @return the blended color
constexpr hal::video::Color blend(hal::video::Color base, hal::video::Color target, uint8_t coef)
{
   uint32_t out = 0;
   // Each 5-bit channel moves coef / 16 of the way toward the target's.
   for (int s = 0; s < kColorBits; s += kChannelBits)
   {
      const int b = (base >> s) & kChannelMask;
      const int t = (target >> s) & kChannelMask;
      out |= static_cast<uint32_t>(b + (((t - b) * coef) / kFadeFull)) << s;
   }
   return static_cast<hal::video::Color>(out);
}

/// UC: blends one color toward another as games of the reference hardware do: each channel moves by
/// ((target - base) x coef) >> 4, an arithmetic shift (rounds toward minus infinity, so a fade toward black darkens a
/// step sooner than blend()).
/// @param base    the color
/// @param target  the color to blend toward
/// @param coef    0 (base) to 16 (target)
/// @return the blended color
constexpr hal::video::Color blend_floor(hal::video::Color base, hal::video::Color target, uint8_t coef)
{
   uint32_t out = 0;
   for (int s = 0; s < kColorBits; s += kChannelBits)
   {
      const int b = (base >> s) & kChannelMask;
      const int t = (target >> s) & kChannelMask;
      const int d = (t - b) * coef;
      // floor division by 16 without shifting a negative number: -((-d + 15) / 16) for d < 0
      const int step = d >= 0 ? d / kFadeFull : -((-d + kFadeFull - 1) / kFadeFull);
      out |= static_cast<uint32_t>(b + step) << s;
   }
   return static_cast<hal::video::Color>(out);
}

/// UC: how fades blend.
enum class Rounding : uint8_t
{
   Truncate,   ///< blend(): toward zero (the default).
   Floor,      ///< blend_floor(): an arithmetic shift, as the reference games' palette fades.
   NUM_ROUNDINGS   ///< Number of values.
};

/// UC: the base colors of every bank (a full-screen scene keeps the field's and gives them back).
struct Snapshot
{
   hal::video::Color bg[kColors];    ///< BG base colors.
   hal::video::Color obj[kColors];   ///< OBJ base colors.
};

/// Clears every palette to black and cancels the fade.
void reset();

/// Loads BG palette colors (base colors; the visible ones get the fade).
/// @param first   first entry, 0-255
/// @param colors  the colors
/// @param count   number of entries
void load_bg(uint8_t first, const hal::video::Color* colors, uint16_t count);

/// Loads OBJ palette colors (base colors; the visible ones get the fade).
/// @param first   first entry, 0-255
/// @param colors  the colors
/// @param count   number of entries
void load_obj(uint8_t first, const hal::video::Color* colors, uint16_t count);

/// Sets the software fade. Banks in @p mask show their base colors blended
/// toward @p target; every other bank shows its base colors.
/// @param mask    banks: bits 0-15 BG, 16-31 OBJ
/// @param target  color to blend toward
/// @param coef    0 (none) to 16 (all target)
void set_fade(uint32_t mask, hal::video::Color target, uint8_t coef);

/// UC: sets the fade of some banks only (the other banks keep theirs): banks in @p mask show their base colors
/// blended toward @p target by @p coef. A game whose fade steps the BG and OBJ banks on different frames calls it
/// once per half.
/// @param mask    banks: bits 0-15 BG, 16-31 OBJ
/// @param target  color to blend toward
/// @param coef    0 (none) to 16 (all target)
void set_bank_fade(uint32_t mask, hal::video::Color target, uint8_t coef);

/// UC: a bank's fade amount.
/// @param bank_bit  0-15 BG bank, 16-31 OBJ bank (kObjBankShift + n)
/// @return 0-16
uint8_t bank_coef(int bank_bit);

/// UC: sets how fades blend (reset() keeps it; a game sets it once).
/// @param r  the rounding
void set_rounding(Rounding r);

/// UC: how fades blend.
/// @return the rounding
Rounding rounding();

/// UC: copies every bank's base colors.
/// @param[out] out  the copy
void save(Snapshot& out);

/// UC: gives every bank its saved base colors (the visible colors get each bank's fade).
/// @param in  a copy save() made
void restore(const Snapshot& in);

/// UC: a base color as loaded.
/// @param obj    the OBJ palette (else BG)
/// @param index  entry 0-255
/// @return the color
hal::video::Color base(bool obj, int index);

} // namespace capsule::gfx::palette
