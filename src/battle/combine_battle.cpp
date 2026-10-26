/// @file
/// Combination, the battle side (D-28 section 1, package B1): the Inherit op of the `on combine` hook, which writes
/// a combination's result record (the field context's member) from its inputs (the context's other battlers,
/// capsule/battle/field_context.h field_add_party()), and the scope that marks a field context as a combination's.
/// B2: a nursery record's growth (grow_record(), level_with()). The modules share internal.h.
#include "capsule/battle/field_context.h"
#include "capsule/combine/nursery.h"

#include "internal.h"

namespace capsule::battle
{

namespace
{

/// The inputs of the combination whose hook runs (0: none runs: the Inherit op fails).
uint8_t g_inputs = 0;
/// Most elements of one role (a custom role's count is a byte: tools/battlec/layout.py MAX_CUSTOM_COUNT).
constexpr int kMaxElements = 255;

/// Copies every element of a role from one record to another.
/// @param to    the record written
/// @param from  the record read
/// @param role  the role
/// @return true when a value changed
bool copy_whole(monster::Record& to, const monster::Record& from, monster::RoleId role)
{
   bool changed = false;
   for (int e = 0; e < monster::count(role); ++e)
   {
      const uint32_t v = monster::get(from, role, e);
      changed = changed || monster::get(to, role, e) != v;
      monster::set(to, role, e, v);
   }
   return changed;
}

/// The record of a combination's input k (battler kFieldOther + k).
/// @param s  the context
/// @param k  input
/// @return its record, or nullptr
const monster::Record* input_record(State& s, int k)
{
   return detail::record_of(s, static_cast<uint8_t>(kFieldOther + k));
}

} // namespace

CombineScope::CombineScope(uint8_t inputs)
{
   detail::set_combine_inputs(inputs);
}

CombineScope::~CombineScope()
{
   detail::set_combine_inputs(0);
}

namespace detail
{

/// Sets how many battlers after the result are a combination's inputs (input.1 .. input.N; 0: no combination).
/// @param count  the inputs
void set_combine_inputs(uint8_t count)
{
   g_inputs = count;
}

/// Op::Inherit: an input's values onto the result's record role (copy, the better of several picks, a chance, added).
/// @param s  the lent State (the result is battler kFieldBattler)
/// @param a  the action (subject: the input, role, mode; operands)
/// @param i  the action's subject (copy / odds: the input it reads)
/// @return the outcome (failed outside a combination or for a role it cannot take)
effect::Outcome inherit_primitive(State& s, const effect::Action& a, uint8_t i)
{
   constexpr effect::Outcome kFailed{0, false, false, true};
   monster::Record* result = record_of(s, kFieldBattler);
   const monster::RoleId role{static_cast<uint8_t>(a.id)};
   if (g_inputs == 0 || result == nullptr || !monster::has(role))
   {
      return kFailed;   // not a combination, or a role the layout does not store
   }
   const auto op = static_cast<effect::InheritOp>(a.flags);
   switch (op)
   {
   case effect::InheritOp::Copy:
   case effect::InheritOp::Odds:
   {
      const monster::Record* from = i != kFieldBattler ? record_of(s, i) : nullptr;
      if (from == nullptr)
      {
         return kFailed;   // the subject is not one of the inputs
      }
      // Odds: with odds a in b (b <= 0: never), drawn from the context's stream
      const bool go = op == effect::InheritOp::Copy ||
                      (a.args[1] > 0 && a.args[0] > 0 &&
                       rng::chance(s.rng, static_cast<uint32_t>(a.args[0]), static_cast<uint32_t>(a.args[1])));
      return effect::Outcome{go && copy_whole(*result, *from, role) ? 1 : 0, false, false, false};
   }
   case effect::InheritOp::Pick:
   {
      // a distinct elements, each from a random input: a partial shuffle of the element indices
      const int n = clamp_to<int>(monster::count(role), 0, kMaxElements);
      uint8_t order[kMaxElements] = {};
      for (int e = 0; e < n; ++e)
      {
         order[e] = static_cast<uint8_t>(e);
      }
      const int take = clamp_to<int32_t>(a.args[0], 0, n);
      int changed = 0;
      for (int k = 0; k < take; ++k)
      {
         const int j = k + static_cast<int>(rng::below(s.rng, static_cast<uint32_t>(n - k)));
         const uint8_t e = order[j];
         order[j] = order[k];
         order[k] = e;
         const monster::Record* from = input_record(s, static_cast<int>(rng::below(s.rng, g_inputs)));
         if (from != nullptr)
         {
            const uint32_t v = monster::get(*from, role, e);
            changed += monster::get(*result, role, e) != v ? 1 : 0;
            monster::set(*result, role, e, v);
         }
      }
      return effect::Outcome{changed > 0 ? 1 : 0, false, false, false};
   }
   case effect::InheritOp::Add:
   {
      // a counter role (a "+" value): the role + a, within 0 and its maximum
      const auto top = static_cast<int64_t>(monster::max_value(role));
      const int64_t now = monster::get(*result, role, 0);
      const int64_t next = clamp_to<int64_t>(now + a.args[0], 0, top);
      monster::set(*result, role, 0, static_cast<uint32_t>(next));
      return effect::Outcome{next != now ? 1 : 0, false, false, false};
   }
   default:
      return kFailed;
   }
}

} // namespace detail

using namespace detail;

namespace
{

/// A record's experience with more added, within what its species' curve reaches at the top level.
/// @param rec     the record
/// @param d       its species
/// @param amount  experience added
/// @return the experience
uint32_t grown_exp(const monster::Record& rec, const SpeciesData& d, uint32_t amount)
{
   const uint32_t top = detail::exp_for(d, data().rules.level_max);
   const uint32_t now = monster::get(rec, monster::Role::Experience);
   const uint32_t room = top > now ? top - now : 0;
   return now + (amount < room ? amount : room);
}

/// Whether a record knows a move.
/// @param rec   the record
/// @param move  the move
/// @return true when a slot holds it
bool knows(const monster::Record& rec, uint16_t move)
{
   for (int slot = 0; slot < move_slots(); ++slot)
   {
      if (monster::get(rec, monster::Role::Moves, slot) == move)
      {
         return true;
      }
   }
   return false;
}

/// Learns a move into a free slot, else forgets the first move (the others move up, the new one goes last).
/// @param rec   the record
/// @param move  the move
void learn_pushing(monster::Record& rec, uint16_t move)
{
   const int slots = move_slots();
   for (int slot = 0; slot < slots; ++slot)
   {
      if (monster::get(rec, monster::Role::Moves, slot) >= data().move_count)
      {
         monster::set(rec, monster::Role::Moves, slot, move);
         return;
      }
   }
   for (int slot = 0; slot + 1 < slots; ++slot)
   {
      monster::set(rec, monster::Role::Moves, slot, monster::get(rec, monster::Role::Moves, slot + 1));
   }
   monster::set(rec, monster::Role::Moves, slots - 1, move);
}

} // namespace

uint8_t level_with(const monster::Record& rec, uint32_t amount)
{
   const uint32_t level = record_value(rec, monster::id_of(monster::Role::Level));
   const uint16_t sp = find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
   if (sp == kNone16)
   {
      return static_cast<uint8_t>(level);
   }
   const SpeciesData& d = data().species[sp];
   const uint32_t exp = grown_exp(rec, d, amount);
   int lv = static_cast<int>(level);
   while (lv < data().rules.level_max && detail::exp_for(d, lv + 1) <= exp)
   {
      ++lv;
   }
   return static_cast<uint8_t>(lv);
}

uint8_t grow_record(monster::Record& rec, uint32_t amount)
{
   const uint16_t sp = find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
   if (sp == kNone16)
   {
      return static_cast<uint8_t>(record_value(rec, monster::id_of(monster::Role::Level)));
   }
   const SpeciesData& d = data().species[sp];
   const uint32_t before = record_value(rec, monster::id_of(monster::Role::Level));
   const uint8_t after = level_with(rec, amount);
   monster::set(rec, monster::Role::Experience, grown_exp(rec, d, amount));
   monster::set(rec, monster::Role::Level, after);
   // the moves of the levels gained, in learnset order
   for (int j = 0; j < d.learn_count; ++j)
   {
      const LearnEntry& e = d.learnset[j];
      if (e.level > before && e.level <= after && e.move < data().move_count && !knows(rec, e.move))
      {
         learn_pushing(rec, e.move);
      }
   }
   fill_resources(rec);   // its stats follow the level; it comes back rested
   return after;
}

} // namespace capsule::battle
