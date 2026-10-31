/// @file
/// Test-app kit: the small on-screen console every test app shares, so each
/// app is only the code for the system it exercises.
///
/// The console is BG0 (priority 0, above everything): the default font in
/// character block 0 from tile 0, map in screen block 31. Palette banks 0-3
/// are the console inks (entry 1 = ink, entry 2 = shadow); entry 0 of bank 0
/// is the backdrop. Banks 4-15, BG1-BG3, character blocks 1-3 and screen
/// blocks 8-30 are free for the app.
///
/// Layout convention (30x20 cells): row 0 = app and page title, rows 1-17 =
/// the page, rows 18-19 = controls.
#pragma once
#include "capsule/hal/input.h"
#include "capsule/hal/video.h"
#include "capsule/text/encode.h"

#include <cstdint>

/// Shared console and pad helpers for the test apps (apps/test_*).
namespace capsule::testkit
{

using hal::input::KeyBits;
using text::Code;

/// Console inks: palette banks 0-3.
enum Ink : uint8_t
{
   INK_DIM,     ///< Labels.
   INK_ON,      ///< Values, active items.
   INK_OFF,     ///< Unavailable items, control hints.
   INK_TITLE,   ///< Titles.
   NUM_INKS     ///< Number of inks.
};

inline constexpr hal::video::BgIndex kConsoleBg = 0;   ///< Console layer.
inline constexpr uint8_t kConsoleCharBlock = 0;        ///< Character block with the font.
inline constexpr uint8_t kConsoleScreenBlock = 31;     ///< Console map.
inline constexpr int kCols = 30;                       ///< Visible columns.
inline constexpr int kRows = 20;                       ///< Visible rows.
inline constexpr uint8_t kFirstAppBank = 4;            ///< First palette bank free for the app.

inline constexpr int kColorsPerBank = 16;   ///< Colors in one 4bpp palette bank.
inline constexpr int kInkEntry = 1;         ///< Palette entry of a console ink's text color, within its bank.
inline constexpr int kShadowEntry = 2;      ///< Palette entry of a console ink's shadow, within its bank.

inline constexpr int kMapCells = 32;      ///< Cells along one side of a 32x32 BG map (one screen block).
inline constexpr int kScrollMask = 511;   ///< BG scroll offsets wrap at 512 pixels; masks a scroll counter into range.

inline constexpr int kTitleRow = 0;          ///< Row of the app and page title.
inline constexpr int kPageTop = 1;           ///< First row of the page.
inline constexpr int kPageBottom = 17;       ///< Last row of the page.
inline constexpr int kControlsTop = 18;      ///< First controls row.
inline constexpr int kControlsBottom = 19;   ///< Second (last) controls row.

/// Sets mode 0, the console palette and font, and turns BG0 on, cleared.
/// Call after system::init().
void init_console();

/// Where a console lives, for apps that need the default spots for themselves
/// (e.g. the map sandbox, whose map owns character blocks 0-1 and banks 0-12).
struct ConsoleLayout
{
   uint8_t char_block;     ///< Character block for the font (glyph 0 at tile 0).
   uint8_t screen_block;   ///< Screen block of BG0's map.
   uint8_t first_bank;     ///< Palette bank of INK_DIM; the other inks follow.
   bool backdrop;          ///< Also set the backdrop color (palette entry 0).
};

/// The layout init_console() uses.
inline constexpr ConsoleLayout kDefaultConsoleLayout = {kConsoleCharBlock, kConsoleScreenBlock, 0, true};

/// Sets up the console on BG0 with a given layout; the console keeps using it
/// until the next init_console().
/// @param layout  where the font, map and inks go
void init_console(const ConsoleLayout& layout);

/// Clears the whole console map.
void clear_console();

/// Clears console rows @p y0 to @p y1, inclusive.
/// @param y0  first row
/// @param y1  last row
void clear_rows(int y0, int y1);

/// Blanks columns [@p x0, @p x1) of a row.
/// @param x0  first column
/// @param x1  column after the last
/// @param y   row
void blank(int x0, int x1, int y);

/// Prints encoded text (text::print(): control escapes and prompts take no
/// cell, a page break starts the next row, field placeholders print their text).
/// @param ink    console ink
/// @param x      column
/// @param y      row
/// @param codes  text ending with kCodeEnd
/// @return the column after the last cell of the first line (text::line_cells())
int put(Ink ink, int x, int y, const Code* codes);

/// Prints a run-time ASCII string; characters outside printable ASCII show as '?'.
/// @param ink  console ink
/// @param x    column
/// @param y    row
/// @param s    NUL-terminated string (at most kCols characters are shown)
/// @return the column after the last character
int put_ascii(Ink ink, int x, int y, const char* s);

/// Prints an unsigned number.
/// @param ink    console ink
/// @param x      column
/// @param y      row
/// @param value  the number
/// @param width  right-align in this many columns (0 = no padding)
/// @return the column after the last character
int put_uint(Ink ink, int x, int y, uint32_t value, int width = 0);

/// Prints a signed number.
/// @param ink    console ink
/// @param x      column
/// @param y      row
/// @param value  the number
/// @param width  right-align in this many columns (0 = no padding)
/// @return the column after the last character
int put_int(Ink ink, int x, int y, int32_t value, int width = 0);

/// Prints a number in hexadecimal, upper case, zero-padded.
/// @param ink     console ink
/// @param x       column
/// @param y       row
/// @param value   the number
/// @param digits  digits to print, 1-8
/// @return the column after the last character
int put_hex(Ink ink, int x, int y, uint32_t value, int digits);

/// Writes the title row: "<app>  <page>/<pages> <page name>".
/// @param app        app name
/// @param page       current page, 0-based
/// @param pages      page count; 0 omits the page fields
/// @param page_name  current page's name (unused when pages == 0)
void title(const Code* app, int page, int pages, const Code* page_name);

/// Writes the controls rows 18-19 (kControlsTop, kControlsBottom).
/// @param line1  first line, or nullptr
/// @param line2  second line, or nullptr
void controls(const Code* line1, const Code* line2);

/// Raw buttons with edge detection; call update() once per frame after
/// system::update().
struct Pad
{
   KeyBits held = 0;           ///< Keys held this frame.
   KeyBits pressed = 0;        ///< Keys newly pressed this frame.
   uint8_t repeat_timer = 0;   ///< Frames the current key set has been held (see repeat()).

   /// Reads the HAL and updates held, pressed and the repeat timer.
   void update();

   /// Whether a key is held.
   /// @param k  the key
   /// @return true if held
   bool down(hal::input::Key k) const
   {
      return (held & hal::input::key_bit(k)) != 0;
   }

   /// Whether a key was newly pressed this frame.
   /// @param k  the key
   /// @return true on the press frame
   bool hit(hal::input::Key k) const
   {
      return (pressed & hal::input::key_bit(k)) != 0;
   }

   /// Newly pressed, then auto-repeating after a short hold (menus, counters).
   /// @param k  the key
   /// @return true on the press frame and on each repeat
   bool repeat(hal::input::Key k) const;
};

} // namespace capsule::testkit
