/// @file
/// Palette blends for scenes and animations (see capsule/screen/blend.h).
#include "capsule/screen/blend.h"

#include "capsule/gfx/palette.h"
#include "capsule/screen/sprites.h"

namespace capsule::screen::blend
{
namespace
{

namespace palette = gfx::palette;

constexpr int kAllBanks = palette::kBanks * 2;   ///< BG and OBJ banks (mask bits 0-31).
constexpr uint32_t kBgBanks = 0xFFFFu;           ///< Mask of the BG banks.

}   // namespace

void banks(uint32_t mask, uint8_t coef, hal::video::Color color)
{
   for (int b = 0; b < kAllBanks; ++b)
   {
      if ((mask >> b & 1u) == 0)
      {
         continue;
      }
      const bool obj = b >= palette::kObjBankShift;
      const int bank = obj ? b - palette::kObjBankShift : b;
      palette::blend_range(obj, bank * palette::kBankColors, palette::kBankColors, coef, color);
   }
}

void banks_unfaded(uint32_t mask, uint8_t coef, hal::video::Color color)
{
   // every entry's loaded colours first (a blend of 0 shows them as loaded), one range per palette
   palette::blend_range(false, 0, palette::kColors, 0, color);
   palette::blend_range(true, 0, palette::kColors, 0, color);
   banks(mask, coef, color);
}

void range(bool obj, int first, int count, uint8_t coef, hal::video::Color color)
{
   palette::blend_range(obj, first, count, coef, color);
}

uint32_t tag_mask(uint16_t tag)
{
   const uint8_t bank = sprites::palette_bank(tag);
   return bank == sprites::kNoBank ? 0u : palette::obj_bank(bank);
}

bool tag(uint16_t tag, uint8_t coef, hal::video::Color color)
{
   const uint32_t bit = tag_mask(tag);
   if (bit == 0)
   {
      return false;
   }
   banks(bit, coef, color);
   return true;
}

bool fade_tags(const uint16_t* tags, int count, uint32_t bg_mask, int8_t delay, uint8_t from, uint8_t to,
               hal::video::Color color, const fade::FadeStyle& style)
{
   uint32_t mask = bg_mask & kBgBanks;
   for (int i = 0; i < count; ++i)
   {
      mask |= tag_mask(tags[i]);
   }
   return fade::begin(fade::Fade{mask, delay, from, to, color}, style);
}

bool step(Ramp& r)
{
   if (r.wait != 0)
   {
      --r.wait;
      return true;
   }
   banks(r.mask, r.coef, r.color);
   if (r.coef == r.target)
   {
      return false;
   }
   r.coef = static_cast<uint8_t>(r.coef < r.target ? r.coef + 1 : r.coef - 1);
   r.wait = r.delay;
   return true;
}

}   // namespace capsule::screen::blend
