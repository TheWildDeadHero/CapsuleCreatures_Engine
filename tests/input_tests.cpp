// Runtime tests for the input layer (B-11), run against both targets' maps as
// compiled from assets/input/default_input.json. Pure logic: no HAL linked.
#include "capsule/input/input.h"

#include <cstdio>

namespace capsule::input
{
extern const InputMap kTestInputMapGba;
extern const InputMap kTestInputMapSnes;
extern const InputMap kTestInputMapDreamcast;
}   // namespace capsule::input

namespace
{

using namespace capsule::input;
using namespace capsule::hal::input;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                                    \
   do                                                                                                                  \
   {                                                                                                                   \
      ++g_checks;                                                                                                      \
      if (!(cond))                                                                                                     \
      {                                                                                                                \
         ++g_failures;                                                                                                 \
         std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)

constexpr KeyBits K(Key k)
{
   return key_bit(k);
}

struct Runner
{
   const InputMap& map;
   InputState state{};
   int events[static_cast<int>(InputEvent::NUM_INPUT_EVENTS)]{};
   void frames(KeyBits keys, int n)
   {
      for (int i = 0; i < n; ++i)
      {
         update(state, keys, map);
         ++events[static_cast<int>(state.event)];
      }
   }
   int count(InputEvent e) const
   {
      return events[static_cast<int>(e)];
   }
   bool held(Action a) const
   {
      return state.held & action_bit(a);
   }
   bool pressed(Action a) const
   {
      return state.pressed & action_bit(a);
   }
};

// --- Mapping per target ------------------------------------------------------

void test_target_ids()
{
   CHECK(kTestInputMapGba.target == INPUT_TARGET_GBA);
   CHECK(kTestInputMapSnes.target == INPUT_TARGET_SNES_LAYOUT);
   CHECK(kTestInputMapDreamcast.target == INPUT_TARGET_DREAMCAST);
}

void test_gba_mapping()
{
   const InputMap& m = kTestInputMapGba;
   struct
   {
      Key key;
      Action action;
   } cases[] = {
      {KEY_A, ACTION_ACCEPT}, {KEY_B, ACTION_BACK},    {KEY_START, ACTION_MAIN_MENU}, {KEY_SELECT, ACTION_DEVICE_MENU},
      {KEY_UP, ACTION_UP},    {KEY_DOWN, ACTION_DOWN}, {KEY_LEFT, ACTION_LEFT},       {KEY_RIGHT, ACTION_RIGHT},
   };
   for (const auto& c : cases)
   {
      Runner r{m};
      r.frames(K(c.key), 1);
      CHECK(r.held(c.action));
      CHECK(r.pressed(c.action));
      CHECK(r.state.held == action_bit(c.action));   // and nothing else
   }
   // GBA has no button for these: they become main-menu entries.
   CHECK(needs_menu_entry(m, ACTION_TOGGLE_RUN));
   CHECK(needs_menu_entry(m, ACTION_HELP));
   CHECK(!needs_menu_entry(m, ACTION_MAIN_MENU));
   CHECK(!needs_menu_entry(m, ACTION_DEVICE_MENU));
   // X/Y are not GBA buttons; even if a HAL reported them, nothing is bound.
   Runner r{m};
   r.frames(K(KEY_X) | K(KEY_Y), 1);
   CHECK(r.state.held == 0);
}

void test_snes_mapping()
{
   const InputMap& m = kTestInputMapSnes;
   struct
   {
      Key key;
      Action action;
   } cases[] = {
      {KEY_A, ACTION_ACCEPT},         {KEY_B, ACTION_BACK},      {KEY_X, ACTION_MAIN_MENU}, {KEY_Y, ACTION_DEVICE_MENU},
      {KEY_START, ACTION_TOGGLE_RUN}, {KEY_SELECT, ACTION_HELP}, {KEY_UP, ACTION_UP},       {KEY_DOWN, ACTION_DOWN},
      {KEY_LEFT, ACTION_LEFT},        {KEY_RIGHT, ACTION_RIGHT},
   };
   for (const auto& c : cases)
   {
      Runner r{m};
      r.frames(K(c.key), 1);
      CHECK(r.state.held == action_bit(c.action));
      CHECK(r.pressed(c.action));
   }
   for (int a = 0; a < NUM_ACTIONS; ++a)
   {
      CHECK(!needs_menu_entry(m, static_cast<Action>(a)));
   }
}

void test_dreamcast_mapping()
{
   const InputMap& m = kTestInputMapDreamcast;
   struct
   {
      Key key;
      Action action;
   } cases[] = {
      {KEY_A, ACTION_ACCEPT},      {KEY_B, ACTION_BACK},           {KEY_X, ACTION_MAIN_MENU},
      {KEY_Y, ACTION_DEVICE_MENU}, {KEY_START, ACTION_TOGGLE_RUN}, {KEY_UP, ACTION_UP},
      {KEY_DOWN, ACTION_DOWN},     {KEY_LEFT, ACTION_LEFT},        {KEY_RIGHT, ACTION_RIGHT},
   };
   for (const auto& c : cases)
   {
      Runner r{m};
      r.frames(K(c.key), 1);
      CHECK(r.state.held == action_bit(c.action));
      CHECK(r.pressed(c.action));
   }
   // No Select on the pad: Help is a main-menu entry, nothing is bound to Select.
   CHECK(needs_menu_entry(m, ACTION_HELP));
   CHECK(!needs_menu_entry(m, ACTION_TOGGLE_RUN));
   for (int a = 0; a < NUM_ACTIONS; ++a)
   {
      CHECK((m.bindings[a] & K(KEY_SELECT)) == 0);
   }
   CHECK(m.wake_keys == K(KEY_START));
}

void test_same_button_differs_by_target()
{
   Runner gba{kTestInputMapGba};
   Runner snes{kTestInputMapSnes};
   gba.frames(K(KEY_START), 1);
   snes.frames(K(KEY_START), 1);
   CHECK(gba.state.held == action_bit(ACTION_MAIN_MENU));
   CHECK(snes.state.held == action_bit(ACTION_TOGGLE_RUN));
}

void test_pressed_is_edge()
{
   Runner r{kTestInputMapGba};
   r.frames(K(KEY_A), 1);
   CHECK(r.pressed(ACTION_ACCEPT));
   r.frames(K(KEY_A), 1);
   CHECK(r.held(ACTION_ACCEPT) && !r.pressed(ACTION_ACCEPT));
   r.frames(0, 1);
   CHECK(!r.held(ACTION_ACCEPT));
   r.frames(K(KEY_A), 1);
   CHECK(r.pressed(ACTION_ACCEPT));
}

// --- Shoulder intent filter (both targets bind wheels the same way) -------------

void test_shoulders(const InputMap& m)
{
   {   // Tap shorter than the window: nothing.
      Runner r{m};
      r.frames(K(KEY_L), m.chord_window - 1);
      r.frames(0, 5);
      CHECK(r.count(InputEvent::OpenWheel) == 0);
      CHECK(r.state.shoulders.state == ShoulderState::Idle);
   }
   {   // Held through the window: L opens WHEEL_2, release closes it.
      Runner r{m};
      r.frames(K(KEY_L), m.chord_window);
      CHECK(r.count(InputEvent::OpenWheel) == 0);   // pressed frame + window-1 frames
      r.frames(K(KEY_L), 1);
      CHECK(r.count(InputEvent::OpenWheel) == 1);
      CHECK(r.state.wheel == ACTION_WHEEL_2);
      CHECK(r.held(ACTION_WHEEL_2) && r.pressed(ACTION_WHEEL_2));
      r.frames(K(KEY_L), 30);
      CHECK(r.count(InputEvent::OpenWheel) == 1 && r.held(ACTION_WHEEL_2));
      r.frames(0, 1);
      CHECK(r.count(InputEvent::CloseWheel) == 1 && !r.held(ACTION_WHEEL_2));
   }
   {   // R opens WHEEL_1.
      Runner r{m};
      r.frames(K(KEY_R), m.chord_window + 1);
      CHECK(r.count(InputEvent::OpenWheel) == 1 && r.state.wheel == ACTION_WHEEL_1);
   }
   {   // Second shoulder inside the window: chord, no wheel.
      Runner r{m};
      r.frames(K(KEY_L), 3);
      r.frames(K(KEY_L) | K(KEY_R), 20);
      CHECK(r.count(InputEvent::OpenWheel) == 0);
      CHECK(r.state.shoulders.state == ShoulderState::Chord);
      r.frames(0, 1);
      CHECK(r.state.shoulders.state == ShoulderState::Idle);
      CHECK(r.count(InputEvent::Sleep) == 0 && r.count(InputEvent::SoftReset) == 0);
   }
   {   // L+R+Start held: sleep exactly once, at sleep_hold_frames.
      Runner r{m};
      r.frames(K(KEY_L) | K(KEY_R), 1);
      r.frames(K(KEY_L) | K(KEY_R) | K(KEY_START), m.sleep_hold_frames - 1);
      CHECK(r.count(InputEvent::Sleep) == 0);
      r.frames(K(KEY_L) | K(KEY_R) | K(KEY_START), 1);
      CHECK(r.count(InputEvent::Sleep) == 1);
      r.frames(K(KEY_L) | K(KEY_R) | K(KEY_START), 200);
      CHECK(r.count(InputEvent::Sleep) == 1);
      CHECK(r.count(InputEvent::SoftReset) == 0);
   }
   {   // L+R+Start+Select: reset, never sleep.
      Runner r{m};
      const KeyBits all = K(KEY_L) | K(KEY_R) | K(KEY_START) | K(KEY_SELECT);
      r.frames(all, m.reset_hold_frames + 1);
      CHECK(r.count(InputEvent::SoftReset) == 1);
      CHECK(r.count(InputEvent::Sleep) == 0);
   }
   {   // Switching chords restarts the count.
      Runner r{m};
      const KeyBits sleep = K(KEY_L) | K(KEY_R) | K(KEY_START);
      r.frames(sleep, m.sleep_hold_frames - 10);
      r.frames(sleep | K(KEY_SELECT), m.reset_hold_frames - 10);
      r.frames(sleep, m.sleep_hold_frames - 10);
      CHECK(r.count(InputEvent::Sleep) == 0 && r.count(InputEvent::SoftReset) == 0);
   }
   {   // Releasing early: nothing.
      Runner r{m};
      r.frames(K(KEY_L) | K(KEY_R) | K(KEY_START), m.sleep_hold_frames / 2);
      r.frames(0, 120);
      CHECK(r.count(InputEvent::Sleep) == 0);
   }
}

// Rule 5: Start/Select inside a chord never trigger their actions, on either target.
void test_chord_keys_suppressed(const InputMap& m, Action start_action, Action select_action)
{
   Runner r{m};
   ActionMask seen = 0;
   auto run = [&](KeyBits keys, int n)
   {
      for (int i = 0; i < n; ++i)
      {
         r.frames(keys, 1);
         seen |= r.state.pressed;
      }
   };
   run(K(KEY_L) | K(KEY_R), 2);
   run(K(KEY_L) | K(KEY_R) | K(KEY_START) | K(KEY_SELECT), 10);
   run(K(KEY_START) | K(KEY_SELECT), 10);   // shoulders released first
   run(0, 2);
   CHECK(!(seen & action_bit(start_action)));
   CHECK(!(seen & action_bit(select_action)));
   // Once released, Start works again.
   r.frames(K(KEY_START), 1);
   CHECK(r.pressed(start_action));
}

} // namespace

int main()
{
   test_target_ids();
   test_gba_mapping();
   test_snes_mapping();
   test_dreamcast_mapping();
   test_same_button_differs_by_target();
   test_pressed_is_edge();
   test_shoulders(kTestInputMapGba);
   test_shoulders(kTestInputMapSnes);
   test_shoulders(kTestInputMapDreamcast);
   test_chord_keys_suppressed(kTestInputMapGba, ACTION_MAIN_MENU, ACTION_DEVICE_MENU);
   test_chord_keys_suppressed(kTestInputMapSnes, ACTION_TOGGLE_RUN, ACTION_HELP);
   std::printf("%d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
