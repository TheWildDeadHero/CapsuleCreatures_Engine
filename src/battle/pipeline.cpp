/// @file
/// Pipelines (D-20 D): the engine side of the generated pipelines: modifier points and their sources, rolls,
/// the helpers that read the battle's tables (point handlers read through effect::point_get / point_has,
/// src/battle/effect_host.cpp).
/// See capsule/battle/pipeline.h; the modules share internal.h.
#include "internal.h"

#include "capsule/battle/pipeline.h"

namespace capsule::battle::detail
{

/// @name Points
///@{
constexpr int32_t kPointValueMax = kMaxU16;   ///< Every point handler sees its value clamped to [0, this].
///@}

/// The battler a point source belongs to: the target for the target's sources, else the user (the weather's and
/// the move's holder too).
/// @param c     the pipeline
/// @param from  the source
/// @return battler index (kBench and others out of range: no holder)
uint8_t source_holder(const PipelineCtx& c, PointSource from)
{
   switch (from)
   {
   case PointSource::TargetPassive:
   case PointSource::TargetItem:
   case PointSource::TargetStatus:
   case PointSource::TargetSide:
   case PointSource::TargetVolatile:
      return c.target;
   default:
      return c.user;
   }
}

/// Calls @p f (const effect::Owner&, uint8_t holder) for each owner of a point source with a handler for the point:
/// the holder's passive, item or status, the weather, the move, or (E13b) every side condition on the holder's side
/// and every volatile the holder has, in data order. Stops early when @p f returns false.
/// @tparam F     callable returning bool
/// @param c      the pipeline
/// @param from   the source
/// @param point  point id
/// @param f      the visitor
/// @return false if @p f stopped it
template <typename F>
bool for_each_source_owner(const PipelineCtx& c, PointSource from, uint8_t point, F&& f)
{
   const State& s = *c.s;
   const uint8_t holder = source_holder(c, from);
   if (holder >= s.battler_count)
   {
      return true;
   }
   const Battler& b = s.battlers[holder];
   auto visit = [&](uint16_t owner) -> bool
   {
      const effect::Owner* o = owner_at(owner);
      // the owner's point mask rejects most calls before its handlers are read
      return o == nullptr || !has_flag(o->point_mask, point) || f(*o, holder);
   };
   switch (from)
   {
   case PointSource::UserPassive:
   case PointSource::TargetPassive:
      return visit(b.passive < data().passive_count ? data().passives[b.passive].owner : kNone16);
   case PointSource::UserItem:
   case PointSource::TargetItem:
      return visit(live_item(b) != 0 && b.item < data().item_count ? data().items[b.item].owner : kNone16);
   case PointSource::UserStatus:
   case PointSource::TargetStatus:
      return visit(b.status != 0 && b.status < data().status_count ? data().statuses[b.status].owner : kNone16);
   case PointSource::Weather:
   {
      const uint8_t w = acting_weather(s);   // C6: a suppressed weather's points do not act
      return visit(w < data().weather_count ? data().weathers[w].owner : kNone16);
   }
   case PointSource::Move:
      return visit(c.move < data().move_count ? data().moves[c.move].owner : kNone16);
   case PointSource::UserSide:
   case PointSource::TargetSide:
   {
      const uint8_t side = side_of(s, holder);
      for (uint8_t k = 0; side < kMaxSides && k < data().side_cond_count; ++k)
      {
         if (has_flag(s.sides[side].cond, k) && !visit(data().side_conds[k].owner))
         {
            return false;
         }
      }
      return true;
   }
   case PointSource::UserVolatile:
   case PointSource::TargetVolatile:
      for (uint8_t k = 0; k < data().volatile_count; ++k)
      {
         if (has_flag(b.vol, k) && !visit(data().volatiles[k].owner))
         {
            return false;
         }
      }
      return true;
   default:
      return true;
   }
}

}   // namespace capsule::battle::detail

// -- public ------------------------------------------------------------------------------------------------

