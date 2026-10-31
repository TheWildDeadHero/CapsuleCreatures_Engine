/// @file
/// Tests for the asset table and the graphics modules that consume it
/// (gfx::background, gfx::sprites, gfx::palette, text::layer), drawn with the
/// software video model and checked on composed pixels. Uses the table compiled
/// from tests/data/assets/test_assets.json.
#include "capsule/asset/asset.h"
#include "capsule/gfx/background.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/text/placeholder.h"
#include "capsule/text/print.h"
#include "capsule/text/text_layer.h"

#include "../src/hal/soft/soft_video.h"

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

using namespace capsule;
namespace video = hal::video;

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

uint16_t g_frame[hal::soft::kFramePixels];   ///< Last composed frame.

/// Latches and composes a frame.
void show()
{
   hal::vram::flush();   // as system::update(): queued uploads land, then the frame
   hal::soft::latch();
   hal::soft::compose(g_frame);
}

/// One pixel of the last frame.
/// @param x  column
/// @param y  row
/// @return BGR555
uint16_t pixel(int x, int y)
{
   return g_frame[y * video::kScreenWidth + x];
}

constexpr video::Color kRed = video::rgb5(31, 0, 0);     ///< Fixture palette entry 1.
constexpr video::Color kGreen = video::rgb5(0, 31, 0);   ///< Fixture palette entry 2.

/// Fresh video state with every module reset.
void fresh()
{
   hal::soft::reset();
   hal::vram::reset();
   gfx::palette::reset();
   gfx::background::reset();
   gfx::sprites::reset();
   text::layer::reset();
}

/// Keys, kinds and lists resolve; key_v1 matches the compiler's hash.
void lookup()
{
   CHECK(asset::background(asset::key_v1("image:HALVES")) != nullptr);
   CHECK(asset::sprite(asset::key_v1("sprite:BLINK")) != nullptr);
   CHECK(asset::text(asset::key_v1("text:HELLO")) != nullptr);
   CHECK(asset::sprite(asset::key_v1("image:HALVES")) == nullptr);   // wrong kind
   CHECK(asset::find(asset::key_v1("image:NOPE")) == nullptr);
   const asset::List* l = asset::list(asset::key_v1("list:THINGS"));
   CHECK(l != nullptr && l->count == 2 && raw(l->items[0]) == raw(asset::key_v1("sprite:BLINK")));
   const asset::Background* disc = asset::background(asset::key_v1("image:DISC"));
   CHECK(disc != nullptr && disc->other != nullptr && disc->affine != disc->other->affine);
}

/// A regular image on BG1 uses palette bank 1 and shows its pixels.
void regular_background()
{
   fresh();
   const asset::Background* halves = asset::background(asset::key_v1("image:HALVES"));
   CHECK(gfx::background::load(1, *halves));
   gfx::background::show(1, true);
   show();
   CHECK(pixel(10, 10) == kRed && pixel(200, 10) == kGreen);
   CHECK((video::read_map_cell(1, 0, 0) >> 12) == 1);
   gfx::background::set_position(1, 64, 0);   // regular layer: scroll
   show();
   CHECK(pixel(70, 10) == kGreen);
}

/// The two-form image loads 8bpp on an affine layer, 4bpp on a regular one,
/// with its declared priority.
void both_forms_and_priority()
{
   fresh();
   const asset::Background* disc = asset::background(asset::key_v1("image:DISC"));
   gfx::background::set_display_mode(1);
   CHECK(gfx::background::layer_is_affine(2) && !gfx::background::layer_is_affine(1));
   CHECK(gfx::background::load(2, *disc));
   CHECK(gfx::background::is_affine(2));
   gfx::background::show(2, true);
   show();
   CHECK(pixel(64, 64) == kRed && pixel(5, 5) == 0);   // disc centre; corner shows the black backdrop
   gfx::background::set_position(2, 32, 32);           // affine layer: reference point
   show();
   CHECK(pixel(32, 32) == kRed);
   gfx::background::set_display_mode(0);
   CHECK(gfx::background::load(0, *disc));
   CHECK(!gfx::background::is_affine(0));
   CHECK((video::read_map_cell(0, 8, 8) & 0x3FF) != 0);
}

