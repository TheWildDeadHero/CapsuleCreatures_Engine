/// @file
/// Step effects over the script bank's implicit variables (see capsule/script/step_effects.h).
#include "capsule/script/step_effects.h"

#include "capsule/script/script.h"

namespace capsule::script::step_effects
{
namespace
{

/// Bits of the due mask.
constexpr int kDueBits = 16;
static_assert(kMaxStepEffects <= kDueBits, "g_due holds a bit per step effect");

/// Bit n: effect n is due.
uint16_t g_due = 0;
/// B2: steps walked() counted that take_walked() did not claim yet.
uint32_t g_walked = 0;

/// The bit of one effect.
/// @param effect  index (below kMaxStepEffects)
/// @return its bit
uint16_t bit_of(uint8_t effect)
{
   return static_cast<uint16_t>(1u << effect);
}

} // namespace

uint8_t count()
{
   return kBank.step_effect_count < kMaxStepEffects ? kBank.step_effect_count : static_cast<uint8_t>(kMaxStepEffects);
}

const StepEffectInfo* info(uint8_t effect)
{
   return effect < count() ? &kBank.step_effects[effect] : nullptr;
}

int32_t counted(uint8_t effect)
{
   const StepEffectInfo* e = info(effect);
   return e != nullptr ? var(VarId{e->var}) : 0;
}

int32_t steps_left(uint8_t effect)
{
   const StepEffectInfo* e = info(effect);
   return e != nullptr ? static_cast<int32_t>(e->every) - counted(effect) : 0;
}

void walked(uint32_t steps)
{
   if (steps == 0)
   {
      return;
   }
   g_walked = g_walked + steps >= g_walked ? g_walked + steps : g_walked;   // no wrap
   for (uint8_t i = 0; i < count(); ++i)
   {
      const StepEffectInfo& e = kBank.step_effects[i];
      const uint32_t every = e.every != 0 ? e.every : 1u;
      // what was counted plus the new steps; past `every` the effect runs and the rest counts on (several runs'
      // worth in one call run it once: the walker makes a step or two per frame)
      const uint32_t total = static_cast<uint32_t>(var(VarId{e.var})) + steps;
      if (total >= every)
      {
         g_due = static_cast<uint16_t>(g_due | bit_of(i));
      }
      set_var(VarId{e.var}, static_cast<int32_t>(total % every));
   }
}

bool take_due(uint8_t* effect)
{
   for (uint8_t i = 0; i < count(); ++i)
   {
      if ((g_due & bit_of(i)) != 0)
      {
         g_due = static_cast<uint16_t>(g_due & ~bit_of(i));
         *effect = i;
         return true;
      }
   }
   return false;
}

bool due(uint8_t effect)
{
   return effect < count() && (g_due & bit_of(effect)) != 0;
}

void clear_due()
{
   g_due = 0;
}

void set_outcome(uint8_t member, uint8_t outcome)
{
   if (kBank.step_member_var != kNoVar)
   {
      set_var(VarId{kBank.step_member_var}, member);
   }
   if (kBank.step_outcome_var != kNoVar)
   {
      set_var(VarId{kBank.step_outcome_var}, outcome);
   }
}

uint32_t take_walked()
{
   const uint32_t n = g_walked;
   g_walked = 0;
   return n;
}

uint16_t hatch_script()
{
   return kBank.hatch_script;
}

void set_egg_member(uint8_t member)
{
   if (kBank.egg_member_var != kNoVar)
   {
      set_var(VarId{kBank.egg_member_var}, member);
   }
}

} // namespace capsule::script::step_effects
