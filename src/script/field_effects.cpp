/// @file
/// Timed field effects over the script bank's implicit variables (see capsule/script/field_effects.h).
#include "capsule/script/field_effects.h"

#include "capsule/core/clock.h"
#include "capsule/script/script.h"

namespace capsule::script::field_effects
{
namespace
{

uint32_t g_pending = 0;        ///< Bit n: effect n ended and was not reported yet.
uint32_t g_last_minute = 0;    ///< The minute clock's last reading.
bool g_clock_read = false;     ///< g_last_minute holds a reading.

/// The effect's variable.
/// @param fx  its record
/// @return its VarId
VarId var_of(const FieldEffectInfo& fx)
{
   return VarId{fx.var};
}

/// Takes time off every running effect of one unit; one that reaches 0 is pending.
/// @param unit    the unit
/// @param amount  steps or minutes gone by
void run_down(FieldEffectUnit unit, uint32_t amount)
{
   if (amount == 0)
   {
      return;
   }
   const Bank& b = kBank;
   for (uint8_t i = 0; i < b.field_effect_count && i < kMaxFieldEffects; ++i)
   {
      const FieldEffectInfo& fx = b.field_effects[i];
      const int32_t left = var(var_of(fx));
      if (fx.unit != unit || left <= 0)
      {
         continue;
      }
      // an amount past what is left ends it (never below 0)
      const int32_t after = amount >= static_cast<uint32_t>(left) ? 0 : left - static_cast<int32_t>(amount);
      set_var(var_of(fx), after);
      if (after == 0)
      {
         g_pending |= 1u << i;
      }
   }
}

} // namespace

uint8_t count()
{
   return kBank.field_effect_count < kMaxFieldEffects ? kBank.field_effect_count
                                                      : static_cast<uint8_t>(kMaxFieldEffects);
}

const FieldEffectInfo* info(uint8_t effect)
{
   return effect < count() ? &kBank.field_effects[effect] : nullptr;
}

bool start(uint8_t effect, int32_t amount)
{
   const FieldEffectInfo* fx = info(effect);
   if (fx == nullptr || amount < 1)
   {
      return false;
   }
   const VarId v = var_of(*fx);
   const int32_t left = var(v);
   if (left > 0 && fx->stack == FieldEffectStack::Refuse)
   {
      return false;
   }
   // Add saturates at the variable's maximum (set_var() clamps to its declared range)
   const int32_t limit = kBank.vars[fx->var].max;
   const bool adds = left > 0 && fx->stack == FieldEffectStack::Add;
   set_var(v, adds && amount > limit - left ? limit : (adds ? left + amount : amount));
   g_pending &= ~(1u << effect);   // started again before its expiry was reported: it did not end
   return true;
}

bool active(uint8_t effect)
{
   return remaining(effect) > 0;
}

int32_t remaining(uint8_t effect)
{
   const FieldEffectInfo* fx = info(effect);
   return fx != nullptr ? var(var_of(*fx)) : 0;
}

void stop(uint8_t effect)
{
   const FieldEffectInfo* fx = info(effect);
   if (fx != nullptr)
   {
      set_var(var_of(*fx), 0);
      g_pending &= ~(1u << effect);
   }
}

void walked(uint32_t steps)
{
   run_down(FieldEffectUnit::Steps, steps);
}

void tick()
{
   const uint32_t now = clock::minutes();
   if (!g_clock_read)
   {
      g_last_minute = now;
      g_clock_read = true;
      return;
   }
   // unsigned difference: a clock that wraps at 2^32 still counts forward
   const uint32_t gone = now - g_last_minute;
   g_last_minute = now;
   run_down(FieldEffectUnit::Minutes, gone);
}

void reset_clock()
{
   g_clock_read = false;
   g_pending = 0;   // C8: an expiry queued before a load or a new scene belongs to the old game
}

bool take_expired(uint8_t* effect)
{
   for (uint8_t i = 0; i < count(); ++i)
   {
      if (((g_pending >> i) & 1u) != 0)
      {
         g_pending &= ~(1u << i);
         *effect = i;
         return true;
      }
   }
   return false;
}

} // namespace capsule::script::field_effects
