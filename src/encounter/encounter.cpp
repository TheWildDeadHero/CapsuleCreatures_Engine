/// @file
/// Encounter rolls (see capsule/encounter/encounter.h).
#include "capsule/encounter/encounter.h"

namespace capsule::encounter
{
namespace
{

/// The bank used when the build has none.
const Bank kEmptyBank = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0, 0, 0};

/// Bits in one next32() draw.
constexpr int kNext32Bits = 32;
/// Vehicle classes Method::vehicles has a bit for.
constexpr int kVehicleClasses = 8;

/// Active condition bits.
uint32_t g_conditions = 0;
/// The bank set_bank() gave, or nullptr for the world's.
const Bank* g_bank = nullptr;
/// The attached rules, or nullptr for roll_plain().
RollRules g_rules = nullptr;

/// 32 random bits from two 16-bit draws.
/// @param r  stream
/// @return 0 to 2^32 - 1
uint32_t next32(rng::Stream& r)
{
   const uint32_t hi = rng::next16(r);
   return (hi << rng::kDrawBits) | rng::next16(r);
}

} // namespace

const Bank& bank()
{
   if (g_bank != nullptr)
   {
      return *g_bank;
   }
   return map::kWorld.encounters != nullptr ? *map::kWorld.encounters : kEmptyBank;
}

void set_bank(const Bank* b)
{
   g_bank = b;
}

void set_rules(RollRules r)
{
   g_rules = r;
}

RollRules rules()
{
   return g_rules;
}

void set_conditions(uint32_t bits)
{
   g_conditions = bits;
}

uint32_t conditions()
{
   return g_conditions;
}

uint16_t table_for(const map::Map& m, uint8_t slot, uint8_t method)
{
   for (uint16_t i = 0; i < m.encounter_ref_count; ++i)
   {
      const Ref& ref = m.encounter_refs[i];
      if (ref.slot == slot && ref.method == method)
      {
         return ref.table;
      }
   }
   return kNoTable;
}

uint32_t entry_weight(const Entry& e, uint32_t active)
{
   if ((e.required & active) != e.required || (e.excluded & active) != 0)
   {
      return 0;
   }
   // The compiler bounds weight x every multiplier to 32 bits (P-18).
   uint32_t w = e.weight;
   const Bank& b = bank();
   for (uint8_t k = 0; k < e.modifier_count; ++k)
   {
      const WeightModifier& wm = b.modifiers[e.modifier_first + k];
      if ((active >> wm.condition) & 1u)
      {
         w = (w * wm.multiplier_q8) >> kQ8Shift;
      }
   }
   return w;
}

bool choose(uint16_t table, uint32_t active, const KeepMask* allowed, rng::Stream& r, uint16_t* entry)
{
   const Bank& b = bank();
   if (table >= b.table_count)
   {
      return false;
   }
   const Table& t = b.tables[table];
   uint32_t weights[kMaxEntriesPerTable];
   uint32_t total = 0;
   const uint16_t n = t.entry_count <= kMaxEntriesPerTable ? t.entry_count : kMaxEntriesPerTable;
   for (uint16_t i = 0; i < n; ++i)
   {
      // an entry the mask leaves out weighs nothing
      const bool may = allowed == nullptr || keeps(*allowed, i);
      weights[i] = may ? entry_weight(b.entries[t.entry_first + i], active) : 0;
      total += weights[i];   // the compiler bounds the sum to 32 bits
   }
   if (total == 0)
   {
      return false;
   }
   // Scales a 32-bit draw to [0, total) without division.
   uint32_t at = static_cast<uint32_t>((static_cast<uint64_t>(next32(r)) * total) >> kNext32Bits);
   for (uint16_t i = 0; i < n; ++i)
   {
      if (at < weights[i])
      {
         *entry = i;
         return true;
      }
      at -= weights[i];
   }
   return false;
}

bool pick(uint16_t table, uint32_t active, rng::Stream& r, Result* out)
{
   uint16_t i = 0;
   if (!choose(table, active, nullptr, r, &i))
   {
      return false;
   }
   const Bank& b = bank();
   const uint16_t at = static_cast<uint16_t>(b.tables[table].entry_first + i);
   const Entry& e = b.entries[at];
   out->species = e.species;
   out->level = static_cast<uint8_t>(rng::between(r, e.min_level, e.max_level));
   out->method = 0;
   out->table = table;
   out->entry = at;
   return true;
}

bool roll_plain(uint16_t table, uint8_t method, rng::Stream& r, Result* out)
{
   const Table& t = bank().tables[table];
   if (!rng::chance(r, t.rate_n, t.rate_m))
   {
      return false;
   }
   if (!pick(table, g_conditions, r, out))
   {
      return false;
   }
   out->method = method;
   return true;
}

namespace
{

/// A table's roll: the attached rules, or roll_plain().
/// @param table   bank table index
/// @param method  method index, stored in the result
/// @param r       stream
/// @param out     the encounter
/// @return true when an encounter starts
bool roll_table(uint16_t table, uint8_t method, rng::Stream& r, Result* out)
{
   return g_rules != nullptr ? g_rules(table, method, r, out) : roll_plain(table, method, r, out);
}

} // namespace

bool roll_step(const map::Map& m, map::MapCell c, uint8_t vehicle, rng::Stream& r, Result* out)
{
   if (c == map::kCellUndefined || !map::cell_encounter(c))
   {
      return false;
   }
   const Bank& b = bank();
   const uint8_t slot = map::cell_encounter_slot(c);
   for (uint8_t i = 0; i < b.method_count; ++i)
   {
      const Method& md = b.methods[i];
      if (md.trigger != Trigger::Step || vehicle >= kVehicleClasses || ((md.vehicles >> vehicle) & 1u) == 0)
      {
         continue;
      }
      const uint16_t t = table_for(m, slot, i);
      if (t == kNoTable)
      {
         continue;
      }
      return roll_table(t, i, r, out);   // one step method rolls per step
   }
   return false;
}

bool roll_at(const map::Map& m, int x, int y, uint8_t method, rng::Stream& r, Result* out)
{
   const map::MapCell c = map::cell_at(m, x, y);
   if (!map::is_painted(m, x, y) || !map::cell_encounter(c) || method >= bank().method_count)
   {
      return false;
   }
   const uint16_t t = table_for(m, map::cell_encounter_slot(c), method);
   return t != kNoTable && roll_table(t, method, r, out);
}

} // namespace capsule::encounter
