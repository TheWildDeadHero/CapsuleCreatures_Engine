/// @file
/// Encounter leads: the lead rule, the lead's encounter hooks, the encounter pipelines and the creature made (see
/// capsule/battle/encounter_lead.h). The modules share internal.h.
#include "capsule/battle/encounter_lead.h"

#include "capsule/battle/data/field.h"
#include "capsule/battle/field_context.h"
#include "capsule/combine/eggs.h"
#include "capsule/growth/growth.h"

#include "internal.h"

namespace capsule::battle
{

using namespace detail;

namespace
{

/// What a LeadScope attached (one at a time).
struct Attached
{
   State* state;             ///< The State lent for the field contexts.
   monster::Party* party;    ///< The player's party.
};

Attached g_attached = {nullptr, nullptr};   ///< The attached scope's State and party.
EncounterTrace g_trace{};                   ///< The last roll's trace.
/// The last roll's plan and lead level (encounter_reroll picks again with them).
EncounterPlan g_last_plan{};
int32_t g_last_lead_level = 0;              ///< The last roll's lead level.

/// A plan that changes nothing.
/// @return the plan
EncounterPlan neutral_plan()
{
   EncounterPlan p{};
   p.rate_scale = kRateScaleOne;
   p.bias = EncounterBias::None;
   return p;
}

/// Makes a plan the target of the Encounter op while it lives (the hook's run; RAII).
class PlanScope
{
public:
   /// Sets the plan.
   /// @param p  the plan
   /// @param h  the hook running
   PlanScope(EncounterPlan& p, effect::Hook h)
   {
      set_encounter_plan(&p, h);
   }
   /// Clears it.
   ~PlanScope()
   {
      set_encounter_plan(nullptr, effect::Hook::Encounter);
   }
   PlanScope(const PlanScope&) = delete;              ///< Not copied.
   PlanScope& operator=(const PlanScope&) = delete;   ///< Not assigned.
};

/// The field context of one hook run on the lead (RAII: opened on the lent State with the roll's stream, closed
/// without a write-back; the stream goes back to the roll).
class LeadContext
{
public:
   /// Opens it.
   /// @param r     the roll's stream (the hooks draw from it)
   /// @param lead  the lead's party slot
   /// @param made  the creature made (encounter_made), or nullptr
   LeadContext(rng::Stream& r, uint8_t lead, monster::Party* made)
      : rng_(r), open_(field_open(*g_attached.state, *g_attached.party, lead))
   {
      if (open_ && made != nullptr)
      {
         open_ = field_add_other(*g_attached.state, *made);
      }
      g_attached.state->rng = r;
   }
   /// Closes it.
   ~LeadContext()
   {
      rng_ = g_attached.state->rng;
      field_detach(*g_attached.state);
   }
   LeadContext(const LeadContext&) = delete;              ///< Not copied.
   LeadContext& operator=(const LeadContext&) = delete;   ///< Not assigned.

