/// @file
/// The engine's default generator (D-22 decision 4): next16() for every game
/// that does not link its own (cmake/parts/rng.cmake).
///
/// Choice: a 32-bit permuted congruential generator, the RXS-M-XS 32/32
/// variant of PCG (M. O'Neill, 2014). Each draw
/// 1. advances the state with a full-period LCG modulo 2^32,
///    x = 747796405 x + 2891336453 (multiplier = 1 mod 4 and an odd
///    increment, so every one of the 2^32 states is visited once per period);
/// 2. permutes the new state: a random xorshift (the top 4 bits pick a right
///    shift of 4-19 bits), a multiply by 277803737, then an xorshift by 22.
///    The permutation is a bijection on 32 bits, so the output has the
///    state's full period too, and it mixes the LCG's weak low bits with its
///    strong high bits;
/// 3. returns the top 16 bits of that output.
/// Integers only: two multiplies, shifts and xors per draw (no division, and
/// register-specified shifts are single instructions on the ARM7TDMI). The
/// state stays 32 bits (Stream unchanged).
#include "capsule/core/random.h"

namespace capsule::rng
{

namespace
{

/// Multiplier of the state LCG (full period: = 1 mod 4).
constexpr uint32_t kStateMul = 747796405u;
/// Increment of the state LCG (full period: odd).
constexpr uint32_t kStateAdd = 2891336453u;
/// Bits of the state that select the random shift (its top 4 bits).
constexpr int kSelectShift = 28;
/// Smallest random shift: the selected shift is 4 + (state >> 28), 4-19.
constexpr int kSelectBase = 4;
/// Multiplier of the output permutation (odd, so the multiply is a bijection).
constexpr uint32_t kOutputMul = 277803737u;
/// Final xorshift of the output permutation.
constexpr int kOutputShift = 22;
/// Bits in the state (Stream::state).
constexpr int kStateBits = 32;
/// Low output bits a draw drops (the top 16 bits are returned).
constexpr int kDrawShift = kStateBits - kDrawBits;

/// Mask of a value's residue modulo 4 (the full-period rule for the multiplier).
constexpr uint32_t kModFourMask = 3u;

static_assert((kStateMul & kModFourMask) == 1u, "full period needs multiplier = 1 mod 4");
static_assert((kStateAdd & 1u) == 1u, "full period needs an odd increment");
static_assert((kOutputMul & 1u) == 1u, "the output multiply must be invertible");

} // namespace

uint16_t next16(Stream& s)
{
   s.state = s.state * kStateMul + kStateAdd;

   // Output permutation of the new state (RXS-M-XS): random xorshift, multiply, xorshift.
   const uint32_t x = s.state;
   uint32_t word = ((x >> ((x >> kSelectShift) + kSelectBase)) ^ x) * kOutputMul;
   word ^= word >> kOutputShift;
   return static_cast<uint16_t>(word >> kDrawShift);
}

} // namespace capsule::rng
