/// @file
/// The effect VM's host (capsule/battle/effect.h): what effect code reads, writes, draws and does in a battle,
/// and what generated point handlers read (effect::point_get / point_has): battlers (with their volatile bank and
/// taken record), the move, the sides' and the field's conditions. Reads of things this engine does not keep yet
/// (move data columns) give 0.
/// See capsule/battle/battle.h; the modules share internal.h.
#include "internal.h"

#include "capsule/battle/encounter_lead.h"
#include "capsule/script/field_effects.h"

namespace capsule::battle::detail
{

/// @name Values
///@{
constexpr int32_t kMaxCounter = 0xFF;        ///< Counters, condition turns and status scratch are bytes.
constexpr int32_t kMaxText = 0xFFFF;         ///< Event values (and volatile words) are 16 bits.
constexpr uint32_t kRewardPoolMax = UINT32_MAX;   ///< State::reward saturates here (effect `reward`, E13b).
///@}

/// The battler a place names, or kBench.
/// @param s   battle
/// @param at  the place
/// @return battler index, or kBench
uint8_t place_battler(const State& s, const effect::Place& at)
{
   return (at.kind == effect::PlaceKind::Battler || at.kind == effect::PlaceKind::Side) &&
                at.battler < s.battler_count
             ? at.battler
             : kBench;
}

/// The tags a battler's owners give it.
/// @param s  battle
/// @param i  battler
/// @return tag bits
uint16_t tags_of(const State& s, uint8_t i)
{
   uint16_t t = 0;
   for_each_owner(s, i, [&t](Source, const effect::Owner& o)
   {
      t = static_cast<uint16_t>(t | o.tags);
      return true;
   });
   return t;
}

/// Reads a battler (shared by the VM's host_get and the point handlers' point_get).
/// @param s     battle
/// @param who   battler, or kBench
/// @param ref   what
/// @param arg   stat, stage, slot or resource
/// @param move  the context move (a move resource's pool is the slot holding it)
/// @return the value ("none" for ids and 0 for numbers without a battler)
int32_t battler_read(const State& s, uint8_t who, effect::Ref ref, uint8_t arg, uint16_t move)
{
   using effect::Ref;
   if (who >= s.battler_count)
   {
      return ref == Ref::Status || ref == Ref::Passive || ref == Ref::Type ? kNone8
           : ref == Ref::Item || ref == Ref::Species || ref == Ref::LastMove ? kNone16 : 0;
   }
   const Battler& b = s.battlers[who];
   switch (ref)
   {
   case Ref::Hp:
      return b.hp;
   case Ref::MaxHp:
      return max_hp(b);
   case Ref::Level:
      return b.level;
   case Ref::Stat:
      return arg < data().stat_count ? b.stats[arg] : 0;
   case Ref::Stage:
      return arg < data().stage_count ? b.stages[arg] : 0;
   case Ref::Status:
      // statuses are named by their manifest index (engine index 0 is "none")
      return b.status != 0 ? b.status - 1 : kNone8;
   case Ref::StatusCounter:
      return b.status_counter;
   case Ref::StatusAux:
      return b.status_aux;
   case Ref::Counter:
      return arg < kMaxVolatileCounters ? b.vcounter[arg] : 0;
   case Ref::Word:
      return arg < kMaxVolatileWords ? b.vword[arg] : 0;
   case Ref::TakenAmount:
      return b.taken.amount;
   case Ref::TakenFrom:
      return b.taken.from;
   case Ref::TakenMove:
      return b.taken.move;
   case Ref::TakenCategory:
      return b.taken.category;
   case Ref::TakenTurn:
      return b.taken.turn;
   case Ref::Type:
      return arg < kMaxTypesPer ? b.types[arg] : kNone8;
   case Ref::Passive:
      return b.passive;
   case Ref::Item:
      // items too (engine index 0 is "none")
      return b.item != 0 ? b.item - 1 : kNone16;
   case Ref::Species:
      return b.species;
   case Ref::LastMove:
      return b.last_move;
   case Ref::Position:
      return b.position;
   case Ref::Side:
      return side_of(s, who);
   case Ref::Party:
      return b.party;
   case Ref::Alive:
      return b.hp > 0 ? 1 : 0;
   case Ref::CanSwitch:
      return next_bench(s, b.party) != kBench ? 1 : 0;
   case Ref::Acted:
      return has_flag(b.flags, BATTLER_ACTED) ? 1 : 0;
   case Ref::SpeciesField:
      // E13b: the species' data column (structure species_fields; the generated table)
      return rules::species_field_of(b.species, arg);
   case Ref::TargetedBy:
      return b.targeted_by;
   case Ref::DelayedTurns:
      // the pending effect at its position (none off the field)
      return b.position < kMaxPositions ? s.delayed[b.position].turns : 0;
   case Ref::Human:
      return s.parties[b.party].controller == Controller::Human ? 1 : 0;
   case Ref::Unowned:
      return has_flag(s.parties[b.party].flags, PARTY_UNOWNED) ? 1 : 0;
   case Ref::Gene:
      return arg < data().stat_count && arg < kMaxStats ? b.genes[arg] : 0;
   case Ref::PlayerSide:
      return side_of(s, who) == player_side(s) ? 1 : 0;
   case Ref::SideActive:
   {
      // battlers active on its side (point handlers read it here; the VM through host_get())
      int n = 0;
      for (uint8_t p = 0; p < format(s).positions; ++p)
      {
         n += s.at[p] != kBench && alive_active(s, s.at[p]) && side_of(s, s.at[p]) == side_of(s, who) ? 1 : 0;
      }
      return n;
   }
   case Ref::Resource:
   case Ref::ResourceMax:
   {
      // D-25: a battler pool, or the context move's slot of a move pool (0 when it holds no such move)
      const ResourceData* d = resource_at(arg);
      if (d == nullptr)
      {
         return 0;
      }
      const int slot = is_move_resource(*d) ? slot_of(b, move) : 0;
      return ref == Ref::Resource ? value_of(b, *d, slot) : max_of(b, *d, slot);
   }
   default:
      return 0;   // not kept by this engine yet (move data columns)
   }
}

/// A membership test of a battler (shared by host_has and point_has).
/// @param s     battle
/// @param who   battler, or kBench
/// @param kind  what kind of id
/// @param id    the id
/// @param move  the context move (move flags)
/// @return true when present
bool battler_has(const State& s, uint8_t who, effect::HasKind kind, uint8_t id, uint16_t move)
{
   using effect::HasKind;
   if (kind == HasKind::MoveFlag)
   {
      return move < data().move_count && id < kMaxMoveFlags && has_flag(data().moves[move].flags, id);
   }
   if (kind == HasKind::FieldCond)
   {
      return id < data().field_cond_count && has_flag(s.field.cond, id);
   }
   if (who >= s.battler_count)
   {
      return false;
   }
   const Battler& b = s.battlers[who];
   switch (kind)
   {
   case HasKind::Status:
      return b.status != 0 && b.status - 1 == id;
   case HasKind::Volatile:
      return id < data().volatile_count && has_flag(b.vol, id);
   case HasKind::Type:
      return has_type(b, id);
   case HasKind::Tag:
      return id < kMaxTags && has_flag(tags_of(s, who), id);
   case HasKind::SideCond:
   {
      // a battler subject names its side
      const uint8_t side = side_of(s, who);
      return id < data().side_cond_count && side < kMaxSides && has_flag(s.sides[side].cond, id);
   }
   default:
      return false;
   }
}

/// A membership test of a place (host_has): a battler's, its side's (side and field conditions take a battler or
/// a side subject) or the field's.
/// @param s     battle
/// @param at    the place
/// @param kind  what kind of id
/// @param id    the id
/// @param move  the context move (move flags)
/// @return true when present
bool place_has(const State& s, const effect::Place& at, effect::HasKind kind, uint8_t id, uint16_t move)
{
   using effect::HasKind;
   const bool side_kind = kind == HasKind::SideCond;
   const uint8_t who = at.kind == effect::PlaceKind::Battler || (side_kind && at.kind == effect::PlaceKind::Side)
                          ? place_battler(s, at)
                          : kBench;
   return battler_has(s, who, kind, id, move);
}

/// Reads of the sides' and the field's conditions (host_get, point_get).
/// @param s      battle
/// @param who    a battler of the side (side refs), or kBench
/// @param ref    what
/// @param arg    the condition
/// @param found  set when @p ref is one of them
/// @return the value
int32_t condition_read(const State& s, uint8_t who, effect::Ref ref, uint8_t arg, bool& found)
{
   using effect::Ref;
   found = ref == Ref::SideTurns || ref == Ref::SideLayers || ref == Ref::FieldTurns;
   if (ref == Ref::FieldTurns)
   {
      return arg < data().field_cond_count ? s.field.turns[arg] : 0;
   }
   if (!found || who >= s.battler_count)
   {
      return 0;
   }
   const uint8_t side = side_of(s, who);
   if (side >= kMaxSides || arg >= data().side_cond_count)
   {
      return 0;
   }
   return ref == Ref::SideTurns ? s.sides[side].turns[arg] : s.sides[side].layers[arg];
}

/// A read of a place that is not a battler's own (side and field condition refs).
/// @param s      battle
/// @param at     the place
/// @param ref    what
/// @param arg    the condition
/// @param found  set when @p ref is one of them
/// @return the value
int32_t place_read(const State& s, const effect::Place& at, effect::Ref ref, uint8_t arg, bool& found)
{
   const bool battler = at.kind == effect::PlaceKind::Battler || at.kind == effect::PlaceKind::Side;
   return condition_read(s, battler ? place_battler(s, at) : kBench, ref, arg, found);
}

/// A move's data, or nullptr.
/// @param move  move index
/// @return the move
const MoveData* move_data(uint16_t move)
{
   return move < data().move_count ? &data().moves[move] : nullptr;
}

/// Reads of the context move, the field and the turn (shared by host_get and point_get).
/// @param s         battle
/// @param move      the context move
/// @param ref       what
/// @param arg       resource (MoveBase, MoveCost)
/// @param out       the value
/// @param type      the type the move hits with (C5: MoveCursor::hit_type, PipelineCtx::move_type), or kNone8 for
///                  its own
/// @param category  the category it hits with, or kNone8 for its own
/// @return false when @p ref is not one of them
bool context_read(const State& s, uint16_t move, effect::Ref ref, uint8_t arg, int32_t& out, uint8_t type,
                  uint8_t category)
{
   using effect::Ref;
   const MoveData* m = move_data(move);
   switch (ref)
   {
   case Ref::MoveType:
      out = type != kNone8 ? type : (m != nullptr ? m->type : kNone8);
      return true;
   case Ref::MoveCategory:
      out = category != kNone8 ? category : (m != nullptr ? m->category : kNone8);
      return true;
   case Ref::MovePower:
      out = m != nullptr ? m->power : 0;
      return true;
   case Ref::MoveAccuracy:
      out = m != nullptr ? m->accuracy : 0;
      return true;
   case Ref::MovePriority:
      out = m != nullptr ? m->priority : 0;
      return true;
   case Ref::MoveBase:
      out = move_base(move, arg);
      return true;
   case Ref::MoveCost:
   {
      // the move's fixed cost of a resource (0: none, or a formula cost)
      out = 0;
      for (int k = 0; m != nullptr && k < m->cost_count; ++k)
      {
         const MoveCost& c = data().move_costs[m->cost_first + k];
         if (c.resource == arg && c.formula == kNone8)
         {
            out = c.amount;
         }
      }
      return true;
   }
   case Ref::WeatherId:
      out = acting_weather(s);   // C6: none while it is suppressed
      return true;
   case Ref::WeatherTurns:
      out = s.weather_turns;
      return true;
   case Ref::Turn:
      out = s.turn;
      return true;
   default:
      return false;
   }
}

/// A subject battler's place in the format: the battlers a battler set holds.
/// @param s    battle
/// @param set  which set
/// @param i    relative to this battler
/// @return battler bit mask
uint32_t battlers_of(const State& s, effect::BattlerSet set, uint8_t i)
{
   using effect::BattlerSet;
   uint32_t mask = 0;
   if (i >= s.battler_count)
   {
      return 0;
   }
   const uint8_t near = reach(s, s.battlers[i].position);
   for (uint8_t p = 0; p < format(s).positions; ++p)
   {
      const uint8_t o = s.at[p];
      if (o == kBench || !alive_active(s, o))
      {
         continue;
      }
      const bool self = o == i;
      const bool foe = side_of(s, o) != side_of(s, i);
      bool in = false;
      switch (set)
      {
      case BattlerSet::Foes:
         in = foe;
         break;
      case BattlerSet::AdjacentFoes:
         in = foe && ((near >> p) & 1) != 0;
         break;
      case BattlerSet::Allies:
         in = !foe && !self;
         break;
      case BattlerSet::Others:
         in = !self;
         break;
      default:
         in = true;
         break;
      }
      mask |= in ? bit(o) : 0;
   }
   return mask;
}

/// The party record of a battler (data members have none: their roles read the initial values).
/// @param s     battle
/// @param i     battler
/// @return the record, or nullptr for a battler without one
monster::Record* record_of(State& s, uint8_t i)
{
   if (i >= s.battler_count || !has_flag(s.battlers[i].flags, BATTLER_FROM_RECORD))
   {
      return nullptr;
   }
   PartyState& p = s.parties[s.battlers[i].party];
   return p.records != nullptr ? &p.records->members[s.battlers[i].party_slot] : nullptr;
}

/// The battler of the top hook dispatch's announcement, if it announces now.
/// @param s      battle
/// @param r      the running run (its owner)
void announce(State& s, const effect::EffectRun& r)
{
   const effect::Owner* o = owner_at(r.ctx.owner);
   if (s.dispatch_depth == 0 || o == nullptr || (o->flags & effect::kOwnerAnnounce) == 0)
   {
      return;   // only an announcing owner's hook handlers announce, once per dispatch
   }
   Dispatch& d = s.dispatch[s.dispatch_depth - 1];
   const uint8_t mine = static_cast<uint8_t>(bit(d.source));
   if ((d.announced & mine) != 0 || d.battler >= s.battler_count)
   {
      return;
   }
   d.announced = static_cast<uint8_t>(d.announced | mine);
   const Battler& b = s.battlers[d.battler];
   if (d.source == static_cast<uint8_t>(Source::Item))
   {
      say(s, Msg::ItemUsed, d.battler, kBench, b.item);
   }
   else if (d.source == static_cast<uint8_t>(Source::Passive))
   {
      say(s, Msg::Passive, d.battler, kBench, b.passive);
   }
}

/// Puts a landing delayed hit's snapshot (DelayedSnapshot; G1) in place of its user's values for the scope: the
/// landing's accuracy and damage pipelines read what its record (DelayedHitData::at_use) takes from the time the
/// move was used. The user's own values come back when the scope ends (only pipelines run inside it: they change
/// nothing).
struct SnapshotScope
{
   Battler& user;                        ///< The delayed hit's user.
   Battler& target;                      ///< The battler it lands on.
   uint8_t at_use;                       ///< DelayedAtUse bits put in place.
   uint8_t defense;                      ///< The target's stat that resists the move's category, or kNone8.
   uint16_t stats[kMaxStats];            ///< The user's own stats while the snapshot's are in place.
   int8_t stages[kMaxStages];            ///< Its own stages.
   uint8_t types[kMaxTypesPer];          ///< Its own types.
   uint8_t level;                        ///< Its own level.
   uint16_t defense_value;               ///< The target's own value of that stat.
   int8_t defense_stage;                 ///< ...and its stage.

