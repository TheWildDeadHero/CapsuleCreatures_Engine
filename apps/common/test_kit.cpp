/// @file
/// Test-app console and pad helpers (see test_kit.h).
#include "test_kit.h"

#include "capsule/text/font.h"
#include "capsule/text/placeholder.h"
#include "capsule/text/print.h"

namespace capsule::testkit
{
namespace
{

namespace video = hal::video;

constexpr video::Color kBackdrop = video::rgb5(3, 6, 12);   ///< Backdrop (deep blue).
constexpr video::Color kShadow = video::rgb5(1, 2, 5);      ///< Text shadow.
/// Ink colors by Ink.
constexpr video::Color kInk[NUM_INKS] = {
   video::rgb5(18, 20, 24),   // dim: labels
   video::rgb5(31, 28, 6),    // on: values, active
   video::rgb5(8, 10, 14),    // off: unavailable
   video::rgb5(31, 31, 31),   // title
};

constexpr uint8_t kRepeatDelay = 20;   ///< Frames before auto-repeat.
constexpr uint8_t kRepeatRate = 4;     ///< Frames between repeats.

constexpr Code kFirstPrintable = 0x20;   ///< First printable ASCII character (space).
constexpr Code kLastPrintable = 0x7E;    ///< Last printable ASCII character ('~').

constexpr uint32_t kDecimalBase = 10;   ///< Base of decimal numbers.
constexpr int kMaxDecimalDigits = 10;   ///< Decimal digits of the largest 32-bit number.
constexpr int kMaxHexDigits = 8;        ///< Hexadecimal digits of a 32-bit number.
constexpr int kBitsPerHexDigit = 4;     ///< Bits one hexadecimal digit stands for.
constexpr uint32_t kHexDigitMask = 15;   ///< Low bits of one hexadecimal digit.
constexpr int kTitlePageGap = 2;        ///< Columns between the app name and the page number.

ConsoleLayout g_layout = kDefaultConsoleLayout;   ///< Current console layout.

/// Print target for a console ink.
/// @param ink  the ink
/// @return BG0, font at tile 0, the ink's bank
text::PrintTarget target(Ink ink)
{
   return {kConsoleBg, 0, static_cast<uint8_t>(g_layout.first_bank + ink)};
}

} // namespace

void init_console()
{
   init_console(kDefaultConsoleLayout);
}

void init_console(const ConsoleLayout& layout)
{
   g_layout = layout;
   video::set_mode(0);
   if (layout.backdrop)
   {
      video::load_bg_palette(0, &kBackdrop, 1);
   }
   for (uint8_t ink = 0; ink < NUM_INKS; ++ink)
   {
      const auto bank = static_cast<uint8_t>(layout.first_bank + ink);
      video::load_bg_palette(static_cast<uint8_t>(bank * kColorsPerBank + kInkEntry), &kInk[ink], 1);
      video::load_bg_palette(static_cast<uint8_t>(bank * kColorsPerBank + kShadowEntry), &kShadow, 1);
   }
   text::load_font(text::kDefaultFont, text::kDefaultFontGlyphs, layout.char_block, 0);
   video::configure_bg(kConsoleBg, true, 0, layout.char_block, layout.screen_block, false, 0);
   video::set_bg_scroll(kConsoleBg, 0, 0);
   clear_console();
}

void clear_console()
{
   text::clear(target(INK_DIM), text::kDefaultFont);
}

void blank(int x0, int x1, int y)
{
   constexpr auto kSpace = text::encode(" ");
   for (int x = x0; x < x1; ++x)
   {
      put(INK_DIM, x, y, kSpace.codes);
   }
}

void clear_rows(int y0, int y1)
{
   for (int y = y0; y <= y1; ++y)
   {
      blank(0, kCols, y);
   }
}

int put(Ink ink, int x, int y, const Code* codes)
{
   text::print(target(ink), text::kDefaultFont, x, y, codes);
   // Cells, not codes: escapes and prompts take none, a field placeholder its text's.
   return x + text::line_cells(codes);
}

int put_ascii(Ink ink, int x, int y, const char* s)
{
   Code buf[kCols + 1];
   int n = 0;
   for (; s[n] != '\0' && n < kCols; ++n)
   {
      const auto c = static_cast<Code>(s[n]);
      buf[n] = (c >= kFirstPrintable && c <= kLastPrintable) ? c : static_cast<Code>('?');
   }
   buf[n] = text::kCodeEnd;
   return put(ink, x, y, buf);
}

int put_uint(Ink ink, int x, int y, uint32_t value, int width)
{
   Code digits[kMaxDecimalDigits + 1];
   int n = 0;
   do
   {
      digits[n++] = static_cast<Code>('0' + value % kDecimalBase);
      value /= kDecimalBase;
   }
   while (value != 0);
   Code buf[kCols + 1];
   int i = 0;
   for (int pad = width - n; pad > 0 && i < kCols - n; --pad)
   {
      buf[i++] = ' ';
   }
   while (n > 0)
   {
      buf[i++] = digits[--n];
   }
   buf[i] = text::kCodeEnd;
   return put(ink, x, y, buf);
}

int put_int(Ink ink, int x, int y, int32_t value, int width)
{
   if (value >= 0)
   {
      return put_uint(ink, x, y, static_cast<uint32_t>(value), width);
   }
   // Magnitude without overflowing on INT32_MIN.
   const uint32_t mag = 0u - static_cast<uint32_t>(value);
   Code digits[kMaxDecimalDigits + 1];
   int n = 0;
   uint32_t v = mag;
   do
   {
      digits[n++] = static_cast<Code>('0' + v % kDecimalBase);
      v /= kDecimalBase;
   }
   while (v != 0);
   Code buf[kCols + 1];
   int i = 0;
   for (int pad = width - n - 1; pad > 0 && i < kCols - n - 1; --pad)
   {
      buf[i++] = ' ';
   }
   buf[i++] = '-';
   while (n > 0)
   {
      buf[i++] = digits[--n];
   }
   buf[i] = text::kCodeEnd;
   return put(ink, x, y, buf);
}

int put_hex(Ink ink, int x, int y, uint32_t value, int digits)
{
   constexpr char kHex[] = "0123456789ABCDEF";
   Code buf[kMaxHexDigits + 1];
   if (digits > kMaxHexDigits)
   {
      digits = kMaxHexDigits;
   }
   for (int i = 0; i < digits; ++i)
   {
      // Digit i from the left: shift the wanted nibble down to bits 0-3.
      buf[i] = static_cast<Code>(kHex[(value >> (kBitsPerHexDigit * (digits - 1 - i))) & kHexDigitMask]);
   }
   buf[digits] = text::kCodeEnd;
   return put(ink, x, y, buf);
}

void title(const Code* app, int page, int pages, const Code* page_name)
{
   blank(0, kCols, kTitleRow);
   int x = put(INK_TITLE, 0, kTitleRow, app);
   if (pages > 0)
   {
      x = put_uint(INK_DIM, x + kTitlePageGap, kTitleRow, static_cast<uint32_t>(page + 1));
      constexpr auto kSlash = text::encode("/");
      x = put(INK_DIM, x, kTitleRow, kSlash.codes);
      x = put_uint(INK_DIM, x, kTitleRow, static_cast<uint32_t>(pages));
      put(INK_ON, x + 1, kTitleRow, page_name);
   }
}

void controls(const Code* line1, const Code* line2)
{
   clear_rows(kControlsTop, kControlsBottom);
   if (line1 != nullptr)
   {
      put(INK_OFF, 0, kControlsTop, line1);
   }
   if (line2 != nullptr)
   {
      put(INK_OFF, 0, kControlsBottom, line2);
   }
}

void Pad::update()
{
   const KeyBits now = hal::input::held();
   pressed = static_cast<KeyBits>(now & ~held);
   if (now != held)
   {
      repeat_timer = 0;
   }
   else if (++repeat_timer >= kRepeatDelay + kRepeatRate)
   {
      repeat_timer = kRepeatDelay;   // stays in [delay, delay + rate) while held (P-18)
   }
   held = now;
}

bool Pad::repeat(hal::input::Key k) const
{
   return hit(k) || (down(k) && repeat_timer == kRepeatDelay);
}

} // namespace capsule::testkit
