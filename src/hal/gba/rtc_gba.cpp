/// @file
/// M6a K1: the GBA real-time clock (capsule/hal/rtc.h): the cartridge's Seiko S-3511A on the GPIO port, from the
/// published hardware description of the port and the chip (GBATEK "GBA Cart Real-Time Clock"; NOT TESTED ON
/// HARDWARE, only against emulators' models of the chip).
///
/// The port's three data lines are the chip's serial clock (bit 0), data (bit 1) and chip select (bit 2); the
/// direction register sets each line's direction (1: out) and the control register makes the port readable (else
/// reads at its addresses give the ROM's bytes). A transaction:
///   1. chip select low with the clock high, then chip select high (the chip starts listening);
///   2. the command byte, most significant bit first: 0110 (fixed), the register (3 bits), then 1 to read;
///   3. the register's bytes, least significant bit first, the data line turned to an input; each bit is read
///      while the clock is high, after a low phase;
///   4. chip select low again.
/// Registers read here: the status (1 byte: bit 7 the power-lost flag, bit 6 the 24-hour mode) and the date and time
/// (7 bytes, BCD: year, month, day, weekday, hour (bit 7: afternoon), minute, second). A chip in 12-hour mode or
/// with its power-lost flag set reads as invalid (the game's clock then falls back to play time, C-05g); the
/// engine never sets the chip's time.
///
/// The port is made readable only for a read and given back to the ROM after it, so the ROM's bytes at the port's
/// addresses (the cartridge's start-up code) stay readable. No interrupt handler touches the port; interrupts stay
/// on (a read takes about 130 port writes).
#include "capsule/hal/rtc.h"

#include <cstdint>

