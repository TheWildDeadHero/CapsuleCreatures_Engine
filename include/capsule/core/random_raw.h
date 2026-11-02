/// @file
/// Raw draws with explicit arithmetic (M5 BX): for a game whose reference takes its random numbers as one 16-bit
/// draw reduced with the remainder or a bit mask, where the ranges of capsule/core/random.h (multiply-shift, and no
/// draw at all for certain odds) would give other values or skip draws.
///
/// Each helper draws exactly once from the stream, whatever its argument, so the number of draws is the caller's
/// (a reference that draws even when the odds are certain does so here too):
/// - draw(): the 16 bits as the linked generator returns them (rng::next16());
/// - draw_mod(): draw() % n, the remainder of the division (the ARM7TDMI has no hardware divide: the compiler's
///   runtime divides, as the reference's own code does);
/// - draw_mask(): draw() & m, a power-of-two range or a bit field.
/// The overloads without a stream draw from the gameplay stream (the one a reference game's single generator maps
/// to).
#pragma once

#include "capsule/core/random.h"

#include <cstdint>

namespace capsule::rng
{

/// One raw draw.
/// @param s  the stream
/// @return 0-65535
inline uint16_t draw(Stream& s)
{
   return next16(s);
}

/// One raw draw reduced with the remainder: draw() % @p n.
/// @param s  the stream
/// @param n  the divisor (0 draws and returns the draw unchanged: a division by zero has no defined result, so the
///           value is kept instead of trapping)
/// @return 0 to n - 1 (the draw itself when n is 0)
inline uint16_t draw_mod(Stream& s, uint16_t n)
{
   const uint16_t r = next16(s);
   return n == 0 ? r : static_cast<uint16_t>(r % n);
}

/// One raw draw reduced with a bit mask: draw() & @p m.
/// @param s  the stream
/// @param m  the mask
/// @return the masked draw
inline uint16_t draw_mask(Stream& s, uint16_t m)
{
   return static_cast<uint16_t>(next16(s) & m);
}

/// draw() from the gameplay stream.
/// @return 0-65535
inline uint16_t draw()
{
   return draw(gameplay);
}

/// draw_mod() from the gameplay stream.
/// @param n  the divisor
/// @return draw() % n
inline uint16_t draw_mod(uint16_t n)
{
   return draw_mod(gameplay, n);
}

/// draw_mask() from the gameplay stream.
/// @param m  the mask
/// @return draw() & m
inline uint16_t draw_mask(uint16_t m)
{
   return draw_mask(gameplay, m);
}

}   // namespace capsule::rng
