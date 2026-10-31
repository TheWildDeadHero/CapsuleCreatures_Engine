/// @file
/// N3 (D-32): the ui module (capsule/ui/) and the text window's arrow image (capsule/text/window.h) on the software
/// video model with the fixture tests/data/ui (simple shapes): the pad's repeat counter, a column menu's input
/// delay, wrap rule, sounds and cancel, the every-key order, a grid's moves, a list's middle-row scrolling against an
/// independent model, the scroll arrows' bounce and visibility, print_at() and the arrow image's steps.
#include "capsule/asset/asset.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/text/encode.h"
#include "capsule/text/window.h"
#include "capsule/ui/arrows.h"
#include "capsule/ui/keys.h"
#include "capsule/ui/menu.h"
#include "hal/soft/soft_video.h"

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

namespace asset = capsule::asset;
namespace input = capsule::hal::input;
namespace text = capsule::text;
namespace ui = capsule::ui;
namespace video = capsule::hal::video;
namespace window = capsule::text::window;

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.
int g_sounds = 0;     ///< Sounds played (ui::play_sound()).
uint16_t g_last_sound = 0;   ///< The last one.

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

constexpr uint16_t kMoveSound = 5;   ///< The fixture's move / choose / cancel sound.
constexpr uint16_t kOpenSound = 7;   ///< The list's open sound.
constexpr uint8_t kStartDelay = 40;  ///< The repeat counter's start (a handheld game's 40 frames).
constexpr uint8_t kEvery = 5;        ///< Its interval.
constexpr int kNibbleBits = 4;       ///< Bits of one 4bpp pixel.
constexpr uint32_t kNibbleMask = 15; ///< One 4bpp pixel.
constexpr uint8_t kBackground = 1;   ///< The fixture windows' background entry.
constexpr uint8_t kInk = 2;          ///< Their ink entry.
constexpr int kTilePixels = 8;       ///< Pixels per tile side.
constexpr int kRowHeight = 4;        ///< The fixture menus' row height.
constexpr int kTop = 1;              ///< Their top margin.
constexpr int kOptionX = 4;          ///< Their option column.

const text::Code kA[] = {'A', text::kCodeEnd};   ///< An option.
const text::Code kB[] = {'B', text::kCodeEnd};   ///< An option.
const text::Code kC[] = {'C', text::kCodeEnd};   ///< An option.
/// Eight options for the lists and menus.
const text::Code* const kItems[] = {kA, kB, kC, kA, kB, kC, kA, kB};

window::Window g_window;   ///< The window under test.
constexpr int kTileRows = 16;   ///< C13c: rows of its storage.
/// C13c: its tiles (the largest window: the fixture's styles are smaller).
window::TileBuffer<window::kMaxTiles / kTileRows, kTileRows> g_tiles;

/// A pixel of the window's RAM tiles.
/// @param x  column
/// @param y  row
/// @return its palette entry
uint8_t pixel(int x, int y)
{
   const int t = (y / kTilePixels) * g_window.c.width + x / kTilePixels;
   const uint32_t word = g_window.store.words[t * window::kWordsPerTile + y % kTilePixels];
   return static_cast<uint8_t>((word >> ((x % kTilePixels) * kNibbleBits)) & kNibbleMask);
}

/// A style of the fixture.
/// @param key  its asset key
/// @return it (never nullptr: the fixture has every style)
const asset::TextBox& style(const char* key)
{
   const asset::TextBox* s = asset::text_box(asset::key_v1(key));
   if (s == nullptr)
   {
      std::printf("FAIL: no style %s\n", key);
      std::exit(1);
   }
   return *s;
}

/// Opens the window of a style (character block 0).
/// @param s  the style
void open_window(const asset::TextBox& s)
{
   window::Config c = s.window;
   c.char_block = 0;
   window::attach(g_window, g_tiles.tiles());
   window::open(g_window, c);
}

/// One frame's keys with nothing repeated.
/// @param pressed  keys that went down
/// @return the keys
ui::Keys press(input::KeyBits pressed)
{
   ui::Keys k;
   k.held = pressed;
   k.pressed = pressed;
   k.repeated = pressed;
   return k;
}

/// A key's bit.
/// @param k  the key
/// @return its bit
constexpr input::KeyBits bit(input::Key k)
{
   return input::key_bit(k);
}

/// Whether the cursor glyph shows before a row (its first ink pixel).
/// @param row  the row
/// @return true when it does
bool cursor_on(int row)
{
   return pixel(0, kTop + row * kRowHeight) == kInk;
}

