/// @file
/// Logical input actions (B-11).
///
/// Actions are mapped from physical buttons through a per-target table
/// compiled from assets/input/*.json (P-1). The game reads actions, never
/// physical buttons; which button means what on a target is data.
#pragma once

#include "capsule/core/pod.h"
#include "capsule/core/types.h"
#include "capsule/hal/input.h"

#include <cstdint>

/// Logical input: actions, the per-target map and the shoulder filter.
namespace capsule::input
{

using hal::input::KeyBits;

/// Logical actions. Order is locked against tools/input_compiler.py by the
/// generated tables.
enum Action : uint8_t
{
   ACTION_UP,            ///< Move / cursor up.
   ACTION_DOWN,          ///< Move / cursor down.
   ACTION_LEFT,          ///< Move / cursor left.
   ACTION_RIGHT,         ///< Move / cursor right.
   ACTION_ACCEPT,        ///< Confirm, interact.
   ACTION_BACK,          ///< Cancel; also run / walk.
   ACTION_MAIN_MENU,     ///< Open the main menu.
   ACTION_DEVICE_MENU,   ///< Open the device menu.
   ACTION_TOGGLE_RUN,    ///< Toggle running.
   ACTION_HELP,          ///< Help.
   ACTION_WHEEL_1,       ///< First shortcut wheel (from the shoulder filter).
   ACTION_WHEEL_2,       ///< Second shortcut wheel (from the shoulder filter).
   NUM_ACTIONS           ///< Number of actions.
};
using ActionMask = uint16_t;   ///< Set of Action bits.
static_assert(fits_in<ActionMask>(NUM_ACTIONS));

/// Bit for one action.
/// @param a  the action
/// @return an ActionMask with only @p a set
constexpr ActionMask action_bit(Action a)
{
   return flag_mask<ActionMask>(a);
}

/// Which button layout an input map was compiled for.
enum InputTarget : uint8_t
{
   INPUT_TARGET_GBA,           ///< GBA: no X / Y.
   INPUT_TARGET_SNES_LAYOUT,   ///< Desktop keyboard / pads, SNES layout.
   INPUT_TARGET_DREAMCAST,     ///< Dreamcast pad: no Select.
   NUM_INPUT_TARGETS           ///< Number of targets.
};

/// Compiled per target. A binding of 0 means the target has no button for the
/// action; it is then offered as a main-menu entry instead.
struct InputMap
{
   KeyBits bindings[NUM_ACTIONS];   ///< Buttons bound to each action.
   KeyBits wake_keys;               ///< Sleep wake combo (C-05a); all must be held.
   uint8_t chord_window;            ///< Frames before a lone shoulder opens its wheel.
   uint8_t sleep_hold_frames;       ///< Frames L+R+Start must be held to sleep.
   uint8_t reset_hold_frames;       ///< Frames L+R+Start+Select must be held to reset.
   uint8_t target;                  ///< InputTarget this map was compiled for.
};
CAPSULE_POD(InputMap, 30);

/// The active target's map, generated at build time.
extern const InputMap kInputMap;

/// Whether an action has no button on this target and needs a menu entry.
/// @param map  the input map
/// @param a    the action
/// @return true if nothing is bound to @p a
constexpr bool needs_menu_entry(const InputMap& map, Action a)
{
   return map.bindings[a] == 0;
}

/// @name Shoulder intent filter (B-11)
///@{

/// States of the shoulder filter.
enum class ShoulderState : uint8_t
{
   Idle,                 ///< No shoulder held.
   Pending,              ///< One shoulder held, within the chord window.
   Wheel,                ///< One shoulder held past the window: its wheel is open.
   Chord,                ///< Both shoulders held (sleep / reset chords).
   NUM_SHOULDER_STATES   ///< Number of states.
};

/// Shoulder filter state, 4 B of RAM.
struct ShoulderFilter
{
   ShoulderState state;   ///< Current state.
   uint8_t timer;         ///< Frames in Pending, or frames the current chord has been held.
   uint8_t first;         ///< Shoulder pressed first: 0 = L, 1 = R.
   uint8_t mode;          ///< Chord being held: 0 = none, 1 = sleep, 2 = reset.
};
CAPSULE_POD(ShoulderFilter, 4);

/// What the shoulder filter reports for a frame.
enum class InputEvent : uint8_t
{
   None,              ///< Nothing this frame.
   OpenWheel,         ///< A wheel opened (InputState::wheel says which).
   CloseWheel,        ///< The open wheel closed.
   Sleep,             ///< L+R+Start held long enough.
   SoftReset,         ///< L+R+Start+Select held long enough.
   NUM_INPUT_EVENTS   ///< Number of events.
};
///@}

/// Per-frame input state, updated by update().
struct InputState
{
   KeyBits keys;           ///< Physical, held this frame.
   KeyBits keys_pressed;   ///< Physical, newly pressed this frame.
   KeyBits suppressed;     ///< Start/Select held during a shoulder, ignored until released.
   ActionMask held;           ///< Actions held this frame.
   ActionMask pressed;        ///< Actions newly held this frame.
   InputEvent event;          ///< This frame's shoulder event; None most frames.
   Action wheel;          ///< Wheel action for OpenWheel / CloseWheel.
   ShoulderFilter shoulders;      ///< Shoulder filter state.
};

/// Feeds one frame of physical buttons through the map.
/// - Directional and button actions are held while any bound button is held.
/// - Wheel actions come only from the shoulder filter (OpenWheel / CloseWheel).
/// - Start or Select held together with a shoulder is ignored until that
///   button is released, so the sleep and reset chords never trigger the
///   actions bound to them, even if the shoulders are let go first (rule 5).
/// @param state     state carried between frames; updated
/// @param physical  buttons held this frame (hal::input::held())
/// @param map       the target's input map
void update(InputState& state, KeyBits physical, const InputMap& map);

/// The shoulder filter on its own, exposed for testing.
/// @param filter   filter state; updated
/// @param held     buttons held this frame
/// @param pressed  buttons newly pressed this frame
/// @param map      the target's input map (chord window, hold times)
/// @return the frame's event; for OpenWheel, filter.first says which shoulder
///         (0 = L, 1 = R)
InputEvent update_shoulders(ShoulderFilter& filter, KeyBits held, KeyBits pressed, const InputMap& map);

} // namespace capsule::input
