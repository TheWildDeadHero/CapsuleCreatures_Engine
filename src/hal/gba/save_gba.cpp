/// @file
/// GBA save medium (see capsule/hal/save.h): the cartridge's 64 KB flash, or with CAPSULE_SAVE_SRAM defined (CMake
/// CAPSULE_GBA_SAVE_SRAM) its 32 KB SRAM. Either is an 8-bit bus at 0x0E000000 read with 8 wait states. The ROM
/// carries the save type's tag string, which emulators and flash carts look for.
///
/// Flash (C6, review4 finding 8; NOT TESTED ON HARDWARE, only against emulators' flash models and the tests' fake
/// medium):
///   - the chip is identified once, at the first begin() (manufacturer and device id) and its command set chosen:
///     Macronix, Panasonic, Sanyo and SST chips erase 4 KB sectors and program single bytes; Atmel chips program
///     128-byte pages (a page write erases it); an unknown id makes every erase and program fail (MediumError),
///     reads still work;
///   - C7: id mode is entered and left with waits per the chips' datasheets (kIdWaitMicros each, about 20 ms),
///     timed by VCOUNT's scanlines (the LCD's timing runs in forced blank too), not a guessed spin; interrupts stay
///     masked for both waits (about 2.4 frames, once per power-on);
///   - C7: 128 KB chips (Macronix MX29L010, Sanyo LE26FV10N1TS) have bank 0 selected at every begin() (the bank
///     command AA / 55 / B0, then the bank number written to 0x0000), so the first 64 KB are used whatever bank a
///     game or a reset left selected;
///   - C7: a poll that times out ends with the unlocked reset sequence (AA / 55 / F0 at the command address);
///   - C7: Atmel pages are written once per save: erase() only marks its range, program() writes each page whole
///     (bytes it does not give read 0xFF while the page lies in the marked range, else what the page holds); the
///     engine streams in page-aligned 128-byte chunks (capsule::save kChunk), so each page is one page write;
///   - the code that reads the chip, reads its id and polls it runs from IWRAM (CAPSULE_IWRAM_CODE): the cartridge
///     bus cannot fetch ROM code while the flash answers in id or status mode;
///   - interrupts are masked only while a command sequence and each status poll run (SaveScope per poll), not for a
///     whole erase, so VBlank keeps its pace during a sector erase (about 100 ms);
///   - C15a (R10 L3): reads do not mask interrupts at all (ReadScope: only the 8 wait states), as the reference's
///     flash reads: a boot or Continue read of tens of KB no longer merges vertical blanks or delays the audio's
///     VCOUNT interrupt past the ring's end. A read runs in the chip's read mode, after any command has finished
///     (every erase and program waits for its status poll), from IWRAM; no interrupt handler touches the save chip
///     or WAITCNT, so one that runs in the middle of a read changes nothing the read sees. The same holds for the
///     checks after an erase (erased()) and the page reads of an Atmel program;
///   - an erase is checked over the whole sector (every byte reads 0xFF), a program byte by byte.
#include "capsule/hal/save.h"

#include "capsule/core/memory.h"

#include <gba_interrupt.h>

#include <cstdint>

