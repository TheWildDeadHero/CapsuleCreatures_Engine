/// @file
/// Physical buttons as the HAL reports them (B-11).
///
/// The HAL reports physical button bits; logical actions are mapped from these
/// by the compiled input map (capsule/input/input.h). Bits 0-9 follow the GBA
/// KEYINPUT register; X and Y exist only on SNES-layout and Dreamcast targets,
/// so the GBA HAL never reports them.
#pragma once

#include <cstdint>

/// Physical buttons.
namespace capsule::hal::input
{

using KeyBits = uint16_t;   ///< Set of Key bits.

/// Physical buttons, as bit positions in KeyBits.
enum Key : uint8_t
{
   KEY_A,        ///< A
   KEY_B,        ///< B
   KEY_SELECT,   ///< Select (absent on Dreamcast)
   KEY_START,    ///< Start
   KEY_RIGHT,    ///< D-pad right
   KEY_LEFT,     ///< D-pad left
   KEY_UP,       ///< D-pad up
   KEY_DOWN,     ///< D-pad down
   KEY_R,        ///< R shoulder (Dreamcast: right trigger)
   KEY_L,        ///< L shoulder (Dreamcast: left trigger)
   KEY_X,        ///< X (SNES layout and Dreamcast only)
   KEY_Y,        ///< Y (SNES layout and Dreamcast only)
   NUM_KEYS      ///< Number of keys.
};
static_assert(NUM_KEYS <= sizeof(KeyBits) * 8);

/// Bit for one key.
/// @param k  the key
/// @return a KeyBits value with only @p k set
constexpr KeyBits key_bit(Key k)
{
   return static_cast<KeyBits>(1u << k);
}

/// Buttons held during the last system::update().
/// @return held keys, 1 = held
KeyBits held();

} // namespace capsule::hal::input
