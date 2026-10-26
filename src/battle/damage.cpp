/// @file
/// Damage (C-08): stages, the type chart's tiers and their combine modes (data/chart.h), and the calls of the
/// ruleset's pipelines (speed, critical hits, damage, accuracy, self-hits; capsule/battle/pipeline.h) with the
/// previews.
/// See capsule/battle/battle.h; the modules share internal.h.
#include "internal.h"

namespace capsule::battle::detail
{

/// @name Value ranges
///@{
constexpr int32_t kKoEffectiveness = 0x7FFF;  ///< effectiveness() of a type that knocks out at once.
///@}

/// Whether a move's category deals formula damage.
/// @param m  move
/// @return true if its category has CATEGORY_DAMAGING
bool damaging(const MoveData& m)
{
   return m.category < data().category_count && has_flag(data().categories[m.category].flags, CATEGORY_DAMAGING);
}

/// A battler's value of a stage.
/// @param b      battler
/// @param stage  stage index, or kNone8
/// @return the value, or 0 for kNone8 (no such stage)
int stage_at(const Battler& b, uint8_t stage)
{
   return stage < data().stage_count ? b.stages[stage] : 0;
}

/// A battler's current stage of a stat.
/// @param b     battler
/// @param stat  stat index
/// @return the stage, or 0 for a stat without one
int stage_value(const Battler& b, int stat)
{
   return stage_at(b, stage_of(static_cast<uint8_t>(stat)));
}

/// A pipeline context.
/// @param s       battle
/// @param rng     stream rolls draw from, or nullptr (previews: rolls take @p roll)
/// @param user    acting battler
/// @param target  other battler, or kBench
/// @param move    move, or kNone16
/// @param roll    fixed roll, or kNoFixedRoll
/// @return the context
PipelineCtx pipeline_ctx(const State& s, rng::Stream* rng, uint8_t user, uint8_t target, uint16_t move, int roll)
{
   // C5: the move in progress's hit type and category for this target (or for any pipeline of its user without a
   // target, the crit's) reach the point handlers; otherwise they read the move's own
   const MoveCursor& mc = s.mv;
   const bool aimed = move != kNone16 && mc.move == move && mc.user == user && mc.hit_type != kNone8 &&
                      (target == mc.hit_target || target == kBench);
   return PipelineCtx{&s, rng, user, target, move, static_cast<int16_t>(roll), 0, aimed ? mc.hit_type : kNone8,
                      aimed ? mc.hit_category : kNone8, kNoDraw};
}

/// The type a move hits with (E13b): its own, or what the ruleset's `move_type` point makes of it
/// (Rules::move_type_point; e.g. a move whose type follows the weather or the user's genes). A value that names no
/// type keeps the move's own. Point handlers are pure: no draw.
/// @param s       battle
/// @param user    the move's user
/// @param target  its target, or kBench
/// @param move    move index
/// @return type index (the move's data type when the ruleset has no such point)
uint8_t move_type_of(const State& s, uint8_t user, uint8_t target, uint16_t move)
{
   const uint8_t own = data().moves[move].type;
   const uint8_t point = data().rules.move_type_point;
   if (point >= data().point_count)
   {
      return own;
   }
   PipelineCtx c = pipeline_ctx(s, nullptr, user, target, move, 0);
   c.move_type = kNone8;   // its handlers read the move's own type and category
   c.move_category = kNone8;
   const int32_t t = run_point(c, point, own);
   return t >= 0 && t < data().type_count ? static_cast<uint8_t>(t) : own;
}

/// The category a move hits with when its hit type is @p type (C5): the structure's hit_category_by_type
/// (BattleData::hit_categories) for a damaging move whose hit type is not its own type, else the move's category.
/// @param move  move index
/// @param type  the hit type (move_type_of())
/// @return category index
uint8_t category_of(uint16_t move, uint8_t type)
{
   const MoveData& m = data().moves[move];
   if (data().hit_categories == nullptr || type == m.type || type >= data().type_count || !damaging(m))
   {
      return m.category;
   }
   return data().hit_categories[type];
}

/// Works out once the type and the category the move in progress hits a target with (C5: MoveCursor::hit_type;
/// each target's when its checks start, a field move's with kBench once it has its targets; before that the use
/// and target phases read the move's own). Hooks (`move.type`, `move.category`), point handlers and the damage read
/// it from there: one move_type point run per target, not per hit.
/// @param s       battle
/// @param target  the target, or kBench
void aim_hit(State& s, uint8_t target)
{
   MoveCursor& mc = s.mv;
   mc.hit_type = kNone8;   // not worked out while the move_type point runs
   if (mc.move >= data().move_count)
   {
      return;
   }
   const uint8_t type = move_type_of(s, mc.user, target, mc.move);
   mc.hit_type = type;
   mc.hit_category = category_of(mc.move, type);
   mc.hit_target = target;
}

/// The type a move hits a battler with: the move in progress's worked-out one for that target (aim_hit()), else
/// move_type_of() (the AI's estimates, previews).
/// @param s       battle
/// @param user    the move's user
/// @param target  its target
/// @param move    move index
/// @return type index
uint8_t hit_type_of(const State& s, uint8_t user, uint8_t target, uint16_t move)
{
   const MoveCursor& mc = s.mv;
   if (mc.move == move && mc.user == user && mc.hit_target == target && mc.hit_type != kNone8)
   {
      return mc.hit_type;
   }
   return move_type_of(s, user, target, move);
}

/// Speed for turn order: the ruleset's speed pipeline (the order stat with its stage, then its points).
/// @param s  battle
/// @param i  battler
/// @return effective speed
int32_t speed_of(State& s, uint8_t i)
{
   const Battler& b = s.battlers[i];
   const int order = data().order_stat;
   PipelineCtx c = pipeline_ctx(s, &s.rng, i, kBench, kNone16, kNoFixedRoll);
   return rules::speed(c, SpeedInputs{b.stats[order], stage_value(b, order), stage_of(static_cast<uint8_t>(order))});
}

/// One combine step: a Q8 factor times a tier's factor, truncating, saturating at kMaxU16.
/// @param q8    factor so far
/// @param tier  the tier's factor
/// @return the product
int32_t combine_q8(int32_t q8, uint16_t tier)
{
   const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(q8) * tier) >> kQ8Shift);
   return v < kMaxU16 ? v : kMaxU16;
}

