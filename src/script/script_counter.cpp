/// @file
/// C5: the engine's default counter box (capsule/script/port.h world::counter()): the text layer's banner, one line:
/// the label, a space, the value; Update prints the value again over the old one, Hide closes the banner. Weak
/// (CAPSULE_WEAK): a game that draws its own boxes defines world::counter() itself. Linked wherever the other
/// world defaults are (src/script/script_port_defaults.cpp).
#include "capsule/core/memory.h"
#include "capsule/hal/video.h"
#include "capsule/script/port.h"
#include "capsule/text/text_layer.h"

namespace capsule::script::world
{
namespace
{

namespace layer = text::layer;

constexpr int kCounterDigits = 10;          ///< C5: the most digits of a value (32 bits).
constexpr int kCounterCodes = 32;           ///< C5: the most codes of a counter's line.
constexpr int kDecimalBase = 10;            ///< C5: digits are decimal.
constexpr uint8_t kFrameCells = 2;          ///< C5: the banner's frame cells (both sides; and above / below).
constexpr uint8_t kCounterLines = 1;        ///< C5: a counter's lines inside its frame.
constexpr uint8_t kScreenCells = 30;        ///< C5: the screen's width in cells (the banner stays on it).
/// C5: the default counter's colours (entries 1-15 of the banner bank: ink, shadow, fill, the box frame).
constexpr hal::video::Color kCounterColors[] = {
   hal::video::rgb5(4, 4, 4),     // ink
   hal::video::rgb5(20, 20, 20),  // shadow
   hal::video::rgb5(31, 31, 31),  // fill
   hal::video::rgb5(8, 8, 16),    // frame
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

/// C5: the open default counter (the banner holds one).
struct Counter
{
   bool open;          ///< Shown.
   uint8_t box;        ///< Its box.
   uint8_t width;      ///< The banner's cells.
   uint8_t value_at;   ///< The value's first column inside the frame.
};
Counter g_counter{};   ///< The default counter.

/// C5: a value's decimal digits (no sign: 0 for a value below 0).
/// @param value  the value
/// @param out    kCounterDigits + 1 codes (ends with kCodeEnd)
/// @return digits written
int counter_digits(int32_t value, text::Code* out)
{
   text::Code rev[kCounterDigits];
   int n = 0;
   uint32_t v = value > 0 ? static_cast<uint32_t>(value) : 0;
   do
   {
      rev[n++] = static_cast<text::Code>(text::kCodeDigitZero + v % kDecimalBase);
      v /= kDecimalBase;
   }
   while (v > 0 && n < kCounterDigits);
   for (int i = 0; i < n; ++i)
   {
      out[i] = rev[n - 1 - i];
   }
   out[n] = text::kCodeEnd;
   return n;
}

} // namespace

CAPSULE_WEAK void counter(CounterOp op, uint8_t box, int32_t value, uint8_t x, uint8_t y, const text::Code* label)
{
   text::Code digits[kCounterDigits + 1];
   counter_digits(value, digits);
   switch (op)
   {
   case CounterOp::Show:
   {
      int label_cells = 0;
      while (label != nullptr && label[label_cells] != text::kCodeEnd && label_cells < kCounterCodes)
      {
         ++label_cells;
      }
      // the label, a space, room for the most digits, the frame; kept on the screen
      int w = label_cells + 1 + kCounterDigits + kFrameCells;
      w = w > kScreenCells ? kScreenCells : w;
      const int left = x + w > kScreenCells ? kScreenCells - w : x;
      layer::open_banner(static_cast<uint8_t>(left), y, static_cast<uint8_t>(w),
                         static_cast<uint8_t>(kCounterLines + kFrameCells), nullptr, kCounterColors);
      if (label != nullptr)
      {
         layer::print(layer::kBannerTarget, 0, 0, layer::Align::Left, false, label);
      }
      g_counter = Counter{true, box, static_cast<uint8_t>(w), static_cast<uint8_t>(label_cells + 1)};
      layer::print(layer::kBannerTarget, g_counter.value_at, 0, layer::Align::Left, false, digits);
      return;
   }
   case CounterOp::Update:
      if (g_counter.open && g_counter.box == box && layer::banner_open())
      {
         // the old value's cells first (a shorter value leaves none behind)
         text::Code blank[kCounterDigits + 1];
         for (int i = 0; i < kCounterDigits; ++i)
         {
            blank[i] = text::kCodeSpace;
         }
         blank[kCounterDigits] = text::kCodeEnd;
         layer::print(layer::kBannerTarget, g_counter.value_at, 0, layer::Align::Left, false, blank);
         layer::print(layer::kBannerTarget, g_counter.value_at, 0, layer::Align::Left, false, digits);
      }
      return;
   case CounterOp::Hide:
      if (g_counter.open && g_counter.box == box)
      {
         layer::close_banner();
         g_counter = Counter{};
      }
      return;
   case CounterOp::NUM_COUNTER_OPS:
      break;
   }
}

} // namespace capsule::script::world
