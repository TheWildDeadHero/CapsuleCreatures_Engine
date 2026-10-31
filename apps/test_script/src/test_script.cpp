/// @file
/// Script test: runs the demo scripts (data/demo.cscript) in the VM with the
/// engine's port (messages and menus in the text box) and shows the VM's
/// state, the flags and the variables on BG1.
///
/// Idle: Up / Down pick a script, A runs it, START enters the map "room"
/// (runs its on_load, arms its trigger; 3 coins fire it), SELECT clears every
/// bank, L starts a new day, R changes area. Running: the script reads A / B
/// and the D-pad; B on the list stops nothing (scripts end themselves).
#include "capsule/asset/asset.h"
#include "capsule/gfx/background.h"
#include "capsule/gfx/palette.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/script/port.h"
#include "capsule/script/script.h"
#include "capsule/text/text_layer.h"

#include "script_ids.h"

#include <cstdint>

namespace
{

using namespace capsule;
namespace layer = text::layer;
namespace video = hal::video;
using hal::input::key_bit;
using hal::input::KeyBits;

constexpr uint8_t kPanel = 1;     ///< Text target for the panel (BG1).
constexpr int kListRow = 2;       ///< First row of the script list.
constexpr int kStateRow = 12;     ///< VM state row.

constexpr int kLineChars = 30;      ///< Characters in one panel line (the screen width).
constexpr int kTextBuffer = 32;     ///< Size of a scratch line buffer (a line, its NUL and room to spare).
constexpr int kNumberBuffer = 12;   ///< Size of a number buffer (10 digits, NUL, one spare).
constexpr uint32_t kDecimalBase = 10;   ///< Base of decimal numbers.

/// @name Panel inks (text styles)
///@{
constexpr uint8_t kStyleBody = 0;        ///< Plain text.
constexpr uint8_t kStyleHighlight = 1;   ///< Title and the selected script.
constexpr uint8_t kStyleLabel = 2;       ///< Labels and hints.
constexpr uint8_t kStyleAlert = 3;       ///< The beep.
///@}

/// @name Panel layout
///@{
constexpr int kMapStatusCol = 16;   ///< Column of the map status, right of the title.
constexpr int kListMax = 9;         ///< Scripts the list shows at most.
constexpr int kVmValueCol = 4;      ///< Column of the VM state, after "VM:".
constexpr int kNamesShown = 3;      ///< Flags (and variables) shown, three to a line.
constexpr int kFlagNameChars = 7;   ///< Letters of a flag name shown.
constexpr int kBeepCol = 20;        ///< Column of "BEEP!".
constexpr int kHintRow = 15;        ///< First row of the idle controls hint.
///@}

/// @name Demo natives and areas
///@{
constexpr uint8_t kAddArgs = 2;        ///< Arguments of add(a, b).
constexpr uint16_t kBeepFrames = 60;   ///< Length of one beep: a second at 60 frames per second.
constexpr uint16_t kFromArea = 1;      ///< Area R leaves.
constexpr uint16_t kToArea = 2;        ///< Area R enters.
///@}

/// App state.
struct App
{
   script::Context vm;      ///< The field context.
   uint16_t cursor;         ///< Selected script.
   KeyBits held;            ///< Keys last frame.
   bool in_room;            ///< Entered "room".
   uint16_t beep_frames;    ///< Frames left of the current beep (native).
   uint8_t beeps;           ///< Beeps done.
};
App g{};   ///< The app.

constexpr int kRows = 18;   ///< Panel rows the app writes.

/// One panel line: text and ink.
struct Line
{
   char text[kLineChars + 1];   ///< NUL-terminated.
   uint8_t style;   ///< Ink 0-3.
};
Line g_lines[kRows];   ///< This frame's panel.
Line g_shown[kRows];   ///< What is on screen (only changed lines are rewritten).

/// Writes text into this frame's panel (lines start blank each frame).
/// @param x      column
/// @param y      row
/// @param s      NUL-terminated text
/// @param style  ink 0-3 (of the whole line)
void put(int x, int y, const char* s, uint8_t style = 0)
{
   if (y < 0 || y >= kRows)
   {
      return;
   }
   Line& l = g_lines[y];
   int len = 0;
   while (l.text[len] != '\0')
   {
      ++len;
   }
   while (len < x && len < kLineChars)
   {
      l.text[len++] = ' ';
   }
   for (int i = 0; s[i] != '\0' && x + i < kLineChars; ++i)
   {
      l.text[x + i] = s[i];
      if (x + i >= len)
      {
         len = x + i + 1;
      }
   }
   l.text[len] = '\0';
   l.style = style > l.style ? style : l.style;
}

/// Rewrites the panel lines that changed since the last frame.
void flush_panel()
{
   for (int y = 0; y < kRows; ++y)
   {
      Line& a = g_lines[y];
      Line& b = g_shown[y];
      bool same = a.style == b.style;
      for (int i = 0; same && i < kLineChars + 1; ++i)
      {
         same = a.text[i] == b.text[i];
         if (a.text[i] == '\0')
         {
            break;
         }
      }
      if (!same)
      {
         layer::clear(kPanel, static_cast<uint8_t>(y), 1);
         text::Code codes[kTextBuffer];
         int n = 0;
         while (a.text[n] != '\0' && n < kLineChars)
         {
            codes[n] = static_cast<text::Code>(a.text[n]);
            ++n;
         }
         codes[n] = text::kCodeEnd;
         layer::set_style(a.style);
         layer::print(kPanel, 0, static_cast<uint8_t>(y), layer::Align::Left, false, codes);
         b = a;
      }
      a = Line{};
   }
}

/// Formats an unsigned number.
/// @param out    buffer of at least 12 chars
/// @param value  the number
/// @return out
char* num(char* out, uint32_t value)
{
   char tmp[kNumberBuffer];
   int n = 0;
   do
   {
      tmp[n++] = static_cast<char>('0' + value % kDecimalBase);
      value /= kDecimalBase;
   }
   while (value != 0);
   int i = 0;
   while (n > 0)
   {
      out[i++] = tmp[--n];
   }
   out[i] = '\0';
   return out;
}

/// Appends to a string.
/// @param dst  NUL-terminated buffer (large enough)
/// @param src  text to append
/// @return dst
char* cat(char* dst, const char* src)
{
   char* p = dst;
   while (*p != '\0')
   {
      ++p;
   }
   while (*src != '\0')
   {
      *p++ = *src++;
   }
   *p = '\0';
   return dst;
}

/// Name of a script (debug names are compiled in).
/// @param s  script ID
/// @return its name
const char* script_name(uint16_t s)
{
   return script::kBank.script_names != nullptr ? script::kBank.script_names[s] : "?";
}

/// Scripts shown in the list (the generated trigger conditions are left out).
/// @return count
uint16_t listed()
{
   uint16_t n = 0;
   while (n < script::kBank.script_count)
   {
      const char* name = script_name(n);
      bool generated = false;
      for (const char* p = name; *p != '\0'; ++p)
      {
         generated = generated || *p == '.';
      }
      if (generated)
      {
         break;
      }
      ++n;
   }
   return n;
}

/// Redraws the panel.
void draw()
{
   put(0, 0, "SCRIPT TEST", kStyleHighlight);
   put(kMapStatusCol, 0, g.in_room ? "map: room" : "no map", kStyleLabel);
   const uint16_t n = listed();
   for (uint16_t i = 0; i < n && i < kListMax; ++i)
   {
      char line[kTextBuffer] = "";
      cat(line, i == g.cursor ? "> " : "  ");
      cat(line, script_name(i));
      put(0, kListRow + i, line, i == g.cursor ? kStyleHighlight : kStyleBody);
   }
   char line[kTextBuffer] = "";
   char buf[kNumberBuffer];
   if (script::running(g.vm))
   {
      static const char* const kWait[] = {"run", "delay", "typing", "button", "yes/no", "menu", "native"};
      const script::Frame& f = g.vm.stack[g.vm.depth - 1];
      cat(line, script_name(f.script));
      cat(line, " pc ");
      cat(line, num(buf, f.pc));
      cat(line, " d");
      cat(line, num(buf, g.vm.depth));
      cat(line, " ");
      cat(line, kWait[static_cast<int>(g.vm.wait)]);
   }
   else
   {
      cat(line, "idle");
   }
   put(0, kStateRow - 1, "VM:", kStyleLabel);
   put(kVmValueCol, kStateRow - 1, line, kStyleHighlight);

   // Flags and variables.
   const script::Bank& b = script::kBank;
   char flags[kTextBuffer] = "";
   for (uint16_t i = 0; i < b.flag_first[script::kNumLifetimes] && i < kNamesShown; ++i)
   {
      char name[kFlagNameChars + 1] = "";
      for (int k = 0; k < kFlagNameChars && b.flag_names[i][k] != '\0'; ++k)
      {
         name[k] = b.flag_names[i][k];
      }
      cat(flags, name);   // first 7 letters, so three fit a line
      cat(flags, script::flag(script::FlagId{i}) ? "=1 " : "=0 ");
   }
   put(0, kStateRow, flags);
   char vars[kTextBuffer] = "";
   for (uint16_t i = 0; i < b.var_count && i < kNamesShown; ++i)
   {
      cat(vars, b.var_names[i]);
      cat(vars, "=");
      cat(vars, num(buf, static_cast<uint32_t>(script::var(script::VarId{i}))));
      cat(vars, " ");
   }
   put(0, kStateRow + 1, vars);
   if (g.beep_frames != 0)
   {
      put(kBeepCol, kStateRow - 1, "BEEP!", kStyleAlert);
   }
   if (!script::running(g.vm))
   {
      put(0, kHintRow, "A run  START room  SELECT clear", kStyleLabel);
      put(0, kHintRow + 1, "L new day  R new area", kStyleLabel);
   }
}

} // namespace

