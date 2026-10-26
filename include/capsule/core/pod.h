/// @file
/// Plain-data size lock for game data structs (P-7, P-1).
///
/// Game data structs are plain aggregates. Every data struct is followed by
/// CAPSULE_POD(T, bytes), which enforces plain data and the P-1 size lock.
#pragma once

#include <cstddef>
#include <type_traits>

namespace capsule
{

/// @name Word padding
/// Lengths of the `reserved` arrays that pad a data struct to a whole 32-bit
/// word, by how many bytes its last word already holds.
///@{
inline constexpr int kPadAfterOneByte = 3;    ///< The last word holds 1 byte.
inline constexpr int kPadAfterTwoBytes = 2;   ///< The last word holds 2 bytes.
inline constexpr int kPadAfterThreeBytes = 1; ///< The last word holds 3 bytes.
///@}

} // namespace capsule

/// Asserts at compile time that @p T is plain data of exactly @p bytes bytes:
/// trivially copyable, standard layout and an aggregate, so the compiled ROM
/// tables (P-1) and the C++ struct can never drift apart.
/// @param T      the struct to check
/// @param bytes  its required size in bytes
#define CAPSULE_POD(T, bytes)                                                                                          \
   static_assert(std::is_trivially_copyable_v<T>);                                                                     \
   static_assert(std::is_standard_layout_v<T>);                                                                        \
   static_assert(std::is_aggregate_v<T>);                                                                              \
   static_assert(sizeof(T) == (bytes))
