/// @file
/// Text on background layers and in the text box, from encoded text such as
/// text assets; optionally typed character by character.
///
/// Targets 0-3 are background layers (prepared with
/// gfx::background::prepare_text() when nothing is loaded there); target 4 is
/// the text box, drawn on BG0 with an opaque background. Text tiles live at
/// gfx::background::kTextTiles and up in each layer's character block: the
/// box frame first, then the default font. Styles 0-3 select the ink: style s
/// uses BG palette bank 15 - s (entries 1 ink, 2 shadow, 3 box fill, 4 box
/// frame). While the box is open, text printed on BG0 also gets the box
/// background, since both share BG0's font tiles.
///
/// Over a map (gfx::background's map plan): BG0 moves to the character and
/// screen blocks no map uses, so the box and BG0 text show over the map;
/// the other layers are the map's or unavailable. A style whose bank the map
/// uses (the map has more than 12 palettes) prints in style 0's bank. After
/// a map is entered or left, the next call reloads the style palettes and
/// tiles. Over a map whose tile layers use BG0 there is no text box (and no
/// text on other layers): that asserts, and release builds draw nothing.
///
/// Call update() once per frame; it types pending text at one character
/// every kTypeFrames frames.
///
/// Codes above the glyphs (capsule/text/encode.h): control escapes take no
/// cell and no time. A field placeholder (FD 80-FE) types the field
/// resolver's text in its place (capsule/text/placeholder.h). Typed text
/// stops at a prompt (FA) or a page break (FB): busy() stays true and
/// waiting() turns true until resume(), which goes on after a prompt, and
/// after a page break first clears the rows the page used and starts again
/// at the text's first row. The script port resumes on Accept / Back
/// (src/script/script_port_hal.cpp); other callers of typed text that may
/// hold FA or FB must call resume() themselves. Untyped text (and text
/// finished at once by a new print()) skips prompts and continues a page
/// break on the next row.
#pragma once

#include "capsule/hal/video.h"
#include "capsule/text/encode.h"

#include <cstdint>

/// Text on layers and in the text box.
namespace capsule::text::layer
{

inline constexpr uint8_t kBoxTarget = 4;   ///< Target index of the text box.
inline constexpr uint8_t kBannerTarget = 5;   ///< Target index of the banner (D-17).
inline constexpr uint8_t kBannerBank = 13;    ///< BG palette bank of the banner (style 2's while it shows).
inline constexpr int kStyles = 4;          ///< Text styles.
inline constexpr int kTypeFrames = 2;      ///< Frames per typed character.
/// First BG palette bank the text styles use (style n uses bank 15 - n); the
/// overworld's screen effects leave these banks alone, so text keeps its colors.
inline constexpr uint8_t kFirstTextBank = static_cast<uint8_t>(16 - kStyles);

/// Text alignment around the anchor column.
enum class Align : uint8_t
{
   Left,         ///< Starts at the anchor.
   Center,       ///< Centred on the anchor.
   Right,        ///< Ends at the anchor (inclusive).
   NUM_ALIGNS    ///< Number of alignments.
};

/// Loads the style palettes and closes the box; clears nothing on the layers.
void reset();

/// Selects the style for the next print().
/// @param style  0 to kStyles - 1 (others use 0)
void set_style(uint8_t style);

/// Prints text, replacing any typing in progress (which finishes instantly).
///
/// Typed text is read a character per update(), long after print() returns:
/// @p codes must then stay valid and unchanged until busy() is false (or the
/// next print(), clear() of the target, close_box() or reset()). Use ROM
/// (asset texts, script banks, constexpr encode() results with static
/// storage) or a static buffer, never a stack buffer of a function that
/// returns before the typing ends. Debug GBA builds assert that a typed
/// text is not on the stack; untyped text is read before print() returns.
/// @param target  0-3 = BG layer, kBoxTarget = text box, kBannerTarget = banner (coordinates inside their frames)
/// @param x       anchor column
/// @param y       row
/// @param align   alignment of each line around the anchor
/// @param typed   type it character by character instead of all at once
/// @param codes   encoded text ending with kCodeEnd (see above for how long it must live when typed)
void print(uint8_t target, uint8_t x, uint8_t y, Align align, bool typed, const Code* codes);

/// Clears rows of a target.
/// @param target     0-3 or kBoxTarget
/// @param first_row  first row
/// @param rows       rows to clear; 0 = all
void clear(uint8_t target, uint8_t first_row, uint8_t rows);

/// Opens the text box: frame and opaque interior on BG0.
/// An open banner closes first.
/// @param x  left, cells (frame included)
/// @param y  top, cells
/// @param w  width, cells (at least 3)
/// @param h  height, cells (at least 3)
void open_box(uint8_t x, uint8_t y, uint8_t w, uint8_t h);

/// Closes the text box and clears its cells.
void close_box();

/// Opens the banner (D-17, location names): a panel on BG0 with its own
/// frame and colors, in palette bank kBannerBank (style 2's: style 2 text
/// printed meanwhile uses style 0's colors, and style 2 text already on
/// screen shows in the banner's until close_banner() restores the bank). Text printed on
/// kBannerTarget shows inside it.
/// @param x       left, cells (frame included)
/// @param y       top, cells
/// @param w       width, cells (at least 3)
/// @param h       height, cells (at least 3)
/// @param frame   9 4bpp tiles (corners, edges, interior), or nullptr: the box frame
/// @param colors  palette entries 1-15 (1 ink, 2 shadow, 3 fill, 4 the box frame)
void open_banner(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint32_t* frame, const hal::video::Color* colors);

/// Closes the banner and clears its cells.
void close_banner();

/// Clears every target this module printed on and closes the box.
void clear_all();

/// Whether typed text is still being typed.
/// @return true while typing
bool busy();

/// Whether typed text waits at a prompt or page break for resume().
/// @return true while it waits
bool waiting();

/// Goes on with typed text that waits at a prompt or page break (a page
/// break first clears the page); does nothing otherwise.
void resume();

/// Whether the text box is open. Ask this rather than keeping a copy: reset()
/// and a change of the map plan (gfx::background) close the box too.
/// @return true while it is (false once the map plan changed since it opened)
bool box_open();

/// Whether the banner is open.
/// @return true while it is
bool banner_open();

/// Types pending text; call once per frame.
void update();

} // namespace capsule::text::layer
