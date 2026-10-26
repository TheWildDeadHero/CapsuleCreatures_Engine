/// @file
/// Save medium in a file (desktop; see capsule/hal/save.h). Compiled per game (CMakeLists.txt
/// CAPSULE_RUNTIME_SOURCES) with CAPSULE_GAME_NAME, the project's name: the file is `<name>.sav`, relative to the
/// working folder, or inside the folder the environment variable CAPSULE_SAVE_DIR names. Bytes past the file's end
/// read as erased (0xFF); writing grows it. A missing file (ENOENT) is an erased medium; any other error opening or
/// reading it is a medium error. The file stays open for one save operation (begin() / end(), C6). Host code:
/// stdio, no engine heap.
///
/// C7 (final review): the path is built at every open, so a changed CAPSULE_SAVE_DIR takes effect at the next
/// operation; a path longer than kPathChars is a medium error (never a truncated name); commit() flushes the file
/// and syncs it to its disk (fsync, Windows _commit), and the folder too when the operation created the file.
#include "capsule/hal/save.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef CAPSULE_GAME_NAME
/// The game's name (capsule_game() passes the project's name; a build without one uses this).
#define CAPSULE_GAME_NAME "capsule"
#endif

namespace capsule::hal::save
{

namespace
{

/// Bytes the file may hold (build_settings PLATFORM_LIMITS max_save_bytes for a non-GBA platform).
constexpr uint32_t kFileBytes = 0x40000;
/// An erased byte.
constexpr uint8_t kErased = 0xFF;
/// Longest path kept (folder, separator, name, suffix).
constexpr int kPathChars = 1024;
/// Bytes moved per stdio call while erasing.
constexpr uint32_t kEraseChunk = 256;

/// The file's path, built at every open.
char g_path[kPathChars] = {};

/// The folder of the save file (CAPSULE_SAVE_DIR, or nullptr: the working folder).
/// @return it
const char* folder()
{
   const char* dir = std::getenv("CAPSULE_SAVE_DIR");
   return dir != nullptr && dir[0] != '\0' ? dir : nullptr;
}

/// The path of the save file, built from the environment as it is now.
/// @return it (CAPSULE_SAVE_DIR/NAME.sav or NAME.sav, NAME the game's), or nullptr when it is longer than kPathChars
const char* path()
{
   const char* dir = folder();
   const int n = dir != nullptr ? std::snprintf(g_path, sizeof(g_path), "%s/%s.sav", dir, CAPSULE_GAME_NAME)
                                : std::snprintf(g_path, sizeof(g_path), "%s.sav", CAPSULE_GAME_NAME);
   if (n < 0 || n >= kPathChars)
   {
      g_path[0] = '\0';
      return nullptr;   // a truncated name would be another file
   }
   return g_path;
}

/// The file of the current session (begin() / end()), or nullptr outside one.
std::FILE* g_file = nullptr;
/// Whether the session's file does not exist yet (reading: every byte is erased).
bool g_missing = false;
/// Sessions begun and not ended (a nested begin() reuses the open file).
int g_depth = 0;
/// Whether this session created the file (commit() then syncs its folder too).
bool g_created = false;

/// Opens the save file (C6, review4 finding 16): a file that is not there is created only for writing, and only
/// when the error says it is missing (ENOENT); any other error is a medium error, so an existing save is never
/// truncated by "w+b".
/// @param writing  true: for update
/// @param missing  set when the file does not exist (reading)
/// @return the file, or nullptr (missing for reading, or an error; a path too long is an error)
std::FILE* open_file(bool writing, bool& missing)
{
   missing = false;
   const char* name = path();
   if (name == nullptr)
   {
      return nullptr;
   }
   errno = 0;
   std::FILE* f = std::fopen(name, writing ? "r+b" : "rb");
   if (f != nullptr)
   {
      return f;
   }
   if (errno != ENOENT)
   {
      return nullptr;   // there but not usable (permissions, a folder, ...): never replaced
   }
   missing = true;
   if (!writing)
   {
      return nullptr;
   }
   f = std::fopen(name, "w+b");
   g_created = g_created || f != nullptr;
   return f;
}

/// Syncs a file's bytes to its disk.
/// @param f  the file (flushed before)
/// @return false on an error
bool sync_file(std::FILE* f)
{
#if defined(_WIN32)
   return _commit(_fileno(f)) == 0;
#else
   return fsync(fileno(f)) == 0;
#endif
}

/// Syncs the save file's folder (its new directory entry; nothing to do on Windows).
/// @return false on an error
bool sync_folder()
{
#if defined(_WIN32)
   return true;
#else
   const char* dir = folder();
   const int fd = open(dir != nullptr ? dir : ".", O_RDONLY);
   if (fd < 0)
   {
      return false;
   }
   const bool ok = fsync(fd) == 0;
   return close(fd) == 0 && ok;
#endif
}

/// The file one call uses: the session's, or one opened for this call alone (a call outside a session).
struct CallFile
{
   std::FILE* f;     ///< The file, or nullptr.
   bool missing;     ///< The file does not exist (reading).
   bool own;         ///< Opened for this call (closed with it).

