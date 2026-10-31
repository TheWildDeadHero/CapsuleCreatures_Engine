/// @file
/// Conditions (B-2, D-23 sections 1.5 and 2): the state the engine keeps for them and the moments they change.
/// Primary statuses (inflict / cure), each battler's volatile bank (bits, counters, words; cleared at the end of
/// the turn, on switch-out and on faint as each record says; handed on by a passing switch), side and field
/// conditions with turns and layers, the taken record, and the end-of-turn phases (weather damage, the battlers'
/// residual hooks, side and field residual phases and their countdowns). What a condition does is its owner's
/// effect code: the engine runs its try_start, start, already and end hooks (run_own()) around each change, and
/// its battler hooks through the dispatch (src/battle/effects.cpp).
/// See capsule/battle/battle.h; the modules share internal.h.
#include "internal.h"

namespace capsule::battle::detail
{

/// @name Records (C-07a)
///@{
constexpr int kDefaultStatusBits = 4;         ///< Status id bits of data without a layout (the empty default).
constexpr int32_t kMaxTurns = 0xFF;           ///< Condition turns are bytes.
///@}

/// Bits of the PrimaryStatus role that hold the status id (BattleData::status_bits).
/// @return 1 to kMaxStatusBits
int status_id_bits()
{
   return data().status_bits != 0 ? data().status_bits : kDefaultStatusBits;
}

/// Mask of the status id in the PrimaryStatus role.
/// @return mask
uint32_t status_id_mask()
{
   return (uint32_t{1} << status_id_bits()) - 1u;
}

/// Runs a condition's try_start hook.
/// @param s       battle
/// @param owner   the condition's owner
/// @param holder  battler it would start on (a side condition: one of the side)
/// @param other   its source, or kBench
/// @param turns   the duration it is started with (the hook's value)
/// @return Started, Refused (immune / block) or Failed
Started try_start(State& s, uint16_t owner, uint8_t holder, uint8_t other, int32_t turns)
{
   const HookCtx c = run_own(s, owner, effect::Hook::TryStart, holder, other, turns);
   if (c.stopped == kRunFailed)
   {
      return Started::Failed;
   }
   return c.verdict != kVerdictNone || c.stopped != 0 ? Started::Refused : Started::Started;
}

/// Inflicts a status: refused when the battler has one (the "already" text of its status when @p loud), when
/// the status's try_start hook refuses it (immune: "no effect", block: "blocked" when @p loud) and when a
/// set_status hook of the battler blocks it; then the status is set with its counter and scratch at 0, and its
/// start hook runs (a length's roll, ...).
/// @param s       battle
/// @param target  battler
/// @param status  status index
/// @param source  inflicting battler (kBench for none)
/// @param loud    show the "already" / "no effect" / "blocked" messages (a status move's main effect)
/// @return Started when set; Failed when its try_start failed (the move fails)
Started inflict(State& s, uint8_t target, uint8_t status, uint8_t source, bool loud)
{
   if (status == 0 || status >= data().status_count || !alive_active(s, target))
   {
      return Started::Refused;
   }
   Battler& t = s.battlers[target];
   const StatusData& sd = data().statuses[status];
   if (t.status != 0)
   {
      if (loud)
      {
         emit(s, EventKind::StatusText, target, source, kStatusAlready, t.status);
      }
      return Started::Refused;
   }
   const HookCtx tried = run_own(s, sd.owner, effect::Hook::TryStart, target, source);
   if (tried.stopped == kRunFailed)
   {
      return Started::Failed;
   }
   if (tried.verdict != kVerdictNone || tried.stopped != 0)
   {
      if (loud)
      {
         // immune (immune_types, ...): no effect; any other refusal: blocked
         if (tried.verdict == kVerdictImmune)
         {
            say(s, Msg::NoEffect, source, target);
         }
         else
         {
            say(s, Msg::Blocked, target, source);
         }
      }
      return Started::Refused;
   }
   HookCtx c{target, source, kNone16, status, kVerdictNone, 0, 0};
   run_hook(s, effect::Hook::SetStatus, c);
   if (c.verdict != kVerdictNone)
   {
      if (loud)
      {
         say(s, Msg::Blocked, target, source);
      }
      return Started::Refused;
   }
   t.status = status;
   t.status_counter = 0;
   t.status_aux = 0;
   emit(s, EventKind::Status, target, kBench, 0, status);
   emit(s, EventKind::StatusText, target, source, kStatusInflicted, status);
   run_own(s, sd.owner, effect::Hook::Start, target, source);
   return Started::Started;
}

/// Cures a battler's status (its counter and scratch go back to 0), then runs the status's end hook.
/// @param s      battle
/// @param i      battler
/// @param quiet  no "cured" text
void cure(State& s, uint8_t i, bool quiet)
{
   Battler& b = s.battlers[i];
   if (b.status == 0)
   {
      return;
   }
   const uint8_t old = b.status;
   b.status = 0;
   b.status_counter = 0;
   b.status_aux = 0;
   emit(s, EventKind::Status, i, kBench, 0, 0);
   if (!quiet)
   {
      emit(s, EventKind::StatusText, i, kBench, kStatusCured, old);
   }
   run_own(s, data().statuses[old].owner, effect::Hook::End, i, kBench);
}

/// Emits a Volatile event for a volatile with VOLATILE_HUD.
/// @param s   battle
/// @param i   battler
/// @param v   volatile
/// @param on  on or off
void volatile_event(State& s, uint8_t i, uint8_t v, bool on)
{
   if (has_flag(data().volatiles[v].flags, VOLATILE_HUD))
   {
      emit(s, EventKind::Volatile, i, kBench, on ? 1 : 0, v);
   }
}

/// Starts a volatile (effect `vol_on`): when it is on already its already hook runs (it may fail the run that
/// started it); else its try_start hook may refuse it, then its bit goes on and its start hook runs (ctx.value =
/// @p turns: a duration's roll or count).
/// @param s       battle
/// @param i       battler
/// @param v       volatile
/// @param source  who started it (the hooks' other), or kBench
/// @param turns   duration asked for (0: the volatile's own)
/// @return how it went
Started volatile_on(State& s, uint8_t i, uint8_t v, uint8_t source, int32_t turns)
{
   if (v >= data().volatile_count || i >= s.battler_count)
   {
      return Started::Refused;
   }
   Battler& b = s.battlers[i];
   const VolatileData& vd = data().volatiles[v];
   if (has_flag(b.vol, v))
   {
      // already on: its already hook (a chain's roll may fail the run that started it)
      const HookCtx again = run_own(s, vd.owner, effect::Hook::Already, i, source, turns);
      return again.stopped == kRunFailed ? Started::Failed : Started::Already;
   }
   const Started tried = try_start(s, vd.owner, i, source, turns);
   if (tried != Started::Started)
   {
      return tried;
   }
   b.vol |= bit(v);
   if (vd.source < kMaxBoundVolatiles)
   {
      b.vsource[vd.source] = source;   // G1: a bound volatile remembers who started it
   }
   volatile_event(s, i, v, true);
   run_own(s, vd.owner, effect::Hook::Start, i, source, turns);
   return Started::Started;
}

/// Ends a volatile (effect `vol_off`): its bit goes off and its end hook runs. Its counter and word stay (a
/// chain counter outlives its turn's bit); the record's clears_on events reset them.
/// @param s       battle
/// @param i       battler
/// @param v       volatile
/// @param source  who ended it, or kBench
void volatile_off(State& s, uint8_t i, uint8_t v, uint8_t source)
{
   if (v >= data().volatile_count || i >= s.battler_count || !has_flag(s.battlers[i].vol, v))
   {
      return;
   }
   s.battlers[i].vol &= ~bit(v);
   if (data().volatiles[v].source < kMaxBoundVolatiles)
   {
      s.battlers[i].vsource[data().volatiles[v].source] = kBench;
   }
   volatile_event(s, i, v, false);
   run_own(s, data().volatiles[v].owner, effect::Hook::End, i, source);
}

/// Clears, silently (no hooks, no events), the volatiles whose record has a flag: at the end of the turn only
/// their bits; on switch-out and faint their bits, counters and words.
/// @param b     battler
/// @param when  VOLATILE_TURN, VOLATILE_SWITCH_OUT or VOLATILE_FAINT
void clear_volatiles(Battler& b, VolatileFlag when)
{
   for (int k = 0; k < data().volatile_count; ++k)
   {
      const VolatileData& vd = data().volatiles[k];
      if (!has_flag(vd.flags, when))
      {
         continue;
      }
      b.vol &= ~bit(static_cast<unsigned>(k));
      if (when != VOLATILE_TURN)
      {
         if (vd.counter < kMaxVolatileCounters)
         {
            b.vcounter[vd.counter] = 0;
         }
         if (vd.word < kMaxVolatileWords)
         {
            b.vword[vd.word] = 0;
         }
         if (vd.source < kMaxBoundVolatiles)
         {
            b.vsource[vd.source] = kBench;
         }
      }
   }
}

/// Hands the volatiles with VOLATILE_PASSES (bit, counter, word) of a battler that left to its replacement
/// (effect `switch ... pass`).
/// @param from  the battler as it was before it left
/// @param to    the replacement
void pass_volatiles(const Battler& from, Battler& to)
{
   for (int k = 0; k < data().volatile_count; ++k)
   {
      const VolatileData& vd = data().volatiles[k];
      if (!has_flag(vd.flags, VOLATILE_PASSES) || !has_flag(from.vol, static_cast<unsigned>(k)))
      {
         continue;
      }
      to.vol |= bit(static_cast<unsigned>(k));
      if (vd.counter < kMaxVolatileCounters)
      {
         to.vcounter[vd.counter] = from.vcounter[vd.counter];
      }
      if (vd.word < kMaxVolatileWords)
      {
         to.vword[vd.word] = from.vword[vd.word];
      }
      if (vd.source < kMaxBoundVolatiles)
      {
         to.vsource[vd.source] = from.vsource[vd.source];
      }
   }
}

/// Ends, through volatile_off() (its end hook runs), every bound volatile (VOLATILE_BOUND, G1) that a battler
/// started, on whoever holds it: the battler left the field or fainted.
/// @param s       battle
/// @param source  the battler that left
void end_bound(State& s, uint8_t source)
{
   for (uint8_t i = 0; i < s.battler_count; ++i)
   {
      for (uint8_t v = 0; v < data().volatile_count; ++v)
      {
         const VolatileData& vd = data().volatiles[v];
         if (vd.source < kMaxBoundVolatiles && has_flag(s.battlers[i].vol, v) &&
             s.battlers[i].vsource[vd.source] == source)
         {
            s.battlers[i].vsource[vd.source] = kBench;
            volatile_off(s, i, v, source);
         }
      }
   }
}

/// Hands the bound volatiles (G1) a battler started over to its replacement after a passing switch (effect `switch
/// ... pass`): they now end when the replacement leaves.
/// @param s     battle
/// @param from  the battler that left
/// @param to    its replacement
void rebind_bound(State& s, uint8_t from, uint8_t to)
{
   for (uint8_t i = 0; i < s.battler_count; ++i)
   {
      for (uint8_t v = 0; v < data().volatile_count; ++v)
      {
         const VolatileData& vd = data().volatiles[v];
         if (vd.source < kMaxBoundVolatiles && has_flag(s.battlers[i].vol, v) &&
             s.battlers[i].vsource[vd.source] == from)
         {
            s.battlers[i].vsource[vd.source] = to;
         }
      }
   }
}

/// A turn count as a condition keeps it.
/// @param turns  asked for
/// @return 0 to kMaxTurns
uint8_t turns_byte(int32_t turns)
{
   return static_cast<uint8_t>(clamp_to<int32_t>(turns, 0, kMaxTurns));
}

/// Starts a side condition (effect `side_on`), or adds a layer to it (`add_layer`, up to its record's cap; its
/// already hook runs). A new one passes its try_start hook, then starts with one layer and its start hook runs.
/// @param s          battle
/// @param side       side
/// @param c          side condition
/// @param holder     the battler the effect named (the hooks' holder)
/// @param turns      turns it lasts (0: until it is ended)
/// @param add_layer  add a layer when it is on already
/// @return how it went
Started side_on(State& s, uint8_t side, uint8_t c, uint8_t holder, int32_t turns, bool add_layer)
{
   if (side >= kMaxSides || c >= data().side_cond_count)
   {
      return Started::Refused;
   }
   SideState& st = s.sides[side];
   const SideCondData& cd = data().side_conds[c];
   if (has_flag(st.cond, c))
   {
      if (add_layer && st.layers[c] < cd.max_layers)
      {
         ++st.layers[c];
         emit(s, EventKind::Side, holder, side, st.turns[c], c, st.layers[c]);
      }
      run_own(s, cd.owner, effect::Hook::Already, holder, kBench, turns);
      return Started::Already;
   }
   const Started tried = try_start(s, cd.owner, holder, kBench, turns);
   if (tried != Started::Started)
   {
      return tried;
   }
   st.cond = static_cast<uint16_t>(st.cond | bit(c));
   st.turns[c] = turns_byte(turns);
   st.layers[c] = 1;
   emit(s, EventKind::Side, holder, side, st.turns[c], c, st.layers[c]);
   run_own(s, cd.owner, effect::Hook::Start, holder, kBench, turns);
   return Started::Started;
}

/// Ends a side condition (effect `side_off`, or its turns ran out): its end hook runs.
/// @param s       battle
/// @param side    side
/// @param c       side condition
/// @param holder  a battler of the side for the hook, or kBench
void side_off(State& s, uint8_t side, uint8_t c, uint8_t holder)
{
   if (side >= kMaxSides || c >= data().side_cond_count || !has_flag(s.sides[side].cond, c))
   {
      return;
   }
   SideState& st = s.sides[side];
   st.cond = static_cast<uint16_t>(st.cond & ~bit(c));
   st.turns[c] = 0;
   st.layers[c] = 0;
   emit(s, EventKind::Side, holder, side, 0, c, 0);
   run_own(s, data().side_conds[c].owner, effect::Hook::End, holder, kBench);
}

/// Starts a field condition (effect `field_on`): when it is on already its already hook runs; else its
/// try_start hook may refuse it, then it starts and its start hook runs.
/// @param s       battle
/// @param c       field condition
/// @param holder  the effect's holder (the hooks' holder)
/// @param turns   turns it lasts (0: until it is ended)
/// @return how it went
Started field_on(State& s, uint8_t c, uint8_t holder, int32_t turns)
{
   if (c >= data().field_cond_count)
   {
      return Started::Refused;
   }
   const FieldCondData& cd = data().field_conds[c];
   if (has_flag(s.field.cond, c))
   {
      run_own(s, cd.owner, effect::Hook::Already, holder, kBench, turns);
      return Started::Already;
   }
   const Started tried = try_start(s, cd.owner, holder, kBench, turns);
   if (tried != Started::Started)
   {
      return tried;
   }
   s.field.cond = static_cast<uint16_t>(s.field.cond | bit(c));
   s.field.turns[c] = turns_byte(turns);
   emit(s, EventKind::Field, holder, kBench, s.field.turns[c], c, 1);
   run_own(s, cd.owner, effect::Hook::Start, holder, kBench, turns);
   return Started::Started;
}

/// Ends a field condition (effect `field_off`, or its turns ran out): its end hook runs.
/// @param s       battle
/// @param c       field condition
/// @param holder  a battler for the hook, or kBench
void field_off(State& s, uint8_t c, uint8_t holder)
{
   if (c >= data().field_cond_count || !has_flag(s.field.cond, c))
   {
      return;
   }
   s.field.cond = static_cast<uint16_t>(s.field.cond & ~bit(c));
   s.field.turns[c] = 0;
   emit(s, EventKind::Field, holder, kBench, 0, c, 0);
   run_own(s, data().field_conds[c].owner, effect::Hook::End, holder, kBench);
}

namespace
{

/// Whether a battler's own owners (passive, item, status, volatiles; not the weather or the sides) give a tag of a
/// mask.
/// @param s     battle
/// @param i     battler
/// @param mask  tag bits
/// @return true if one does
bool holds_tag(const State& s, uint8_t i, uint16_t mask)
{
   const Battler& b = s.battlers[i];
   const effect::Owner* o = b.passive < data().passive_count ? owner_at(data().passives[b.passive].owner) : nullptr;
   if (o != nullptr && (o->tags & mask) != 0)
   {
      return true;
   }
   o = live_item(b) != 0 && b.item < data().item_count ? owner_at(data().items[b.item].owner) : nullptr;
   if (o != nullptr && (o->tags & mask) != 0)
   {
      return true;
   }
   o = b.status != 0 && b.status < data().status_count ? owner_at(data().statuses[b.status].owner) : nullptr;
   if (o != nullptr && (o->tags & mask) != 0)
   {
      return true;
   }
   for (uint8_t k = 0; k < data().volatile_count; ++k)
   {
      o = has_flag(b.vol, k) ? owner_at(data().volatiles[k].owner) : nullptr;
      if (o != nullptr && (o->tags & mask) != 0)
      {
         return true;
      }
   }
   return false;
}

} // namespace

uint8_t acting_weather(const State& s)
{
   if (s.weather >= data().weather_count)
   {
      return kNone8;
   }
   const uint16_t mask = data().weather_suppress_tags;
   for (uint8_t i = 0; mask != 0 && i < s.battler_count; ++i)
   {
      if (alive_active(s, i) && holds_tag(s, i, mask))
      {
         return kNone8;   // suppressed: the weather stays, it does not act
      }
   }
   return s.weather;
}

void weather_changed(State& s, uint8_t source, uint8_t now)
{
   for (uint8_t p = 0; p < format(s).positions; ++p)
   {
      const uint8_t i = s.at[p];
      if (i != kBench && alive_active(s, i))
      {
         HookCtx c{i, source, kNone16, now, kVerdictNone, 0, 0};
         run_hook(s, effect::Hook::WeatherChanged, c);
      }
   }
}

/// Sets the weather (effect `weather`; D-23 section 1.5: the exclusive field condition), as field_on() starts a
/// field condition: when it is the weather already its already hook runs; else its try_start hook may refuse it,
/// then the old weather's end hook runs while it is still the weather (field.weather reads it, C4), it is
/// replaced (Weather event, the new one's started text) and the new weather's start hook runs.
/// @param s       battle
/// @param w       weather
/// @param holder  the effect's holder (the hooks' holder)
/// @param turns   turns it lasts (0: until replaced)
/// @return how it went
Started weather_on(State& s, uint8_t w, uint8_t holder, int32_t turns)
{
   if (w >= data().weather_count)
   {
      return Started::Refused;
   }
   const WeatherWatch watch(s, holder);   // C6: weather_changed when the acting weather changes
   const uint16_t owner = data().weathers[w].owner;
   if (s.weather == w)
   {
      run_own(s, owner, effect::Hook::Already, holder, kBench, turns);
      return Started::Already;
   }
   const Started tried = try_start(s, owner, holder, kBench, turns);
   if (tried != Started::Started)
   {
      return tried;
   }
   const uint8_t old = s.weather;
   if (old < data().weather_count)
   {
      // the old weather ends first: its end hook acts on itself (the weather is still it)
      run_own(s, data().weathers[old].owner, effect::Hook::End, holder, kBench);
   }
   s.weather = w;
   s.weather_turns = turns_byte(turns);
   emit(s, EventKind::Weather, holder, kBench, 0, w);
   emit(s, EventKind::WeatherText, holder, kBench, kWeatherStarted, w);
   run_own(s, owner, effect::Hook::Start, holder, kBench, s.weather_turns);
   return Started::Started;
}

/// Ends the weather (its turns ran out), as field_off() ends a field condition: its ended text, the Weather event
/// and its end hook, which reads the weather that ends (field.weather is still it while the hook runs, C4).
/// @param s       battle
/// @param holder  a battler for the hook (condition_holder()), or kBench (no hook runs)
void weather_off(State& s, uint8_t holder)
{
   const uint8_t old = s.weather;
   if (old >= data().weather_count)
   {
      return;
   }
   const WeatherWatch watch(s, holder);
   emit(s, EventKind::WeatherText, kBench, kBench, kWeatherEnded, old);
   s.weather_turns = 0;
   emit(s, EventKind::Weather, kBench, kBench, 0, kNone8);
   run_own(s, data().weathers[old].owner, effect::Hook::End, holder, kBench);
   // the hook may have set another weather (effect `weather`): only the one that ended goes
   if (s.weather == old)
   {
      s.weather = kNone8;
   }
}

/// Keeps the last damage a battler took from a move (the effect refs taken.amount, taken.from, ...).
/// @param s       battle
/// @param target  the battler hit
/// @param from    who hit it
/// @param move    the move
/// @param amount  HP it took
void record_taken(State& s, uint8_t target, uint8_t from, uint16_t move, int32_t amount)
{
   if (target >= s.battler_count || move >= data().move_count)
   {
      return;
   }
   // C5: the category the move in progress hit this target with (hit_category_by_type), else the move's own
   const MoveCursor& mc = s.mv;
   const bool aimed = mc.move == move && mc.user == from && mc.hit_target == target && mc.hit_type != kNone8;
   const uint8_t category = aimed ? mc.hit_category : data().moves[move].category;
   s.battlers[target].taken =
      Taken{static_cast<uint16_t>(clamp_to<int32_t>(amount, 0, kMaxU16)), from, category, move, s.turn};
}

/// Whether the weather has an end-of-turn phase for the battlers (a field_residual handler: residual damage).
/// @param s  battle
/// @return true when it has
bool weather_hurts(const State& s)
{
   const uint8_t w = acting_weather(s);
   if (w >= data().weather_count)
   {
      return false;
   }
   const effect::Owner* o = owner_at(data().weathers[w].owner);
   return o != nullptr && effect::has_hook(*o, effect::Hook::FieldResidual);
}

/// End of turn, the weather's field_residual phase for one battler (residual damage and its text).
/// @param s  battle
/// @param i  battler
void end_turn_weather(State& s, uint8_t i)
{
   const uint8_t w = acting_weather(s);
   if (!alive_active(s, i) || w >= data().weather_count)
   {
      return;
   }
   run_own(s, data().weathers[w].owner, effect::Hook::FieldResidual, i, kBench);
   check_faints(s, kBench);
}

/// End of turn, a battlers phase for one battler: its residual hooks, the phase's sources in the phase's order
/// (the sample's: its status's residual damage first).
/// @param s      battle
/// @param i      battler
/// @param phase  the phase (EndPhaseData::sources)
void end_turn_battler(State& s, uint8_t i, const EndPhaseData& phase)
{
   if (!alive_active(s, i))
   {
      return;
   }
   HookCtx c{i, kBench, kNone16, 0, kVerdictNone, 0, 0};
   run_hook_in(s, effect::Hook::Residual, c, SourceOrder{phase.sources, phase.source_count});
   check_faints(s, kBench);
}

/// The first battler (State::order: fastest first) a side or field condition holds for that is still on the
/// field.
/// @param s        battle
/// @param on_side  a side condition (else a field condition)
/// @param side     its side
/// @return the battler, or kBench
uint8_t condition_holder(const State& s, bool on_side, uint8_t side)
{
   for (int k = 0; k < s.order_count; ++k)
   {
      const uint8_t i = s.order[k];
      if (alive_active(s, i) && (!on_side || side_of(s, i) == side))
      {
         return i;
      }
   }
   return kBench;
}

/// End of turn, a sides or field phase (one unit per call): for each condition that is on (the sides' in side
/// order, or the field's), its residual phase once for each active battler it holds for (its side's, or every
/// one; turn order: State::order) or once (PhaseEach::Once: for the first of them), then its countdown (it ends
/// when its turns run out). State::cond_next is the condition, State::order_next the battler.
/// @param s      battle
/// @param field  the field's conditions (else the sides')
/// @param each   PhaseEach of the phase
/// @return false when every condition is done (the cursors are back at 0)
bool end_turn_conditions(State& s, bool field, uint8_t each)
{
   constexpr int kSideSlots = kMaxSides * kMaxSideConds;
   constexpr int kSlots = kSideSlots + kMaxFieldConds;
   const int last = field ? kSlots : kSideSlots;
   if (field && s.cond_next < kSideSlots)
   {
      s.cond_next = static_cast<uint8_t>(kSideSlots);   // the phase starts: the field's first slot
   }
   while (s.cond_next < last)
   {
      const bool on_side = s.cond_next < kSideSlots;
      const uint8_t side = static_cast<uint8_t>(on_side ? s.cond_next / kMaxSideConds : kNoSide);
      const uint8_t c = static_cast<uint8_t>(on_side ? s.cond_next % kMaxSideConds : s.cond_next - kSideSlots);
      const bool active = on_side ? c < data().side_cond_count && has_flag(s.sides[side].cond, c)
                                  : c < data().field_cond_count && has_flag(s.field.cond, c);
      if (!active)
      {
         s.cond_next = static_cast<uint8_t>(s.cond_next + 1);
         s.order_next = 0;
         continue;
      }
      const uint16_t owner = on_side ? data().side_conds[c].owner : data().field_conds[c].owner;
      const effect::Hook residual = on_side ? effect::Hook::SideResidual : effect::Hook::FieldResidual;
      if (s.order_next < s.order_count)
      {
         // one battler's residual phase of this condition, or its one run for the first of them
         uint8_t i = kBench;
         if (each == static_cast<uint8_t>(PhaseEach::Once))
         {
            i = condition_holder(s, on_side, side);
            s.order_next = s.order_count;
         }
         else
         {
            i = s.order[s.order_next++];
         }
         if (i != kBench && alive_active(s, i) && (!on_side || side_of(s, i) == side))
         {
            run_own(s, owner, residual, i, kBench);
            check_faints(s, kBench);
         }
         return true;
      }
      // its countdown (the hooks' holder: the first of its battlers still on the field)
      const uint8_t holder = condition_holder(s, on_side, side);
      uint8_t& turns = on_side ? s.sides[side].turns[c] : s.field.turns[c];
      if (turns > 0 && --turns == 0)
      {
         if (on_side)
         {
            side_off(s, side, c, holder);
         }
         else
         {
            field_off(s, c, holder);
         }
      }
      s.cond_next = static_cast<uint8_t>(s.cond_next + 1);
      s.order_next = 0;
      return true;
   }
   s.cond_next = 0;
   s.order_next = 0;
   return false;
}

/// End of turn, last: the volatiles that last one turn end (silently).
/// @param s  battle
void end_turn_clear(State& s)
{
   for (uint8_t i = 0; i < s.battler_count; ++i)
   {
      clear_volatiles(s.battlers[i], VOLATILE_TURN);
   }
}

}   // namespace capsule::battle::detail
