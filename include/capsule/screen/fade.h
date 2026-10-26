/// @file
/// Palette fades as data (UC, D-33): a screen's fade in and out, stepped once a frame (the screen's Step::Fade).
///
/// A fade moves a blend amount y from `from` to `to` (0 = the loaded colors, 16 = the fade color) over the banks of
/// its mask, as the reference games' normal palette fade does: a step every delay + 1 calls, y moving by
/// FadeStyle::step (more for a negative delay: step - delay, with no wait), the BG banks blended on one call and the
/// OBJ banks on the next when FadeStyle::alternate is set (one step then takes two calls), and the fade staying
/// active for FadeStyle::finish_calls more calls once y reached `to`. begin() runs the first call at once.
#pragma once

#include "capsule/gfx/palette.h"
#include "capsule/hal/video.h"

#include <cstdint>

/// Screen palette fades.
namespace capsule::screen::fade
{

/// How a fade steps (a game's rule; data).
struct FadeStyle
{
   uint8_t step;            ///< y change per step (a negative delay adds to it).
   uint8_t finish_calls;    ///< Calls the fade stays active after its last step.
   bool alternate;          ///< BG banks on one call, OBJ banks on the next.
   gfx::palette::Rounding rounding;   ///< How the blend rounds (set on the palette module at begin()).
};

/// The reference games' normal palette fade: step 2, BG and OBJ halves on alternate calls, four more calls, the
/// blend an arithmetic shift.
inline constexpr FadeStyle kHandheldFade = {2, 4, true, gfx::palette::Rounding::Floor};

/// One fade (data).
struct Fade
{
   uint32_t mask;              ///< Banks: bits 0-15 BG, 16-31 OBJ.
   int8_t delay;               ///< Calls waited before each step (negative: a faster step, no wait).
   uint8_t from;               ///< Starting y, 0-16.
   uint8_t to;                 ///< Final y, 0-16.
   hal::video::Color color;    ///< The fade color.
};

/// @name Common fades
///@{
inline constexpr uint32_t kAllBanks = 0xFFFFFFFFu;   ///< Every BG and OBJ bank.
inline constexpr uint8_t kClear = 0;                 ///< y of the loaded colors.
inline constexpr uint8_t kFull = gfx::palette::kFadeFull;   ///< y of the fade color.
inline constexpr hal::video::Color kBlack = 0;       ///< Black.
/// White.
inline constexpr hal::video::Color kWhite = hal::video::rgb5(hal::video::kChannelMax, hal::video::kChannelMax,
                                                             hal::video::kChannelMax);
inline constexpr Fade kToBlack = {kAllBanks, 0, kClear, kFull, kBlack};     ///< Every bank to black.
inline constexpr Fade kFromBlack = {kAllBanks, 0, kFull, kClear, kBlack};   ///< Every bank from black.
///@}

/// Starts a fade and runs its first call (refused while one is active, as the reference games' is).
/// @param f      the fade
/// @param style  how it steps
/// @return false when a fade is active (nothing changes)
bool begin(const Fade& f, const FadeStyle& style = kHandheldFade);

/// One call: the next step of the active fade (nothing when none is active).
/// @return true while the fade is active after the call
bool update();

/// Whether a fade is active.
/// @return true until its last call
bool active();

/// Stops the active fade where it is (the banks keep their blend).
void cancel();

/// The current y.
/// @return 0-16
uint8_t level();

}   // namespace capsule::screen::fade