/// The pad's repeat counter: the start, the interval, a change of keys starting it again.
void repeat_counter()
{
   ui::KeyState s;
   const ui::Repeat timing{kStartDelay, kEvery};
   const input::KeyBits down = bit(input::KEY_DOWN);
   ui::Keys k = ui::read(s, timing, down);
   CHECK(ui::pressed(k, input::KEY_DOWN) && ui::repeated(k, input::KEY_DOWN));
   int repeats = 0;
   int first = 0;
   for (int f = 1; f <= kStartDelay + kEvery * 2; ++f)
   {
      k = ui::read(s, timing, down);
      CHECK(!ui::pressed(k, input::KEY_DOWN));
      if (ui::repeated(k, input::KEY_DOWN))
      {
         first = repeats == 0 ? f : first;
         ++repeats;
      }
   }
   CHECK(first == kStartDelay && repeats == 3);   // at 40, 45 and 50 frames after the press
   // another key joins: the counter starts again (and the new key is a press)
   k = ui::read(s, timing, static_cast<input::KeyBits>(down | bit(input::KEY_A)));
   CHECK(ui::pressed(k, input::KEY_A) && !ui::pressed(k, input::KEY_DOWN) && ui::held(k, input::KEY_DOWN));
   for (int f = 1; f < kStartDelay; ++f)
   {
      k = ui::read(s, timing, static_cast<input::KeyBits>(down | bit(input::KEY_A)));
      CHECK(!ui::repeated(k, input::KEY_DOWN));
   }
}

/// A column: the input delay, the wrap rule (wraps above three options), sounds, Accept and cancel.
void column()
{
   const asset::TextBox& s = style("text_box:UI_COLUMN");
   CHECK(s.menu != nullptr && s.frame_tiles != nullptr && s.frame_palette != nullptr && s.palette == nullptr);
   CHECK(style("text_box:UI_MESSAGE").palette != nullptr && style("text_box:UI_MESSAGE").frame_tiles == nullptr);
   open_window(s);
   ui::Menu m{};
   ui::Options o;
   o.items = kItems;
   o.count = 4;
   o.cursor = 1;
   ui::open(m, g_window, *s.menu, o);
   CHECK(cursor_on(1) && !cursor_on(0));
   CHECK(pixel(kOptionX + 1, kTop + kRowHeight) == kInk);   // B's ink pixel at x 1 of its cell: drawn at 4 + 1
   g_sounds = 0;
   // the style's input delay: two frames read nothing
   CHECK(ui::update(m, press(bit(input::KEY_DOWN))) == ui::kNothing && m.cursor == 1);
   CHECK(ui::update(m, press(bit(input::KEY_DOWN))) == ui::kNothing && m.cursor == 1 && g_sounds == 0);
   CHECK(ui::update(m, press(bit(input::KEY_DOWN))) == ui::kNothing && m.cursor == 2 && g_sounds == 1);
   CHECK(cursor_on(2) && !cursor_on(1));
   ui::update(m, press(bit(input::KEY_DOWN)));
   ui::update(m, press(bit(input::KEY_DOWN)));   // four options: past the last to the first
   CHECK(m.cursor == 0 && cursor_on(0) && !cursor_on(3));
   ui::update(m, press(bit(input::KEY_UP)));
   CHECK(m.cursor == 3);
   // one action a frame: Accept before Down
   const int sounds = g_sounds;
   CHECK(ui::update(m, press(static_cast<input::KeyBits>(bit(input::KEY_A) | bit(input::KEY_DOWN)))) == 3);
   CHECK(m.cursor == 3 && g_sounds == sounds + 1 && g_last_sound == kMoveSound);
   ui::close(m);
   // three options do not wrap: the end stays, without a sound; B does nothing without cancel
   o.count = 3;
   o.cursor = 0;
   o.cancel = false;
   o.delay = 0;
   open_window(s);
   ui::open(m, g_window, *s.menu, o);
   g_sounds = 0;
   ui::update(m, press(bit(input::KEY_UP)));
   CHECK(m.cursor == 0 && g_sounds == 0);
   CHECK(ui::update(m, press(bit(input::KEY_B))) == ui::kNothing && g_sounds == 0);
   ui::close(m);
   o.cancel = true;
   ui::open(m, g_window, *s.menu, o);
   CHECK(ui::update(m, press(bit(input::KEY_B))) == ui::kCancel && g_sounds == 1);
   ui::close(m);
   CHECK(!ui::is_open(m));
}

