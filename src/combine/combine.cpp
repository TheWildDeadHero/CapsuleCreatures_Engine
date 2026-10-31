/// @file
/// Combination (D-28 section 1, package B1): the expressions' inputs, the checks and result rules, the preview and
/// the Combination scope (see capsule/combine/combine.h). The `on combine` hook's Inherit op is the battle side's
/// (src/battle/combine_battle.cpp).
#include "capsule/combine/combine.h"

#include "capsule/battle/data.h"
#include "capsule/battle/effect.h"
#include "capsule/battle/field_context.h"
#include "capsule/battle/pipeline.h"
#include "capsule/battle/ruleset.h"
#include "capsule/combine/eggs.h"
#include "capsule/combine/nursery.h"
#include "capsule/core/saturate.h"
#include "capsule/growth/growth.h"

namespace capsule::combine
{

namespace
{

using monster::Role;

/// The highest value the odds and fee expressions pass their pipelines (ruleset_compiler COMBINE_VALUE_MAX).
constexpr int32_t kValueMax = (int32_t{1} << 24) - 1;
/// Index of the result in its party.
constexpr uint8_t kResult = 0;
/// A pick's passes: the members its where holds for, then its fallback over the whole group.
constexpr int kPickPasses = 2;

/// Which kinds of method an evaluation takes (B2).
enum class Mode : uint8_t
{
   Preview,   ///< Either kind (no draw).
   Combine,   ///< An immediate method over party and storage records.
   Nursery,   ///< The nursery method over the nursery's records.
};

/// What a result rule gave.
struct Picked
{
   bool matched;       ///< The rule matched.
   bool random;        ///< A random pick without a stream: group says which group.
   uint16_t species;   ///< The species (matched, not random).
   uint16_t group;     ///< The group of a random pick.
};

/// A record's species.
/// @param rec  the record
/// @return its species index, or kNoSpecies
uint16_t species_of(const monster::Record& rec)
{
   const uint16_t sp = battle::find_species(AssetKey{monster::get(rec, Role::SpeciesKey)});
   return sp == battle::kNone16 ? kNoSpecies : sp;
}

/// A record's level.
/// @param rec  the record
/// @return its level
int32_t level_of(const monster::Record& rec)
{
   return static_cast<int32_t>(battle::record_value(rec, monster::id_of(Role::Level)));
}

/// A record's move in a slot.
/// @param rec   the record
/// @param slot  move slot
/// @return the move, or monster::kNoMove
uint16_t move_of(const monster::Record& rec, int slot)
{
   const uint32_t v = monster::get(rec, Role::Moves, slot);
   return v >= battle::kData.move_count ? monster::kNoMove : static_cast<uint16_t>(v);
}

/// A species' value of a table's key.
/// @param t        the table
/// @param species  species index
/// @return its field value or its first type
int32_t key_of(const TableData& t, uint16_t species)
{
   if (t.key == KeyKind::Type)
   {
      return battle::kData.species[species].types[0];
   }
   return rules::species_field_of(static_cast<int32_t>(species), t.field);
}

/// Fills the expressions' inputs.
/// @param in       written
/// @param recs     the inputs
/// @param count    how many
/// @param ctx      the context values (kept: in.contexts points at them)
void fill(ValueInputs& in, const monster::Record* const* recs, uint8_t count, const growth::Context& ctx)
{
   in = ValueInputs{};
   in.count = count;
   in.contexts = ctx.values;
   in.candidate = kNoSpecies;
   for (uint8_t k = 0; k < count && k < kMaxInputs; ++k)
   {
      in.recs[k] = recs[k];
      in.species[k] = species_of(*recs[k]);
   }
}

/// Whether an input meets its slot's requirement (a growth rule over its record, the script trigger).
/// @param rule  the requirement (kNoRequirement: none)
/// @param rec   the input
/// @param ctx   the context
/// @return true when it does
bool fits(uint16_t rule, const monster::Record& rec, const growth::Context& ctx)
{
   if (rule == kNoRequirement)
   {
      return true;
   }
   uint16_t stats[battle::kMaxStats] = {};
   battle::record_stats(rec, stats);
   const growth::WhenInputs w = {&rec, &ctx, stats, species_of(rec)};
   return rules::growth_when(rule, w);
}

/// Whether one matcher takes a species.
/// @param mt       the matcher
/// @param species  species index
/// @return true when it does
bool takes(const MatcherData& mt, uint16_t species)
{
   switch (mt.kind)
   {
   case MatcherKind::Species:
      return species == mt.value;
   case MatcherKind::Type:
      return battle::kData.species[species].types[0] == mt.value;
   default:
      return rules::species_field_of(static_cast<int32_t>(species), mt.field) == mt.value;
   }
}

/// Places inputs k onward on the recipe's free matchers (a multiset: any order; at most kMaxInputs deep).
/// @param first  the recipe's first matcher
/// @param n      its matchers (= the inputs)
/// @param in     the inputs
/// @param k      the next input
/// @param used   bit per matcher taken
/// @return true when every input took one
bool place_inputs(const MatcherData* first, int n, const ValueInputs& in, int k, uint32_t used)
{
   if (k == n)
   {
      return true;
   }
   for (int j = 0; j < n; ++j)
   {
      if (((used >> j) & 1u) == 0 && takes(first[j], in.species[k]) &&
          place_inputs(first, n, in, k + 1, used | 1u << j))
      {
         return true;
      }
   }
   return false;
}

/// A pick's order key of a species.
/// @param r        the rule
/// @param species  species index
/// @return the field it orders by, or the index
int32_t order_of(const RuleData& r, uint16_t species)
{
   return r.order == kNoField ? static_cast<int32_t>(species) : rules::species_field_of(species, r.order);
}

/// Whether a is before b in a pick's order (the order key, then the index).
/// @param r  the rule
/// @param a  species
/// @param b  species
/// @return true when a comes first
bool before(const RuleData& r, uint16_t a, uint16_t b)
{
   const int32_t ka = order_of(r, a);
   const int32_t kb = order_of(r, b);
   return ka != kb ? ka < kb : a < b;
}

/// A table rule's pick: the group's members (key value = the cell) for which where holds, the first / last by the
/// order or a random one; none: the fallback over the whole group.
/// @param r      the rule
/// @param t      its table
/// @param group  the cell's group
/// @param in     the inputs (the candidate is set while where runs)
/// @param rng    the stream, or nullptr (a preview: a random choice among several gives the group)
/// @return what it gave
Picked pick(const RuleData& r, const TableData& t, uint16_t group, ValueInputs& in, rng::Stream* rng)
{
   const Picked none{false, false, kNoSpecies, kNoGroup};
   const uint16_t n = battle::kData.species_count;
   // two passes at most: the members where holds for, then (none of them) the fallback over the whole group
   for (int pass = 0; pass < kPickPasses; ++pass)
   {
      const bool all = pass == 1;
      if (all && r.otherwise == PickElse::None)
      {
         return none;
      }
      const PickChoose choose = all ? (r.otherwise == PickElse::First ? PickChoose::First : PickChoose::Last)
                                    : r.choose;
      uint16_t best = kNoSpecies;
      uint16_t hits = 0;
      for (uint16_t sp = 0; sp < n; ++sp)
      {
         if (key_of(t, sp) != group)
         {
            continue;
         }
         in.candidate = sp;
         if (!all && r.expr != kNoExpr && rules::combine_value(r.expr, in) == 0)
         {
            continue;
         }
         ++hits;
         if (best == kNoSpecies || (choose == PickChoose::First && before(r, sp, best)) ||
             (choose == PickChoose::Last && before(r, best, sp)))
         {
            best = sp;
         }
      }
      in.candidate = kNoSpecies;
      if (hits == 0)
      {
         continue;
      }
      if (choose != PickChoose::Random || hits == 1)
      {
         return Picked{true, false, best, kNoGroup};
      }
      if (rng == nullptr)
      {
         return Picked{true, true, kNoSpecies, group};
      }
      // the k-th member (index order) of the ones where holds for
      uint16_t k = static_cast<uint16_t>(rng::below(*rng, hits));
      for (uint16_t sp = 0; sp < n; ++sp)
      {
         if (key_of(t, sp) != group)
         {
            continue;
         }
         in.candidate = sp;
         const bool ok = r.expr == kNoExpr || rules::combine_value(r.expr, in) != 0;
         in.candidate = kNoSpecies;
         if (ok && k-- == 0)
         {
            return Picked{true, false, sp, kNoGroup};
         }
      }
   }
   return none;
}

/// What one result rule gives.
/// @param r    the rule
/// @param in   the inputs
/// @param rng  the stream, or nullptr (a preview)
/// @return what it gave
Picked rule_result(const RuleData& r, ValueInputs& in, rng::Stream* rng)
{
   const Picked none{false, false, kNoSpecies, kNoGroup};
   switch (r.kind)
   {
   case RuleKind::Recipe:
      if (r.count != in.count || !place_inputs(&kCombine.matchers[r.first], r.count, in, 0, 0))
      {
         return none;
      }
      return Picked{true, false, r.result, kNoGroup};
   case RuleKind::Expression:
   {
      const int32_t v = rules::combine_value(r.expr, in);
      if (v < 1 || v > battle::kData.species_count)
      {
         return none;
      }
      return Picked{true, false, static_cast<uint16_t>(v - 1), kNoGroup};
   }
   default:
      break;
   }
   const TableData& t = kCombine.tables[r.first];
   if (t.first_input >= in.count || t.second_input >= in.count)
   {
      return none;
   }
   const int32_t a = key_of(t, in.species[t.first_input]);
   const int32_t b = key_of(t, in.species[t.second_input]);
   for (uint16_t c = 0; c < t.cell_count; ++c)
   {
      const CellData& cell = kCombine.cells[t.first_cell + c];
      if ((cell.a == a && cell.b == b) || (t.symmetric != 0 && cell.a == b && cell.b == a))
      {
         return pick(r, t, cell.value, in, rng);
      }
   }
   return none;
}

/// A pipeline context out of battle (no battlers; a preview's rolls fixed at 0).
/// @param rng  the stream, or nullptr
/// @return the context
battle::PipelineCtx pipeline_ctx(rng::Stream* rng)
{
   return battle::PipelineCtx{nullptr, rng, battle::kBench, battle::kBench, battle::kNone16,
                              rng == nullptr ? int16_t{0} : battle::kNoFixedRoll, 0, battle::kNone8, battle::kNone8,
                              battle::kNoDraw};
}

/// A method's value (the odds or fee expression) within what its pipeline takes.
/// @param expr  the expression
/// @param in    the inputs
/// @return the value
int32_t value_of(uint16_t expr, const ValueInputs& in)
{
   return clamp_to<int32_t>(rules::combine_value(expr, in), 0, kValueMax);
}

/// The fee of a result.
/// @param method  method index
/// @param m       the method
/// @param in      the inputs (its result set here)
/// @param species the result's species
/// @param level   its level
/// @return the fee (0 without a fee expression)
int32_t fee_for(uint8_t method, const MethodData& m, ValueInputs& in, uint16_t species, uint8_t level)
{
   if (m.fee == kNoExpr || species == kNoSpecies)
   {
      return 0;
   }
   in.result_species = static_cast<uint16_t>(species + 1);
   in.result_level = level;
   battle::PipelineCtx c = pipeline_ctx(nullptr);
   const battle::CombineFeeInputs fi{method, in.count, aggregate(in, Aggregate::LevelMin),
                                     aggregate(in, Aggregate::LevelMax), aggregate(in, Aggregate::LevelAvg), level,
                                     value_of(m.fee, in)};
   return rules::combine_fee(c, fi);
}

/// The highest fee over a random pick's candidates (C9: what a preview shows before the draw: the most it can cost).
/// @param method  method index
/// @param m       the method
/// @param r       the table rule whose pick is random
/// @param group   its group
/// @param in      the inputs (the candidate and the result change while it runs, and are cleared after)
/// @return the fee (0 without a fee expression)
int32_t highest_fee(uint8_t method, const MethodData& m, const RuleData& r, uint16_t group, ValueInputs& in)
{
   if (m.fee == kNoExpr || r.kind != RuleKind::Table)
   {
      return 0;
   }
   const TableData& t = kCombine.tables[r.first];
   const int32_t top = battle::kData.rules.level_max;
   int32_t best = 0;
   for (uint16_t sp = 0; sp < battle::kData.species_count; ++sp)
   {
      if (key_of(t, sp) != group)
      {
         continue;
      }
      // the candidates the pick's where holds for (the ones a draw can give)
      in.candidate = sp;
      const bool ok = r.expr == kNoExpr || rules::combine_value(r.expr, in) != 0;
      in.candidate = kNoSpecies;
      if (!ok)
      {
         continue;
      }
      in.result_species = static_cast<uint16_t>(sp + 1);
      const auto level = static_cast<uint8_t>(clamp_to<int32_t>(rules::combine_value(m.level, in), 1, top));
      const int32_t fee = fee_for(method, m, in, sp, level);
      best = fee > best ? fee : best;
   }
   in.result_species = 0;
   in.result_level = 0;
   return best;
}

/// Runs a method's checks and result rules.
/// @param method  method index
/// @param recs    the inputs
/// @param count   how many
/// @param rng     the stream (random picks draw from it), or nullptr (a preview)
/// @param mode    which kinds it takes (B2)
/// @return what it gives
Preview evaluate(uint8_t method, const monster::Record* const* recs, uint8_t count, rng::Stream* rng, Mode mode)
{
   Preview out{Status::BadInput, kNone, kNone, 0, kNoSpecies, kNoGroup, 0, 0};
   const MethodData* m = method_at(method);
   if (m == nullptr)
   {
      return out;
   }
   // B2: a nursery method runs in the nursery only, an immediate one outside it (a preview takes either)
   const bool nursery_kind = m->kind == MethodKind::Nursery;
   if ((mode == Mode::Combine && nursery_kind) || (mode == Mode::Nursery && !nursery_kind))
   {
      out.status = Status::NotImmediate;
      return out;
   }
   if (count < m->min_inputs || count > m->max_inputs || count > kMaxInputs)
   {
      out.status = Status::WrongCount;
      return out;
   }
   for (uint8_t k = 0; k < count; ++k)
   {
      if (recs[k] == nullptr || species_of(*recs[k]) == kNoSpecies)
      {
         return out;   // BadInput
      }
   }
   const growth::Context ctx = growth::make_context(growth::Trigger::Script);
   for (uint8_t k = 0; k < count; ++k)
   {
      // B2: an egg is unfit unless the method takes eggs
      const bool egg_refused = is_egg(*recs[k]) && (m->flags & kAllowEggs) == 0;
      if (egg_refused || !fits(kCombine.slots[m->first_slot + k].requirement, *recs[k], ctx))
      {
         out.status = Status::Unfit;
         out.slot = k;
         return out;
      }
   }
   ValueInputs in{};
   fill(in, recs, count, ctx);
   out.odds = 1;
   if (m->odds != kNoExpr)
   {
      battle::PipelineCtx c = pipeline_ctx(nullptr);
      const battle::CombineOddsInputs oi{method, count, aggregate(in, Aggregate::LevelMin),
                                         aggregate(in, Aggregate::LevelMax), aggregate(in, Aggregate::LevelAvg),
                                         aggregate(in, Aggregate::SameSpecies), value_of(m->odds, in)};
      out.odds = rules::combine_odds(c, oi);
   }
   if (out.odds <= 0)
   {
      out.status = Status::Incompatible;
      return out;
   }
   for (uint8_t j = 0; j < m->rule_count; ++j)
   {
      const RuleData& r = kCombine.rules[m->first_rule + j];
      if (r.when != kNoExpr && rules::combine_value(r.when, in) == 0)
      {
         continue;
      }
      const Picked got = rule_result(r, in, rng);
      if (!got.matched)
      {
         continue;
      }
      out.status = Status::Ok;
      out.rule = j;
      out.species = got.random ? kNoSpecies : got.species;
      out.group = got.random ? got.group : kNoGroup;
      const int32_t top = battle::kData.rules.level_max;
      // the level expression reads the result (0 while a random pick's species is not drawn)
      in.result_species = got.random ? uint16_t{0} : static_cast<uint16_t>(got.species + 1);
      out.level = static_cast<uint8_t>(clamp_to<int32_t>(rules::combine_value(m->level, in), 1, top));
      // C9: a random pick (a preview) shows the highest fee over its candidates
      out.fee = got.random ? highest_fee(method, *m, r, got.group, in)
                           : fee_for(method, *m, in, out.species, out.level);
      return out;
   }
   out.status = Status::Refused;
   return out;
}

/// Whether any input of a party knows a move.
/// @param inputs  the inputs
/// @param move    the move
/// @param every   true: every input must know it
/// @return true when they do
bool known(const monster::Party& inputs, uint16_t move, bool every)
{
   int found = 0;
   for (int k = 0; k < inputs.count; ++k)
   {
      bool has = false;
      for (int slot = 0; slot < battle::move_slots(); ++slot)
      {
         has = has || move_of(inputs.members[k], slot) == move;
      }
      found += has ? 1 : 0;
   }
   return every ? found == inputs.count && found > 0 : found > 0;
}

} // namespace

// -- what the generated expressions read --------------------------------------------------------------------

uint32_t input_role(const ValueInputs& in, int slot, monster::RoleId role, int element)
{
   if (slot < 0 || slot >= in.count || in.recs[slot] == nullptr)
   {
      return 0;
   }
   return battle::record_value(*in.recs[slot], role, element);
}

int32_t input_part(const ValueInputs& in, int slot, Part part)
{
   if (slot < 0 || slot >= in.count || in.species[slot] == kNoSpecies)
   {
      return 0;
   }
   const uint16_t sp = in.species[slot];
   switch (part)
   {
   case Part::Species:
      return sp + 1;
   case Part::Base:
      return (sp < kCombine.species_count ? kCombine.base[sp] : sp) + 1;
   case Part::Type:
      return battle::kData.species[sp].types[0];
   default:
      return 0;
   }
}

int32_t input_field(const ValueInputs& in, int slot, int field)
{
   if (slot < 0 || slot >= in.count || in.species[slot] == kNoSpecies)
   {
      return 0;
   }
   return rules::species_field_of(in.species[slot], field);
}

int32_t aggregate(const ValueInputs& in, Aggregate a)
{
   int32_t sum = 0;
   int32_t lo = 0;
   int32_t hi = 0;
   bool same = in.count > 0;
   for (int k = 0; k < in.count; ++k)
   {
      const int32_t lv = in.recs[k] != nullptr ? level_of(*in.recs[k]) : 0;
      sum += lv;
      lo = k == 0 || lv < lo ? lv : lo;
      hi = lv > hi ? lv : hi;
      same = same && in.species[k] == in.species[0];
   }
   switch (a)
   {
   case Aggregate::LevelSum:
      return sum;
   case Aggregate::LevelAvg:
      return in.count > 0 ? sum / in.count : 0;
   case Aggregate::LevelMin:
      return lo;
   case Aggregate::LevelMax:
      return hi;
   default:
      return same ? 1 : 0;
   }
}

int32_t result_part(const ValueInputs& in, Part part, int field)
{
   ValueInputs view = in;
   view.candidate = in.result_species > 0 ? static_cast<uint16_t>(in.result_species - 1) : kNoSpecies;
   return candidate_part(view, part, field);
}

int32_t candidate_part(const ValueInputs& in, Part part, int field)
{
   const uint16_t sp = in.candidate;
   if (sp == kNoSpecies || sp >= battle::kData.species_count)
   {
      return 0;
   }
   switch (part)
   {
   case Part::Species:
      return sp + 1;
   case Part::Base:
      return (sp < kCombine.species_count ? kCombine.base[sp] : sp) + 1;
   case Part::Type:
      return battle::kData.species[sp].types[0];
   default:
      return rules::species_field_of(sp, field);
   }
}

// -- the methods ----------------------------------------------------------------------------------------------

uint8_t method_count()
{
   return kCombine.method_count;
}

const MethodData* method_at(uint8_t method)
{
   return method < kCombine.method_count ? &kCombine.methods[method] : nullptr;
}

Preview preview(uint8_t method, const monster::Record* const* inputs, uint8_t count)
{
   return evaluate(method, inputs, count, nullptr, Mode::Preview);
}

int32_t fee_of(uint8_t method, const monster::Record* const* inputs, uint8_t count, uint16_t species, uint8_t level)
{
   const MethodData* m = method_at(method);
   if (m == nullptr || count > kMaxInputs)
   {
      return 0;
   }
   const growth::Context ctx = growth::make_context(growth::Trigger::Script);
   ValueInputs in{};
   fill(in, inputs, count, ctx);
   return fee_for(method, *m, in, species, level);
}

// -- Combination ----------------------------------------------------------------------------------------------

Combination::Combination(battle::State& s, monster::Party& party, storage::Storage* boxes, uint8_t method,
                         const InputRef* inputs, uint8_t count, rng::Stream& rng)
   : m_state(s), m_party(party), m_boxes(boxes), m_rng(rng), m_result_party{}, m_inputs{}, m_refs{},
     m_candidates{}, m_order{}, m_result{Status::BadInput, kNone, kNone, 0, kNoSpecies, kNoGroup, 0, 0},
     m_placed{From::Party, 0, 0}, m_method(method), m_count(0), m_candidate_count(0), m_chosen_count(0),
     m_stage(Stage::Refused)
{
   const MethodData* m = method_at(method);
   if (m == nullptr || inputs == nullptr || count > kMaxInputs || count > monster::kMaxPartySize)
   {
      return;
   }
   // the inputs' copies, each from where it is (one record given twice is refused; B2: nursery records only for
   // the nursery's method, which takes nothing else)
   bool from_nursery = false;
   for (uint8_t k = 0; k < count; ++k)
   {
      const InputRef& ref = inputs[k];
      for (uint8_t j = 0; j < k; ++j)
      {
         if (inputs[j].from == ref.from && inputs[j].index == ref.index)
         {
            return;
         }
      }
      const monster::Record* held = ref.from == From::Nursery && ref.index <= kNone ?
                                       nursery::record(static_cast<uint8_t>(ref.index)) : nullptr;
      bool ok = false;
      switch (ref.from)
      {
      case From::Party:
         ok = ref.index < party.count;
         break;
      case From::Storage:
         ok = boxes != nullptr && storage::read(*boxes, ref.index, m_inputs.members[k]);
         break;
      default:
         ok = held != nullptr && (k == 0 || from_nursery);
         from_nursery = true;
         break;
      }
      if (!ok || (from_nursery && ref.from != From::Nursery))
      {
         return;
      }
      if (ref.from == From::Party)
      {
         m_inputs.members[k] = party.members[ref.index];
      }
      else if (held != nullptr)
      {
         m_inputs.members[k] = *held;
      }
      m_refs[k] = ref;
   }
   m_inputs.count = count;
   m_count = count;
   const monster::Record* recs[kMaxInputs] = {};
   for (uint8_t k = 0; k < count; ++k)
   {
      recs[k] = &m_inputs.members[k];
   }
   m_result = evaluate(method, recs, count, &m_rng, from_nursery ? Mode::Nursery : Mode::Combine);
   if (m_result.status != Status::Ok)
   {
      refuse(m_result.status, m_result.slot);
      return;
   }
   // the result record at its level, then the game's `on combine` logic over it and the inputs
   monster::Record& result = m_result_party.members[kResult];
   if (!battle::make_monster(result, m_result.species, m_result.level, m_rng))
   {
      m_result.status = Status::BadInput;
      return;
   }
   m_result_party.count = 1;
   bool refused = false;
   if (!battle::field_open(m_state, m_result_party, kResult) || !battle::field_add_party(m_state, m_inputs))
   {
      // C9: the `on combine` logic must run: a context that cannot open refuses (it is never skipped)
      battle::field_detach(m_state);
      m_result.status = Status::BadInput;
      m_stage = Stage::Refused;
      return;
   }
   {
      const battle::CombineScope scope(count);
      m_state.rng = m_rng;
      const battle::effect::Bank& bank = battle::kData.effects;
      for (uint16_t k = 0; k < bank.owner_count && !refused; ++k)
      {
         const battle::effect::Owner& o = bank.owners[k];
         if (o.kind != battle::effect::OwnerKind::Global || !battle::effect::has_hook(o, battle::effect::Hook::Combine))
         {
            continue;
         }
         const battle::effect::Status st = battle::field_hook(m_state, k, battle::effect::Hook::Combine, method,
                                                              battle::kBench, count);
         refused = st == battle::effect::Status::Failed || st == battle::effect::Status::Blocked;
      }
      m_rng = m_state.rng;
      // the hook wrote the result's record (role writes, inherit); the battler's own changes are not kept
      battle::field_detach(m_state);
   }
   if (refused)
   {
      refuse(Status::Refused);
      return;
   }
   battle::fill_resources(result);   // its stats may follow what it inherited
   m_result.fee = fee_of(method, recs, count, m_result.species, m_result.level);
   // the moves: the sources' candidates, then the chooser
   gather_moves();
   m_stage = Stage::Ready;
   if (m->source_count == 0)
   {
      return;   // the result keeps its own moves
   }
   if (m_candidate_count <= move_limit() || m->chooser == Chooser::Ordered)
   {
      // C9: pushed moves are all learned in order (the last ones stay), else the first ones up to the limit
      const bool every = (m->flags & kPushMoves) != 0;
      for (uint8_t k = 0; k < m_candidate_count && (every || m_chosen_count < move_limit()); ++k)
      {
         m_order[m_chosen_count++] = k;
      }
      apply_moves();
      return;
   }
   if (m->chooser == Chooser::Random)
   {
      choose_random();
      return;
   }
   m_stage = Stage::Choosing;   // the player picks (choose() / done_choosing() / choose_random())
}

void Combination::refuse(Status status, uint8_t slot)
{
   m_result.status = status;
   m_result.slot = slot;
   m_stage = Stage::Refused;
   const Moment moment = status == Status::Unfit ? Moment::Unfit
                       : status == Status::Incompatible ? Moment::Incompatible : Moment::Refused;
   if (status != Status::Unfit && status != Status::Incompatible && status != Status::Refused)
   {
      return;   // a bad call has no message of the method's
   }
   // the message names the unfit input, else the first one
   const uint8_t about = slot != kNone && slot < m_count ? slot : 0;
   if (m_count > 0)
   {
      say(moment, m_inputs.members[about]);
   }
}

void Combination::say(Moment moment, const monster::Record& about)
{
   const MethodData* m = method_at(m_method);
   const uint8_t msg = m != nullptr ? m->messages[static_cast<int>(moment)] : kNoMessage;
   if (msg != kNoMessage)
   {
      battle::field_say(m_state, msg, about);
   }
}

uint16_t Combination::candidate(uint8_t k) const
{
   return k < m_candidate_count ? m_candidates[k] : monster::kNoMove;
}

bool Combination::chosen(uint8_t k) const
{
   for (uint8_t j = 0; j < m_chosen_count; ++j)
   {
      if (m_order[j] == k)
      {
         return true;
      }
   }
   return false;
}

uint8_t Combination::move_limit() const
{
   const MethodData* m = method_at(m_method);
   const int slots = battle::move_slots();
   return static_cast<uint8_t>(m != nullptr && m->move_limit < slots ? m->move_limit : slots);
}

bool Combination::choose(uint8_t k)
{
   if (m_stage != Stage::Choosing || k >= m_candidate_count || chosen(k))
   {
      return false;
   }
   m_order[m_chosen_count++] = k;
   if (m_chosen_count >= move_limit() || m_chosen_count >= m_candidate_count)
   {
      apply_moves();
      m_stage = Stage::Ready;
   }
   return true;
}

void Combination::done_choosing()
{
   if (m_stage == Stage::Choosing)
   {
      apply_moves();
      m_stage = Stage::Ready;
   }
}

void Combination::choose_random()
{
   if (m_stage != Stage::Choosing && m_stage != Stage::Ready)
   {
      return;
   }
   // the candidates not chosen yet, shuffled in place (a partial Fisher-Yates), until the limit
   uint8_t rest[kMaxCandidates] = {};
   uint8_t n = 0;
   for (uint8_t k = 0; k < m_candidate_count; ++k)
   {
      if (!chosen(k))
      {
         rest[n++] = k;
      }
   }
   for (uint8_t j = 0; j < n && m_chosen_count < move_limit(); ++j)
   {
      const auto pickk = static_cast<uint8_t>(j + rng::below(m_rng, static_cast<uint32_t>(n - j)));
      const uint8_t taken = rest[pickk];
      rest[pickk] = rest[j];
      rest[j] = taken;
      m_order[m_chosen_count++] = taken;
   }
   apply_moves();
   m_stage = Stage::Ready;
}

void Combination::gather_moves()
{
   const MethodData& m = *method_at(m_method);
   const uint16_t sp = m_result.species;
   m_candidate_count = 0;
   for (uint8_t s = 0; s < m.source_count && s < kMaxMoveSources; ++s)
   {
      const MoveSourceData& src = m.sources[s];
      if ((src.flags & kSourceOn) == 0)
      {
         continue;
      }
      uint8_t taken = 0;
      // adds one move unless it is there already, the source's limit or the list is full
      auto add = [this, &src, &taken](uint16_t move)
      {
         if (move == monster::kNoMove || m_candidate_count >= kMaxCandidates || (src.limit != 0 && taken >= src.limit))
         {
            return;
         }
         for (uint8_t k = 0; k < m_candidate_count; ++k)
         {
            if (m_candidates[k] == move)
            {
               return;
            }
         }
         m_candidates[m_candidate_count++] = move;
         ++taken;
      };
      if (src.from != kAnyInput)
      {
         gather_from(src, sp, add);   // C9: one input's moves, in its slot order
         continue;
      }
      switch (src.kind)
      {
      case MoveSource::Known:
         for (int k = 0; k < m_inputs.count; ++k)
         {
            for (int slot = 0; slot < battle::move_slots(); ++slot)
            {
               add(move_of(m_inputs.members[k], slot));
            }
         }
         break;
      case MoveSource::CombineMoves:
         for (uint16_t j = kCombine.move_first[sp]; sp < kCombine.species_count && j < kCombine.move_first[sp + 1];
              ++j)
         {
            const uint16_t mv = kCombine.moves[j];
            if ((src.flags & kSourceAll) != 0 || known(m_inputs, mv, false))
            {
               add(mv);
            }
         }
         break;
      case MoveSource::SharedLevelUp:
      {
         const battle::SpeciesData& d = battle::kData.species[sp];
         for (int j = 0; j < d.learn_count; ++j)
         {
            if (known(m_inputs, d.learnset[j].move, true))
            {
               add(d.learnset[j].move);
            }
         }
         break;
      }
      default:
         // the result's teachable moves (sorted, BattleData::teach_moves) an input knows
         if (battle::kData.teach_first != nullptr)
         {
            for (uint16_t j = battle::kData.teach_first[sp]; j < battle::kData.teach_first[sp + 1]; ++j)
            {
               if (known(m_inputs, battle::kData.teach_moves[j], false))
               {
                  add(battle::kData.teach_moves[j]);
               }
            }
         }
         break;
      }
   }
}

template <typename Add>
void Combination::gather_from(const MoveSourceData& src, uint16_t sp, Add& add) const
{
   if (src.from >= m_inputs.count)
   {
      return;
   }
   const monster::Record& input = m_inputs.members[src.from];
   for (int slot = 0; slot < battle::move_slots(); ++slot)
   {
      const uint16_t mv = move_of(input, slot);
      if (mv == monster::kNoMove)
      {
         continue;
      }
      // the input's move counts when the source's list has it (Known: every move of the input)
      bool listed = src.kind == MoveSource::Known;
      if (src.kind == MoveSource::CombineMoves && sp < kCombine.species_count)
      {
         for (uint16_t j = kCombine.move_first[sp]; j < kCombine.move_first[sp + 1] && !listed; ++j)
         {
            listed = kCombine.moves[j] == mv;
         }
      }
      else if (src.kind == MoveSource::SharedLevelUp)
      {
         const battle::SpeciesData& d = battle::kData.species[sp];
         for (int j = 0; j < d.learn_count && !listed; ++j)
         {
            listed = d.learnset[j].move == mv;
         }
      }
      else if (src.kind == MoveSource::TeachableKnown && battle::kData.teach_first != nullptr)
      {
         for (uint16_t j = battle::kData.teach_first[sp]; j < battle::kData.teach_first[sp + 1] && !listed; ++j)
         {
            listed = battle::kData.teach_moves[j] == mv;
         }
      }
      if (listed)
      {
         add(mv);
      }
   }
}

void Combination::apply_moves()
{
   const MethodData& m = *method_at(m_method);
   monster::Record& result = m_result_party.members[kResult];
   const int slots = battle::move_slots();
   uint16_t moves[monster::kMaxMoveSlots] = {};
   int n = 0;
   if ((m.flags & kPushMoves) != 0)
   {
      push_moves(moves, n);
   }
   for (uint8_t k = 0; k < m_chosen_count && n < slots && (m.flags & kPushMoves) == 0; ++k)
   {
      moves[n++] = m_candidates[m_order[k]];
   }
   // the result's own moves fill the slots left (those it does not have already)
   for (int slot = 0; slot < slots && n < slots && (m.flags & kFillMoves) != 0; ++slot)
   {
      const uint16_t own = move_of(result, slot);
      bool dup = own == monster::kNoMove;
      for (int k = 0; k < n; ++k)
      {
         dup = dup || moves[k] == own;
      }
      if (!dup)
      {
         moves[n++] = own;
      }
   }
   for (int slot = 0; slot < slots; ++slot)
   {
      monster::set(result, Role::Moves, slot, slot < n ? moves[slot] : monster::kNoMove);
   }
   battle::fill_resources(result);   // the new moves' pools full
}

void Combination::push_moves(uint16_t* moves, int& n) const
{
   const monster::Record& result = m_result_party.members[kResult];
   const int slots = battle::move_slots();
   for (int slot = 0; slot < slots; ++slot)
   {
      const uint16_t own = move_of(result, slot);
      if (own != monster::kNoMove)
      {
         moves[n++] = own;
      }
   }
   for (uint8_t k = 0; k < m_chosen_count; ++k)
   {
      const uint16_t mv = m_candidates[m_order[k]];
      bool dup = false;
      for (int j = 0; j < n; ++j)
      {
         dup = dup || moves[j] == mv;
      }
      if (dup || slots == 0)
      {
         continue;   // a move it knows already is not learned again
      }
      if (n >= slots)
      {
         // full: the first move goes, the others move up, the new one takes the last slot
         for (int j = 0; j + 1 < slots; ++j)
         {
            moves[j] = moves[j + 1];
         }
         n = slots - 1;
      }
      moves[n++] = mv;
   }
}

Status Combination::place()
{
   if (m_stage != Stage::Ready)
   {
      return Status::Refused;
   }
   const MethodData& m = *method_at(m_method);
   const bool consume = m.afterwards == Afterwards::Consumed;
   monster::Record& result = m_result_party.members[kResult];
   if (m.delivery == Delivery::Egg && !is_egg(result))
   {
      make_egg(result, m.cycle_steps);   // B2: it arrives as an egg
   }
   // where it goes: a consumed party input's slot (replaced at once: placed before anything leaves), else the
   // party's end, else the boxes
   uint8_t at = kNone;
   for (uint8_t k = 0; k < m_count && consume && at == kNone; ++k)
   {
      at = m_refs[k].from == From::Party ? static_cast<uint8_t>(m_refs[k].index) : kNone;
   }
   const int room = monster::kLayout.party_size < monster::kMaxPartySize ? monster::kLayout.party_size
                                                                        : monster::kMaxPartySize;
   InputRef placed{From::Party, 0, 0};
   if (at != kNone)
   {
      m_party.members[at] = result;
      placed.index = at;
   }
   else if (m_party.count < room)
   {
      m_party.members[m_party.count] = result;
      placed.index = m_party.count;
      ++m_party.count;
   }
   else
   {
      const uint16_t slot = m_boxes != nullptr ? storage::deposit(*m_boxes, result) : storage::kNoSlot;
      if (slot == storage::kNoSlot)
      {
         say(Moment::NoRoom, result);
         return Status::NoRoom;
      }
      placed = InputRef{From::Storage, 0, slot};
   }
   // then the consumed inputs leave (party slots from the highest down, so the lower ones keep their place)
   if (consume)
   {
      for (uint8_t k = 0; k < m_count; ++k)
      {
         if (m_refs[k].from == From::Storage)
         {
            storage::release(*m_boxes, m_refs[k].index);
         }
      }
      for (int idx = m_party.count - 1; idx >= 0; --idx)
      {
         bool gone = false;
         for (uint8_t k = 0; k < m_count; ++k)
         {
            gone = gone || (m_refs[k].from == From::Party && m_refs[k].index == idx && idx != at);
         }
         if (!gone)
         {
            continue;
         }
         for (int j = idx; j + 1 < m_party.count; ++j)
         {
            m_party.members[j] = m_party.members[j + 1];
         }
         --m_party.count;
         if (placed.from == From::Party && placed.index > idx)
         {
            --placed.index;   // the result moved down with the members after it
         }
      }
   }
   m_placed = placed;
   m_stage = Stage::Placed;
   say(Moment::Combined, result);
   return Status::Ok;
}

} // namespace capsule::combine
