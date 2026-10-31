/// @file
/// The turn loop (C-08): turn order, the phases of step(), and the choices of request() / choose(). The turn
/// structure is the data's (D-22 A11, TurnRules in capsule/battle/data/rules.h): action brackets and the tie
/// rule, the end-of-turn phases in order, the battle start's send-out and switch-in order.
/// See capsule/battle/battle.h; the modules share internal.h.
#include "internal.h"

namespace capsule::battle::detail
{

/// @name Turn order (action_key())
///@{
constexpr int32_t kBracketBias = 64;          ///< Makes every bracket non-negative (brackets are -64..63).
constexpr int kBracketShift = 24;             ///< Shift of the bracket in the key.
constexpr int kSpeedShift = 8;                ///< Shift of the speed in the key (the tiebreak is below).
constexpr uint32_t kTiebreakValues = 256;     ///< Tiebreak values (one byte).
constexpr int kSidePasses = 2;                ///< FoesFirst send-out passes: foes, then the player's side.
///@}

/// @name End of turn (State::sub: 0, then 1 + the phase index in TurnRules::end, then 1 + end_count)
///@{
constexpr uint8_t kEndBegin = 0;              ///< Not started.
constexpr uint8_t kEndFirst = 1;              ///< The first phase.
///@}

/// The turn structure of the build's data.
/// @return TurnRules
inline const TurnRules& turn_rules()
{
   return data().rules.turn;
}

/// Sorts battlers fastest first (each speed read once, so a roll in the
/// speed pipeline draws once per battler).
/// @param s    battle
/// @param ids  battlers (sorted in place)
/// @param n    count
void sort_by_speed(State& s, uint8_t* ids, int n)
{
   int32_t keys[kMaxPositions];
   for (int k = 0; k < n; ++k)
   {
      keys[k] = speed_of(s, ids[k]);
   }
   for (int a = 1; a < n; ++a)
   {
      for (int k = a; k > 0 && keys[k] > keys[k - 1]; --k)
      {
         const int32_t tk = keys[k];
         keys[k] = keys[k - 1];
         keys[k - 1] = tk;
         const uint8_t t = ids[k];
         ids[k] = ids[k - 1];
         ids[k - 1] = t;
      }
   }
}

/// Fastest-first comparison key of an action: its bracket (manifest `turn.brackets`), the battler's speed, then
/// its tiebreak byte (0 under TieRule::Position: the stable sort keeps position order).
/// @param s    battle
/// @param pos  position
/// @return sort key (bigger acts first)
int32_t action_key(State& s, uint8_t pos)
{
   const TurnRules& tr = turn_rules();
   const uint8_t i = s.at[pos];
   const Action& a = s.actions[pos];
   int32_t bracket = 0;
   if (a.kind == ActionKind::Run)
   {
      bracket = tr.run_bracket;
   }
   else if (a.kind == ActionKind::Switch)
   {
      bracket = tr.switch_bracket;
   }
   else if (a.kind == ActionKind::Item)
   {
      bracket = tr.item_bracket;   // G3: the bag's Item action
   }
   else if (tr.move_priority == 0)
   {
      bracket = tr.move_bracket;
   }
   else
   {
      uint16_t move = a.slot == kFallbackSlot ? data().rules.fallback_move
                    : a.slot == kLockedSlot   ? s.battlers[i].lock_move
                                              : (a.slot < move_slots() ? s.battlers[i].moves[a.slot] : kNone16);
      bracket = move < data().move_count ? data().moves[move].priority : 0;
   }
   // bracket + 64 <= 127 (7 bits), speed <= stat_cap <= 0xFFFF (16 bits), tiebreak 8 bits: fits int32.
   HookCtx c{i, kBench, kNone16,
             (bracket + kBracketBias) << kBracketShift | (speed_of(s, i) & kMaxU16) << kSpeedShift |
                s.battlers[i].tiebreak,
             kVerdictNone, 0, 0};
   run_hook(s, effect::Hook::Order, c);   // order handlers may change the key (and draw)
   return c.value;
}

/// The weather phase's start: the weather counts down (it may end); its per-battler part runs only when it goes
/// on and has a field_residual handler.
/// @param s  battle
/// @return true when the per-battler part runs
bool weather_countdown(State& s)
{
   if (s.weather == kNone8)
   {
      return false;
   }
   if (s.weather_turns > 0 && --s.weather_turns == 0)
   {
      // it ends as a field condition does: its end hook runs (holder: the first battler on the field)
      weather_off(s, condition_holder(s, false, kNoSide));
      return false;
   }
   emit(s, EventKind::WeatherText, kBench, kBench, kWeatherContinues, s.weather);
   return weather_hurts(s);
}

/// Starts the end-of-turn phase State::sub names (the battler cursor back at the fastest; the weather counts
/// down here, so its texts come where the manifest puts the phase).
/// @param s  battle
void enter_end_phase(State& s)
{
   s.order_next = 0;
   const EndPhaseData& ph = turn_rules().end[s.sub - kEndFirst];
   if (ph.phase == static_cast<uint8_t>(EndPhase::Weather) && !weather_countdown(s))
   {
      s.order_next = s.order_count;   // no per-battler part
   }
}

/// Whether try_switch hooks keep a battler from switching out or running away (E13b: trapping; C5: foes too). Its
/// own owners' handlers run first (holder = it, other = kBench), then the per_foe handlers of each active foe's
/// owners in position order (holder = the foe, other = it); value = the battler in both. The first block traps it
/// (the blocking handler says why: its texts are events of the caller's). A forced switch (effect `switch`) does not
/// ask.
/// @param s  battle
/// @param i  the battler choosing
/// @return true when a handler blocked
bool trapped(State& s, uint8_t i)
{
   HookCtx own{i, kBench, kNone16, i, kVerdictNone, 0, 0};
   run_hook(s, effect::Hook::TrySwitch, own, HandlerPart::OwnOnly);
   if (own.verdict == kVerdictBlocked)
   {
      return true;
   }
   for (uint8_t p = 0; p < format(s).positions; ++p)
   {
      const uint8_t o = s.at[p];
      if (o == kBench || !alive_active(s, o) || side_of(s, o) == side_of(s, i))
      {
         continue;
      }
      HookCtx foe{o, i, kNone16, i, kVerdictNone, 0, 0};
      run_hook(s, effect::Hook::TrySwitch, foe, HandlerPart::FoesOnly);
      if (foe.verdict == kVerdictBlocked)
      {
         return true;
      }
   }
   return false;
}

/// A refused switch or run of a trapped battler (C5): the trapping handler's texts, once per turn per battler
/// (State::trap_told) and only while the event ring has a unit's room (a choice is not a step: nothing may be lost).
/// @param s  battle
/// @param i  the battler choosing
void tell_trapped(State& s, uint8_t i)
{
   if (has_flag(s.trap_told, i) || kEventCapacity - s.ev_count < kEventsPerStep)
   {
      return;
   }
   s.trap_told |= bit(i);
   trapped(s, i);
}

/// A delayed hit landing (G1): for the scope, the move cursor holds the delayed hit's move as the move in progress
/// on the battler at the position (its land phase's `damage` is a hit of it: hook registers, `hits`, `crit`,
/// `absorbed`) and State::landing names the position (whose snapshot the hit reads). The cursor goes back to no
/// move when the scope ends.
struct LandingScope
{
   State& s;   ///< The battle.

