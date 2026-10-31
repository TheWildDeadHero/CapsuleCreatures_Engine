/// @file
/// Menus drawn in a text window (see capsule/ui/menu.h).
#include "capsule/ui/menu.h"

#include "capsule/core/memory.h"

namespace capsule::ui
{
namespace
{

namespace input = hal::input;
namespace window = text::window;

constexpr int kHalf = 2;         ///< A list's middle row: half its shown rows.
constexpr int kTilePixels = 8;   ///< Pixels per window cell side.

/// The cursor's position of an option (a list: its row among the shown ones).
/// @param m       the menu
/// @param option  the option
/// @param x       column, pixels
/// @param y       row, pixels
void cursor_at(const Menu& m, uint8_t option, int& x, int& y)
{
   const int index = option - m.scroll;
   x = m.style->cursor_x + (index % m.columns) * m.option_width;
   y = m.style->top + (index / m.columns) * m.style->row_height;
}

/// Draws the cursor glyph before an option.
/// @param m       the menu
/// @param option  the option
void draw_cursor(Menu& m, uint8_t option)
{
   if (m.style->cursor == kNoCursor)
   {
      return;
   }
   int x = 0;
   int y = 0;
   cursor_at(m, option, x, y);
   const text::Code glyph[] = {m.style->cursor, text::kCodeEnd};
   window::print_at(*m.window, x, y, glyph);
}

/// Clears the cursor before an option (its rectangle back to the window's background).
/// @param m       the menu
/// @param option  the option
void erase_cursor(Menu& m, uint8_t option)
{
   if (m.style->cursor == kNoCursor)
   {
      return;
   }
   int x = 0;
   int y = 0;
   cursor_at(m, option, x, y);
   window::fill(*m.window, x, y, m.style->cursor_width, m.style->cursor_height, m.window->c.background);
}

/// Prints the shown options.
/// @param m      the menu
/// @param clear  clear their rows first (a list's scroll)
void draw_items(Menu& m, bool clear)
{
   window::Window& w = *m.window;
   if (clear)
   {
      // the rows below the top margin, as far as the window goes
      window::fill(w, 0, m.style->top, w.c.width * kTilePixels, w.c.height * kTilePixels - m.style->top,
                   w.c.background);
   }
   for (int i = 0; i < m.shown && m.scroll + i < m.count; ++i)
   {
      const int x = m.style->option_x + (i % m.columns) * m.option_width;
      const int y = m.style->top + (i / m.columns) * m.style->row_height;
      window::print_at(w, x, y, m.items[m.scroll + i]);
   }
}

/// Plays a sound unless it is kNoSound.
/// @param id  the sound
void sound(uint16_t id)
{
   if (id != kNoSound)
   {
      play_sound(id);
   }
}

/// Whether the menu scrolls (a list showing fewer options than it has).
/// @param m  the menu
/// @return true for a list
bool is_list(const Menu& m)
{
   return m.columns == 1 && m.shown < m.count;
}

/// What a list's move did.
enum class ListMove : uint8_t
{
   None,      ///< Nothing moved (the end of the list).
   Cursor,    ///< The cursor moved among the shown rows.
   Scrolled,  ///< The list scrolled.
   NUM_MOVES  ///< Number of values.
};

/// A list's move by one option, keeping the cursor near the middle of the shown rows.
/// @param m     the menu
/// @param down  moving down
/// @return what moved
ListMove list_step(Menu& m, bool down)
{
   const int row = m.cursor - m.scroll;
   const int shown = m.shown;
   if (!down)
   {
      // the row the cursor stops at while the list can still scroll up: above the middle
      const int stop = shown == 1 ? 0 : shown - (shown / kHalf + shown % kHalf) - 1;
      if (m.scroll == 0 || row > stop)
      {
         if (row == 0)
         {
            return ListMove::None;   // at the top of a list that does not scroll further
         }
         --m.cursor;
         return ListMove::Cursor;
      }
      --m.scroll;   // the list scrolls and the cursor goes to the row above the middle
      m.cursor = static_cast<uint8_t>(m.scroll + stop);
      return ListMove::Scrolled;
   }
   const int stop = shown == 1 ? 0 : shown / kHalf + shown % kHalf;
   if (m.scroll == m.count - shown || row < stop)
   {
      if (row == shown - 1)
      {
         return ListMove::None;   // at the bottom of a list that does not scroll further
      }
      ++m.cursor;
      return ListMove::Cursor;
   }
   ++m.scroll;   // the list scrolls and the cursor goes to the row below the middle
   m.cursor = static_cast<uint8_t>(m.scroll + stop);
   return ListMove::Scrolled;
}

/// Moves the cursor of a column or a list one option.
/// @param m     the menu
/// @param down  down (else up)
/// @return true when the cursor moved
bool step(Menu& m, bool down)
{
   const uint8_t old = m.cursor;
   if (is_list(m))
   {
      const ListMove moved = list_step(m, down);
      if (moved == ListMove::Scrolled)
      {
         draw_items(m, true);
         draw_cursor(m, m.cursor);
      }
      else if (moved == ListMove::Cursor)
      {
         erase_cursor(m, old);   // the list did not scroll: the old row is still shown
         draw_cursor(m, m.cursor);
      }
      return moved != ListMove::None;
   }
   if (down)
   {
      m.cursor = m.cursor + 1 < m.count ? static_cast<uint8_t>(m.cursor + 1)
                                        : (m.wrap ? static_cast<uint8_t>(0) : m.cursor);
   }
   else
   {
      m.cursor = m.cursor > 0 ? static_cast<uint8_t>(m.cursor - 1)
                              : (m.wrap ? static_cast<uint8_t>(m.count - 1) : m.cursor);
   }
   erase_cursor(m, old);
   draw_cursor(m, m.cursor);   // drawn again even when it stayed (the game redraws it)
   return m.cursor != old;
}

/// Moves a grid's cursor one cell (no wrap; a cell past the last option keeps it where it was).
/// @param m   the menu
/// @param dx  -1, 0 or 1
/// @param dy  -1, 0 or 1
/// @return true when the cursor moved
bool grid_step(Menu& m, int dx, int dy)
{
   const uint8_t old = m.cursor;
   int pos = m.cursor;
   const int col = pos % m.columns;
   const int row = pos / m.columns;
   if (dx != 0 && col + dx >= 0 && col + dx < m.columns)
   {
      pos += dx;
   }
   if (dy != 0 && row + dy >= 0 && row + dy < m.rows)
   {
      pos += m.columns * dy;
   }
   if (pos >= m.count)
   {
      return false;   // no such option: stays (and is not drawn again)
   }
   m.cursor = static_cast<uint8_t>(pos);
   erase_cursor(m, old);
   draw_cursor(m, m.cursor);
   return m.cursor != old;
}

/// Acts on Up or Down (a grid: on any direction).
/// @param m   the menu
/// @param dx  -1, 0 or 1 (a grid)
/// @param dy  -1 or 1, or 0 with dx
void move(Menu& m, int dx, int dy)
{
   const bool moved = m.columns > 1 ? grid_step(m, dx, dy) : step(m, dy > 0);
   if (moved || (m.style->move_sound == kSoundAlways && !is_list(m)))
   {
      sound(m.style->sound_move);
   }
}

/// Whether a direction key acts this frame (on its press, or with kMoveRepeated on a repeat too).
/// @param m    the menu
/// @param k    the frame's keys
/// @param key  the direction
/// @return true when it acts
bool direction(const Menu& m, const Keys& k, input::Key key)
{
   return m.style->move_keys == kMoveRepeated ? repeated(k, key) : pressed(k, key);
}

/// Whether a cancel key went down this frame.
/// @param m  the menu
/// @param k  the frame's keys
/// @return true when one did
bool cancel_pressed(const Menu& m, const Keys& k)
{
   return (k.pressed & m.style->cancel_keys) != 0;
}

/// Accept or cancel this frame.
/// @param m       the menu
/// @param k       the frame's keys
/// @param answer  set to the answer when one is given
/// @return true when the frame's input ended in an answer (or an ignored cancel, which ends a kOneAction frame)
bool answer_keys(Menu& m, const Keys& k, int16_t& answer)
{
   if (pressed(k, input::KEY_A))
   {
      sound(m.style->sound_choose);
      answer = m.cursor;
      return true;
   }
   if (cancel_pressed(m, k))
   {
      if (m.cancel)
      {
         sound(m.style->sound_cancel);
         answer = kCancel;
      }
      return true;
   }
   return false;
}

}   // namespace

void open(Menu& m, window::Window& w, const MenuStyle& style, const Options& o)
{
   m.style = &style;
   m.window = &w;
   m.items = o.items;
   m.count = o.count > kMaxItems ? static_cast<uint8_t>(kMaxItems) : o.count;
   m.columns = o.columns > 1 ? o.columns : static_cast<uint8_t>(1);
   m.rows = static_cast<uint8_t>(m.count / m.columns);
   m.option_width = m.columns > 1 ? o.option_width : static_cast<uint8_t>(0);
   m.shown = m.columns == 1 && o.shown != 0 && o.shown < m.count ? o.shown : m.count;
   m.cursor = o.cursor < m.count ? o.cursor : static_cast<uint8_t>(0);
   m.scroll = 0;
   if (is_list(m))
   {
      const int last_scroll = m.count - m.shown;
      m.scroll = o.scroll > last_scroll ? static_cast<uint8_t>(last_scroll) : o.scroll;
      m.cursor = static_cast<uint8_t>(m.scroll + (o.cursor < m.shown ? o.cursor : 0));   // a list's cursor: its row
   }
   m.delay = o.delay == kStyleDelay ? style.input_delay : static_cast<uint8_t>(o.delay);
   m.cancel = o.cancel;
   m.wrap = m.columns == 1 && !is_list(m) &&
            (style.wrap == kWrapAlways || (style.wrap == kWrapAbove && m.count > style.wrap_above));
   m.open = true;
   draw_items(m, false);
   draw_cursor(m, m.cursor);
   if (is_list(m))
   {
      sound(style.sound_open);
   }
}

int16_t update(Menu& m, const Keys& k)
{
   if (!m.open)
   {
      return kNothing;
   }
   if (m.delay > 0)
   {
      --m.delay;
      return kNothing;
   }
   int16_t answer = kNothing;
   if (m.style->order == kEveryKey)
   {
      // each key acts in turn: Up, Down, then Accept (which ends the frame), then the cancel keys
      if (direction(m, k, input::KEY_UP))
      {
         move(m, 0, -1);
      }
      if (direction(m, k, input::KEY_DOWN))
      {
         move(m, 0, 1);
      }
      answer_keys(m, k, answer);
      return answer;
   }
   if (answer_keys(m, k, answer))
   {
      return answer;
   }
   if (direction(m, k, input::KEY_UP))
   {
      move(m, 0, -1);
   }
   else if (direction(m, k, input::KEY_DOWN))
   {
      move(m, 0, 1);
   }
   else if (m.columns > 1 && direction(m, k, input::KEY_LEFT))
   {
      move(m, -1, 0);
   }
   else if (m.columns > 1 && direction(m, k, input::KEY_RIGHT))
   {
      move(m, 1, 0);
   }
   return kNothing;
}

void move_to(Menu& m, uint8_t option)
{
   if (!m.open || option >= m.count)
   {
      return;
   }
   if (is_list(m) && (option < m.scroll || option >= m.scroll + m.shown))
   {
      const int last_scroll = m.count - m.shown;
      m.scroll = static_cast<uint8_t>(option > last_scroll ? last_scroll : option);
      m.cursor = option;
      draw_items(m, true);
      draw_cursor(m, m.cursor);
      return;
   }
   erase_cursor(m, m.cursor);
   m.cursor = option;
   draw_cursor(m, m.cursor);
}

void redraw(Menu& m)
{
   if (!m.open)
   {
      return;
   }
   draw_items(m, false);
   draw_cursor(m, m.cursor);
}

void close(Menu& m)
{
   m.open = false;
}

bool is_open(const Menu& m)
{
   return m.open;
}

int widest(const text::PropFont& font, const text::Code* const* items, uint8_t count)
{
   int w = 0;
   for (uint8_t i = 0; i < count; ++i)
   {
      const int iw = items[i] != nullptr ? window::line_width(font, items[i]) : 0;
      w = iw > w ? iw : w;
   }
   return w;
}

}   // namespace capsule::ui

/// The engine's default: no sound (a game with audio defines play_sound(); its parameter: capsule/ui/menu.h).
CAPSULE_WEAK void capsule::ui::play_sound(uint16_t id)
{
   (void)id;   // no audio in the engine yet
}
