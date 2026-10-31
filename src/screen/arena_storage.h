/// @file
/// The screen arena's storage (capsule/screen/arena.h), defined per game by src/screen/arena_storage.cpp (a runtime
/// source compiled with the game's build settings: screen_arena_bytes).
#pragma once

#include <cstdint>

/// The arena's storage (engine internal).
namespace capsule::screen::arena::detail
{

extern uint8_t g_storage[];               ///< The arena's bytes (EWRAM on the GBA; word-aligned).
extern const uint32_t g_storage_bytes;    ///< Its size (the build setting screen_arena_bytes).

}   // namespace capsule::screen::arena::detail
