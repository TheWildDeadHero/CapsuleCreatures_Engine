/// @file
/// Hello world: prints a greeting with the engine's default font on BG0 and
/// lets the D-pad move it around. Built unchanged for every target; everything
/// platform-specific goes through the HAL.
#include "capsule/hal/input.h"
#include "capsule/input/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/text/encode.h"
#include "capsule/text/font.h"
#include "capsule/text/print.h"

namespace
{

using namespace capsule;
namespace video = hal::video;

constexpr video::BgIndex kTextBg = 0;    ///< Layer the text is on.
constexpr uint8_t kCharBlock = 0;    ///< Character block holding the font.
constexpr uint8_t kScreenBlock = 31;  ///< Last 2 KB of BG VRAM, clear of the font tiles.

constexpr auto kGreeting = text::encode("Hello, world!");            ///< Greeting.
constexpr auto kHint = text::encode("Move me with the D-pad");   ///< Hint line.

constexpr int kGreetingX = 8;   ///< Greeting column (centred on the 30-column screen).
constexpr int kGreetingY = 9;   ///< Greeting row.
constexpr int kHintX = 4;       ///< Hint column.
constexpr int kHintY = 11;      ///< Hint row, one blank row below the greeting.

/// Backdrop and text colors.
constexpr video::Color kPalette[] = {
   video::rgb5(3, 6, 12),    // 0: backdrop (deep blue)
   video::rgb5(31, 31, 31),  // 1: text
   video::rgb5(1, 2, 5),     // 2: text shadow
};

} // namespace

/// Shows the greeting and scrolls it with the D-pad until the platform quits.
/// @return 0
int main()
{
   hal::system::init();

   video::set_mode(0);
   video::load_bg_palette(0, kPalette, sizeof(kPalette) / sizeof(kPalette[0]));
   text::load_font(text::kDefaultFont, text::kDefaultFontGlyphs, kCharBlock, 0);
   video::configure_bg(kTextBg, true, 0, kCharBlock, kScreenBlock, false, 0);

   const text::PrintTarget target{kTextBg, 0, 0};
   text::clear(target, text::kDefaultFont);
   text::print(target, text::kDefaultFont, kGreetingX, kGreetingY, kGreeting.codes);
   text::print(target, text::kDefaultFont, kHintX, kHintY, kHint.codes);

   // Scroll offsets wrap at the 512-pixel hardware range on purpose; the mask
   // keeps the counters inside it so nothing overflows (P-18).
   constexpr int kScrollMask = 511;
   int scroll_x = 0;
   int scroll_y = 0;
   input::InputState in{};
   while (hal::system::update())
   {
      input::update(in, hal::input::held(), input::kInputMap);   // B-11: actions, not buttons
      // Scrolling the layer one way moves the text the other way.
      if (in.held & input::action_bit(input::ACTION_LEFT))
      {
         scroll_x = (scroll_x + 1) & kScrollMask;
      }
      if (in.held & input::action_bit(input::ACTION_RIGHT))
      {
         scroll_x = (scroll_x - 1) & kScrollMask;
      }
      if (in.held & input::action_bit(input::ACTION_UP))
      {
         scroll_y = (scroll_y + 1) & kScrollMask;
      }
      if (in.held & input::action_bit(input::ACTION_DOWN))
      {
         scroll_y = (scroll_y - 1) & kScrollMask;
      }
      video::set_bg_scroll(kTextBg, static_cast<int16_t>(scroll_x), static_cast<int16_t>(scroll_y));
   }
   return 0;
}
