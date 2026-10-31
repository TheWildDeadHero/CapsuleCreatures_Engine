/// @file
/// Saturating arithmetic for stored game values (P-18).
///
/// No silent under/overflow. Stored game values change only through these
/// helpers, which clamp to the field's declared range [lo, hi]; nothing wraps.
#pragma once

#include <type_traits>

namespace capsule
{

/// Clamps @p v to [@p lo, @p hi].
/// @tparam T  any ordered type
/// @param v   value to clamp
/// @param lo  lowest allowed value
/// @param hi  highest allowed value
/// @return @p v limited to the range
template <typename T>
constexpr T clamp_to(T v, T lo, T hi)
{
   return v < lo ? lo : (hi < v ? hi : v);
}

/// Adds @p delta to @p v, saturating at [@p lo, @p hi], including when the
/// sum would overflow @p T itself.
/// @tparam T     an integer type
/// @param v      current value
/// @param delta  amount to add (may be negative)
/// @param lo     lowest allowed value
/// @param hi     highest allowed value
/// @return the clamped sum
template <typename T>
constexpr T sat_add(T v, T delta, T lo, T hi)
{
   static_assert(std::is_integral_v<T>);
   T out{};
   if (__builtin_add_overflow(v, delta, &out))
   {
      return delta < T{} ? lo : hi;
   }
   return clamp_to(out, lo, hi);
}

/// Subtracts @p delta from @p v, saturating at [@p lo, @p hi], including when
/// the difference would overflow @p T itself.
/// @tparam T     an integer type
/// @param v      current value
/// @param delta  amount to subtract (may be negative)
/// @param lo     lowest allowed value
/// @param hi     highest allowed value
/// @return the clamped difference
template <typename T>
constexpr T sat_sub(T v, T delta, T lo, T hi)
{
   static_assert(std::is_integral_v<T>);
   T out{};
   if (__builtin_sub_overflow(v, delta, &out))
   {
      return delta < T{} ? hi : lo;
   }
   return clamp_to(out, lo, hi);
}

} // namespace capsule
