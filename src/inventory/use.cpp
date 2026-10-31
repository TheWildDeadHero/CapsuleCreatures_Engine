/// @file
/// Item use out of battle: the use_field hook in a field context (see capsule/inventory/use.h).
#include "capsule/inventory/use.h"

#include "capsule/battle/data.h"
#include "capsule/battle/field_context.h"
#include "capsule/combine/eggs.h"

namespace capsule::inventory
{

namespace
{

/// Whether a use leaves its item in the bag when it had an effect.
/// @param item  item id
/// @return true for an item that is not consumed (a key item never is)
bool kept(uint16_t item)
{
   return !has_flag(item, kItemConsumed) || has_flag(item, kItemKey);
}

/// The battler of the field context, when it has one.
/// @param u  the use
/// @return the member's battler, or nullptr
const battle::Battler* member_battler(const FieldUse& u)
{
   return u.open && u.member != kNoMember ? &u.state.battlers[battle::kFieldBattler] : nullptr;
}

} // namespace

FieldUse::FieldUse(battle::State& s, monster::Party& p, uint8_t target)
   : state(s), party(p), member(target), open(battle::field_open(s, p, target == kNoMember ? battle::kBench : target)),
     waiting_item(0), consumed(false)
{
}

FieldUse::~FieldUse()
{
   if (open)
   {
      battle::field_close(state);
   }
}

UseResult FieldUse::use(Bag& bag, uint16_t item, uint8_t slot)
{
   const ItemInfo* in = info(item);
   if (in == nullptr || !field_usable(item) || count(bag, item) == 0 || waiting_item != 0)
   {
      return UseResult::NotUsable;
   }
   // the target the item's field asks for
   const battle::Battler* b = member_battler(*this);
   const bool wants_member = in->target == Target::Member || in->target == Target::MoveSlot;
   if (!open || (wants_member && b == nullptr))
   {
      return UseResult::BadTarget;
   }
   // B2: an egg is no target unless the item allows eggs (battle.json items[].allow_eggs)
   if (member != kNoMember && member < party.count && combine::is_egg(party.members[member]) &&
       !has_flag(item, kItemEggs))
   {
      return UseResult::BadTarget;
   }
   uint16_t move = battle::kNone16;
   if (in->target == Target::MoveSlot)
   {
      if (slot >= battle::move_slots() || b->moves[slot] == battle::kNone16)
      {
         return UseResult::BadTarget;
      }
      move = b->moves[slot];   // the logic's context move: restore / raise reach its slot
   }
   const uint16_t species = b != nullptr ? b->species : battle::kNone16;
   // C6 (review4 finding 10): what the logic changed before it failed is put back
   battle::FieldSnapshot before{};
   battle::field_snapshot(state, before);
   const battle::effect::Status st =
      battle::field_run(state, battle::kData.items[item].owner, battle::effect::Hook::UseField, move);
   if (st != battle::effect::Status::Done)
   {
      battle::field_restore(state, before);
      return UseResult::NoEffect;   // fail (or a fault): no effect, the item stays
   }
   if (battle::field_offer_waiting(state))
   {
      waiting_item = item;   // consumed once the move is learned
      // G4: a transformation (`transform`) happened whatever the answer: the item is used up now
      consumed = b != nullptr && b->species != species;
      if (consumed && !kept(item))
      {
         remove(bag, item, 1);
      }
      return UseResult::LearnOffer;
   }
   if (!kept(item))
   {
      remove(bag, item, 1);
   }
   return UseResult::Used;
}

UseResult FieldUse::answer_learn(Bag& bag, uint8_t slot)
{
   if (waiting_item == 0)
   {
      return UseResult::NotUsable;
   }
   const uint16_t item = waiting_item;
   const bool used_up = consumed;
   waiting_item = 0;
   consumed = false;
   const bool learned = battle::field_answer(state, slot);
   if (used_up)
   {
      // G4: the transformation's offer: the item went with the transformation; the next offer of the level may wait
      if (battle::field_offer_waiting(state))
      {
         waiting_item = item;
         consumed = true;
         return UseResult::LearnOffer;
      }
      return UseResult::Used;
   }
   if (!learned)
   {
      return UseResult::NoEffect;   // kept every move: the item stays
   }
   if (!kept(item))
   {
      remove(bag, item, 1);
   }
   return UseResult::Used;
}

uint16_t FieldUse::offered_move() const
{
   return battle::field_offer_waiting(state) ? state.learn.move : battle::kNone16;
}

bool FieldUse::next_text(text::Code* out, int cap)
{
   battle::Event e{};
   while (battle::pop_event(state, e))
   {
      // texts only: the pools' and status' own events are what the party screen shows afterwards
      if (battle::format_text(state, e, out, cap))
      {
         return true;
      }
   }
   return false;
}

} // namespace capsule::inventory
