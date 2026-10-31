/// @file
/// The encounter plan of the hook running (F1): the Op::Encounter primitive's side of the lead rules (see
/// capsule/battle/encounter_lead.h). A battle module of its own, so every program with the effect host has the
/// primitive while only the engine library's programs (and tests that list it) link the rolls themselves
/// (src/battle/encounter_lead.cpp, which needs the encounter module).
#include "capsule/battle/encounter_lead.h"

#include "internal.h"

namespace capsule::battle
{

using namespace detail;

namespace
{

constexpr int32_t kMaxU8 = 0xFF;   ///< Highest 8-bit operand (odds, levels, level shifts).
constexpr int kOperandA = 0;       ///< Action::args index of the Encounter op's first operand.
constexpr int kOperandB = 1;       ///< Its second.
constexpr int kOperandC = 2;       ///< Its third.

EncounterPlan* g_plan = nullptr;                      ///< The plan of the encounter hook running, or nullptr.
effect::Hook g_plan_hook = effect::Hook::Encounter;   ///< The hook running (encounter_reroll: EncounterMade only).

} // namespace

namespace detail
{

/// Makes a plan the one the Encounter op changes (encounter_lead.cpp's scope around a hook's run).
/// @param plan  the plan, or nullptr when no encounter hook runs
/// @param hook  the hook running (encounter_reroll needs EncounterMade)
void set_encounter_plan(EncounterPlan* plan, effect::Hook hook)
{
   g_plan = plan;
   g_plan_hook = hook;
}

} // namespace detail

effect::Outcome encounter_primitive(const effect::Action& a)
{
   constexpr effect::Outcome kFail{0, false, false, true};
   if (g_plan == nullptr)
   {
      return kFail;   // not in an encounter hook's run
   }
   EncounterPlan& p = *g_plan;
   const int32_t x = a.args[kOperandA];
   const int32_t y = a.args[kOperandB];
   const int32_t z = a.args[kOperandC];
   // every operand is clamped to the range the compiler checks (an unknown range reaches here unchecked)
   switch (static_cast<effect::EncounterOp>(a.id))
   {
   case effect::EncounterOp::Rate:
   {
      const int32_t n = clamp_to<int32_t>(x, 0, kMaxU8);
      const int32_t d = clamp_to<int32_t>(y, 1, kMaxU8);
      p.rate_scale = clamp_to<int32_t>(p.rate_scale * n / d, 0, kMaxRateScale);
      break;
   }
   case effect::EncounterOp::Levels:
      p.lo_add = static_cast<int16_t>(clamp_to<int32_t>(p.lo_add + x, -kMaxU8, kMaxU8));
      p.hi_add = static_cast<int16_t>(clamp_to<int32_t>(p.hi_add + y, -kMaxU8, kMaxU8));
      break;
   case effect::EncounterOp::LevelBias:
      p.level_bias = static_cast<uint8_t>(clamp_to<int32_t>(x, 0, kMaxU8));
      break;
   case effect::EncounterOp::SkipBelow:
      p.skip_below = static_cast<uint8_t>(clamp_to<int32_t>(x, 0, kMaxU8));
      break;
   case effect::EncounterOp::PreferType:
      p.bias = EncounterBias::Type;
      p.bias_id = a.flags;
      p.bias_n = static_cast<uint8_t>(clamp_to<int32_t>(x, 0, kMaxU8));
      p.bias_d = static_cast<uint8_t>(clamp_to<int32_t>(y, 1, kMaxU8));
      break;
   case effect::EncounterOp::PreferField:
      p.bias = EncounterBias::Field;
      p.bias_id = a.flags;
      p.bias_value = static_cast<uint16_t>(clamp_to<int32_t>(x, 0, kMaxU16));
      p.bias_n = static_cast<uint8_t>(clamp_to<int32_t>(y, 0, kMaxU8));
      p.bias_d = static_cast<uint8_t>(clamp_to<int32_t>(z, 1, kMaxU8));
      break;
   case effect::EncounterOp::Reroll:
      if (g_plan_hook != effect::Hook::EncounterMade)
      {
         return kFail;
      }
      p.reroll = 1;
      break;
   default:
      return kFail;
   }
   return effect::Outcome{0, false, false, false};
}

} // namespace capsule::battle
