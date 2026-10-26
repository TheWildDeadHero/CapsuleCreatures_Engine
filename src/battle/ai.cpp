/// @file
/// The AI (D-18, D-26 section 1): an AI party's profile (capsule/battle/data/ai.h) scores every usable move and
/// target from the state at the start of the turn, then its switching rules may switch out instead. Every rule's
/// number is data; the rules run in the profile's order, and only a Noise rule draws.
/// See capsule/battle/battle.h; the modules share internal.h.
#include "internal.h"

namespace capsule::battle::detail
{

/// The AI's estimate of a fixed-damage move (no formula involved; MoveData's fixed-damage summary), against the
/// type the move would hit with (C5: the move_type point's result, as calc_damage() uses for formula moves).
/// @param s     battle
/// @param i     user
/// @param t     target
/// @param move  move index
/// @return the hit
Hit fixed_estimate(const State& s, uint8_t i, uint8_t t, uint16_t move)
{
   const MoveData& m = data().moves[move];
   Hit h{0, type_effect(hit_type_of(s, i, t, move), s.battlers[t]), false, false, false};
   h.zero = h.type.kind == static_cast<uint8_t>(TierKind::Zero);
   h.ko = h.type.kind == static_cast<uint8_t>(TierKind::Ko);
   if (h.zero)
   {
      return h;
   }
   h.damage = m.fixed_amount + static_cast<int32_t>(s.battlers[i].level) * m.fixed_levels;
   if (m.fixed_fraction != 0)
   {
      h.damage += rules::max32(1, (static_cast<int32_t>(s.battlers[t].hp) * m.fixed_fraction) >> kQ8Shift);
   }
   return h;
}

/// What one option (a move aimed one way) would do to the battlers it hits, worked out once for every rule of the
/// profile. The estimates draw nothing and queue nothing (no crit, the turn's roll; D-18).
struct AiOption
{
   uint16_t move;                    ///< The move.
   uint8_t slot;                     ///< Its slot.
   uint8_t hits;                     ///< Positions it would hit (a chosen target, or what it hits on its own).
   uint8_t valid;                    ///< Positions of hits with a battler other than the user.
   uint8_t foes;                     ///< ...of them, the foes.
   uint8_t zero;                     ///< ...of them, those it has no effect on (a decisive Zero tier).
   uint8_t target;                   ///< The battler the ai_score hook and pipeline see as its target.
   int32_t damage[kMaxPositions];    ///< Its expected damage per position (damaging moves).
};

/// The rules of an AI profile: scoring or switching.
struct AiRules
{
   const AiRuleData* rules;   ///< First rule.
   int count;                 ///< Rules.
};

/// The scoring rules of a party's profile (none when the data has no profiles).
/// @param s      battle
/// @param party  party index
/// @param which  false: scoring, true: switching
/// @return the rules
AiRules profile_rules(const State& s, uint8_t party, bool which)
{
   const uint8_t p = s.parties[party].ai_profile;
   if (p >= data().ai_profile_count || data().ai_profiles == nullptr)
   {
      return AiRules{nullptr, 0};
   }
   const AiProfileData& pd = data().ai_profiles[p];
   return which ? AiRules{data().ai_rules + pd.first_switch, pd.switch_count}
                : AiRules{data().ai_rules + pd.first_rule, pd.rule_count};
}

/// Works out an option's estimates: per position it would hit, the battler there, whether it is a foe and, for a
/// damaging move, the hit (or that it has no effect).
/// @param s      battle
/// @param i      user
/// @param o      the option (move, slot and hits set; the rest filled in)
/// @param draw   the turn's roll draw
void estimate(State& s, uint8_t i, AiOption& o, int32_t draw)
{
   const MoveData& m = data().moves[o.move];
   o.valid = o.foes = o.zero = 0;
   o.target = kBench;
   for (uint8_t q = 0; q < format(s).positions; ++q)
   {
      o.damage[q] = 0;
      const uint8_t t = s.at[q];
      if (((o.hits >> q) & 1) == 0 || t == kBench || t == i)
      {
         continue;
      }
      o.valid = static_cast<uint8_t>(o.valid | bit(q));
      const bool foe = side_of(s, t) != side_of(s, i);
      o.foes = static_cast<uint8_t>(foe ? o.foes | bit(q) : o.foes);
      o.target = o.target == kBench ? t : o.target;
      if (!damaging(m))
      {
         continue;
      }
      // Snapshot estimate: no crit, the turn's fixed roll (D-18); the stream and the ring are put back.
      const rng::Stream saved = s.rng;
      const uint16_t saved_events = s.ev_count;
      const Hit h = m.formula != 0 ? calc_damage(s, i, t, o.move, o.hits != bit(q), -1, CritMode::Never, draw)
                                   : fixed_estimate(s, i, t, o.move);
      s.rng = saved;
      s.ev_count = saved_events;
      o.zero = static_cast<uint8_t>(h.zero ? o.zero | bit(q) : o.zero);
      o.damage[q] = h.damage;
   }
   o.target = o.target == kBench ? i : o.target;   // a move that hits nobody else: the user itself
}

/// Runs the ai_score handlers of one owner on a score (G1; the compiler keeps them pure: reads, modify, block).
/// @param s      battle
/// @param o      the owner
/// @param ctx    the run's registers (holder, other, user, target, move; value in)
/// @param value  the score: in and out
/// @return false when a handler vetoes it (block)
bool ai_owner(State& s, const effect::Owner& o, effect::Context ctx, int32_t& value)
{
   const effect::Bank& bank = data().effects;
   effect::HostState host{s};
   ctx.owner = static_cast<uint16_t>(&o - bank.owners);
   for (int k = 0; k < o.handler_count; ++k)
   {
      const effect::Handler& hn = bank.handlers[o.first_handler + k];
      if ((hn.flags & effect::kHandlerPoint) != 0 || hn.hook != static_cast<uint8_t>(effect::Hook::AiScore))
      {
         continue;
      }
      ctx.value = value;
      const effect::Status st = effect::invoke(s.vm, bank, host, hn.program, ctx);
      value = ctx.value;
      if (st == effect::Status::Blocked)
      {
         return false;
      }
   }
   return true;
}

/// Runs the ai_score handlers of every owner a battler holds (in the dispatch order).
/// @param s       battle
/// @param holder  the battler
/// @param ctx     the registers (holder and owner filled in per owner)
/// @param value   the score: in and out
/// @return false on a veto
bool ai_holder(State& s, uint8_t holder, effect::Context ctx, int32_t& value)
{
   ctx.holder = holder;
   return for_each_owner(s, holder, [&](Source, const effect::Owner& o)
   {
      return !effect::has_hook(o, effect::Hook::AiScore) || ai_owner(s, o, ctx, value);
   });
}

/// The ai_score hook for an option (G1): the move's own handlers (holder = the user), the user's owners, then the
/// target's (holder = the target, other = the user).
/// @param s      battle
/// @param i      the AI's battler
/// @param o      the option
/// @param score  the score: in and out
/// @return false on a veto
bool ai_hook(State& s, uint8_t i, const AiOption& o, int32_t& score)
{
   const effect::Context ctx{score, 0, o.move, 0, i, o.target, i, o.target};
   const effect::Owner* own = owner_at(data().moves[o.move].owner);
   if (own != nullptr && effect::has_hook(*own, effect::Hook::AiScore) &&
       !ai_owner(s, *own, ctx, score))
   {
      return false;
   }
   if (!ai_holder(s, i, ctx, score))
   {
      return false;
   }
   // the target's owners: holder = the target, other = the user (the registers user and target stay)
   return o.target == i || ai_holder(s, o.target, effect::Context{0, 0, o.move, 0, o.target, i, i, o.target}, score);
}

/// The ruleset pipeline ai_score for an option (G1): its `score` (added), from the score so far, the expected
/// damage, the user and the target.
/// @param s      battle
/// @param i      the AI's battler
/// @param o      the option
/// @param score  the score so far
/// @return what the pipeline adds
int32_t ai_pipeline(State& s, uint8_t i, const AiOption& o, int32_t score)
{
   const MoveData& m = data().moves[o.move];
   int32_t damage = 0;
   int32_t ko = 0;
   for (uint8_t q = 0; q < format(s).positions; ++q)
   {
      if (((o.foes >> q) & 1) != 0 && ((o.zero >> q) & 1) == 0)
      {
         damage = rules::min32(kMaxU16, damage + o.damage[q]);
         ko = ko != 0 || o.damage[q] >= s.battlers[s.at[q]].hp ? 1 : 0;
      }
   }
   const Battler& u = s.battlers[i];
   const Battler& t = s.battlers[o.target];
   PipelineCtx c = pipeline_ctx(s, nullptr, i, o.target, o.move, 0);
   const AiScoreInputs in{clamp_to<int32_t>(score, INT16_MIN, INT16_MAX), damage, ko, damaging(m) ? 1 : 0,
                          m.accuracy, m.statuses, u.hp, max_hp(u), t.hp, max_hp(t), s.turn};
   return rules::ai_score(c, in);
}

/// Scores one option by the profile's rules, in order (D-18's terms are rules with their numbers as data, G1).
/// @param s      battle
/// @param i      the AI's battler
/// @param o      the option
/// @param rules  the profile's scoring rules
/// @param score  out: the score
/// @return false when a rule vetoes it (or it is a damaging move that hits nobody)
bool score_option(State& s, uint8_t i, const AiOption& o, AiRules rules, int32_t& score)
{
   const MoveData& m = data().moves[o.move];
   const Battler& b = s.battlers[i];
   const bool hurts = damaging(m);
   bool kept = !(hurts && o.hits == 0);   // a damaging move with nothing to hit is never chosen
   score = 0;
   // Every rule runs (a veto does not skip the draws of the rules after it).
   for (int k = 0; k < rules.count; ++k)
   {
      const AiRuleData& r = rules.rules[k];
      switch (r.kind)
      {
      case AiRuleKind::Noise:
         score += static_cast<int32_t>(rng::below(s.rng, static_cast<uint32_t>(rules::max32(r.arg, 1))));
         break;
      case AiRuleKind::Damage:
         for (uint8_t q = 0; hurts && q < format(s).positions; ++q)
         {
            if (((o.valid >> q) & 1) == 0 || ((o.zero >> q) & 1) != 0)
            {
               continue;
            }
            // damage x hit chance; a knock-out is worth the target's HP and the bonus (an ally's counts against)
            const int32_t acc = m.accuracy == 0 ? kPercent : m.accuracy;
            const int32_t hp = s.battlers[s.at[q]].hp;
            const int32_t worth = o.damage[q] >= hp ? hp + r.arg2 : o.damage[q];
            score += (((o.foes >> q) & 1) != 0 ? worth : -worth) * acc / kPercent * r.arg;
         }
         break;
      case AiRuleKind::AvoidUseless:
         kept = kept && !(hurts && (o.zero & o.foes) != 0);
         break;
      case AiRuleKind::Status:
         for (uint8_t q = 0; !hurts && q < format(s).positions; ++q)
         {
            if (((o.foes >> q) & 1) != 0 && s.battlers[s.at[q]].status == 0)
            {
               score += r.arg * m.statuses;   // per status it inflicts
            }
         }
         break;
      case AiRuleKind::StatusMove:
         score += hurts ? 0 : r.arg;
         break;
      case AiRuleKind::SaveResources:
         if (score >= 0)
         {
            // affordable moves first; a cost never makes a move look vetoed (a negative score)
            score = rules::max32(0, score - cost_share(b, o.move, o.slot, r.arg));
         }
         break;
      case AiRuleKind::Pipeline:
      {
         const int32_t add = ai_pipeline(s, i, o, score);
         kept = kept && (r.arg == kAiNoVeto || add >= r.arg);
         score += add;
         break;
      }
      case AiRuleKind::Hook:
         kept = ai_hook(s, i, o, score) && kept;
         break;
      default:
         break;   // switching rules
      }
   }
   return kept;
}

/// Whether the profile's switching rules switch the battler out (G1), given the best move's score.
/// @param s     battle
/// @param i     the AI's battler
/// @param best  the best move's score (-1: none worth using)
/// @return true to switch
bool wants_switch(State& s, uint8_t i, int32_t best)
{
   const AiRules rules = profile_rules(s, s.battlers[i].party, true);
   for (int k = 0; k < rules.count; ++k)
   {
      const AiRuleData& r = rules.rules[k];
      if (r.kind == AiRuleKind::SwitchBelow && best < r.arg)
      {
         return true;
      }
      if (r.kind == AiRuleKind::SwitchHook)
      {
         // the ai_score hook with move none: what switching out is worth against the best move
         uint8_t foe = kBench;
         for (uint8_t p = 0; p < format(s).positions && foe == kBench; ++p)
         {
            const uint8_t o = s.at[p];
            foe = o != kBench && alive_active(s, o) && side_of(s, o) != side_of(s, i) ? o : kBench;
         }
         int32_t value = 0;
         const bool kept = ai_holder(s, i, effect::Context{0, 0, kNone16, 0, i, foe, i, foe}, value);
         if (kept && value > best)
         {
            return true;
         }
      }
   }
   return false;
}

/// The item-use rules of a party's profile (G3): right after its switching rules.
/// @param s      battle
/// @param party  party index
/// @return the rules (none when the data has no profiles)
AiRules item_rules(const State& s, uint8_t party)
{
   const uint8_t p = s.parties[party].ai_profile;
   if (p >= data().ai_profile_count || data().ai_profiles == nullptr)
   {
      return AiRules{nullptr, 0};
   }
   const AiProfileData& pd = data().ai_profiles[p];
   return AiRules{data().ai_rules + pd.first_switch + pd.switch_count, pd.item_count};
}

/// The Item action an AI battler takes on itself (G3): the item's target is the battler (a member, its position,
/// or nothing to choose).
/// @param s     battle
/// @param i     the AI's battler
/// @param item  item id
/// @return the action
Action self_item(const State& s, uint8_t i, uint16_t item)
{
   Action a{ActionKind::Item, 0, s.battlers[i].position, i};
   a.item = item;
   return a;
}

/// Whether the profile's item-use rules use an item (G3), given the best move's score: the first rule whose item
/// the party still has (and can use on the battler) and whose condition holds.
/// @param s     battle
/// @param i     the AI's battler
/// @param best  the best move's score (-1: none worth using)
/// @return the Item action, or kind None
Action wants_item(State& s, uint8_t i, int32_t best)
{
   const Battler& b = s.battlers[i];
   const AiRules rules = item_rules(s, b.party);
   for (int k = 0; k < rules.count; ++k)
   {
      const AiRuleData& r = rules.rules[k];
      const uint16_t item = static_cast<uint16_t>(r.arg2);
      const Action use = self_item(s, i, item);
      if (!item_usable(s, i, item) || !item_target_valid(s, i, use))
      {
         continue;
      }
      if (r.kind == AiRuleKind::ItemBelowHp && static_cast<int32_t>(b.hp) * kPercent < max_hp(b) * r.arg)
      {
         return use;
      }
      if (r.kind == AiRuleKind::ItemHook)
      {
         // the item's own ai_score handler with move none: what using it is worth against the best move
         const effect::Owner* o = owner_at(data().items[item].owner);
         int32_t value = 0;
         const bool kept = o != nullptr && effect::has_hook(*o, effect::Hook::AiScore) &&
                           ai_owner(s, *o, effect::Context{0, 0, kNone16, 0, i, kBench, i, kBench}, value);
         if (kept && value > best + r.arg)
         {
            return use;
         }
      }
   }
   return Action{ActionKind::None, 0, kBench, 0};
}

/// The replacement an AI switch sends in: the first one of its party that may come in (none while trapped).
/// @param s  battle
/// @param i  the AI's battler
/// @return battler, or kBench
uint8_t ai_replacement(const State& s, uint8_t i)
{
   const PartyState& p = s.parties[s.battlers[i].party];
   for (int k = 0; k < p.count; ++k)
   {
      const uint8_t c = static_cast<uint8_t>(p.first_battler + k);
      if (switch_allowed(s, i, c) && !pending_send(s, c))
      {
         return is_trapped(s, i) ? kBench : c;
      }
   }
   return kBench;
}

/// The AI's choice for one battler, from the state at the start of the turn.
/// @param s  battle
/// @param i  battler
/// @return the action
Action ai_choose(State& s, uint8_t i)
{
   const Battler& b = s.battlers[i];
   const AiRules rules = profile_rules(s, b.party, false);
   Action best{ActionKind::Move, kFallbackSlot, kBench, 0};
   int32_t best_score = -1;
   // One draw for the turn (D-18): every estimate's damage rolls scale it to their span.
   const int32_t draw = rng::next16(s.rng);
   bool any = false;
   for (uint8_t slot = 0; slot < move_slots(); ++slot)
   {
      if (!move_usable(s, i, slot))
      {
         continue;
      }
      any = true;
      AiOption o{};
      o.move = b.moves[slot];
      o.slot = slot;
      const MoveData& m = data().moves[o.move];
      const uint8_t choices = target_choices(s, i, o.move);
      // Positions the move would hit for each choice: a chosen target, or
      // everything it hits on its own (spread, random foe, self, field).
      uint8_t spread_mask = 0;
      if (choices == 0)
      {
         if (m.target == Target::AllFoes || m.target == Target::RandomFoe)
         {
            spread_mask = static_cast<uint8_t>(occupied(s, i, true) & reach(s, b.position));
         }
         else if (m.target == Target::AllOthers)
         {
            spread_mask = static_cast<uint8_t>((occupied(s, i, true) | occupied(s, i, false)) & reach(s, b.position));
         }
      }
      const uint8_t options = choices != 0 ? choices : static_cast<uint8_t>(bit(b.position));
      for (uint8_t p = 0; p < format(s).positions; ++p)
      {
         if (((options >> p) & 1) == 0)
         {
            continue;
         }
         o.hits = choices != 0 ? static_cast<uint8_t>(bit(p)) : spread_mask;
         estimate(s, i, o, draw);
         int32_t score = 0;
         if (!score_option(s, i, o, rules, score))
         {
            continue;
         }
         if (score > best_score)
         {
            best_score = score;
            best = Action{ActionKind::Move, slot, choices != 0 ? p : kBench, 0};
         }
      }
   }
   if (any && wants_switch(s, i, best_score))
   {
      const uint8_t to = ai_replacement(s, i);
      if (to != kBench)
      {
         return Action{ActionKind::Switch, 0, kBench, to};
      }
   }
   // G3: the item-use rules, after the switching rules
   const Action item = wants_item(s, i, best_score);
   if (item.kind == ActionKind::Item)
   {
      return item;
   }
   if (!any)
   {
      return Action{ActionKind::Move, kFallbackSlot, kBench, 0};
   }
   if (best_score < 0)
   {   // every move looked useless: use one anyway
      for (uint8_t slot = 0; slot < move_slots(); ++slot)
      {
         if (move_usable(s, i, slot))
         {
            return Action{ActionKind::Move, slot, kBench, 0};
         }
      }
   }
   return best;
}

}   // namespace capsule::battle::detail