/// The type chart's result for a move type against a battler (C-08h, D-22 A6): the defender's types in order, the
/// first decisive tier ending it; the scaling tiers combined by BattleData::combine.
/// @param type  attacking type
/// @param b     defender
/// @return the result (neutral for a type outside the chart)
TypeEffect type_effect(uint8_t type, const Battler& b)
{
   TypeEffect fx{kQ8One, 0, kNone8, static_cast<uint8_t>(TierKind::Scale)};
   if (type >= data().type_count)
   {
      return fx;
   }
   for (uint8_t t : b.types)
   {
      if (t == kNone8 || t >= data().type_count)
      {
         continue;
      }
      const uint8_t index = chart(type, t);
      const TierData& tier = data().tiers[index];
      if (tier.kind != static_cast<uint8_t>(TierKind::Scale))
      {
         return TypeEffect{kQ8One, 0, index, tier.kind};
      }
      fx.exp = static_cast<int16_t>(fx.exp + tier.exp);
      fx.q8 = combine_q8(fx.q8, tier.q8);
   }
   if (data().combine == static_cast<uint8_t>(Combine::Exponent))
   {
      // every scaling tier is a power of two: the factor follows from the sum, capped like pow2()
      fx.q8 = rules::pow2(kQ8One, fx.exp);
   }
   else
   {
      fx.exp = 0;   // type_exp is the Exponent mode's
   }
   return fx;
}

/// Says a tier's message (a Msg), if it has one.
/// @param s        battle
/// @param message  Msg code, or kNone8
/// @param user     the hit's user
/// @param target   its target
void say_tier(State& s, uint8_t message, uint8_t user, uint8_t target)
{
   if (message < data().message_count)
   {
      say(s, static_cast<Msg>(message), user, target);
   }
}

