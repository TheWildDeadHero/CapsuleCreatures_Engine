/// @file
/// The screen arena's storage (see src/screen/arena_storage.h). A runtime source (CAPSULE_RUNTIME_SOURCES): each game
/// compiles it with its own build settings, so CAPSULE_SCREEN_ARENA_BYTES is the game's screen_arena_bytes
/// (tools/build_settings.py); a build without settings (the headless library, the engine's tests) gets the default.
#include "arena_storage.h"

#include "capsule/core/memory.h"
#include "capsule/screen/arena.h"

#ifndef CAPSULE_SCREEN_ARENA_BYTES
/// The arena's size when no build setting gives one (tools/build_settings.py's default).
#define CAPSULE_SCREEN_ARENA_BYTES 32768
#endif

namespace capsule::screen::arena::detail
{

namespace
{
constexpr uint32_t kBytes = CAPSULE_SCREEN_ARENA_BYTES;   ///< The arena's size.
static_assert(kBytes % kAlign == 0, "screen_arena_bytes is a multiple of the arena's alignment");
}   // namespace

alignas(kStorageAlign) CAPSULE_EWRAM_BSS uint8_t g_storage[kBytes];   ///< The arena's bytes.
extern const uint32_t g_storage_bytes = kBytes;                ///< Its size.

}   // namespace capsule::screen::arena::detail
