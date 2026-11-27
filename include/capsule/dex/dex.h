/// @file
/// D-36 §3.1 G3: the dex's records. Two bit sets, seen and caught, over a game's dex entries 1 to N (the game numbers
/// its species: Emerald by national number). N comes from the project file (`"dex": {"entries": N}`), which
/// capsule_game() turns into data() and the two sets (EWRAM, sized by the build); without it data() is nullptr and
/// every query answers 0. Saved in the image's Dex section (capsule/save/save.h). The Dex statement's port
/// (capsule/script/port.h, capsule::script::dex) answers from them by default; a game's own rules (a regional list,
/// a completion rule, extra copies) stay the game's, over these functions.
///
/// The functions are inline over data() (its engine default is in src/save/save.cpp, as plots::data()'s), so every
/// program that links the save image's code has them without another source file.
#pragma once

#include <cstdint>

namespace capsule::dex
{

inline constexpr uint16_t kNoEntry = 0;          ///< Entry 0: no species (entries start at 1).
inline constexpr uint16_t kMaxEntries = 0xFFFF;  ///< The most entries a project may declare (the save's u16 count).
inline constexpr uint32_t kBitsPerByte = 8;      ///< Entries per byte of a set.

/// The bytes one set of n entries takes.
/// @param n  entries
/// @return its bytes
constexpr uint32_t set_bytes(uint32_t n)
{
   return (n + kBitsPerByte - 1) / kBitsPerByte;
}

/// The build's dex (capsule_game() from the project file's "dex").
struct Data
{
   uint16_t entries;   ///< N: entries 1 to N.
   uint8_t* seen;      ///< set_bytes(N) bytes: entry e is bit (e - 1) % 8 of byte (e - 1) / 8.
   uint8_t* caught;    ///< Likewise.
};

/// The build's dex (link time: the generated project source defines it; the engine's default answers nullptr).
/// @return it, or nullptr when the project declares none
const Data* data();

/// The build's entries.
/// @return N (0 without a dex)
inline uint16_t entries()
{
   const Data* d = data();
   return d != nullptr ? d->entries : 0;
}

/// One entry's bit in a set.
/// @param set    the set
/// @param entry  1 to N
/// @param mask   the bit's mask (out)
/// @return its byte, or nullptr (no dex, entry 0 or past N)
inline uint8_t* entry_byte(uint8_t* set, uint16_t entry, uint8_t& mask)
{
   if (set == nullptr || entry == kNoEntry || entry > entries())
   {
      return nullptr;
   }
   const uint32_t bit = entry - 1u;
   mask = static_cast<uint8_t>(1u << (bit % kBitsPerByte));
   return &set[bit / kBitsPerByte];
}

/// Whether an entry is in a set.
/// @param caught  the caught set (else seen)
/// @param entry   1 to N
/// @return true when it is (false for no dex, entry 0 or past N)
inline bool has(bool caught, uint16_t entry)
{
   const Data* d = data();
   uint8_t mask = 0;
   const uint8_t* b = d != nullptr ? entry_byte(caught ? d->caught : d->seen, entry, mask) : nullptr;
   return b != nullptr && (*b & mask) != 0;
}

/// Puts an entry in a set or takes it out (nothing for no dex, entry 0 or past N).
/// @param caught  the caught set (else seen)
/// @param entry   1 to N
/// @param on      in (else out)
inline void mark(bool caught, uint16_t entry, bool on)
{
   const Data* d = data();
   uint8_t mask = 0;
   uint8_t* b = d != nullptr ? entry_byte(caught ? d->caught : d->seen, entry, mask) : nullptr;
   if (b != nullptr)
   {
      *b = static_cast<uint8_t>(on ? (*b | mask) : (*b & ~mask));
   }
}

/// Whether an entry was seen.
/// @param entry  1 to N
/// @return true when it was
inline bool seen(uint16_t entry)
{
   return has(false, entry);
}

/// Whether an entry was caught.
/// @param entry  1 to N
/// @return true when it was
inline bool caught(uint16_t entry)
{
   return has(true, entry);
}

/// Marks an entry seen (or not).
/// @param entry  1 to N
/// @param on     seen (else not)
inline void set_seen(uint16_t entry, bool on = true)
{
   mark(false, entry, on);
}

/// Marks an entry caught (or not). Being caught does not mark it seen: a game that wants both sets both.
/// @param entry  1 to N
/// @param on     caught (else not)
inline void set_caught(uint16_t entry, bool on = true)
{
   mark(true, entry, on);
}

/// The entries of a range in a set.
/// @param caught  the caught set (else seen)
/// @param first   the range's first entry (1 or more)
/// @param last    its last (past N: N)
/// @return how many are in the set
inline uint32_t count(bool caught, uint16_t first, uint16_t last)
{
   uint32_t n = 0;
   const uint16_t end = last < entries() ? last : entries();
   for (uint32_t e = first < 1 ? 1 : first; e <= end; ++e)
   {
      n += has(caught, static_cast<uint16_t>(e)) ? 1 : 0;
   }
   return n;
}

/// Both sets empty (a new game).
inline void clear()
{
   const Data* d = data();
   if (d == nullptr)
   {
      return;
   }
   for (uint32_t i = 0; i < set_bytes(d->entries); ++i)
   {
      d->seen[i] = 0;
      d->caught[i] = 0;
   }
}

} // namespace capsule::dex
