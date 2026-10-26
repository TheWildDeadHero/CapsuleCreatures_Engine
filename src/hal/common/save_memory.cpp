/// @file
/// Save medium in RAM (see capsule/hal/save.h): headless builds, the Dreamcast (no VMU backend yet) and tests.
/// It is the link-time default: a game's own backend (the desktop file, src/hal/sdl2/save_file.cpp) defines every
/// symbol of this file and keeps it out of the link. The bytes live as long as the program.
#include "capsule/hal/save.h"

namespace capsule::hal::save
{

namespace
{

/// Bytes kept (the GBA flash's size, so a test sees the cartridge's budget).
constexpr uint32_t kMemoryBytes = 0x10000;
/// An erased byte.
constexpr uint8_t kErased = 0xFF;

/// The medium; first use erases it (BSS starts at 0).
uint8_t g_bytes[kMemoryBytes];
/// Whether g_bytes was erased once.
bool g_ready = false;

/// Erases the medium on first use.
void ready()
{
   if (!g_ready)
   {
      for (uint8_t& b : g_bytes)
      {
         b = kErased;
      }
      g_ready = true;
   }
}

/// Whether a range lies inside the medium.
/// @param offset  first byte
/// @param size    bytes
/// @return true if it does
bool inside(uint32_t offset, uint32_t size)
{
   return offset <= kMemoryBytes && size <= kMemoryBytes - offset;
}

} // namespace

bool begin(bool)
{
   return true;
}

void end()
{
}

uint32_t capacity()
{
   return kMemoryBytes;
}

uint32_t erase_unit()
{
   return 1;
}

bool read(uint32_t offset, uint8_t* dst, uint32_t size)
{
   ready();
   if (!inside(offset, size))
   {
      return false;
   }
   for (uint32_t i = 0; i < size; ++i)
   {
      dst[i] = g_bytes[offset + i];
   }
   return true;
}

bool erase(uint32_t offset, uint32_t size)
{
   ready();
   if (!inside(offset, size))
   {
      return false;
   }
   for (uint32_t i = 0; i < size; ++i)
   {
      g_bytes[offset + i] = kErased;
   }
   return true;
}

bool program(uint32_t offset, const uint8_t* src, uint32_t size)
{
   ready();
   if (!inside(offset, size))
   {
      return false;
   }
   for (uint32_t i = 0; i < size; ++i)
   {
      g_bytes[offset + i] = src[i];
   }
   return true;
}

bool commit()
{
   return true;   // RAM: nothing to sync
}

} // namespace capsule::hal::save