namespace capsule::script::game
{

// The demo's natives: beep(times) waits a second per beep; add(a, b) adds.
// (Documented with the declaration in capsule/script/port.h.)
NativeStatus native(uint16_t id, const int32_t* args, uint8_t argc, int32_t& result)
{
   if (id == script_ids::NATIVE_ADD && argc == kAddArgs)
   {
      result = args[0] + args[1];
      return NativeStatus::Done;
   }
   if (id == script_ids::NATIVE_BEEP && argc == 1)
   {
      if (g.beep_frames == 0)
      {
         if (g.beeps >= args[0])
         {
            g.beeps = 0;
            return NativeStatus::Done;
         }
         ++g.beeps;
         g.beep_frames = kBeepFrames;
      }
      return NativeStatus::Wait;
   }
   result = 0;
   return NativeStatus::Done;
}

} // namespace capsule::script::game

/// Runs the script test until the platform quits.
/// @return 0
int main()
{
   hal::system::init();
   gfx::palette::reset();
   gfx::background::reset();
   constexpr video::Color kBackdrop = video::rgb5(2, 3, 6);   // dark blue-grey
   gfx::palette::load_bg(0, &kBackdrop, 1);
   layer::reset();
   script::clear_all();
   while (hal::system::update())
   {
      const KeyBits now = hal::input::held();
      const auto pressed = static_cast<KeyBits>(now & ~g.held);
      g.held = now;
      if (g.beep_frames != 0)
      {
         --g.beep_frames;
      }
      const bool was_running = script::running(g.vm);
      script::frame();        // input edges for the port, once per frame
      script::update(g.vm);
      if (!was_running && !script::running(g.vm))
      {
         const uint16_t n = listed();
         if (pressed & key_bit(hal::input::KEY_UP))
         {
            g.cursor = static_cast<uint16_t>(g.cursor == 0 ? n - 1 : g.cursor - 1);
         }
         if (pressed & key_bit(hal::input::KEY_DOWN))
         {
            g.cursor = static_cast<uint16_t>(g.cursor + 1 >= n ? 0 : g.cursor + 1);
         }
         if (pressed & key_bit(hal::input::KEY_A))
         {
            script::start(g.vm, script::ScriptId{g.cursor});
         }
         if (pressed & key_bit(hal::input::KEY_START))
         {
            script::enter_map(asset::key_v1("map:ROOM"));
            g.in_room = true;
         }
         if (pressed & key_bit(hal::input::KEY_SELECT))
         {
            script::clear_all();
         }
         if (pressed & key_bit(hal::input::KEY_L))
         {
            script::new_day();
         }
         if (pressed & key_bit(hal::input::KEY_R))
         {
            script::change_map(kFromArea, kToArea);
         }
         script::ScriptId fired{};
         if (!script::running(g.vm) && script::poll_triggers(fired))
         {
            script::start(g.vm, fired);
         }
      }
      layer::update();
      draw();
      flush_panel();
   }
   return 0;
}