/// Sprite frames animate at the declared period; scale doubles the size.
void sprite_animation_and_scale()
{
   fresh();
   const video::Color backdrop = video::rgb5(0, 0, 8);
   gfx::palette::load_bg(0, &backdrop, 1);
   gfx::sprites::load(3, *asset::sprite(asset::key_v1("sprite:BLINK")));
   gfx::sprites::set_position(3, 40, 40);
   gfx::sprites::show(3, true);
   gfx::sprites::update();
   show();
   const uint16_t white = video::rgb5(31, 31, 31);
   CHECK(pixel(40, 40) == white && pixel(55, 55) == white && pixel(56, 56) == backdrop);
   for (int i = 0; i < 4; ++i)
   {
      gfx::sprites::update();
   }   // period 4: frames 0,0,0,0 then 1 (empty)
   show();
   CHECK(pixel(40, 40) == backdrop);
   gfx::sprites::play(3, 0);
   gfx::sprites::set_transform(3, 512, 0);   // 2x, double-size box around the centre
   gfx::sprites::update();
   show();
   CHECK(pixel(33, 33) == white && pixel(62, 62) == white && pixel(30, 30) == backdrop);
}

/// Image frames: auto palette colors, per-frame durations (2 then 5 display frames).
void sprite_image_durations()
{
   fresh();
   const video::Color backdrop = video::rgb5(0, 8, 0);
   gfx::palette::load_bg(0, &backdrop, 1);
   gfx::sprites::load(2, *asset::sprite(asset::key_v1("sprite:FLIP")));
   gfx::sprites::set_position(2, 100, 50);
   gfx::sprites::show(2, true);
   const video::Color blue = video::rgb5(0, 0, 31);
   // Frame 0 shows for 2 display frames, frame 1 for 5, then the loop returns to frame 0.
   constexpr int kExpected[9] = {0, 0, 1, 1, 1, 1, 1, 0, 0};
   bool timing = true;
   for (int f = 0; f < 9; ++f)
   {
      gfx::sprites::update();
      show();
      const int frame = pixel(100, 50) == kRed ? 0 : (pixel(100, 50) == blue ? 1 : -1);
      timing = timing && frame == kExpected[f];
      if (f == 2)
      {
         CHECK(pixel(104, 50) == blue && pixel(103, 53) == video::rgb5(31, 31, 31));
      }
   }
   CHECK(timing);
}

/// Software palette fade blends masked banks only, and lifts cleanly.
void palette_fade()
{
   fresh();
   const video::Color c[2] = {kRed, kGreen};
   gfx::palette::load_bg(16 + 1, c, 2);
   gfx::palette::set_fade(gfx::palette::bg_bank(1), 0, 16);
   CHECK(gfx::palette::blend(kRed, 0, 8) == video::rgb5(16, 0, 0));
   gfx::palette::set_fade(0, 0, 0);
   CHECK(gfx::palette::blend(kRed, kGreen, 0) == kRed);
}

/// Text from an asset lands as glyph cells; typed text takes time.
void text_layer()
{
   fresh();
   const text::Code* hello = asset::text(asset::key_v1("text:HELLO"));
   text::layer::print(1, 2, 3, text::layer::Align::Left, false, hello);
   const auto glyph = [](char ch)
   {
      return gfx::background::kTextTiles + 16 + (ch - 0x20);
   };
   CHECK((video::read_map_cell(1, 2, 3) & 0x3FF) == glyph('H'));
   CHECK((video::read_map_cell(1, 2, 4) & 0x3FF) == glyph('t'));   // newline returns to the anchor column
   CHECK((video::read_map_cell(1, 2, 3) >> 12) == 15);               // style 0: bank 15
   text::layer::open_box(0, 14, 30, 6);
   text::layer::print(text::layer::kBoxTarget, 0, 0, text::layer::Align::Left, true, hello);
   CHECK(text::layer::busy());
   for (int i = 0; i < 40 && text::layer::busy(); ++i)
   {
      text::layer::update();
   }
   CHECK(!text::layer::busy());
   CHECK((video::read_map_cell(0, 1, 15) & 0x3FF) == glyph('H'));   // inside the frame
   text::layer::close_box();
   CHECK((video::read_map_cell(0, 1, 15) & 0x3FF) == gfx::background::kTextTiles + 9);   // blank again
}