   /// Swaps the snapshot's values in.
   /// @param s             battle (State::landing names the delayed hit)
   /// @param user_index    the user battler
   /// @param target_index  the battler it lands on
   SnapshotScope(State& s, uint8_t user_index, uint8_t target_index)
      : user(s.battlers[user_index]), target(s.battlers[target_index]), at_use(0), defense(kNone8), stats{},
        stages{}, types{}, level(0), defense_value(0), defense_stage(0)
   {
      if (s.landing >= kMaxPositions || s.delayed[s.landing].snap.rule >= data().delayed_hit_count)
      {
         return;
      }
      const Delayed& d = s.delayed[s.landing];
      at_use = data().delayed_hits[d.snap.rule].at_use;
      defense = data().categories[data().moves[d.move].category].defense;
      swap(d.snap);
   }
   SnapshotScope(const SnapshotScope&) = delete;
   SnapshotScope& operator=(const SnapshotScope&) = delete;
   /// Puts the user's own values back.
   ~SnapshotScope()
   {
      restore();
   }

   /// Keeps the user's values and puts the snapshot's in place.
   /// @param snap  the snapshot
   void swap(const DelayedSnapshot& snap)
   {
      for (int k = 0; k < kMaxStats; ++k)
      {
         stats[k] = user.stats[k];
         user.stats[k] = has_flag(at_use, DELAYED_STATS) ? snap.stats[k] : user.stats[k];
      }
      for (int k = 0; k < kMaxStages; ++k)
      {
         stages[k] = user.stages[k];
         user.stages[k] = has_flag(at_use, DELAYED_STAGES) ? snap.stages[k] : user.stages[k];
      }
      for (int k = 0; k < kMaxTypesPer; ++k)
      {
         types[k] = user.types[k];
         user.types[k] = has_flag(at_use, DELAYED_TYPES) ? snap.types[k] : user.types[k];
      }
      level = user.level;
      user.level = has_flag(at_use, DELAYED_LEVEL) ? snap.level : user.level;
      const uint8_t stage = defense < data().stat_count ? stage_of(defense) : kNone8;
      if (has_flag(at_use, DELAYED_TARGET_DEFENSE) && defense < data().stat_count)
      {
         // the battler there now resists as the one there when the move was used did
         defense_value = target.stats[defense];
         target.stats[defense] = snap.defense;
         if (stage < data().stage_count)
         {
            defense_stage = target.stages[stage];
            target.stages[stage] = snap.defense_stage;
         }
      }
   }

