/// @file
/// Software emulation of the GBA video model (C-01b).
///
/// Shared by every HAL that is not the GBA itself: desktop (SDL2), Dreamcast,
/// headless. It implements the hal::video API (capsule/hal/video.h) over
/// emulated registers, VRAM and palette; a backend only latches the registers
/// once per frame and presents the composed frame.
///
/// VRAM holds the GBA's little-endian byte layout on every host (P-19), so the
/// same tiles and maps work on big-endian targets.
#pragma once

#include "capsule/hal/video.h"

#include <cstdint>

/// Software GBA video model used by the non-GBA HALs.
namespace capsule::hal::soft
{

/// Pixels in one composed frame (240 x 160).
inline constexpr int kFramePixels = video::kScreenWidth * video::kScreenHeight;

/// Power-on state: forced blank, registers cleared, palette black.
void reset();

/// Start of "VBlank": leave forced blank and make the pending register writes
/// live, exactly like the GBA HAL's register shadow.
void latch();

/// Composes the live state into a frame.
/// @param frame_bgr555  kFramePixels BGR555 pixels, row-major; written
void compose(uint16_t* frame_bgr555);

/// Reads one halfword of video memory (tests and debugging tools).
/// @param region  the region
/// @param offset  byte offset, even
/// @return the halfword (GBA value; palettes: the color)
uint16_t peek16(video::Region region, uint32_t offset);

/// BLDCNT as last written, not yet latched (tests): the blend mode is bits
/// 6-7 (video::BlendMode), the first targets bits 0-5, the second 8-13.
/// @return the register value
uint16_t pending_bldcnt();

/// The Brighten / Darken level as last written, not yet latched (tests; C8).
/// @return 0-16
uint8_t pending_evy();

/// A window's width as last written, not yet latched (tests; C8).
/// @param win  window 0 or 1
/// @return x1 - x0, pixels
int pending_window_width(uint8_t win);

/// @name Backend pixel formats
///@{
inline constexpr uint32_t kArgbOpaque = 0xFF000000u;   ///< ARGB8888 alpha byte, fully opaque.
inline constexpr int kArgbRedShift = 16;              ///< ARGB8888 bit position of red.
inline constexpr int kArgbGreenShift = 8;             ///< ARGB8888 bit position of green.
inline constexpr int kExpand8Shift = 3;               ///< 5-bit channel to 8 bits: c << 3 ...
inline constexpr int kExpand8Fill = 2;                ///< ... | c >> 2 (its top bits fill the low ones).
inline constexpr int kRgb565RedShift = 11;            ///< RGB565 bit position of red.
inline constexpr int kRgb565GreenShift = 5;           ///< RGB565 bit position of green.
inline constexpr int kExpand6Shift = 1;               ///< 5-bit green to 6 bits: g << 1 ...
inline constexpr int kExpand6Fill = 4;                ///< ... | g >> 4 (its top bit fills the low one).
///@}

/// Converts a BGR555 pixel for backends. 5-bit channels expand with
/// (c << 3) | (c >> 2).
/// @param c  BGR555 color
/// @return opaque ARGB8888
constexpr uint32_t bgr555_to_argb8888(uint16_t c)
{
   const uint32_t r = c & video::kChannelMax;
   const uint32_t g = (c >> video::kGreenShift) & video::kChannelMax;
   const uint32_t b = (c >> video::kBlueShift) & video::kChannelMax;
   return kArgbOpaque | (((r << kExpand8Shift) | (r >> kExpand8Fill)) << kArgbRedShift) |
          (((g << kExpand8Shift) | (g >> kExpand8Fill)) << kArgbGreenShift) |
          ((b << kExpand8Shift) | (b >> kExpand8Fill));
}

/// Converts a BGR555 pixel to RGB565; green expands with (g << 1) | (g >> 4).
/// @param c  BGR555 color
/// @return RGB565
constexpr uint16_t bgr555_to_rgb565(uint16_t c)
{
   const uint32_t r = c & video::kChannelMax;
   const uint32_t g = (c >> video::kGreenShift) & video::kChannelMax;
   const uint32_t b = (c >> video::kBlueShift) & video::kChannelMax;
   return static_cast<uint16_t>((r << kRgb565RedShift) |
                                (((g << kExpand6Shift) | (g >> kExpand6Fill)) << kRgb565GreenShift) | b);
}

} // namespace capsule::hal::soft
