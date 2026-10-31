/// @file
/// Input mapping and the shoulder intent filter (B-11; see capsule/input/input.h).
#include "capsule/input/input.h"

namespace capsule::input
{

namespace
{

using hal::input::key_bit;
using hal::input::KEY_L;
using hal::input::KEY_R;
using hal::input::KEY_SELECT;
using hal::input::KEY_START;

constexpr KeyBits kShoulders = key_bit(KEY_L) | key_bit(KEY_R);           ///< L and R.
constexpr KeyBits kChordKeys = key_bit(KEY_START) | key_bit(KEY_SELECT);   ///< Keys of the sleep/reset chords.

/// @name Chord modes (ShoulderFilter::mode)
///@{
constexpr uint8_t kChordNone = 0;    ///< No chord held.
constexpr uint8_t kChordSleep = 1;   ///< L + R + Start: sleep.
constexpr uint8_t kChordReset = 2;   ///< L + R + Start + Select: soft reset.
///@}

/// Highest chord hold count (the timer saturates there).
constexpr uint8_t kTimerMax = 255;

/// Which wheel action a shoulder opens on this target (the one bound to it).
/// @param map       the target's input map
/// @param shoulder  0 = L, 1 = R
/// @return ACTION_WHEEL_1 or ACTION_WHEEL_2
Action wheel_for(const InputMap& map, uint8_t shoulder)
{
   const KeyBits key = shoulder == 0 ? key_bit(KEY_L) : key_bit(KEY_R);
   return (map.bindings[ACTION_WHEEL_1] & key) ? ACTION_WHEEL_1 : ACTION_WHEEL_2;
}

} // namespace

InputEvent update_shoulders(ShoulderFilter& f, KeyBits held, KeyBits pressed, const InputMap& c)
{
   const bool l = held & key_bit(KEY_L);
   const bool r = held & key_bit(KEY_R);
   const bool st = held & key_bit(KEY_START);
   switch (f.state)
   {
   case ShoulderState::Idle:
      if (pressed & kShoulders)
      {
         f.first = (pressed & key_bit(KEY_L)) ? 0 : 1;
         f.timer = 0;
         f.mode = kChordNone;
         f.state = ShoulderState::Pending;
         if (l && r)
         {
            f.state = ShoulderState::Chord;
         }   // both in the same frame
      }
      break;
   case ShoulderState::Pending:
   {
      const bool first_held = f.first == 0 ? l : r;
      if (l && r)
      {
         f.timer = 0;
         f.mode = kChordNone;
         f.state = ShoulderState::Chord;
      }
      else if (!first_held)
      {
         f.state = ShoulderState::Idle;   // released before the window: nothing
      }
      else if (++f.timer >= c.chord_window)
      {
         f.state = ShoulderState::Wheel;
         return InputEvent::OpenWheel;
      }
      break;
   }
   case ShoulderState::Wheel:
      if (!l && !r)
      {
         f.state = ShoulderState::Idle;
         return InputEvent::CloseWheel;
      }
      break;
   case ShoulderState::Chord:
   {
      if (!l && !r)
      {
         f.state = ShoulderState::Idle;
         break;
      }   // fully released: nothing happened
      const bool sel = held & key_bit(KEY_SELECT);
      const uint8_t mode = (l && r && st) ? (sel ? kChordReset : kChordSleep) : kChordNone;
      // timer = frames this chord has been held, so an event fires after exactly
      // *_hold_frames frames. Changing chord restarts the count; it saturates (P-18).
      if (mode == kChordNone)
      {
         f.timer = 0;
      }
      else if (mode != f.mode)
      {
         f.timer = 1;
      }
      else if (f.timer < kTimerMax)
      {
         ++f.timer;
      }
      f.mode = mode;
      if (mode == kChordReset && f.timer >= c.reset_hold_frames)
      {
         f.state = ShoulderState::Idle;
         return InputEvent::SoftReset;
      }
      if (mode == kChordSleep && f.timer >= c.sleep_hold_frames)
      {
         f.state = ShoulderState::Idle;
         return InputEvent::Sleep;
      }
      break;
   }
   default:
      break;
   }
   return InputEvent::None;
}

void update(InputState& s, KeyBits physical, const InputMap& map)
{
   s.keys_pressed = static_cast<KeyBits>(physical & ~s.keys);
   s.keys = physical;

   // Rule 5: chord keys held with a shoulder stay ignored until released.
   s.suppressed = static_cast<KeyBits>(s.suppressed & physical);
   if (physical & kShoulders)
   {
      s.suppressed = static_cast<KeyBits>(s.suppressed | (physical & kChordKeys));
   }
   const KeyBits effective = static_cast<KeyBits>(physical & ~s.suppressed);

   ActionMask held = 0;
   for (uint8_t a = 0; a < NUM_ACTIONS; ++a)
   {
      if (a == ACTION_WHEEL_1 || a == ACTION_WHEEL_2)
      {
         continue;
      }   // filter-driven
      if (effective & map.bindings[a])
      {
         held = static_cast<ActionMask>(held | action_bit(static_cast<Action>(a)));
      }
   }

   s.event = update_shoulders(s.shoulders, physical, s.keys_pressed, map);
   if (s.event == InputEvent::OpenWheel)
   {
      s.wheel = wheel_for(map, s.shoulders.first);
   }
   if (s.shoulders.state == ShoulderState::Wheel)
   {
      held = static_cast<ActionMask>(held | action_bit(s.wheel));
   }

   s.pressed = static_cast<ActionMask>(held & ~s.held);
   s.held = held;
}

} // namespace capsule::input
