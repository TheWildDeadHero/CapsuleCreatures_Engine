/// @file
/// The engine's script port (see capsule/script/port.h): messages in the
/// text box (capsule/text/text_layer.h), answers from the pad.
///
/// - Messages: the box spans the bottom 6 rows (4 lines inside the frame);
///   text is typed. The game loop calls text::layer::update() each frame.
///   A prompt (FA) waits for A or B, then the text goes on; a page break
///   (FB) waits the same way, then the box is cleared and the next page
///   types from its first line. Field placeholders (FD 80-FE) print the
///   field resolver's text (capsule/text/placeholder.h).
/// - Yes / no: "YES" and "NO" on the box's last line; Left / Right / Up /
///   Down move the cursor, A answers, B answers no.
/// - Menus: the box grows to fit the options (one per line, cursor in the
///   first column); A picks, B picks the cancel index when there is one.
///   The question's text is replaced while the menu is open.
/// - Box position (D-17): bottom (default), middle or top.
/// - Number entry: the digits on the box's last line, right-aligned; Up /
///   Down change the chosen digit, Left / Right choose it, A answers, B
///   answers 0.
/// - Key input: the first of A, B, Select, Start, the D-pad, R and L newly
///   pressed.
/// - I4: the port's functions are weak (CAPSULE_WEAK): a game that draws its
///   own message box (an imported game's window and font) defines them
///   itself and keeps the party functions here.
/// - Party records: role_get() / role_set() reach the records of the party
///   given to party::attach() (capsule/monster/monster.h), slots 0 to its
///   count - 1.
#include "capsule/script/port.h"

#include "capsule/core/assert.h"
#include "capsule/core/memory.h"
#include "capsule/hal/input.h"
#include "capsule/script/data.h"
#include "capsule/text/text_layer.h"