namespace capsule::hal::rtc
{

namespace
{

constexpr uintptr_t kPortData = 0x080000C4u;        ///< The GPIO port's data lines.
constexpr uintptr_t kPortDirection = 0x080000C6u;   ///< Their directions (1: out).
constexpr uintptr_t kPortControl = 0x080000C8u;     ///< 1: the port reads back (0: the ROM's bytes).

constexpr uint16_t kSck = 1u << 0;   ///< The serial clock line.
constexpr uint16_t kSio = 1u << 1;   ///< The serial data line.
constexpr uint16_t kCs = 1u << 2;    ///< The chip select line.
constexpr uint16_t kAllOut = kSck | kSio | kCs;   ///< Every line an output.
constexpr uint16_t kDataIn = kSck | kCs;          ///< The data line an input, the others outputs.
constexpr uint16_t kReadable = 1;                 ///< kPortControl: the port reads back.
constexpr uint16_t kRomVisible = 0;               ///< kPortControl: the ROM's bytes.
constexpr int kSioShift = 1;                      ///< The data line's bit.

constexpr int kByteBits = 8;                   ///< Bits a command or register byte has.
constexpr uint8_t kCommandBase = 0x60;         ///< A command's fixed upper bits (0110).
constexpr int kRegisterShift = 1;              ///< A command's register number sits above the read bit.
constexpr uint8_t kReadBit = 1;                ///< A command's low bit: read.
constexpr uint8_t kRegisterStatus = 1;         ///< The status register.
constexpr uint8_t kRegisterDateTime = 2;       ///< The date and time registers.
constexpr int kDateTimeBytes = 7;              ///< Bytes of the date and time.
constexpr uint8_t kStatusPowerLost = 0x80;     ///< Status: the battery ran out since the clock was set.
constexpr uint8_t kStatus24Hour = 0x40;        ///< Status: 24-hour mode.
constexpr uint8_t kHourPm = 0x80;              ///< The hour's afternoon flag (24-hour mode sets it too).
constexpr uint8_t kNibbleBits = 4;             ///< BCD: a digit's bits.
constexpr uint8_t kNibbleMask = 0x0F;          ///< BCD: a digit.
constexpr uint8_t kDecimalBase = 10;           ///< BCD: digits are decimal.
constexpr int kSetupWrites = 3;                ///< Port writes a clock low phase lasts (the chip's setup time).

/// @name Positions in the date and time registers (3, the weekday, is not read: the engine works it out from the
/// date)
///@{
constexpr int kYear = 0;     ///< The year in the century.
constexpr int kMonth = 1;    ///< The month.
constexpr int kDay = 2;      ///< The day of the month.
constexpr int kHour = 4;     ///< The hour (with the afternoon flag).
constexpr int kMinute = 5;   ///< The minute.
constexpr int kSecond = 6;   ///< The second.
///@}

/// A port register.
/// @param address  its address
/// @return it
volatile uint16_t& port(uintptr_t address)
{
   return *reinterpret_cast<volatile uint16_t*>(address);
}

/// Sends a byte, most significant bit first (each bit: the clock low for kSetupWrites writes, then high).
/// @param value  the byte
void send(uint8_t value)
{
   for (int i = kByteBits - 1; i >= 0; --i)
   {
      const uint16_t bit = static_cast<uint16_t>(((value >> i) & 1u) << kSioShift);
      for (int w = 0; w < kSetupWrites; ++w)
      {
         port(kPortData) = bit | kCs;
      }
      port(kPortData) = bit | kSck | kCs;
   }
}

/// Receives a byte, least significant bit first.
/// @return it
uint8_t receive()
{
   uint8_t value = 0;
   for (int i = 0; i < kByteBits; ++i)
   {
      for (int w = 0; w < kSetupWrites; ++w)
      {
         port(kPortData) = kCs;
      }
      port(kPortData) = kSck | kCs;
      const uint8_t bit = static_cast<uint8_t>((port(kPortData) & kSio) >> kSioShift);
      value = static_cast<uint8_t>(value | (bit << i));
   }
   return value;
}

/// Reads a register's bytes, the port readable only for the transaction.
/// @param reg    the register
/// @param out    receives the bytes
/// @param count  how many
void read_register(uint8_t reg, uint8_t* out, int count)
{
   port(kPortControl) = kReadable;
   port(kPortData) = kSck;
   port(kPortData) = kSck | kCs;
   port(kPortDirection) = kAllOut;
   send(static_cast<uint8_t>(kCommandBase | (reg << kRegisterShift) | kReadBit));
   port(kPortDirection) = kDataIn;
   for (int i = 0; i < count; ++i)
   {
      out[i] = receive();
   }
   port(kPortData) = kSck;   // chip select low: the transaction ends
   port(kPortControl) = kRomVisible;
}

/// A BCD byte's value.
/// @param bcd    the byte
/// @param value  receives its value
/// @return false when a digit is not decimal
bool from_bcd(uint8_t bcd, uint8_t& value)
{
   const uint8_t high = static_cast<uint8_t>(bcd >> kNibbleBits);
   const uint8_t low = static_cast<uint8_t>(bcd & kNibbleMask);
   if (high >= kDecimalBase || low >= kDecimalBase)
   {
      return false;
   }
   value = static_cast<uint8_t>(high * kDecimalBase + low);
   return true;
}

} // namespace

bool read(DateTime& out)
{
   uint8_t status = 0;
   read_register(kRegisterStatus, &status, 1);
   if ((status & kStatusPowerLost) != 0 || (status & kStatus24Hour) == 0)
   {
      return false;   // no chip (the data line reads all ones or zeros), a lost power or 12-hour mode
   }
   uint8_t raw[kDateTimeBytes] = {};
   read_register(kRegisterDateTime, raw, kDateTimeBytes);
   raw[kHour] = static_cast<uint8_t>(raw[kHour] & ~kHourPm);
   uint8_t year = 0;
   DateTime t{};
   if (!from_bcd(raw[kYear], year) || !from_bcd(raw[kMonth], t.month) || !from_bcd(raw[kDay], t.day) ||
       !from_bcd(raw[kHour], t.hour) || !from_bcd(raw[kMinute], t.minute) || !from_bcd(raw[kSecond], t.second))
   {
      return false;
   }
   t.year = static_cast<uint16_t>(kFirstYear + year);
   if (!in_range(t))
   {
      return false;
   }
   out = t;
   return true;
}

void set_for_test(const DateTime&, bool)
{
}

} // namespace capsule::hal::rtc