   /// Whether it opened.
   /// @return true when the hooks can run
   bool open() const
   {
      return open_;
   }

private:
   rng::Stream& rng_;   ///< The roll's stream.
   bool open_;          ///< The context opened.
};

/// A record's species index.
/// @param rec  the record
/// @return its species, or kNone16
uint16_t species_of(const monster::Record& rec)
{
   return find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
}

/// The passive a record's battler has (as from_record() picks it).
/// @param rec      the record
/// @param species  its species index
/// @return the passive, or kNone8
uint8_t record_passive(const monster::Record& rec, uint16_t species)
{
   const uint32_t selector = record_value(rec, monster::id_of(monster::Role::PassiveSelector));
   return passive_for(data().species[species], static_cast<uint8_t>(selector & data().passive_selector_mask));
}

/// Calls @p f with the game's global owners that have a handler for an encounter hook.
/// @param h  the hook
/// @param f  called with each owner index; returns false to stop
/// @return false when @p f stopped
template <typename F>
bool for_each_global_owner(effect::Hook h, F&& f)
{
   const effect::Bank& bank = data().effects;
   for (uint16_t k = 0; k < bank.owner_count; ++k)
   {
      if (bank.owners[k].kind == effect::OwnerKind::Global && effect::has_hook(bank.owners[k], h) && !f(k))
      {
         return false;
      }
   }
   return true;
}

/// Calls @p f with a member's passive, then its held item, when they have a handler for an encounter hook.
/// @param rec  the member's record
/// @param h    the hook
/// @param f    called with each owner index; returns false to stop
/// @return false when @p f stopped
template <typename F>
bool for_each_member_owner(const monster::Record& rec, effect::Hook h, F&& f)
{
   const uint16_t species = species_of(rec);
   if (species == kNone16 || combine::is_egg(rec))
   {
      return true;   // C9: an egg (picked by a rule that takes eggs) has no passive or held item that reacts
   }
   const uint8_t passive = record_passive(rec, species);
   if (passive < data().passive_count)
   {
      const uint16_t owner = data().passives[passive].owner;
      const effect::Owner* o = owner_at(owner);
      if (o != nullptr && effect::has_hook(*o, h) && !f(owner))
      {
         return false;
      }
   }
   const uint32_t item = monster::get(rec, monster::Role::HeldItem);
   if (item != 0 && item < data().item_count)
   {
      const uint16_t owner = data().items[item].owner;
      const effect::Owner* o = owner_at(owner);
      if (o != nullptr && effect::has_hook(*o, h) && !f(owner))
      {
         return false;
      }
   }
   return true;
}

/// Runs one group of an encounter hook's handlers in a field context on one holder (opened only when the group has
/// a handler).
/// @param h       the hook
/// @param holder  the holder's party slot
/// @param each    calls its functor with each owner of the group (for_each_global_owner() or
///                for_each_member_owner() bound to the hook)
/// @param value   the hook's value
/// @param made    the creature made (encounter_made), or nullptr
/// @param plan    the plan its primitives change
/// @param r       the roll's stream
/// @return false when a handler said block (no encounter)
template <typename Each>
bool run_group(effect::Hook h, uint8_t holder, Each&& each, int32_t value, monster::Party* made, EncounterPlan& plan,
               rng::Stream& r)
{
   bool any = false;
   each([&any](uint16_t)
   {
      any = true;
      return false;
   });
   if (!any)
   {
      return true;   // no handler: no field context to open
   }
   LeadContext ctx(r, holder, made);
   if (!ctx.open())
   {
      return true;
   }
   PlanScope scope(plan, h);
   bool blocked = false;
   const uint8_t other = made != nullptr ? kFieldOther : kBench;
   each([&](uint16_t owner)
   {
      ++g_trace.handlers;
      // a handler that fails or faults ends its own owner's run; block ends the roll
      blocked = field_hook(*g_attached.state, owner, h, value, other) == effect::Status::Blocked;
      return !blocked;
   });
   return !blocked;
}

/// Runs an encounter hook: the game's global owners on the lead, then the passive and held item of the hooks'
/// member (C8: `lead.hook_when`, by default the lead itself: then one field context runs them all, as before).
/// @param h          the hook
/// @param lead       the lead's party slot (the global owners' holder)
/// @param hook_lead  the hooks' member's party slot (its passive and held item), or kBench: none
/// @param value      the hook's value
/// @param made       the creature made (encounter_made), or nullptr
/// @param plan       the plan its primitives change
/// @param r          the roll's stream
/// @return false when a handler said block (no encounter)
bool run_hook(effect::Hook h, uint8_t lead, uint8_t hook_lead, int32_t value, monster::Party* made,
              EncounterPlan& plan, rng::Stream& r)
{
   const monster::Party& party = *g_attached.party;
   if (hook_lead == lead)
   {
      const monster::Record& rec = party.members[lead];
      return run_group(h, lead, [&rec, h](auto&& f)
      {
         return for_each_global_owner(h, f) && for_each_member_owner(rec, h, f);
      }, value, made, plan, r);
   }
   if (!run_group(h, lead, [h](auto&& f) { return for_each_global_owner(h, f); }, value, made, plan, r))
   {
      return false;
   }
   if (hook_lead == kBench)
   {
      return true;
   }
   const monster::Record& rec = party.members[hook_lead];
   return run_group(h, hook_lead, [&rec, h](auto&& f) { return for_each_member_owner(rec, h, f); }, value, made,
                    plan, r);
}

/// Whether a species passes a plan's bias.
/// @param plan     the plan
/// @param species  battle species index
/// @return true when it does
bool matches(const EncounterPlan& plan, uint16_t species)
{
   if (species >= data().species_count)
   {
      return false;
   }
   if (plan.bias == EncounterBias::Type)
   {
      const SpeciesData& sp = data().species[species];
      for (uint8_t k = 0; k < data().species_types && k < kMaxTypesPer; ++k)
      {
         if (sp.types[k] == plan.bias_id)
         {
            return true;
         }
      }
      return false;
   }
   return rules::species_field_of(species, plan.bias_id) == plan.bias_value;
}

/// The pipeline context of the field pipelines (pure arithmetic and rolls from the roll's stream).
/// @param r     the roll's stream
/// @return the context
PipelineCtx field_pipeline(rng::Stream& r)
{
   return PipelineCtx{g_attached.state, &r, kBench, kBench, kNone16, kNoFixedRoll, 0, kNone8, kNone8, kNoDraw};
}

/// The pick of a roll whose trigger odds passed: the bias, the entry, the level (encounter_level), the skip level.
/// @param table       bank table index
/// @param method      method index
/// @param plan        the hooks' plan
/// @param lead_level  the lead's level (0 without a lead)
/// @param r           the roll's stream
/// @param out         the encounter (species, level, table, entry, method)
/// @return true when a creature at or above the skip level was picked (the trace says why not)
bool pick_with(uint16_t table, uint8_t method, const EncounterPlan& plan, int32_t lead_level, rng::Stream& r,
               encounter::Result* out)
{
   const encounter::Bank& b = encounter::bank();
   const encounter::Table& t = b.tables[table];
   // the bias: with its odds, the entries that match and weigh something under the current conditions;
   // when none does (e.g. the only match is condition-gated off) the pick stays unbiased
   const uint32_t active = encounter::conditions();
   encounter::KeepMask keep{};
   bool kept = false;
   if (plan.bias != EncounterBias::None && rng::chance(r, plan.bias_n, plan.bias_d))
   {
      for (uint16_t i = 0; i < t.entry_count && i < encounter::kMaxEntriesPerTable; ++i)
      {
         const encounter::Entry& candidate = b.entries[t.entry_first + i];
         if (matches(plan, candidate.species) && encounter::entry_weight(candidate, active) > 0)
         {
            encounter::keep(keep, i);
            kept = true;
         }
      }
   }
   uint16_t i = 0;
   if (!encounter::choose(table, active, kept ? &keep : nullptr, r, &i))
   {
      g_trace.end = EncounterEnd::NoEntry;
      return false;
   }
   const encounter::Entry& e = b.entries[t.entry_first + i];
   // the bounds the hooks moved, within 1..level_max
   const int32_t top = data().rules.level_max;
   const int32_t lo = clamp_to<int32_t>(e.min_level + plan.lo_add, 1, top);
   const int32_t hi = clamp_to<int32_t>(e.max_level + plan.hi_add, lo, top);
   PipelineCtx c = field_pipeline(r);
   const EncounterLevelInputs li{lo, hi, lead_level, plan.level_bias, method};
   const auto level = static_cast<uint8_t>(clamp_to<int32_t>(rules::encounter_level(c, li), 1, top));
   g_trace.species = e.species;
   g_trace.level = level;
   if (level < plan.skip_below)
   {
      g_trace.end = EncounterEnd::Skipped;
      return false;
   }
   out->species = e.species;
   out->level = level;
   out->method = method;
   out->table = table;
   out->entry = static_cast<uint16_t>(t.entry_first + i);
   g_trace.end = EncounterEnd::Picked;
   return true;
}

/// The lead rules' roll (encounter::RollRules).
/// @param table   bank table index
/// @param method  method index
/// @param r       the roll's stream
/// @param out     the encounter
/// @return true when an encounter starts
bool lead_roll(uint16_t table, uint8_t method, rng::Stream& r, encounter::Result* out)
{
   g_trace = EncounterTrace{};
   g_trace.species = kNone16;
   g_trace.table = table;
   g_trace.method = method;
   EncounterPlan plan = neutral_plan();
   const uint8_t lead = g_attached.party != nullptr ? lead_of(*g_attached.party) : kBench;
   g_trace.lead = lead;
   int32_t lead_level = 0;
   if (lead != kBench)
   {
      lead_level = static_cast<int32_t>(monster::get(g_attached.party->members[lead], monster::Role::Level));
      if (!run_hook(effect::Hook::Encounter, lead, hook_lead_of(*g_attached.party), method, nullptr, plan, r))
      {
         g_trace.end = EncounterEnd::Blocked;
         return false;
      }
   }
   g_trace.rate_scale = plan.rate_scale;
   g_trace.skip_below = plan.skip_below;
   g_last_plan = plan;
   g_last_lead_level = lead_level;
   // the trigger odds (the ruleset's encounter_rate: the table's own times the scale by default)
   const encounter::Table& t = encounter::bank().tables[table];
   PipelineCtx c = field_pipeline(r);
   const EncounterRateInputs in{t.rate_n, t.rate_m, plan.rate_scale, lead_level, method};
   const ChanceResult odds = rules::encounter_rate(c, in);
   // rng::chance() takes odds out of up to 65536 (a 16-bit draw's span)
   constexpr int32_t kSpan = int32_t{1} << rng::kDrawBits;
   const auto chance = static_cast<uint32_t>(clamp_to<int32_t>(odds.chance, 0, kSpan));
   const auto out_of = static_cast<uint32_t>(clamp_to<int32_t>(odds.out_of, 1, kSpan));
   g_trace.chance = chance;
   g_trace.out_of = out_of;
   if (odds.result == 0 && !rng::chance(r, chance, out_of))
   {
      g_trace.end = EncounterEnd::NoTrigger;
      return false;
   }
   return pick_with(table, method, plan, lead_level, r, out);
}

} // namespace

namespace
{

/// The first member a lead rule holds for.
/// @param party  the party
/// @param rule   a growth rule id, or kNoLeadRule: the first member that can battle
/// @return its party slot, or kBench
uint8_t first_member(const monster::Party& party, uint16_t rule)
{
   const uint8_t n = party.count < monster::kMaxPartySize ? party.count : static_cast<uint8_t>(monster::kMaxPartySize);
   const bool hp_stored = monster::has(monster::Role::CurrentHp);
   for (uint8_t k = 0; k < n; ++k)
   {
      const monster::Record& rec = party.members[k];
      const uint16_t species = species_of(rec);
      if (species == kNone16)
      {
         continue;
      }
      if (rule == kNoLeadRule)
      {
         // the first member that can battle (a layout without stored hit points: the first member; C9: an egg
         // the battle refuses never leads)
         if ((!hp_stored || monster::get(rec, monster::Role::CurrentHp) > 0) &&
             !combine::refuses(rec, combine::EggUse::Battle))
         {
            return k;
         }
         continue;
      }
      if (combine::refuses(rec, combine::EggUse::Lead))
      {
         continue;   // C9: a lead rule sees eggs only when the structure's lead.allow_eggs says so
      }
      // the structure's lead rule: a growth-style `when` over the member (the script trigger's context)
      const growth::Context ctx = growth::make_context(growth::Trigger::Script);
      uint16_t stats[kMaxStats] = {};
      record_stats(rec, stats);
      const growth::WhenInputs wi = {&rec, &ctx, stats, species};
      if (rules::growth_when(rule, wi))
      {
         return k;
      }
   }
   return kBench;
}

} // namespace

uint8_t lead_of(const monster::Party& party)
{
   return first_member(party, kFieldRules.lead_rule);
}

uint8_t hook_lead_of(const monster::Party& party)
{
   return kFieldRules.hook_lead_rule == kNoLeadRule ? lead_of(party) : first_member(party, kFieldRules.hook_lead_rule);
}

LeadScope::LeadScope(State& s, monster::Party& party) : previous_(encounter::rules())
{
   g_attached = Attached{&s, &party};
   encounter::set_rules(&lead_roll);
}

LeadScope::~LeadScope()
{
   encounter::set_rules(previous_);
   g_attached = Attached{nullptr, nullptr};
}

Made make_encounter(const encounter::Result& r, monster::Party& made, rng::Stream& rng)
{
   encounter::Result at = r;
   const uint8_t lead = g_attached.party != nullptr ? lead_of(*g_attached.party) : kBench;
   const uint8_t hook_lead = g_attached.party != nullptr ? hook_lead_of(*g_attached.party) : kBench;
   for (int pick = 0;; ++pick)
   {
      made.count = 1;
      if (!make_monster(made.members[0], at.species, at.level, rng))
      {
         made.count = 0;
         return Made::Skipped;
      }
      if (lead == kBench)
      {
         return Made::Battle;
      }
      EncounterPlan plan = neutral_plan();
      if (!run_hook(effect::Hook::EncounterMade, lead, hook_lead, at.level, &made, plan, rng))
      {
         g_trace.end = EncounterEnd::Blocked;
         made.count = 0;
         return Made::Skipped;
      }
      if (plan.reroll == 0 || pick >= kMaxEncounterRerolls)
      {
         return Made::Battle;
      }
      // another pick from the same table with the roll's plan (its bias and skip level apply again)
      ++g_trace.rerolls;
      if (!pick_with(at.table, at.method, g_last_plan, g_last_lead_level, rng, &at))
      {
         made.count = 0;
         return Made::Skipped;
      }
   }
}

const EncounterTrace& last_encounter()
{
   return g_trace;
}

} // namespace capsule::battle
