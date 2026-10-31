/// @file
/// Parties (D-20 A, B): presenters, sending battlers in and out, replacements, the end of the battle
/// and writing the results back into the records.
/// See capsule/battle/battle.h; the modules share internal.h.
#include "internal.h"

namespace capsule::battle::detail
{

/// The presenter in one of a party's presenter slots.
/// @param p     party
/// @param slot  0..kMaxPresenters - 1
/// @return presenter index, or kNone8
uint8_t presenter_in(const PartyState& p, int slot)
{
   if (p.data == nullptr)
   {
      return kNone8;
   }
   const uint8_t index = p.data->presenters[slot];
   return index < data().presenter_count ? index : kNone8;
}

/// Queues a presenter cue and keeps PartyState::shown up to date.
/// @param s     battle
/// @param t     party
/// @param slot  presenter slot in the party
/// @param cue   kCueEnter, kCueExit or kCueReturn
void presenter_cue(State& s, int t, int slot, uint8_t cue)
{
   PartyState& p = s.parties[t];
   const uint8_t shown = static_cast<uint8_t>(bit(static_cast<unsigned>(slot)));
   p.shown = static_cast<uint8_t>(cue == kCueExit ? p.shown & ~shown : p.shown | shown);
   emit(s, EventKind::Presenter, kBench, static_cast<uint8_t>(t), cue, presenter_in(p, slot),
        static_cast<uint16_t>(slot));
}

/// Queues one text of every presenter of a party that has it.
/// @param s     battle
/// @param t     party
/// @param what  kTextIntro, kTextDefeat or kTextVictory
/// @return true if a presenter spoke
bool presenters_say(State& s, int t, uint8_t what)
{
   bool spoke = false;
   for (int slot = 0; slot < kMaxPresenters; ++slot)
   {
      const uint8_t index = presenter_in(s.parties[t], slot);
      if (index == kNone8)
      {
         continue;
      }
      const PresenterData& pd = data().presenters[index];
      const text::Code* const texts[kPresenterTexts] = {pd.intro, pd.defeat, pd.victory};
      if (texts[what] != nullptr)
      {
         emit(s, EventKind::PresenterText, kBench, kBench, what, index, static_cast<uint16_t>(t));
         spoke = true;
      }
   }
   return spoke;
}

/// Brings back a party's returning presenters (PRESENTER_RETURNS) that are off screen.
/// @param s  battle
/// @param t  party
void presenters_return(State& s, int t)
{
   for (int slot = 0; slot < kMaxPresenters; ++slot)
   {
      const uint8_t index = presenter_in(s.parties[t], slot);
      if (index != kNone8 && has_flag(data().presenters[index].flags, PRESENTER_RETURNS) &&
          !has_flag(s.parties[t].shown, static_cast<unsigned>(slot)))
      {
         presenter_cue(s, t, slot, kCueReturn);
      }
   }
}

/// A party's intro (one unit of Phase::Intro): its presenters enter and
/// speak (or the party_challenge message), and those that leave after the
/// intro go.
/// @param s  battle
/// @param t  party
void party_intro(State& s, int t)
{
   const PartyState& p = s.parties[t];
   for (int slot = 0; slot < kMaxPresenters; ++slot)
   {
      if (presenter_in(p, slot) != kNone8)
      {
         presenter_cue(s, t, slot, kCueEnter);
      }
   }
   const bool spoke = presenters_say(s, t, kTextIntro);

   // Without a presenter intro a party given as data challenges with the message (an empty one says nothing).
   const text::Code* challenge = data().messages[static_cast<int>(Msg::PartyChallenge)];
   if (!spoke && p.data != nullptr && !has_flag(p.flags, PARTY_UNOWNED) && challenge != nullptr &&
       challenge[0] != text::kCodeEnd)
   {
      say(s, Msg::PartyChallenge, kBench, kBench, static_cast<uint16_t>(t));
   }
   for (int slot = 0; slot < kMaxPresenters; ++slot)
   {
      const uint8_t index = presenter_in(p, slot);
      if (index != kNone8 && has_flag(data().presenters[index].flags, PRESENTER_EXIT_AFTER_INTRO))
      {
         presenter_cue(s, t, slot, kCueExit);
      }
   }
}

/// The presenters still on screen leave (all but PRESENTER_STAYS), right
/// before their party's first send-out.
/// @param s  battle
/// @param t  party
void dismiss_presenters(State& s, int t)
{
   for (int slot = 0; slot < kMaxPresenters; ++slot)
   {
      const uint8_t index = presenter_in(s.parties[t], slot);
      if (index != kNone8 && has_flag(s.parties[t].shown, static_cast<unsigned>(slot)) &&
          !has_flag(data().presenters[index].flags, PRESENTER_STAYS))
      {
         presenter_cue(s, t, slot, kCueExit);
      }
   }
}

/// Whether a human-controlled party is on a side.
/// @param s     battle
/// @param side  side
/// @return true if one is
bool human_on_side(const State& s, uint8_t side)
{
   for (int t = 0; t < s.party_count; ++t)
   {
      if (s.parties[t].controller == Controller::Human && s.parties[t].side == side)
      {
         return true;
      }
   }
   return false;
}

/// Sends a battler into a position.
/// @param s      battle
/// @param i      battler
/// @param pos    position
/// @param hooks  run its switch-in hooks now (false while the battle opens:
///               they run once everyone is out)
void send_in(State& s, uint8_t i, uint8_t pos, bool hooks)
{
   const WeatherWatch watch(s, i);   // C6: a battler that suppresses the weather came
   Battler& b = s.battlers[i];
   b.position = pos;
   s.at[pos] = i;
   b.targeted_by = kNone16;   // C5: a record of this time on the field (a replacement after a faint too)
   b.flags = static_cast<uint8_t>(b.flags | bit(BATTLER_ANNOUNCED));
   dismiss_presenters(s, b.party);
   const PartyState& t = s.parties[b.party];
   if (has_flag(t.flags, PARTY_UNOWNED))
   {
      say(s, Msg::Appeared, i);
   }
   else if (t.data != nullptr && !human_on_side(s, t.side))
   {
      say(s, Msg::PartySends, i, kBench, static_cast<uint16_t>(b.party));
   }
   else
   {
      say(s, Msg::SendOut, i);
   }
   emit(s, EventKind::SwitchIn, i, pos);
   // Record who faced whom (experience).
   for (uint8_t p = 0; p < format(s).positions; ++p)
   {
      const uint8_t o = s.at[p];
      if (o == kBench || o == i || !alive_active(s, o) || side_of(s, o) == side_of(s, i))
      {
         continue;
      }
      s.battlers[o].faced |= bit(i);
      b.faced |= bit(o);
   }
   if (hooks)
   {
      switch_in_hooks(s, i);
   }
}

/// Takes a battler off the field (switch).
/// @param s  battle
/// @param i  battler
void withdraw(State& s, uint8_t i)
{
   const WeatherWatch watch(s, i);   // C6: ... or went
   Battler& b = s.battlers[i];
   const uint8_t pos = b.position;
   say(s, Msg::ComeBack, i);
   emit(s, EventKind::SwitchOut, i, pos);
   hook_simple(s, effect::Hook::SwitchOut, i, kBench);   // before the resets (a status's switch_out templates)
   for (int8_t& st : b.stages)
   {
      st = 0;
   }
   clear_volatiles(b, VOLATILE_SWITCH_OUT);
   end_bound(s, i);   // G1: what it started on others ends with it (a passing switch handed it on first)
   b.taken = kNoTaken;
   b.targeted_by = kNone16;   // E13b: a record of this time on the field
   b.lock_flags = 0;
   b.lock_move = kNone16;
   for (int k = 0; k < kMaxTypesPer; ++k)
   {
      // back to the species' types: added types end when it leaves the field
      b.types[k] = k < data().species_types ? battler_species(b).types[k] : kNone8;   // its form's (G4)
   }
   b.position = kBench;
   if (pos < kMaxPositions)
   {
      s.at[pos] = kBench;
   }
}

/// Whether a battler was chosen as a replacement and not sent in yet.
/// @param s  battle
/// @param i  battler
/// @return true if pending
bool pending_send(const State& s, uint8_t i)
{
   for (uint8_t p = 0; p < kMaxPositions; ++p)
   {
      const Action& a = s.actions[p];
      if (a.kind == ActionKind::Switch && a.slot == kPendingSend && a.battler == i)
      {
         return true;
      }
   }
   return false;
}

/// First bench battler of a party that can be sent in.
/// @param s  battle
/// @param t  party
/// @return battler, or kBench
uint8_t next_bench(const State& s, int t)
{
   const PartyState& tr = s.parties[t];
   for (int k = 0; k < tr.count; ++k)
   {
      const uint8_t i = static_cast<uint8_t>(tr.first_battler + k);
      if (s.battlers[i].position == kBench && usable(s.battlers[i]) && !pending_send(s, i))
      {
         return i;
      }
   }
   return kBench;
}

/// The party owning a position.
/// @param s    battle
/// @param pos  position
/// @return party index, or -1
int owner_of(const State& s, uint8_t pos)
{
   for (int t = 0; t < s.party_count; ++t)
   {
      if (((s.parties[t].positions >> pos) & 1) != 0)
      {
         return t;
      }
   }
   return -1;
}

/// Checks whether the battle is over; queues the end events.
/// @param s  battle
/// @return true if it ended
bool check_end(State& s)
{
   bool side_alive[kMaxSides] = {};
   for (int t = 0; t < s.party_count; ++t)
   {
      if (party_alive(s, t))
      {
         side_alive[s.parties[t].side] = true;
      }
      else if (s.parties[t].defeated == 0)
      {
         s.parties[t].defeated = 1;
      }
   }
   int alive = 0;
   uint8_t winner = kNoSide;
   for (uint8_t sd = 0; sd < format(s).sides; ++sd)
   {
      if (side_alive[sd])
      {
         ++alive;
         winner = sd;
      }
   }
   if (alive > 1)
   {
      return false;
   }
   const uint8_t ps = player_side(s);
   s.winning_side = alive == 1 ? winner : kNoSide;
   if (alive == 0)
   {
      s.outcome = Outcome::Stalemate;
   }
   else if (winner == ps)
   {
      s.outcome = Outcome::Victory;
      for (int t = 0; t < s.party_count; ++t)
      {
         const PartyState& tr = s.parties[t];
         if (tr.side == ps || tr.data == nullptr || tr.count == 0)
         {
            continue;
         }
         presenters_return(s, t);
         say(s, Msg::PartyDefeated, kBench, kBench, static_cast<uint16_t>(t));
         presenters_say(s, t, kTextDefeat);
         const uint8_t last = s.battlers[tr.first_battler + tr.count - 1].level;
         const int32_t amount = rules::reward(tr.data->reward, last);
         s.reward += static_cast<uint32_t>(amount > 0 ? amount : 0);
      }
      if (s.reward > 0)
      {
         say(s, Msg::Reward, kBench, kBench, static_cast<uint16_t>(s.reward > kMaxU16 ? kMaxU16 : s.reward));
      }
   }
   else
   {
      s.outcome = Outcome::Defeat;
      say(s, Msg::OutOfCreatures, kBench);
      for (int t = 0; t < s.party_count; ++t)
      {
         if (s.parties[t].side == winner && s.parties[t].data != nullptr)
         {
            presenters_return(s, t);
            presenters_say(s, t, kTextVictory);
         }
      }
   }
   s.phase = Phase::Ended;
   emit(s, EventKind::End, kBench, kBench, 0, static_cast<uint16_t>(s.outcome));
   return true;
}

/// Ends the battle at once with an outcome (effect `end_battle`, E13b): the move in progress (and a move it chained)
/// stops, and so does the unit of work running (C5: a move's after_move hooks, the start's switch-in hooks, the end
/// of turn's later units); the next step() pays the experience already earned, then shows the End event and writes
/// the records back (finish_ended()). Nothing happens once the battle has an outcome.
/// @param s  battle
/// @param o  the outcome (not Outcome::None)
void end_battle(State& s, Outcome o)
{
   if (s.outcome != Outcome::None || s.phase == Phase::Ended || o == Outcome::None)
   {
      return;
   }
   s.outcome = o;
   s.mv.move = kNone16;
   s.mv.chain_move = kNone16;
}

/// The end of a battle whose outcome is set: the End event, then the records are written back (a successful run;
/// effect `end_battle`).
/// @param s  battle
void finish_ended(State& s)
{
   s.phase = Phase::Ended;
   emit(s, EventKind::End, kBench, kBench, 0, static_cast<uint16_t>(s.outcome));
   write_back(s);
}

/// Writes battle results back into the parties' records (not unowned parties').
/// @param s  battle
void write_back(State& s)
{
   using monster::Role;
   for (int t = 0; t < s.party_count; ++t)
   {
      PartyState& tr = s.parties[t];
      if (tr.records == nullptr || has_flag(tr.flags, PARTY_UNOWNED))
      {
         continue;
      }
      for (int k = 0; k < tr.count; ++k)
      {
         const Battler& b = s.battlers[tr.first_battler + k];
         if (!has_flag(b.flags, BATTLER_FROM_RECORD))
         {
            continue;
         }
         monster::Record& rec = tr.records->members[b.party_slot];
         store_resources(b, rec);   // the persisted pools and raises (D-25; the hit points among them)
         // the status id in the layout's status_bits, its counter in the bits above (as many as the role has)
         const uint32_t counter_max = monster::max_value(Role::PrimaryStatus) >> status_id_bits();
         const uint32_t counter = b.status_counter < counter_max ? b.status_counter : counter_max;
         monster::set(rec, Role::PrimaryStatus, static_cast<uint32_t>(b.status) | counter << status_id_bits());
         monster::set(rec, Role::Experience, b.exp);
         monster::set(rec, Role::Level, b.level);
         monster::set(rec, Role::HeldItem, b.item);
         for (int m = 0; m < move_slots(); ++m)
         {
            monster::set(rec, Role::Moves, m, b.moves[m]);
         }
         store_training(b, rec);   // G4: the training gained (a form changed in battle is battle-only)
      }
   }
}

/// Positions whose battler fainted and whose owner can still send one in.
/// @param s     battle
/// @param pos   position
/// @return true if it needs a replacement
bool needs_replacement(const State& s, uint8_t pos)
{
   const uint8_t i = s.at[pos];
   if (i != kBench && !has_flag(s.battlers[i].flags, BATTLER_FAINTED))
   {
      return false;
   }
   const int t = owner_of(s, pos);
   if (t < 0 || has_flag(s.parties[t].flags, PARTY_UNOWNED))
   {
      return false;
   }
   return next_bench(s, t) != kBench;
}

/// Whether a battler may take an active battler's place (C5: what can_switch_to() checks besides trapping; a forced
/// switch and an effect's SwitchTarget request ask only this): on the bench, not fainted, the same party, and not
/// already chosen by an ally's switch this turn.
/// @param s       battle
/// @param active  the battler being replaced
/// @param bench   the candidate
/// @return true if allowed
bool switch_allowed(const State& s, uint8_t active, uint8_t bench)
{
   if (active >= s.battler_count || bench >= s.battler_count)
   {
      return false;
   }
   const Battler& b = s.battlers[bench];
   if (b.party != s.battlers[active].party || b.position != kBench || !usable(b))
   {
      return false;
   }
   // Not already chosen by an ally's switch this turn.
   for (uint8_t p = 0; p < kMaxPositions; ++p)
   {
      if (s.actions[p].kind == ActionKind::Switch && s.actions[p].battler == bench && s.at[p] != active)
      {
         return false;
      }
   }
   return true;
}

}   // namespace capsule::battle::detail

// -- public ------------------------------------------------------------------------------------------------

namespace capsule::battle
{

using namespace detail;

bool can_escape(const State& s, uint8_t battler)
{
   if (battler >= s.battler_count || has_flag(s.flags, SETUP_NO_ESCAPE))
   {
      return false;
   }
   const uint8_t side = side_of(s, battler);
   for (int t = 0; t < s.party_count; ++t)
   {
      if (s.parties[t].side != side && has_flag(s.parties[t].flags, PARTY_NO_ESCAPE))
      {
         return false;
      }
   }
   return !is_trapped(s, battler);
}

bool can_switch_to(const State& s, uint8_t active, uint8_t bench)
{
   return switch_allowed(s, active, bench) && !is_trapped(s, active);
}

}   // namespace capsule::battle
