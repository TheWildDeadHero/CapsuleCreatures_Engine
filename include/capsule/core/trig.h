/// @file
/// Integer sine and cosine (P-6: no floating point).
///
/// Angles are 16-bit turns (65536 = one full turn); results are Q14
/// (16384 = 1.0). A 65-entry quarter-wave table gives 256 steps per turn; the
/// low 8 bits of the angle are ignored. Used for sprite rotation (A-8) and
/// wave effects (A-7).
#pragma once

#include <cstdint>

namespace capsule
{

/// Unit of sin_q14() / cos_q14() results: 1.0.
inline constexpr int32_t kQ14One = 16384;
/// Fractional bits of a Q14 value (x >> kQ14Shift drops them).
inline constexpr int kQ14Shift = 14;

/// @name Angle layout
/// A 16-bit angle is 8 table-step bits over 8 ignored bits; the step's top 2
/// bits are the quadrant, its low 6 bits the index into the quarter wave.
///@{
inline constexpr int kAngleStepShift = 8;          ///< Ignored low angle bits.
inline constexpr int kQuadrantShift = 6;           ///< Step bits below the quadrant.
inline constexpr int kQuarterStepMask = 63;        ///< Step bits inside a quadrant.
inline constexpr int kQuarterSteps = 64;           ///< Steps per quadrant (the table has one more entry).
inline constexpr int kNegativeQuadrant = 2;        ///< First quadrant with a negative sine.
inline constexpr uint16_t kQuarterTurn = 16384;    ///< 90 degrees as a 16-bit turn.
inline constexpr uint16_t kHalfTurn = 32768;       ///< 180 degrees as a 16-bit turn.
///@}

/// sin(i * 90 / 64 degrees) in Q14, i = 0..64.
inline constexpr int16_t kQuarterSineQ14[65] = {
   0,     402,   804,   1205,  1606,  2006,  2404,  2801,  3196,  3590,  3981,  4370,  4756,
   5139,  5520,  5897,  6270,  6639,  7005,  7366,  7723,  8076,  8423,  8765,  9102,  9434,
   9760,  10080, 10394, 10702, 11003, 11297, 11585, 11866, 12140, 12406, 12665, 12916, 13160,
   13395, 13623, 13842, 14053, 14256, 14449, 14635, 14811, 14978, 15137, 15286, 15426, 15557,
   15679, 15791, 15893, 15986, 16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379, 16384,
};

/// Sine of an angle.
/// @param angle  16-bit turn (16384 = 90 degrees)
/// @return sine, Q14
constexpr int32_t sin_q14(uint16_t angle)
{
   const int step = angle >> kAngleStepShift;    // 0-255
   const int quadrant = step >> kQuadrantShift;  // 0-3
   const int i = step & kQuarterStepMask;

   // Odd quadrants run the quarter wave backwards; the second half turn is negative.
   const int32_t v = (quadrant & 1) ? kQuarterSineQ14[kQuarterSteps - i] : kQuarterSineQ14[i];
   return quadrant >= kNegativeQuadrant ? -v : v;
}

/// Cosine of an angle.
/// @param angle  16-bit turn
/// @return cosine, Q14
constexpr int32_t cos_q14(uint16_t angle)
{
   return sin_q14(static_cast<uint16_t>(angle + kQuarterTurn));   // cos(a) = sin(a + 90 degrees)
}

static_assert(sin_q14(0) == 0 && sin_q14(16384) == kQ14One && sin_q14(32768) == 0 && sin_q14(49152) == -kQ14One);
static_assert(cos_q14(0) == kQ14One && cos_q14(32768) == -kQ14One);

} // namespace capsule
