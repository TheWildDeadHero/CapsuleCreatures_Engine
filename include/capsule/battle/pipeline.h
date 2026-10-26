/// @file
/// Pipelines and modifier points (D-20 D, D-23 section 1.1, P-14).
///
/// The arithmetic of a hit (damage, a confused battler's self-hit, turn-order
/// speed, accuracy and critical hits) is not engine code: a game's ruleset
/// (`capsule.ruleset/2`, tools/ruleset_compiler.py) declares each as a
/// pipeline, a list of steps (let a value, run a modifier point on it, draw a
/// roll), and the battle compiler emits each as straight-line C++ in the
/// build's battle data: rules::damage(), self_hit(), speed(), accuracy(),
/// crit(), effect_odds() and ai_score() below (effect_odds and ai_score are optional in a ruleset: without them
/// the compiler emits plain "chance in out_of" odds and a score of 0; G1). The engine gathers the inputs (the structs below, a fixed
/// contract the compiler knows the ranges of) and uses the outputs.
///
/// **Points.** A point step hands its value to battle::run_point(), which runs
/// the handlers of the point's sources (data/points.h) in their declared
/// order: each is pure effect code (`point NAME [when ...] { modify by ...; }`,
/// tools/effect_compiler.py) compiled to one generated function,
/// rules::point_handler(). Handlers read the battle through effect::point_get()
/// and point_has() (implemented by the engine) and never write, draw or emit,
/// so pipelines run the same for real hits, the AI's estimates and previews.
///
/// **Rolls.** A roll step calls battle::pipeline_roll(): a draw from the
/// battle's stream at exactly that position, a draw made earlier
/// (PipelineCtx::draw, the AI's roll of the turn) or a fixed roll
/// (PipelineCtx::fixed_roll, previews; PipelineCtx::rng is nullptr there).
///
/// The engine library carries defaults (battle_empty.cpp) that a build's
/// generated functions replace at link time.
#pragma once

#include "capsule/battle/effect.h"
#include "capsule/core/pod.h"
#include "capsule/core/random.h"

#include <cstdint>

