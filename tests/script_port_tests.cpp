/// @file
/// The engine's script port (src/script/script_port_hal.cpp) with the real
/// text layer: a message, then whatever closes the text box behind the
/// port's back (a location preview ends with text::layer::reset(), see
/// overworld.cpp; a map coming or going changes the map plan), then a second
/// message must open the box again and show its text. C3: the port's positioned menus (a grid, a scrolling list,
/// the delay, Back).
#include "capsule/gfx/background.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/input.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/script/port.h"
#include "capsule/text/encode.h"
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

namespace capsule::hal::input
{
KeyBits g_held = 0;   ///< C3: the keys the test holds.
/// Test build: the keys the test holds.
/// @return them
KeyBits held()
{
   return g_held;
}
} // namespace capsule::hal::input

namespace
{

using namespace capsule;
namespace video = hal::video;
namespace layer = text::layer;

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

constexpr auto kFirst = text::encode("Hi");     ///< The first message (static storage: typed text).
constexpr auto kSecond = text::encode("Yo");    ///< The second one.
constexpr auto kRed = text::encode("Red");      ///< C3: menu options.
constexpr auto kGreen = text::encode("Green");  ///< ...
constexpr auto kBlue = text::encode("Blue");    ///< ...
constexpr auto kGrey = text::encode("Grey");    ///< ...
constexpr auto kPink = text::encode("Pink");    ///< ...

/// Fresh video state with every module reset.
void fresh()
{
   hal::soft::reset();
   hal::vram::reset();
   gfx::palette::reset();
   gfx::background::reset();
   gfx::sprites::reset();
   layer::reset();
}

/// Types the pending message to its end.
void type_out()
{
   for (int i = 0; i < 60 && layer::busy(); ++i)
   {
      layer::update();
   }
}

/// The glyph a box cell shows, as its character.
/// @param col  column inside the frame
/// @param row  row inside the frame
/// @return the character ('\0' when the cell holds no glyph)
char box_char(int col, int row)
{
   // The default (bottom) box: column 0, rows 14-19; its first inside cell is (1, 15).
   const int tile = (video::read_map_cell(0, 1 + col, 15 + row) & 0x3FF) - (gfx::background::kTextTiles + 16);
   return tile >= 0 && tile < 0x60 ? static_cast<char>(tile + 0x20) : '\0';
}

/// C3: the glyph a screen cell of BG0's map shows, as its character.
/// @param x  cell column
/// @param y  cell row
/// @return the character ('\0' when the cell holds no glyph)
char screen_char(int x, int y)
{
   const int tile = (video::read_map_cell(0, x, y) & 0x3FF) - (gfx::background::kTextTiles + 16);
   return tile >= 0 && tile < 0x60 ? static_cast<char>(tile + 0x20) : '\0';
}

/// C3: one frame with a key pressed (held this frame, released before the next), then the menu's poll.
/// @param key  the key, or a negative number for none
/// @return the poll's answer
int16_t press(int key)
{
   hal::input::g_held = 0;
   script::port::tick();   // released
   hal::input::g_held = key >= 0 ? hal::input::key_bit(static_cast<hal::input::Key>(key)) : 0;
   script::port::tick();
   return script::port::menu_poll();
}

/// C3: a grid at its cells: the delay's frame reads nothing, Right moves along the row, A answers the index.
void menu_grid()
{
   fresh();
   const text::Code* items[] = {kRed.codes, kGreen.codes, kBlue.codes};
   script::port::MenuRequest r{};
   r.items = items;
   r.count = 3;
   r.x = 2;
   r.y = 3;
   r.columns = 3;
   r.delay = 1;
   r.cancel = 127;
   r.back = true;
   CHECK(script::port::menu_open(r) == script::port::kMenuWaiting && layer::box_open());
   // the frame at (2, 3): row 4 holds the cursor and Red from column 3, Green a column (cursor + widest) on
   constexpr int kRow = 4;
   constexpr int kFirstCell = 3;
   constexpr int kColumn = 6;   // the cursor's cell and "Green"
   CHECK(screen_char(kFirstCell, kRow) == '>' && screen_char(kFirstCell + 1, kRow) == 'R');
   CHECK(screen_char(kFirstCell + kColumn + 1, kRow) == 'G');
   CHECK(press(hal::input::KEY_RIGHT) == script::port::kMenuWaiting);   // the delay's frame
   CHECK(screen_char(kFirstCell, kRow) == '>');
   CHECK(press(hal::input::KEY_RIGHT) == script::port::kMenuWaiting);
   CHECK(screen_char(kFirstCell + kColumn, kRow) == '>' && screen_char(kFirstCell, kRow) == ' ');
   CHECK(press(hal::input::KEY_A) == 1 && !layer::box_open());
}

/// C3: a scrolling list of two rows: Down scrolls it, Back answers the cancel value; without back it does nothing.
void menu_list()
{
   fresh();
   const text::Code* items[] = {kRed.codes, kGreen.codes, kBlue.codes, kGrey.codes, kPink.codes};
   script::port::MenuRequest r{};
   r.items = items;
   r.count = 5;
   r.x = 0;
   r.y = 0;
   r.columns = 1;
   r.rows = 2;
   r.delay = script::kMenuStyleDelay;   // the port's: none
   r.cancel = 127;
   r.back = false;
   CHECK(script::port::menu_open(r) == script::port::kMenuWaiting);
   CHECK(screen_char(2, 1) == 'R' && screen_char(2, 2) == 'G' && screen_char(2, 3) != 'B');   // two rows, framed
   CHECK(press(hal::input::KEY_DOWN) == script::port::kMenuWaiting);
   CHECK(press(hal::input::KEY_DOWN) == script::port::kMenuWaiting);
   CHECK(screen_char(2, 1) == 'G' && screen_char(2, 2) == 'B' && screen_char(1, 2) == '>');   // scrolled by one
   CHECK(press(hal::input::KEY_B) == script::port::kMenuWaiting);   // no back: nothing
   r.back = true;
   CHECK(script::port::menu_open(r) == script::port::kMenuWaiting);
   CHECK(press(hal::input::KEY_UP) == script::port::kMenuWaiting);   // wraps to the last option
   CHECK(screen_char(2, 1) == 'G' && screen_char(2, 2) == 'P');
   CHECK(press(hal::input::KEY_B) == 127 && !layer::box_open());
}

/// Message, the box closed by text::layer::reset() (a location preview's end), message.
void message_after_reset()
{
   fresh();
   script::port::message(kFirst.codes);
   type_out();
   CHECK(layer::box_open() && box_char(0, 0) == 'H');
   layer::reset();   // what overworld does when a location preview ends
   CHECK(!layer::box_open());
   script::port::message(kSecond.codes);
   CHECK(layer::box_open());   // the port asked the layer instead of trusting its own copy
   type_out();
   CHECK(box_char(0, 0) == 'Y' && box_char(1, 0) == 'o');
   script::port::close_message();
   CHECK(!layer::box_open());
}

/// Message, a map plan change (a map came and went), message.
void message_after_plan_change()
{
   fresh();
   script::port::message(kFirst.codes);
   type_out();
   CHECK(layer::box_open());
   gfx::background::begin_map_plan(0b0110, 4);   // BG1 and BG2 hold the map: BG0 stays usable
   gfx::background::end_map_plan();
   CHECK(!layer::box_open());   // the layer forgets its box when the plan changes
   script::port::message(kSecond.codes);
   CHECK(layer::box_open());
   type_out();
   CHECK(box_char(0, 0) == 'Y' && box_char(1, 0) == 'o');
}

} // namespace

/// Runs every check.
/// @return 0 when all pass
int main()
{
   message_after_reset();
   message_after_plan_change();
   menu_grid();
   menu_list();
   std::printf("script port tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