   /// Puts the landing in place.
   /// @param state  battle (no move in progress: the end of a turn)
   /// @param pos    the position
   /// @param d      its delayed hit
   /// @param t      the battler there
   LandingScope(State& state, uint8_t pos, const Delayed& d, uint8_t t) : s(state)
   {
      ENGINE_ASSERT(s.mv.move == kNone16);
      MoveCursor& mc = s.mv;
      const DelayedHitData& dh = data().delayed_hits[d.snap.rule];
      mc.move = d.move;
      mc.user = d.user;
      mc.chain_move = kNone16;
      mc.count = 1;
      mc.targets[0] = t;
      mc.index = 0;
      mc.stage = MoveStage::Hit;
      mc.flags = static_cast<uint8_t>(bit(CURSOR_CHECKED));
      mc.chosen = pos;
      mc.hits = 0;
      mc.hit_target = t;
      mc.hit_type = kNone8;
      if (has_flag(dh.flags, DELAYED_TYPED))
      {
         aim_hit(s, t);   // the move's type (and its move_type point), as a hit of it
      }
      else
      {
         mc.hit_type = kTypeless;   // no type: a neutral chart, no same-type match
         mc.hit_category = data().moves[d.move].category;
      }
      s.landing = pos;
   }
   LandingScope(const LandingScope&) = delete;
   LandingScope& operator=(const LandingScope&) = delete;
   /// Ends the landing: no move in progress.
   ~LandingScope()
   {
      s.landing = kBench;
      s.mv.move = kNone16;
      s.mv.count = 0;
      s.mv.flags = 0;
      s.mv.hits = 0;
      s.mv.hit_type = kNone8;
      s.mv.hit_target = kBench;
   }
};

/// One position of the end-of-turn `delayed` phase (E13b; State::order_next is the position): its pending effect
/// counts down and lands at 0: the move's land phase runs on the battler there (nothing on an empty position). A
/// delayed hit (G1, effect `delay_hit`) lands inside a LandingScope and keeps its snapshot until it is done.
/// @param s  battle
/// @return false when every position is done
bool delayed_unit(State& s)
{
   if (s.order_next >= format(s).positions || s.order_next >= kMaxPositions)
   {
      return false;
   }
   const uint8_t pos = s.order_next++;
   Delayed& d = s.delayed[pos];
   if (d.turns == 0 || --d.turns != 0)
   {
      return true;
   }
   const uint16_t move = d.move;
   const uint8_t user = d.user;
   const int32_t amount = d.amount;
   const bool hit = d.snap.rule < data().delayed_hit_count && move < data().move_count && user < s.battler_count;
   if (!hit)
   {
      d = Delayed{};   // a plain delay: the slot is free for its land phase
   }
   const uint8_t t = s.at[pos];
   const uint16_t prog = move < data().move_count ? phase_program(move, effect::Hook::Land) : kNoProgram;
   if (t != kBench && alive_active(s, t) && prog != kNoProgram)
   {
      // the land phase: holder = user = who used the move, target = the battler there, value = the stored amount
      effect::Context ctx{amount, 0, move, data().moves[move].owner, user, t, user, t};
      effect::HostState host{s};
      if (hit)
      {
         const LandingScope landing(s, pos, d, t);
         effect::invoke(s.vm, data().effects, host, prog, ctx);
      }
      else
      {
         effect::invoke(s.vm, data().effects, host, prog, ctx);
      }
      check_faints(s, kBench);
   }
   if (hit)
   {
      s.delayed[pos] = Delayed{};   // its snapshot was read until now
   }
   return true;
}

/// Runs one unit of the current end-of-turn phase (one battler, or one condition's battler or countdown).
/// @param s  battle
/// @return false when the phase has nothing left
bool end_phase_unit(State& s)
{
   const EndPhaseData& ph = turn_rules().end[s.sub - kEndFirst];
   switch (static_cast<EndPhase>(ph.phase))
   {
   case EndPhase::Weather:
   case EndPhase::Battlers:
   case EndPhase::Regen:
   {
      if (s.order_next >= s.order_count)
      {
         return false;
      }
      const uint8_t i = s.order[s.order_next++];
      if (ph.phase == static_cast<uint8_t>(EndPhase::Weather))
      {
         end_turn_weather(s, i);
      }
      else if (ph.phase == static_cast<uint8_t>(EndPhase::Regen))
      {
         regen_battler(s, i);   // the resources' regen (D-25)
      }
      else
      {
         end_turn_battler(s, i, ph);
      }
      return true;
   }
   case EndPhase::Delayed:
      return delayed_unit(s);
   case EndPhase::Sides:
      return end_turn_conditions(s, false, ph.each);
   case EndPhase::Field:
      return end_turn_conditions(s, true, ph.each);
   default:
      return false;
   }
}

/// End of turn, part 1 (one unit): the order for the per-battler parts is set (fastest first) and the first
/// phase starts.
/// @param s  battle
void end_turn_begin(State& s)
{
   s.order_count = 0;
   for (uint8_t p = 0; p < format(s).positions; ++p)
   {
      if (s.at[p] != kBench && alive_active(s, s.at[p]))
      {
         s.order[s.order_count++] = s.at[p];
      }
   }
   sort_by_speed(s, s.order, s.order_count);
   s.order_next = 0;
   s.cond_next = 0;
   s.sub = kEndFirst;
   if (turn_rules().end_count > 0)
   {
      enter_end_phase(s);
   }
}

/// Prepares a new turn: AI choices from the current state.
/// @param s  battle
void begin_turn(State& s)
{
   s.turn = static_cast<uint16_t>(s.turn + 1);
   s.trap_told = 0;
   emit(s, EventKind::Turn, kBench, kBench, 0, s.turn);
   for (uint8_t p = 0; p < kMaxPositions; ++p)
   {
      s.actions[p] = Action{ActionKind::None, 0, kBench, 0};
   }
   const bool roll_ties = turn_rules().tie == static_cast<uint8_t>(TieRule::RandomByte);
   for (uint8_t i = 0; i < s.battler_count; ++i)
   {
      s.battlers[i].flags = static_cast<uint8_t>(s.battlers[i].flags & ~bit(BATTLER_ACTED));
      s.battlers[i].tiebreak = roll_ties ? static_cast<uint8_t>(rng::below(s.rng, kTiebreakValues)) : 0;
   }
   for (uint8_t p = 0; p < format(s).positions; ++p)
   {
      const uint8_t i = s.at[p];
      if (i != kBench && alive_active(s, i))
      {
         hook_simple(s, effect::Hook::TurnStart, i, kBench);
      }
   }
   for (uint8_t p = 0; p < format(s).positions; ++p)
   {
      const uint8_t i = s.at[p];
      if (i == kBench || !alive_active(s, i))
      {
         continue;
      }
      const Battler& b = s.battlers[i];
      if ((b.lock_flags & kLockSet) != 0)
      {
         s.actions[p] = Action{ActionKind::Move, kLockedSlot, b.lock_target, 0};   // locked: no choice
      }
      else if (s.parties[b.party].controller == Controller::Ai)
      {
         s.actions[p] = ai_choose(s, i);
      }
   }
   s.phase = Phase::Choose;
}

}   // namespace capsule::battle::detail

