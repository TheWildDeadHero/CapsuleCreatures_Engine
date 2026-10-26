/// @file
/// N1 (D-32) engine tests: the per-frame gameplay draw (capsule/core/random.h set_frame_advance(); every HAL's
/// system::update() calls frame_advance() once a frame): off by default, one draw per frame while the game's
/// condition holds, none while it does not, off again with nullptr.
#include "capsule/core/random.h"

#include <cstdint>
#include <cstdio>

namespace
{

using namespace capsule;

int g_failures = 0;   ///< Failed checks.

/// Records one check.
/// @param ok    whether it held
/// @param what  what was checked
void check(bool ok, const char* what)
{
   if (!ok)
   {
      ++g_failures;
      std::printf("FAIL: %s\n", what);
   }
}

bool g_allowed = true;   ///< The test's condition.

/// The test's condition (a game's "not in a link battle").
/// @return g_allowed
bool allowed()
{
   return g_allowed;
}

/// The gameplay stream's state after n draws from a state.
/// @param state  the start
/// @param n      draws
/// @return the state
uint32_t after(uint32_t state, int n)
{
   rng::Stream s{state};
   for (int i = 0; i < n; ++i)
   {
      rng::next16(s);
   }
   return s.state;
}

/// The per-frame draw.
void test_frame_advance()
{
   constexpr int kFrames = 10;   // frames per phase
   rng::gameplay.state = 0x12345678u;
   const uint32_t start = rng::gameplay.state;
   rng::frame_advance();
   check(rng::gameplay.state == start && rng::frame_advances() == 0, "off by default");

   rng::set_frame_advance(allowed);
   for (int i = 0; i < kFrames; ++i)
   {
      rng::frame_advance();
   }
   check(rng::frame_advances() == kFrames, "one draw per frame");
   check(rng::gameplay.state == after(start, kFrames), "each is the gameplay stream's next draw");

   g_allowed = false;
   const uint32_t held = rng::gameplay.state;
   for (int i = 0; i < kFrames; ++i)
   {
      rng::frame_advance();
   }
   check(rng::gameplay.state == held && rng::frame_advances() == kFrames, "no draw while the condition fails");

   g_allowed = true;
   rng::set_frame_advance(nullptr);
   rng::frame_advance();
   check(rng::gameplay.state == held, "off again");
}

/// C13b: the advance's clock is the game's choice, Blank by default (the GBA HAL's update() draws once per
/// hardware blank with Blank, once per frame with Frame; emulator runs check the hardware side).
void test_advance_clock()
{
   check(rng::advance_clock() == rng::AdvanceClock::Blank, "blank clock by default (C11)");
   rng::set_advance_clock(rng::AdvanceClock::Frame);
   check(rng::advance_clock() == rng::AdvanceClock::Frame, "a game may choose one draw per frame");
   rng::set_advance_clock(rng::AdvanceClock::Blank);
   check(rng::advance_clock() == rng::AdvanceClock::Blank, "and back");
}

/// C13d: spend_blanks() makes the per-frame advance once per modelled blank, under the condition, on either clock.
void test_spend_blanks()
{
   constexpr uint32_t kBlanks = 3;   // a load's modelled extra blanks
   constexpr uint32_t kCallsWithBlanks = 3;   // the spend_blanks(kBlanks) calls below
   rng::gameplay.state = 0x0BADF00Du;
   const uint32_t start = rng::gameplay.state;
   const uint32_t advances = rng::frame_advances();
   const uint32_t spent = rng::spent_blanks();
   rng::spend_blanks(kBlanks);
   check(rng::gameplay.state == start && rng::spent_blanks() == spent + kBlanks,
         "no draw while the per-frame advance is off (the blanks are still counted)");

   rng::set_frame_advance(allowed);
   rng::set_advance_clock(rng::AdvanceClock::Frame);
   rng::spend_blanks(kBlanks);
   check(rng::gameplay.state == after(start, static_cast<int>(kBlanks)), "one draw per extra blank");
   check(rng::frame_advances() == advances + kBlanks, "each one is a per-frame advance");
   rng::spend_blanks(0);
   check(rng::gameplay.state == after(start, static_cast<int>(kBlanks)), "no blanks, no draw");

   g_allowed = false;
   const uint32_t held = rng::gameplay.state;
   rng::spend_blanks(kBlanks);
   check(rng::gameplay.state == held, "the condition holds back every blank's draw (a link battle)");
   check(rng::spent_blanks() == spent + kCallsWithBlanks * kBlanks, "every call counts its blanks");

   g_allowed = true;
   rng::set_advance_clock(rng::AdvanceClock::Blank);
   rng::spend_blanks(1);
   check(rng::gameplay.state == after(held, 1), "the blank clock draws them too");
   rng::set_frame_advance(nullptr);
}

} // namespace

/// Runs the tests.
/// @return 0 when all pass
int main()
{
   test_frame_advance();
   test_advance_clock();
   test_spend_blanks();
   std::printf("n1 tests: %s\n", g_failures == 0 ? "ok" : "FAILED");
   return g_failures == 0 ? 0 : 1;
}
