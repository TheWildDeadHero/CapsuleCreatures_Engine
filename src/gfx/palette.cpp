/// @file
/// Palette manager with software fade (see capsule/gfx/palette.h).
#include "capsule/gfx/palette.h"

#include "capsule/core/assert.h"
#include "capsule/core/memory.h"
#include "capsule/hal/vram_queue.h"

namespace capsule::gfx::palette
{
namespace
{

using hal::video::Color;

constexpr uint32_t kColorBytes = 2u;   ///< Bytes of one color in palette RAM.

// The loaded colors are only read when a range is recomputed (a load or a
// fade step): EWRAM. The visible colors stay in IWRAM: the VRAM queue copies
// them in VBlank (Source::Iwram), where every cycle counts.
CAPSULE_EWRAM_BSS Color g_base_bg[kColors];    ///< Loaded BG colors.
CAPSULE_EWRAM_BSS Color g_base_obj[kColors];   ///< Loaded OBJ colors.
Color g_out_bg[kColors];        ///< Visible BG colors; the VRAM queue reads them when it lands (A-5).
Color g_out_obj[kColors];       ///< Visible OBJ colors; see g_out_bg.
constexpr int kAllBanks = kBanks * 2;   ///< BG and OBJ banks (fade mask bits 0-31).
uint8_t g_coef[kAllBanks];      ///< UC: each bank's fade amount, 0-16 (bit order of the fade mask).
Color g_target[kAllBanks];      ///< UC: each bank's fade color.
Rounding g_rounding = Rounding::Truncate;   ///< UC: how fades blend.

/// Recomputes the visible colors of a range and queues exactly that range
/// (entries outside it may hold colors written straight to the hardware;
/// adjacent ranges still merge into one queue request).
/// @param base    loaded colors
/// @param out     visible colors
/// @param region  palette region
/// @param shift   0 for BG, 16 for OBJ (bit of bank 0 in the fade mask)
/// @param first   first entry
/// @param count   entries
void show(const Color* base, Color* out, hal::video::Region region, int shift, int first, int count)
{
   if (count <= 0)
   {
      return;
   }
   for (int i = first; i < first + count; ++i)
   {
      const int bank = shift + i / kBankColors;
      const uint8_t coef = g_coef[bank];
      if (coef == 0)
      {
         out[i] = base[i];
      }
      else
      {
         out[i] = g_rounding == Rounding::Floor ? blend_floor(base[i], g_target[bank], coef)
                                                : blend(base[i], g_target[bank], coef);
      }
   }
   hal::vram::copy16(region, static_cast<uint32_t>(first) * kColorBytes, out + first, static_cast<uint32_t>(count),
                     hal::vram::Priority::MustLand, hal::video::Source::Iwram);
}

/// Shows a BG range (see show()).
/// @param first  first entry
/// @param count  entries
void show_bg(int first, int count)
{
   show(g_base_bg, g_out_bg, hal::video::Region::BgPalette, 0, first, count);
}

/// Shows an OBJ range (see show()).
/// @param first  first entry
/// @param count  entries
void show_obj(int first, int count)
{
   show(g_base_obj, g_out_obj, hal::video::Region::ObjPalette, kObjBankShift, first, count);
}

} // namespace

void reset()
{
   for (Color& c : g_base_bg)
   {
      c = 0;
   }
   for (Color& c : g_base_obj)
   {
      c = 0;
   }
   for (int b = 0; b < kAllBanks; ++b)
   {
      g_coef[b] = 0;
      g_target[b] = 0;
   }
   show_bg(0, kColors);
   show_obj(0, kColors);
}

void load_bg(uint8_t first, const hal::video::Color* colors, uint16_t count)
{
   ENGINE_ASSERT(first + count <= kColors);
   for (uint16_t i = 0; i < count; ++i)
   {
      g_base_bg[first + i] = colors[i];
   }
   show_bg(first, count);
}

void load_obj(uint8_t first, const hal::video::Color* colors, uint16_t count)
{
   ENGINE_ASSERT(first + count <= kColors);
   for (uint16_t i = 0; i < count; ++i)
   {
      g_base_obj[first + i] = colors[i];
   }
   show_obj(first, count);
}

namespace
{

/// UC: sets the fade of the banks in @p change: those in @p mask get (target, coef), the others none; the banks
/// whose fade changed are shown again.
/// @param change  banks to set
/// @param mask    banks of @p change that fade
/// @param target  fade color
/// @param coef    fade amount
void apply_fade(uint32_t change, uint32_t mask, hal::video::Color target, uint8_t coef)
{
   uint32_t touched = 0;
   for (int b = 0; b < kAllBanks; ++b)
   {
      const uint32_t bit = uint32_t{1} << b;
      if ((change & bit) == 0)
      {
         continue;
      }
      const uint8_t c = (mask & bit) != 0 ? coef : 0;
      const Color tg = (mask & bit) != 0 ? target : g_target[b];
      if (c != g_coef[b] || (c != 0 && tg != g_target[b]))
      {
         touched |= bit;
      }
      g_coef[b] = c;
      g_target[b] = tg;
   }
   for (int bank = 0; bank < kBanks; ++bank)
   {   // adjacent banks merge into one queue request
      if (touched & bg_bank(bank))
      {
         show_bg(bank * kBankColors, kBankColors);
      }
   }
   for (int bank = 0; bank < kBanks; ++bank)
   {
      if (touched & obj_bank(bank))
      {
         show_obj(bank * kBankColors, kBankColors);
      }
   }
}

constexpr uint32_t kEveryBank = 0xFFFFFFFFu;   ///< Every BG and OBJ bank.

}   // namespace

void set_fade(uint32_t mask, hal::video::Color target, uint8_t coef)
{
   ENGINE_ASSERT(coef <= kFadeFull);
   apply_fade(kEveryBank, mask, target, coef);   // banks outside the mask show their base colors
}

void set_bank_fade(uint32_t mask, hal::video::Color target, uint8_t coef)
{
   ENGINE_ASSERT(coef <= kFadeFull);
   apply_fade(mask, mask, target, coef);
}

uint8_t bank_coef(int bank_bit)
{
   ENGINE_ASSERT(bank_bit >= 0 && bank_bit < kAllBanks);
   return g_coef[bank_bit];
}

void set_rounding(Rounding r)
{
   if (r == g_rounding)
   {
      return;
   }
   g_rounding = r;
   show_bg(0, kColors);
   show_obj(0, kColors);
}

Rounding rounding()
{
   return g_rounding;
}

void save(Snapshot& out)
{
   for (int i = 0; i < kColors; ++i)
   {
      out.bg[i] = g_base_bg[i];
      out.obj[i] = g_base_obj[i];
   }
}

void restore(const Snapshot& in)
{
   load_bg(0, in.bg, kColors);
   load_obj(0, in.obj, kColors);
}

hal::video::Color base(bool obj, int index)
{
   ENGINE_ASSERT(index >= 0 && index < kColors);
   return obj ? g_base_obj[index] : g_base_bg[index];
}

} // namespace capsule::gfx::palette
