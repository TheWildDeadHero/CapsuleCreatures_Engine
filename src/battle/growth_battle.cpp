/// @file
/// The battle side of growth (D-26 section 4, package G4; capsule/growth/growth.h): a battler's species view (its
/// form), its stats and resource maxima re-derived after a change, a transformation or form change applied to a
/// battler (the field context's member, or in battle the effect primitive `form`), training gained from a
/// defeated foe, the effect primitives `form`, `transform` and `train`, and what the battle writes back of them.
/// See capsule/battle/battle.h; the modules share internal.h.
#include "capsule/growth/growth.h"

#include "internal.h"

namespace capsule::battle
{

namespace detail
{

namespace
{

/// The party record a battler was built from.
/// @param s  battle (or field context)
/// @param i  battler
/// @return the record, or nullptr (member data, no records)
monster::Record* record_for(State& s, uint8_t i)
{
   if (i >= s.battler_count || !has_flag(s.battlers[i].flags, BATTLER_FROM_RECORD))
   {
      return nullptr;
   }
   PartyState& p = s.parties[s.battlers[i].party];
   return p.records != nullptr ? &p.records->members[s.battlers[i].party_slot] : nullptr;
}

/// The passive slot a battler's record names (its passive selector's low bits), 0 for member data.
/// @param s  battle
/// @param i  battler
/// @return passive slot
uint8_t passive_slot(State& s, uint8_t i)
{
   const monster::Record* rec = record_for(s, i);
   if (rec == nullptr)
   {
      return 0;
   }
   return static_cast<uint8_t>(record_value(*rec, monster::id_of(monster::Role::PassiveSelector)) &
                               data().passive_selector_mask);
}

/// Whether a battler's name is a species' name (no nickname replaced it).
/// @param name     the battler's name (kCodeEnd-terminated)
/// @param species  the species' name, or nullptr
/// @return true when they are the same codes
bool same_name(const text::Code* name, const text::Code* species)
{
   if (species == nullptr)
   {
      return false;
   }
   int n = 0;
   while (n < kNameCodes && name[n] == species[n] && name[n] != text::kCodeEnd)
   {
      ++n;
   }
   return n == kNameCodes || (name[n] == text::kCodeEnd && species[n] == text::kCodeEnd);
}

/// Copies a species' name into a battler's name.
/// @param dst  the battler's name (kNameCodes + 1 codes)
/// @param src  the species' name, or nullptr
void take_name(text::Code* dst, const text::Code* src)
{
   int n = 0;
   while (src != nullptr && n < kNameCodes && src[n] != text::kCodeEnd)
   {
      dst[n] = src[n];
      ++n;
   }
   dst[n] = text::kCodeEnd;
}

/// Sets a battler's types to its species view's (no added type).
/// @param b   the battler
/// @param sp  its species view
void take_types(Battler& b, const SpeciesData& sp)
{
   for (int k = 0; k < kMaxTypesPer; ++k)
   {
      b.types[k] = k < data().species_types ? sp.types[k] : kNone8;
   }
}

/// Whether two species views give the same passives.
/// @param a  one
/// @param b  the other
/// @return true when every passive slot matches
bool same_passives(const SpeciesData& a, const SpeciesData& b)
{
   for (int k = 0; k < kMaxPassiveSlots; ++k)
   {
      if (a.passives[k] != b.passives[k])
      {
         return false;
      }
   }
   return true;
}

/// The item an effect owner belongs to (an item's use_field logic running `transform`).
/// @param owner  effect owner index
/// @return item id, or 0 when the owner is no item's
uint16_t item_of_owner(uint16_t owner)
{
   for (uint16_t k = 1; k < data().item_count; ++k)
   {
      if (data().items[k].owner == owner)
      {
         return k;
      }
   }
   return 0;
}

} // namespace

/// A battler's species as its form gives it (growth::species_view()).
/// @param b  the battler
/// @return the species' data or its form's view
const SpeciesData& battler_species(const Battler& b)
{
   return growth::species_view(b.species, b.form);
}

/// Re-derives a battler's stats after its species, form or training values changed: the hit points gain (or lose)
/// what their maximum did, the other pools what theirs gained (as a level-up); an Hp event when they changed.
/// @param s  the State
/// @param i  battler
void restat(State& s, uint8_t i)
{
   Battler& b = s.battlers[i];
   const uint16_t old_max = max_hp(b);
   const uint16_t old_hp = b.hp;
   int32_t old_pools[kMaxPools] = {};
   pool_maxima(b, old_pools);
   compute_stats(battler_species(b), b.level, b.genes, b.training, b.awaken, b.trait, b.stats);
   if (usable(b))
   {
      // as a level-up: the hit points gain (or lose) what their maximum did, the other pools what theirs gained
      b.hp = static_cast<uint16_t>(clamp_to<int32_t>(b.hp + max_hp(b) - old_max, 1, max_hp(b)));
      grow_pools(s, i, old_pools);
   }
   else if (b.hp > max_hp(b))
   {
      b.hp = max_hp(b);
   }
   if (b.hp != old_hp)
   {
      emit(s, EventKind::Hp, i, kBench, 0, b.hp, old_hp);
   }
}

namespace
{

/// The battle's training point (C6): what a battler's owners make of one stat's gain.
struct GainCtx
{
   State& s;      ///< The battle.
   uint8_t i;     ///< The gaining battler.
};

/// One stat's gain through the training point (growth::GainPoint).
/// @param gain  the formula's gain
/// @param stat  the stat
/// @param ctx   a GainCtx
/// @return the gain
int32_t gain_point(int32_t gain, int stat, void* ctx)
{
   (void)stat;
   GainCtx& g = *static_cast<GainCtx*>(ctx);
   PipelineCtx c = pipeline_ctx(g.s, nullptr, g.i, kBench, kNone16, 0);
   return run_point(c, growth::kGrowth.training.gain_point, gain);
}

} // namespace

/// Training a battler gains from a defeated foe it shares the experience of (structure training; G4).
/// @param s    battle
/// @param i    the receiving battler (a record's: member data writes nothing back)
/// @param foe  the defeated foe's species
void gain_battler_training(State& s, uint8_t i, uint16_t foe)
{
   Battler& b = s.battlers[i];
   if (growth::kGrowth.training.enabled == 0 || !has_flag(b.flags, BATTLER_FROM_RECORD))
   {
      return;   // nothing to gain, or member data (nothing is written back)
   }
   // the stats follow at the next level-up (compute_stats() reads Battler::training), as the record's do at once
   GainCtx g{s, i};
   const bool pointed = growth::kGrowth.training.gain_point < data().point_count;
   growth::gain_training(b.training, foe, b.level, pointed ? gain_point : nullptr, &g);
}

/// Writes a battler's training values into its record (write_back(), field_close()).
/// @param b    the battler
/// @param rec  its record
void store_training(const Battler& b, monster::Record& rec)
{
   using monster::Role;
   if (!monster::has(Role::StatTraining))
   {
      return;
   }
   for (int st = 0; st < data().stat_count && st < monster::count(Role::StatTraining); ++st)
   {
      monster::set(rec, Role::StatTraining, st, b.training[st]);
   }
}

/// Writes what growth may change of a field context's member back into its record: its species, its form (a stored
/// form role), its training values; after a species or form change the stored derived roles that read them are
/// re-derived (GrowthData::rederive), the others keep their values.
/// @param b    the battler
/// @param rec  its record
void write_growth(const Battler& b, monster::Record& rec)
{
   using monster::Role;
   bool changed = false;
   const uint32_t key = raw(data().species[b.species].key);
   if (monster::get(rec, Role::SpeciesKey) != key)
   {
      monster::set(rec, Role::SpeciesKey, key);
      changed = true;
   }
   const monster::RoleId form_role = growth::kGrowth.form_role;
   if (form_role != monster::kNoRole && monster::has(form_role) && growth::record_form(rec) != b.form)
   {
      monster::set(rec, form_role, 0, b.form);
      changed = true;
   }
   store_training(b, rec);
   if (changed)
   {
      // the stored derived roles that read the species (or the form) follow it; every other role keeps its value
      const monster::RoleSet keep = {~growth::kGrowth.rederive.engine, ~growth::kGrowth.rederive.custom};
      rederive(rec, &keep);
   }
}

/// Effect `form WHO NAME` (Op::Form): the subject's form by name id.
/// @param s     the State
/// @param i     battler
/// @param name  form name id (growth FORM_<NAME>; 0: the species' own form)
/// @return pushes 1 when the form changed, else 0
effect::Outcome form_primitive(State& s, uint8_t i, uint8_t name)
{
   if (i >= s.battler_count)
   {
      return effect::Outcome{0, false, false, false};
   }
   const uint8_t form = growth::form_of_name(s.battlers[i].species, name);
   // a form the species does not have: nothing changes (pushes 0)
   const bool changed = form != growth::kNoForm && set_form(s, i, form);
   return effect::Outcome{changed ? 1 : 0, false, false, false};
}

/// Effect `transform WHO` (Op::Transform): the running item's item_use rules on a field context's member; a
/// transformation or form change, then the new species' moves of its level (learn_level()).
/// @param s      the State (a field context)
/// @param i      battler
/// @param owner  the running owner (an item's: its id is the context's item)
/// @return pushes 1 when something changed; fails the run when no rule holds, in battle or while an offer waits
effect::Outcome transform_primitive(State& s, uint8_t i, uint16_t owner)
{
   const effect::Outcome fail{0, false, false, true};
   monster::Record* rec = record_for(s, i);
   if (rec == nullptr || s.battlers[i].position != kBench || s.learn.battler != kBench)
   {
      return fail;   // out of battle only (a field context's member), with its record, no offer waiting
   }
   const growth::Context ctx = growth::make_context(growth::Trigger::ItemUse, item_of_owner(owner));
   const growth::Found f = growth::find(*rec, ctx);
   if (f.kind == growth::ChangeKind::Species)
   {
      transform_battler(s, i, f.into);
      if (growth::kGrowth.learn != 0)
      {
         learn_level(s, i);   // an offer waits in State::learn: the item's use asks for its answer
      }
      return effect::Outcome{1, false, false, false};
   }
   if (f.kind == growth::ChangeKind::Form && set_form(s, i, f.form))
   {
      return effect::Outcome{1, false, false, false};
   }
   return fail;   // no rule holds (or the blocked expression does): no effect, the item stays
}

/// Effect `train WHO STAT AMOUNT` (Op::Train): a training value within the structure's caps; stats follow.
/// @param s       the State
/// @param i       battler
/// @param stat    stat index
/// @param amount  the change
/// @return pushes the change made (0 without training)
effect::Outcome train_primitive(State& s, uint8_t i, uint8_t stat, int32_t amount)
{
   if (i >= s.battler_count || stat >= data().stat_count || growth::kGrowth.training.enabled == 0)
   {
      return effect::Outcome{0, false, false, false};
   }
   Battler& b = s.battlers[i];
   const int32_t applied = growth::add_training(b.training, stat, amount);
   if (applied != 0)
   {
      restat(s, i);   // the stats (and the resources' maxima) follow at once
   }
   return effect::Outcome{applied, false, false, false};
}

} // namespace detail

using namespace detail;

bool transform_battler(State& s, uint8_t i, uint16_t into)
{
   if (i >= s.battler_count || into >= data().species_count)
   {
      return false;
   }
   Battler& b = s.battlers[i];
   const uint16_t from = b.species;
   const SpeciesData& before = battler_species(b);
   const bool own_name = same_name(b.name, before.name);
   b.species = into;
   b.form = growth::kBaseForm;   // the new species' own form
   const SpeciesData& sp = data().species[into];
   take_types(b, sp);
   b.passive = passive_for(sp, passive_slot(s, i));
   if (own_name)
   {
      take_name(b.name, sp.name);   // a creature called by its species' name takes the new one
   }
   restat(s, i);
   emit(s, EventKind::Transform, i, kBench, growth::kTransformDone, from, into);
   return true;
}

bool set_form(State& s, uint8_t i, uint8_t form)
{
   if (i >= s.battler_count)
   {
      return false;
   }
   Battler& b = s.battlers[i];
   const SpeciesData& before = battler_species(b);
   const uint8_t old = b.form;
   if (form == old || form > growth::form_count(b.species))
   {
      return false;   // the same form, or one the species does not have
   }
   b.form = form;
   const SpeciesData& sp = battler_species(b);
   take_types(b, sp);
   if (!same_passives(sp, before))
   {
      b.passive = passive_for(sp, passive_slot(s, i));
   }
   restat(s, i);
   emit(s, EventKind::Transform, i, kBench, growth::kTransformForm, form, old);
   return true;
}

bool learn_level(State& s, uint8_t i)
{
   if (i >= s.battler_count)
   {
      return false;
   }
   return learn_level_moves(s, i, 0);
}

void growth_event(State& s, uint8_t i, uint8_t extra, uint16_t a, uint16_t b)
{
   emit(s, EventKind::Transform, i, kBench, extra, a, b);
}

} // namespace capsule::battle
