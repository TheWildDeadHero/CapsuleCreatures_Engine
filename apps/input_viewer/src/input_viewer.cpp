/// @file
/// Input viewer: shows, live, which physical buttons the HAL reports, which
/// logical actions they map to on this target (B-11), and the shoulder
/// filter's state and events. Built for every target from the same source; the
/// only difference is the compiled input map each build links.
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/input/input.h"
#include "capsule/text/encode.h"
#include "capsule/text/font.h"
#include "capsule/text/print.h"

#include <cstdint>

namespace
{

using namespace capsule;
namespace video = hal::video;
namespace hin = hal::input;
using text::Code;

constexpr video::BgIndex kBg = 0;       ///< Text layer.
constexpr uint8_t kCharBlock = 0;       ///< Character block with the font.
constexpr uint8_t kScreenBlock = 31;    ///< Text map.
constexpr int kCols = 30;               ///< Visible columns.

constexpr uint8_t kNumBanks = 4;     ///< Palette banks in use (one per Bank).
constexpr int kColorsPerBank = 16;   ///< Colors in one 4bpp palette bank.
constexpr int kInkEntry = 1;         ///< Palette entry of a bank's text color.
constexpr int kShadowEntry = 2;      ///< Palette entry of a bank's text shadow.

constexpr unsigned kDecimalBase = 10;          ///< Base of decimal numbers.
constexpr int kMaxDigits = 10;                 ///< Decimal digits of the largest 32-bit number.
constexpr int kNumberCodes = kMaxDigits + 1;   ///< Codes of a printed number: digits and kCodeEnd.
constexpr unsigned kMaxFrame = 0xFFFFFFFFu;    ///< Largest frame count (the counter saturates there).

/// Palette banks: text color per bank in entry 1, shadow in entry 2.
enum Bank : uint8_t
{
   BANK_DIM,     ///< Idle labels.
   BANK_ON,      ///< Held / active.
   BANK_OFF,     ///< Button not on this target.
   BANK_TITLE    ///< Titles.
};
constexpr video::Color kBackdrop = video::rgb5(3, 6, 12);   ///< Backdrop.
constexpr video::Color kShadow = video::rgb5(1, 2, 5);      ///< Text shadow.
/// Ink color per Bank.
constexpr video::Color kInk[] = {
   video::rgb5(18, 20, 24),   // dim: idle labels
   video::rgb5(31, 28, 6),    // on: held / active
   video::rgb5(8, 10, 14),    // off: button not on this target
   video::rgb5(31, 31, 31),   // title
};

/// Print target for a bank.
/// @param bank  palette bank
/// @return the target
text::PrintTarget target(Bank bank)
{
   return {kBg, 0, bank};
}

/// Prints text.
/// @param bank   palette bank
/// @param x      column
/// @param y      row
/// @param codes  encoded text
void put(Bank bank, int x, int y, const Code* codes)
{
   text::print(target(bank), text::kDefaultFont, x, y, codes);
}

/// Length of encoded text.
/// @param codes  encoded text
/// @return codes before kCodeEnd
int length(const Code* codes)
{
   int n = 0;
   while (codes[n] != text::kCodeEnd)
   {
      ++n;
   }
   return n;
}

/// Blanks columns [@p x0, @p x1) of a row.
/// @param x0  first column
/// @param x1  column after the last
/// @param y   row
void blank(int x0, int x1, int y)
{
   constexpr auto kSpace = text::encode(" ");   ///< Screen text " ".
   for (int x = x0; x < x1; ++x)
   {
      put(BANK_DIM, x, y, kSpace.codes);
   }
}

/// Unsigned number to codes, terminated.
/// @param value  the number
/// @param out    receives the digits and kCodeEnd
void number(unsigned value, Code (&out)[kNumberCodes])
{
   Code digits[kMaxDigits];
   int n = 0;
   do
   {
      digits[n++] = static_cast<Code>('0' + value % kDecimalBase);
      value /= kDecimalBase;
   }
   while (value != 0 && n < kMaxDigits);
   int i = 0;
   while (n > 0)
   {
      out[i++] = digits[--n];
   }
   out[i] = text::kCodeEnd;
}

/// @name Static text
///@{
constexpr auto kTitle = text::encode("INPUT TEST");   ///< Screen text "INPUT TEST".
constexpr auto kTargetGba = text::encode("GBA target");   ///< Screen text "GBA target".
constexpr auto kTargetSnes = text::encode("SNES-layout target");   ///< Screen text "SNES-layout target".
constexpr auto kTargetDc = text::encode("Dreamcast target");   ///< Screen text "Dreamcast target".
constexpr auto kEventLbl = text::encode("Event:");   ///< Screen text "Event:".
constexpr auto kFilterLbl = text::encode("Filter:");   ///< Screen text "Filter:".
constexpr auto kHeader = text::encode("Action       Bound to");   ///< Screen text "Action       Bound to".
constexpr auto kMenuEntry = text::encode("(menu entry)");   ///< Screen text "(menu entry)".
constexpr auto kAt = text::encode("@");   ///< Screen text "@".
///@}

/// Where a button's name is drawn.
struct KeyLabel
{
   hin::Key key;        ///< The button.
   int x;               ///< Column.
   int y;               ///< Row.
   const Code* text;    ///< Its name.
};

/// @name Button names
///@{
constexpr auto kA = text::encode("A");   ///< Screen text "A".
constexpr auto kB = text::encode("B");   ///< Screen text "B".
constexpr auto kX = text::encode("X");   ///< Screen text "X".
constexpr auto kY = text::encode("Y");   ///< Screen text "Y".
constexpr auto kL = text::encode("L");   ///< Screen text "L".
constexpr auto kR = text::encode("R");   ///< Screen text "R".
constexpr auto kStart = text::encode("START");   ///< Screen text "START".
constexpr auto kSelect = text::encode("SELECT");   ///< Screen text "SELECT".
constexpr auto kUp = text::encode("UP");   ///< Screen text "UP".
constexpr auto kDown = text::encode("DOWN");   ///< Screen text "DOWN".
constexpr auto kLeft = text::encode("LEFT");   ///< Screen text "LEFT".
constexpr auto kRight = text::encode("RIGHT");   ///< Screen text "RIGHT".
///@}

/// Button labels and their positions.
const KeyLabel kKeyLabels[] = {
   {hin::KEY_A, 1, 4, kA.codes},          {hin::KEY_B, 3, 4, kB.codes},
   {hin::KEY_X, 5, 4, kX.codes},          {hin::KEY_Y, 7, 4, kY.codes},
   {hin::KEY_L, 9, 4, kL.codes},          {hin::KEY_R, 11, 4, kR.codes},
   {hin::KEY_START, 13, 4, kStart.codes}, {hin::KEY_SELECT, 19, 4, kSelect.codes},
   {hin::KEY_UP, 1, 5, kUp.codes},        {hin::KEY_DOWN, 4, 5, kDown.codes},
   {hin::KEY_LEFT, 9, 5, kLeft.codes},    {hin::KEY_RIGHT, 14, 5, kRight.codes},
};

/// Button names by Key bit, for the "Bound to" column.
const Code* const kKeyNames[hin::NUM_KEYS] = {
   kA.codes,  kB.codes,    kSelect.codes, kStart.codes, kRight.codes, kLeft.codes,
   kUp.codes, kDown.codes, kR.codes,      kL.codes,     kX.codes,     kY.codes,
};

/// @name Action names
///@{
constexpr auto aUp = text::encode("UP");   ///< Screen text "UP".
constexpr auto aDown = text::encode("DOWN");   ///< Screen text "DOWN".
constexpr auto aLeft = text::encode("LEFT");   ///< Screen text "LEFT".
constexpr auto aRight = text::encode("RIGHT");   ///< Screen text "RIGHT".
constexpr auto aAccept = text::encode("ACCEPT");   ///< Screen text "ACCEPT".
constexpr auto aBack = text::encode("BACK");   ///< Screen text "BACK".
constexpr auto aMenu = text::encode("MAIN MENU");   ///< Screen text "MAIN MENU".
constexpr auto aDevice = text::encode("DEVICE MENU");   ///< Screen text "DEVICE MENU".
constexpr auto aRun = text::encode("TOGGLE RUN");   ///< Screen text "TOGGLE RUN".
constexpr auto aHelp = text::encode("HELP");   ///< Screen text "HELP".
constexpr auto aW1 = text::encode("WHEEL 1");   ///< Screen text "WHEEL 1".
constexpr auto aW2 = text::encode("WHEEL 2");   ///< Screen text "WHEEL 2".
///@}

/// Action names by Action.
const Code* const kActionNames[input::NUM_ACTIONS] = {
   aUp.codes,   aDown.codes,   aLeft.codes, aRight.codes, aAccept.codes, aBack.codes,
   aMenu.codes, aDevice.codes, aRun.codes,  aHelp.codes,  aW1.codes,     aW2.codes,
};

/// @name Event and filter names
///@{
constexpr auto eNone = text::encode("-");   ///< Screen text "-".
constexpr auto eOpen1 = text::encode("OPEN WHEEL 1");   ///< Screen text "OPEN WHEEL 1".
constexpr auto eOpen2 = text::encode("OPEN WHEEL 2");   ///< Screen text "OPEN WHEEL 2".
constexpr auto eClose1 = text::encode("CLOSE WHEEL 1");   ///< Screen text "CLOSE WHEEL 1".
constexpr auto eClose2 = text::encode("CLOSE WHEEL 2");   ///< Screen text "CLOSE WHEEL 2".
constexpr auto eSleep = text::encode("SLEEP");   ///< Screen text "SLEEP".
constexpr auto eReset = text::encode("SOFT RESET");   ///< Screen text "SOFT RESET".

constexpr auto sIdle = text::encode("IDLE");   ///< Screen text "IDLE".
constexpr auto sPending = text::encode("PENDING");   ///< Screen text "PENDING".
constexpr auto sWheel = text::encode("WHEEL");   ///< Screen text "WHEEL".
constexpr auto sChord = text::encode("CHORD");   ///< Screen text "CHORD".
///@}

/// Shoulder filter state names by ShoulderState.
const Code* const kFilterNames[] = {sIdle.codes, sPending.codes, sWheel.codes, sChord.codes};

/// Display name of a shoulder event.
/// @param e      the event
/// @param wheel  wheel it concerns (OpenWheel / CloseWheel)
/// @return encoded name
const Code* event_name(input::InputEvent e, input::Action wheel)
{
   const bool w1 = wheel == input::ACTION_WHEEL_1;
   switch (e)
   {
   case input::InputEvent::OpenWheel:
      return w1 ? eOpen1.codes : eOpen2.codes;
   case input::InputEvent::CloseWheel:
      return w1 ? eClose1.codes : eClose2.codes;
   case input::InputEvent::Sleep:
      return eSleep.codes;
   case input::InputEvent::SoftReset:
      return eReset.codes;
   default:
      return eNone.codes;
   }
}

/// Whether a button exists on the map's target.
/// @param map  the input map
/// @param k    the button
/// @return false for X / Y on GBA and Select on Dreamcast
bool key_on_target(const input::InputMap& map, hin::Key k)
{
   switch (map.target)
   {
   case input::INPUT_TARGET_GBA:
      return k != hin::KEY_X && k != hin::KEY_Y;
   case input::INPUT_TARGET_DREAMCAST:
      return k != hin::KEY_SELECT;
   default:
      return true;
   }
}

/// Display name of the map's target.
/// @param map  the input map
/// @return encoded name
const text::Code* target_name(const input::InputMap& map)
{
   switch (map.target)
   {
   case input::INPUT_TARGET_GBA:
      return kTargetGba.codes;
   case input::INPUT_TARGET_DREAMCAST:
      return kTargetDc.codes;
   default:
      return kTargetSnes.codes;
   }
}

constexpr int kActionRow0 = 8;   ///< Row of the first action.
constexpr int kBoundCol = 14;    ///< Column of "Bound to".

constexpr int kTargetCol = 12;      ///< Column of the target name, right of the title.
constexpr int kEventRow = 1;        ///< Row of the last shoulder event.
constexpr int kFilterRow = 2;       ///< Row of the shoulder filter state.
constexpr int kHeaderRow = 7;       ///< Row of the action table's header.
constexpr int kEventValueCol = 8;   ///< Column of the event name, after "Event:".
constexpr int kFilterValueCol = 9;   ///< Column of the filter state, after "Filter:".

/// Draws the parts that never change: titles and bindings.
/// @param map  the input map
void draw_static(const input::InputMap& map)
{
   put(BANK_TITLE, 1, 0, kTitle.codes);
   put(BANK_TITLE, kTargetCol, 0, target_name(map));
   put(BANK_DIM, 1, kEventRow, kEventLbl.codes);
   put(BANK_DIM, 1, kFilterRow, kFilterLbl.codes);
   put(BANK_TITLE, 1, kHeaderRow, kHeader.codes);
   for (int a = 0; a < input::NUM_ACTIONS; ++a)
   {
      const int y = kActionRow0 + a;
      if (input::needs_menu_entry(map, static_cast<input::Action>(a)))
      {
         put(BANK_OFF, kBoundCol, y, kMenuEntry.codes);
         continue;
      }
      int x = kBoundCol;
      for (int k = 0; k < hin::NUM_KEYS; ++k)
      {
         if (!(map.bindings[a] & hin::key_bit(static_cast<hin::Key>(k))))
         {
            continue;
         }
         put(BANK_DIM, x, y, kKeyNames[k]);
         x += length(kKeyNames[k]) + 1;
      }
   }
}

/// Draws the per-frame parts: held buttons and actions, filter state, last event.
/// @param map         the input map
/// @param s           this frame's input state
/// @param last_event  the most recent shoulder event
/// @param last_wheel  its wheel
/// @param last_frame  frame it happened on
void draw_live(const input::InputMap& map, const input::InputState& s, input::InputEvent last_event,
               input::Action last_wheel, unsigned last_frame)
{
   for (const KeyLabel& k : kKeyLabels)
   {
      const Bank bank = !key_on_target(map, k.key) ? BANK_OFF : (s.keys & hin::key_bit(k.key)) ? BANK_ON : BANK_DIM;
      put(bank, k.x, k.y, k.text);
   }
   for (int a = 0; a < input::NUM_ACTIONS; ++a)
   {
      const bool on = s.held & input::action_bit(static_cast<input::Action>(a));
      put(on ? BANK_ON : BANK_DIM, 1, kActionRow0 + a, kActionNames[a]);
   }
   blank(kFilterValueCol, kCols, kFilterRow);
   put(BANK_ON, kFilterValueCol, kFilterRow, kFilterNames[static_cast<int>(s.shoulders.state)]);

   blank(kEventValueCol, kCols, kEventRow);
   const Code* name = event_name(last_event, last_wheel);
   put(last_event == input::InputEvent::None ? BANK_DIM : BANK_ON, kEventValueCol, kEventRow, name);
   if (last_event != input::InputEvent::None)
   {
      Code digits[kNumberCodes];
      number(last_frame, digits);
      const int x = kEventValueCol + length(name) + 1;
      put(BANK_DIM, x, kEventRow, kAt.codes);       // "@<frame>" after the event name
      put(BANK_DIM, x + 1, kEventRow, digits);
   }
}

} // namespace