namespace capsule::script::port
{
namespace
{

namespace layer = text::layer;
using hal::input::Key;
using hal::input::key_bit;
using hal::input::KeyBits;

constexpr uint8_t kBoxX = 0;        ///< Box left, cells.
constexpr uint8_t kBoxW = 30;       ///< Box width.
constexpr uint8_t kBoxH = 6;        ///< Message box height (4 lines inside).
constexpr uint8_t kScreenRows = 20; ///< Visible rows.
constexpr int kFrameCells = 2;      ///< Rows the box frame adds to its contents (top and bottom).
constexpr int kOneCodeLength = 2;   ///< A one-character string: the code and kCodeEnd.

/// @name Box positions (State::position, D-17)
///@{
constexpr uint8_t kPositionBottom = 0;   ///< Bottom of the screen.
constexpr uint8_t kPositionMiddle = 1;   ///< Centred.
constexpr uint8_t kPositionTop = 2;      ///< Top of the screen; also the highest position.
///@}

/// @name Menus (State::menu)
///@{
constexpr uint8_t kMenuNone = 0;     ///< No menu open.
constexpr uint8_t kMenuYesNo = 1;    ///< Yes / no.
constexpr uint8_t kMenuChoice = 2;   ///< A choice.
///@}

/// @name Yes / no line, columns inside the frame
///@{
constexpr uint8_t kYesCursorCol = 18;   ///< Cursor before YES.
constexpr uint8_t kYesCol = 19;         ///< YES.
constexpr uint8_t kNoCursorCol = 23;    ///< Cursor before NO.
constexpr uint8_t kNoCol = 24;          ///< NO.
///@}

/// @name Number entry
///@{
constexpr int kDecimalBase = 10;            ///< Digits are decimal.
constexpr int32_t kMaxDigitValue = 9;       ///< Highest digit (UP wraps it to 0, DOWN wraps 0 to it).
constexpr uint8_t kMaxDigits = 9;           ///< Most digits (the value stays inside int32_t).
constexpr int kNumberRightCol = 28;         ///< Column after the last digit.
constexpr int kNumberDigitCells = 2;        ///< Cells per digit: its cursor column and the digit.
///@}

/// Port state.
struct State
{
   KeyBits held;          ///< Keys at the last tick().
   KeyBits pressed;       ///< Newly pressed at the last tick(); consumed by answers.
   uint8_t menu;          ///< 0 none, 1 yes / no, 2 choice.
   uint8_t cursor;        ///< Selected option.
   uint8_t count;         ///< Options (choice).
   uint8_t cancel;        ///< Choice: Back's index, or kNoCancel.
   int16_t answer;        ///< -1 until answered.
   uint8_t position;      ///< 0 bottom, 1 middle, 2 top (D-17).
   uint8_t digits;        ///< Number entry: digits (0: none open).
   uint8_t digit;         ///< Number entry: the digit the cursor is on (0 = leftmost).
   int32_t number;        ///< Number entry: the value.
   int32_t number_answer; ///< Number entry: -1 until answered.
};
State g{};   ///< The port.

/// Whether a key was newly pressed (and consumes it).
/// @param k  the key
/// @return true once per press
bool take(Key k)
{
   const KeyBits b = key_bit(k);
   if (!(g.pressed & b))
   {
      return false;
   }
   g.pressed = static_cast<KeyBits>(g.pressed & ~b);
   return true;
}

/// Prints one character code in the box.
/// @param x  column inside the frame
/// @param y  row inside the frame
/// @param c  the code
void put_char(uint8_t x, uint8_t y, text::Code c)
{
   const text::Code s[kOneCodeLength] = {c, text::kCodeEnd};
   layer::print(layer::kBoxTarget, x, y, layer::Align::Left, false, s);
}

/// Top row of a box of some height at the current position.
/// @param h  box height, cells
/// @return its top row
uint8_t box_top(uint8_t h)
{
   if (g.position == kPositionTop)
   {
      return 0;
   }
   if (g.position == kPositionMiddle)
   {
      return static_cast<uint8_t>((kScreenRows - h) / kHalf);
   }
   return static_cast<uint8_t>(kScreenRows - h);
}

/// Opens the message box at the current position, if it is not open. The
/// text layer is asked every time (not a copy kept here): text::layer::reset()
/// or a map plan change (a location preview, a battle) closes the box behind
/// the port's back.
void ensure_box()
{
   if (!layer::box_open())
   {
      layer::open_box(kBoxX, box_top(kBoxH), kBoxW, kBoxH);
   }
}

/// Draws the number entry on the box's last line: each digit after a
/// column for the cursor (">" before the chosen digit).
void draw_number()
{
   constexpr uint8_t kRow = kBoxH - 3;
   int32_t v = g.number;
   for (int i = g.digits - 1; i >= 0; --i)
   {
      const auto col = static_cast<uint8_t>(kNumberRightCol - kNumberDigitCells * (g.digits - i));
      put_char(col, kRow, i == g.digit ? '>' : ' ');
      put_char(static_cast<uint8_t>(col + 1), kRow, static_cast<text::Code>('0' + v % kDecimalBase));
      v /= kDecimalBase;
   }
}

/// Draws the yes / no line with the cursor.
void draw_yesno()
{
   static constexpr text::Code kYes[] = {'Y', 'E', 'S', text::kCodeEnd};
   static constexpr text::Code kNo[] = {'N', 'O', text::kCodeEnd};
   constexpr uint8_t kRow = kBoxH - 3;
   put_char(kYesCursorCol, kRow, g.cursor == 0 ? '>' : ' ');
   layer::print(layer::kBoxTarget, kYesCol, kRow, layer::Align::Left, false, kYes);
   put_char(kNoCursorCol, kRow, g.cursor == 1 ? '>' : ' ');
   layer::print(layer::kBoxTarget, kNoCol, kRow, layer::Align::Left, false, kNo);
}

/// Draws the menu cursor.
void draw_cursor()
{
   for (uint8_t i = 0; i < g.count; ++i)
   {
      put_char(0, i, i == g.cursor ? '>' : ' ');
   }
}

} // namespace

CAPSULE_WEAK void message(const text::Code* codes)
{
   ensure_box();
   layer::clear(layer::kBoxTarget, 0, 0);
   layer::print(layer::kBoxTarget, 0, 0, layer::Align::Left, true, codes);
}

CAPSULE_WEAK bool message_busy()
{
   // A prompt or page break in the message waits for Accept / Back, then the text goes on.
   if (layer::waiting() && button())
   {
      layer::resume();
   }
   return layer::busy();
}

CAPSULE_WEAK void close_message()
{
   if (layer::box_open())
   {
      layer::close_box();
   }
   g.menu = kMenuNone;
}

CAPSULE_WEAK bool button()
{
   return take(hal::input::KEY_A) || take(hal::input::KEY_B);
}

CAPSULE_WEAK void yesno_open()
{
   ensure_box();
   g.menu = kMenuYesNo;
   g.cursor = 0;
   g.answer = -1;
   draw_yesno();
}

CAPSULE_WEAK int8_t yesno_poll()
{
   if (g.menu != kMenuYesNo)
   {
      return g.answer < 0 ? int8_t{0} : static_cast<int8_t>(g.answer);
   }
   if (take(hal::input::KEY_LEFT) || take(hal::input::KEY_RIGHT) || take(hal::input::KEY_UP) ||
       take(hal::input::KEY_DOWN))
   {
      g.cursor = static_cast<uint8_t>(g.cursor ^ 1);
      draw_yesno();
   }
   if (take(hal::input::KEY_A))
   {
      g.answer = g.cursor == 0 ? 1 : 0;
   }
   else if (take(hal::input::KEY_B))
   {
      g.answer = 0;
   }
   if (g.answer < 0)
   {
      return -1;
   }
   g.menu = kMenuNone;
   return static_cast<int8_t>(g.answer);
}

CAPSULE_WEAK void choice_open(const text::Code* const* options, uint8_t count, uint8_t cancel)
{
   ENGINE_ASSERT(count >= 1 && count <= kMaxChoices);
   const auto h = static_cast<uint8_t>(count + kFrameCells);
   if (layer::box_open())
   {   // the menu box replaces the message box
      layer::close_box();
   }
   layer::open_box(kBoxX, box_top(h), kBoxW, h);
   for (uint8_t i = 0; i < count; ++i)
   {
      layer::print(layer::kBoxTarget, 1, i, layer::Align::Left, false, options[i]);
   }
   g.menu = kMenuChoice;
   g.count = count;
   g.cursor = 0;
   g.cancel = cancel;
   g.answer = -1;
   draw_cursor();
}

CAPSULE_WEAK int16_t choice_poll()
{
   if (g.menu != kMenuChoice)
   {
      return g.answer;
   }
   if (take(hal::input::KEY_UP))
   {
      g.cursor = static_cast<uint8_t>(g.cursor == 0 ? g.count - 1 : g.cursor - 1);
      draw_cursor();
   }
   if (take(hal::input::KEY_DOWN))
   {
      g.cursor = static_cast<uint8_t>(g.cursor + 1 == g.count ? 0 : g.cursor + 1);
      draw_cursor();
   }
   if (take(hal::input::KEY_A))
   {
      g.answer = g.cursor;
   }
   else if (g.cancel != kNoCancel && take(hal::input::KEY_B))
   {
      g.answer = g.cancel;
   }
   if (g.answer < 0)
   {
      return -1;
   }
   g.menu = kMenuNone;
   close_message();   // the menu box goes; the next message opens a fresh box
   return g.answer;
}

CAPSULE_WEAK void message_position(uint8_t pos)
{
   g.position = pos <= kPositionTop ? pos : kPositionBottom;
   if (layer::box_open())
   {   // the box moves with the next message
      layer::close_box();
   }
}

CAPSULE_WEAK void number_open(uint8_t digits)
{
   ensure_box();
   g.digits = digits >= 1 && digits <= kMaxDigits ? digits : uint8_t{1};
   g.digit = static_cast<uint8_t>(g.digits - 1);
   g.number = 0;
   g.number_answer = -1;
   draw_number();
}

CAPSULE_WEAK int32_t number_poll()
{
   if (g.digits == 0)
   {
      return g.number_answer < 0 ? 0 : g.number_answer;
   }
   int32_t place = 1;
   for (uint8_t i = static_cast<uint8_t>(g.digit + 1); i < g.digits; ++i)
   {
      place *= kDecimalBase;
   }
   const int32_t d = (g.number / place) % kDecimalBase;   // the digit under the cursor
   bool changed = false;
   if (take(hal::input::KEY_UP))
   {
      g.number += d == kMaxDigitValue ? -kMaxDigitValue * place : place;
      changed = true;
   }
   if (take(hal::input::KEY_DOWN))
   {
      g.number += d == 0 ? kMaxDigitValue * place : -place;
      changed = true;
   }
   if (take(hal::input::KEY_LEFT) && g.digit > 0)
   {
      --g.digit;
      changed = true;
   }
   if (take(hal::input::KEY_RIGHT) && g.digit + 1 < g.digits)
   {
      ++g.digit;
      changed = true;
   }
   if (changed)
   {
      draw_number();
   }
   if (take(hal::input::KEY_A))
   {
      g.number_answer = g.number;
   }
   else if (take(hal::input::KEY_B))
   {
      g.number_answer = 0;
   }
   if (g.number_answer < 0)
   {
      return -1;
   }
   g.digits = 0;
   return g.number_answer;
}

CAPSULE_WEAK int8_t key_poll()
{
   for (uint8_t k = 0; k <= hal::input::KEY_L; ++k)
   {
      if (take(static_cast<Key>(k)))
      {
         return static_cast<int8_t>(k + 1);
      }
   }
   return -1;
}

CAPSULE_WEAK void tick()
{
   const KeyBits now = hal::input::held();
   g.pressed = static_cast<KeyBits>(now & ~g.held);
   g.held = now;
}

} // namespace capsule::script::port