/// Control escapes (FC nn) take no cell and no typing time in either printer:
/// the escape and the code it carries are skipped, even when that code would
/// be a printable glyph.
void control_escapes()
{
   fresh();
   const text::Code skip[] = {'A', text::kCodeControl, 'Z', 'B', text::kCodeEnd};
   const text::Code tail[] = {'C', text::kCodeControl, text::kCodeEnd};   // escape cut off by the end
   const auto glyph = [](char ch)
   {
      return gfx::background::kTextTiles + 16 + (ch - 0x20);
   };
   // text::layer, instant: B lands right after A; nothing is drawn for the escape.
   text::layer::print(1, 4, 6, text::layer::Align::Left, false, skip);
   CHECK((video::read_map_cell(1, 4, 6) & 0x3FF) == glyph('A'));
   CHECK((video::read_map_cell(1, 5, 6) & 0x3FF) == glyph('B'));
   CHECK((video::read_map_cell(1, 6, 6) & 0x3FF) != glyph('Z'));
   // Right-aligned: the line is two cells wide, so it ends on the anchor column.
   text::layer::print(1, 20, 8, text::layer::Align::Right, false, skip);
   CHECK((video::read_map_cell(1, 19, 8) & 0x3FF) == glyph('A'));
   CHECK((video::read_map_cell(1, 20, 8) & 0x3FF) == glyph('B'));
   text::layer::print(1, 4, 10, text::layer::Align::Left, false, tail);
   CHECK((video::read_map_cell(1, 4, 10) & 0x3FF) == glyph('C'));
   // text::layer, typed: two glyphs type in two updates, the escape costs none.
   text::layer::open_box(0, 14, 30, 6);
   text::layer::print(text::layer::kBoxTarget, 0, 0, text::layer::Align::Left, true, skip);
   for (int i = 0; i < 40 && text::layer::busy(); ++i)
   {
      text::layer::update();
   }
   CHECK(!text::layer::busy());
   CHECK((video::read_map_cell(0, 1, 15) & 0x3FF) == glyph('A'));
   CHECK((video::read_map_cell(0, 2, 15) & 0x3FF) == glyph('B'));
   text::layer::close_box();
   // text::print (fixed cells): same skipping.
   const text::PrintTarget target{2, 0x100, 3};
   text::print(target, text::kDefaultFont, 2, 3, skip);
   const auto cell = [&](char ch)
   {
      return static_cast<int>(0x100 + (ch - text::kDefaultFont.first_code));
   };
   CHECK((video::read_map_cell(2, 2, 3) & 0x3FF) == cell('A'));
   CHECK((video::read_map_cell(2, 3, 3) & 0x3FF) == cell('B'));
   CHECK((video::read_map_cell(2, 3, 3) >> 12) == 3);
   CHECK((video::read_map_cell(2, 4, 3) & 0x3FF) != cell('Z'));
   text::print(target, text::kDefaultFont, 2, 5, tail);
   CHECK((video::read_map_cell(2, 2, 5) & 0x3FF) == cell('C'));
}

/// Test field resolver: placeholder 0 is "Ann", 1 is empty, 2 writes a
/// non-glyph code between two glyphs (dropped), anything else "?".
/// @param index     placeholder index
/// @param out       glyph codes
/// @param capacity  most codes
/// @return codes written
int test_resolver(uint8_t index, text::Code* out, int capacity)
{
   static constexpr text::Code kAnn[] = {'A', 'n', 'n'};
   static constexpr text::Code kMixed[] = {'x', text::kCodeNewline, 'y'};
   const text::Code* src = index == 0 ? kAnn : (index == 2 ? kMixed : nullptr);
   int n = index == 0 ? 3 : (index == 2 ? 3 : (index == 1 ? 0 : 1));
   for (int i = 0; i < n && i < capacity; ++i)
   {
      out[i] = src != nullptr ? src[i] : text::Code{'?'};
   }
   return n < capacity ? n : capacity;
}