/// The effectiveness message of a hit: a decisive Ko tier's own, else chart.messages.above / below by the
/// combined factor.
/// @param s       battle
/// @param hit     the hit
/// @param user    its user
/// @param target  its target
void say_effectiveness(State& s, const Hit& hit, uint8_t user, uint8_t target)
{
   if (hit.ko)
   {
      say_tier(s, data().tiers[hit.type.decisive].message, user, target);
      return;
   }
   if (hit.type.q8 > kQ8One)
   {
      say_tier(s, data().above_message, user, target);
   }
   if (hit.type.q8 < kQ8One)
   {
      say_tier(s, data().below_message, user, target);
   }
}

/// Changes a stat stage with its messages.
/// @param s       battle
/// @param i       battler
/// @param stage   stage index (Battler::stages)
/// @param delta   change
/// @param quiet_fail  no message when nothing changes
/// @param silent      no message at all (E13b: effect `stage ... silent`; the Stage event stays for the HUD)
/// @return true if it changed
bool change_stage(State& s, uint8_t i, uint8_t stage, int delta, bool quiet_fail, bool silent)
{
   if (stage >= data().stage_count || delta == 0 || !alive_active(s, i))
   {
      return false;
   }
   Battler& b = s.battlers[i];
   const int lo = data().stages[stage].lo;
   const int hi = data().stages[stage].hi;
   const int old = b.stages[stage];
   const int now = clamp_to(old + delta, lo, hi);
   if (now == old)
   {
      if (!quiet_fail)
      {
         say(s, delta > 0 ? Msg::StatMax : Msg::StatMin, i, kBench, 0, stage);
      }
      return false;
   }
   b.stages[stage] = static_cast<int8_t>(now);
   emit(s, EventKind::Stage, i, kBench, stage, 0, static_cast<uint16_t>(now - old));
   if (silent)
   {
      return true;
   }
   const Msg m = delta > 0 ? (delta > 1 ? Msg::StatUp2 : Msg::StatUp) : (delta < -1 ? Msg::StatDown2 : Msg::StatDown);
   say(s, m, i, kBench, 0, stage);
   return true;
}

/// The crit pipeline for a move used by a battler.
/// @param s     battle
/// @param rng   stream it draws from, or nullptr (previews: no draw)
/// @param user  attacker
/// @param move  move index
/// @return the outcome and the odds
ChanceResult crit_of(const State& s, rng::Stream* rng, uint8_t user, uint16_t move)
{
   PipelineCtx c = pipeline_ctx(s, rng, user, kBench, move, rng != nullptr ? kNoFixedRoll : 0);
   return rules::crit(c, CritInputs{stage_at(s.battlers[user], data().crit_stage), data().moves[move].crit_stage});
}

/// Damage of a move from one battler to another (C-08: the decisive type tiers, then the ruleset's crit and
/// damage pipelines). Real hits, the AI's estimates and damage_preview() all come through here.
/// @param s        battle
/// @param user     attacker
/// @param target   defender
/// @param move     move index
/// @param spread   hits more than one target
/// @param roll     fixed damage roll (0 = lowest), or -1 to roll now
/// @param crit     roll for a critical hit, or take it as given
/// @param draw     a 16-bit draw made earlier that the rolls scale (the AI's roll of the turn), or kNoDraw
/// @return the hit
Hit calc_damage(State& s, uint8_t user, uint8_t target, uint16_t move, bool spread, int roll, CritMode crit,
                int32_t draw)
{
   const MoveData& m = data().moves[move];
   const Battler& u = s.battlers[user];
   const Battler& t = s.battlers[target];
   const uint8_t type = hit_type_of(s, user, target, move);
   Hit hit{0, type_effect(type, t), false, false, false};
   hit.zero = hit.type.kind == static_cast<uint8_t>(TierKind::Zero);
   hit.ko = hit.type.kind == static_cast<uint8_t>(TierKind::Ko);
   if (hit.zero)
   {
      return hit;
   }
   if (hit.ko)
   {
      hit.damage = t.hp;
      return hit;
   }
   hit.crit = crit == CritMode::Roll ? crit_of(s, &s.rng, user, move).result != 0 : crit == CritMode::Always;

   // The category names the stats (the compiler only gives damage effects to damaging categories; C5: the hit
   // type's under hit_category_by_type, a damaging one too).
   const uint8_t category = category_of(move, type);
   const CategoryData& cat = data().categories[category];
   PipelineCtx c = pipeline_ctx(s, &s.rng, user, target, move, roll);
   c.crit = hit.crit ? 1 : 0;
   c.move_type = type;
   c.move_category = category;
   c.draw = draw;
   const DamageInputs in{u.level, m.power, u.stats[cat.offense], stage_value(u, cat.offense), t.stats[cat.defense],
                         stage_value(t, cat.defense), c.crit, spread ? 1 : 0, has_type(u, type) ? 1 : 0,
                         hit.type.exp, stage_of(cat.offense), stage_of(cat.defense)};
   hit.damage = rules::damage(c, in);
   return hit;
}

