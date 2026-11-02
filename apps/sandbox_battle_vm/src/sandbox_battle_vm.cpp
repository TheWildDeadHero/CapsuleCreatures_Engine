/// @file
/// The battle script machine's sandbox (M5 BX, simple shapes): the script bank data/sandbox_bank.json (assembled
/// at build time by tools/script_bank.py, its commands declared by data/sandbox_vm_ext.json, D-39) drives two
/// squares through the controller channel; every third lap both squares make a wide slide (a lap counter the
/// script keeps with the generic aliases addbyte / jumpifbyte). The logic side is the
/// script machine, one command a frame; the presentation side is two endpoints, each sliding its square (its own
/// background layer) a pixel a frame and finishing its command when the square arrives. The machine waits while
/// a square moves. The top line shows the label the script is in, its position and the exec mask. A pauses or
/// resumes the script, B starts it again.
#include "capsule/battle/controller.h"
#include "capsule/battle/vm.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/input/input.h"
#include "capsule/text/encode.h"
#include "capsule/text/font.h"
#include "capsule/text/print.h"

#include "sandbox_bank_ids.h"

#include <cstdint>

namespace sandbox_bank
{
extern const uint8_t kBank[];      ///< The assembled bank (generated).
extern const uint32_t kBankSize;   ///< Its bytes.
}   // namespace sandbox_bank

