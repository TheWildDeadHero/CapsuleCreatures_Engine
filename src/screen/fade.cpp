/// @file
/// Palette fades as data (see capsule/screen/fade.h).
#include "capsule/screen/fade.h"

#include "capsule/core/assert.h"

namespace capsule::screen::fade
{
namespace
{

constexpr uint32_t kBgBanks = 0x0000FFFFu;    ///< The BG half of a mask.
constexpr uint32_t kObjBanks = 0xFFFF0000u;   ///< The OBJ half.

/// The running fade.
struct Run
{
   Fade fade;            ///< What it does.
   FadeStyle style;      ///< How it steps.
   uint8_t y;            ///< Current blend amount.
   uint8_t step;         ///< y change per step.
   uint8_t delay;        ///< Calls waited before a step.
   uint8_t waited;       ///< Calls waited so far.
   uint8_t finished;     ///< Calls since the last step (finishing).
   bool active;          ///< A fade runs.
   bool obj_turn;        ///< The next call blends the OBJ half (alternate).
   bool finishing;       ///< y reached its target: the finishing calls run.
};
Run g{};   ///< The fade.

/// Blends a half of the mask at y.
/// @param half  kBgBanks, kObjBanks, or both
void blend(uint32_t half)
{
   gfx::palette::set_bank_fade(g.fade.mask & half, g.fade.color, g.y);
}

/// Moves y one step toward the target, or starts finishing when it is there.
void advance()
{
   if (g.y == g.fade.to)
   {
      g.finishing = true;
      g.finished = 0;
   }
   else if (g.y < g.fade.to)
   {
      g.y = static_cast<uint8_t>(g.y + g.step > g.fade.to ? g.fade.to : g.y + g.step);
   }
   else
   {
      g.y = static_cast<uint8_t>(g.y < g.fade.to + g.step ? g.fade.to : g.y - g.step);
   }
}

}   // namespace

bool begin(const Fade& f, const FadeStyle& style)
{
   if (g.active)
   {
      return false;
   }
   ENGINE_ASSERT(f.from <= kFull && f.to <= kFull);
   g = Run{};
   g.fade = f;
   g.style = style;
   g.y = f.from;
   g.step = style.step;
   if (f.delay < 0)
   {
      g.step = static_cast<uint8_t>(g.step - f.delay);   // a negative delay: a bigger step, no wait
   }
   else
   {
      g.delay = static_cast<uint8_t>(f.delay);
   }
   g.waited = g.delay;   // the first step does not wait
   g.active = true;
   gfx::palette::set_rounding(style.rounding);
   update();
   return true;
}

bool update()
{
   if (!g.active)
   {
      return false;
   }
   if (g.finishing)
   {
      if (g.finished == g.style.finish_calls)
      {
         g.active = false;
         g.finishing = false;
      }
      else
      {
         ++g.finished;
      }
      return g.active;
   }
   if (!g.obj_turn)
   {
      if (g.waited < g.delay)
      {
         ++g.waited;
         return true;
      }
      g.waited = 0;
   }
   if (!g.style.alternate)
   {
      blend(kBgBanks | kObjBanks);
      advance();
      return true;
   }
   blend(g.obj_turn ? kObjBanks : kBgBanks);
   g.obj_turn = !g.obj_turn;
   if (!g.obj_turn)
   {
      advance();   // both halves are blended at this y: the next step
   }
   return true;
}

bool active()
{
   return g.active;
}

void cancel()
{
   g.active = false;
   g.finishing = false;
}

uint8_t level()
{
   return g.y;
}

}   // namespace capsule::screen::fade
