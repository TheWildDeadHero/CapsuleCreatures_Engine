/// @file
/// The field context: effect logic out of battle on a party member (see capsule/battle/field_context.h).
/// See capsule/battle/battle.h; the modules share internal.h.
#include "capsule/battle/field_context.h"

#include "internal.h"

namespace capsule::battle
{

using namespace detail;

namespace
{

/// The field context's party of the member.
constexpr uint8_t kFieldParty = 0;
/// The party of the other creature (F1: field_add_other()).
constexpr uint8_t kFieldOtherParty = 1;

/// Whether a State is an open field context with its member.
/// @param s  the State
/// @return true when battler 0 is the member and its record is attached
bool has_member(const State& s)
{
   return s.battler_count > kFieldBattler && s.parties[kFieldParty].records != nullptr;
}

} // namespace

bool field_open(State& s, monster::Party& party, uint8_t member)
{
   // the same empty state start() begins from (a field context has no format, sides or positions)
   s = State{};
   s.mv.move = kNone16;
   s.mv.chain_move = kNone16;
   s.mv.ask = kNone8;
   s.learn.battler = kBench;
   s.weather = kNone8;
   s.winning_side = kNoSide;
   for (uint8_t& at : s.at)
   {
      at = kBench;
   }
   PartyState& p = s.parties[kFieldParty];
   p.controller = Controller::Human;
   s.party_count = 1;
   if (member == kBench)
   {
      return true;   // an item with target none: logic without a holder
   }
   if (member >= party.count || member >= monster::kMaxPartySize)
   {
      return false;
   }
   Battler& b = s.battlers[kFieldBattler];
   if (!from_record(b, party.members[member]))
   {
      return false;
   }
   // out of battle the member's held item and passive do not react (their hooks stay out of the run); its status
   // stays (curing it runs the status's end hook)
   b.item = 0;
   b.passive = kNone8;
   b.party = kFieldParty;
   b.party_slot = member;
   p.records = &party;
   p.count = 1;
   p.first_battler = kFieldBattler;
   s.battler_count = 1;
   return true;
}

namespace
{

/// Runs one owner's handlers for a hook, in handler order, until one ends the run.
/// @param s      the context
/// @param owner  owner index
/// @param h      the hook
/// @param move   the context move, or kNone16
/// @param value  the hook's value
/// @param other  the other battler (the move's target too), or kBench
/// @param dealt  the context's dealt register (the combine hook's input count)
/// @return Done when every handler finished, else how the first one that did not ended
effect::Status run_owner(State& s, uint16_t owner, effect::Hook h, uint16_t move, int32_t& value, uint8_t other,
                         int32_t dealt)
{
   const effect::Owner* o = owner_at(owner);
   if (o == nullptr)
   {
      return effect::Status::Done;
   }
   const effect::Bank& bank = data().effects;
   effect::HostState host{s};
   const uint8_t holder = has_member(s) ? kFieldBattler : kBench;
   // without another battler the member is the target too (an item's logic aims at its holder)
   const uint8_t target = other != kBench ? other : holder;
   for (int k = 0; k < o->handler_count; ++k)
   {
      const effect::Handler& hn = bank.handlers[o->first_handler + k];
      if ((hn.flags & effect::kHandlerPoint) != 0 || hn.hook != static_cast<uint8_t>(h))
      {
         continue;
      }
      // holder = user = the member
      effect::Context ctx{value, dealt, move, owner, holder, other, holder, target};
      const effect::Status st = effect::invoke(s.vm, bank, host, hn.program, ctx);
      value = ctx.value;   // B2: a handler's modify carries to the next one (field_hook_value())
      if (st != effect::Status::Done)
      {
         return st;
      }
   }
   return effect::Status::Done;
}

} // namespace

effect::Status field_run(State& s, uint16_t owner, effect::Hook h, uint16_t move)
{
   int32_t value = 0;
   return run_owner(s, owner, h, move, value, kBench, 0);
}

effect::Status field_hook(State& s, uint16_t owner, effect::Hook h, int32_t value, uint8_t other, int32_t dealt)
{
   return run_owner(s, owner, h, kNone16, value, other, dealt);
}

effect::Status field_hook_value(State& s, uint16_t owner, effect::Hook h, int32_t& value)
{
   return run_owner(s, owner, h, kNone16, value, kBench, 0);
}

bool field_add_party(State& s, monster::Party& others)
{
   if (!has_member(s) || s.battler_count != kFieldOther)
   {
      return false;
   }
   const int n = others.count < monster::kMaxPartySize ? others.count : monster::kMaxPartySize;
   if (kFieldOther + n > kMaxBattlers)
   {
      return false;   // C9: every one of them must be there (none is left out silently)
   }
   for (int k = 0; k < n; ++k)
   {
      Battler& b = s.battlers[kFieldOther + k];
      if (!from_record(b, others.members[k]))
      {
         return false;
      }
      // C9: the input keeps its held item for reads (`input.X.item == NAME`), quiet: no hooks, points or tags
      b.flags = static_cast<uint8_t>(b.flags | bit(BATTLER_QUIET_ITEM));
      b.passive = kNone8;
      b.party = kFieldOtherParty;
      b.party_slot = static_cast<uint8_t>(k);
   }
   PartyState& p = s.parties[kFieldOtherParty];
   p.controller = Controller::Ai;
   p.records = &others;
   p.count = static_cast<uint8_t>(n);
   p.first_battler = kFieldOther;
   s.party_count = kFieldOtherParty + 1;
   s.battler_count = static_cast<uint8_t>(kFieldOther + n);
   return true;
}

void field_say(State& s, uint8_t message, const monster::Record& about)
{
   if (message >= data().message_count)
   {
      return;
   }
   // the record as battler 0 for its name only: no records attached, so nothing is written back
   Battler& b = s.battlers[kFieldBattler];
   if (!from_record(b, about))
   {
      return;
   }
   s.parties[kFieldParty].records = nullptr;
   if (s.battler_count == 0)
   {
      s.battler_count = 1;
   }
   say(s, static_cast<Msg>(message), kFieldBattler);
}

bool field_add_other(State& s, monster::Party& other)
{
   if (!has_member(s) || other.count == 0 || s.battler_count != kFieldOther)
   {
      return false;
   }
   Battler& b = s.battlers[kFieldOther];
   if (!from_record(b, other.members[0]))
   {
      return false;
   }
   b.item = 0;
   b.passive = kNone8;
   b.party = kFieldOtherParty;
   b.party_slot = 0;
   PartyState& p = s.parties[kFieldOtherParty];
   p.controller = Controller::Ai;
   p.records = &other;
   p.count = 1;
   p.first_battler = kFieldOther;
   s.party_count = kFieldOtherParty + 1;
   s.battler_count = kFieldOther + 1;
   return true;
}

void field_detach(State& s)
{
   for (uint8_t k = 0; k < s.party_count && k < kMaxParties; ++k)
   {
      s.parties[k].records = nullptr;
   }
}

void field_snapshot(const State& s, FieldSnapshot& out)
{
   out.battler = s.battlers[kFieldBattler];
   out.learn = s.learn;
   out.ev_count = s.ev_count;
   out.has_record = has_member(s);
   out.record = out.has_record ? s.parties[kFieldParty].records->members[out.battler.party_slot] : monster::Record{};
}

void field_restore(State& s, const FieldSnapshot& snap)
{
   s.battlers[kFieldBattler] = snap.battler;
   s.learn = snap.learn;
   if (s.ev_count > snap.ev_count)
   {
      s.ev_count = snap.ev_count;   // the run's texts go with it
   }
   if (snap.has_record && has_member(s))
   {
      s.parties[kFieldParty].records->members[snap.battler.party_slot] = snap.record;
   }
}

bool field_offer_waiting(const State& s)
{
   return s.learn.battler != kBench && s.learn.answered == 0;
}

bool field_answer(State& s, uint8_t slot)
{
   if (!field_offer_waiting(s))
   {
      return false;
   }
   const bool learned = slot < move_slots();
   s.learn.answer = learned ? slot : kKeepMoves;
   s.learn.answered = 1;
   answer_offer(s);   // learns it into the slot (or says it was not learned)
   return learned;
}

void field_close(State& s)
{
   if (has_member(s))
   {
      // the persisted pools, raises, status and moves; the record's held item, experience and level stay
      const Battler& b = s.battlers[kFieldBattler];
      monster::Record& rec = s.parties[kFieldParty].records->members[b.party_slot];
      store_resources(b, rec);
      const uint32_t counter_max = monster::max_value(monster::Role::PrimaryStatus) >> status_id_bits();
      const uint32_t counter = b.status_counter < counter_max ? b.status_counter : counter_max;
      monster::set(rec, monster::Role::PrimaryStatus, static_cast<uint32_t>(b.status) | counter << status_id_bits());
      for (int m = 0; m < move_slots(); ++m)
      {
         monster::set(rec, monster::Role::Moves, m, b.moves[m]);
      }
      write_growth(b, rec);   // G4: species, form, training (and the derived roles that follow the species)
   }
   // closed: no record to write to (the battler stays, so its queued texts still name it)
   s.parties[kFieldParty].records = nullptr;
}

} // namespace capsule::battle