/// The every-key order (a start menu): Up and Accept in one frame move, then pick; Start cancels.
void every_key()
{
   const asset::TextBox& s = style("text_box:UI_START");
   open_window(s);
   ui::Menu m{};
   ui::Options o;
   o.items = kItems;
   o.count = 3;
   ui::open(m, g_window, *s.menu, o);
   CHECK(ui::update(m, press(static_cast<input::KeyBits>(bit(input::KEY_UP) | bit(input::KEY_A)))) == 2);
   CHECK(m.cursor == 2);   // wraps always (three options)
   g_sounds = 0;
   ui::update(m, press(static_cast<input::KeyBits>(bit(input::KEY_UP) | bit(input::KEY_DOWN))));
   CHECK(m.cursor == 2 && g_sounds == 2);   // up, then down: two moves, two sounds
   CHECK(ui::update(m, press(bit(input::KEY_START))) == ui::kCancel);
   ui::close(m);
}

/// A grid: moves inside its rows and columns, none past the edges or onto a cell past the last option.
void grid()
{
   const asset::TextBox& s = style("text_box:UI_COLUMN");
   open_window(s);
   ui::Menu m{};
   ui::Options o;
   o.items = kItems;
   o.count = 6;
   o.columns = 2;
   o.option_width = kTilePixels;
   o.delay = 0;
   ui::open(m, g_window, *s.menu, o);
   CHECK(m.rows == 3);
   g_sounds = 0;
   ui::update(m, press(bit(input::KEY_RIGHT)));
   CHECK(m.cursor == 1 && g_sounds == 1);
   CHECK(pixel(kTilePixels, kTop) == kInk);   // the cursor in the second column
   ui::update(m, press(bit(input::KEY_RIGHT)));
   CHECK(m.cursor == 1 && g_sounds == 1);     // the edge: no move, no sound
   ui::update(m, press(bit(input::KEY_DOWN)));
   ui::update(m, press(bit(input::KEY_DOWN)));
   CHECK(m.cursor == 5);
   ui::update(m, press(bit(input::KEY_DOWN)));
   CHECK(m.cursor == 5);
   ui::close(m);
   o.count = 5;   // the last row has one option: down from the second column stays
   open_window(s);
   ui::open(m, g_window, *s.menu, o);
   ui::move_to(m, 3);
   ui::update(m, press(bit(input::KEY_DOWN)));
   CHECK(m.cursor == 3);
   ui::close(m);
}

/// The list model: the row a move down / up stops at before scrolling, as written out independently.
struct Model
{
   int count;    ///< Options.
   int shown;    ///< Rows shown.
   int scroll;   ///< First shown.
   int row;      ///< The cursor's row.

   /// One move.
   /// @param down  down
   void move(bool down)
   {
      const int half = shown / 2 + shown % 2;
      if (down)
      {
         const int stop = shown == 1 ? 0 : half;
         if (scroll == count - shown)
         {
            row = row < shown - 1 ? row + 1 : row;
         }
         else if (row < stop)
         {
            ++row;
         }
         else
         {
            ++scroll;
            row = stop;
         }
         return;
      }
      const int stop = shown == 1 ? 0 : shown - half - 1;
      if (scroll == 0)
      {
         row = row > 0 ? row - 1 : row;
      }
      else if (row > stop)
      {
         --row;
      }
      else
      {
         --scroll;
         row = stop;
      }
   }
};

/// A list: the open sound, the middle-row rule against the model (down to the end and back), repeats, sounds only
/// on a change.
void list()
{
   const asset::TextBox& s = style("text_box:UI_LIST");
   for (int shown = 1; shown <= 4; ++shown)
   {
      open_window(s);
      ui::Menu m{};
      ui::Options o;
      o.items = kItems;
      o.count = 8;
      o.shown = static_cast<uint8_t>(shown);
      g_sounds = 0;
      ui::open(m, g_window, *s.menu, o);
      CHECK(g_sounds == 1 && g_last_sound == kOpenSound);
      Model model{8, shown, 0, 0};
      bool same = true;
      for (int i = 0; i < 20; ++i)
      {
         const bool down = i < 10;
         model.move(down);
         ui::update(m, press(bit(down ? input::KEY_DOWN : input::KEY_UP)));
         same = same && m.scroll == model.scroll && m.cursor == model.scroll + model.row;
      }
      CHECK(same);
      CHECK(m.cursor == 0 && m.scroll == 0);
      ui::close(m);
   }
   // repeats move a list (move_keys "repeated"); a move that changes nothing plays nothing
   open_window(s);
   ui::Menu m{};
   ui::Options o;
   o.items = kItems;
   o.count = 8;
   o.shown = 4;
   ui::open(m, g_window, *s.menu, o);
   ui::Keys k;
   k.held = bit(input::KEY_DOWN);
   k.repeated = k.held;
   ui::update(m, k);
   CHECK(m.cursor == 1);
   g_sounds = 0;
   ui::update(m, press(bit(input::KEY_UP)));
   ui::update(m, press(bit(input::KEY_UP)));
   CHECK(m.cursor == 0 && g_sounds == 1);
   ui::close(m);
}

