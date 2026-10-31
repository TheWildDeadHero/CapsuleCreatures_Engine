/// @file
/// N3 (D-32, engine B2 core): the keys menus read each frame: held, newly pressed and pressed-or-repeated. The
/// repeat is one counter for the whole pad, as handheld games keep it: while the same keys stay held it counts down
/// from Repeat::start frames, then every Repeat::every frames; when it runs out the held keys count as repeated that
/// frame. Any change of the held keys (or none held) starts it again. A game keeps one KeyState for the whole
/// program and reads it once a frame (read()), so a key held before a menu opened repeats on the same frames as it
/// would anywhere else.
#pragma once

#include "capsule/hal/input.h"

#include <cstdint>

/// Menus, lists and boxes drawn in text windows (N3).
namespace capsule::ui
{

/// How the repeat counter runs (a game's data).
struct Repeat
{
   uint8_t start = 0;   ///< Frames the same keys are held before the first repeat (0: no repeat).
   uint8_t every = 0;   ///< Frames between repeats after that.
};

/// The pad's state between frames (plain data; one per program).
struct KeyState
{
   hal::input::KeyBits held = 0;   ///< The keys held last frame.
   uint8_t counter = 0;            ///< Frames left of the repeat counter.
};

/// One frame's keys.
struct Keys
{
   hal::input::KeyBits held = 0;       ///< Held this frame.
   hal::input::KeyBits pressed = 0;    ///< Went down this frame.
   hal::input::KeyBits repeated = 0;   ///< Went down this frame, or held and repeated this frame.
};

/// Reads one frame's keys and advances the repeat counter.
/// @param s       the program's state (updated)
/// @param timing  the repeat timing
/// @param held    the keys held this frame
/// @return the frame's keys
Keys read(KeyState& s, const Repeat& timing, hal::input::KeyBits held);

/// Whether a key went down this frame.
/// @param k    the frame's keys
/// @param key  the key
/// @return true on its press
constexpr bool pressed(const Keys& k, hal::input::Key key)
{
   return (k.pressed & hal::input::key_bit(key)) != 0;
}

/// Whether a key went down or repeated this frame.
/// @param k    the frame's keys
/// @param key  the key
/// @return true on its press or a repeat
constexpr bool repeated(const Keys& k, hal::input::Key key)
{
   return (k.repeated & hal::input::key_bit(key)) != 0;
}

/// Whether a key is held.
/// @param k    the frame's keys
/// @param key  the key
/// @return true while it is
constexpr bool held(const Keys& k, hal::input::Key key)
{
   return (k.held & hal::input::key_bit(key)) != 0;
}

}   // namespace capsule::ui