/// Damage of a self-hit (effect `self_hit`: a confused battler hitting itself): the ruleset's self_hit pipeline
/// over the stats of the rules' self-hit category (its own offense stat against its own defense stat). Real
/// self-hits and self_hit_preview() both come through here.
/// @param s     battle
/// @param i     battler
/// @param roll  fixed damage roll (0 = lowest), or -1 to roll now
/// @param power  the hit's power (effect `self_hit`), or -1 for Rules::self_hit_power
/// @return HP to take
int32_t self_hit_damage(State& s, uint8_t i, int roll, int32_t power)
{
   const Battler& b = s.battlers[i];
   const CategoryData& cat = data().categories[data().rules.self_hit_category];
   PipelineCtx c = pipeline_ctx(s, &s.rng, i, i, kNone16, roll);
   const int32_t pw = power < 0 ? data().rules.self_hit_power : power;
   const SelfHitInputs in{b.level, pw, b.stats[cat.offense], stage_value(b, cat.offense),
                          b.stats[cat.defense], stage_value(b, cat.defense), stage_of(cat.offense),
                          stage_of(cat.defense)};
   return rules::self_hit(c, in);
}

/// The accuracy pipeline for a move that can miss: its accuracy, the user's accuracy stage and the target's
/// evasion stage. Real accuracy checks and hit_chance_preview() both come through here.
/// @param s       battle
/// @param rng     stream it draws from, or nullptr (previews: no draw)
/// @param user    attacker
/// @param target  defender
/// @param move    move index (accuracy != 0)
/// @return the outcome and the odds
ChanceResult accuracy_of(const State& s, rng::Stream* rng, uint8_t user, uint8_t target, uint16_t move)
{
   return accuracy_at(s, rng, user, target, move, data().moves[move].accuracy);
}

/// The accuracy pipeline with an accuracy of its own (G1: a delayed hit's landing, DelayedHitData::accuracy).
/// @param s         battle
/// @param rng       stream it draws from, or nullptr (previews: no draw)
/// @param user      attacker
/// @param target    defender
/// @param move      move index
/// @param accuracy  percent (1-100)
/// @return the outcome and the odds
ChanceResult accuracy_at(const State& s, rng::Stream* rng, uint8_t user, uint8_t target, uint16_t move,
                         int32_t accuracy)
{
   PipelineCtx c = pipeline_ctx(s, rng, user, target, move, rng != nullptr ? kNoFixedRoll : 0);
   const AccuracyInputs in{accuracy, stage_at(s.battlers[user], data().accuracy_stage),
                           stage_at(s.battlers[target], data().evasion_stage)};
   return rules::accuracy(c, in);
}