/// The scroll arrows: the bounce's values, hidden on the first frame, shown towards where the list can scroll.
void arrows()
{
   CHECK(ui::bounce(2, 0) == 0 && ui::bounce(2, 64) == 2 && ui::bounce(2, 192) == -2);
   CHECK(ui::bounce(2, 21) == 0 && ui::bounce(2, 22) == 1 && ui::bounce(2, 234) == -1);
   const asset::TextBox& s = style("text_box:UI_LIST");
   capsule::gfx::palette::reset();
   capsule::gfx::sprites::reset();
   ui::Arrows a{};
   constexpr int16_t kX = 40;
   constexpr int16_t kUpY = 20;
   constexpr int16_t kDownY = 60;
   ui::open(a, *s.menu, 14, 15, kX, kUpY, kDownY);
   CHECK(a.open);
   static uint16_t frame[capsule::hal::soft::kFramePixels];
   auto red_at = [](int x, int y)
   {
      capsule::gfx::sprites::update();
      capsule::hal::vram::flush();   // as system::update(): queued uploads land, then the frame
      capsule::hal::soft::latch();
      capsule::hal::soft::compose(frame);
      return frame[y * video::kScreenWidth + x] == video::rgb5(31, 0, 0);
   };
   constexpr int kBarX = 6;   // the up arrow's vertical bar: x 6-9, y 2-13 of its 16 x 16 frame
   constexpr int kBarY = 2;
   ui::update(a, 0, 4);       // the first frame: animations start, nothing shows
   CHECK(!red_at(kX + kBarX, kUpY + kBarY));
   ui::update(a, 0, 4);       // scroll 0: only the down arrow (frame 1 flipped: its bar at the same columns)
   CHECK(!red_at(kX + kBarX, kUpY + kBarY) && red_at(kX + kBarX, kDownY + kBarY));
   ui::update(a, 2, 4);
   CHECK(red_at(kX + kBarX, kUpY + kBarY + ui::bounce(2, 8)));   // the second shown frame: phase 8
   ui::update(a, 4, 4);       // the end: only the up arrow
   CHECK(!red_at(kX + kBarX, kDownY + kBarY + ui::bounce(2, static_cast<uint8_t>(-16))));
   ui::close(a);
   CHECK(!a.open);
}

/// print_at() over the window's pixels; the arrow image's steps at the pen (shown at once, then every delay + 1
/// frames, through the offsets), erased by the press.
void arrow_image()
{
   const asset::TextBox& s = style("text_box:UI_MESSAGE");
   CHECK(s.window.arrow_kind == window::kArrowImage && s.window.arrow_steps == 4);
   open_window(s);
   window::print_at(g_window, 0, kTop, kA);
   CHECK(pixel(0, kTop) == kInk);
   window::print_at(g_window, 8, kTop, kB);
   CHECK(pixel(0, kTop) == kInk && pixel(9, kTop) == kInk);   // nothing cleared
   window::fill(g_window, 0, 0, 8, 8, kBackground);
   CHECK(pixel(0, kTop) == kBackground);
   static constexpr text::Code kText[] = {'A', text::kCodePageBreak, 'B', text::kCodeEnd};
   window::print(g_window, kText, false);
   window::update(g_window, false, false);   // A
   window::update(g_window, false, false);   // the page break: the wait begins
   CHECK(window::waiting(g_window));
   const int pen = g_window.pen_x;
   // the image is 3 rows of a triangle from row 2 of 6: step offsets 0, 1, 2, 1 show its top row at 2, 1, 0, 1
   auto top_row = [pen]()
   {
      for (int y = 0; y < 4; ++y)
      {
         if (pixel(pen + 1, kTop + y) == kInk)
         {
            return y;
         }
      }
      return -1;
   };
   window::update(g_window, false, false);
   CHECK(top_row() == 2);
   for (int f = 0; f < 8; ++f)
   {
      window::update(g_window, false, false);
   }
   CHECK(top_row() == 2);
   window::update(g_window, false, false);   // the ninth frame: the second step
   CHECK(top_row() == 1);
   for (int f = 0; f < 9; ++f)
   {
      window::update(g_window, false, false);
   }
   CHECK(top_row() == 0);
   window::update(g_window, true, false);   // the press: the page clears
   CHECK(top_row() == -1);
   window::close(g_window);
}

}   // namespace

/// The tests' sounds: counted.
/// @param id  the sound
void capsule::ui::play_sound(uint16_t id)
{
   ++g_sounds;
   g_last_sound = id;
}

int main()
{
   capsule::hal::soft::reset();
   repeat_counter();
   column();
   every_key();
   grid();
   list();
   arrows();
   arrow_image();
   std::printf("ui tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
