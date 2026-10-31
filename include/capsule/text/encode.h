/// @file
/// Text encoding (C-2, D-20).
///
/// Strings are encoded at build time (tools/text_encoder.py) into the code
/// page of the game's language: a manifest that gives each character an
/// 8-bit glyph code, 0x00-0xF9 (kMaxGlyphCode). The engine's default page is
/// printable ASCII (assets/text/ascii.codepage.json). Every page keeps the
/// space at 0x20 and the digits '0'-'9' at 0x30-0x39, so blanks and numbers
/// print the same in every language. Above the glyphs:
///
/// | Code | Meaning |
/// |---|---|
/// | FA | prompt: typed text waits for a button, then goes on (text::layer::resume()) |
/// | FB | page break: typed text waits for a button, then clears what it printed and goes on at its first row |
/// | FC nn [args] | control nn (I4: Control, with its argument bytes); printers skip controls they do not know, with their arguments (escape_length()) |
/// | FD nn | placeholder nn, see below |
/// | FE | newline |
/// | FF | end of string |
///
/// Placeholder arguments come in two ranges that never meet:
///
/// | nn | Placeholders | Replaced by |
/// |---|---|---|
/// | 00-7F | battle (Arg) | battle::format(), before the message is printed |
/// | 80-FE | field: names a game declares (field text, D-22 decision 6) | the printers, through the field resolver (capsule/text/placeholder.h) |
///
/// FD FF never occurs (a printer reading it stops at the end code). Printers
/// that cannot wait (text::print(), untyped text::layer::print()) skip FA
/// and continue a page break on the next row.
///
/// A character the code page cannot represent is a build error. The GBA HAL's
/// assert screen (src/hal/gba/hal_gba.cpp) prints ASCII strings through
/// kDefaultFont; with a game font whose code page does not keep ASCII
/// letters at their ASCII codes its message shows other glyphs (the space and
/// the digits, which every page keeps, still read correctly).
#pragma once

#include <cstddef>
#include <cstdint>

