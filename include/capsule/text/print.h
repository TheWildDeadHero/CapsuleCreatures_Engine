/// @file
/// Fixed-cell text on a background layer: one map cell per character.
///
/// This is the minimal printer used until the C-01c text box and C-1
/// variable-width renderer land.
#pragma once

#include "capsule/hal/video.h"
#include "capsule/text/encode.h"
#include "capsule/text/font.h"

namespace capsule::text
{

/// Where and how print() writes.
struct PrintTarget
{
   hal::video::BgIndex bg;                ///< Layer to write to.
   hal::video::TileIndex font_first_tile;   ///< Where load_font() put glyph 0.
   uint8_t palette_bank;      ///< Palette bank for the cells.
};

/// Writes codes until kCodeEnd, starting at tile (@p x, @p y). A newline
/// continues at column @p x of the next row. Every glyph code must be in the
/// font's range (asserted). The codes above the glyphs (capsule/text/encode.h)
/// take no cell: control escapes (FC nn) are skipped with their code, a
/// prompt (FA) is skipped (there is nothing to wait for) and a page break
/// (FB) continues on the next row like a newline. A field placeholder (FD
/// 80-FE) prints the field resolver's text (capsule/text/placeholder.h); a
/// battle placeholder (FD 00-7F) must have been replaced by battle::format()
/// (asserted; skipped in release builds).
/// @param target  layer, font tiles and palette bank
/// @param font    the loaded font
/// @param x       start column, in tiles
/// @param y       start row, in tiles
/// @param codes   encoded text ending with kCodeEnd
void print(const PrintTarget& target, const FontInfo& font, int x, int y, const Code* codes);

/// Fills the layer's first 32x32 cells with the font's blank glyph (code 0x20).
/// @param target  layer, font tiles and palette bank
/// @param font    the loaded font
void clear(const PrintTarget& target, const FontInfo& font);

} // namespace capsule::text
