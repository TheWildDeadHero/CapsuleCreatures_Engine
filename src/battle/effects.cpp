/// @file
/// Hook dispatch (D-23 section 1.3): engine events run the handlers of a battler's owners through the effect VM,
/// one nested run each, in the manifest's dispatch order (`turn.effect_order`: its primary status, volatiles,
/// its side's and the field's conditions, the weather, its passive and its held item in the sample; an
/// end-of-turn battlers phase may name its own). A battler never re-enters a hook it is running (the guard that
/// kept OnUpdate from reacting to its own heals, generalised); past effect::kMaxEffectDepth nested dispatches
/// nothing runs and State::vm counts a fault. Condition-own hooks (try_start, start, already, end, the residual
/// phases of weathers, side and field conditions) run one owner (run_own()).
/// See capsule/battle/battle.h; the modules share internal.h.
#include "internal.h"

namespace capsule::battle::detail
{

/// Highest fault count (effect::Machine::faults saturates).
constexpr uint16_t kMaxFaults = 0xFFFF;

/// An owner of the battle's effect bank.
/// @param owner  owner index
/// @return the owner, or nullptr
const effect::Owner* owner_at(uint16_t owner)
{
   return owner < data().effects.owner_count ? &data().effects.owners[owner] : nullptr;
}

/// Owners one source can give a battler (the slots source_owner() takes).
/// @param src  source
/// @return 1, or the number of volatiles / side conditions / field conditions
int source_slots(Source src)
{
   switch (src)
   {
   case Source::Volatile:
      return data().volatile_count;
   case Source::SideCond:
      return data().side_cond_count;
   case Source::FieldCond:
      return data().field_cond_count;
   default:
      return 1;
   }
}

/// The slots of a source a battler holds now (bit per slot: its volatiles, its side's or the field's conditions;
/// bit 0 for its status, passive and item).
/// @param s    battle
/// @param i    battler
/// @param src  source
/// @return slot bits
uint32_t source_mask(const State& s, uint8_t i, Source src)
{
   const Battler& b = s.battlers[i];
   switch (src)
   {
   case Source::Status:
      return b.status != 0 ? 1u : 0u;
   case Source::Volatile:
      return b.vol;
   case Source::SideCond:
   {
      const uint8_t side = side_of(s, i);
      return side < kMaxSides ? s.sides[side].cond : 0u;
   }
   case Source::FieldCond:
      return s.field.cond;
   case Source::Weather:
      return acting_weather(s) < data().weather_count ? 1u : 0u;
   case Source::Passive:
      return b.passive != kNone8 ? 1u : 0u;
   case Source::Item:
      return live_item(b) != 0 ? 1u : 0u;
   default:
      return 0u;
   }
}

/// The owner a battler has from one slot of one source (read from the battle as it is now).
/// @param s    battle
/// @param i    battler
/// @param src  source
/// @param k    slot (the volatile / condition index; 0 for the others)
/// @return its owner, or nullptr
const effect::Owner* source_owner(const State& s, uint8_t i, Source src, int k)
{
   const Battler& b = s.battlers[i];
   switch (src)
   {
   case Source::Status:
      return b.status != 0 && b.status < data().status_count ? owner_at(data().statuses[b.status].owner) : nullptr;
   case Source::Volatile:
      return k < data().volatile_count && has_flag(b.vol, static_cast<unsigned>(k))
                ? owner_at(data().volatiles[k].owner)
                : nullptr;
   case Source::SideCond:
   {
      const uint8_t side = side_of(s, i);
      return side < kMaxSides && k < data().side_cond_count && has_flag(s.sides[side].cond, static_cast<unsigned>(k))
                ? owner_at(data().side_conds[k].owner)
                : nullptr;
   }
   case Source::FieldCond:
      return k < data().field_cond_count && has_flag(s.field.cond, static_cast<unsigned>(k))
                ? owner_at(data().field_conds[k].owner)
                : nullptr;
   case Source::Weather:
   {
      const uint8_t w = acting_weather(s);   // C6: a suppressed weather is no source
      return w < data().weather_count ? owner_at(data().weathers[w].owner) : nullptr;
   }
   case Source::Passive:
      return b.passive < data().passive_count ? owner_at(data().passives[b.passive].owner) : nullptr;
   case Source::Item:
      return live_item(b) != 0 && b.item < data().item_count ? owner_at(data().items[b.item].owner) : nullptr;
   default:
      return nullptr;
   }
}

effect::Context hook_context(const State& s, const HookCtx& c, uint16_t owner)
{
   const MoveCursor& mc = s.mv;
   const bool moving = mc.move != kNone16;
   const uint8_t target = moving && mc.index < mc.count ? mc.targets[mc.index] : kBench;
   return effect::Context{c.value, 0, c.move, owner, c.holder, c.other, moving ? mc.user : c.holder,
                          moving ? target : c.other};
}

/// Records a fault of the effect machinery.
/// @param s  battle
/// @param f  what went wrong
void count_fault(State& s, effect::Fault f)
{
   if (s.vm.faults < kMaxFaults)
   {
      ++s.vm.faults;
   }
   s.vm.last_fault = f;
}

/// Opens a dispatch: refused when the battler already runs the hook for the same owner (kNone16: a dispatch over
/// every owner it holds), or past the depth limit (a fault). Keying on the owner lets one condition's own hook
/// run inside another's (a status's end hook ending a volatile runs the volatile's end hook); the depth limit
/// still bounds every chain.
/// @param s        battle
/// @param battler  holder
/// @param h        hook
/// @param owner    the condition-own hook's owner (run_own()), or kNone16
/// @return true when it may run (State::dispatch has its entry on top)
bool open_dispatch(State& s, uint8_t battler, effect::Hook h, uint16_t owner)
{
   for (int k = 0; k < s.dispatch_depth; ++k)
   {
      const Dispatch& d = s.dispatch[k];
      if (d.battler == battler && d.hook == static_cast<uint8_t>(h) && d.owner == owner)
      {
         return false;   // its own handlers' effects do not trigger it again
      }
   }
   if (s.dispatch_depth >= effect::kMaxEffectDepth)
   {
      count_fault(s, effect::Fault::Depth);
      return false;
   }
   s.dispatch[s.dispatch_depth++] = Dispatch{battler, static_cast<uint8_t>(h), 0, 0, kVerdictNone, 0, 0, owner};
   return true;
}

/// Closes the top dispatch and hands its verdict to the hook's context.
/// @param s  battle
/// @param c  the hook
void close_dispatch(State& s, HookCtx& c)
{
   const Dispatch& d = s.dispatch[--s.dispatch_depth];
   c.verdict = d.verdict;
   c.heal_q8 = d.heal_q8;
}

/// A hook dispatch held for a scope: opened on construction when the battler
/// may run the hook (see open_dispatch()), closed when the scope ends on
/// every path, handing its verdict to the hook's context (close_dispatch()).
struct DispatchScope
{
   State& state;    ///< The battle.
   HookCtx& hook;   ///< The hook's context (receives the verdict).
   bool open;       ///< The dispatch opened: the handlers may run.