/// Runs the viewer until the platform quits.
/// @return 0
int main()
{
   hal::system::init();
   const input::InputMap& map = input::kInputMap;

   video::set_mode(0);
   video::load_bg_palette(0, &kBackdrop, 1);
   for (uint8_t bank = 0; bank < kNumBanks; ++bank)
   {
      video::load_bg_palette(static_cast<uint8_t>(bank * kColorsPerBank + kInkEntry), &kInk[bank], 1);
      video::load_bg_palette(static_cast<uint8_t>(bank * kColorsPerBank + kShadowEntry), &kShadow, 1);
   }
   text::load_font(text::kDefaultFont, text::kDefaultFontGlyphs, kCharBlock, 0);
   video::configure_bg(kBg, true, 0, kCharBlock, kScreenBlock, false, 0);
   text::clear(target(BANK_DIM), text::kDefaultFont);
   draw_static(map);

   input::InputState state{};
   input::InputEvent last_event = input::InputEvent::None;
   input::Action last_wheel = input::ACTION_WHEEL_1;
   unsigned frame = 0;
   unsigned last_frame = 0;
   while (hal::system::update())
   {
      frame = frame < kMaxFrame ? frame + 1 : frame;   // saturates (P-18)
      input::update(state, hin::held(), map);
      if (state.event != input::InputEvent::None)
      {
         last_event = state.event;
         last_wheel = state.wheel;
         last_frame = frame;
      }
      draw_live(map, state, last_event, last_wheel, last_frame);
   }
   return 0;
}
