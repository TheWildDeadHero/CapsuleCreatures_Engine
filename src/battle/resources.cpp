/// @file
/// Resources (D-25; capsule/battle/data/resources.h): the pools of a battler (its hit points, Battler::pool) and
/// of its move slots (Battler::move_pool with Battler::raises), their maxima, loading them from and storing them
/// in party records, move costs (selectability and payment), the regen end-of-turn phase and the effect
/// primitives Restore and Raise.
/// See capsule/battle/battle.h; the modules share internal.h.
#include "internal.h"

namespace capsule::battle::detail
{

/// The data of a resource.
/// @param r  resource index
/// @return its ResourceData, or nullptr for an unknown index
const ResourceData* resource_at(uint8_t r)
{
   return r < data().resource_count ? &data().resources[r] : nullptr;
}

/// Whether a resource has one pool per move slot.
/// @param d  the resource
/// @return true for ResourceScope::Move
bool is_move_resource(const ResourceData& d)
{
   return d.scope == static_cast<uint8_t>(ResourceScope::Move);
}

/// Whether a resource is the hit points (Battler::hp).
/// @param d  the resource
/// @return true for ResourceEmpty::Faint
bool is_faint_resource(const ResourceData& d)
{
   return d.empty == static_cast<uint8_t>(ResourceEmpty::Faint);
}

/// The highest value a resource's pool holds: its storage, or less when its persist role is narrower (the
/// write-back keeps it).
/// @param d  the resource
/// @return the cap
int32_t pool_cap(const ResourceData& d)
{
   const int32_t storage = is_move_resource(d) ? kMaxMovePoolValue : kMaxPoolValue;
   if (d.role == monster::kNoRole || !monster::has(d.role))
   {
      return storage;
   }
   const uint32_t held = monster::max_value(d.role);
   return held < static_cast<uint32_t>(storage) ? static_cast<int32_t>(held) : storage;
}

/// A move resource's maximum for a move and its raises: the max formula of (base, raises), or the base.
/// @param d       the resource (move scope)
/// @param move    the move, or kNone16 (0)
/// @param raises  the slot's raises
/// @return the maximum, clamped to the pool
int32_t move_max_of(const ResourceData& d, uint16_t move, int32_t raises)
{
   if (move >= data().move_count)
   {
      return 0;
   }
   const int32_t base = data().moves[move].base[d.slot];
   const int32_t got = d.max_formula == kNone8 ? base : rules::resource_formula(d.max_formula, base, raises);
   return clamp_to<int32_t>(got, 0, pool_cap(d));
}

/// The move slot that holds a move.
/// @param b     battler
/// @param move  move
/// @return the slot, or -1
int slot_of(const Battler& b, uint16_t move)
{
   for (int k = 0; move != kNone16 && k < move_slots(); ++k)
   {
      if (b.moves[k] == move)
      {
         return k;
      }
   }
   return -1;
}

/// A battler's pool of a resource (detail form of resource_value()).
/// @param b     battler
/// @param d     the resource
/// @param slot  move slot (move resources; out of range: 0)
/// @return the value
int32_t value_of(const Battler& b, const ResourceData& d, int slot)
{
   if (is_faint_resource(d))
   {
      return b.hp;
   }
   if (!is_move_resource(d))
   {
      return b.pool[d.slot];
   }
   return slot >= 0 && slot < move_slots() ? b.move_pool[d.slot][slot] : 0;
}

/// A battler's maximum of a resource (detail form of resource_max()).
/// @param b     battler
/// @param d     the resource
/// @param slot  move slot (move resources; out of range: 0)
/// @return the maximum
int32_t max_of(const Battler& b, const ResourceData& d, int slot)
{
   if (is_faint_resource(d))
   {
      return max_hp(b);
   }
   if (!is_move_resource(d))
   {
      return d.stat < data().stat_count ? clamp_to<int32_t>(b.stats[d.stat], 0, pool_cap(d)) : 0;
   }
   if (slot < 0 || slot >= move_slots())
   {
      return 0;
   }
   return move_max_of(d, b.moves[slot], b.raises[d.slot][slot]);
}

/// Sets a battler's pool of a resource, clamped to 0..its maximum: the hit points through set_hp() (Hp event,
/// update hooks), another battler resource with a Resource event, a move slot's pool silently.
/// @param s     battle
/// @param i     battler
/// @param r     resource index
/// @param slot  move slot (move resources)
/// @param v     new value
void set_resource(State& s, uint8_t i, uint8_t r, int slot, int32_t v)
{
   const ResourceData* d = resource_at(r);
   if (d == nullptr || i >= s.battler_count)
   {
      return;
   }
   Battler& b = s.battlers[i];
   if (is_faint_resource(*d))
   {
      set_hp(s, i, v);
      return;
   }
   const int32_t to = clamp_to<int32_t>(v, 0, max_of(b, *d, slot));
   if (is_move_resource(*d))
   {
      if (slot >= 0 && slot < move_slots())
      {
         b.move_pool[d->slot][slot] = static_cast<uint8_t>(to);
      }
      return;
   }
   const uint16_t old = b.pool[d->slot];
   b.pool[d->slot] = static_cast<uint16_t>(to);
   if (old != b.pool[d->slot])
   {
      emit(s, EventKind::Resource, i, kBench, r, b.pool[d->slot], old);
   }
}

/// Fills a battler's pools when it is built: from its record (each persisted resource, clamped to its maximum;
/// the raises from their role) or full (a resource without a persist role, or a battler without a record).
/// Moves and stats must be set.
/// @param b    battler
/// @param rec  its record, or nullptr (member data)
void load_resources(Battler& b, const monster::Record* rec)
{
   for (uint8_t r = 0; r < data().resource_count; ++r)
   {
      const ResourceData& d = data().resources[r];
      const bool stored = rec != nullptr && d.role != monster::kNoRole;
      if (!is_move_resource(d))
      {
         const int32_t top = max_of(b, d, 0);
         const int32_t v = stored ? clamp_to<int32_t>(static_cast<int32_t>(monster::get(*rec, d.role)), 0, top) : top;
         if (is_faint_resource(d))
         {
            b.hp = static_cast<uint16_t>(v);
         }
         else
         {
            b.pool[d.slot] = static_cast<uint16_t>(v);
         }
         continue;
      }
      for (int k = 0; k < move_slots(); ++k)
      {
         const bool raised = rec != nullptr && d.raises != monster::kNoRole;
         b.raises[d.slot][k] =
            static_cast<uint8_t>(raised ? clamp_to<uint32_t>(monster::get(*rec, d.raises, k), 0, kMaxMovePoolValue) : 0);
         const int32_t top = max_of(b, d, k);
         const int32_t v = stored ? clamp_to<int32_t>(static_cast<int32_t>(monster::get(*rec, d.role, k)), 0, top) : top;
         b.move_pool[d.slot][k] = static_cast<uint8_t>(v);
      }
   }
}

/// A pool value as its persist role stores it. The compiler refuses a persist role narrower than the pool's
/// highest value (tools/battlec/resources.py check_persist_roles()), so the clamp only guards the write: a wider
/// value would be masked by write_bits() and corrupt the record.
/// @param role   the persist role
/// @param value  the pool's value (>= 0)
/// @return the value to store
uint32_t stored_value(monster::RoleId role, int32_t value)
{
   const uint32_t top = monster::max_value(role);
   const uint32_t v = static_cast<uint32_t>(value < 0 ? 0 : value);
   ENGINE_ASSERT(v <= top);
   return v <= top ? v : top;
}

/// Writes a battler's persisted pools and raises into its record (the battle's write-back), each clamped to its
/// role (stored_value()).
/// @param b    battler
/// @param rec  its record
void store_resources(const Battler& b, monster::Record& rec)
{
   for (uint8_t r = 0; r < data().resource_count; ++r)
   {
      const ResourceData& d = data().resources[r];
      if (!is_move_resource(d))
      {
         if (d.role != monster::kNoRole)
         {
            monster::set(rec, d.role, 0, stored_value(d.role, value_of(b, d, 0)));
         }
         continue;
      }
      for (int k = 0; k < move_slots(); ++k)
      {
         if (d.role != monster::kNoRole)
         {
            monster::set(rec, d.role, k, stored_value(d.role, b.move_pool[d.slot][k]));
         }
         if (d.raises != monster::kNoRole)
         {
            monster::set(rec, d.raises, k, b.raises[d.slot][k]);   // a learned move's raises start at 0
         }
      }
   }
}

/// A move slot gets a new move: no raises, full pools.
/// @param b     battler
/// @param slot  move slot (its move is set)
void reset_move_slot(Battler& b, int slot)
{
   for (uint8_t r = 0; r < data().resource_count; ++r)
   {
      const ResourceData& d = data().resources[r];
      if (is_move_resource(d))
      {
         b.raises[d.slot][slot] = 0;
         b.move_pool[d.slot][slot] = static_cast<uint8_t>(max_of(b, d, slot));
      }
   }
}

/// learn.on_full replace_oldest: every move slot's pools and raises move up a slot with its move.
/// @param b  battler
void shift_move_slots(Battler& b)
{
   for (int m = 0; m < data().move_resource_count; ++m)
   {
      for (int k = 0; k + 1 < move_slots(); ++k)
      {
         b.move_pool[m][k] = b.move_pool[m][k + 1];
         b.raises[m][k] = b.raises[m][k + 1];
      }
   }
}

/// The maxima of a battler's pools besides the hit points (before a level-up changes its stats).
/// @param b    battler
/// @param out  kMaxPools maxima (Battler::pool order)
void pool_maxima(const Battler& b, int32_t out[kMaxPools])
{
   for (uint8_t r = 0; r < data().resource_count; ++r)
   {
      const ResourceData& d = data().resources[r];
      if (!is_move_resource(d) && !is_faint_resource(d))
      {
         out[d.slot] = max_of(b, d, 0);
      }
   }
}

/// After a level-up: each pool besides the hit points gains what its maximum gained (as the hit points do).
/// @param s        battle
/// @param i        battler
/// @param old_max  pool_maxima() before the level-up
void grow_pools(State& s, uint8_t i, const int32_t old_max[kMaxPools])
{
   const Battler& b = s.battlers[i];
   for (uint8_t r = 0; r < data().resource_count; ++r)
   {
      const ResourceData& d = data().resources[r];
      if (!is_move_resource(d) && !is_faint_resource(d))
      {
         set_resource(s, i, r, 0, b.pool[d.slot] + max_of(b, d, 0) - old_max[d.slot]);
      }
   }
}

/// One cost's amount for a battler: the fixed amount, or its formula of the pool's (value, max).
/// @param b     battler
/// @param c     the cost
/// @param slot  the move's slot (move resources)
/// @return the amount (0 for an unknown resource)
int32_t cost_amount(const Battler& b, const MoveCost& c, int slot)
{
   const ResourceData* d = resource_at(c.resource);
   if (d == nullptr)
   {
      return 0;
   }
   if (c.formula == kNone8)
   {
      return c.amount;
   }
   return clamp_to<int32_t>(rules::resource_formula(c.formula, value_of(b, *d, slot), max_of(b, *d, slot)), 0,
                            pool_cap(*d));
}

/// Whether a battler can pay every cost of a move from a slot (a move resource's cost needs the slot's pool).
/// A cost on the faint resource (hit points) needs more than its amount: paying never empties the hit points,
/// so a move never runs with a user that fainted paying for it (D-25 addendum; the move is not usable instead).
/// @param b     battler
/// @param move  the move
/// @param slot  its slot, or -1 (then a move resource's cost cannot be paid)
/// @return true when every pool holds its cost
bool can_pay(const Battler& b, uint16_t move, int slot)
{
   if (move >= data().move_count)
   {
      return false;
   }
   const MoveData& m = data().moves[move];
   for (int k = 0; k < m.cost_count; ++k)
   {
      const MoveCost& c = data().move_costs[m.cost_first + k];
      const ResourceData* d = resource_at(c.resource);
      if (d == nullptr || (is_move_resource(*d) && slot < 0))
      {
         return false;
      }
      const int32_t have = value_of(b, *d, slot);
      const int32_t amount = cost_amount(b, c, slot);
      if (is_faint_resource(*d) ? have <= amount : have < amount)
      {
         return false;
      }
   }
   return true;
}

/// What a move's costs take from a battler's pools, as a share of each pool's maximum (the AI's cost term,
/// a SaveResources rule of an AI profile, G1): the sum over its costs of amount * scale / maximum (a pool without a maximum counts as
/// fully spent).
/// @param b      battler
/// @param move   the move
/// @param slot   its slot (move resources)
/// @param scale  what spending a whole pool is worth
/// @return the cost term (>= 0)
int32_t cost_share(const Battler& b, uint16_t move, int slot, int32_t scale)
{
   if (move >= data().move_count)
   {
      return 0;
   }
   const MoveData& m = data().moves[move];
   int32_t share = 0;
   for (int k = 0; k < m.cost_count; ++k)
   {
      const MoveCost& c = data().move_costs[m.cost_first + k];
      const ResourceData* d = resource_at(c.resource);
      if (d == nullptr)
      {
         continue;
      }
      const int32_t top = max_of(b, *d, slot);
      const int32_t amount = cost_amount(b, c, slot);
      // amount <= the pool's cap (kMaxPoolValue) and scale is a small score: the product fits int32
      share += top > 0 ? clamp_to<int32_t>(amount, 0, top) * scale / top : scale;
   }
   return share;
}

/// Pays every cost of a move, in the manifest's resource order (the caller checked can_pay()).
/// @param s     battle
/// @param i     battler
/// @param move  the move
/// @param slot  its slot (move resources)
void pay(State& s, uint8_t i, uint16_t move, int slot)
{
   const MoveData& m = data().moves[move];
   for (int k = 0; k < m.cost_count; ++k)
   {
      const MoveCost& c = data().move_costs[m.cost_first + k];
      const ResourceData* d = resource_at(c.resource);
      if (d != nullptr)
      {
         const Battler& b = s.battlers[i];
         set_resource(s, i, c.resource, slot, value_of(b, *d, slot) - cost_amount(b, c, slot));
      }
   }
}

/// End of turn, the regen phase (one battler): each resource with a regen, in order, gains it (a formula of the
/// pool's value and maximum, or the fixed amount; a move resource per slot with a move), then fainting is checked.
/// @param s  battle
/// @param i  battler
void regen_battler(State& s, uint8_t i)
{
   if (!alive_active(s, i))
   {
      return;
   }
   for (uint8_t r = 0; r < data().resource_count; ++r)
   {
      const ResourceData& d = data().resources[r];
      if (d.regen_formula == kNone8 && d.regen == 0)
      {
         continue;
      }
      const int slots = is_move_resource(d) ? move_slots() : 1;
      for (int k = 0; k < slots; ++k)
      {
         const Battler& b = s.battlers[i];
         if (is_move_resource(d) && b.moves[k] == kNone16)
         {
            continue;
         }
         const int32_t v = value_of(b, d, k);
         const int32_t amount =
            d.regen_formula == kNone8 ? d.regen : rules::resource_formula(d.regen_formula, v, max_of(b, d, k));
         set_resource(s, i, r, k, v + amount);
      }
   }
   check_faints(s, kBench);
}

/// The move slots a Restore / Raise reaches: every slot, or the one holding the context move.
/// @param b      battler
/// @param flags  the primitive's flags (kResourceEverySlot)
/// @param move   the context move
/// @param first  first slot (out)
/// @param end    one past the last slot (out; first == end: none)
void target_slots(const Battler& b, uint8_t flags, uint16_t move, int& first, int& end)
{
   if ((flags & effect::kResourceEverySlot) != 0)
   {
      first = 0;
      end = move_slots();
      return;
   }
   first = slot_of(b, move);
   end = first < 0 ? first : first + 1;
}

/// Effect `restore WHO RESOURCE [amount] [every_slot]`: the pool gains the amount up to its maximum (to it without
/// one; kResourceFull). The hit points of a fainted battler are not restored.
/// @param s      battle
/// @param i      battler
/// @param a      the primitive (id: resource; args[0]: amount)
/// @param move   the context move (a move resource's slot)
/// @return what the pools gained
int32_t restore_resource(State& s, uint8_t i, const effect::Action& a, uint16_t move)
{
   const auto r = static_cast<uint8_t>(a.id);
   const ResourceData* d = resource_at(r);
   if (d == nullptr || i >= s.battler_count || (is_faint_resource(*d) && !usable(s.battlers[i])))
   {
      return 0;
   }
   int first = 0;
   int end = 1;
   if (is_move_resource(*d))
   {
      target_slots(s.battlers[i], a.flags, move, first, end);
   }
   const bool full = (a.flags & effect::kResourceFull) != 0;
   int32_t gained = 0;
   for (int k = first; k < end; ++k)
   {
      const Battler& b = s.battlers[i];
      const int32_t before = value_of(b, *d, k);
      const int32_t amount = full ? max_of(b, *d, k) : clamp_to<int32_t>(a.args[0], 0, kMaxPoolValue);
      set_resource(s, i, r, k, before + amount);
      gained += value_of(s.battlers[i], *d, k) - before;
   }
   return gained;
}

/// Effect `raise WHO RESOURCE delta [every_slot]`: a move resource's raises change by delta (within its raises
/// role), its maximum follows and the pool gains (or loses) what the maximum gained.
/// @param s      battle
/// @param i      battler
/// @param a      the primitive (id: resource; args[0]: delta)
/// @param move   the context move (the slot)
/// @return the raises applied
int32_t raise_resource(State& s, uint8_t i, const effect::Action& a, uint16_t move)
{
   const ResourceData* d = resource_at(static_cast<uint8_t>(a.id));
   if (d == nullptr || i >= s.battler_count || !is_move_resource(*d) || d->raises == monster::kNoRole)
   {
      return 0;
   }
   Battler& b = s.battlers[i];
   const int32_t top = clamp_to<int32_t>(static_cast<int32_t>(monster::max_value(d->raises)), 0, kMaxMovePoolValue);
   int first = 0;
   int end = 0;
   target_slots(b, a.flags, move, first, end);
   int32_t applied = 0;
   for (int k = first; k < end; ++k)
   {
      if (b.moves[k] == kNone16)
      {
         continue;
      }
      const int32_t old_max = max_of(b, *d, k);
      const int32_t old_raises = b.raises[d->slot][k];
      // the delta is any int32 from the VM stack: bound it first, so the sum cannot overflow
      const int32_t delta = clamp_to<int32_t>(a.args[0], -kMaxMovePoolValue, kMaxMovePoolValue);
      b.raises[d->slot][k] = static_cast<uint8_t>(clamp_to<int32_t>(old_raises + delta, 0, top));
      applied += b.raises[d->slot][k] - old_raises;
      // the pool follows its maximum
      const int32_t v = b.move_pool[d->slot][k] + max_of(b, *d, k) - old_max;
      b.move_pool[d->slot][k] = static_cast<uint8_t>(clamp_to<int32_t>(v, 0, max_of(b, *d, k)));
   }
   return applied;
}

}   // namespace capsule::battle::detail

