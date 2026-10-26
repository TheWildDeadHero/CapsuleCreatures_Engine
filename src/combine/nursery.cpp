/// @file
/// The nursery (D-28 section 2, package B2): deposit / withdraw, experience per step, fees, the checks and the
/// waiting result (see capsule/combine/nursery.h).
#include "capsule/combine/nursery.h"

#include "capsule/battle/data.h"
#include "capsule/battle/pipeline.h"
#include "capsule/combine/eggs.h"
#include "capsule/core/memory.h"

#include <new>

namespace capsule::combine::nursery
{

namespace
{

static_assert(kMaxNurserySlots <= 8, "State::used holds a bit per slot");

/// The arrangements of k of n things (n! / (n - k)!).
/// @param n  things
/// @param k  taken
/// @return how many ordered picks there are
constexpr int arrangements(int n, int k)
{
   int out = 1;
   for (int j = 0; j < k; ++j)
   {
      out *= n - j;
   }
   return out;
}

/// Every ordered pick of 1 to kMaxInputs of kMaxNurserySlots held records (C9: the bound of a check's tries; the
/// enumeration ends by itself, this guards the loop).
/// @return the count
constexpr int all_arrangements()
{
   int out = 0;
   for (int k = 1; k <= kMaxInputs && k <= kMaxNurserySlots; ++k)
   {
      out += arrangements(kMaxNurserySlots, k);
   }
   return out;
}

/// Most slot orders a check tries: every subset of the held records within the method's input count, each in every
/// order (C9: sized for the platform's slots and inputs; GBA 4 slots / 4 inputs: 64).
constexpr int kMaxOrders = all_arrangements();
/// The highest experience a held record gains (the 32-bit counter stays below its top).
constexpr uint32_t kMaxGained = 0x7FFFFFFFu;

/// The nursery (EWRAM on the GBA: a few records; zero at start-up).
CAPSULE_EWRAM_BSS State g;

/// The nursery's rules.
/// @return them
const NurseryData& rules_of()
{
   return kCombine.nursery;
}

/// A slot's bit.
/// @param slot  nursery slot
/// @return its bit in State::used
uint8_t bit_of(uint8_t slot)
{
   return static_cast<uint8_t>(1u << slot);
}

/// Whether a slot holds a record.
/// @param slot  nursery slot
/// @return true when it does
bool held(uint8_t slot)
{
   return slot < slot_count() && (g.used & bit_of(slot)) != 0;
}

/// The party's room.
/// @return members it holds
int party_room()
{
   return monster::kLayout.party_size < monster::kMaxPartySize ? monster::kLayout.party_size
                                                                : monster::kMaxPartySize;
}

/// Members of a party that are not eggs.
/// @param party  the party
/// @return how many
int hatched_members(const monster::Party& party)
{
   int n = 0;
   for (int k = 0; k < party.count; ++k)
   {
      n += is_egg(party.members[k]) ? 0 : 1;
   }
   return n;
}

/// A pipeline context out of battle (no battlers, no draw).
/// @return the context
battle::PipelineCtx pipeline_ctx()
{
   return battle::PipelineCtx{nullptr, nullptr, battle::kBench, battle::kBench, battle::kNone16, 0, 0,
                              battle::kNone8, battle::kNone8, battle::kNoDraw};
}

/// The held slots in slot order.
/// @param[out] out  their indices
/// @return how many
uint8_t held_slots(uint8_t* out)
{
   uint8_t n = 0;
   for (uint8_t k = 0; k < slot_count(); ++k)
   {
      if (held(k))
      {
         out[n++] = k;
      }
   }
   return n;
}

/// The next order of a permutation (lexicographic; false after the last one).
/// @param a  the order (changed)
/// @param n  its length
/// @return false when a was the last order
bool next_order(uint8_t* a, uint8_t n)
{
   constexpr int kLastPair = 2;   // the last two places: the rightmost pair compared first
   int i = n - kLastPair;
   while (i >= 0 && a[i] >= a[i + 1])
   {
      --i;
   }
   if (i < 0)
   {
      return false;
   }
   int j = n - 1;
   while (a[j] <= a[i])
   {
      --j;
   }
   const uint8_t t = a[i];
   a[i] = a[j];
   a[j] = t;
   for (int lo = i + 1, hi = n - 1; lo < hi; ++lo, --hi)
   {
      const uint8_t u = a[lo];
      a[lo] = a[hi];
      a[hi] = u;
   }
   return true;
}

/// The next subset of k of n indices in increasing order (lexicographic; false after the last one).
/// @param idx  the subset (changed)
/// @param k    its size
/// @param n    the indices' count
/// @return false when idx was the last subset
bool next_subset(uint8_t* idx, uint8_t k, uint8_t n)
{
   int i = k - 1;
   while (i >= 0 && idx[i] == n - k + i)
   {
      --i;
   }
   if (i < 0)
   {
      return false;
   }
   ++idx[i];
   for (int j = i + 1; j < k; ++j)
   {
      idx[j] = static_cast<uint8_t>(idx[j - 1] + 1);
   }
   return true;
}

/// The first arrangement of the held records the method takes (no draw; C9: with more records held than the method
/// takes, its subsets too): the largest subsets first (the method's most inputs down to its fewest), each subset in
/// slot order first, then its other orders. An order whose only fault is an unfit slot may fit in another order;
/// any other answer (incompatible, refused) holds for the whole subset, and the next subset is tried.
/// @param[out] order  the slots in input order
/// @param[out] count  how many
/// @param[out] out    the preview of that order (the last one tried when none fits)
/// @return true when one fits (status Ok)
bool fitting_order(uint8_t* order, uint8_t& count, Preview& out)
{
   const MethodData* m = method_at(rules_of().method);
   uint8_t slots[kMaxNurserySlots] = {};
   const uint8_t held_count = held_slots(slots);
   count = 0;
   out = Preview{Status::WrongCount, kNone, kNone, 0, kNoSpecies, kNoGroup, 0, 0};
   if (m == nullptr || held_count < m->min_inputs)
   {
      return false;
   }
   const int most = m->max_inputs < kMaxInputs ? m->max_inputs : kMaxInputs;
   int tries = 0;
   for (int size = held_count < most ? held_count : most; size >= m->min_inputs; --size)
   {
      const auto k = static_cast<uint8_t>(size);
      uint8_t idx[kMaxInputs] = {};
      for (uint8_t j = 0; j < k; ++j)
      {
         idx[j] = j;
      }
      do
      {
         // the subset's slots in slot order: the first arrangement of next_order()
         for (uint8_t j = 0; j < k; ++j)
         {
            order[j] = slots[idx[j]];
         }
         bool more = true;
         while (more && tries < kMaxOrders)
         {
            ++tries;
            const monster::Record* recs[kMaxInputs] = {};
            for (uint8_t j = 0; j < k; ++j)
            {
               recs[j] = &g.records[order[j]];
            }
            out = preview(rules_of().method, recs, k);
            if (out.status == Status::Ok)
            {
               count = k;
               return true;
            }
            more = out.status == Status::Unfit && next_order(order, k);
         }
      } while (tries < kMaxOrders && next_subset(idx, k, held_count));
   }
   return false;
}

/// The experience a held record has gained, within what its curve takes.
/// @param slot  nursery slot
/// @return experience
uint32_t gained_of(uint8_t slot)
{
   return held(slot) ? g.gained[slot] : 0;
}

/// The room a check's Combination is built in (EWRAM on the GBA; one check at a time).
alignas(Combination) CAPSULE_EWRAM_BSS unsigned char g_combo[sizeof(Combination)];

/// Builds a Combination in g_combo and ends it when it goes (RAII: no heap, no large stack frame).
class CombinationRoom
{
public:
   /// Builds it (see Combination's constructor).
   /// @param s       the lent State
   /// @param party   the player's party
   /// @param method  method index
   /// @param refs    the inputs
   /// @param count   how many
   /// @param rng     the stream
   CombinationRoom(battle::State& s, monster::Party& party, uint8_t method, const InputRef* refs, uint8_t count,
                   rng::Stream& rng)
      : m_combo(new (g_combo) Combination(s, party, nullptr, method, refs, count, rng))
   {
   }
   /// Ends it.
   ~CombinationRoom()
   {
      m_combo->~Combination();
   }
   CombinationRoom(const CombinationRoom&) = delete;              ///< Not copied.
   CombinationRoom& operator=(const CombinationRoom&) = delete;   ///< Not assigned.
   /// The Combination.
   /// @return it
   Combination& get() const
   {
      return *m_combo;
   }

private:
   Combination* m_combo;   ///< The one in g_combo.
};

} // namespace

State& state()
{
   return g;
}

void clear()
{
   g = State{};
}

bool exists()
{
   return rules_of().slots != 0;
}

uint8_t slot_count()
{
   return rules_of().slots < kMaxNurserySlots ? rules_of().slots : static_cast<uint8_t>(kMaxNurserySlots);
}

uint8_t count()
{
   uint8_t n = 0;
   for (uint8_t k = 0; k < slot_count(); ++k)
   {
      n = static_cast<uint8_t>(n + (held(k) ? 1 : 0));
   }
   return n;
}

const monster::Record* record(uint8_t slot)
{
   return held(slot) ? &g.records[slot] : nullptr;
}

Result deposit(monster::Party& party, uint8_t member, uint8_t* slot)
{
   if (!exists())
   {
      return Result::NoNursery;
   }
   if (member >= party.count || member >= monster::kMaxPartySize)
   {
      return Result::BadSlot;
   }
   const MethodData* m = method_at(rules_of().method);
   const monster::Record& rec = party.members[member];
   const bool egg = is_egg(rec);
   // an egg the method does not take, or the last member that could battle, stays
   if ((egg && (m == nullptr || (m->flags & kAllowEggs) == 0)) || (!egg && hatched_members(party) <= 1))
   {
      return Result::Refused;
   }
   uint8_t free_slot = kNone;
   for (uint8_t k = 0; k < slot_count() && free_slot == kNone; ++k)
   {
      free_slot = held(k) ? kNone : k;
   }
   if (free_slot == kNone)
   {
      return Result::Full;
   }
   g.records[free_slot] = rec;
   g.gained[free_slot] = 0;
   g.level[free_slot] = static_cast<uint8_t>(battle::record_value(rec, monster::id_of(monster::Role::Level)));
   g.used = static_cast<uint8_t>(g.used | bit_of(free_slot));
   // the member leaves; the ones after it move up
   for (int j = member; j + 1 < party.count; ++j)
   {
      party.members[j] = party.members[j + 1];
   }
   --party.count;
   party.members[party.count] = monster::Record{};
   if (slot != nullptr)
   {
      *slot = free_slot;
   }
   return Result::Ok;
}

uint8_t level_now(uint8_t slot)
{
   return held(slot) ? battle::level_with(g.records[slot], gained_of(slot)) : 0;
}

uint8_t levels_gained(uint8_t slot)
{
   const uint8_t now = level_now(slot);
   return held(slot) && now > g.level[slot] ? static_cast<uint8_t>(now - g.level[slot]) : 0;
}

int32_t fee(uint8_t slot)
{
   if (!held(slot) || (rules_of().flags & kNurseryFee) == 0)
   {
      return 0;
   }
   battle::PipelineCtx c = pipeline_ctx();
   const battle::NurseryFeeInputs in{levels_gained(slot), level_now(slot), count()};
   return rules::nursery_fee(c, in);
}

Result withdraw(monster::Party& party, uint8_t slot)
{
   if (!exists())
   {
      return Result::NoNursery;
   }
   if (slot >= slot_count())
   {
      return Result::BadSlot;
   }
   if (!held(slot))
   {
      return Result::Empty;
   }
   if (party.count >= party_room())
   {
      return Result::NoRoom;
   }
   monster::Record rec = g.records[slot];
   battle::grow_record(rec, gained_of(slot));
   party.members[party.count++] = rec;
   g.records[slot] = monster::Record{};
   g.gained[slot] = 0;
   g.level[slot] = 0;
   g.used = static_cast<uint8_t>(g.used & ~bit_of(slot));
   return Result::Ok;
}

int32_t odds()
{
   uint8_t order[kMaxNurserySlots] = {};
   uint8_t n = 0;
   Preview p{};
   return fitting_order(order, n, p) ? p.odds : 0;
}

bool result_ready()
{
   return exists() && g.pending != 0;
}

const monster::Record* result()
{
   return result_ready() ? &g.result : nullptr;
}

Result take_result(monster::Party& party)
{
   if (!exists())
   {
      return Result::NoNursery;
   }
   if (g.pending == 0)
   {
      return Result::Empty;
   }
   if (party.count >= party_room())
   {
      return Result::NoRoom;
   }
   party.members[party.count++] = g.result;
   g.result = monster::Record{};
   g.pending = 0;
   return Result::Ok;
}

void walked(uint32_t steps)
{
   if (!exists() || (rules_of().flags & kNurseryGain) == 0 || steps == 0)
   {
      return;
   }
   battle::PipelineCtx c = pipeline_ctx();
   for (uint8_t k = 0; k < slot_count(); ++k)
   {
      if (!held(k) || is_egg(g.records[k]))
      {
         continue;   // C9: an egg left there (a method that takes eggs) gains nothing
      }
      const battle::NurseryGainInputs in{g.level[k], k};
      const int32_t per_step = rules::nursery_gain(c, in);
      const uint64_t add = static_cast<uint64_t>(per_step > 0 ? per_step : 0) * steps;
      const uint64_t total = g.gained[k] + add;
      g.gained[k] = static_cast<uint32_t>(total < kMaxGained ? total : kMaxGained);
   }
}

bool check(battle::State& lent, monster::Party& party, rng::Stream& rng)
{
   if (!exists() || g.pending != 0)
   {
      return false;
   }
   uint8_t order[kMaxNurserySlots] = {};
   uint8_t n = 0;
   Preview p{};
   if (!fitting_order(order, n, p) || p.odds <= 0)
   {
      return false;
   }
   // the check's draw: the odds in the nursery's scale
   const uint32_t scale = rules_of().odds_scale != 0 ? rules_of().odds_scale : 1u;
   if (rng::below(rng, scale) >= static_cast<uint32_t>(p.odds))
   {
      return false;
   }
   InputRef refs[kMaxInputs] = {};
   for (uint8_t k = 0; k < n; ++k)
   {
      refs[k] = InputRef{From::Nursery, 0, order[k]};
   }
   // the combination in its own static room (two parties: too large for the GBA's stack), ended in scope
   const CombinationRoom room(lent, party, rules_of().method, refs, n, rng);
   Combination& combo = room.get();
   if (combo.stage() == Stage::Choosing)
   {
      combo.choose_random();   // nothing asks while walking
   }
   if (combo.stage() != Stage::Ready)
   {
      return false;
   }
   const MethodData& m = *method_at(rules_of().method);
   g.result = combo.record();
   if (m.delivery == Delivery::Egg)
   {
      make_egg(g.result, m.cycle_steps);
   }
   g.pending = 1;
   if (m.afterwards == Afterwards::Consumed)
   {
      // consumed inputs leave their slots once the result is kept
      for (uint8_t k = 0; k < n; ++k)
      {
         g.records[order[k]] = monster::Record{};
         g.gained[order[k]] = 0;
         g.level[order[k]] = 0;
         g.used = static_cast<uint8_t>(g.used & ~bit_of(order[k]));
      }
   }
   return true;
}

} // namespace capsule::combine::nursery