/// Text: encoding, fonts and printing.
namespace capsule::text
{

using Code = uint8_t;   ///< One code in the game's code page.

inline constexpr Code kMaxGlyphCode = 0xF9;   ///< Highest glyph code.
inline constexpr int kMaxFontGlyphs = kMaxGlyphCode + 1;   ///< Most glyphs a font holds (codes 0 to kMaxGlyphCode).
inline constexpr Code kCodePrompt = 0xFA;      ///< Reserved: wait for a button, then continue.
inline constexpr Code kCodePageBreak = 0xFB;   ///< Reserved: wait for a button, then clear the box.
inline constexpr Code kCodeControl = 0xFC;     ///< Control escape: the next code names the control.
inline constexpr Code kCodePlaceholder = 0xFD;   ///< Placeholder escape: the next code is an Arg.
inline constexpr Code kCodeNewline = 0xFE;   ///< Continue on the next row.
inline constexpr Code kCodeEnd = 0xFF;   ///< End of string.
inline constexpr Code kCodeSpace = 0x20;       ///< The space, in every code page.
inline constexpr Code kCodeDigitZero = 0x30;   ///< The digit '0', in every code page ('1'-'9' follow it).
inline constexpr int kFirstPrintable =
   0x20;   ///< First printable ASCII character (space): the ASCII page matches ASCII up to the last.
inline constexpr int kLastPrintable = 0x7E;    ///< Last printable ASCII character (tilde).
inline constexpr Code kLastBattleArg = 0x7F;   ///< Highest battle placeholder argument (Arg); battle arguments start at 0.
inline constexpr Code kFirstFieldArg = 0x80;   ///< Argument of field placeholder 0 (a game's first declared name).
inline constexpr Code kLastFieldArg = 0xFE;    ///< Highest field placeholder argument (kCodeEnd is never one).
inline constexpr int kMaxFieldArgs = kLastFieldArg - kFirstFieldArg + 1;   ///< Most field placeholders a game declares.
inline constexpr int kMaxPlaceholderCodes = 24;   ///< Most glyphs one field placeholder expands to.

/// Placeholder arguments: the code after kCodePlaceholder. Written as
/// {user}, {target}, ... in manifests that allow them.
enum class Arg : uint8_t
{
   User,      ///< {user}: the acting battler's name.
   Target,    ///< {target}: the affected battler's name.
   Move,      ///< {move}: a move name.
   Value,     ///< {value}: a number.
   Stat,      ///< {stat}: a stat name.
   Party,     ///< {party}: a party's name.
   Passive,   ///< {passive}: a passive's name.
   Item,      ///< {item}: an item name.
   Status,    ///< {status}: a status name.
   NUM_ARGS   ///< Number of arguments.
};
static_assert(static_cast<int>(Arg::NUM_ARGS) <= kLastBattleArg + 1, "battle arguments must stay below the field range");

/// Whether a placeholder argument is a field placeholder (kFirstFieldArg to kLastFieldArg).
/// @param arg  the code after kCodePlaceholder
/// @return true for a field placeholder, false for a battle one (or kCodeEnd)
constexpr bool is_field_arg(Code arg)
{
   return arg >= kFirstFieldArg && arg <= kLastFieldArg;
}

/// I4: the generic controls (FC nn and their argument bytes; tools/text_encoder.py CONTROL_CODES). Printers act on
/// the ones they know and skip the others with their arguments (escape_length()).
enum class Control : uint8_t
{
   None,          ///< 00: no control (skipped).
   Color,         ///< 01 c: text color.
   Highlight,     ///< 02 c: background color.
   Shadow,        ///< 03 c: shadow color.
   Colors,        ///< 04 c h s: all three.
   Palette,       ///< 05 p: palette.
   Font,          ///< 06 f: font.
   ResetFont,     ///< 07: back to the box's font.
   Pause,         ///< 08 n: wait n frames.
   WaitButton,    ///< 09: wait for a button (no prompt arrow).
   WaitSe,        ///< 0A: wait until the sound effect ends.
   Bgm,           ///< 0B lo hi: play music.
   Escape,        ///< 0C c: one glyph of the extra set.
   ShiftRight,    ///< 0D n: move to column n (pixels).
   ShiftDown,     ///< 0E n: move down n pixels.
   Fill,          ///< 0F: fill the window.
   Se,            ///< 10 lo hi: a sound effect.
   Clear,         ///< 11 n: clear n pixels.
   SkipTo,        ///< 12 n: skip to pixel n.
   ClearTo,       ///< 13 n: clear to pixel n.
   MinSpacing,    ///< 14 n: least letter width.
   LangJp,        ///< 15: Japanese spacing.
   LangEn,        ///< 16: back to the default.
   PauseMusic,    ///< 17: pause the music.
   ResumeMusic,   ///< 18: resume it.
   Scroll,        ///< 19: wait for a button, then scroll one line up (the next line continues below).
   Icon,          ///< 1A n: a button icon.
   NameEnd,       ///< 1B: the end of a name.
   Dynamic,       ///< 1C n: dynamic text n.
   NUM_CONTROLS   ///< Number of controls.
};

/// Argument bytes of each control, by Control.
inline constexpr uint8_t kControlArgs[] = {0, 1, 1, 1, 3, 1, 1, 0, 1, 0, 0, 2, 1, 1, 1, 0,
                                           2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1};
static_assert(sizeof(kControlArgs) == static_cast<int>(Control::NUM_CONTROLS), "one entry per control");

inline constexpr int kEscapeHead = 2;   ///< Codes of an escape before its arguments (FC or FD and the next code).

/// How many codes an escape at a position takes: FD and its argument; FC, its control and the control's argument
/// bytes (an unknown control: none); 1 when the string ends inside it.
/// @param p  the escape (kCodeControl or kCodePlaceholder)
/// @return its codes, at least 1
constexpr int escape_length(const Code* p)
{
   if (p[1] == kCodeEnd)
   {
      return 1;
   }
   if (p[0] != kCodeControl || p[1] >= static_cast<Code>(Control::NUM_CONTROLS))
   {
      return kEscapeHead;
   }
   const int n = kEscapeHead + kControlArgs[p[1]];
   for (int i = kEscapeHead; i < n; ++i)
   {
      if (p[i] == kCodeEnd)
      {
         return i;   // cut short: stop at the end code
      }
   }
   return n;
}

/// A string encoded at compile time by encode().
/// @tparam N  number of codes, including the terminating kCodeEnd
template <std::size_t N>
struct EncodedText
{
   Code codes[N];   ///< The codes, ending with kCodeEnd.
};

/// Implementation details of encode().
namespace detail
{
/// Deliberately not constexpr: reaching it during constant evaluation is the
/// compile error for unsupported characters. Never defined.
void unsupported_character_in_text();
}   // namespace detail

/// Encodes a string literal into the default (ASCII) code page at compile time.
/// @tparam N   literal length including its terminating NUL
/// @param utf8 the literal; printable ASCII and '\n' only
/// @return the encoded text, ending with kCodeEnd
template <std::size_t N>
consteval EncodedText<N> encode(const char (&utf8)[N])
{
   EncodedText<N> out{};
   for (std::size_t i = 0; i + 1 < N; ++i)
   {
      const char c = utf8[i];
      if (c == '\n')
      {
         out.codes[i] = kCodeNewline;
      }
      else if (c >= kFirstPrintable && c <= kLastPrintable)
      {
         out.codes[i] = static_cast<Code>(c);
      }
      else
      {
         detail::unsupported_character_in_text();
      }
   }
   out.codes[N - 1] = kCodeEnd;
   return out;
}

} // namespace capsule::text
