/// @file
/// Save medium (D-26 decision 1, package G2): the bytes the engine's save image (capsule/save/save.h) is kept in.
///
/// Backends:
///   - GBA (`src/hal/gba/save_gba.cpp`): the cartridge's 64 KB flash (sector erase, byte program; Macronix /
///     Panasonic / SST command set; Atmel page writes), or with CAPSULE_SAVE_SRAM defined its 32 KB SRAM;
///   - desktop (`src/hal/sdl2/save_file.cpp`, compiled per game): the file `GAME.sav` relative to the working
///     folder, or in the folder the environment variable CAPSULE_SAVE_DIR names (a platform's user folder; read
///     at every begin(), C7);
///   - headless, Dreamcast and tests (`src/hal/common/save_memory.cpp`, the default a game's backend replaces at
///     link time): RAM only.
///
/// An erased byte reads 0xFF on every medium. program() only writes bytes erase() erased (flash cannot set bits).
/// Exception (C7): an Atmel flash chip has no erase; its erase() only marks the range, and the pages are written
/// once when program() reaches them (bytes of an erased page program() does not give are written 0xFF; erased bytes
/// past the last page programmed keep their old values until a later program()).
///
/// One save write or read is a session (C6): begin() before its reads, erases and programs and end() after them
/// (Session is the scope), so a file medium opens its file once per operation instead of once per chunk.
#pragma once

#include <cstdint>

namespace capsule::hal::save
{

/// Starts an operation (the file medium opens its file; the others do nothing).
/// @param writing  true: erases and programs follow (a missing file is created)
/// @return false when the medium cannot be used (a file that exists but does not open)
bool begin(bool writing);

/// Ends the operation begin() started.
void end();

/// One operation's scope: begin() on entry, end() on exit.
struct Session
{
   const bool ok;   ///< begin()'s answer.

   /// Begins.
   /// @param writing  begin()'s argument
   explicit Session(bool writing)
      : ok(begin(writing))
   {
   }
   Session(const Session&) = delete;
   Session& operator=(const Session&) = delete;
   /// Ends.
   ~Session()
   {
      end();
   }
};

/// Bytes the medium holds (0: no medium).
/// @return capacity
uint32_t capacity();

/// Bytes one erase() unit covers (a flash sector; 1 where any byte can be rewritten).
/// @return the unit
uint32_t erase_unit();

/// Reads bytes.
/// @param offset  first byte
/// @param dst     destination
/// @param size    bytes
/// @return false past the capacity or on a medium error
bool read(uint32_t offset, uint8_t* dst, uint32_t size);

/// Erases the units covering a range (their bytes read 0xFF).
/// @param offset  first byte (a multiple of erase_unit())
/// @param size    bytes (rounded up to whole units)
/// @return false past the capacity or on a medium error
bool erase(uint32_t offset, uint32_t size);

/// Writes bytes into an erased range.
/// @param offset  first byte
/// @param src     source
/// @param size    bytes
/// @return false past the capacity or when a byte did not take
bool program(uint32_t offset, const uint8_t* src, uint32_t size);

/// Makes what program() wrote durable before a write reports success (C7; desktop: the file flushed and synced to
/// its disk; the other media write through and answer true).
/// @return false on a medium error
bool commit();

} // namespace capsule::hal::save
