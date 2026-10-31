/// @file
/// Growth (D-26 section 4, package G4): form views, rule lookup, the trigger context, training values and the
/// Change scope over a field context (see capsule/growth/growth.h). The battle side (changes to a battler, the
/// effect primitives) is src/battle/growth_battle.cpp.
#include "capsule/growth/growth.h"

#include "capsule/battle/data.h"
#include "capsule/battle/field_context.h"
#include "capsule/battle/ruleset.h"
#include "capsule/combine/eggs.h"
#include "capsule/core/clock.h"

namespace capsule::growth
{

namespace
{

/// A species' growth entry, when the tables have one.
/// @param species  species index
/// @return its entry, or nullptr
const SpeciesGrowth* growth_of(uint16_t species)
{
   return species < kGrowth.species_count ? &kGrowth.species[species] : nullptr;
}

/// Whether a trigger is in a rule's trigger bits.
/// @param triggers  bit per Trigger
/// @param t         the trigger
/// @return true when the rule can hold for it
constexpr bool on_trigger(uint8_t triggers, Trigger t)
{
   return ((triggers >> static_cast<unsigned>(t)) & 1u) != 0;
}

/// The sum of a battler's training values over the manifest's stats.
/// @param training  kMaxStats values
/// @return the sum
int32_t training_total(const uint8_t* training)
{
   int32_t total = 0;
   for (int st = 0; st < battle::kData.stat_count; ++st)
   {
      total += training[st];
   }
   return total;
}

/// A found rule's "nothing" value.
constexpr Found kNothing = {ChangeKind::None, kBaseForm, 0, 0, kNoSpecies, kNoSpecies, kNoRule};

} // namespace

const battle::SpeciesData& species_view(uint16_t species, uint8_t form)
{
   const SpeciesGrowth* g = growth_of(species);
   if (g != nullptr && form != kBaseForm && form <= g->form_count)
   {
      return kGrowth.forms[g->first_form + form - 1].species;
   }
   return battle::kData.species[species];
}

uint8_t form_count(uint16_t species)
{
   const SpeciesGrowth* g = growth_of(species);
   return g != nullptr ? g->form_count : 0;
}

uint8_t form_of_name(uint16_t species, uint8_t name)
{
   if (name == kBaseForm)
   {
      return kBaseForm;
   }
   const SpeciesGrowth* g = growth_of(species);
   for (uint8_t k = 0; g != nullptr && k < g->form_count; ++k)
   {
      if (kGrowth.forms[g->first_form + k].name == name)
      {
         return static_cast<uint8_t>(k + 1);
      }
   }
   return kNoForm;
}

uint8_t record_form(const monster::Record& rec)
{
   if (kGrowth.form_role == monster::kNoRole)
   {
      return kBaseForm;
   }
   const uint32_t v = battle::record_value(rec, kGrowth.form_role);
   return static_cast<uint8_t>(v <= static_cast<uint32_t>(kMaxForms) ? v : kBaseForm);
}

Context make_context(Trigger trigger, uint16_t item)
{
   Context ctx = {};
   ctx.trigger = trigger;
   // only the triggers that carry an item have one (the rules' analysis assumed so)
   ctx.item = trigger == Trigger::ItemUse || trigger == Trigger::Trade ? item : 0;
   for (uint8_t k = 0; k < kGrowth.context_count && k < kMaxContextValues; ++k)
   {
      const ContextValue& cv = kGrowth.contexts[k];
      const int32_t v = cv.source == ContextSource::Clock ? static_cast<int32_t>(clock::time_of_day())
                                                         : context_value(k);
      // a value outside the declared range would break the rules' analysis: it is clamped into it
      ctx.values[k] = v < cv.lo ? cv.lo : v > cv.hi ? cv.hi : v;
   }
   return ctx;
}

Found find(const monster::Record& rec, const Context& ctx)
{
   const uint16_t species = battle::find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
   const SpeciesGrowth* g = growth_of(species);
   if (species == battle::kNone16)
   {
      return kNothing;
   }
   uint16_t stats[battle::kMaxStats] = {};
   battle::record_stats(rec, stats);
   const WhenInputs in = {&rec, &ctx, stats, species};
   if (kGrowth.blocked_rule != kNoRule && rules::growth_when(kGrowth.blocked_rule, in))
   {
      return Found{ChangeKind::Blocked, kBaseForm, 0, 0, kNoSpecies, kNoSpecies, kGrowth.blocked_rule};
   }
   if (g == nullptr)
   {
      return kNothing;
   }
   for (int k = 0; k < g->transform_count; ++k)
   {
      const TransformRule& t = kGrowth.transforms[g->first_transform + k];
      if (on_trigger(t.triggers, ctx.trigger) && rules::growth_when(t.rule, in))
      {
         return Found{ChangeKind::Species, kBaseForm, t.flags, 0, t.into, t.also, t.rule};
      }
   }
   const uint8_t current = record_form(rec);
   for (int k = 0; k < g->form_count; ++k)
   {
      const FormData& f = kGrowth.forms[g->first_form + k];
      const auto form = static_cast<uint8_t>(k + 1);
      if (f.rule != kNoRule && form != current && on_trigger(f.triggers, ctx.trigger) &&
          rules::growth_when(f.rule, in))
      {
         return Found{ChangeKind::Form, form, 0, 0, kNoSpecies, kNoSpecies, f.rule};
      }
   }
   // back to its own form (a species' forms[] "base" entry)
   if (current != kBaseForm && g->base_rule != kNoRule && on_trigger(g->base_triggers, ctx.trigger) &&
       rules::growth_when(g->base_rule, in))
   {
      return Found{ChangeKind::Form, kBaseForm, 0, 0, kNoSpecies, kNoSpecies, g->base_rule};
   }
   return kNothing;
}

uint32_t levelled_members(const battle::State& s, const monster::Party& party)
{
   uint32_t out = 0;
   for (uint8_t i = 0; i < s.battler_count; ++i)
   {
      const battle::Battler& b = s.battlers[i];
      const bool mine = s.parties[b.party].records == &party && (b.flags & (1u << battle::BATTLER_FROM_RECORD)) != 0;
      if (mine && ((s.levelled >> i) & 1u) != 0)
      {
         out |= 1u << b.party_slot;
      }
   }
   return out;
}

int32_t add_training(uint8_t* training, uint8_t stat, int32_t delta)
{
   const TrainingRules& tr = kGrowth.training;
   if (tr.enabled == 0 || stat >= battle::kData.stat_count)
   {
      return 0;
   }
   const int32_t value = training[stat];
   const int32_t room = tr.total_cap - training_total(training);
   // up: within the stat cap and what the total cap leaves; down: never below 0
   const int32_t up = tr.stat_cap - value < room ? tr.stat_cap - value : room;
   const int32_t applied = delta > up ? (up > 0 ? up : 0) : delta < -value ? -value : delta;
   training[stat] = static_cast<uint8_t>(value + applied);
   return applied;
}

bool gain_training(uint8_t* training, uint16_t foe, uint8_t level, GainPoint point, void* ctx)
{
   const TrainingRules& tr = kGrowth.training;
   if (tr.enabled == 0 || foe >= battle::kData.species_count)
   {
      return false;
   }
   bool changed = false;
   for (int st = 0; st < battle::kData.stat_count; ++st)
   {
      if (tr.yield_fields[st] == kNoField)
      {
         continue;
      }
      const int32_t yield = rules::species_field_of(foe, tr.yield_fields[st]);
      if (yield <= 0)
      {
         continue;   // no yield, no gain (whatever the formula would say of 0)
      }
      const int32_t total = training_total(training);
      if (total >= tr.total_cap)
      {
         break;   // the total cap reached: no stat gains more
      }
      int32_t gain = rules::train_gain(yield, training[st], total, level);
      if (point != nullptr && gain > 0)
      {
         gain = point(gain, st, ctx);   // C6: the gainer's owners (a held item that doubles it, ...)
      }
      if (gain > 0 && add_training(training, static_cast<uint8_t>(st), gain) != 0)
      {
         changed = true;
      }
   }
   return changed;
}

// -- Change ---------------------------------------------------------------------------------------------------

Change::Change(battle::State& s, monster::Party& p, uint8_t slot, const Context& ctx)
   : state(s), party(p), context(ctx), found(kNothing), from(battle::kNone16), member(slot), stage(Stage::None),
     open(battle::field_open(s, p, slot < p.count ? slot : battle::kBench))
{
   if (!open || slot >= p.count)
   {
      return;
   }
   from = s.battlers[battle::kFieldBattler].species;
   if (combine::refuses(p.members[slot], combine::EggUse::Transform))
   {
      return;   // C9: an egg does not transform unless the structure allows it (Stage::None)
   }
   found = find(p.members[slot], ctx);
   if (found.kind == ChangeKind::Species)
   {
      stage = Stage::Asking;
      battle::growth_event(s, battle::kFieldBattler, kTransformStart, from, found.into);
   }
   else if (found.kind == ChangeKind::Form)
   {
      stage = Stage::Asking;   // go() applies it (a form change is never stopped)
   }
}

Change::~Change()
{
   if (!open)
   {
      return;
   }
   const bool transformed = found.kind == ChangeKind::Species && (stage == Stage::Done || stage == Stage::Learning);
   if (stage == Stage::Learning)
   {
      battle::field_answer(state, battle::kKeepMoves);   // an offer left unanswered keeps every move
   }
   battle::field_close(state);   // pools, status, moves, species, form, training (and the roles that follow)
   if (!transformed || found.also == kNoSpecies || party.count >= monster::kLayout.party_size ||
       party.count >= monster::kMaxPartySize)
   {
      return;
   }
   // a rule's companion: a second creature of the rule's `also` species, a copy of the member without its held item,
   // nickname or status, every persisted pool full, in the first free party slot
   monster::Record extra = party.members[member];
   monster::set(extra, monster::Role::SpeciesKey, raw(battle::kData.species[found.also].key));
   monster::set(extra, monster::Role::HeldItem, 0);
   monster::set(extra, monster::Role::PrimaryStatus, 0);
   for (int n = 0; n < monster::count(monster::Role::Nickname); ++n)
   {
      monster::set(extra, monster::Role::Nickname, n, text::kCodeEnd);
   }
   if (kGrowth.form_role != monster::kNoRole && monster::has(kGrowth.form_role))
   {
      monster::set(extra, kGrowth.form_role, 0, kBaseForm);
   }
   const monster::RoleSet keep = {~kGrowth.rederive.engine, ~kGrowth.rederive.custom};
   battle::rederive(extra, &keep);
   battle::fill_resources(extra);
   party.members[party.count] = extra;
   battle::growth_event(state, battle::kFieldBattler, kTransformAlso, found.also, party.count);
   ++party.count;
}

bool Change::cancellable() const
{
   return stage == Stage::Asking && found.kind == ChangeKind::Species &&
          ((kGrowth.cancel_mask >> static_cast<unsigned>(context.trigger)) & 1u) != 0;
}

Stage Change::go()
{
   if (stage != Stage::Asking)
   {
      return stage;
   }
   if (found.kind == ChangeKind::Form)
   {
      battle::set_form(state, battle::kFieldBattler, found.form);
      stage = Stage::Done;
      return stage;
   }
   battle::transform_battler(state, battle::kFieldBattler, found.into);
   if ((found.flags & kTakesItem) != 0)
   {
      monster::set(party.members[member], monster::Role::HeldItem, 0);   // the trade item is used up
   }
   const bool offer = kGrowth.learn != 0 && battle::learn_level(state, battle::kFieldBattler);
   stage = offer ? Stage::Learning : Stage::Done;
   return stage;
}

bool Change::stop()
{
   if (!cancellable())
   {
      return false;
   }
   battle::growth_event(state, battle::kFieldBattler, kTransformStopped, from, found.into);
   stage = Stage::Stopped;
   return true;
}

Stage Change::answer_learn(uint8_t slot)
{
   if (stage != Stage::Learning)
   {
      return stage;
   }
   battle::field_answer(state, slot);   // answer_offer() goes on with the level's other moves
   stage = battle::field_offer_waiting(state) ? Stage::Learning : Stage::Done;
   return stage;
}

uint16_t Change::offered_move() const
{
   return stage == Stage::Learning && battle::field_offer_waiting(state) ? state.learn.move : battle::kNone16;
}

} // namespace capsule::growth
