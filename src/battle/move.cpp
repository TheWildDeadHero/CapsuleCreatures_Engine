/// @file
/// Moves (C-08, D-23 section 1.3): targets and the move skeleton, whose phases run as effect code one unit at a
/// time (State::mv).
/// See capsule/battle/battle.h; the modules share internal.h.
#include "internal.h"

namespace capsule::battle::detail
{

/// Positions reachable from a position (format reach mask).
/// @param s    battle
/// @param pos  position
/// @return mask
uint8_t reach(const State& s, uint8_t pos)
{
   return pos < kMaxPositions ? format(s).reach[pos] : 0;
}

/// Mask of positions holding battlers able to be hit, filtered by side.
/// @param s       battle
/// @param user    battler
/// @param foes    foes only (else allies only)
/// @return mask
uint8_t occupied(const State& s, uint8_t user, bool foes)
{
   uint8_t m = 0;
   const uint8_t us = side_of(s, user);
   for (uint8_t p = 0; p < format(s).positions; ++p)
   {
      const uint8_t b = s.at[p];
      if (b == kBench || !alive_active(s, b) || b == user)
      {
         continue;
      }
      if ((side_of(s, b) != us) == foes)
      {
         m = static_cast<uint8_t>(m | bit(p));
      }
   }
   return m;
}

/// Picks one position from a mask at random.
/// @param s  battle
/// @param m  mask (non-zero)
/// @return position
uint8_t random_in(State& s, uint8_t m)
{
   int n = 0;
   for (uint8_t p = 0; p < kMaxPositions; ++p)
   {
      n += (m >> p) & 1;
   }
   if (n == 0)
   {
      return kBench;
   }
   int k = static_cast<int>(rng::below(s.rng, static_cast<uint32_t>(n)));
   for (uint8_t p = 0; p < kMaxPositions; ++p)
   {
      if (((m >> p) & 1) != 0 && k-- == 0)
      {
         return p;
      }
   }
   return kBench;
}

/// Resolves a move's targets (battler indices).
/// @param s       battle
/// @param user    battler
/// @param move    move
/// @param chosen  chosen position, or kBench
/// @param out     targets
/// @return count (0 = no target)
int resolve_targets(State& s, uint8_t user, uint16_t move, uint8_t chosen, uint8_t* out)
{
   const MoveData& m = data().moves[move];
   const uint8_t from = s.battlers[user].position;
   const uint8_t rch = reach(s, from);
   if (chosen >= format(s).positions)
   {
      chosen = kBench;
   }
   int n = 0;
   auto add_mask = [&](uint8_t mask)
   {
      for (uint8_t p = 0; p < format(s).positions; ++p)
      {
         if (((mask >> p) & 1) != 0)
         {
            out[n++] = s.at[p];
         }
      }
   };
   switch (m.target)
   {
   case Target::Self:
      out[n++] = user;
      break;
   case Target::Field:
      break;
   case Target::AllFoes:
      add_mask(static_cast<uint8_t>(occupied(s, user, true) & rch));
      break;
   case Target::AllOthers:
      add_mask(static_cast<uint8_t>((occupied(s, user, true) | occupied(s, user, false)) & rch));
      break;
   case Target::RandomFoe:
   {
      const uint8_t p = random_in(s, static_cast<uint8_t>(occupied(s, user, true) & rch));
      if (p != kBench)
      {
         out[n++] = s.at[p];
      }
      break;
   }
   case Target::Ally:
   {
      const uint8_t allies = static_cast<uint8_t>(occupied(s, user, false) & rch);
      if (chosen != kBench && ((allies >> chosen) & 1) != 0)
      {
         out[n++] = s.at[chosen];
      }
      else
      {
         const uint8_t p = random_in(s, allies);
         if (p != kBench)
         {
            out[n++] = s.at[p];
         }
      }
      break;
   }
   case Target::Selected:
   case Target::SelectedFoe:
   default:
   {
      const uint8_t foes = static_cast<uint8_t>(occupied(s, user, true) & rch);
      const uint8_t any =
         m.target == Target::Selected ? static_cast<uint8_t>(foes | (occupied(s, user, false) & rch)) : foes;
      if (chosen != kBench && ((any >> chosen) & 1) != 0)
      {
         out[n++] = s.at[chosen];
      }
      else if (chosen != kBench && format(s).on_target_fainted == OnTargetFainted::Fail)
      {
         break;   // the chosen target is gone
      }
      else
      {
         const uint8_t p = random_in(s, foes != 0 ? foes : any);
         if (p != kBench)
         {
            out[n++] = s.at[p];
         }
      }
      break;
   }
   }
   return n;
}

/// The program of one of a move's phases (use, target, try_hit, hit, after_move).
/// @param move   move index
/// @param phase  the phase
/// @return program index, or kNoProgram
uint16_t phase_program(uint16_t move, effect::Hook phase)
{
   const effect::Owner* o = move < data().move_count ? owner_at(data().moves[move].owner) : nullptr;
   if (o == nullptr || !effect::has_hook(*o, phase))
   {
      return kNoProgram;
   }
   for (int k = 0; k < o->handler_count; ++k)
   {
      const effect::Handler& h = data().effects.handlers[o->first_handler + k];
      if ((h.flags & effect::kHandlerPoint) == 0 && h.hook == static_cast<uint8_t>(phase))
      {
         return h.program;
      }
   }
   return kNoProgram;
}

/// Whether the move in progress waits for an answer (request() / choose()).
/// @param s  battle
/// @return true while its run asks and no answer came
bool move_asking(const State& s)
{
   return s.mv.move != kNone16 && s.mv.run.state == effect::RunState::Asking && s.mv.answered == 0;
}

/// Context registers of a move phase.
/// @param s       battle
/// @param target  the target, or kBench
/// @return the registers
effect::Context move_context(const State& s, uint8_t target)
{
   const MoveCursor& mc = s.mv;
   const uint16_t owner = data().moves[mc.move].owner;
   return effect::Context{0, 0, mc.move, owner, mc.user, target, mc.user, target};
}

/// Runs (or goes on with) one of the move's top-level phases: up to its next yield or request, or its end.
/// @param s       battle
/// @param phase   use, hit or after_move
/// @param target  the target, or kBench
/// @return how it stopped (Done when the move has no such phase)
effect::Status move_phase(State& s, effect::Hook phase, uint8_t target)
{
   MoveCursor& mc = s.mv;
   if (!has_flag(mc.flags, CURSOR_BEGUN))
   {
      const uint16_t prog = phase_program(mc.move, phase);
      if (prog == kNoProgram)
      {
         return effect::Status::Done;
      }
      effect::begin(mc.run, prog, move_context(s, target), true);
      mc.flags = static_cast<uint8_t>(mc.flags | bit(CURSOR_BEGUN));
   }
   const bool answering = mc.run.state == effect::RunState::Asking && mc.answered != 0;
   const effect::Status st = run_phase(s, answering);
   check_faints(s, kBench);   // HP lost without a check yet (damage_hp no_faint_check) is checked now
   if (effect::finished(st))
   {
      mc.flags = static_cast<uint8_t>(mc.flags & ~bit(CURSOR_BEGUN));
   }
   return st;
}

/// Whether a target kind is a chosen single target (a move that redirect hooks may retarget; C5: a random foe is
/// not chosen, as Gen-3 Follow Me takes only a selected target).
/// @param t  the target kind
/// @return true for Selected and SelectedFoe
constexpr bool single_target(Target t)
{
   return t == Target::Selected || t == Target::SelectedFoe;
}

/// The target of a single-target move after the active battlers' redirect hooks (E13b; Follow Me style). C5: the
/// hooks see the target the move picked as `target` and value kNoClaim in; the battlers run in position order (the
/// picked target among them, never the user) and the first one whose handlers leave a valid battler (active, not
/// the user) as the value takes the move: the first claim wins, even one that keeps the picked target.
/// @param s       battle
/// @param user    the move's user
/// @param move    the move
/// @param target  the target it picked
/// @return the target it goes to
uint8_t redirected(State& s, uint8_t user, uint16_t move, uint8_t target)
{
   MoveCursor& mc = s.mv;
   // the hooks' `target` register: the picked target (hook_context() reads the cursor's current target)
   mc.count = 1;
   mc.index = 0;
   mc.targets[0] = target;
   for (uint8_t p = 0; p < format(s).positions; ++p)
   {
      const uint8_t b = s.at[p];
      if (b == kBench || b == user || !alive_active(s, b))
      {
         continue;
      }
      HookCtx c{b, user, move, effect::kNoClaim, kVerdictNone, 0, 0};
      run_hook(s, effect::Hook::Redirect, c);
      const bool valid = c.value >= 0 && c.value < s.battler_count && c.value != user &&
                         alive_active(s, static_cast<uint8_t>(c.value));
      if (valid)
      {
         return static_cast<uint8_t>(c.value);
      }
   }
   return target;
}

/// Ends the move in progress (a chained move starts in its place).
/// @param s  battle
void finish_move(State& s)
{
   MoveCursor& mc = s.mv;
   const uint16_t chain = mc.chain_move;
   const uint8_t user = mc.chain_user;
   mc.move = kNone16;
   if (chain != kNone16 && alive_active(s, user))
   {
      begin_move(s, user, Action{ActionKind::Move, kChainSlot, kBench, 0});   // it takes the chain over
   }
}

/// The move's targets: its target phase (`set_target`), else its target kind; then the UseMove cue.
/// @param s       battle
/// @param chosen  the action's target position
void move_targets(State& s, uint8_t chosen)
{
   MoveCursor& mc = s.mv;
   const uint8_t i = mc.user;
   const uint16_t move = mc.move;
   const MoveData& m = data().moves[move];
   uint8_t targets[kMaxPositions];
   int n = 0;
   const uint16_t prog = phase_program(move, effect::Hook::Target);
   if (prog != kNoProgram)
   {
      mc.stage = MoveStage::Targets;
      mc.count = 0;
      effect::Context ctx = move_context(s, kBench);
      effect::HostState host{s};
      if (effect::invoke(s.vm, data().effects, host, prog, ctx) == effect::Status::Failed)
      {
         say(s, Msg::Failed, i);
         mc.move = kNone16;
         return;
      }
      n = mc.count;
      for (int k = 0; k < n; ++k)
      {
         targets[k] = mc.targets[k];
      }
   }
   if (n == 0)
   {
      n = resolve_targets(s, i, move, chosen, targets);
   }
   // a chosen single target, picked by the target kind or by the target phase (C5: set_target moves too, as Gen-3
   // Counter goes to a Follow Me user), may be drawn away
   if (n == 1 && single_target(m.target) && targets[0] != i && targets[0] < s.battler_count)
   {
      targets[0] = redirected(s, i, move, targets[0]);
   }
   emit(s, EventKind::UseMove, i, n > 0 ? s.battlers[targets[0]].position : kBench, 0, move);
   if (m.target == Target::Field)
   {
      mc.count = 1;
      mc.index = 0;
      mc.targets[0] = kBench;
      mc.flags = static_cast<uint8_t>(mc.flags | bit(CURSOR_CHECKED));
      mc.stage = MoveStage::Hit;
      aim_hit(s, kBench);
      return;
   }
   if (n == 0)
   {
      say(s, Msg::NoTarget, i);
      mc.move = kNone16;
      return;
   }
   mc.index = 0;
   mc.count = static_cast<uint8_t>(n);
   for (int k = 0; k < n; ++k)
   {
      mc.targets[k] = targets[k];
   }
   if (n > 1)
   {
      mc.flags = static_cast<uint8_t>(mc.flags | bit(CURSOR_SPREAD));
   }
   mc.stage = MoveStage::Check;
}

/// Starts a move (one unit): the before_move hooks (conditions first), its costs (D-25: paid in the manifest's
/// resource order; the fallback move pays nothing), the "used" message, then its use phase or its targets. The
/// cursor (State::mv) then runs it unit by unit.
/// @param s     battle
/// @param i     battler
/// @param a     its action (kLockedSlot: the locked move; kChainSlot: the chained move, no checks, no costs)
void begin_move(State& s, uint8_t i, const Action& a)
{
   Battler& b = s.battlers[i];
   const uint16_t chain = s.mv.chain_move;
   s.mv = MoveCursor{};
   s.mv.move = kNone16;
   s.mv.chain_move = kNone16;
   s.mv.ask = kNone8;
   s.mv.hit_type = kNone8;
   s.mv.hit_target = kBench;
   const bool locked = a.slot == kLockedSlot;
   const bool chained = a.slot == kChainSlot;
   uint16_t move = kNone16;
   bool free = false;
   if (a.slot == kFallbackSlot)
   {
      move = data().rules.fallback_move;
   }
   else if (locked)
   {
      // the lock is used up by this action
      move = (b.lock_flags & kLockSet) != 0 ? b.lock_move : kNone16;
      free = (b.lock_flags & effect::kLockFree) != 0;
      b.lock_flags = 0;
      b.lock_move = kNone16;
   }
   else if (chained)
   {
      move = chain;
   }
   else if (a.slot < move_slots())
   {
      move = b.moves[a.slot];
   }
   if (move == kNone16 || move >= data().move_count)
   {
      say(s, Msg::NoUsableMove, i);   // no usable move and no fallback move: the turn is lost
      return;
   }
   if (!chained)
   {
      // the user's conditions first (a status's lost turn, a volatile's), then its passive and item
      HookCtx before{i, kBench, move, 0, kVerdictNone, 0, 0};
      run_hook(s, effect::Hook::BeforeMove, before);
      if (before.stopped != 0)
      {
         return;   // a before_move handler stopped: the turn is lost
      }
      if (a.slot < move_slots())
      {
         if (!can_pay(b, move, a.slot))
         {
            say(s, Msg::NoUsableMove, i);   // the costs went up (or the pools down) since it was chosen
            return;
         }
         pay(s, i, move, a.slot);
      }
      // a lock that pays takes its costs from the move's slot when it can (else the locked action is free)
      const int lock_slot = locked && !free ? slot_of(b, move) : -1;
      if (lock_slot >= 0 && can_pay(b, move, lock_slot))
      {
         pay(s, i, move, lock_slot);
      }
      hook_simple(s, effect::Hook::UsedMove, i, kBench, move);
   }
   b.last_move = move;
   say(s, Msg::UsedMove, i, kBench, move);
   MoveCursor& mc = s.mv;
   mc.user = i;
   mc.move = move;
   mc.chosen = a.target;
   mc.flags = static_cast<uint8_t>(locked ? bit(CURSOR_LOCKED) : 0);
   if (phase_program(move, effect::Hook::Use) != kNoProgram)
   {
      mc.stage = MoveStage::Use;   // the use phase runs before the targets, from the next unit
      return;
   }
   move_targets(s, a.target);
}

/// The current target's checks before the move's hit phase: the target's before_accuracy hooks (protection),
/// accuracy, the move's try_hit phase, the target's try_hit hooks.
/// @param s  battle
/// @param t  target
/// @return false if the target is not hit
bool target_checks(State& s, uint8_t t)
{
   const MoveCursor& mc = s.mv;
   const uint8_t i = mc.user;
   const uint16_t move = mc.move;
   const MoveData& m = data().moves[move];
   if (t == i)
   {
      return true;
   }
   Battler& tb = s.battlers[t];
   HookCtx guard{t, i, move, 0, kVerdictNone, 0, 0};
   run_hook(s, effect::Hook::BeforeAccuracy, guard);
   if (guard.verdict == kVerdictBlocked)
   {
      return false;   // the handler said why
   }
   if (m.accuracy != 0)
   {
      if (accuracy_of(s, &s.rng, i, t, move).result == 0)
      {
         say(s, Msg::Missed, i, t);
         return false;
      }
   }
   const uint16_t own = phase_program(move, effect::Hook::TryHit);
   if (own != kNoProgram)
   {
      effect::Context ctx = move_context(s, t);
      effect::HostState host{s};
      const effect::Status st = effect::invoke(s.vm, data().effects, host, own, ctx);
      if (st == effect::Status::Immune)
      {
         say(s, Msg::NoEffect, i, t);
         return false;
      }
      if (st == effect::Status::Failed)
      {
         say(s, Msg::Failed, i);
         return false;
      }
      if (st == effect::Status::Blocked)
      {
         return false;
      }
   }
   HookCtx c{t, i, move, 0, kVerdictNone, 0, 0};
   run_hook(s, effect::Hook::TryHit, c);
   if (c.verdict == kVerdictImmune)
   {
      if (c.heal_q8 > 0 && tb.hp < max_hp(tb))
      {
         set_hp(s, t, tb.hp + rules::max32(1, (static_cast<int32_t>(max_hp(tb)) * c.heal_q8) >> kQ8Shift));
         say(s, Msg::Absorbed, t);
      }
      else
      {
         say(s, Msg::NoEffect, i, t);
      }
      return false;
   }
   if (c.verdict == kVerdictBlocked)
   {
      say(s, Msg::Blocked, t, i);
      return false;
   }
   return true;
}

/// Goes on with the next target (the after_move phase after the last one).
/// @param s  battle
void next_target(State& s)
{
   MoveCursor& mc = s.mv;
   ++mc.index;
   mc.hits = 0;
   const uint8_t keep = static_cast<uint8_t>(mc.flags & (bit(CURSOR_SPREAD) | bit(CURSOR_LOCKED)));
   mc.flags = static_cast<uint8_t>(keep | (mc.targets[0] == kBench ? bit(CURSOR_CHECKED) : 0));
   mc.stage = mc.index >= mc.count ? MoveStage::AfterMove
            : (mc.targets[0] == kBench ? MoveStage::Hit : MoveStage::Check);
}

/// Runs one unit of the move in progress.
/// @param s  battle
void move_unit(State& s)
{
   MoveCursor& mc = s.mv;
   switch (mc.stage)
   {
   case MoveStage::Use:
   {
      const effect::Status st = move_phase(s, effect::Hook::Use, kBench);
      if (!effect::finished(st))
      {
         return;
      }
      if (st == effect::Status::Failed)
      {
         say(s, Msg::Failed, mc.user);
      }
      if (st == effect::Status::Failed || st == effect::Status::Stopped || !alive_active(s, mc.user))
      {
         finish_move(s);
         return;
      }
      move_targets(s, mc.chosen);
      return;
   }
   case MoveStage::Check:
   {
      const uint8_t t = mc.targets[mc.index];
      if (!alive_active(s, t) || !alive_active(s, mc.user))
      {
         next_target(s);
         return;
      }
      aim_hit(s, t);   // C5: the type it hits this target with, once (its checks' hooks read it already)
      if (!target_checks(s, t))
      {
         if (s.outcome == Outcome::None)
         {
            next_target(s);
         }
         return;
      }
      if (t != mc.user)
      {
         // C5: the record of the last move that reached it (past protection, accuracy and immunity; effect
         // `targeted_by`)
         s.battlers[t].targeted_by = mc.move;
      }
      mc.flags = static_cast<uint8_t>(mc.flags | bit(CURSOR_CHECKED));
      mc.stage = MoveStage::Hit;
      return;
   }
   case MoveStage::Hit:
   {
      const effect::Status st = move_phase(s, effect::Hook::Hit, mc.targets[mc.index]);
      if (!effect::finished(st))
      {
         return;   // the next unit goes on (or the answer to its request)
      }
      if (st == effect::Status::Failed)
      {
         say(s, Msg::Failed, mc.user);
         mc.move = kNone16;   // the move fails: nothing more
         return;
      }
      if (s.outcome != Outcome::None)
      {
         return;   // C5: the battle ended (end_battle stopped the move)
      }
      next_target(s);   // done with this target (an immune target or `stop` end it early)
      return;
   }
   case MoveStage::AfterMove:
   {
      const effect::Status st = move_phase(s, effect::Hook::AfterMove, kBench);
      if (!effect::finished(st) || s.outcome != Outcome::None)
      {
         return;   // C5: a phase that ended the battle ends the unit (no after_move hooks)
      }
      if (alive_active(s, mc.user))
      {
         hook_simple(s, effect::Hook::AfterMove, mc.user, kBench, mc.move);
      }
      finish_move(s);
      return;
   }
   default:
      mc.move = kNone16;
      return;
   }
}

/// Runs the move in progress until a unit queues events, it waits for an answer or it ends.
/// @param s  battle
void advance_move(State& s)
{
   const uint16_t before = s.ev_count;
   while (s.mv.move != kNone16 && s.ev_count == before && !move_asking(s))
   {
      move_unit(s);
   }
}

}   // namespace capsule::battle::detail

// -- public ------------------------------------------------------------------------------------------------

namespace capsule::battle
{

using namespace detail;

bool move_usable(const State& s, uint8_t battler, uint8_t slot)
{
   if (battler >= s.battler_count || slot >= move_slots())
   {
      return false;
   }
   const Battler& b = s.battlers[battler];
   return b.moves[slot] != kNone16 && b.moves[slot] < data().move_count && can_pay(b, b.moves[slot], slot);
}

uint8_t target_choices(const State& s, uint8_t battler, uint16_t move)
{
   if (move >= data().move_count || battler >= s.battler_count)
   {
      return 0;
   }
   const MoveData& m = data().moves[move];
   const uint8_t rch = reach(s, s.battlers[battler].position);
   switch (m.target)
   {
   case Target::Selected:
      return static_cast<uint8_t>((occupied(s, battler, true) | occupied(s, battler, false)) & rch);
   case Target::SelectedFoe:
      return static_cast<uint8_t>(occupied(s, battler, true) & rch);
   case Target::Ally:
      return static_cast<uint8_t>(occupied(s, battler, false) & rch);
   default:
      return 0;
   }
}

}   // namespace capsule::battle
