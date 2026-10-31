/// @file
/// Little-endian load/store helpers (P-19).
///
/// Data that crosses a boundary (ROM blobs, saves, link and trade records,
/// emulated GBA VRAM) has a fixed little-endian byte layout. It is read and
/// written only through these helpers, never by casting a byte pointer to a
/// multi-byte type:
///   - big-endian hosts (PowerPC) get the same bytes as the GBA;
///   - no misaligned multi-byte access: the SH-4 (Dreamcast) faults on one,
///     and the ARM7TDMI (GBA) silently returns rotated data.
///
/// Byte-wise and constexpr; compilers fold them into single loads/stores where
/// the target allows it.
#pragma once

#include <cstdint>

namespace capsule
{

/// @name Little-endian byte layout
/// Byte positions in a multi-byte value (byte 0 is the lowest) and the shift
/// that moves each byte into place.
///@{
inline constexpr int kLeByte2 = 2;         ///< Index of the third byte.
inline constexpr int kLeByte3 = 3;         ///< Index of the fourth byte.
inline constexpr int kLeShiftByte1 = 8;    ///< Shift of byte 1.
inline constexpr int kLeShiftByte2 = 16;   ///< Shift of byte 2.
inline constexpr int kLeShiftByte3 = 24;   ///< Shift of byte 3.
///@}

/// Reads a little-endian 16-bit value.
/// @param p  first byte; any alignment
/// @return the value
constexpr uint16_t load_le16(const uint8_t* p)
{
   return static_cast<uint16_t>(p[0] | (p[1] << kLeShiftByte1));
}

/// Reads a little-endian 32-bit value.
/// @param p  first byte; any alignment
/// @return the value
constexpr uint32_t load_le32(const uint8_t* p)
{
   return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << kLeShiftByte1) |
          (static_cast<uint32_t>(p[kLeByte2]) << kLeShiftByte2) | (static_cast<uint32_t>(p[kLeByte3]) << kLeShiftByte3);
}

/// Writes a 16-bit value little-endian.
/// @param p  first byte; any alignment
/// @param v  value to write
constexpr void store_le16(uint8_t* p, uint16_t v)
{
   p[0] = static_cast<uint8_t>(v);
   p[1] = static_cast<uint8_t>(v >> kLeShiftByte1);
}

/// Writes a 32-bit value little-endian.
/// @param p  first byte; any alignment
/// @param v  value to write
constexpr void store_le32(uint8_t* p, uint32_t v)
{
   p[0] = static_cast<uint8_t>(v);
   p[1] = static_cast<uint8_t>(v >> kLeShiftByte1);
   p[kLeByte2] = static_cast<uint8_t>(v >> kLeShiftByte2);
   p[kLeByte3] = static_cast<uint8_t>(v >> kLeShiftByte3);
}

/// Reads a little-endian signed 16-bit value.
/// @param p  first byte; any alignment
/// @return the value
constexpr int16_t load_le16s(const uint8_t* p)
{
   return static_cast<int16_t>(load_le16(p));
}

/// Reads a little-endian signed 32-bit value.
/// @param p  first byte; any alignment
/// @return the value
constexpr int32_t load_le32s(const uint8_t* p)
{
   return static_cast<int32_t>(load_le32(p));
}

} // namespace capsule