   /// Takes the session's file or opens one.
   /// @param writing  true: for update
   explicit CallFile(bool writing)
      : f(g_file), missing(g_missing), own(false)
   {
      if (g_depth == 0 || (writing && f == nullptr))
      {
         f = open_file(writing, missing);
         own = true;
      }
   }
   CallFile(const CallFile&) = delete;
   CallFile& operator=(const CallFile&) = delete;
   /// Closes a file opened for this call.
   ~CallFile()
   {
      if (own && f != nullptr)
      {
         std::fclose(f);
      }
   }
   /// Whether the call has its file (or none is needed: reading a missing file).
   /// @param writing  true: for update
   /// @return true if it can go on
   bool usable(bool writing) const
   {
      return f != nullptr || (!writing && missing);
   }
};

/// Whether a range lies inside the medium.
/// @param offset  first byte
/// @param size    bytes
/// @return true if it does
bool inside(uint32_t offset, uint32_t size)
{
   return offset <= kFileBytes && size <= kFileBytes - offset;
}

} // namespace

bool begin(bool writing)
{
   if (g_depth++ > 0)
   {
      return g_file != nullptr || g_missing;
   }
   g_file = open_file(writing, g_missing);
   return g_file != nullptr || (!writing && g_missing);
}

void end()
{
   if (g_depth > 0 && --g_depth == 0)
   {
      if (g_file != nullptr)
      {
         std::fclose(g_file);
      }
      g_file = nullptr;
      g_missing = false;
      g_created = false;
   }
}

uint32_t capacity()
{
   return kFileBytes;
}

uint32_t erase_unit()
{
   return 1;
}

bool read(uint32_t offset, uint8_t* dst, uint32_t size)
{
   if (!inside(offset, size))
   {
      return false;
   }
   for (uint32_t i = 0; i < size; ++i)
   {
      dst[i] = kErased;
   }
   const CallFile file(false);
   if (!file.usable(false))
   {
      return false;   // the file is there but does not open: a medium error, not an erased medium
   }
   if (file.f == nullptr)
   {
      return true;   // no file yet: everything is erased
   }
   if (std::fseek(file.f, static_cast<long>(offset), SEEK_SET) != 0)
   {
      return false;
   }
   // a short read at the end leaves the bytes past it erased; a read error is a medium error
   const std::size_t got = std::fread(dst, 1, size, file.f);
   return got == size || std::ferror(file.f) == 0;
}

bool erase(uint32_t offset, uint32_t size)
{
   if (!inside(offset, size))
   {
      return false;
   }
   uint8_t ones[kEraseChunk];
   for (uint8_t& b : ones)
   {
      b = kErased;
   }
   const CallFile file(true);
   if (!file.usable(true) || std::fseek(file.f, static_cast<long>(offset), SEEK_SET) != 0)
   {
      return false;
   }
   for (uint32_t done = 0; done < size;)
   {
      const uint32_t n = size - done < kEraseChunk ? size - done : kEraseChunk;
      if (std::fwrite(ones, 1, n, file.f) != n)
      {
         return false;
      }
      done += n;
   }
   return std::fflush(file.f) == 0;
}

bool program(uint32_t offset, const uint8_t* src, uint32_t size)
{
   if (!inside(offset, size))
   {
      return false;
   }
   const CallFile file(true);
   if (!file.usable(true) || std::fseek(file.f, static_cast<long>(offset), SEEK_SET) != 0)
   {
      return false;
   }
   return std::fwrite(src, 1, size, file.f) == size && std::fflush(file.f) == 0;
}

bool commit()
{
   const CallFile file(true);
   if (!file.usable(true) || std::fflush(file.f) != 0 || !sync_file(file.f))
   {
      return false;
   }
   const bool created = g_created;
   if (file.own)
   {
      g_created = false;   // a call outside a session: its own file
   }
   return !created || sync_folder();
}

} // namespace capsule::hal::save