namespace capsule::battle
{

struct State;

/// @name Pipeline inputs (ruleset_compiler.PIPELINES lists the same fields)
///@{

/// Inputs of rules::damage().
struct DamageInputs
{
   int32_t level;           ///< The user's level.
   int32_t move_power;      ///< The move's power.
   int32_t offense_raw;     ///< The user's offense stat (the move category's), no stage.
   int32_t offense_stage;   ///< Its stage (0 for a stat without one).
   int32_t defense_raw;     ///< The target's defense stat, no stage.
   int32_t defense_stage;   ///< Its stage.
   int32_t crit;            ///< 1 for a critical hit.
   int32_t spread;          ///< 1 when the move hits more than one target.
   int32_t same_type;       ///< 1 when the user has the move's type.
   int32_t type_exp;        ///< Type chart exponent sum of the scaling tiers (combine Exponent; else 0).
   int32_t offense_stage_id;   ///< The offense stat's stage (BattleData::stages index), or kNone8.
   int32_t defense_stage_id;   ///< The defense stat's stage, or kNone8.
};

/// Inputs of rules::self_hit() (a battler hitting itself, e.g. confused: the self-hit category's stats).
struct SelfHitInputs
{
   int32_t level;           ///< The battler's level.
   int32_t move_power;      ///< The self-hit's power (effect `self_hit POWER`; previews: Rules::self_hit_power).
   int32_t offense_raw;     ///< Its offense stat, no stage.
   int32_t offense_stage;   ///< Its stage.
   int32_t defense_raw;     ///< Its own defense stat, no stage.
   int32_t defense_stage;   ///< Its stage.
   int32_t offense_stage_id;   ///< The offense stat's stage (BattleData::stages index), or kNone8.
   int32_t defense_stage_id;   ///< The defense stat's stage, or kNone8.
};

/// Inputs of rules::speed() (turn order, escape).
struct SpeedInputs
{
   int32_t speed_raw;       ///< The order stat, no stage.
   int32_t speed_stage;     ///< Its stage.
   int32_t speed_stage_id;  ///< Its stage's BattleData::stages index, or kNone8.
};

/// Inputs of rules::accuracy() (moves that can miss).
struct AccuracyInputs
{
   int32_t move_accuracy;   ///< The move's accuracy (percent).
   int32_t accuracy_stage;  ///< The user's accuracy stage.
   int32_t evasion_stage;   ///< The target's evasion stage.
};

/// Inputs of rules::crit().
struct CritInputs
{
   int32_t crit_stage;        ///< The user's critical-hit stage.
   int32_t move_crit_stage;   ///< The move's added stage.
};

/// Inputs of rules::effect_odds(): the odds of a secondary effect or a status succeeding (effect logic
/// `odds(chance, out_of[, by, on])`, E9), with the odds stat (BattleData::odds_stat) of both battlers.
struct EffectOddsInputs
{
   int32_t effect_chance;   ///< Successes the logic asks for (>= 1).
   int32_t effect_out_of;   ///< ...in this many (>= 1).
   int32_t user_stat;       ///< The odds stat of the battler causing it, no stage (0 without an odds stat).
   int32_t target_stat;     ///< The odds stat of the battler it lands on, no stage (0 without an odds stat).
   int32_t user_level;      ///< The causing battler's level.
   int32_t target_level;    ///< The other battler's level.
};

/// Inputs of rules::ai_score() (G1): an AI profile's Pipeline rule scores one option (a move aimed at a target)
/// from the score its earlier rules gave, the AI's damage estimate and both battlers.
struct AiScoreInputs
{
   int32_t so_far;           ///< The score the earlier rules gave (clamped to 16 bits).
   int32_t damage;           ///< Expected damage to the foes it hits (no crit, the turn's roll; up to 65535).
   int32_t ko;               ///< 1 when it would knock a foe out.
   int32_t damaging;         ///< 1 for a damaging move.
   int32_t move_accuracy;    ///< Its accuracy percent (0: never misses).
   int32_t move_statuses;    ///< Statuses it inflicts (MoveData::statuses).
   int32_t user_hp;          ///< The user's HP.
   int32_t user_max_hp;      ///< Its maximum.
   int32_t target_hp;        ///< The target's HP (the user's own for a move that hits nobody else).
   int32_t target_max_hp;    ///< Its maximum.
   int32_t turn;             ///< The turn number.
};

/// Inputs of rules::recruit_odds() (G3, D-26 section 3): the odds of one recruit check (effect `recruit`; the
/// structure's recruit.checks draws, each `chance` in `out_of`, all of which must pass; `sure`: no draw).
struct RecruitOddsInputs
{
   int32_t target_hp;        ///< The battler's HP.
   int32_t target_max_hp;    ///< Its maximum.
   int32_t recruit_rate;     ///< Its species' recruit rate (the species field structure recruit.rate names; 0 without).
   int32_t bonus;            ///< The bonus the item's logic gives (`recruit WHO BONUS`, 0-255).
   int32_t status_bonus;     ///< The bonus of its primary status (structure recruit.status_bonus).
   int32_t target_level;     ///< Its level.
   int32_t user_level;       ///< The level of the battler using the item.
   int32_t turn;             ///< The turn number.
};

/// Inputs of rules::encounter_rate() (F1, D-27 section 1): the trigger odds of one encounter roll (per step of a
/// step method, per use of a script method), after the lead's `encounter` hooks (capsule/battle/encounter_lead.h).
struct EncounterRateInputs
{
   int32_t rate_n;       ///< The table's odds: rate_n...
   int32_t rate_m;       ///< ...in rate_m.
   int32_t rate_scale;   ///< The hooks' scale of the odds in Q8 (256 = x1; effect `encounter_rate N D` multiplies).
   int32_t lead_level;   ///< The lead's level (0 without a lead).
   int32_t method;       ///< The method rolling (the world's encounter_methods index).
};

/// Inputs of rules::encounter_level() (F1): the level of the creature an encounter picked.
struct EncounterLevelInputs
{
   int32_t min_level;    ///< The entry's lowest level (after the hooks' `encounter_levels`, at least 1).
   int32_t max_level;    ///< Its highest (at least min_level).
   int32_t lead_level;   ///< The lead's level (0 without a lead).
   int32_t level_bias;   ///< What the hooks' `encounter_level_bias N` asked for (0: nothing).
   int32_t method;       ///< The method rolling.
};

/// Inputs of rules::combine_odds() (B1, D-28 section 1): a combination method's odds over its inputs (0: they are
/// incompatible; capsule/combine/combine.h).
struct CombineOddsInputs
{
   int32_t method;         ///< The method (structure combine_methods index).
   int32_t count;          ///< Its inputs.
   int32_t level_min;      ///< Their lowest level.
   int32_t level_max;      ///< Their highest level.
   int32_t level_avg;      ///< Their average level (rounded down).
   int32_t same_species;   ///< 1 when every input is one species.
   int32_t value;          ///< The method's odds expression over the inputs (clamped into the pipeline's range).
};

/// Inputs of rules::combine_fee() (B1): what a combination costs (the script reads it and charges).
struct CombineFeeInputs
{
   int32_t method;         ///< The method.
   int32_t count;          ///< Its inputs.
   int32_t level_min;      ///< Their lowest level.
   int32_t level_max;      ///< Their highest level.
   int32_t level_avg;      ///< Their average level (rounded down).
   int32_t result_level;   ///< The result's level.
   int32_t value;          ///< The method's fee expression (clamped into the pipeline's range).
};

/// Inputs of rules::nursery_gain() (B2, D-28 section 2): the experience a record left in the nursery gains per step.
struct NurseryGainInputs
{
   int32_t level;   ///< Its level when it was left there.
   int32_t slot;    ///< Its nursery slot.
};

/// Inputs of rules::nursery_fee() (B2): what taking a record back from the nursery costs.
struct NurseryFeeInputs
{
   int32_t levels_gained;   ///< The levels it gained there.
   int32_t level;           ///< Its level now.
   int32_t count;           ///< The records the nursery holds.
};

/// Result of a chance pipeline (accuracy, crit): the outcome and the odds it was drawn with.
struct ChanceResult
{
   int32_t result;   ///< 1: hit / critical hit.
   int32_t chance;   ///< Successes...
   int32_t out_of;   ///< ...in this many (previews show them; out_of 0 = never).
};
///@}

/// PipelineCtx::fixed_roll: no fixed roll.
inline constexpr int16_t kNoFixedRoll = -1;
/// PipelineCtx::draw: no draw made earlier.
inline constexpr int32_t kNoDraw = -1;

/// What a pipeline runs for (rolls, and the battle point handlers read).
struct PipelineCtx
{
   const State* s;       ///< The battle.
   rng::Stream* rng;     ///< Stream rolls draw from, or nullptr (previews: rolls are fixed).
   uint8_t user;         ///< Acting battler.
   uint8_t target;       ///< Other battler, or kBench.
   uint16_t move;        ///< Move, or kNone16.
   int16_t fixed_roll;   ///< Every roll takes this value (clamped below its span), or kNoFixedRoll.
   uint8_t crit;         ///< 1 when the hit is critical (point handlers read `crit`).
   uint8_t move_type;    ///< The type the hit has (the move_type point's result; point handlers read it as
                         ///< `move.type`), or kNone8: the move's own (E13b).
   uint8_t move_category;   ///< The category the hit has (hit_category_by_type; `move.category`), or kNone8: the
                            ///< move's own (C5).
   int32_t draw;         ///< A 16-bit draw made earlier that every roll scales to its span, or kNoDraw.
};

/// Runs a modifier point (data/points.h) on a value: the handlers of its sources in order (each sees the value
/// clamped to [0, 65535]), then the point's range clamp. Returns @p v at once when no source has a handler.
/// @param c      the pipeline
/// @param point  point id (BattleData::points)
/// @param v      the value
/// @return the modified value
int32_t run_point(PipelineCtx& c, uint8_t point, int32_t v);

/// A roll in [0, @p span): the fixed roll, or the earlier draw scaled, or a draw now (rng::below()).
/// @param c     the pipeline
/// @param span  outcomes (<= 0: no draw, 0)
/// @return the roll
int32_t pipeline_roll(PipelineCtx& c, int32_t span);

/// @name Pipeline helpers (the battle's tables; ruleset_compiler.HELPERS)
///@{

/// A value scaled by a stage at a stage value (the value clamped to the stage's range): its table's multiplier,
/// its ruleset formula, or x1 (no table, or @p id names no stage).
/// @param id     stage (BattleData::stages index; the inputs' *_stage_id)
/// @param v      value
/// @param stage  stage value
/// @return v * num / den, or the formula's value
int32_t pipeline_stage_scale(int32_t id, int32_t v, int32_t stage);
/// A value scaled by the table every stat stage shares (BattleData::stat_stage; the compiler refuses staged()
/// when the stat stages differ).
/// @param v      value
/// @param stage  stage value
/// @return the scaled value
int32_t pipeline_staged(int32_t v, int32_t stage);
/// A value scaled by the accuracy stage (BattleData::accuracy_stage).
/// @param v      value
/// @param stage  stage value (accuracy minus evasion)
/// @return the scaled value
int32_t pipeline_accuracy_staged(int32_t v, int32_t stage);
/// Successes of the crit stage's odds at a crit stage value (clamped to its range).
/// @param stage  crit stage value
/// @return n
int32_t pipeline_crit_chance(int32_t stage);
/// Outcomes of the crit stage's odds.
/// @param stage  crit stage value
/// @return m
int32_t pipeline_crit_out_of(int32_t stage);
/// A value scaled by the type chart for the pipeline's move against its target, by BattleData::combine:
/// Exponent: pow2(v, exponent sum); Multiply: v times the combined factor; Sequential: v times each scaling
/// tier's factor in turn (truncating each step). Decisive tiers never reach a pipeline (calc_damage() resolves
/// them first); x1 without a move or a target.
/// @param c  the pipeline (move, target)
/// @param v  value (>= 0)
/// @return the scaled value
int32_t pipeline_type_scale(const PipelineCtx& c, int32_t v);
///@}

} // namespace capsule::battle

