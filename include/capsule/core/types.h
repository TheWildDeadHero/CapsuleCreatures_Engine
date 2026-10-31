/// @file
/// Fixed point (P-4) and intent-named / strong types (P-9).
#pragma once

#include <cstdint>
#include <type_traits>

/// The CapsuleCreatures engine.
namespace capsule
{

/// @name Fixed point (P-4)
/// One fixed-point format, 8 fractional bits. Apply a multiplier as
/// (value * q8) >> 8; results truncate. Callers own the range (P-18).
///@{
using Q8 = int16_t;   ///< Q8.8 fixed point.
using Q8Wide = int32_t;   ///< Q24.8 fixed point, for accumulators and positions.
inline constexpr int kQ8Shift = 8;                         ///< Fractional bits.
inline constexpr Q8Wide kQ8One = Q8Wide{1} << kQ8Shift;     ///< 1.0 in Q8.
///@}

/// @name Common quantities
///@{
inline constexpr unsigned kBitsPerByte = 8u;   ///< Bits in one byte.
inline constexpr int kWordBytes = 4;           ///< Bytes in one 32-bit word.
inline constexpr int kHalf = 2;                ///< Divisor that halves a value (midpoints, centring).
inline constexpr int kTilePixels = 8;          ///< Pixels per side of an 8x8 hardware tile (BG or OBJ character).
inline constexpr int kBgMapCells = 32;         ///< Cells per side of a regular background map (one screen block).
inline constexpr int kFramesPerSecond = 60;    ///< Display frames per second (the GBA's ~59.73, rounded).
///@}

/// @name Strong types (P-9)
/// IDs that must never be mixed. A swapped argument is a compile error; the
/// machine code is identical to the raw integer.
///@{

/// Index into the species table.
enum class SpeciesIndex : uint16_t
{
};
/// Index into the move table.
enum class MoveIndex : uint16_t
{
};
/// Asset Key v1 (C-02d): FNV-1a 32 of "ns:NAME".
enum class AssetKey : uint32_t
{
};
///@}

/// The underlying integer of a strong type or enum.
/// @tparam E  an enum type
/// @param e   the value
/// @return @p e as its underlying integer type
template <typename E>
constexpr auto raw(E e)
{
   return static_cast<std::underlying_type_t<E>>(e);
}

/// Mask for one bit of a flag enum (P-9). Flag enums list bit positions and
/// end with NUM_...; pair each with a fits_in() check, e.g.
/// `static_assert(fits_in<TraitMask>(NUM_TRAITS));`
/// @tparam Mask  unsigned storage type of the mask
/// @tparam E     the flag enum
/// @param bit    bit position
/// @return a mask with only @p bit set
template <typename Mask, typename E>
constexpr Mask flag_mask(E bit)
{
   static_assert(std::is_unsigned_v<Mask>);
   return static_cast<Mask>(Mask{1} << static_cast<unsigned>(bit));
}

/// Whether @p count flags fit in @p Mask.
/// @tparam Mask  unsigned storage type
/// @param count  number of flag bits (the enum's NUM_... entry)
/// @return true if every flag has a bit in @p Mask
template <typename Mask>
constexpr bool fits_in(unsigned count)
{
   return count <= sizeof(Mask) * kBitsPerByte;
}

} // namespace capsule