namespace capsule::hal::save
{

namespace
{

/// Address of the save chip.
constexpr uintptr_t kSaveBase = 0x0E000000u;
/// WAITCNT's address.
constexpr uintptr_t kWaitcntAddress = 0x04000204u;
/// WAITCNT bits of the SRAM / flash wait states.
constexpr uint16_t kSramWaitMask = 0x0003u;
/// Those bits for 8 wait states (what flash needs; SRAM takes it too).
constexpr uint16_t kSramWait8 = 0x0003u;
/// An erased byte.
constexpr uint8_t kErased = 0xFF;
/// Alignment of the save type's tag (word-aligned, where the tools that look for it search).
constexpr int kTagAlign = 4;

#if defined(CAPSULE_SAVE_SRAM)
/// Bytes of the SRAM.
constexpr uint32_t kSaveBytes = 0x8000u;
/// The save type's tag (emulators read it from the ROM).
alignas(kTagAlign) const char kSaveTag[] = "SRAM_V113";
#else
/// Bytes of the flash used (a 128 KB chip is used as its first 64 KB bank).
constexpr uint32_t kSaveBytes = 0x10000u;
/// Bytes of a flash sector (the erase unit).
constexpr uint32_t kSectorBytes = 0x1000u;
/// Bytes of an Atmel page (its program unit).
constexpr uint32_t kAtmelPage = 0x80u;
/// The save type's tag (emulators read it from the ROM).
alignas(kTagAlign) const char kSaveTag[] = "FLASH512_V131";
/// @name Flash command addresses and bytes
///@{
constexpr uint32_t kCommandA = 0x5555u;     ///< First command address.
constexpr uint32_t kCommandB = 0x2AAAu;     ///< Second command address.
constexpr uint8_t kUnlockA = 0xAA;          ///< First unlock byte.
constexpr uint8_t kUnlockB = 0x55;          ///< Second unlock byte.
constexpr uint8_t kEraseSetup = 0x80;       ///< Erase set-up.
constexpr uint8_t kEraseSector = 0x30;      ///< Erase the sector written to.
constexpr uint8_t kProgramByte = 0xA0;      ///< Program the next byte written (Atmel: the next page).
constexpr uint8_t kIdEnter = 0x90;          ///< Id mode.
constexpr uint8_t kIdExit = 0xF0;           ///< Back to reading (also after a timeout).
constexpr uint8_t kBankSelect = 0xB0;       ///< Bank select (128 KB chips; the bank number follows at 0x0000).
constexpr uint8_t kBankZero = 0;            ///< The bank used (the first 64 KB).
///@}
/// @name Id mode waits (C7)
///@{
constexpr uint32_t kIdWaitMicros = 20000;           ///< Wait after entering and after leaving id mode (datasheets).
constexpr uint64_t kCpuHz = 16777216u;              ///< The GBA's clock.
constexpr uint64_t kCyclesPerLine = 1232u;          ///< CPU cycles per scanline (VCOUNT steps once per line).
constexpr uint64_t kMicrosPerSecond = 1000000u;     ///< Microseconds per second.
/// Scanlines the wait takes (rounded up: about 273 for 20 ms).
constexpr uint32_t kIdWaitLines = static_cast<uint32_t>(
   (kIdWaitMicros * kCpuHz + kCyclesPerLine * kMicrosPerSecond - 1) / (kCyclesPerLine * kMicrosPerSecond));
/// VCOUNT's address (the current scanline).
constexpr uintptr_t kVcountAddress = 0x04000006u;
///@}
/// @name Chip ids (manufacturer << 8 | device)
///@{
constexpr uint16_t kMacronix512 = 0xC21Cu;  ///< Macronix MX29L512.
constexpr uint16_t kMacronix1M = 0xC209u;   ///< Macronix MX29L010 (128 KB, its first bank).
constexpr uint16_t kPanasonic = 0x321Bu;    ///< Panasonic MN63F805MNP.
constexpr uint16_t kSanyo = 0x6213u;        ///< Sanyo LE26FV10N1TS (128 KB, its first bank).
constexpr uint16_t kSst = 0xBFD4u;          ///< SST 39VF512.
constexpr uint16_t kAtmel = 0x1F3Du;        ///< Atmel AT29LV512 (page programming).
///@}
/// Status polls before an erase or program counts as failed (a sector erase takes up to ~100 ms).
constexpr uint32_t kPollLimit = 0x400000u;
/// Bits of the manufacturer byte in a chip id.
constexpr uint32_t kManufacturerShift = 8;
#endif

/// One byte of the save chip.
/// @param offset  byte offset
/// @return the byte (volatile: every access reaches the bus)
volatile uint8_t& chip(uint32_t offset)
{
   return *reinterpret_cast<volatile uint8_t*>(kSaveBase + offset);
}

/// Interrupts masked and 8 wait states for the save chip while a scope lasts (a command sequence or one poll).
struct SaveScope
{
   const uint16_t ime;       ///< REG_IME before.
   const uint16_t waitcnt;   ///< WAITCNT before.