   /// Puts the user's own values back (nothing when no snapshot went in).
   void restore()
   {
      if (at_use == 0)
      {
         return;
      }
      if (has_flag(at_use, DELAYED_TARGET_DEFENSE) && defense < data().stat_count)
      {
         // undone in the reverse order of swap(): the target's first (it may be the user itself)
         target.stats[defense] = defense_value;
         const uint8_t stage = stage_of(defense);
         if (stage < data().stage_count)
         {
            target.stages[stage] = defense_stage;
         }
      }
      for (int k = 0; k < kMaxStats; ++k)
      {
         user.stats[k] = stats[k];
      }
      for (int k = 0; k < kMaxStages; ++k)
      {
         user.stages[k] = stages[k];
      }
      for (int k = 0; k < kMaxTypesPer; ++k)
      {
         user.types[k] = types[k];
      }
      user.level = level;
      at_use = 0;
   }
};

/// The record of the delayed hit landing now (G1; State::landing), or nullptr outside a landing.
/// @param s  battle
/// @return the record
const DelayedHitData* landing_hit(const State& s)
{
   if (s.landing >= kMaxPositions || s.delayed[s.landing].snap.rule >= data().delayed_hit_count)
   {
      return nullptr;
   }
   return &data().delayed_hits[s.delayed[s.landing].snap.rule];
}

/// A landing delayed hit's accuracy check (G1: its record's accuracy, 0 for none): the target's before_accuracy
/// hooks (protection, a semi-invulnerable state), then the accuracy pipeline with the user's snapshot.
/// @param s     battle
/// @param dh    the delayed hit's record
/// @param user  its user
/// @param t     the battler at the position
/// @param move  its move
/// @return true when it hits (a miss said its record's message, or a hook said why)
bool landing_hits(State& s, const DelayedHitData& dh, uint8_t user, uint8_t t, uint16_t move)
{
   if (dh.accuracy == 0)
   {
      return true;
   }
   HookCtx guard{t, user, move, 0, kVerdictNone, 0, 0};
   run_hook(s, effect::Hook::BeforeAccuracy, guard);
   if (guard.verdict == kVerdictBlocked || !alive_active(s, t))
   {
      return false;   // the handler said why
   }
   bool hit = false;
   {
      const SnapshotScope snap(s, user, t);
      hit = accuracy_at(s, &s.rng, user, t, move, dh.accuracy).result != 0;
   }
   if (!hit && dh.missed < data().message_count)
   {
      say(s, static_cast<Msg>(dh.missed), user, t);
   }
   return hit;
}

/// One hit of the move in progress on its current target (effect `damage`): the decisive tiers and pipelines
/// (calc_damage), the target's before_damage hooks, the HP, the taken record, the messages, the target's
/// damaging_hit hooks, the faint check and the user's after_hit hooks. A delayed hit's landing (G1: the move's
/// land phase, State::landing) first checks its accuracy, may hit while its user is off the field and reads the
/// user's snapshot (SnapshotScope); its crit pipeline runs only when its record says so.
/// @param s  battle
/// @param r  the move's run
/// @return dealt; stop when the target is immune (or a landing missed)
effect::Outcome hit_once(State& s, const effect::EffectRun& r)
{
   MoveCursor& mc = s.mv;
   const uint8_t user = r.ctx.user;
   const uint8_t t = r.ctx.target;
   const uint16_t move = r.ctx.move;
   const DelayedHitData* landing = landing_hit(s);
   if (move >= data().move_count || mc.move != move || user >= s.battler_count || !alive_active(s, t) ||
       (landing == nullptr && !alive_active(s, user)))
   {
      return effect::Outcome{0, false, false, false};
   }
   if (landing != nullptr && !landing_hits(s, *landing, user, t, move))
   {
      return effect::Outcome{0, false, true, false};   // missed: the land phase goes no further
   }
   const CritMode crit = landing == nullptr || has_flag(landing->flags, DELAYED_CRIT) ? CritMode::Roll
                                                                                      : CritMode::Never;
   Hit hit{};
   {
      const SnapshotScope snap(s, user, t);
      hit = calc_damage(s, user, t, move, has_flag(mc.flags, CURSOR_SPREAD), -1, crit);
   }
   if (hit.zero)
   {
      say_tier(s, data().tiers[hit.type.decisive].message, user, t);
      return effect::Outcome{0, false, true, false};   // immune: the move goes on to its other targets
   }
   // E13b: the hit's crit flag is set before the before_damage hooks run, so they can read `crit`
   mc.flags = static_cast<uint8_t>(hit.crit ? mc.flags | bit(CURSOR_CRIT) : mc.flags & ~bit(CURSOR_CRIT));
   mc.flags = static_cast<uint8_t>(mc.flags & ~bit(CURSOR_ABSORBED));   // G1: a decoy's handler marks it (absorb)
   HookCtx before{t, user, move, hit.damage, kVerdictNone, 0, 0};
   run_hook(s, effect::Hook::BeforeDamage, before);
   const int32_t damage = before.verdict == kVerdictBlocked ? 0 : clamp_to<int32_t>(before.value, 0, kMaxU16);
   const int32_t hp = s.battlers[t].hp;
   set_hp(s, t, hp - damage);
   const int32_t dealt = hp - s.battlers[t].hp;
   s.battlers[t].last_hit_by = move;
   if (dealt > 0)
   {
      record_taken(s, t, user, move, dealt);
   }
   mc.hits = static_cast<uint8_t>(mc.hits < kCountMax8 ? mc.hits + 1 : kCountMax8);
   if (hit.crit)
   {
      say(s, Msg::Critical, user, t);
   }
   if (!has_flag(mc.flags, CURSOR_SHOWN))
   {
      mc.flags = static_cast<uint8_t>(mc.flags | bit(CURSOR_SHOWN));
      say_effectiveness(s, hit, user, t);
   }
   if (alive_active(s, t))
   {
      HookCtx c{t, user, move, dealt, kVerdictNone, 0, 0};
      run_hook(s, effect::Hook::DamagingHit, c);
   }
   check_faints(s, user);
   if (alive_active(s, user))
   {
      HookCtx c{user, t, move, dealt, kVerdictNone, 0, 0};
      run_hook(s, effect::Hook::AfterHit, c);
   }
   return effect::Outcome{dealt, false, false, false};
}

/// Direct HP damage (effect `damage_hp`). `typed`: a hit of the move (type immunity, then the target's
/// before_damage hooks with the amount as their value (E13b: a decoy or an endure-style effect sees fixed,
/// counter and one-hit-knockout damage too), the move's dealt, the faint check credits the user);
/// `no_faint_check`: the faint check waits for the end of the unit.
/// @param s  battle
/// @param r  the run
/// @param a  the primitive
/// @return dealt; stop when a typed hit finds the target immune
effect::Outcome damage_hp(State& s, effect::EffectRun& r, const effect::Action& a)
{
   const uint8_t i = place_battler(s, a.at);
   if (i == kBench || !alive_active(s, i))
   {
      return effect::Outcome{0, false, false, false};
   }
   const bool typed = (a.flags & effect::kDamageTyped) != 0;
   const MoveData* m = move_data(r.ctx.move);
   if (typed && m != nullptr)
   {
      const TypeEffect fx = type_effect(hit_type_of(s, r.ctx.user, i, r.ctx.move), s.battlers[i]);
      if (fx.kind == static_cast<uint8_t>(TierKind::Zero))
      {
         if ((a.flags & effect::kDamageQuiet) == 0)
         {
            say_tier(s, data().tiers[fx.decisive].message, r.ctx.user, i);
         }
         return effect::Outcome{0, false, true, false};
      }
   }
   int32_t amount = clamp_to<int32_t>(a.args[0], 0, kMaxU16);
   if (typed)
   {
      // C5: not a hit of the damage pipeline: no critical hit (a `damage` before it may have left the flag set)
      s.mv.flags = static_cast<uint8_t>(s.mv.flags & ~(bit(CURSOR_CRIT) | bit(CURSOR_ABSORBED)));
      HookCtx before{i, r.ctx.user, r.ctx.move, amount, kVerdictNone, 0, 0};
      run_hook(s, effect::Hook::BeforeDamage, before);
      amount = before.verdict == kVerdictBlocked ? 0 : clamp_to<int32_t>(before.value, 0, kMaxU16);
      if (!alive_active(s, i))
      {
         return effect::Outcome{0, false, false, false};   // a before_damage handler took it off the field
      }
   }
   const int32_t hp = s.battlers[i].hp;
   set_hp(s, i, hp - amount);
   const int32_t dealt = hp - s.battlers[i].hp;
   if (typed)
   {
      s.battlers[i].last_hit_by = r.ctx.move;
      if (dealt > 0)
      {
         record_taken(s, i, r.ctx.user, r.ctx.move, dealt);
      }
      if (s.mv.move != kNone16)
      {
         s.mv.run.ctx.dealt = effect::sat_add(s.mv.run.ctx.dealt, dealt);
      }
   }
   if ((a.flags & effect::kDamageNoFaintCheck) == 0)
   {
      check_faints(s, typed ? r.ctx.user : kBench);
   }
   return effect::Outcome{dealt, false, false, false};
}

/// A switch (effect `switch`): the battler leaves for the replacement (Pass: with its stages and its volatiles
/// with VOLATILE_PASSES).
/// @param s  battle
/// @param a  the primitive (args[0]: the replacement, or kHostPicks)
void switch_out(State& s, const effect::Action& a)
{
   const uint8_t i = place_battler(s, a.at);
   if (i == kBench || !alive_active(s, i))
   {
      return;
   }
   int32_t to = a.args[0];
   if (to == effect::kHostPicks)
   {
      // a random replacement from the bench
      uint8_t list[kMaxBattlers];
      int n = 0;
      const PartyState& p = s.parties[s.battlers[i].party];
      for (int k = 0; k < p.count; ++k)
      {
         const uint8_t c = static_cast<uint8_t>(p.first_battler + k);
         if (can_switch_to(s, i, c) && !pending_send(s, c))
         {
            list[n++] = c;
         }
      }
      to = n > 0 ? list[rng::below(s.rng, static_cast<uint32_t>(n))] : kBench;
   }
   if (to < 0 || to >= s.battler_count || !can_switch_to(s, i, static_cast<uint8_t>(to)))
   {
      return;
   }
   const uint8_t pos = s.battlers[i].position;
   const Battler before = s.battlers[i];   // what a passing switch hands on (withdraw() resets it)
   const bool passing = static_cast<effect::SwitchMode>(a.id) == effect::SwitchMode::Pass;
   if (passing)
   {
      rebind_bound(s, i, static_cast<uint8_t>(to));   // G1: what it started stays, bound to the replacement
   }
   withdraw(s, i);
   Battler& nb = s.battlers[to];
   if (passing)
   {
      for (int k = 0; k < kMaxStages; ++k)
      {
         nb.stages[k] = before.stages[k];
      }
      pass_volatiles(before, nb);
   }
   send_in(s, static_cast<uint8_t>(to), pos);
}

/// A request (effect `ask`): an AI party answers at once; a human's run waits for choose().
/// @param s  battle
/// @param r  the run
/// @param a  the primitive
/// @return the answer, or suspend
effect::Outcome ask(State& s, const effect::EffectRun& r, const effect::Action& a)
{
   const uint8_t i = place_battler(s, a.at);
   const auto kind = static_cast<effect::AskKind>(a.id);
   if (i == kBench || &r != &s.mv.run || s.parties[s.battlers[i].party].controller != Controller::Human)
   {
      const int32_t v = kind == effect::AskKind::SwitchTarget && i != kBench ? next_bench(s, s.battlers[i].party) : 0;
      return effect::Outcome{v, false, false, false};
   }
   const RequestKind k = kind == effect::AskKind::SwitchTarget ? RequestKind::SwitchTarget
                       : kind == effect::AskKind::LearnMove    ? RequestKind::LearnMove
                                                               : RequestKind::Choice;
   s.mv.ask = static_cast<uint8_t>(k);
   s.mv.ask_battler = i;
   s.mv.answered = 0;
   return effect::Outcome{0, true, false, false};
}

/// The outcome of a primitive that pushes nothing (or @p value), failing the run when a condition's try_start
/// failed.
/// @param st     how starting the condition went
/// @param value  the value it pushes
/// @return the outcome
effect::Outcome started(Started st, int32_t value = 0)
{
   return effect::Outcome{value, false, false, st == Started::Failed};
}

/// Every stage of a battler back to 0 (effect `reset_stages`, E13b): a Stage event per stage that changes, no texts,
/// no stage_change hooks (a clear, not a change one battler makes to another).
/// @param s  battle
/// @param i  battler
/// @return stages that changed
int32_t reset_stages(State& s, uint8_t i)
{
   if (!alive_active(s, i))
   {
      return 0;
   }
   Battler& b = s.battlers[i];
   int32_t changed = 0;
   for (uint8_t k = 0; k < data().stage_count && k < kMaxStages; ++k)
   {
      const int old = b.stages[k];
      if (old != 0)
      {
         b.stages[k] = 0;
         emit(s, EventKind::Stage, i, kBench, k, 0, static_cast<uint16_t>(-old));
         ++changed;
      }
   }
   return changed;
}

/// Stores the move's pending effect at a battler's position (effect `delay`, E13b): its land phase runs at the end
/// of the turn @p turns ends of turn from now (1: this turn's), on whoever is there then.
/// @param s       battle
/// @param r       the run (its move and user)
/// @param i       the battler whose position it is
/// @param amount  the value the land phase gets
/// @param turns   ends of turn until it lands (clamped to 1..255)
/// @return false when the position already has one, or the battler is off the field
bool delay_at(State& s, const effect::EffectRun& r, uint8_t i, int32_t amount, int32_t turns)
{
   const uint8_t pos = s.battlers[i].position;
   if (pos >= kMaxPositions || s.delayed[pos].turns != 0 || pos == s.landing || r.ctx.move >= data().move_count)
   {
      return false;   // G1: nor at the position whose delayed hit is landing
   }
   s.delayed[pos] = Delayed{r.ctx.move, static_cast<uint16_t>(clamp_to<int32_t>(amount, 0, kMaxU16)), r.ctx.user,
                            static_cast<uint8_t>(clamp_to<int32_t>(turns, 1, kCountMax8)), DelayedSnapshot{}};
   s.delayed[pos].snap.rule = kNone8;   // a plain delay
   return true;
}

/// Stores a delayed hit at a battler's position (effect `delay_hit`, G1): delay_at() with the delayed-hit record
/// and a snapshot of the move's user as it is now (its stats, stages, types and level; the record says which of
/// them the landing reads).
/// @param s      battle
/// @param r      the run (its move and user)
/// @param i      the battler whose position it is
/// @param rule   the delayed-hit record (BattleData::delayed_hits)
/// @param turns  ends of turn until it lands (clamped to 1..255)
/// @return false when delay_at() refuses it or the record does not exist
bool delay_hit_at(State& s, const effect::EffectRun& r, uint8_t i, uint8_t rule, int32_t turns)
{
   if (rule >= data().delayed_hit_count || r.ctx.user >= s.battler_count || !delay_at(s, r, i, 0, turns))
   {
      return false;
   }
   const Battler& u = s.battlers[r.ctx.user];
   DelayedSnapshot& snap = s.delayed[s.battlers[i].position].snap;
   for (int k = 0; k < kMaxStats; ++k)
   {
      snap.stats[k] = u.stats[k];
   }
   for (int k = 0; k < kMaxStages; ++k)
   {
      snap.stages[k] = u.stages[k];
   }
   for (int k = 0; k < kMaxTypesPer; ++k)
   {
      snap.types[k] = u.types[k];
   }
   snap.level = u.level;
   snap.rule = rule;
   // the target's stat that resists the move's category, as it is now (DELAYED_TARGET_DEFENSE)
   const uint8_t defense = data().categories[data().moves[r.ctx.move].category].defense;
   const Battler& t = s.battlers[i];
   snap.defense = defense < data().stat_count ? t.stats[defense] : 0;
   const uint8_t stage = defense < data().stat_count ? stage_of(defense) : kNone8;
   snap.defense_stage = stage < data().stage_count ? t.stages[stage] : 0;
   return true;
}

/// The battle primitives that change a battler (heal, inflict, cure, stage, volatiles, items, passive, types).
/// @param s  battle
/// @param r  the run
/// @param a  the primitive
/// @param i  the subject battler (kBench: none)
/// @return the value it pushes (fail: a condition's try_start failed the run)
effect::Outcome battler_action(State& s, const effect::EffectRun& r, const effect::Action& a, uint8_t i)
{
   using effect::Op;
   const effect::Outcome none{0, false, false, false};
   if (i == kBench)
   {
      return none;
   }
   Battler& b = s.battlers[i];
   switch (a.op)
   {
   case Op::Heal:
      if (alive_active(s, i))
      {
         set_hp(s, i, b.hp + clamp_to<int32_t>(a.args[0], 0, kMaxU16));
      }
      return none;
   case Op::Inflict:
   {
      const Started st =
         inflict(s, i, static_cast<uint8_t>(a.id + 1), r.ctx.holder, (a.flags & effect::kInflictQuiet) == 0);
      return started(st, st == Started::Started ? 1 : 0);
   }
   case Op::Cure:
      cure(s, i, (a.flags & effect::kCureQuiet) != 0);
      return none;
   case Op::Stage:
   {
      HookCtx c{i, r.ctx.holder, r.ctx.move, a.args[0], kVerdictNone, 0, 0};
      run_hook(s, effect::Hook::StageChange, c);
      if (c.verdict == kVerdictBlocked)
      {
         return none;
      }
      const int delta = clamp_to<int32_t>(c.value, INT8_MIN, INT8_MAX);
      const bool silent = (a.id & effect::kStageSilent) != 0;
      const bool applied = change_stage(s, i, static_cast<uint8_t>(a.id & effect::kStageIdMask), delta,
                                        silent || (a.id & effect::kStageQuiet) != 0, silent);
      return effect::Outcome{applied ? 1 : 0, false, false, false};
   }
   case Op::ResetStages:
      return effect::Outcome{reset_stages(s, i), false, false, false};
   case Op::Delay:
      return effect::Outcome{delay_at(s, r, i, a.args[0], a.args[1]) ? 1 : 0, false, false, false};
   case Op::DelayHit:
      return effect::Outcome{delay_hit_at(s, r, i, static_cast<uint8_t>(a.id), a.args[0]) ? 1 : 0, false, false,
                             false};
   case Op::VolOn:
      return started(volatile_on(s, i, static_cast<uint8_t>(a.id), r.ctx.holder, a.args[0]));
   case Op::VolOff:
      volatile_off(s, i, static_cast<uint8_t>(a.id), r.ctx.holder);
      return none;
   case Op::ConsumeItem:
      b.item = 0;
      return none;
   case Op::SetItem:
      b.item = static_cast<uint16_t>(a.args[0] >= 0 && a.args[0] + 1 < data().item_count ? a.args[0] + 1 : 0);
      return none;
   case Op::SetPassive:
      b.passive = static_cast<uint8_t>(a.args[0] >= 0 && a.args[0] < data().passive_count ? a.args[0] : kNone8);
      return none;
   case Op::SetTypes:
      static_assert(kMaxTypesPer <= effect::kMaxActionArgs, "set_types passes every type as an operand");
      for (int k = 0; k < kMaxTypesPer; ++k)
      {
         const int32_t ty = k < a.flags ? a.args[k] : kNone8;
         b.types[k] = static_cast<uint8_t>(ty >= 0 && ty < data().type_count ? ty : kNone8);
      }
      return none;
   case Op::Lock:
      b.lock_move = r.ctx.move;
      b.lock_target = s.mv.move != kNone16 ? s.mv.chosen : kBench;
      b.lock_flags = static_cast<uint8_t>(kLockSet | (a.flags & effect::kLockFree));
      return none;
   case Op::Unlock:
      b.lock_flags = 0;
      b.lock_move = kNone16;
      return none;
   case Op::Restore:
      return effect::Outcome{restore_resource(s, i, a, r.ctx.move), false, false, false};
   case Op::Raise:
      return effect::Outcome{raise_resource(s, i, a, r.ctx.move), false, false, false};
   case Op::UseMove:
      if (s.mv.move != kNone16 && a.args[0] >= 0 && a.args[0] < data().move_count)
      {
         s.mv.chain_move = static_cast<uint16_t>(a.args[0]);
         s.mv.chain_user = i;
      }
      return none;
   case Op::CallMove:
   {
      // G1: `use_move ... unless FLAG`: none, or a move with the flag (uncallable, unmirrorable, ...), fails the run
      const bool known = a.args[0] >= 0 && a.args[0] < data().move_count;
      if (s.mv.move == kNone16 || !known ||
          (a.id < kMaxMoveFlags && has_flag(data().moves[a.args[0]].flags, static_cast<unsigned>(a.id))))
      {
         return effect::Outcome{0, false, false, true};
      }
      s.mv.chain_move = static_cast<uint16_t>(a.args[0]);
      s.mv.chain_user = i;
      return none;
   }
   case Op::SelfHit:
   {
      if (!alive_active(s, i))
      {
         return none;
      }
      const int32_t d = self_hit_damage(s, i, -1, a.args[0]);
      set_hp(s, i, b.hp - d);
      check_faints(s, kBench);
      return none;
   }
   default:
      return none;
   }
}

/// The side a side condition primitive names (the battler of its `side(subject)`).
/// @param s   battle
/// @param at  the place
/// @return side, or kNoSide
uint8_t place_side(const State& s, const effect::Place& at)
{
   const uint8_t i = place_battler(s, at);
   return i != kBench ? side_of(s, i) : kNoSide;
}

/// The condition text of a status or weather owner (effect `text` with kConditionText): its StatusText /
/// WeatherText event.
/// @param s     battle
/// @param r     the run (its owner)
/// @param slot  the event's extra (kStatus* / kWeather*)
void condition_text(State& s, const effect::EffectRun& r, uint8_t slot)
{
   const effect::Owner* o = owner_at(r.ctx.owner);
   if (o == nullptr)
   {
      return;
   }
   if (o->kind == effect::OwnerKind::Status && slot < kStatusTexts)
   {
      // statuses are named by their manifest index (engine index 0 is "none")
      emit(s, EventKind::StatusText, r.ctx.holder, r.ctx.other, slot, static_cast<uint16_t>(o->name + 1));
   }
   else if (o->kind == effect::OwnerKind::Weather && slot < kWeatherTexts)
   {
      emit(s, EventKind::WeatherText, r.ctx.holder, r.ctx.other, slot, o->name);
   }
}

/// The battler of a place for an event's battler fields.
/// @param s   battle
/// @param at  the place
/// @return battler, or kBench
uint8_t event_battler(const State& s, const effect::Place& at)
{
   return at.kind == effect::PlaceKind::Battler ? place_battler(s, at) : kBench;
}

/// Resumes the move's suspended phase program (State::mv.run), or gives it the answer choose() took.
/// @param s            battle
/// @param give_answer  true: the run waits for an answer and has it
/// @return how it stopped
effect::Status run_phase(State& s, bool give_answer)
{
   effect::HostState host{s};
   if (give_answer)
   {
      s.mv.ask = kNone8;
      return effect::answer(s.vm, data().effects, s.mv.run, host, s.mv.answer);
   }
   return effect::resume(s.vm, data().effects, s.mv.run, host);
}

}   // namespace capsule::battle::detail