namespace capsule::battle
{

using namespace detail;

int32_t run_point(PipelineCtx& c, uint8_t point, int32_t v)
{
   if (point >= data().point_count)
   {
      return v;
   }
   const PointData& p = data().points[point];
   const int sources = p.source_count < kMaxPointSources ? p.source_count : kMaxPointSources;
   // C5: one pass; the value is clamped to the point's range only when a handler ran (none: it stays as it is)
   bool ran = false;
   for (int k = 0; k < sources; ++k)
   {
      for_each_source_owner(c, p.sources[k], point, [&c, &v, &ran, point](const effect::Owner& o, uint8_t holder)
      {
         const uint8_t other = holder == c.user ? c.target : c.user;
         for (int i = 0; i < o.handler_count; ++i)
         {
            const effect::Handler& h = data().effects.handlers[o.first_handler + i];
            if ((h.flags & effect::kHandlerPoint) == 0 || h.hook != point)
            {
               continue;
            }
            effect::PointCtx pc{&c, holder, other, {0, 0}};
            v = rules::point_handler(h.program, pc, clamp_to<int32_t>(v, 0, kPointValueMax));
            ran = true;
         }
         return true;
      });
   }
   return ran ? clamp_to<int32_t>(v, p.lo, p.hi) : v;
}

int32_t pipeline_roll(PipelineCtx& c, int32_t span)
{
   if (span <= 0)
   {
      return 0;
   }
   if (c.fixed_roll >= 0)
   {
      return c.fixed_roll < span ? c.fixed_roll : span - 1;
   }
   if (c.draw >= 0)
   {
      // the draw scaled to [0, span) exactly as rng::below() scales one
      return static_cast<int32_t>((static_cast<uint32_t>(c.draw) * static_cast<uint32_t>(span)) >> rng::kDrawBits);
   }
   if (c.rng != nullptr)
   {
      return static_cast<int32_t>(rng::below(*c.rng, static_cast<uint32_t>(span)));
   }
   return 0;
}

int32_t pipeline_stage_scale(int32_t id, int32_t v, int32_t stage)
{
   if (id < 0 || id >= data().stage_count)
   {
      return v;   // no such stage: x1
   }
   const StageData& st = data().stages[id];
   const int32_t at = clamp_to<int32_t>(stage, st.lo, st.hi);
   if (st.table != nullptr)
   {
      const Ratio r = st.table[at - st.lo];
      return r.den != 0 ? v * r.num / r.den : v;
   }
   if (st.formula != kNone8)
   {
      return rules::stage_formula(st.formula, v, at);
   }
   return v;
}

int32_t pipeline_staged(int32_t v, int32_t stage)
{
   return pipeline_stage_scale(data().stat_stage, v, stage);
}

int32_t pipeline_accuracy_staged(int32_t v, int32_t stage)
{
   return pipeline_stage_scale(data().accuracy_stage, v, stage);
}

namespace
{

/// The crit stage's odds at a crit stage value (clamped to its range).
/// @param stage  crit stage value
/// @return the odds, or {0, 0} (never) without a crit stage or odds table
Odds crit_odds_at(int32_t stage)
{
   if (data().crit_stage >= data().stage_count)
   {
      return Odds{0, 0};
   }
   const StageData& st = data().stages[data().crit_stage];
   return st.odds != nullptr ? st.odds[clamp_to<int32_t>(stage, st.lo, st.hi) - st.lo] : Odds{0, 0};
}

} // namespace

int32_t pipeline_crit_chance(int32_t stage)
{
   return crit_odds_at(stage).n;
}

int32_t pipeline_crit_out_of(int32_t stage)
{
   return crit_odds_at(stage).m;
}

namespace
{

/// v times a Q8 factor, truncating, in 32-bit arithmetic: the ruleset compiler refuses a negative type_scale()
/// argument and any v whose product with the chart's largest factor leaves int32 (ruleset_compiler.py
/// type_scale_interval()), so the unsigned product cannot wrap (no 64-bit multiply on the ARM7TDMI).
/// @param v   value (>= 0)
/// @param q8  factor (Q8, 0..kMaxU16: a tier's, or a combined factor that combine_q8() saturates)
/// @return the scaled value
int32_t scale_q8(int32_t v, int32_t q8)
{
   ENGINE_ASSERT(v >= 0 && q8 >= 0);
   const uint32_t value = static_cast<uint32_t>(v < 0 ? 0 : v);
   const uint32_t factor = static_cast<uint32_t>(q8 < 0 ? 0 : q8);
   return static_cast<int32_t>((value * factor) >> kQ8Shift);
}

} // namespace

int32_t pipeline_type_scale(const PipelineCtx& c, int32_t v)
{
   if (c.s == nullptr || c.move >= data().move_count || c.target >= c.s->battler_count)
   {
      return v;
   }
   // the type the hit has (E13b: the move_type point's result), else the move's own
   const uint8_t type = c.move_type != kNone8 ? c.move_type : data().moves[c.move].type;
   const Battler& b = c.s->battlers[c.target];
   if (data().combine != static_cast<uint8_t>(Combine::Sequential))
   {
      // one combined factor (decisive tiers never reach a pipeline)
      const detail::TypeEffect fx = detail::type_effect(type, b);
      if (data().combine == static_cast<uint8_t>(Combine::Exponent))
      {
         return rules::pow2(v, fx.exp);
      }
      return scale_q8(v, fx.q8);
   }
   if (type >= data().type_count)
   {
      return v;
   }

   // each scaling tier in the defender's type order, truncating after each
   for (uint8_t t : b.types)
   {
      if (t == kNone8 || t >= data().type_count)
      {
         continue;
      }
      const TierData& tier = data().tiers[chart(type, t)];
      if (tier.kind == static_cast<uint8_t>(TierKind::Scale))
      {
         v = scale_q8(v, tier.q8);
      }
   }
   return v;
}

}   // namespace capsule::battle