namespace capsule::battle::effect
{

/// What a generated point handler reads (effect.h leaves its definition to the embedder).
struct PointCtx
{
   PipelineCtx* pipe;    ///< The pipeline (user, target, move, crit).
   uint8_t holder;       ///< The handler's holder: the battler whose passive, item or status it is (the user
                         ///< for weathers).
   uint8_t other;        ///< The pipeline's other battler (the target when the holder is the user), or kBench.
   uint8_t reserved[kPadAfterTwoBytes];   ///< Padding; 0.
};

} // namespace capsule::battle::effect

namespace capsule::rules
{

/// @name Pipelines (generated per game from the ruleset)
///@{

/// Damage of a hit (after the decisive type tiers).
/// @param ctx  the pipeline
/// @param in   inputs
/// @return HP the hit takes
int32_t damage(battle::PipelineCtx& ctx, const battle::DamageInputs& in);
/// Damage of a confused battler's self-hit.
/// @param ctx  the pipeline (user = target = the battler)
/// @param in   inputs
/// @return HP it takes
int32_t self_hit(battle::PipelineCtx& ctx, const battle::SelfHitInputs& in);
/// Speed for turn order and escape.
/// @param ctx  the pipeline (target kBench)
/// @param in   inputs
/// @return the speed (>= 1)
int32_t speed(battle::PipelineCtx& ctx, const battle::SpeedInputs& in);
/// Whether a move that can miss hits.
/// @param ctx  the pipeline
/// @param in   inputs
/// @return the outcome and its odds
battle::ChanceResult accuracy(battle::PipelineCtx& ctx, const battle::AccuracyInputs& in);
/// Whether a hit is critical.
/// @param ctx  the pipeline
/// @param in   inputs
/// @return the outcome and its odds
battle::ChanceResult crit(battle::PipelineCtx& ctx, const battle::CritInputs& in);
/// Whether a secondary effect or a status succeeds (effect logic `odds()`; a ruleset without an effect_odds
/// pipeline gets the plain odds effect_chance in effect_out_of, drawn like rng::chance()).
/// @param ctx  the pipeline (user = the causing battler, target = the other, move = the context move)
/// @param in   inputs
/// @return the outcome and its odds
battle::ChanceResult effect_odds(battle::PipelineCtx& ctx, const battle::EffectOddsInputs& in);
/// What an AI profile's Pipeline rule adds to an option's score (G1; a ruleset without an ai_score pipeline gets
/// 0). Pure: it runs without a stream (rng nullptr: its rolls are fixed).
/// @param ctx  the pipeline (user = the AI's battler, target = the option's target, move = its move)
/// @param in   inputs
/// @return the score it adds
int32_t ai_score(battle::PipelineCtx& ctx, const battle::AiScoreInputs& in);
/// The odds of one recruit check (G3; a ruleset without a recruit_odds pipeline never recruits): result = sure (it
/// succeeds without a draw), chance in out_of (out_of up to 65536).
/// @param ctx  the pipeline (user = the item's user, target = the battler to recruit)
/// @param in   inputs
/// @return sure and the odds
battle::ChanceResult recruit_odds(battle::PipelineCtx& ctx, const battle::RecruitOddsInputs& in);
/// The trigger odds of one encounter roll (F1; a ruleset without an encounter_rate pipeline gets the table's odds
/// times rate_scale, rate_n * rate_scale in rate_m * 256 while rate_m <= 256): result = sure (it starts without a
/// draw), else chance in out_of (out_of up to 65536, rng::chance()'s span). Pure
/// arithmetic: no point steps (ruleset_compiler refuses them in the field pipelines).
/// @param ctx  the pipeline (user = the lead in a field context, or kBench)
/// @param in   inputs
/// @return sure and the odds
battle::ChanceResult encounter_rate(battle::PipelineCtx& ctx, const battle::EncounterRateInputs& in);
/// The level of an encounter's creature (F1; a ruleset without an encounter_level pipeline draws one between the
/// lowest and highest level, as the tables do on their own). The engine clamps it to 1..level_max.
/// @param ctx  the pipeline (rolls draw from the encounter's stream)
/// @param in   inputs
/// @return the level
int32_t encounter_level(battle::PipelineCtx& ctx, const battle::EncounterLevelInputs& in);
/// A combination method's odds (B1; a ruleset without a combine_odds pipeline gets the method's own value, at most
/// 65535). Pure arithmetic (no point steps).
/// @param ctx  the pipeline (no battlers)
/// @param in   inputs
/// @return the odds (0: incompatible)
int32_t combine_odds(battle::PipelineCtx& ctx, const battle::CombineOddsInputs& in);
/// A combination's fee (B1; a ruleset without a combine_fee pipeline gets the method's own value).
/// @param ctx  the pipeline (no battlers)
/// @param in   inputs
/// @return the fee
int32_t combine_fee(battle::PipelineCtx& ctx, const battle::CombineFeeInputs& in);
/// The experience per step of a record left in the nursery (B2; a ruleset without a nursery_gain pipeline gives 1).
/// Pure arithmetic.
/// @param ctx  the pipeline (no battlers)
/// @param in   inputs
/// @return experience per step
int32_t nursery_gain(battle::PipelineCtx& ctx, const battle::NurseryGainInputs& in);
/// What taking a record back from the nursery costs (B2; a ruleset without a nursery_fee pipeline: nothing).
/// @param ctx  the pipeline (no battlers)
/// @param in   inputs
/// @return the fee
int32_t nursery_fee(battle::PipelineCtx& ctx, const battle::NurseryFeeInputs& in);
/// A value scaled by a formula stage (StageData::formula; the ruleset's named formula of two arguments: the value
/// and the stage value, already clamped to the stage's range).
/// @param formula  StageData::formula
/// @param v        value
/// @param stage    stage value
/// @return the formula's value
int32_t stage_formula(int32_t formula, int32_t v, int32_t stage);
/// Runs point handler @p fn (Handler::first of a point handler) on a value.
/// @param fn  the handler's function
/// @param c   what it reads
/// @param v   the value
/// @return the modified value
int32_t point_handler(uint16_t fn, battle::effect::PointCtx& c, int32_t v);
///@}

} // namespace capsule::rules