namespace
{

using namespace capsule;
namespace video = hal::video;
namespace vm = battle::vm;
namespace ctl = battle::controller;

/// @name Video layout
///@{
constexpr video::BgIndex kTextBg = 0;          ///< The status line.
constexpr video::BgIndex kFirstBoxBg = 1;      ///< Endpoint e's square is on layer kFirstBoxBg + e.
constexpr uint8_t kFontBlock = 0;              ///< Character block of the font.
constexpr uint8_t kBoxBlock = 1;               ///< Character block of the square's tile.
constexpr uint8_t kTextScreen = 31;            ///< Screen block of the status line.
constexpr uint8_t kFirstBoxScreen = 29;        ///< Endpoint e's layer uses screen block kFirstBoxScreen - e.
constexpr uint8_t kBoxPalette = 1;             ///< Endpoint e's colour is in palette bank kBoxPalette + e.
constexpr uint8_t kBoxColor = 1;               ///< The colour index of a square's tile.
constexpr uint32_t kNibbleSpread = 0x11111111u;   ///< A 4bpp row of one colour: index * this.
constexpr int kTileWords = 8;                  ///< u32 rows of a 4bpp tile.
constexpr int kBoxTiles = 4;                   ///< A square's side in tiles.
constexpr int kBoxY = 8;                       ///< Its top row (tiles).
constexpr int kBoxX[] = {6, 20};               ///< Each endpoint's start column (tiles).
constexpr int kStatusX = 0;                    ///< The status line's column.
constexpr int kStatusY = 1;                    ///< Its row.
constexpr int kHintY = 18;                     ///< The key hint's row.
///@}

/// @name Script commands (data/sandbox_bank.json's ids) and controller commands
///@{
constexpr uint8_t kCmdSlide = 1;          ///< Controller command: slide by a signed byte.
constexpr uint32_t kSlideBytes = 3;       ///< slide: id, endpoint, pixels.
constexpr uint32_t kSlideBothBytes = 2;   ///< slide_both: id, pixels.
constexpr uint32_t kRefAt = 1;            ///< A command's first operand.
constexpr uint32_t kSecondAt = 2;         ///< Its second (after a byte).
constexpr uint16_t kLapVar = 0;           ///< The lap counter's variable id (data/sandbox_bank.json).
///@}

constexpr uint8_t kEndpoints = 2;           ///< The squares.
constexpr uint16_t kBufferBytes = 8;        ///< Bytes of a command or reply buffer.
constexpr int kHexDigits = 4;               ///< Digits of the position and the mask.
constexpr int kNibbleBits = 4;              ///< Bits of a hex digit.
constexpr uint32_t kNibbleMask = 0xFu;      ///< A hex digit's bits.
constexpr int kDecimalDigits = 10;          ///< Digits before the letters.
constexpr int kLineCodes = 30;              ///< The status line's glyphs (the screen's width).
constexpr int kNameCodes = 15;              ///< The label's part of the status line.
constexpr int kPixelBias = 128;             ///< A signed byte stored in a command byte.

/// Colours: backdrop, text, text shadow.
constexpr video::Color kTextPalette[] = {video::rgb5(2, 4, 8), video::rgb5(31, 31, 31), video::rgb5(1, 2, 4)};
/// The squares' palette banks (entry 0 transparent, entry 1 the square).
constexpr video::Color kBoxColors[kEndpoints][2] = {{0, video::rgb5(31, 18, 4)}, {0, video::rgb5(6, 22, 31)}};

constexpr auto kHint = text::encode("A pause/resume  B restart");   ///< The key hint.

/// The sandbox's state.
struct Sandbox
{
   ctl::Buffers<kEndpoints, kBufferBytes> buffers;   ///< The channel's storage.
   ctl::Channel channel;                               ///< Its view.
   vm::Bank bank;                                      ///< The script bank.
   vm::Machine machine;                                ///< The logic side.
   int16_t offset[kEndpoints];                         ///< Each square's slide so far (pixels).
   int16_t target[kEndpoints];                         ///< Where its command takes it.
   uint8_t laps;                                       ///< The script's lap counter (variable 0).
   bool paused;                                        ///< A paused the script.
};

Sandbox g;   ///< The state.

/// Sends a slide to an endpoint.
/// @param endpoint  the square
/// @param pixels    how far (negative: left)
void send_slide(uint8_t endpoint, int8_t pixels)
{
   const uint8_t data[] = {kCmdSlide, static_cast<uint8_t>(pixels + kPixelBias)};
   ctl::send(g.channel, endpoint, data, sizeof(data));
}

/// slide: sends one square its slide.
/// @param m  the machine
void cmd_slide(vm::Machine& m, void*)
{
   send_slide(vm::u8_at(m, kRefAt), vm::s8_at(m, kSecondAt));
   vm::advance(m, kSlideBytes);
}

/// slide_both: sends both squares a slide in one command.
/// @param m  the machine
void cmd_slide_both(vm::Machine& m, void*)
{
   // one command for both endpoints: the next command waits for both
   for (uint8_t e = 0; e < kEndpoints; ++e)
   {
      send_slide(e, vm::s8_at(m, kRefAt));
   }
   vm::advance(m, kSlideBothBytes);
}

/// The script's variables (Variable references: id 0 is the lap counter).
/// @param ref    the reference
/// @param bytes  bytes read or written
/// @return them, or nullptr (Fault::BadRef)
uint8_t* resolve_ref(vm::Machine&, vm::Ref ref, uint16_t bytes, void*)
{
   const bool lap = vm::ref_kind(ref) == vm::RefKind::Variable && vm::ref_id(ref) == kLapVar;
   return lap && vm::ref_offset(ref) + bytes <= sizeof(g.laps) ? &g.laps + vm::ref_offset(ref) : nullptr;
}

// The command table, generated from data/sandbox_vm_ext.json (tools/vm_ext.py, D-39): goto, pause, call, return,
// addbyte and jumpifbyte are aliases of the engine's generic steps; slide and slide_both are the natives above.
#if __has_include("sandbox_dispatch.inc")   // generated by the build (Doxygen reads the source without it)
#include "sandbox_dispatch.inc"
#else
#error "sandbox_dispatch.inc is generated by the build (tools/vm_ext.py)"
#endif

void step_idle(uint8_t endpoint, void* context);   // below

/// An endpoint's slide, a pixel a frame; the command is done when the square arrives.
/// @param endpoint  the square
void step_slide(uint8_t endpoint, void*)
{
   int16_t& at = g.offset[endpoint];
   at = static_cast<int16_t>(at + (g.target[endpoint] > at ? 1 : -1));
   if (at == g.target[endpoint])
   {
      ctl::set_step(g.channel, endpoint, step_idle);
      ctl::complete(g.channel, endpoint);
   }
}

/// The slide command's handler: sets the target and hands the endpoint to step_slide.
/// @param endpoint  the square
void handle_slide(uint8_t endpoint, void*)
{
   const int pixels = ctl::command(g.channel, endpoint)[1] - kPixelBias;
   g.target[endpoint] = static_cast<int16_t>(g.offset[endpoint] + pixels);
   if (pixels == 0)
   {
      ctl::complete(g.channel, endpoint);
      return;
   }
   ctl::set_step(g.channel, endpoint, step_slide);
}

/// The controllers' handlers (controller command ids).
constexpr ctl::Handler kHandlers[] = {nullptr, handle_slide};
/// As the channel takes them (unknown ids complete at once).
constexpr ctl::HandlerTable kHandlerTable = {kHandlers, sizeof(kHandlers) / sizeof(kHandlers[0]), nullptr};

/// An endpoint waiting for a command.
/// @param endpoint  the square
void step_idle(uint8_t endpoint, void*)
{
   ctl::dispatch(g.channel, endpoint, kHandlerTable, nullptr);
}

/// Writes a number's hex digits into codes.
/// @param out    receives kHexDigits codes
/// @param value  the number
void put_hex(text::Code* out, uint32_t value)
{
   for (int i = kHexDigits - 1; i >= 0; --i)
   {
      const uint32_t d = value & kNibbleMask;
      out[i] = static_cast<text::Code>(d < static_cast<uint32_t>(kDecimalDigits) ? text::kCodeDigitZero + d
                                                                                  : 'A' + d - kDecimalDigits);
      value >>= kNibbleBits;
   }
}

/// Prints the status line: the label the script is in, its position and the exec mask.
void print_status()
{
   text::Code line[kLineCodes + 1];
   int n = 0;
   const uint16_t label = vm::label_before(g.bank, g.machine.ip);
   const char* name = label != vm::kNoLabel ? vm::label_name(g.bank, label) : nullptr;
   for (const char* c = name != nullptr ? name : "?"; *c != '\0' && n < kNameCodes; ++c)
   {
      line[n++] = static_cast<text::Code>(*c);   // the default code page is ASCII
   }
   while (n < kNameCodes)
   {
      line[n++] = text::kCodeSpace;
   }
   put_hex(&line[n], g.machine.ip);
   n += kHexDigits;
   line[n++] = text::kCodeSpace;
   put_hex(&line[n], *g.channel.exec);
   n += kHexDigits;
   line[n++] = text::kCodeSpace;
   line[n++] = static_cast<text::Code>(g.paused ? 'P' : vm::blocked(g.machine) ? 'W' : 'R');
   line[n] = text::kCodeEnd;
   text::print(text::PrintTarget{kTextBg, 0, 0}, text::kDefaultFont, kStatusX, kStatusY, line);
}

/// Sets the video up: the font, the status line, the two squares.
void setup_video()
{
   video::set_mode(0);
   video::load_bg_palette(0, kTextPalette, sizeof(kTextPalette) / sizeof(kTextPalette[0]));
   text::load_font(text::kDefaultFont, text::kDefaultFontGlyphs, kFontBlock, 0);
   video::configure_bg(kTextBg, true, 0, kFontBlock, kTextScreen, false, 0);
   text::clear(text::PrintTarget{kTextBg, 0, 0}, text::kDefaultFont);
   text::print(text::PrintTarget{kTextBg, 0, 0}, text::kDefaultFont, kStatusX, kHintY, kHint.codes);
   uint32_t tile[kTileWords];
   for (uint32_t& row : tile)
   {
      row = kBoxColor * kNibbleSpread;
   }
   video::load_bg_tiles(kBoxBlock, 1, tile, 1);   // tile 0 stays empty: the layers' clear cells
   for (uint8_t e = 0; e < kEndpoints; ++e)
   {
      const video::BgIndex bg = static_cast<video::BgIndex>(kFirstBoxBg + e);
      video::load_bg_palette(static_cast<uint8_t>((kBoxPalette + e) * video::kColorsPerBank), kBoxColors[e],
                            sizeof(kBoxColors[e]) / sizeof(kBoxColors[e][0]));
      video::configure_bg(bg, true, 1, kBoxBlock, static_cast<uint8_t>(kFirstBoxScreen - e), false, 0);
      for (int y = 0; y < kBoxTiles; ++y)
      {
         for (int x = 0; x < kBoxTiles; ++x)
         {
            video::write_map_cell(bg, kBoxX[e] + x, kBoxY + y,
                                  video::map_cell(1, static_cast<uint8_t>(kBoxPalette + e)));
         }
      }
   }
}

/// Starts (or restarts) the script and the squares.
void restart()
{
   ctl::reset(g.channel);
   for (uint8_t e = 0; e < kEndpoints; ++e)
   {
      ctl::set_step(g.channel, e, step_idle);
      g.offset[e] = 0;
      g.target[e] = 0;
   }
   g.laps = 0;
   vm::reset(g.machine, g.bank);
   vm::wait_on(g.machine, g.channel.exec);
   vm::start_label(g.machine, sandbox_bank::kLabelLoop);
}

} // namespace

/// Runs the sandbox until the platform quits.
/// @return 0, or 1 when the bank does not open
int main()
{
   hal::system::init();
   setup_video();
   g.channel = g.buffers.view();
   if (vm::open(g.bank, sandbox_bank::kBank, sandbox_bank::kBankSize) != vm::BankError::None)
   {
      return 1;
   }
   restart();
   input::InputState in{};
   while (hal::system::update())
   {
      input::update(in, hal::input::held(), input::kInputMap);
      if (in.pressed & input::action_bit(input::ACTION_ACCEPT))
      {
         g.paused = !g.paused;
      }
      if (in.pressed & input::action_bit(input::ACTION_BACK))
      {
         restart();
      }
      // a frame: the logic's command, then each endpoint's step (the order a game's frame keeps)
      if (!g.paused)
      {
         vm::step(g.machine, kTable, nullptr);
         ctl::run(g.channel, kEndpoints, nullptr);
      }
      for (uint8_t e = 0; e < kEndpoints; ++e)
      {
         // scrolling the layer left moves its square right
         video::set_bg_scroll(static_cast<video::BgIndex>(kFirstBoxBg + e), static_cast<int16_t>(-g.offset[e]), 0);
      }
      print_status();
   }
   return 0;
}
