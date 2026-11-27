/// @file
/// D-36 §3.1 G2: game statistics. The scripts manifest's `stats` (tools/script_compiler.py) number a game's counters
/// (steps taken, battles won, items bought); a stat without a variable is held here, one 32-bit counter each in the
/// script bank (Bank::stats, Bank::stat_values), and saved in the image's Stats section by its key (capsule/save/
/// save.h), so a manifest reordered or grown keeps the saved values. A stat bound to a variable is that variable's:
/// `stat add` adds to it, and these functions see it as not held.
///
/// Every value stays within 0 and the stat's "max" (default 0xFFFFFFFF). The script VM's `Stat` statement adds through
/// world::add_stat(), whose default is add(); a game may replace that port (capsule/script/port.h) and keep its own
/// rules over get() and set(). A new game clears the counters with the script bank (script::clear_all()).
///
/// The functions are inline over the bank (part of the script module): every program that links a bank (the save
/// image's code, the script VM, a game's port) has them without another source file.
#pragma once

#include "capsule/script/data.h"

#include <cstdint>

namespace capsule::stats
{

/// A stat's table entry when the engine holds it.
/// @param stat  the manifest's index
/// @return its entry, or nullptr (no such stat, or one bound to a variable)
inline const script::StatInfo* held_info(uint8_t stat)
{
   const script::Bank& b = script::kBank;
   if (b.stats == nullptr || b.stat_values == nullptr || stat >= b.stat_count)
   {
      return nullptr;
   }
   return (b.stats[stat].flags & script::kStatHeld) != 0 ? &b.stats[stat] : nullptr;
}

/// The build's statistics (the manifest's, held or not).
/// @return their count (0 to kMaxStats)
inline uint8_t count()
{
   return script::kBank.stat_count;
}

/// Whether the engine holds a stat (it has no variable).
/// @param stat  the manifest's index (script_ids.h STAT_...)
/// @return true when it is held here
inline bool held(uint8_t stat)
{
   return held_info(stat) != nullptr;
}

/// A stat's save key (FNV-1a 32 of "stat:NAME").
/// @param stat  the manifest's index
/// @return its key (0 for a stat not held here)
inline uint32_t key(uint8_t stat)
{
   const script::StatInfo* s = held_info(stat);
   return s != nullptr ? s->key : 0;
}

/// A stat's value.
/// @param stat  the manifest's index
/// @return its value (0 for a stat not held here)
inline uint32_t get(uint8_t stat)
{
   return held_info(stat) != nullptr ? script::kBank.stat_values[stat] : 0;
}

/// Sets a stat (kept within its max; a stat not held here is left alone).
/// @param stat   the manifest's index
/// @param value  the new value
inline void set(uint8_t stat, uint32_t value)
{
   const script::StatInfo* s = held_info(stat);
   if (s != nullptr)
   {
      script::kBank.stat_values[stat] = value < s->max ? value : s->max;
   }
}

/// Adds to a stat, kept within 0 and its max (a stat not held here is left alone).
/// @param stat    the manifest's index
/// @param amount  what to add (negative: take away)
inline void add(uint8_t stat, int32_t amount)
{
   const script::StatInfo* s = held_info(stat);
   if (s == nullptr)
   {
      return;
   }
   // in 64 bits, so neither a large amount nor a value near the max wraps
   int64_t v = static_cast<int64_t>(script::kBank.stat_values[stat]) + amount;
   if (v < 0)
   {
      v = 0;
   }
   else if (v > static_cast<int64_t>(s->max))
   {
      v = s->max;
   }
   script::kBank.stat_values[stat] = static_cast<uint32_t>(v);
}

/// Every held stat back to 0.
inline void clear()
{
   const script::Bank& b = script::kBank;
   for (uint8_t k = 0; k < b.stat_count && b.stat_values != nullptr; ++k)
   {
      b.stat_values[k] = 0;
   }
}

} // namespace capsule::stats
