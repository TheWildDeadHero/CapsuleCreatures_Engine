/// @file
/// Proportional text windows (I4, capsule/text/window.h): glyphs drawn at their widths in the window's colors,
/// typing speed, a prompt waiting for a button, a page break clearing, a newline past the last line scrolling, and
/// controls skipped with their arguments; run on the software video model.
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/text/encode.h"
#include "capsule/text/font.h"
#include "capsule/text/window.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace capsule::hal::system
{
/// Test build: report and abort.
/// @param expr  failed condition
/// @param file  file
/// @param line  line
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   std::printf("ASSERT %s:%d: %s\n", file, line, expr);
   std::abort();
}
} // namespace capsule::hal::system

namespace
{

namespace text = capsule::text;
namespace window = capsule::text::window;

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.

/// Records one check.
/// @param cond  condition that must hold
#define CHECK(cond)                                                                                                    \
   do                                                                                                                  \
   {                                                                                                                   \
      ++g_checks;                                                                                                      \
      if (!(cond))                                                                                                     \
      {                                                                                                                \
         ++g_failures;                                                                                                 \
         std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)

constexpr text::Code kA = 'A';   ///< The test font's first glyph.
constexpr text::Code kB = 'B';   ///< Its second.
/// The test font's glyphs: 8x2 cells; A is ink (1) in its first three pixels and shadow (2) under them, B is one
/// ink pixel at x 1. Rows are 2 bytes (8 pixels, 2 bits each, pixel 0 in bits 1:0).
/// C13a: C (third) has ink in pixels 0-5 of both rows but a width of 2: a window draws its first two columns only.
constexpr uint8_t kGlyphs[] = {0x15, 0x00, 0x2A, 0x00, 0x04, 0x00, 0x00, 0x00, 0x55, 0x05, 0x55, 0x05};
constexpr uint8_t kWidths[] = {3, 2, 2};   ///< Advances (A 3, B 2, C 2).
constexpr uint8_t kCellWidth = 8;        ///< The cell's width.
constexpr uint8_t kCellHeight = 2;       ///< Its height.
constexpr uint8_t kLineHeight = 4;       ///< Pixels per line.
constexpr uint8_t kSpacing = 1;          ///< Letter spacing.
/// The test font.
const text::PropFont kFont = {{kA, 3, kCellWidth, kCellHeight}, kGlyphs, kWidths, kLineHeight, kSpacing, 0, 0};
constexpr text::Code kC = 'C';   ///< C13a: the glyph with ink past its width.
/// C13a: the same glyphs drawn one row high (PropFont::glyph_height 1: the shadow row stays undrawn).
const text::PropFont kShortFont = {{kA, 3, kCellWidth, kCellHeight}, kGlyphs, kWidths, kLineHeight, kSpacing, 1, 0};

constexpr uint8_t kInk = 5;          ///< The window's ink entry.
constexpr uint8_t kShadow = 6;       ///< Its shadow entry.
constexpr uint8_t kBackground = 7;   ///< Its background entry.
constexpr uint8_t kFramesPerChar = 2;   ///< Typing speed.
constexpr uint32_t kNibbleMask = 15;    ///< One 4bpp pixel.
constexpr int kNibbleBits = 4;          ///< Bits of one.

constexpr int kTestWidth = 2;    ///< The test window's cells across.
constexpr int kTestHeight = 1;   ///< Its cells down.

window::Window g_window;                                  ///< The window under test.
window::TileBuffer<kTestWidth, kTestHeight> g_tiles;      ///< C13c: its tiles (the owner's storage).

/// A pixel of the window's RAM tiles.
/// @param x  column
/// @param y  row
/// @return its palette entry
uint8_t pixel(int x, int y)
{
   const int t = (y / 8) * g_window.c.width + x / 8;
   return static_cast<uint8_t>((g_window.store.words[t * window::kWordsPerTile + y % 8] >> ((x % 8) * kNibbleBits)) &
                               kNibbleMask);
}

/// Opens the test window: 2 x 1 cells (16 x 8 pixels), two lines.
void open()
{
   window::Config c;
   c.font = &kFont;
   c.width = kTestWidth;
   c.height = kTestHeight;
   c.lines = 2;
   c.ink = kInk;
   c.shadow = kShadow;
   c.background = kBackground;
   c.frames_per_char = kFramesPerChar;
   c.scroll_speed = 0;
   window::attach(g_window, g_tiles.tiles());
   window::open(g_window, c);
}

/// Glyphs at their widths and in the window's colors; controls skipped with their arguments; typing speed.
void drawing()
{
   open();
   CHECK(window::is_open(g_window) && pixel(0, 0) == kBackground);
   // A, a pause control with its argument (skipped by instant printing), B
   static constexpr text::Code kText[] = {kA, text::kCodeControl, static_cast<text::Code>(text::Control::Pause), 30,
                                          kB, text::kCodeEnd};
   window::print(g_window, kText, true);
   CHECK(!window::busy(g_window));
   CHECK(pixel(0, 0) == kInk && pixel(2, 0) == kInk && pixel(3, 0) == kBackground && pixel(0, 1) == kShadow);
   CHECK(pixel(5, 0) == kInk);   // B at 3 + 1 (spacing), its ink pixel at 1
   CHECK(window::line_width(kFont, kText) == kWidths[0] + kWidths[1] + 2 * kSpacing);
   // typed: one glyph every kFramesPerChar frames
   static constexpr text::Code kTwo[] = {kA, kA, text::kCodeEnd};
   window::print(g_window, kTwo, false);
   window::update(g_window, false, false);
   CHECK(pixel(0, 0) == kInk && pixel(4, 0) == kBackground && window::busy(g_window));
   window::update(g_window, false, false);
   window::update(g_window, false, false);
   CHECK(pixel(4, 0) == kInk);
}

/// A prompt waits for a button; a page break clears; a newline past the last line scrolls.
void pauses()
{
   open();
   static constexpr text::Code kText[] = {kA, text::kCodePrompt, kB, text::kCodePageBreak, kB, text::kCodeEnd};
   window::print(g_window, kText, true);
   CHECK(window::waiting(g_window) && pixel(1, 0) == kInk);
   window::update(g_window, true, false);    // the prompt goes on: B
   CHECK(window::waiting(g_window) && pixel(5, 0) == kInk);
   window::update(g_window, true, false);    // the page break: cleared, then B at the start
   CHECK(!window::busy(g_window) && pixel(1, 0) == kInk && pixel(5, 0) == kBackground);
   // three lines in a two-line window: the third scrolls the first out
   static constexpr text::Code kLines[] = {kA, text::kCodeNewline, kB, text::kCodeNewline, kA, text::kCodeEnd};
   window::print(g_window, kLines, true);
   window::update(g_window, false, false);
   window::update(g_window, false, false);
   CHECK(!window::busy(g_window));
   CHECK(pixel(1, 0) == kInk);                 // B, scrolled up from the second line
   CHECK(pixel(0, kLineHeight) == kInk);       // A on the last line
}

/// A frame with a left padding: the border ring one cell outside the padded rectangle, the padding's cells the
/// inside tile, the top row above the padding the top tile, the window's own cells untouched.
void frames()
{
   namespace video = capsule::hal::video;
   constexpr video::TileIndex kFirstPart = 40;   // the frame's nine tiles
   constexpr uint8_t kBank = 3;                  // their palette bank
   constexpr uint8_t kX = 4;                     // the window's left cell
   constexpr uint8_t kY = 6;                     // its top cell
   constexpr int kTopLeft = 0;                   // the parts used below
   constexpr int kTop = 1;
   constexpr int kTopRight = 2;
   constexpr int kLeft = 3;
   constexpr int kInside = 4;
   constexpr int kRight = 5;
   constexpr int kBottomLeft = 6;
   window::Config c;
   c.x = kX;
   c.y = kY;
   c.width = 2;
   c.height = 1;
   window::Frame f;
   f.first_tile = kFirstPart;
   f.palette_bank = kBank;
   f.pad_left = 1;
   const video::MapCell mine = video::map_cell(0, 0);
   video::write_map_cell(c.bg, kX, kY, mine);
   window::draw_frame(c, f);
   const auto part = [&](int k) { return video::map_cell(static_cast<video::TileIndex>(kFirstPart + k), kBank); };
   CHECK(video::read_map_cell(c.bg, kX, kY) == mine);                    // the window's cell
   CHECK(video::read_map_cell(c.bg, kX - 1, kY) == part(kInside));       // the padding
   CHECK(video::read_map_cell(c.bg, kX - 2, kY) == part(kLeft));         // the border, two cells left
   CHECK(video::read_map_cell(c.bg, kX - 2, kY - 1) == part(kTopLeft));
   CHECK(video::read_map_cell(c.bg, kX - 1, kY - 1) == part(kTop));      // above the padding
   CHECK(video::read_map_cell(c.bg, kX + 2, kY - 1) == part(kTopRight)); // one cell right of the window
   CHECK(video::read_map_cell(c.bg, kX + 2, kY) == part(kRight));
   CHECK(video::read_map_cell(c.bg, kX - 2, kY + 1) == part(kBottomLeft));
   const video::MapCell blank = video::map_cell(1, 0);
   window::clear_frame(c, f, blank);
   CHECK(video::read_map_cell(c.bg, kX - 2, kY - 1) == blank && video::read_map_cell(c.bg, kX, kY) == blank);
}

/// C10 (R7 finding 1): the counter typing, frame by frame as a reference game's printer runs: the lead frames, a
/// glyph every char_delay + 1 frames, a newline that costs the counter, a Pause control (n frames, then one more), a
/// press while the counter runs that speeds the text up (held: a glyph a frame), held alone that does not, and the
/// Font control (glyphs from the style's fonts; ResetFont back).
void counter_typing()
{
   constexpr uint8_t kDelay = 3;    // the counter after each code (a glyph every 4 frames)
   constexpr uint8_t kLead = 2;     // frames before the first code
   constexpr uint8_t kPause = 5;    // the Pause control's frames
   constexpr int kFrames = 40;      // frames run
   static constexpr text::Code kGlyphs4[] = {kA, kA, kA, kA, text::kCodeEnd};
   static constexpr text::Code kPaused[] = {kA, text::kCodeControl, static_cast<text::Code>(text::Control::Pause),
                                            kPause, kA, text::kCodeEnd};
   static constexpr text::Code kTwoLines[] = {kA, text::kCodeNewline, kA, text::kCodeEnd};
   const auto counter = [&]()
   {
      open();
      window::Config c = g_window.c;
      c.typing = window::kTypingCounter;
      c.char_delay = kDelay;
      c.speedup = window::kSpeedupPerFrame;
      c.lead_frames = kLead;
      window::open(g_window, c);
   };
   // the update (1-based) on which each glyph appears: the pen moves on that update
   const auto glyph_frames = [&](const text::Code* codes, bool press_at_4, bool hold, int* out, int count)
   {
      window::print(g_window, codes, false);
      int found = 0;
      int16_t pen = g_window.pen_x;
      int16_t line = g_window.line_y;
      for (int f = 1; f <= kFrames && found < count; ++f)
      {
         window::update(g_window, press_at_4 && f == 4, hold);
         if (g_window.pen_x != pen && g_window.line_y == line && g_window.pen_x > pen)
         {
            out[found++] = f;
         }
         else if (g_window.line_y != line && g_window.pen_x > g_window.c.left)
         {
            out[found++] = f;
         }
         pen = g_window.pen_x;
         line = g_window.line_y;
      }
      return found;
   };
   int at[4] = {};
   counter();
   CHECK(glyph_frames(kGlyphs4, false, false, at, 4) == 4);
   CHECK(at[0] == 3 && at[1] == 7 && at[2] == 11 && at[3] == 15);   // lead 2, then every 4 frames
   counter();
   CHECK(glyph_frames(kPaused, false, false, at, 2) == 2);
   CHECK(at[0] == 3 && at[1] == 13);   // read at 7 (the counter ran out): 5 frames, one more, then the glyph
   counter();
   CHECK(glyph_frames(kTwoLines, false, false, at, 2) == 2);
   CHECK(at[0] == 3 && at[1] == 10);   // the newline is read at 7 and counts that frame too
   counter();
   CHECK(glyph_frames(kGlyphs4, true, true, at, 4) == 4);
   CHECK(at[0] == 3 && at[1] == 5 && at[2] == 6 && at[3] == 7);   // pressed at 4: then a glyph a frame
   counter();
   CHECK(glyph_frames(kGlyphs4, false, true, at, 4) == 4);
   CHECK(at[0] == 3 && at[1] == 7);   // held without a press: the normal speed
   // the Font control: glyphs from the style's font 1 (here the same font), ResetFont back to the window's
   static constexpr text::Code kFonts[] = {text::kCodeControl, static_cast<text::Code>(text::Control::Font), 1, kA,
                                           text::kCodeControl, static_cast<text::Code>(text::Control::ResetFont), kB,
                                           text::kCodeEnd};
   const text::PropFont* table[] = {nullptr, &kFont};
   counter();
   window::Config c = g_window.c;
   c.fonts = table;
   c.font_count = 2;
   window::open(g_window, c);
   window::print(g_window, kFonts, true);
   CHECK(!window::busy(g_window) && g_window.glyphs == &kFont && pixel(0, 0) == kInk);
}

/// C13a: a pixel of a one-cell-high buffer of tiles.
/// @param tiles  the buffer
/// @param x      column
/// @param y      row (0-7)
/// @return its palette entry
uint8_t buffer_pixel(const uint32_t* tiles, int x, int y)
{
   return static_cast<uint8_t>((tiles[(x / 8) * window::kWordsPerTile + y] >> ((x % 8) * kNibbleBits)) & kNibbleMask);
}

/// C13a: a control escape's codes.
/// @param k  the control
/// @return its code
constexpr text::Code ctl(text::Control k)
{
   return static_cast<text::Code>(k);
}

/// C13a (R9 M2 / M3, V2): glyphs clipped to their width, their rows and the window as CopyGlyphToWindow; a glyph's 0
/// pixels take the background; Fill, Clear, SkipTo, ClearTo, MinSpacing as RenderText; ShiftDown from the start.
void handheld_rules()
{
   constexpr text::Code F = text::kCodeControl;
   constexpr text::Code E = text::kCodeEnd;
   open();
   // C draws two columns (its ink in columns 2-5 is past its width); over ink, B's 0 pixels take the background
   window::fill(g_window, 0, 0, 16, 8, kInk);
   static constexpr text::Code kClip[] = {kC, E};
   window::print_at(g_window, 8, 0, kClip);
   CHECK(pixel(8, 0) == kInk && pixel(9, 0) == kInk && pixel(10, 0) == kInk);   // 10: the fill, not C's ink
   window::fill(g_window, 0, 0, 16, 8, kShadow);
   window::print_at(g_window, 8, 0, kClip);
   CHECK(pixel(9, 0) == kInk && pixel(10, 0) == kShadow && pixel(9, 1) == kInk && pixel(9, 2) == kShadow);
   static constexpr text::Code kB1[] = {kB, E};
   window::print_at(g_window, 0, 0, kB1);
   CHECK(pixel(0, 0) == kBackground && pixel(1, 0) == kInk && pixel(2, 0) == kShadow);   // B's box: 2 x 2 pixels
   // the window's right edge: A at 14 draws two columns
   window::fill(g_window, 0, 0, 16, 8, kShadow);
   static constexpr text::Code kA1[] = {kA, E};
   CHECK(window::print_at(g_window, 14, 0, kA1) == 14 + kWidths[0] + kSpacing && pixel(15, 0) == kInk);
   // glyph_height 1: the second row is not drawn
   g_window.c.font = &kShortFont;
   window::print_at(g_window, 0, 0, kA1);
   CHECK(pixel(0, 0) == kInk && pixel(0, 1) == kShadow);   // A's shadow row left as it was (the fill's entry)
   g_window.c.font = &kFont;

   // ShiftDown sets the line from the start (twice 4 is 4, not 8)
   static constexpr text::Code kDown[] = {F, ctl(text::Control::ShiftDown), 4, F, ctl(text::Control::ShiftDown), 4,
                                          kA, E};
   window::print(g_window, kDown, true);
   CHECK(pixel(0, 4) == kInk && pixel(0, 0) == kBackground);
   // Clear 3 after A: pixels 4-6 cleared over ink, B at 7 (its ink at 8)
   window::fill(g_window, 0, 0, 16, 8, kInk);
   static constexpr text::Code kClear[] = {kA, F, ctl(text::Control::Clear), 3, kB, E};
   window::print_at(g_window, 0, 0, kClear);
   CHECK(pixel(4, 0) == kBackground && pixel(6, 1) == kBackground && pixel(8, 0) == kInk && pixel(4, 2) == kInk);
   CHECK(window::line_width(kFont, kClear) == kWidths[0] + kSpacing + 3 + kWidths[1] + kSpacing);
   // SkipTo 10 then B (its ink at 11); ClearTo 10 after A clears 4-9
   static constexpr text::Code kSkip[] = {F, ctl(text::Control::SkipTo), 10, kB, E};
   window::print(g_window, kSkip, true);
   CHECK(pixel(11, 0) == kInk && window::line_width(kFont, kSkip) == 10 + kWidths[1] + kSpacing);
   window::fill(g_window, 0, 0, 16, 8, kInk);
   static constexpr text::Code kClearTo[] = {kA, F, ctl(text::Control::ClearTo), 10, kB, E};
   window::print_at(g_window, 0, 0, kClearTo);
   CHECK(pixel(4, 0) == kBackground && pixel(9, 1) == kBackground && pixel(11, 0) == kInk);
   CHECK(window::line_width(kFont, kClearTo) == 10 + kWidths[1] + kSpacing);
   // MinSpacing 6: A takes six pixels (3 of it cleared), B's ink at 7
   static constexpr text::Code kMin[] = {F, ctl(text::Control::MinSpacing), 6, kA, kB, E};
   window::print(g_window, kMin, true);
   CHECK(pixel(7, 0) == kInk && pixel(5, 0) == kBackground && window::line_width(kFont, kMin) == 12);
   // Fill: the window back to the background, the pen to the start (B over where A was)
   static constexpr text::Code kFill[] = {kA, F, ctl(text::Control::Fill), kB, E};
   window::print(g_window, kFill, true);
   CHECK(pixel(0, 0) == kBackground && pixel(1, 0) == kInk && pixel(2, 0) == kBackground);
   // typed: a Clear takes a glyph's step (A, then the Clear, then B, kFramesPerChar frames apart)
   static constexpr text::Code kTyped[] = {kA, F, ctl(text::Control::Clear), 3, kB, E};
   window::print(g_window, kTyped, false);
   window::update(g_window, false, false);   // A
   window::update(g_window, false, false);
   window::update(g_window, false, false);   // the Clear
   window::update(g_window, false, false);
   CHECK(pixel(8, 0) == kBackground && window::busy(g_window));
   window::update(g_window, false, false);   // B
   CHECK(pixel(8, 0) == kInk);

   // the same rules on a screen's own tile buffer (2 x 1 cells)
   uint32_t own[2 * window::kWordsPerTile] = {};
   window::fill_tiles(own, 2, 1, 0, 0, 16, 8, kShadow);
   window::draw_glyph_tiles(own, 2, 1, kFont, kC, 7, 0, kInk, kShadow, kBackground);
   CHECK(buffer_pixel(own, 7, 0) == kInk && buffer_pixel(own, 8, 0) == kInk && buffer_pixel(own, 9, 0) == kShadow &&
         buffer_pixel(own, 6, 0) == kShadow);
}

/// C13c: the owner's tile storage: a TileBuffer's size, a window without storage (or too little) stays closed or
/// keeps the rows that fit, TilePool's first fit and its pieces given back in any order, and the VRAM queue's check
/// of memory a waiting copy reads.
void storage()
{
   static_assert(sizeof(window::TileBuffer<3, 2>) == 3 * 2 * window::kBytesPerTile, "a buffer of exactly W x H tiles");
   static_assert(window::tile_words(3, 2) == 3 * 2 * window::kWordsPerTile, "words of W x H tiles");
   window::Config c;
   c.font = &kFont;
   c.width = kTestWidth;
   c.height = kTestHeight;
   window::Window bare{};
   window::open(bare, c);
   CHECK(!window::is_open(bare));   // no storage: nothing to draw into

   // a pool of 10 tiles: pieces of 4, 4 and 2 fill it; the first given back makes room for one of 3 there
   constexpr int kPoolTiles = 10;
   static uint32_t words[window::tile_words(kPoolTiles, 1)];
   window::TilePool pool{};
   window::pool_init(pool, words, kPoolTiles);
   const window::Tiles a = window::pool_take(pool, 4);
   const window::Tiles b = window::pool_take(pool, 4);
   const window::Tiles d = window::pool_take(pool, 2);
   CHECK(a.words == words && b.words == words + 4 * window::kWordsPerTile && d.count == 2);
   CHECK(window::pool_take(pool, 1).words == nullptr && window::pool_used(pool) == kPoolTiles);   // full
   window::pool_give_back(pool, a);
   const window::Tiles e = window::pool_take(pool, 3);
   CHECK(e.words == words && e.count == 3 && window::pool_used(pool) == 9 && pool.high_water == kPoolTiles);
   CHECK(window::pool_take(pool, 2).words == nullptr);   // one tile left at 3: a piece of 2 does not fit
   window::pool_give_back(pool, b);
   window::pool_give_back(pool, d);
   window::pool_give_back(pool, e);
   CHECK(window::pool_used(pool) == 0);

   // a window on a pool piece draws into it; a window opened larger than its storage keeps the rows that fit
   // (release builds; debug builds assert)
   window::Window w{};
   const window::Tiles piece = window::pool_take(pool, kTestWidth * kTestHeight);
   window::attach(w, piece);
   window::open(w, c);
   CHECK(window::is_open(w) && w.store.words == words);
   window::close(w);
   window::pool_give_back(pool, piece);

   // the queue's check: a copy waiting for the VBlank reads its source
   capsule::hal::vram::reset();
   CHECK(!capsule::hal::vram::reads_from(words, sizeof words));
   capsule::hal::vram::copy32(capsule::hal::video::Region::BgVram, 0, words + window::kWordsPerTile,
                              window::kWordsPerTile, capsule::hal::vram::Priority::MustLand,
                              capsule::hal::video::Source::Ewram);
   CHECK(capsule::hal::vram::reads_from(words, 2 * window::kBytesPerTile));
   CHECK(!capsule::hal::vram::reads_from(words, window::kBytesPerTile));
   // the pool passes the stretch that copy reads (tile 1): a piece of 2 starts at tile 2, one of 1 at tile 0
   const window::Tiles two = window::pool_take(pool, 2);
   const window::Tiles one = window::pool_take(pool, 1);
   CHECK(two.words == words + 2 * window::kWordsPerTile && one.words == words);
   window::pool_give_back(pool, two);
   window::pool_give_back(pool, one);
   capsule::hal::vram::flush_all();
   CHECK(!capsule::hal::vram::reads_from(words, sizeof words));
}

}   // namespace

int main()
{
   drawing();
   pauses();
   frames();
   counter_typing();
   handheld_rules();
   storage();
   window::close(g_window);
   CHECK(!window::is_open(g_window));
   std::printf("text window tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
