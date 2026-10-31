/// @file
/// The engine's default battle data: no types, moves, creatures or formats,
/// no record roles, no generation list, and placeholder formulas and pipelines. A build with battle data links
/// its generated table (tools/battle_compiler.py) instead, which defines
/// every symbol in this file and so keeps it out of the link.
#include "capsule/battle/data.h"
#include "capsule/battle/data/field.h"
#include "capsule/battle/pipeline.h"
#include "capsule/battle/ruleset.h"
#include "capsule/monster/monster.h"

namespace capsule::battle
{

const BattleData kData = {};

/// No recruit rules: no checks, so the `recruit` primitive fails (G3).
const RecruitData kRecruit = {nullptr, 0, kNone8, 0, 0, kNone8, 0,
                              {kNone8, kNone8, kNone8, kNone8, kNone8, kNone8, kNone8, kNone8},
                              {kNone8, kNone8, kNone8, kNone8, kNone8, kNone8, kNone8, kNone8}};

/// No lead rule (the first member that can battle leads) and no timed field effects (F1).
const FieldRules kFieldRules = {kNoLeadRule, 0, 0, kNoActionRule, 0, 0, kNoLeadRule, 0};

} // namespace capsule::battle

namespace capsule::monster
{

namespace
{
/// No roles held.
constexpr uint16_t kNoOffsets[static_cast<int>(Role::NUM_ROLES)] = {
   kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent,
   kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent};
static_assert(sizeof(kNoOffsets) / sizeof(kNoOffsets[0]) == static_cast<int>(Role::NUM_ROLES));
} // namespace

const Layout kLayout = {kNoOffsets, nullptr, nullptr, nullptr, 0, 0, 0, 0, 0, 0, 0, {0}, 0};

} // namespace capsule::monster

namespace capsule::rules
{

namespace
{
/// @name Output ranges of the placeholder formulas (neutral: the widths the engine stores them in, D-22 decision 8)
///@{
constexpr int32_t kStatMax = 0xFFFF;            ///< Highest stat (16 bits).
constexpr int32_t kPowerMax = 0xFFFF;           ///< Highest damage (16 bits).
constexpr int32_t kExpMax = (1 << 24) - 1;      ///< Most experience per award (the 24-bit experience role).
///@}
} // namespace

int32_t stat(int32_t, int32_t base, int32_t, int32_t, int32_t, int32_t level, int32_t)
{
   return clamp32(base + level, 1, kStatMax);
}
int32_t exp_gain(int32_t yield, int32_t, int32_t, int32_t, int32_t, int32_t)
{
   return clamp32(yield, 0, kExpMax);
}
int32_t reward(int32_t, int32_t)
{
   return 0;   // no reward
}
int32_t escape_odds(int32_t, int32_t, int32_t)
{
   return kEscapeOutcomes;   // always escapes
}
int32_t resource_formula(int32_t, int32_t a, int32_t)
{
   return a;   // no resource formulas: a move resource's maximum is its base
}
int32_t derive(int32_t, int32_t, const monster::Record&)
{
   return 0;   // no derived roles
}
int32_t species_field_of(int32_t, int32_t)
{
   return 0;   // no species fields
}
int32_t damage(battle::PipelineCtx&, const battle::DamageInputs& in)
{
   return clamp32(in.move_power, 0, kPowerMax);   // the move's power
}
int32_t self_hit(battle::PipelineCtx&, const battle::SelfHitInputs& in)
{
   return clamp32(in.move_power, 0, kPowerMax);
}
int32_t speed(battle::PipelineCtx&, const battle::SpeedInputs& in)
{
   return clamp32(in.speed_raw, 1, kStatMax);   // the stat, no stage
}
battle::ChanceResult accuracy(battle::PipelineCtx&, const battle::AccuracyInputs&)
{
   return battle::ChanceResult{1, 1, 1};   // always hits
}
battle::ChanceResult crit(battle::PipelineCtx&, const battle::CritInputs&)
{
   return battle::ChanceResult{0, 0, 0};   // never critical
}
battle::ChanceResult effect_odds(battle::PipelineCtx&, const battle::EffectOddsInputs& in)
{
   return battle::ChanceResult{1, in.effect_chance, in.effect_out_of};   // always succeeds
}
int32_t ai_score(battle::PipelineCtx&, const battle::AiScoreInputs&)
{
   return 0;   // adds nothing
}
battle::ChanceResult recruit_odds(battle::PipelineCtx&, const battle::RecruitOddsInputs&)
{
   return battle::ChanceResult{0, 0, 1};   // never recruits
}
battle::ChanceResult encounter_rate(battle::PipelineCtx&, const battle::EncounterRateInputs& in)
{
   // F1: the table's odds times the hooks' Q8 scale (its fraction bits kept while rate_m leaves room for them)
   constexpr int32_t kRoom = int32_t{1} << kQ8Shift;
   if (in.rate_m <= kRoom)
   {
      return battle::ChanceResult{0, in.rate_n * in.rate_scale, in.rate_m << kQ8Shift};
   }
   return battle::ChanceResult{0, (in.rate_n * in.rate_scale) >> kQ8Shift, in.rate_m};
}
int32_t encounter_level(battle::PipelineCtx& ctx, const battle::EncounterLevelInputs& in)
{
   // F1: a level between the entry's lowest and highest, drawn as rng::between() draws it
   return in.min_level + battle::pipeline_roll(ctx, in.max_level - in.min_level + 1);
}
int32_t combine_odds(battle::PipelineCtx&, const battle::CombineOddsInputs& in)
{
   // B1: the method's own value (the default pipeline's min(value, 65535))
   constexpr int32_t kTop = 0xFFFF;
   return in.value < kTop ? in.value : kTop;
}
int32_t combine_fee(battle::PipelineCtx&, const battle::CombineFeeInputs& in)
{
   return in.value;   // B1: the method's own value
}
int32_t nursery_gain(battle::PipelineCtx&, const battle::NurseryGainInputs&)
{
   return 1;   // B2: one experience point per step
}
int32_t nursery_fee(battle::PipelineCtx&, const battle::NurseryFeeInputs&)
{
   return 0;   // B2: no fee
}
int32_t stage_formula(int32_t, int32_t v, int32_t)
{
   return v;   // no formula stages
}
int32_t point_handler(uint16_t, battle::effect::PointCtx&, int32_t v)
{
   return v;   // no handlers
}

} // namespace capsule::rules