   /// Masks interrupts and sets the wait states.
   SaveScope()
      : ime(REG_IME), waitcnt(*reinterpret_cast<volatile uint16_t*>(kWaitcntAddress))
   {
      REG_IME = 0;
      *reinterpret_cast<volatile uint16_t*>(kWaitcntAddress) =
         static_cast<uint16_t>((waitcnt & ~kSramWaitMask) | kSramWait8);
      // the tag must stay in the ROM: this use keeps the linker from dropping it
      asm volatile("" : : "r"(kSaveTag));
   }
   SaveScope(const SaveScope&) = delete;
   SaveScope& operator=(const SaveScope&) = delete;
   /// Restores both.
   ~SaveScope()
   {
      *reinterpret_cast<volatile uint16_t*>(kWaitcntAddress) = waitcnt;
      REG_IME = ime;
   }
};

/// 8 wait states for the save chip while a scope lasts, interrupts left as they are (a read in read mode, C15a).
struct ReadScope
{
   const uint16_t waitcnt;   ///< WAITCNT before.

   /// Sets the wait states.
   ReadScope() : waitcnt(*reinterpret_cast<volatile uint16_t*>(kWaitcntAddress))
   {
      *reinterpret_cast<volatile uint16_t*>(kWaitcntAddress) =
         static_cast<uint16_t>((waitcnt & ~kSramWaitMask) | kSramWait8);
      // the tag must stay in the ROM: this use keeps the linker from dropping it
      asm volatile("" : : "r"(kSaveTag));
   }
   ReadScope(const ReadScope&) = delete;              ///< Not copyable.
   ReadScope& operator=(const ReadScope&) = delete;   ///< Not copyable.
   /// Restores them.
   ~ReadScope()
   {
      *reinterpret_cast<volatile uint16_t*>(kWaitcntAddress) = waitcnt;
   }
};

/// Whether a range lies inside the chip.
/// @param offset  first byte
/// @param size    bytes
/// @return true if it does
bool inside(uint32_t offset, uint32_t size)
{
   return offset <= kSaveBytes && size <= kSaveBytes - offset;
}

/// Copies chip bytes (from IWRAM: the cartridge bus serves the chip while it runs).
/// @param offset  first byte
/// @param dst     destination
/// @param size    bytes
CAPSULE_IWRAM_CODE void copy_out(uint32_t offset, uint8_t* dst, uint32_t size)
{
   for (uint32_t i = 0; i < size; ++i)
   {
      dst[i] = *reinterpret_cast<volatile uint8_t*>(kSaveBase + offset + i);
   }
}

#if !defined(CAPSULE_SAVE_SRAM)
/// How the chip programs.
enum class Kind : uint8_t
{
   Unknown,   ///< Not identified yet.
   Sector,    ///< Sector erase, byte program (Macronix, Panasonic, Sanyo, SST).
   Atmel,     ///< Page program (a page write erases it).
   None,      ///< An id this backend does not know: erases and programs fail.
};

/// The chip's kind, identified on first use.
Kind g_kind = Kind::Unknown;
/// Whether the chip has two 64 KB banks (bank 0 is selected at every begin()).
bool g_banked = false;
/// Atmel: the erased range not programmed yet, [g_fresh_from, g_fresh_to) (erase() marks it; program() writes
/// each page in it with 0xFF where it gives no byte, and moves g_fresh_from past the pages it wrote).
uint32_t g_fresh_from = 0;
uint32_t g_fresh_to = 0;   ///< End of that range.

/// Sends a flash command (the unlock sequence, then the command byte).
/// @param command  the byte
void command(uint8_t command)
{
   chip(kCommandA) = kUnlockA;
   chip(kCommandB) = kUnlockB;
   chip(kCommandA) = command;
}

/// Waits a number of scanlines (VCOUNT steps; from IWRAM: it runs while the chip is in id mode).
/// @param lines  scanlines
CAPSULE_IWRAM_CODE void wait_lines(uint32_t lines)
{
   const volatile uint16_t* vcount = reinterpret_cast<const volatile uint16_t*>(kVcountAddress);
   uint16_t last = *vcount;
   for (uint32_t seen = 0; seen < lines;)
   {
      const uint16_t now = *vcount;
      if (now != last)
      {
         last = now;
         ++seen;
      }
   }
}

/// Reads the chip id in id mode (from IWRAM: the bus cannot fetch ROM code while the chip answers ids).
/// @return manufacturer << 8 | device
CAPSULE_IWRAM_CODE uint16_t read_id()
{
   volatile uint8_t* base = reinterpret_cast<volatile uint8_t*>(kSaveBase);
   base[kCommandA] = kUnlockA;
   base[kCommandB] = kUnlockB;
   base[kCommandA] = kIdEnter;
   wait_lines(kIdWaitLines);   // the datasheets' id entry time (about 20 ms on the slowest chips)
   const uint16_t id = static_cast<uint16_t>(base[0] << kManufacturerShift | base[1]);
   base[kCommandA] = kUnlockA;
   base[kCommandB] = kUnlockB;
   base[kCommandA] = kIdExit;
   base[0] = kIdExit;   // some chips leave id mode on a bare F0 only
   wait_lines(kIdWaitLines);   // and their exit time, before the first read in read mode
   return id;
}

/// Selects bank 0 of a 128 KB chip (the bank command, then the bank number at 0x0000).
void select_bank_zero()
{
   const SaveScope scope;
   command(kBankSelect);
   chip(0) = kBankZero;
}

/// The chip's kind (identified once).
/// @return it
Kind kind()
{
   if (g_kind == Kind::Unknown)
   {
      uint16_t id = 0;
      {
         const SaveScope scope;
         id = read_id();
      }
      switch (id)
      {
      case kMacronix1M:
      case kSanyo:
         g_banked = true;   // 128 KB: two banks
         g_kind = Kind::Sector;
         break;
      case kMacronix512:
      case kPanasonic:
      case kSst:
         g_kind = Kind::Sector;
         break;
      case kAtmel:
         g_kind = Kind::Atmel;
         break;
      default:
         g_kind = Kind::None;
         break;
      }
   }
   return g_kind;
}

/// One status poll: whether a flash byte reads a value (from IWRAM, interrupts masked for the poll only).
/// @param offset  byte offset
/// @param value   the value it reads when done
/// @return true if it does
CAPSULE_IWRAM_CODE bool poll_once(uint32_t offset, uint8_t value)
{
   return *reinterpret_cast<volatile uint8_t*>(kSaveBase + offset) == value;
}

/// Waits until a flash byte reads a value (the chip finished), masking interrupts per poll.
/// @param offset  byte offset
/// @param value   the value it reads when done
/// @return false on a timeout (the chip is reset to reading)
bool wait_for(uint32_t offset, uint8_t value)
{
   for (uint32_t i = 0; i < kPollLimit; ++i)
   {
      const SaveScope scope;
      if (poll_once(offset, value))
      {
         return true;
      }
   }
   const SaveScope scope;
   command(kIdExit);   // the datasheets' reset after a timeout: the unlocked F0 sequence
   return false;
}

/// Whether a whole range reads erased.
/// @param offset  first byte
/// @param size    bytes
/// @return true if every byte is 0xFF
bool erased(uint32_t offset, uint32_t size)
{
   uint8_t buf[kAtmelPage];
   for (uint32_t done = 0; done < size; done += kAtmelPage)
   {
      const uint32_t n = size - done < kAtmelPage ? size - done : kAtmelPage;
      {
         const ReadScope scope;
         copy_out(offset + done, buf, n);
      }
      for (uint32_t i = 0; i < n; ++i)
      {
         if (buf[i] != kErased)
         {
            return false;
         }
      }
   }
   return true;
}

/// Writes one Atmel page whole (128 bytes from @p page).
/// @param at    the page's offset (a multiple of kAtmelPage)
/// @param page  its bytes
/// @return false on a timeout or a byte that did not take
bool atmel_page(uint32_t at, const uint8_t* page)
{
   {
      const SaveScope scope;
      command(kProgramByte);
      for (uint32_t i = 0; i < kAtmelPage; ++i)
      {
         chip(at + i) = page[i];
      }
   }
   return wait_for(at + kAtmelPage - 1, page[kAtmelPage - 1]);
}

/// Programs bytes on an Atmel chip, one page write per page touched: the page's other bytes are 0xFF while it lies
/// in the marked erased range (g_fresh_from / g_fresh_to), else what it holds.
/// @param offset  first byte
/// @param src     bytes
/// @param size    count
/// @return false on a timeout or a byte that did not take
bool atmel_program(uint32_t offset, const uint8_t* src, uint32_t size)
{
   for (uint32_t done = 0; done < size;)
   {
      const uint32_t at = (offset + done) / kAtmelPage * kAtmelPage;
      uint8_t page[kAtmelPage];
      if (at >= g_fresh_from && at + kAtmelPage <= g_fresh_to)
      {
         for (uint8_t& b : page)
         {
            b = kErased;
         }
      }
      else
      {
         const ReadScope scope;
         copy_out(at, page, kAtmelPage);
      }
      uint32_t i = offset + done - at;
      for (; i < kAtmelPage && done < size; ++i, ++done)
      {
         page[i] = src[done];
      }
      if (!atmel_page(at, page))
      {
         return false;
      }
      if (at >= g_fresh_from && at < g_fresh_to)
      {
         g_fresh_from = at + kAtmelPage;   // written: a later program() of this page merges with it
      }
   }
   return true;
}
#endif

} // namespace

bool begin(bool)
{
#if !defined(CAPSULE_SAVE_SRAM)
   if (kind() != Kind::None && g_banked)
   {
      select_bank_zero();   // before any read: the first 64 KB, whatever bank was left selected
   }
#endif
   return true;
}

void end()
{
}

uint32_t capacity()
{
   return kSaveBytes;
}

uint32_t erase_unit()
{
#if defined(CAPSULE_SAVE_SRAM)
   return 1;
#else
   return kSectorBytes;
#endif
}

bool read(uint32_t offset, uint8_t* dst, uint32_t size)
{
   if (!inside(offset, size))
   {
      return false;
   }
   const ReadScope scope;   // interrupts stay on (see the file comment)
   copy_out(offset, dst, size);
   return true;
}

bool erase(uint32_t offset, uint32_t size)
{
   if (!inside(offset, size))
   {
      return false;
   }
#if defined(CAPSULE_SAVE_SRAM)
   const SaveScope scope;
   for (uint32_t i = 0; i < size; ++i)
   {
      chip(offset + i) = kErased;
   }
   return true;
#else
   const Kind k = kind();
   if (k == Kind::Atmel)
   {
      // no erase command: the pages are written once, when program() reaches them (see the file comment)
      g_fresh_from = offset - offset % kSectorBytes;   // the units covering the range, as on the other chips
      g_fresh_to = (offset + size + kSectorBytes - 1) / kSectorBytes * kSectorBytes;
      return true;
   }
   // every sector the range touches
   for (uint32_t at = offset - offset % kSectorBytes; at < offset + size; at += kSectorBytes)
   {
      if (k == Kind::Sector)
      {
         {
            const SaveScope scope;
            command(kEraseSetup);
            chip(kCommandA) = kUnlockA;
            chip(kCommandB) = kUnlockB;
            chip(at) = kEraseSector;
         }
         if (!wait_for(at, kErased))
         {
            return false;
         }
      }
      else
      {
         return false;   // an unknown chip: refused (MediumError), see the file comment
      }
      if (!erased(at, kSectorBytes))
      {
         return false;   // the whole sector, not only the byte polled
      }
   }
   return true;
#endif
}

bool program(uint32_t offset, const uint8_t* src, uint32_t size)
{
   if (!inside(offset, size))
   {
      return false;
   }
#if defined(CAPSULE_SAVE_SRAM)
   const SaveScope scope;
   for (uint32_t i = 0; i < size; ++i)
   {
      chip(offset + i) = src[i];
   }
   return true;
#else
   const Kind k = kind();
   if (k == Kind::Atmel)
   {
      return atmel_program(offset, src, size);
   }
   if (k != Kind::Sector)
   {
      return false;
   }
   for (uint32_t i = 0; i < size; ++i)
   {
      if (src[i] == kErased)
      {
         continue;   // an erased byte already reads 0xFF
      }
      {
         const SaveScope scope;
         command(kProgramByte);
         chip(offset + i) = src[i];
      }
      if (!wait_for(offset + i, src[i]))
      {
         return false;
      }
   }
   return true;
#endif
}

bool commit()
{
   return true;   // the chip is written through
}

} // namespace capsule::hal::save