// -- public ------------------------------------------------------------------------------------------------

namespace capsule::battle
{

using namespace detail;

Progress step(State& s)
{
   if (s.phase == Phase::Ended)
   {
      return Progress::Ended;
   }
   if (kEventCapacity - s.ev_count < kEventsPerStep)
   {
      return Progress::Busy;
   }
   // effect `end_battle` (E13b) may have ended it in the middle of something: nothing more runs but the experience
   // already earned (C5: shown and paid first, level-ups and their move offers too; end_battle() stopped the move)
   const bool ended = s.outcome != Outcome::None;
   // A move in progress runs to its end first, one unit per step (it may wait for an answer).
   if (!ended && s.mv.move != kNone16)
   {
      if (move_asking(s))
      {
         return Progress::Input;
      }
      advance_move(s);
      return Progress::Busy;
   }
   // Then experience and level-ups, one per step (a level-up's move offer waits for its answer); then the
   // battle may be over.
   if (learn_asking(s))
   {
      return Progress::Input;
   }
   if (pay_exp(s))
   {
      return Progress::Busy;
   }
   if (ended)
   {
      finish_ended(s);
      return Progress::Ended;
   }
   if (s.phase != Phase::Intro && s.phase != Phase::Start && check_end(s))
   {
      write_back(s);
      return Progress::Ended;
   }
   switch (s.phase)
   {
   case Phase::Intro:
   {
      // One party's intro per step, in party order; parties with nothing to show take no step.
      const uint16_t before = s.ev_count;
      while (s.sub < s.party_count && s.ev_count == before)
      {
         party_intro(s, s.sub);
         s.sub = static_cast<uint8_t>(s.sub + 1);
      }
      if (s.ev_count != before)
      {
         return Progress::Busy;
      }
      s.sub = 0;
      s.phase = Phase::Start;
      [[fallthrough]];   // nothing left to show: the send-outs come in this same step
   }
   case Phase::Start:
   {
      if (s.weather != kNone8)
      {
         emit(s, EventKind::Weather, kBench, kBench, 0, s.weather);
         emit(s, EventKind::WeatherText, kBench, kBench, kWeatherContinues, s.weather);
      }
      // Foes first, then the player's side (as the originals present it), or in position order (one pass).
      // State::order keeps the battlers as they come out.
      const TurnRules& tr = turn_rules();
      const bool foes_first = tr.send_order == static_cast<uint8_t>(SendOrder::FoesFirst);
      const uint8_t ps = player_side(s);
      s.order_count = 0;
      for (int pass = 0; pass < (foes_first ? kSidePasses : 1); ++pass)
      {
         for (uint8_t p = 0; p < format(s).positions; ++p)
         {
            const int t = owner_of(s, p);
            if (t < 0 || (foes_first && (s.parties[t].side == ps) != (pass == 1)))
            {
               continue;
            }
            const uint8_t i = next_bench(s, t);
            if (i != kBench)
            {
               send_in(s, i, p, false);
               s.order[s.order_count++] = i;
            }
         }
      }
      // Switch-in effects once everyone is out, one battler per step: fastest first (StartHooks::BySpeed) or in
      // send-out order.
      if (tr.start_hooks == static_cast<uint8_t>(StartHooks::BySpeed))
      {
         s.order_count = 0;
         for (uint8_t p = 0; p < format(s).positions; ++p)
         {
            if (s.at[p] != kBench)
            {
               s.order[s.order_count++] = s.at[p];
            }
         }
         sort_by_speed(s, s.order, s.order_count);
      }
      // the Setup weather (said above) starts now that the battlers are out: its start hook, held by the first
      // battler of the switch-in order (as a weather an effect sets)
      if (s.weather < data().weather_count && s.order_count > 0)
      {
         run_own(s, data().weathers[s.weather].owner, effect::Hook::Start, s.order[0], kBench, s.weather_turns);
      }
      s.order_next = 0;
      s.phase = Phase::StartHooks;
      return Progress::Busy;
   }
   case Phase::StartHooks:
   {
      const uint16_t before = s.ev_count;
      while (s.order_next < s.order_count && s.ev_count == before && s.outcome == Outcome::None)
      {
         switch_in_hooks(s, s.order[s.order_next++]);
      }
      if (s.order_next >= s.order_count && s.ev_count == before && s.outcome == Outcome::None)
      {
         begin_turn(s);
      }
      return Progress::Busy;
   }
   case Phase::Choose:
   {
      Request r;
      if (request(s, r))
      {
         return Progress::Input;
      }
      // Order every chosen action.
      s.order_count = 0;
      for (uint8_t p = 0; p < format(s).positions; ++p)
      {
         if (s.at[p] != kBench && s.actions[p].kind != ActionKind::None)
         {
            s.order[s.order_count++] = p;
         }
      }
      int32_t keys[kMaxPositions];
      for (int k = 0; k < s.order_count; ++k)
      {
         keys[k] = action_key(s, s.order[k]);
      }
      for (int a = 1; a < s.order_count; ++a)
      {
         for (int k = a; k > 0 && keys[k] > keys[k - 1]; --k)
         {
            const int32_t tk = keys[k];
            keys[k] = keys[k - 1];
            keys[k - 1] = tk;
            const uint8_t tp = s.order[k];
            s.order[k] = s.order[k - 1];
            s.order[k - 1] = tp;
         }
      }
      s.order_next = 0;
      s.phase = Phase::Execute;
      return Progress::Busy;
   }
   case Phase::Execute:
   {
      if (s.order_next >= s.order_count)
      {
         s.phase = Phase::EndTurn;
         s.sub = kEndBegin;
         return Progress::Busy;
      }
      const uint8_t pos = s.order[s.order_next++];
      const uint8_t i = s.at[pos];
      if (i == kBench || !alive_active(s, i))
      {
         return Progress::Busy;
      }
      const Action a = s.actions[pos];
      s.battlers[i].flags = static_cast<uint8_t>(s.battlers[i].flags | bit(BATTLER_ACTED));
      switch (a.kind)
      {
      case ActionKind::Run:
      {
         int32_t foe_speed = 1;
         for (uint8_t p = 0; p < format(s).positions; ++p)
         {
            const uint8_t o = s.at[p];
            if (o != kBench && alive_active(s, o) && side_of(s, o) != side_of(s, i))
            {
               foe_speed = rules::max32(foe_speed, speed_of(s, o));
            }
         }
         const int32_t odds = rules::escape_odds(speed_of(s, i), foe_speed, s.escape_attempts);
         if (odds >= rules::kEscapeOutcomes ||
             rng::chance(s.rng, static_cast<uint32_t>(odds < 0 ? 0 : odds), rules::kEscapeOutcomes))
         {
            say(s, Msg::GotAway, i);
            s.outcome = Outcome::Escaped;
            finish_ended(s);
            return Progress::Ended;
         }
         if (s.escape_attempts < kCountMax8)
         {
            ++s.escape_attempts;
         }
         say(s, Msg::CantEscape, i);
         break;
      }
      case ActionKind::Switch:
         if (switch_allowed(s, i, a.battler))   // the trap was asked when it was chosen
         {
            withdraw(s, i);
            send_in(s, a.battler, pos);
         }
         break;
      case ActionKind::Move:
         begin_move(s, i, a);
         break;
      case ActionKind::Item:
         item_action(s, i, a);   // G3 (item_action.cpp); a recruit may end the battle here
         break;
      default:
         break;
      }
      return Progress::Busy;
   }
   case Phase::EndTurn:
   {
      const uint16_t before = s.ev_count;
      if (s.sub == kEndBegin)
      {
         end_turn_begin(s);
         return Progress::Busy;
      }
      // The manifest's phases in order, one unit per step (a unit that shows nothing lets the next one run in
      // the same step): weather damage, a battlers phase, one side or field condition's battler or countdown.
      const uint8_t done = static_cast<uint8_t>(kEndFirst + turn_rules().end_count);
      while (s.sub < done && s.ev_count == before && s.outcome == Outcome::None)
      {
         if (!end_phase_unit(s))
         {
            s.sub = static_cast<uint8_t>(s.sub + 1);
            if (s.sub < done)
            {
               enter_end_phase(s);
            }
         }
      }
      if (s.sub >= done && s.outcome == Outcome::None)
      {
         end_turn_clear(s);
         for (uint8_t p = 0; p < kMaxPositions; ++p)
         {
            s.actions[p] = Action{ActionKind::None, 0, kBench, 0};
         }
         s.sub = kEndBegin;
         s.phase = Phase::Replace;
      }
      return Progress::Busy;
   }
   case Phase::Replace:
   {
      // One replacement per step: the human choices made through choose(), then the AI's.
      for (uint8_t p = 0; p < format(s).positions; ++p)
      {
         Action& a = s.actions[p];
         if (a.kind != ActionKind::Switch || a.slot != kPendingSend)
         {
            continue;
         }
         const uint8_t old = s.at[p];
         if (old != kBench)
         {
            s.battlers[old].position = kBench;
            s.at[p] = kBench;
         }
         a.slot = 0;
         send_in(s, a.battler, p);
         return Progress::Busy;
      }
      for (uint8_t p = 0; p < format(s).positions; ++p)
      {
         if (s.actions[p].kind != ActionKind::None || !needs_replacement(s, p))
         {
            continue;
         }
         const int t = owner_of(s, p);
         if (s.parties[t].controller != Controller::Ai)
         {
            continue;
         }
         const uint8_t i = s.at[p];
         if (i != kBench)
         {
            s.battlers[i].position = kBench;
            s.at[p] = kBench;
         }
         const uint8_t n = next_bench(s, t);
         if (n != kBench)
         {
            send_in(s, n, p);
            s.actions[p].kind = ActionKind::Switch;   // filled this round
            return Progress::Busy;
         }
      }
      Request r;
      if (request(s, r))
      {
         return Progress::Input;
      }
      // Clear fainted battlers off positions nobody refills.
      for (uint8_t p = 0; p < format(s).positions; ++p)
      {
         const uint8_t i = s.at[p];
         if (i != kBench && has_flag(s.battlers[i].flags, BATTLER_FAINTED))
         {
            s.battlers[i].position = kBench;
            s.at[p] = kBench;
         }
      }
      if (check_end(s))
      {
         write_back(s);
         return Progress::Ended;
      }
      begin_turn(s);
      return Progress::Busy;
   }
   default:
      break;
   }
   return Progress::Busy;
}

bool request(const State& s, Request& out)
{
   if (move_asking(s))
   {
      // an effect's request
      const uint8_t i = s.mv.ask_battler;
      out = Request{i, i < s.battler_count ? s.battlers[i].position : kBench, static_cast<RequestKind>(s.mv.ask), 0};
      return true;
   }
   if (learn_asking(s))
   {
      // a level-up's move offer (State::learn: the move): which slot's move to forget, or kKeepMoves
      const uint8_t i = s.learn.battler;
      out = Request{i, s.battlers[i].position, RequestKind::LearnMove, 0};
      return true;
   }
   if (s.phase == Phase::Choose)
   {
      for (uint8_t p = 0; p < format(s).positions; ++p)
      {
         const uint8_t i = s.at[p];
         if (i == kBench || !alive_active(s, i) || s.actions[p].kind != ActionKind::None)
         {
            continue;
         }
         if (s.parties[s.battlers[i].party].controller != Controller::Human)
         {
            continue;
         }
         out = Request{i, p, RequestKind::Action, 0};
         return true;
      }
      return false;
   }
   if (s.phase == Phase::Replace)
   {
      for (uint8_t p = 0; p < format(s).positions; ++p)
      {
         if (s.actions[p].kind != ActionKind::None)
         {
            continue;
         }   // already refilled
         const int t = owner_of(s, p);
         if (t < 0 || s.parties[t].controller != Controller::Human)
         {
            continue;
         }
         if (!needs_replacement(s, p))
         {
            continue;
         }
         out = Request{s.at[p], p, RequestKind::Replace, 0};
         return true;
      }
   }
   return false;
}

bool choose(State& s, uint8_t battler, const Action& a)
{
   Request r;
   if (!request(s, r) || (r.battler != battler && r.kind == RequestKind::Action))
   {
      return false;
   }
   const uint8_t pos = r.position;
   if (r.kind == RequestKind::SwitchTarget || r.kind == RequestKind::LearnMove || r.kind == RequestKind::Choice)
   {
      // an effect's request: the answer is the slot (a replacement must be one)
      if (a.kind != ActionKind::Answer || battler != r.battler ||
          (r.kind == RequestKind::SwitchTarget && !switch_allowed(s, battler, a.slot)))
      {
         return false;
      }
      if (!move_asking(s))
      {
         // a level-up's move offer: a move slot to forget, or kKeepMoves
         if (a.slot >= move_slots() && a.slot != kKeepMoves)
         {
            return false;
         }
         s.learn.answer = a.slot;
         s.learn.answered = 1;
         return true;
      }
      s.mv.answer = a.slot;
      s.mv.answered = 1;
      return true;
   }
   if (r.kind == RequestKind::Replace)
   {
      if (a.kind != ActionKind::Switch || a.battler >= s.battler_count)
      {
         return false;
      }
      const int t = owner_of(s, pos);
      const Battler& nb = s.battlers[a.battler];
      if (nb.party != t || nb.position != kBench || !usable(nb) || pending_send(s, a.battler))
      {
         return false;
      }
      s.actions[pos] = Action{ActionKind::Switch, kPendingSend, kBench, a.battler};   // step() sends it in
      return true;
   }
   const Battler& b = s.battlers[battler];
   switch (a.kind)
   {
   case ActionKind::Move:
   {
      if (a.slot == kFallbackSlot)
      {
         for (uint8_t k = 0; k < move_slots(); ++k)
         {
            if (move_usable(s, battler, k))
            {
               return false;
            }   // the fallback move only when no move's cost can be paid
         }
         // Without a fallback move the turn is lost (NoUsableMove), but it can still be given.
      }
      else
      {
         if (!move_usable(s, battler, a.slot))
         {
            return false;
         }
         const uint8_t mask = target_choices(s, battler, b.moves[a.slot]);
         if (a.target != kBench && a.target >= format(s).positions)
         {
            return false;
         }
         if (mask != 0 && a.target != kBench && ((mask >> a.target) & 1) == 0)
         {
            return false;
         }
      }
      break;
   }
   case ActionKind::Switch:
      if (!switch_allowed(s, battler, a.battler))
      {
         return false;
      }
      if (is_trapped(s, battler))
      {
         tell_trapped(s, battler);   // the trapping handler says why, once this turn
         return false;
      }
      break;
   case ActionKind::Run:
      if (!can_escape(s, battler))
      {
         if (is_trapped(s, battler))
         {
            tell_trapped(s, battler);
         }
         return false;
      }
      break;
   case ActionKind::Item:
      // G3: an item the party has left (not taken by this turn's other choices) on a target it fits
      if (!item_usable(s, battler, a.item) || !item_target_valid(s, battler, a))
      {
         return false;
      }
      break;
   default:
      return false;
   }
   s.actions[pos] = a;
   return true;
}

bool is_trapped(const State& s, uint8_t battler)
{
   if (battler >= s.battler_count || !alive_active(s, battler))
   {
      return false;
   }
   // A query on a battle that is not const (State always lives in RAM): the try_switch dispatches run with their
   // events dropped and the stream, the ring and the fault count put back, so the battle is as it was.
   State& w = const_cast<State&>(s);
   const rng::Stream stream = w.rng;
   const uint16_t events = w.ev_count;
   const uint8_t lost = w.events_lost;
   const uint16_t faults = w.vm.faults;
   const effect::Fault fault = w.vm.last_fault;
   const bool held = trapped(w, battler);
   w.rng = stream;
   w.ev_count = events;
   w.events_lost = lost;
   w.vm.faults = faults;
   w.vm.last_fault = fault;
   return held;
}

}   // namespace capsule::battle