   /// Opens the dispatch.
   /// @param s        battle
   /// @param c        the hook's context
   /// @param battler  holder
   /// @param h        hook
   /// @param owner    a condition-own hook's owner, or kNone16 (every owner the battler holds)
   DispatchScope(State& s, HookCtx& c, uint8_t battler, effect::Hook h, uint16_t owner = kNone16)
      : state(s), hook(c), open(open_dispatch(s, battler, h, owner))
   {
   }
   DispatchScope(const DispatchScope&) = delete;
   DispatchScope& operator=(const DispatchScope&) = delete;
   /// Closes the dispatch, if it opened.
   ~DispatchScope()
   {
      if (open)
      {
         close_dispatch(state, hook);
      }
   }
};

/// Whether any owner of a battler has a handler for a hook.
/// @param s  battle
/// @param i  battler
/// @param h  hook
/// @return true if one has
bool has_hook(const State& s, uint8_t i, effect::Hook h)
{
   return !for_each_owner(s, i, [h](Source, const effect::Owner& o) { return !effect::has_hook(o, h); });
}

/// Runs the handlers of one owner for a hook in the open dispatch.
/// @param s        battle
/// @param o        the owner
/// @param h        hook
/// @param c        the hook's context
/// @param part     which handlers run (the per_foe mark: a switch-in's per-foe part, a try_switch's parts)
/// @param src      the owner's source (an item that is used up stops its handlers)
/// @return false when a handler stopped
bool run_owner(State& s, const effect::Owner& o, effect::Hook h, HookCtx& c, HandlerPart part, Source src)
{
   const effect::Bank& bank = data().effects;
   const uint16_t owner = static_cast<uint16_t>(&o - bank.owners);
   const uint16_t item = s.battlers[c.holder].item;
   effect::HostState host{s};
   for (int k = 0; k < o.handler_count; ++k)
   {
      const effect::Handler& hn = bank.handlers[o.first_handler + k];
      if ((hn.flags & effect::kHandlerPoint) != 0 || hn.hook != static_cast<uint8_t>(h))
      {
         continue;
      }
      const bool foe_handler = (hn.flags & effect::kHandlerPerFoe) != 0;
      if ((part == HandlerPart::FoesOnly && !foe_handler) || (part == HandlerPart::OwnOnly && foe_handler))
      {
         continue;
      }
      effect::Context ctx = hook_context(s, c, owner);
      const effect::Status st = effect::invoke(s.vm, bank, host, hn.program, ctx);
      c.value = ctx.value;
      Dispatch& d = s.dispatch[s.dispatch_depth - 1];
      if (st == effect::Status::Immune)
      {
         d.verdict = kVerdictImmune;   // the last verdict counts
      }
      else if (st == effect::Status::Blocked)
      {
         d.verdict = kVerdictBlocked;
      }
      else if (st == effect::Status::Stopped || st == effect::Status::Failed)
      {
         c.stopped = st == effect::Status::Failed ? kRunFailed : kRunStopped;
         return false;
      }
      if (src == Source::Item && s.battlers[c.holder].item != item)
      {
         break;   // used up: the rest of its handlers do not run
      }
   }
   return true;
}

/// Runs every source of the holder for a hook in the open dispatch. An owner a handler removed (a status cured,
/// a volatile ended, an item used up) does not run; once the holder faints, the rest do not run either.
/// @param s        battle
/// @param h        hook
/// @param c        the hook's context
/// @param part     which handlers run
/// @param order    the sources in order
void run_sources(State& s, effect::Hook h, HookCtx& c, HandlerPart part, SourceOrder order = dispatch_order())
{
   const bool fainted = has_flag(s.battlers[c.holder].flags, BATTLER_FAINTED);
   for_each_owner(s, c.holder, [&](Source src, const effect::Owner& o)
   {
      if (!effect::has_hook(o, h))
      {
         return true;
      }
      s.dispatch[s.dispatch_depth - 1].source = static_cast<uint8_t>(src);
      return run_owner(s, o, h, c, part, src) &&
             (fainted || !has_flag(s.battlers[c.holder].flags, BATTLER_FAINTED));
   }, order);
}

/// Runs a battler's handlers for one hook, its sources in the dispatch order.
/// @param s        battle
/// @param h        hook
/// @param c        context (holder, other, move, value in; value, verdict, stopped out)
/// @param part     which handlers run (HandlerPart::Every but for the per_foe parts)
void run_hook(State& s, effect::Hook h, HookCtx& c, HandlerPart part)
{
   c.verdict = kVerdictNone;
   c.stopped = 0;
   if (c.holder >= s.battler_count || !has_hook(s, c.holder, h))
   {
      return;
   }
   const DispatchScope dispatch(s, c, c.holder, h);
   if (dispatch.open)
   {
      run_sources(s, h, c, part);
   }
}

/// Runs a battler's handlers for one hook, only the sources @p order lists and in its order (an end-of-turn
/// battlers phase).
/// @param s      battle
/// @param h      hook
/// @param c      context (as run_hook())
/// @param order  the sources in order
void run_hook_in(State& s, effect::Hook h, HookCtx& c, SourceOrder order)
{
   c.verdict = kVerdictNone;
   c.stopped = 0;
   if (c.holder >= s.battler_count || !has_hook(s, c.holder, h))
   {
      return;   // has_hook() looks at every source: a superset of the order's
   }
   const DispatchScope dispatch(s, c, c.holder, h);
   if (dispatch.open)
   {
      run_sources(s, h, c, HandlerPart::Every, order);
   }
}

/// Runs a hook without a value or verdict.
/// @param s       battle
/// @param h       hook
/// @param holder  battler
/// @param other   the other battler, or kBench
/// @param move    move involved, or kNone16
void hook_simple(State& s, effect::Hook h, uint8_t holder, uint8_t other, uint16_t move)
{
   HookCtx c{holder, other, move, 0, kVerdictNone, 0, 0};
   run_hook(s, h, c);
}

/// Runs a condition-own hook (try_start, start, already, end; a residual phase) of one owner for a holder.
/// @param s       battle
/// @param owner   owner index
/// @param h       hook
/// @param holder  battler (a side condition: one of its side)
/// @param other   the other battler, or kBench
/// @param value   the hook's value (ctx.value)
/// @param move    the run's move (the Item action's move-slot target, G3), or kNone16
/// @return the hook's context (verdict, stopped)
HookCtx run_own(State& s, uint16_t owner, effect::Hook h, uint8_t holder, uint8_t other, int32_t value,
                uint16_t move)
{
   HookCtx c{holder, other, move, value, kVerdictNone, 0, 0};
   const effect::Owner* o = owner_at(owner);
   if (o == nullptr || !effect::has_hook(*o, h) || holder >= s.battler_count)
   {
      return c;   // no handler, or nobody to hold it (a condition ending with no battler on the field)
   }
   {
      const DispatchScope dispatch(s, c, holder, h, owner);
      if (dispatch.open)
      {
         run_owner(s, *o, h, c, HandlerPart::Every, Source::Status);
      }
   }
   return c;
}

/// Runs a battler's switch_in handlers. They run once for the battler (ctx.value kPartUser, other = the first
/// foe on the field); the handlers flagged kHandlerPerFoe then run once per foe on the field (kPartFoe), all
/// in one dispatch (the owner is announced once).
/// @param s  battle
/// @param i  battler
void switch_in_hooks(State& s, uint8_t i)
{
   if (!alive_active(s, i) || !has_hook(s, i, effect::Hook::SwitchIn))
   {
      return;
   }
   HookCtx c{i, kBench, kNone16, effect::kPartUser, kVerdictNone, 0, 0};
   for (uint8_t p = 0; p < format(s).positions; ++p)
   {   // "other" = first foe on the field
      const uint8_t o = s.at[p];
      if (o != kBench && alive_active(s, o) && side_of(s, o) != side_of(s, i))
      {
         c.other = o;
         break;
      }
   }
   {   // one dispatch for the battler's part and every foe's part
      const DispatchScope dispatch(s, c, i, effect::Hook::SwitchIn);
      if (!dispatch.open)
      {
         return;
      }
      run_sources(s, effect::Hook::SwitchIn, c, HandlerPart::Every);

      // Handlers aimed at foes reach every foe on the field.
      bool per_foe = false;
      for_each_owner(s, i, [&per_foe](Source, const effect::Owner& o)
      {
         for (int k = 0; k < o.handler_count; ++k)
         {
            const effect::Handler& hn = data().effects.handlers[o.first_handler + k];
            per_foe = per_foe || (hn.hook == static_cast<uint8_t>(effect::Hook::SwitchIn) &&
                                  (hn.flags & effect::kHandlerPerFoe) != 0);
         }
         return true;
      });
      for (uint8_t p = 0; per_foe && p < format(s).positions; ++p)
      {
         const uint8_t o = s.at[p];
         if (o == kBench || !alive_active(s, o) || side_of(s, o) == side_of(s, i))
         {
            continue;
         }
         c.other = o;
         c.value = effect::kPartFoe;
         run_sources(s, effect::Hook::SwitchIn, c, HandlerPart::FoesOnly);
      }
   }
   check_faints(s, kBench);
}

}   // namespace capsule::battle::detail
