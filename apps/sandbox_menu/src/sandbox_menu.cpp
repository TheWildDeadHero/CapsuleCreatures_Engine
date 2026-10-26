/// @file
/// The ui module's sandbox (N3, D-32): every kind of menu capsule/ui/ draws, styled by the text box assets of
/// data/assets.json (simple shapes: the engine's 8x8 glyphs as a proportional font, a plain frame and arrow image):
/// a column menu (wrapping above three options, two frames before input), a 2 x 2 grid, a scrolling list with its
/// bouncing arrows (keys repeat after 40 frames, every 5), a yes / no menu, a value box and a message box whose
/// arrow image steps while it waits.
///
/// SELECT moves the focus (column, grid, list, yes / no); the focused menu takes the D-pad, A and B. A choice or a
/// cancel shows in the message box (A goes on past its arrow) and the value box counts the choices. Laid out like a
/// game: the engine library, reference/project.json naming data/assets.json, this scene code.
#include "capsule/asset/asset.h"
#include "capsule/core/memory.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/text/encode.h"
#include "capsule/text/window.h"
#include "capsule/ui/arrows.h"
#include "capsule/ui/keys.h"
#include "capsule/ui/menu.h"

#include <cstdint>

namespace
{

namespace asset = capsule::asset;
namespace input = capsule::hal::input;
namespace palette = capsule::gfx::palette;
namespace text = capsule::text;
namespace ui = capsule::ui;
namespace video = capsule::hal::video;
namespace window = capsule::text::window;

/// @name Screen setup
///@{
constexpr video::BgIndex kBg = 0;        ///< The windows' layer.
constexpr uint8_t kCharBlock = 0;        ///< Its tiles.
constexpr uint8_t kScreenBlock = 31;     ///< Its map.
constexpr int kBankColors = 16;          ///< Colors per palette bank.
constexpr uint8_t kUpSlot = 0;           ///< The list's up arrow sprite.
constexpr uint8_t kDownSlot = 1;         ///< Its down arrow.
constexpr int kCellPixels = 8;           ///< Pixels per cell.
constexpr int kArrowHalf = 8;            ///< A 16 x 16 arrow: centre to edge.
constexpr int kHalf = 2;                 ///< Halves.
constexpr uint8_t kRepeatStart = 40;     ///< Frames before the first repeat.
constexpr uint8_t kRepeatEvery = 5;      ///< Frames between repeats.
constexpr uint8_t kGridColumns = 2;      ///< The grid's columns.
constexpr uint8_t kListShown = 4;        ///< Rows the list shows.
constexpr int kValueX = 4;               ///< The value box's text column.
constexpr int kValueY = 1;               ///< Its row.
constexpr int kDecimal = 10;             ///< Numbers print in decimal.
constexpr int kNumberCodes = 12;         ///< Codes of the value box's line.
///@}

/// Which menu takes the keys.
enum class Focus : uint8_t
{
   Column,      ///< The column menu.
   Grid,        ///< The grid.
   List,        ///< The list.
   YesNo,       ///< Yes / no.
   NUM_FOCUS    ///< Number of values.
};

const text::Code kBag[] = {'B', 'A', 'G', text::kCodeEnd};       ///< An option.
const text::Code kSave[] = {'S', 'A', 'V', 'E', text::kCodeEnd};   ///< An option.
const text::Code kExit[] = {'E', 'X', 'I', 'T', text::kCodeEnd};   ///< An option.
const text::Code kOne[] = {'O', 'N', 'E', text::kCodeEnd};        ///< An option.
const text::Code kTwo[] = {'T', 'W', 'O', text::kCodeEnd};        ///< An option.
const text::Code kRed[] = {'R', 'E', 'D', text::kCodeEnd};        ///< An option.
const text::Code kBlue[] = {'B', 'L', 'U', 'E', text::kCodeEnd};  ///< An option.
const text::Code kYes[] = {'Y', 'E', 'S', text::kCodeEnd};        ///< An option.
const text::Code kNo[] = {'N', 'O', text::kCodeEnd};              ///< An option.
const text::Code kHello[] = {'C', 'H', 'O', 'O', 'S', 'E', ' ', 'O', 'N', 'E', '.', text::kCodePrompt,
                             text::kCodeEnd};                     ///< The first message.
const text::Code kChosen[] = {'C', 'H', 'O', 'S', 'E', 'N', '!', text::kCodePrompt, text::kCodeEnd};   ///< A choice.
const text::Code kBack[] = {'B', 'A', 'C', 'K', '.', text::kCodePrompt, text::kCodeEnd};              ///< A cancel.

const text::Code* const kColumnItems[] = {kBag, kSave, kExit};               ///< The column's options.
const text::Code* const kGridItems[] = {kOne, kTwo, kRed, kBlue};            ///< The grid's.
const text::Code* const kListItems[] = {kOne, kTwo, kRed, kBlue, kBag, kSave, kExit, kYes};   ///< The list's.
const text::Code* const kYesNoItems[] = {kYes, kNo};                         ///< Yes / no.

/// The options of a list.
/// @tparam N     its length
/// @param  list  the list
/// @return N
template <int N>
constexpr uint8_t count_of(const text::Code* const (&list)[N])
{
   static_cast<void>(list);
   return static_cast<uint8_t>(N);
}

/// One menu of the scene.
struct Box
{
   const char* style;          ///< Its text box style.
   const text::Code* const* items;   ///< Its options.
   uint8_t count;              ///< How many.
   uint8_t columns;            ///< Grid columns (1: a column).
   uint8_t shown;              ///< List rows shown (0: all).
   window::Window* w;          ///< Its window (its tiles from g_tile_pool).
   ui::Menu menu;              ///< Its menu.
};

/// A focus as an index.
/// @param f  the focus
/// @return its index
constexpr int at(Focus f)
{
   return static_cast<int>(f);
}

constexpr int kBoxes = at(Focus::NUM_FOCUS);   ///< The menus.
CAPSULE_EWRAM_BSS window::Window g_windows[kBoxes];         ///< Their windows.
CAPSULE_EWRAM_BSS window::Window g_value;                   ///< The value box.
CAPSULE_EWRAM_BSS window::Window g_message;                 ///< The message box.
/// C13c: tiles for every window of the scene (the styles' windows: 42 + 32 + 64 + 20 + 16 + 104 tiles, and room).
constexpr int kPoolTiles = 320;
CAPSULE_EWRAM_BSS uint32_t g_tile_words[window::tile_words(kPoolTiles, 1)];   ///< The windows' tiles.
CAPSULE_EWRAM_BSS window::TilePool g_tile_pool;                               ///< Handed out by window size.

/// The scene.
struct Scene
{
   /// The menus (their windows in EWRAM).
   Box boxes[kBoxes] = {
      {"text_box:SANDBOX_COLUMN", kColumnItems, count_of(kColumnItems), 1, 0, &g_windows[at(Focus::Column)], {}},
      {"text_box:SANDBOX_GRID", kGridItems, count_of(kGridItems), kGridColumns, 0, &g_windows[at(Focus::Grid)], {}},
      {"text_box:SANDBOX_LIST", kListItems, count_of(kListItems), 1, kListShown, &g_windows[at(Focus::List)], {}},
      {"text_box:SANDBOX_YESNO", kYesNoItems, count_of(kYesNoItems), 1, 0, &g_windows[at(Focus::YesNo)], {}}};
   ui::Arrows arrows{};        ///< The list's arrows.
   ui::KeyState key_state{};   ///< The pad's repeat counter.
   Focus focus = Focus::Column;   ///< Which menu takes the keys.
   uint32_t choices = 0;       ///< Choices made (the value box).
};

Scene g;   ///< The scene.

/// A style of the sandbox's assets.
/// @param key  its key
/// @return it, or nullptr
const asset::TextBox* style(const char* key)
{
   return asset::text_box(asset::key_v1(key));
}

/// Loads a style's palettes and its frame's art (when it has them).
/// @param s  the style
void load_style(const asset::TextBox& s)
{
   if (s.palette != nullptr)
   {
      palette::load_bg(static_cast<uint8_t>(s.window.palette_bank * kBankColors), s.palette, kBankColors);
   }
   if (s.frame_tiles != nullptr)
   {
      video::load_bg_tiles(kCharBlock, s.frame.first_tile, s.frame_tiles, asset::kFrameTileCount);
   }
   if (s.frame_palette != nullptr)
   {
      palette::load_bg(static_cast<uint8_t>(s.frame.palette_bank * kBankColors), s.frame_palette, kBankColors);
   }
}

/// Opens a style's window with its frame.
/// @param w  the window
/// @param s  the style
void open_framed(window::Window& w, const asset::TextBox& s)
{
   window::Config c = s.window;
   c.char_block = kCharBlock;
   window::pool_give_back(g_tile_pool, w.store);   // C13c: tiles of the window's size (a window opened again too)
   window::attach(w, window::pool_take(g_tile_pool, c.width * c.height));
   window::open(w, c);
   window::draw_frame(w.c, s.frame);
}

/// Opens a box's window, frame and menu.
/// @param b  the box
void open_box(Box& b)
{
   const asset::TextBox* s = style(b.style);
   if (s == nullptr || s->menu == nullptr)
   {
      return;
   }
   load_style(*s);
   open_framed(*b.w, *s);
   ui::Options o;
   o.items = b.items;
   o.count = b.count;
   o.columns = b.columns;
   o.option_width = static_cast<uint8_t>(b.columns > 1 ? b.w->c.width / b.columns * kCellPixels : 0);
   o.shown = b.shown;
   ui::open(b.menu, *b.w, *s->menu, o);
}

/// Prints the value box's count.
void print_value()
{
   text::Code codes[kNumberCodes];
   text::Code rev[kNumberCodes];
   int n = 0;
   uint32_t v = g.choices;
   do
   {
      rev[n++] = static_cast<text::Code>('0' + v % kDecimal);
      v /= kDecimal;
   }
   while (v > 0 && n < kNumberCodes - 1);
   for (int i = 0; i < n; ++i)
   {
      codes[i] = rev[n - 1 - i];
   }
   codes[n] = text::kCodeEnd;
   window::clear(g_value);
   window::print_at(g_value, kValueX, kValueY, codes);
}

/// Shows a text in the message box (typed; its arrow image steps at the prompt).
/// @param codes  the text
void say(const text::Code* codes)
{
   window::clear(g_message);
   window::print(g_message, codes, false);
}

/// Sets the scene up: the layer, every window and menu, the list's arrows, the first message.
void setup()
{
   video::set_mode(0);
   video::configure_bg(kBg, true, 0, kCharBlock, kScreenBlock, false, 0);
   palette::reset();
   capsule::gfx::sprites::reset();
   window::pool_init(g_tile_pool, g_tile_words, kPoolTiles);
   for (window::Window& w : g_windows)
   {
      window::attach(w, window::Tiles{});   // nothing handed out yet
   }
   window::attach(g_value, window::Tiles{});
   window::attach(g_message, window::Tiles{});
   for (Box& b : g.boxes)
   {
      open_box(b);
   }
   const asset::TextBox* list = style("text_box:SANDBOX_LIST");
   Box& lb = g.boxes[at(Focus::List)];
   if (list != nullptr && list->menu != nullptr)
   {
      const int x = lb.w->c.x * kCellPixels + lb.w->c.width * kCellPixels / kHalf - kArrowHalf;
      ui::open(g.arrows, *list->menu, kUpSlot, kDownSlot, static_cast<int16_t>(x),
               static_cast<int16_t>((lb.w->c.y - 1) * kCellPixels - kArrowHalf),
               static_cast<int16_t>((lb.w->c.y + lb.w->c.height) * kCellPixels));
   }
   if (const asset::TextBox* v = style("text_box:SANDBOX_VALUE"))
   {
      open_framed(g_value, *v);
      print_value();
   }
   if (const asset::TextBox* m = style("text_box:SANDBOX_MESSAGE"))
   {
      load_style(*m);
      open_framed(g_message, *m);
      say(kHello);
   }
}

/// One frame: the focus, the focused menu, the arrows, the message box.
/// @param held  the keys held
void frame(input::KeyBits held)
{
   const ui::Keys k = ui::read(g.key_state, ui::Repeat{kRepeatStart, kRepeatEvery}, held);
   if (ui::pressed(k, input::KEY_SELECT))
   {
      g.focus = static_cast<Focus>((at(g.focus) + 1) % kBoxes);
   }
   else
   {
      Box& b = g.boxes[at(g.focus)];
      const int16_t a = ui::update(b.menu, k);
      if (a == ui::kCancel || a >= 0)
      {
         g.choices += a >= 0 ? 1 : 0;
         print_value();
         say(a >= 0 ? kChosen : kBack);
         open_box(b);   // the scene's menus stay: open it again
      }
   }
   Box& lb = g.boxes[at(Focus::List)];
   ui::update(g.arrows, lb.menu.scroll, static_cast<uint8_t>(lb.menu.count - lb.menu.shown));
   window::update(g_message, ui::pressed(k, input::KEY_A), ui::held(k, input::KEY_A));
   capsule::gfx::sprites::update();
}

}   // namespace

/// Runs the sandbox until the platform quits.
/// @return 0
int main()
{
   capsule::hal::system::init();
   setup();
   while (capsule::hal::system::update())
   {
      frame(input::held());
   }
   return 0;
}