// -- the host functions --------------------------------------------------------------------------------------

namespace capsule::battle::effect
{

using namespace capsule::battle::detail;

namespace
{

/// Reads a timed field effect (F1; capsule/script/field_effects.h): whether it runs, what it has left.
/// @param ref   the ref
/// @param arg   the effect's index
/// @param[out] v  the value
/// @return false for another ref
bool field_effect_read(Ref ref, uint8_t arg, int32_t& v)
{
   if (ref == Ref::FieldEffectActive)
   {
      v = script::field_effects::active(arg) ? 1 : 0;
      return true;
   }
   if (ref == Ref::FieldEffectRemaining)
   {
      v = script::field_effects::remaining(arg);
      return true;
   }
   return false;
}

} // namespace

int32_t host_get(HostState& host, const EffectRun& r, Ref ref, Place at, uint8_t arg)
{
   const State& s = host.s;
   int32_t v = 0;
   if (field_effect_read(ref, arg, v))
   {
      return v;
   }
   // C5: the move in progress's hooks and phases read the type and category it hits with (aim_hit())
   const bool aimed = s.mv.move != kNone16 && r.ctx.move == s.mv.move && s.mv.hit_type != kNone8;
   if (context_read(s, r.ctx.move, ref, arg, v, aimed ? s.mv.hit_type : kNone8, aimed ? s.mv.hit_category : kNone8))
   {
      return v;
   }
   switch (ref)
   {
   case Ref::Hits:
      return s.mv.hits;
   case Ref::Crit:
      return has_flag(s.mv.flags, CURSOR_CRIT) ? 1 : 0;
   case Ref::Absorbed:
      return s.mv.move != kNone16 && has_flag(s.mv.flags, CURSOR_ABSORBED) ? 1 : 0;
   case Ref::Locked:
      return s.mv.move != kNone16 && has_flag(s.mv.flags, CURSOR_LOCKED) ? 1 : 0;
   case Ref::SideActive:
      // a side subject (or a battler): battler_read() counts its side's active battlers
      return battler_read(s, place_battler(s, at), ref, arg, r.ctx.move);
   case Ref::Alive:
   {
      // the VM's "alive": on the field and not fainted
      const uint8_t i = place_battler(s, at);
      return i != kBench && alive_active(s, i) ? 1 : 0;
   }
   default:
      break;
   }
   bool found = false;
   v = place_read(s, at, ref, arg, found);
   if (found)
   {
      return v;
   }
   return battler_read(s, at.kind == PlaceKind::Battler ? place_battler(s, at) : kBench, ref, arg, r.ctx.move);
}

void host_set(HostState& host, const EffectRun& r, Ref ref, Place at, uint8_t arg, int32_t value)
{
   State& s = host.s;
   const uint8_t i = at.kind == PlaceKind::Battler ? place_battler(s, at) : kBench;
   if (i == kBench)
   {
      return;
   }
   Battler& b = s.battlers[i];
   const auto byte = static_cast<uint8_t>(clamp_to<int32_t>(value, 0, kMaxCounter));
   switch (ref)
   {
   case Ref::StatusCounter:
      b.status_counter = byte;
      break;
   case Ref::StatusAux:
      b.status_aux = byte;
      break;
   case Ref::Counter:
      if (arg < kMaxVolatileCounters)
      {
         b.vcounter[arg] = byte;
      }
      break;
   case Ref::Word:
      if (arg < kMaxVolatileWords)
      {
         b.vword[arg] = static_cast<uint16_t>(clamp_to<int32_t>(value, 0, kMaxText));
      }
      break;
   case Ref::Resource:
   {
      // D-25: clamped to 0..its maximum (the hit points through set_hp: events, update hooks)
      const ResourceData* d = resource_at(arg);
      if (d != nullptr)
      {
         set_resource(s, i, arg, is_move_resource(*d) ? slot_of(b, r.ctx.move) : 0, value);
      }
      break;
   }
   default:
      break;
   }
}

bool host_has(HostState& host, const EffectRun& r, HasKind kind, Place at, uint8_t id)
{
   return place_has(host.s, at, kind, id, r.ctx.move);
}

int32_t host_role_get(HostState& host, const EffectRun& r, uint8_t battler, uint8_t role, uint8_t elem)
{
   (void)r;
   const monster::Record* rec = record_of(host.s, battler);
   // a record's role through record_value(): stored, or computed on read (an unstored custom role, E13a, or an
   // engine role the layout leaves out); a data member has no record: the role's initial value
   const uint32_t v = rec != nullptr ? record_value(*rec, role, elem) : monster::initial(role);
   return static_cast<int32_t>(v > static_cast<uint32_t>(INT32_MAX) ? INT32_MAX : v);
}

void host_role_set(HostState& host, const EffectRun& r, uint8_t battler, uint8_t role, uint8_t elem, int32_t value)
{
   (void)r;
   monster::Record* rec = record_of(host.s, battler);
   if (rec != nullptr && monster::has(role) && elem < monster::count(role))
   {
      // persistent state: straight into the record (data members have none)
      const uint32_t v = value < 0 ? 0 : static_cast<uint32_t>(value);
      monster::set(*rec, role, elem, v > monster::max_value(role) ? monster::max_value(role) : v);
   }
}

uint32_t host_battlers(HostState& host, const EffectRun& r, BattlerSet set, Place at)
{
   (void)r;
   return battlers_of(host.s, set, place_battler(host.s, at));
}

uint32_t host_below(HostState& host, uint32_t n)
{
   return rng::below(host.s.rng, n);
}

int32_t host_between(HostState& host, int32_t lo, int32_t hi)
{
   return rng::between(host.s.rng, lo, hi);
}

bool host_chance(HostState& host, uint32_t n, uint32_t m)
{
   return rng::chance(host.s.rng, n, m);
}

bool host_odds(HostState& host, const EffectRun& r, uint32_t n, uint32_t m, uint8_t by, uint8_t on)
{
   State& s = host.s;
   if (by >= s.battler_count || on >= s.battler_count)
   {
      return false;   // a battler slot that is not in this battle: nothing to land on, no draw
   }
   return effect_odds_of(s, &s.rng, by, on, r.ctx.move, static_cast<int32_t>(n), static_cast<int32_t>(m)).result != 0;
}

Outcome host_action(HostState& host, EffectRun& r, const Action& a)
{
   State& s = host.s;
   const uint8_t i = place_battler(s, a.at);
   switch (a.op)
   {
   case Op::Damage:
      return hit_once(s, r);
   case Op::DamageHp:
      return damage_hp(s, r, a);
   case Op::Weather:
      // the compiler passes the weather's own turns when the effect names none (0: until replaced); set again, the
      // weather's already hook runs (weather_on(), as field_on())
      return started(weather_on(s, static_cast<uint8_t>(a.id), r.ctx.holder, a.args[0]));
   case Op::Switch:
      switch_out(s, a);
      break;
   case Op::SetTarget:
      if (s.mv.move != kNone16 && s.mv.stage == MoveStage::Targets && a.args[0] >= 0 &&
          a.args[0] < s.battler_count && s.mv.count < kMaxPositions)
      {
         s.mv.targets[s.mv.count++] = static_cast<uint8_t>(a.args[0]);
      }
      break;
   case Op::Credit:
      if (s.mv.move != kNone16)
      {
         s.mv.run.ctx.dealt = sat_add(s.mv.run.ctx.dealt, a.args[0]);
      }
      break;
   case Op::Immune:
      if (s.dispatch_depth > 0)
      {
         s.dispatch[s.dispatch_depth - 1].heal_q8 = static_cast<uint16_t>(clamp_to<int32_t>(a.args[0], 0, kMaxU16));
      }
      break;
   case Op::Block:
   case Op::Text:
      if (a.id != kNoText && (a.id & kConditionText) != 0)
      {
         condition_text(s, r, static_cast<uint8_t>(a.id & kConditionTextMask));
      }
      else if (a.id != kNoText && a.id < data().text_count)
      {
         emit(s, EventKind::Text, r.ctx.holder, r.ctx.other, 0, a.id);
      }
      break;
   case Op::Say:
      if (a.id < data().message_count)
      {
         say(s, static_cast<Msg>(a.id), r.ctx.holder, r.ctx.other);
      }
      break;
   case Op::SayAt:
      if (a.id < data().message_count)
      {
         say(s, static_cast<Msg>(a.id), event_battler(s, a.at), event_battler(s, a.about),
             static_cast<uint16_t>(clamp_to<int32_t>(a.args[0], 0, kMaxText)));
      }
      break;
   case Op::Announce:
      announce(s, r);
      break;
   case Op::Cue:
      emit(s, EventKind::Cue, r.ctx.holder, r.ctx.other, 0, a.id);
      break;
   case Op::Ask:
      return ask(s, r, a);
   case Op::Reward:
      // E13b: the battle's reward pool (announced with the rest on a victory)
      if (a.args[0] > 0)
      {
         s.reward = capsule::sat_add<uint32_t>(s.reward, static_cast<uint32_t>(a.args[0]), 0, kRewardPoolMax);
      }
      break;
   case Op::EndBattle:
      // E13b: the battle ends here (nobody wins); the run goes no further
      end_battle(s, battle::Outcome::Escaped);
      return Outcome{0, false, true, false};
   case Op::Absorb:
      // G1: a decoy took the hit in progress (its before_damage handler; secondary effects and contact hooks read
      // `absorbed`)
      if (s.mv.move != kNone16)
      {
         s.mv.flags = static_cast<uint8_t>(s.mv.flags | bit(CURSOR_ABSORBED));
      }
      break;
   case Op::Teach:
      // G2: a move offered as a level-up does (teach.cpp); known or not takeable: the run fails (no effect)
      return teach_move(s, i, a.args[0]);
   case Op::Recruit:
      // G3: the recruit checks (recruit.cpp); success ends the battle, a refusal fails the run
      return recruit_battler(s, i, a.args[0]);
   case Op::Form:
      // G4: the subject's form by name (growth_battle.cpp); pushes whether it changed
      return form_primitive(s, i, static_cast<uint8_t>(a.id));
   case Op::Transform:
      // G4: the running item's item_use rules on a field context's member; none holds: the run fails
      return transform_primitive(s, i, r.ctx.owner);
   case Op::Train:
      // G4: a training value within the caps; pushes the change made
      return train_primitive(s, i, static_cast<uint8_t>(a.id), a.args[0]);
   case Op::Encounter:
      // F1: an encounter hook changes the roll in progress (encounter_lead.cpp); outside one the run fails
      return encounter_primitive(a);
   case Op::FieldEffect:
      // F1: a timed field effect by its stack rule; refused (or none in this program): the run fails
      if (!script::field_effects::start(static_cast<uint8_t>(a.id), a.args[0]))
      {
         return Outcome{0, false, false, true};
      }
      return Outcome{1, false, false, false};
   case Op::Inherit:
      // B1: the combine hook's inheritance into the result's record (combine_battle.cpp); elsewhere the run fails
      return inherit_primitive(s, a, i);
   case Op::SideOn:
      return started(side_on(s, place_side(s, a.at), static_cast<uint8_t>(a.id), i, a.args[0],
                             (a.flags & kSideAddLayer) != 0));
   case Op::SideOff:
      side_off(s, place_side(s, a.at), static_cast<uint8_t>(a.id), i);
      break;
   case Op::FieldOn:
      return started(field_on(s, static_cast<uint8_t>(a.id), r.ctx.holder, a.args[0]));
   case Op::FieldOff:
      field_off(s, static_cast<uint8_t>(a.id), r.ctx.holder);
      break;
   default:
      return battler_action(s, r, a, i);
   }
   return Outcome{0, false, false, false};
}

// -- point handlers --------------------------------------------------------------------------------------------

/// The battler a point handler's subject code names.
/// @param c     what the handler reads
/// @param subj  subject code (effect::kSubjHolder ...)
/// @return battler index, or kBench
uint8_t point_subject(const PointCtx& c, uint8_t subj)
{
   switch (subj & kSubjBaseMask)
   {
   case kSubjHolder:
      return c.holder;
   case kSubjOther:
      return c.other;
   case kSubjUser:
      return c.pipe->user;
   case kSubjTarget:
      return c.pipe->target;
   default:
      return kBench;
   }
}

int32_t point_get(PointCtx& c, Ref ref, uint8_t subj, uint8_t arg)
{
   const PipelineCtx& p = *c.pipe;
   const State& s = *p.s;
   int32_t v = 0;
   if (field_effect_read(ref, arg, v))
   {
      return v;   // F1: a timed field effect
   }
   // E13b / C5: the type and category the hit has (the move_type point's result)
   if (context_read(s, p.move, ref, arg, v, p.move_type, p.move_category))
   {
      return v;
   }
   if (ref == Ref::Crit)
   {
      return p.crit;
   }
   if (ref == Ref::Move)
   {
      return p.move;
   }
   // the sides' and the field's conditions (a side subject: its battler's side), then battler reads
   const uint8_t who = point_subject(c, subj);
   bool found = false;
   v = condition_read(s, who, ref, arg, found);
   if (found)
   {
      return v;
   }
   if (ref == Ref::SideActive)
   {
      return battler_read(s, who, ref, arg, p.move);   // `side(SUBJECT).active_count` (E13b)
   }
   const bool valid = who < s.battler_count && (subj & kSubjSide) == 0;
   if (ref == Ref::Battler)
   {
      return valid ? who : kBench;
   }
   return battler_read(s, valid ? who : kBench, ref, arg, p.move);
}

bool point_has(PointCtx& c, HasKind kind, uint8_t subj, uint8_t id)
{
   const PipelineCtx& p = *c.pipe;
   const uint8_t who = point_subject(c, subj);
   const bool side_ok = (subj & kSubjSide) == 0 || kind == HasKind::SideCond;
   return battler_has(*p.s, side_ok ? who : kBench, kind, id, p.move);
}

} // namespace capsule::battle::effect
