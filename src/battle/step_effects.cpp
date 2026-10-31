/// @file
/// Step effects, the battle side: the steps hook on each member, the damage rule, the messages and the script (see
/// capsule/battle/step_effects.h). Like encounter_lead.cpp it needs the script and overworld modules: the engine
/// library and the tests that list it link them. The modules share internal.h.
#include "capsule/battle/step_effects.h"

#include "capsule/battle/effect.h"
#include "capsule/battle/field_context.h"
#include "capsule/combine/eggs.h"
#include "capsule/combine/nursery.h"
#include "capsule/field/field.h"
#include "capsule/overworld/overworld.h"
#include "capsule/script/step_effects.h"

#include "internal.h"

namespace capsule::battle
{

using namespace detail;

namespace
{

/// The hit points the survive rule leaves.
constexpr uint16_t kSurviveHp = 1;

/// What a StepScope attached (one at a time).
struct Attached
{
   State* state;             ///< The State lent for the field contexts.
   monster::Party* party;    ///< The player's party.
   rng::Stream* rng;         ///< The handlers' stream.
};

/// The run under way.
struct Run
{
   uint8_t effect;         ///< The effect running, or kNone8 when none is.
   uint8_t next;           ///< The next member's party slot.
   uint8_t worst_member;   ///< The first member (party order) with the worst outcome so far.
   StepOutcome worst;      ///< The worst outcome so far.
   bool after_done;        ///< B2: the run's nursery check and egg cycle ran (once, before its script).
};

Attached g_attached = {nullptr, nullptr, nullptr};   ///< The attached scope's State, party and stream.
Run g_run = {kNone8, 0, kBench, StepOutcome::None, false};   ///< The run under way.
uint16_t g_hatched = 0;   ///< B2: bit per party slot whose egg hatched and whose hatch script has not started.
static_assert(monster::kMaxPartySize <= 16, "g_hatched holds a bit per party slot");
StepEvent g_last = {kNone8, kBench, StepOutcome::None, 0, 0, 0};   ///< The last member's part.

/// A record's species index.
/// @param rec  the record
/// @return its species, or kNone16
uint16_t species_of(const monster::Record& rec)
{
   return find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
}

/// The owner of a record's status (as from_record() reads it: the role's low bits; the counter sits above them).
/// @param rec  the record
/// @return its owner, or kNone16
uint16_t status_owner(const monster::Record& rec)
{
   const uint32_t status_mask = (1u << status_id_bits()) - 1;
   const uint32_t status = monster::get(rec, monster::Role::PrimaryStatus) & status_mask;
   return status != 0 && status < data().status_count ? data().statuses[status].owner : kNone16;
}

/// The owner of a record's passive (as from_record() picks it).
/// @param rec      the record
/// @param species  its species index
/// @return its owner, or kNone16
uint16_t passive_owner(const monster::Record& rec, uint16_t species)
{
   const uint32_t selector = record_value(rec, monster::id_of(monster::Role::PassiveSelector));
   const uint8_t passive = passive_for(data().species[species],
                                       static_cast<uint8_t>(selector & data().passive_selector_mask));
   return passive < data().passive_count ? data().passives[passive].owner : kNone16;
}

/// The owner of a record's held item.
/// @param rec  the record
/// @return its owner, or kNone16
uint16_t item_owner(const monster::Record& rec)
{
   const uint32_t item = monster::get(rec, monster::Role::HeldItem);
   return item != 0 && item < data().item_count ? data().items[item].owner : kNone16;
}

/// Calls @p f with every owner whose steps handlers run for a member: the game's global owners, then its status,
/// its passive, its held item (only owners with a handler for the hook).
/// @param rec      the member's record
/// @param species  its species index
/// @param f        called with each owner index
template <typename F>
void for_each_step_owner(const monster::Record& rec, uint16_t species, F&& f)
{
   const effect::Bank& bank = data().effects;
   constexpr effect::Hook kHook = effect::Hook::Steps;
   for (uint16_t k = 0; k < bank.owner_count; ++k)
   {
      if (bank.owners[k].kind == effect::OwnerKind::Global && effect::has_hook(bank.owners[k], kHook))
      {
         f(k);
      }
   }
   const uint16_t owners[] = {status_owner(rec), passive_owner(rec, species), item_owner(rec)};
   for (const uint16_t owner : owners)
   {
      const effect::Owner* o = owner != kNone16 ? owner_at(owner) : nullptr;
      if (o != nullptr && effect::has_hook(*o, kHook))
      {
         f(owner);
      }
   }
}

/// How bad an outcome is (a run's worst outcome is the one its script sees).
/// @param o  the outcome
/// @return its rank (None lowest)
int rank_of(StepOutcome o)
{
   return static_cast<int>(o);
}

/// Runs one member's handlers for a step effect, then the effect's rule and message.
/// @param info    the effect's record
/// @param effect  its index
/// @param member  party slot
/// @param out     what it did
/// @return false when the member has no species, is an egg the effect skips or has no handler for the hook (nothing
///         ran)
bool run_member(const script::StepEffectInfo& info, uint8_t effect, uint8_t member, StepEvent& out)
{
   monster::Party& party = *g_attached.party;
   const uint16_t species = species_of(party.members[member]);
   if (species == kNone16)
   {
      return false;
   }
   if (combine::is_egg(party.members[member]) && !combine::step_allows_eggs(effect))
   {
      return false;   // C9: an egg's handlers run only for an effect that allows eggs (the egg cycle counts it anyway)
   }
   int handlers = 0;
   for_each_step_owner(party.members[member], species, [&handlers](uint16_t)
   {
      ++handlers;
   });
   State& s = *g_attached.state;
   if (handlers == 0 || !field_open(s, party, member))
   {
      return false;   // no handler: no field context to open
   }
   s.rng = *g_attached.rng;
   const uint16_t before = s.battlers[kFieldBattler].hp;
   for_each_step_owner(party.members[member], species, [&s, effect](uint16_t owner)
   {
      // a handler that fails, blocks or faults ends its own owner's run; the others still run
      field_hook(s, owner, effect::Hook::Steps, effect, kBench);
   });
   Battler& b = s.battlers[kFieldBattler];
   StepOutcome outcome = StepOutcome::None;
   if (before > 0 && b.hp == 0)
   {
      // the effect's damage rule: Survive keeps one hit point, Faint leaves it at 0
      const bool survives = info.damage == script::StepDamage::Survive;
      b.hp = survives ? kSurviveHp : 0;
      outcome = survives ? StepOutcome::Survived : StepOutcome::Fainted;
   }
   else if (b.hp < before)
   {
      outcome = StepOutcome::Hurt;
   }
   if (outcome != StepOutcome::None)
   {
      // the moments follow the outcomes after None (StepMoment::Hurt is outcome Hurt's)
      const uint8_t msg = info.messages[rank_of(outcome) - rank_of(StepOutcome::Hurt)];
      if (msg != script::kNoStepMessage && msg < data().message_count)
      {
         say(s, static_cast<Msg>(msg), kFieldBattler);
      }
   }
   out = StepEvent{effect, member, outcome, static_cast<uint8_t>(handlers), before, b.hp};
   field_close(s);
   *g_attached.rng = s.rng;
   return true;
}

/// Calls @p f with every owner whose hatch handlers run for a member (B2): the game's global owners, then its
/// passive, its held item.
/// @param rec      the member's record
/// @param species  its species index
/// @param f        called with each owner index
template <typename F>
void for_each_hatch_owner(const monster::Record& rec, uint16_t species, F&& f)
{
   const effect::Bank& bank = data().effects;
   constexpr effect::Hook kHook = effect::Hook::Hatch;
   for (uint16_t k = 0; k < bank.owner_count; ++k)
   {
      if (bank.owners[k].kind == effect::OwnerKind::Global && effect::has_hook(bank.owners[k], kHook))
      {
         f(k);
      }
   }
   const uint16_t owners[] = {passive_owner(rec, species), item_owner(rec)};
   for (const uint16_t owner : owners)
   {
      const effect::Owner* o = owner != kNone16 ? owner_at(owner) : nullptr;
      if (o != nullptr && effect::has_hook(*o, kHook))
      {
         f(owner);
      }
   }
}

/// One egg cycle (B2): the steps each egg loses (the cycle's every, or the largest the members' hatch handlers
/// answer), then the eggs at 0 hatch and wait for their script.
/// @param info  the egg cycle's step effect
void egg_cycle(const script::StepEffectInfo& info)
{
   monster::Party& party = *g_attached.party;
   State& s = *g_attached.state;
   int32_t steps = info.every;
   const uint8_t n = party.count < monster::kMaxPartySize ? party.count : static_cast<uint8_t>(monster::kMaxPartySize);
   for (uint8_t k = 0; k < n; ++k)
   {
      const monster::Record& rec = party.members[k];
      const uint16_t species = species_of(rec);
      if (species == kNone16 || combine::is_egg(rec))
      {
         continue;
      }
      int handlers = 0;
      for_each_hatch_owner(rec, species, [&handlers](uint16_t)
      {
         ++handlers;
      });
      if (handlers == 0 || !field_open(s, party, k))
      {
         continue;
      }
      s.rng = *g_attached.rng;
      for_each_hatch_owner(rec, species, [&s, &steps, &info](uint16_t owner)
      {
         // each owner answers from the cycle's own steps; the largest answer counts
         int32_t v = info.every;
         field_hook_value(s, owner, effect::Hook::Hatch, v);
         steps = v > steps ? v : steps;
      });
      *g_attached.rng = s.rng;
      field_detach(s);   // a hatch handler changes no member
   }
   for (uint8_t k = 0; k < n; ++k)
   {
      monster::Record& rec = party.members[k];
      if (combine::count_down(rec, static_cast<uint32_t>(steps > 0 ? steps : 0)) && combine::hatch(rec))
      {
         g_hatched = static_cast<uint16_t>(g_hatched | 1u << k);
      }
   }
}

/// B2: what a run does after its members besides its script: the nursery's check, the egg cycle.
void after_run()
{
   const script::StepEffectInfo* info = script::step_effects::info(g_run.effect);
   if (info == nullptr)
   {
      return;
   }
   if (combine::kCombine.nursery.slots != 0 && g_run.effect == combine::kCombine.nursery.check)
   {
      combine::nursery::check(*g_attached.state, *g_attached.party, *g_attached.rng);
   }
   if (combine::eggs_on() && g_run.effect == combine::kCombine.eggs.step_effect)
   {
      egg_cycle(*info);
   }
}

/// B2: starts the hatch script of the next egg that hatched (its party slot in egg_member).
/// C9: egg_member is written only when the runner is idle (another egg's hatch script may still read it: two eggs
/// hatching in one cycle start their scripts one after the other, each with its own member).
/// @return false while the runner cannot start it yet
bool start_hatch_script()
{
   const uint16_t script = script::step_effects::hatch_script();
   if (script == script::kNoStepScript)
   {
      g_hatched = 0;   // nothing to show: the eggs hatched quietly
      return true;
   }
   if (!overworld::can_run_script())
   {
      return false;   // a script runs (perhaps the last egg's): its egg_member stays as it is
   }
   uint8_t member = 0;
   while ((g_hatched & 1u << member) == 0)
   {
      ++member;
   }
   script::step_effects::set_egg_member(member);
   if (!overworld::run_script(script, field::kNoObject))
   {
      return false;
   }
   g_hatched = static_cast<uint16_t>(g_hatched & ~(1u << member));
   return true;
}

/// Ends the run under way: its script (when it had an outcome and names one) in the overworld runner.
/// @return false while the runner cannot start it yet (the run stays under way)
bool finish_run()
{
   if (!g_run.after_done)
   {
      g_run.after_done = true;
      after_run();
   }
   const script::StepEffectInfo* info = script::step_effects::info(g_run.effect);
   if (info != nullptr && g_run.worst != StepOutcome::None && info->script != script::kNoStepScript)
   {
      if (!overworld::can_run_script())
      {
         return false;   // C9: a running script may read the outcome variables: they are written once it ended
      }
      script::step_effects::set_outcome(g_run.worst_member, static_cast<uint8_t>(g_run.worst));
      if (!overworld::run_script(info->script, field::kNoObject))
      {
         return false;
      }
   }
   g_run = Run{kNone8, 0, kBench, StepOutcome::None, false};
   return true;
}

} // namespace

StepScope::StepScope(State& s, monster::Party& party, rng::Stream& rng)
{
   g_attached = Attached{&s, &party, &rng};
   g_run = Run{kNone8, 0, kBench, StepOutcome::None, false};
   g_hatched = 0;
   script::step_effects::clear_due();
}

StepScope::~StepScope()
{
   g_attached = Attached{nullptr, nullptr, nullptr};
   g_run = Run{kNone8, 0, kBench, StepOutcome::None, false};
   g_hatched = 0;
}

bool step_next(StepEvent& out)
{
   if (g_attached.party == nullptr)
   {
      return false;
   }
   // B2: the steps walked since the last call count into the nursery's experience
   combine::nursery::walked(script::step_effects::take_walked());
   for (;;)
   {
      if (g_hatched != 0 && !start_hatch_script())
      {
         return false;   // the runner is busy: the hatch script waits
      }
      if (g_run.effect == kNone8)
      {
         uint8_t effect = 0;
         if (!script::step_effects::take_due(&effect))
         {
            return false;
         }
         g_run = Run{effect, 0, kBench, StepOutcome::None, false};
      }
      const script::StepEffectInfo* info = script::step_effects::info(g_run.effect);
      const monster::Party& party = *g_attached.party;
      const uint8_t n = party.count < monster::kMaxPartySize ? party.count
                                                             : static_cast<uint8_t>(monster::kMaxPartySize);
      while (info != nullptr && g_run.next < n)
      {
         const uint8_t member = g_run.next++;
         if (run_member(*info, g_run.effect, member, out))
         {
            // C8: strictly worse only: of the members with the worst outcome the first in party order is recorded
            if (rank_of(out.outcome) > rank_of(g_run.worst))
            {
               g_run.worst = out.outcome;
               g_run.worst_member = member;
            }
            g_last = out;
            return true;
         }
      }
      if (!finish_run())
      {
         return false;
      }
   }
}

bool step_busy()
{
   if (g_run.effect != kNone8 || g_hatched != 0)
   {
      return true;
   }
   for (uint8_t k = 0; k < script::step_effects::count(); ++k)
   {
      if (script::step_effects::due(k))
      {
         return true;
      }
   }
   return false;
}

const StepEvent& last_step()
{
   return g_last;
}

} // namespace capsule::battle