// -- public ------------------------------------------------------------------------------------------------

namespace capsule::battle
{

using namespace detail;

int32_t move_base(uint16_t move, uint8_t resource)
{
   const ResourceData* d = resource_at(resource);
   if (d == nullptr || !is_move_resource(*d) || move >= data().move_count)
   {
      return 0;
   }
   return data().moves[move].base[d->slot];
}

int32_t resource_value(const Battler& b, uint8_t resource, int slot)
{
   const ResourceData* d = resource_at(resource);
   return d != nullptr ? value_of(b, *d, slot) : 0;
}

int32_t resource_max(const Battler& b, uint8_t resource, int slot)
{
   const ResourceData* d = resource_at(resource);
   return d != nullptr ? max_of(b, *d, slot) : 0;
}

void fill_move_slot(monster::Record& rec, int slot)
{
   const uint32_t m = monster::get(rec, monster::Role::Moves, slot);
   const uint16_t move = m < data().move_count ? static_cast<uint16_t>(m) : kNone16;
   for (uint8_t r = 0; r < data().resource_count; ++r)
   {
      const ResourceData& d = data().resources[r];
      if (!is_move_resource(d) || d.role == monster::kNoRole)
      {
         continue;
      }
      const int32_t raises = d.raises != monster::kNoRole ? static_cast<int32_t>(monster::get(rec, d.raises, slot)) : 0;
      monster::set(rec, d.role, slot, static_cast<uint32_t>(move_max_of(d, move, raises)));
   }
}

void fill_resources(monster::Record& rec)
{
   uint16_t stats[kMaxStats] = {};
   const bool known = record_stats(rec, stats);
   for (uint8_t r = 0; r < data().resource_count; ++r)
   {
      const ResourceData& d = data().resources[r];
      if (!is_move_resource(d) && d.role != monster::kNoRole && d.stat < data().stat_count)
      {
         const int32_t top = known ? clamp_to<int32_t>(stats[d.stat], 0, pool_cap(d)) : 0;
         monster::set(rec, d.role, 0, static_cast<uint32_t>(top));
      }
   }
   for (int k = 0; k < move_slots(); ++k)
   {
      fill_move_slot(rec, k);
   }
}

}   // namespace capsule::battle
