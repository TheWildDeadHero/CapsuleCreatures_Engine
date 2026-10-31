/// @file
/// The battle's Item action (G3, D-26 sections 2 and 3): a party uses an item from its bag (a human party:
/// Participant::bag) or from its party data's battle items (an AI party: PartyData::items, chosen by its profile's
/// item rules); the item's use_battle logic runs on the target its inventory::Target asks for, and an item that is
/// consumed leaves the bag once its logic had an effect. Its order bracket is the manifest's turn.brackets.item.
/// See capsule/battle/battle.h; the modules share internal.h.
#include "internal.h"

namespace capsule::battle::detail
{

namespace
{

/// The bag and use fields of an item the battle may use.
/// @param item  item id
/// @return its fields, or nullptr for no item or one not used in battle
const inventory::ItemInfo* battle_info(uint16_t item)
{
   const inventory::ItemInfo* in = inventory::info(item);
   return in != nullptr && (in->use == inventory::Use::Battle || in->use == inventory::Use::Both) ? in : nullptr;
}

/// The effect owner of an item with use_battle logic.
/// @param item  item id
/// @return the owner, or nullptr when it has no use_battle handler
const effect::Owner* use_owner(uint16_t item)
{
   const effect::Owner* o = item < data().item_count ? owner_at(data().items[item].owner) : nullptr;
   return o != nullptr && effect::has_hook(*o, effect::Hook::UseBattle) ? o : nullptr;
}

/// How many of an item a party has left (its bag, or its party data's battle items).
/// @param p     the party
/// @param item  item id
/// @return the count
uint32_t held(const PartyState& p, uint16_t item)
{
   if (p.bag != nullptr)
   {
      return inventory::count(*p.bag, item);
   }
   uint32_t n = 0;
   for (const uint16_t it : p.items)
   {
      n += it == item ? 1u : 0u;
   }
   return n;
}

/// How many of an item this turn's other choices of a party's battlers already use.
/// @param s      battle
/// @param party  party index
/// @param item   item id
/// @param pos    the choosing battler's position (its own choice does not count)
/// @return the count
uint32_t chosen(const State& s, uint8_t party, uint16_t item, uint8_t pos)
{
   uint32_t n = 0;
   for (uint8_t p = 0; p < format(s).positions; ++p)
   {
      const uint8_t i = s.at[p];
      const Action& a = s.actions[p];
      if (p != pos && i != kBench && s.battlers[i].party == party && a.kind == ActionKind::Item && a.item == item)
      {
         ++n;
      }
   }
   return n;
}

/// Takes one of an item from a party (its bag, or one entry of its battle items).
/// @param p     the party
/// @param item  item id
void take(PartyState& p, uint16_t item)
{
   if (p.bag != nullptr)
   {
      inventory::remove(*p.bag, item, 1);
      return;
   }
   for (uint16_t& it : p.items)
   {
      if (it == item)
      {
         it = 0;
         return;
      }
   }
}

/// The battler an Item action's logic holds (its holder), by the item's target.
/// @param s  battle
/// @param i  the user
/// @param a  the action
/// @param t  the item's target kind
/// @return the battler, or kBench when the target is gone
uint8_t item_holder(const State& s, uint8_t i, const Action& a, inventory::Target t)
{
   switch (t)
   {
   case inventory::Target::Member:
   case inventory::Target::MoveSlot:
      return a.battler < s.battler_count && s.battlers[a.battler].party == s.battlers[i].party ? a.battler : kBench;
   case inventory::Target::Battler:
      return a.target < format(s).positions ? s.at[a.target] : kBench;
   default:
      return i;   // nothing to choose: the user holds it
   }
}

} // namespace

/// Runs a battler's Item action: the UseItem event, the item's use_battle hook (holder = the target, other = the
/// user) and its consumption when the run did not fail (never a key item).
/// @param s  battle
/// @param i  the user
/// @param a  the action (item, target)
void item_action(State& s, uint8_t i, const Action& a)
{
   const inventory::ItemInfo* in = battle_info(a.item);
   const effect::Owner* o = use_owner(a.item);
   PartyState& party = s.parties[s.battlers[i].party];
   const uint8_t holder = in != nullptr ? item_holder(s, i, a, in->target) : kBench;
   if (in == nullptr || o == nullptr || held(party, a.item) == 0 || holder == kBench)
   {
      say(s, Msg::Failed, i);   // the item or its target is gone since it was chosen
      return;
   }
   emit(s, EventKind::UseItem, i, holder, 0, a.item);
   const uint16_t before = s.ev_count;
   // the context move of a move-slot target (restore / raise reach that slot)
   const uint16_t move = in->target == inventory::Target::MoveSlot && a.slot < move_slots()
                       ? s.battlers[holder].moves[a.slot] : kNone16;
   s.recruit.item = a.item;   // effect `recruit` runs only now
   s.recruit.user = i;
   const HookCtx c = run_own(s, data().items[a.item].owner, effect::Hook::UseBattle, holder, i, 0, move);
   s.recruit.item = kNone16;
   s.recruit.user = kBench;
   const bool failed = c.stopped == kRunFailed;
   if (failed && s.ev_count == before)
   {
      say(s, Msg::Failed, i);   // a run that failed without a word says the engine's message
   }
   // a use that had an effect takes one (a key item never goes)
   if (!failed && (in->flags & inventory::kItemConsumed) != 0 && (in->flags & inventory::kItemKey) == 0)
   {
      take(party, a.item);
   }
   if (s.outcome == Outcome::None)
   {
      check_faints(s, i);
   }
}

} // namespace capsule::battle::detail

namespace capsule::battle
{

using namespace detail;

bool item_usable(const State& s, uint8_t battler, uint16_t item)
{
   if (battler >= s.battler_count || battle_info(item) == nullptr || use_owner(item) == nullptr)
   {
      return false;
   }
   const Battler& b = s.battlers[battler];
   return held(s.parties[b.party], item) > chosen(s, b.party, item, b.position);
}

bool item_target_valid(const State& s, uint8_t battler, const Action& a)
{
   const inventory::ItemInfo* in = battle_info(a.item);
   if (in == nullptr || battler >= s.battler_count)
   {
      return false;
   }
   const uint8_t holder = item_holder(s, battler, a, in->target);
   // B2: an egg is no target unless the item allows eggs (the battle's own refusal keeps it on the bench)
   if (holder != kBench && has_flag(s.battlers[holder].flags, BATTLER_EGG) &&
       (in->flags & inventory::kItemEggs) == 0)
   {
      return false;
   }
   switch (in->target)
   {
   case inventory::Target::MoveSlot:
      return holder != kBench && a.slot < move_slots() && s.battlers[holder].moves[a.slot] != kNone16;
   case inventory::Target::Member:
   case inventory::Target::Battler:
      return holder != kBench;
   default:
      return true;
   }
}

} // namespace capsule::battle