/// Field placeholders (FD 80-FE) print the resolver's text in both printers,
/// in place, typed with the rest, and count for alignment.
void field_placeholders()
{
   fresh();
   text::set_field_resolver(test_resolver);
   const auto glyph = [](char ch)
   {
      return gfx::background::kTextTiles + 16 + (ch - 0x20);
   };
   const text::Code hi[] = {'H', 'i', ' ', text::kCodePlaceholder, text::kFirstFieldArg, '!', text::kCodeEnd};
   const text::Code mixed[] = {'<', text::kCodePlaceholder, 0x82, text::kCodePlaceholder, 0x81, '>', text::kCodeEnd};
   CHECK(text::line_cells(hi) == 7);      // "Hi " + "Ann" + "!"
   CHECK(text::line_cells(mixed) == 4);   // "<", "xy" (the newline dropped), "" , ">"
   text::FieldText t;
   CHECK(text::expand_field(0x82, t) == 2 && t.codes[0] == 'x' && t.codes[1] == 'y' && t.codes[2] == text::kCodeEnd);
   CHECK(text::expand_field(0x05, t) == 0 && t.codes[0] == text::kCodeEnd);   // a battle argument: nothing
   // text::layer, instant and right-aligned: the line is 7 cells, ending on the anchor.
   text::layer::print(1, 20, 6, text::layer::Align::Right, false, hi);
   CHECK((video::read_map_cell(1, 14, 6) & 0x3FF) == glyph('H'));
   CHECK((video::read_map_cell(1, 17, 6) & 0x3FF) == glyph('A'));
   CHECK((video::read_map_cell(1, 19, 6) & 0x3FF) == glyph('n'));
   CHECK((video::read_map_cell(1, 20, 6) & 0x3FF) == glyph('!'));
   // Typed in the box: the placeholder's glyphs type like the text around them (as long as "Hi Ann!").
   const auto typing_updates = [](const text::Code* codes)
   {
      text::layer::print(text::layer::kBoxTarget, 0, 0, text::layer::Align::Left, true, codes);
      int updates = 0;
      for (; updates < 40 && text::layer::busy(); ++updates)
      {
         text::layer::update();
      }
      return updates;
   };
   const text::Code plain[] = {'H', 'i', ' ', 'A', 'n', 'n', '!', text::kCodeEnd};
   text::layer::open_box(0, 14, 30, 6);
   const int plain_updates = typing_updates(plain);
   CHECK(plain_updates > 0 && typing_updates(hi) == plain_updates);
   CHECK((video::read_map_cell(0, 4, 15) & 0x3FF) == glyph('A'));
   CHECK((video::read_map_cell(0, 7, 15) & 0x3FF) == glyph('!'));
   text::layer::close_box();
   // text::print (fixed cells).
   const text::PrintTarget target{2, 0x100, 3};
   text::print(target, text::kDefaultFont, 1, 2, mixed);
   const auto cell = [&](char ch)
   {
      return static_cast<int>(0x100 + (ch - text::kDefaultFont.first_code));
   };
   CHECK((video::read_map_cell(2, 1, 2) & 0x3FF) == cell('<'));
   CHECK((video::read_map_cell(2, 2, 2) & 0x3FF) == cell('x'));
   CHECK((video::read_map_cell(2, 3, 2) & 0x3FF) == cell('y'));
   CHECK((video::read_map_cell(2, 4, 2) & 0x3FF) == cell('>'));
   text::set_field_resolver(nullptr);
}