namespace capsule::script::party
{

namespace
{
monster::Party* g_records = nullptr;   ///< The party attach() gave, or nullptr.
/// Highest value role_get() returns (a script variable is a signed 32-bit value).
constexpr uint32_t kMaxScriptValue = 0x7FFFFFFFu;

/// The record of a party slot.
/// @param slot  party slot
/// @return the record, or nullptr for an empty slot (or no party)
monster::Record* record_of(uint8_t slot)
{
   return g_records != nullptr && slot < g_records->count && slot < monster::kMaxPartySize
             ? &g_records->members[slot]
             : nullptr;
}
} // namespace

void attach(monster::Party* records)
{
   g_records = records;
}

int32_t role_get(uint8_t slot, uint8_t role, uint8_t element)
{
   const monster::Record* rec = record_of(slot);
   if (rec == nullptr)
   {
      return 0;
   }
   const uint32_t v = monster::get(*rec, role, element);
   return static_cast<int32_t>(v > kMaxScriptValue ? kMaxScriptValue : v);
}

void role_set(uint8_t slot, uint8_t role, uint8_t element, int32_t value)
{
   monster::Record* rec = record_of(slot);
   if (rec != nullptr)
   {
      monster::set(*rec, role, element, value < 0 ? 0u : static_cast<uint32_t>(value));
   }
}

} // namespace capsule::script::party