/// The effect odds pipeline (E9): whether a secondary effect or a status of battler @p by lands on battler
/// @p on, from the odds the effect logic asks for and both battlers' odds stat (BattleData::odds_stat; 0 without
/// one) and levels. Effect logic `odds()` comes through here (effect::host_odds()).
/// @param s     battle
/// @param rng   stream it draws from, or nullptr (previews: no draw)
/// @param by    the battler causing it
/// @param on    the battler it lands on
/// @param move  the context move, or kNone16
/// @param n     successes (>= 1; clamped to 1..kMaxU16: the effect compiler refuses an operand whose analysed
///              range passes it, effect_compiler.py ODDS_OPERAND_MAX)
/// @param m     outcomes (>= 1; clamped the same way)
/// @return the outcome and the odds
ChanceResult effect_odds_of(const State& s, rng::Stream* rng, uint8_t by, uint8_t on, uint16_t move, int32_t n,
                            int32_t m)
{
   PipelineCtx c = pipeline_ctx(s, rng, by, on, move, rng != nullptr ? kNoFixedRoll : 0);
   const uint8_t stat = data().odds_stat;
   const Battler& user = s.battlers[by];
   const Battler& target = s.battlers[on];
   const bool has_stat = stat < data().stat_count;
   const EffectOddsInputs in{clamp_to<int32_t>(n, 1, kMaxU16), clamp_to<int32_t>(m, 1, kMaxU16),
                             has_stat ? user.stats[stat] : 0, has_stat ? target.stats[stat] : 0, user.level,
                             target.level};
   return rules::effect_odds(c, in);
}

}   // namespace capsule::battle::detail

// -- public ------------------------------------------------------------------------------------------------

namespace capsule::battle
{

using namespace detail;

int32_t effectiveness(const State& s, uint8_t type, uint8_t battler)
{
   const TypeEffect fx = type_effect(type, s.battlers[battler]);
   if (fx.kind == static_cast<uint8_t>(TierKind::Zero))
   {
      return 0;
   }
   if (fx.kind == static_cast<uint8_t>(TierKind::Ko))
   {
      return kKoEffectiveness;
   }
   return fx.q8;
}

int32_t damage_preview(State& s, uint8_t user, uint8_t target, uint16_t move, bool spread, int roll, bool crit)
{
   ENGINE_ASSERT(user < s.battler_count && target < s.battler_count && move < data().move_count);
   // Point handlers queue nothing and a fixed roll draws nothing; the count is restored anyway, as for the
   // AI's estimates.
   const uint16_t saved_events = s.ev_count;
   const Hit hit = calc_damage(s, user, target, move, spread, roll, crit ? CritMode::Always : CritMode::Never);
   s.ev_count = saved_events;
   return hit.damage;
}

int32_t speed_preview(State& s, uint8_t battler)
{
   ENGINE_ASSERT(battler < s.battler_count);
   const Battler& b = s.battlers[battler];
   const int order = data().order_stat;
   PipelineCtx c = pipeline_ctx(s, nullptr, battler, kBench, kNone16, 0);
   return rules::speed(c, SpeedInputs{b.stats[order], stage_value(b, order), stage_of(static_cast<uint8_t>(order))});
}

Odds hit_chance_preview(const State& s, uint8_t user, uint8_t target, uint16_t move)
{
   ENGINE_ASSERT(user < s.battler_count && target < s.battler_count && move < data().move_count);
   if (data().moves[move].accuracy == 0)
   {
      return Odds{0, 0};
   }
   const ChanceResult r = accuracy_of(s, nullptr, user, target, move);
   return Odds{static_cast<uint16_t>(r.chance), static_cast<uint16_t>(r.out_of)};
}

Odds effect_odds_preview(const State& s, uint8_t by, uint8_t on, uint16_t n, uint16_t m, uint16_t move)
{
   ENGINE_ASSERT(by < s.battler_count && on < s.battler_count);
   if (n == 0 || m == 0)
   {
      return Odds{0, m};   // odds() with them never succeeds (and draws nothing)
   }
   const ChanceResult r = effect_odds_of(s, nullptr, by, on, move, n, m);
   return Odds{static_cast<uint16_t>(r.chance), static_cast<uint16_t>(r.out_of)};
}

Odds crit_chance_preview(const State& s, uint8_t user, uint16_t move)
{
   ENGINE_ASSERT(user < s.battler_count && move < data().move_count);
   const ChanceResult r = crit_of(s, nullptr, user, move);
   return Odds{static_cast<uint16_t>(r.chance), static_cast<uint16_t>(r.out_of)};
}

int32_t self_hit_preview(State& s, uint8_t battler, int roll)
{
   ENGINE_ASSERT(battler < s.battler_count);
   return self_hit_damage(s, battler, roll);
}

}   // namespace capsule::battle