/// Prompts (FA) and page breaks (FB): typed text waits for resume(); a page
/// break then clears the rows the page used and types from the first row
/// again. Printers that cannot wait skip the prompt and continue a page break
/// on the next row.
void prompts_and_pages()
{
   fresh();
   const auto glyph = [](char ch)
   {
      return gfx::background::kTextTiles + 16 + (ch - 0x20);
   };
   const text::Code paged[] = {'A', text::kCodeNewline, 'B', text::kCodePrompt, 'C', text::kCodePageBreak, 'D',
                               text::kCodeEnd};
   CHECK(text::line_cells(paged + 2) == 2);   // "B", the prompt (no cell), "C"; the page break ends the line
   text::layer::open_box(0, 14, 30, 6);
   text::layer::print(text::layer::kBoxTarget, 2, 0, text::layer::Align::Left, true, paged);
   for (int i = 0; i < 40 && !text::layer::waiting(); ++i)
   {
      text::layer::update();
   }
   CHECK(text::layer::busy() && text::layer::waiting());   // at the prompt, after A and B
   CHECK((video::read_map_cell(0, 3, 16) & 0x3FF) == glyph('B'));
   for (int i = 0; i < 10; ++i)
   {
      text::layer::update();   // waiting costs nothing and types nothing
   }
   CHECK((video::read_map_cell(0, 4, 16) & 0x3FF) != glyph('C'));
   text::layer::resume();
   for (int i = 0; i < 40 && !text::layer::waiting(); ++i)
   {
      text::layer::update();
   }
   CHECK(text::layer::waiting());   // at the page break, after C
   CHECK((video::read_map_cell(0, 4, 16) & 0x3FF) == glyph('C'));
   text::layer::resume();           // the page's rows are cleared, D starts the new page at the first row
   CHECK((video::read_map_cell(0, 3, 15) & 0x3FF) != glyph('A'));
   CHECK((video::read_map_cell(0, 3, 16) & 0x3FF) != glyph('B'));
   for (int i = 0; i < 40 && text::layer::busy(); ++i)
   {
      text::layer::update();
   }
   CHECK(!text::layer::busy() && !text::layer::waiting());
   CHECK((video::read_map_cell(0, 3, 15) & 0x3FF) == glyph('D'));
   text::layer::resume();   // nothing waits: no effect
   CHECK(!text::layer::busy());
   // Untyped: the prompt is skipped and the page break continues on the next row.
   text::layer::print(text::layer::kBoxTarget, 0, 0, text::layer::Align::Left, false, paged);
   CHECK(!text::layer::busy());
   CHECK((video::read_map_cell(0, 1, 16) & 0x3FF) == glyph('B'));
   CHECK((video::read_map_cell(0, 2, 16) & 0x3FF) == glyph('C'));
   CHECK((video::read_map_cell(0, 1, 17) & 0x3FF) == glyph('D'));
   // A new print finishes text that waits at once (the same way).
   text::layer::print(text::layer::kBoxTarget, 10, 0, text::layer::Align::Left, true, paged);
   for (int i = 0; i < 40 && !text::layer::waiting(); ++i)
   {
      text::layer::update();
   }
   const text::Code z[] = {'Z', text::kCodeEnd};
   text::layer::print(text::layer::kBoxTarget, 20, 0, text::layer::Align::Left, false, z);
   CHECK((video::read_map_cell(0, 11, 17) & 0x3FF) == glyph('D'));
   CHECK(!text::layer::busy());
   text::layer::close_box();
   // text::print: the same as untyped.
   const text::PrintTarget target{2, 0x100, 3};
   text::print(target, text::kDefaultFont, 1, 7, paged);
   const auto cell = [&](char ch)
   {
      return static_cast<int>(0x100 + (ch - text::kDefaultFont.first_code));
   };
   CHECK((video::read_map_cell(2, 1, 8) & 0x3FF) == cell('B'));
   CHECK((video::read_map_cell(2, 2, 8) & 0x3FF) == cell('C'));
   CHECK((video::read_map_cell(2, 1, 9) & 0x3FF) == cell('D'));
}

} // namespace

/// Runs every check.
/// @return 0 when all pass
int main()
{
   lookup();
   regular_background();
   both_forms_and_priority();
   sprite_animation_and_scale();
   sprite_image_durations();
   palette_fade();
   text_layer();
   control_escapes();
   field_placeholders();
   prompts_and_pages();
   std::printf("asset tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
