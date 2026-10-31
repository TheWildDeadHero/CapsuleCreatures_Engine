/// @file
/// The global random streams (A-4). The battle stream lives in each battle's
/// state (capsule::battle::State), seeded per battle. The start values below
/// only matter until system::init() seeds the streams.
#include "capsule/core/random.h"

namespace capsule::rng
{

namespace
{

/// Start state of the gameplay stream (any odd-looking constant; seed() replaces it).
constexpr uint32_t kGameplayStart = 0x2545F491u;
/// Start state of the cosmetic stream.
constexpr uint32_t kCosmeticStart = 0x9E3779B9u;
/// Salt that keeps the cosmetic stream's seed apart from the gameplay stream's.
constexpr uint32_t kCosmeticSalt = 0x5BD1E995u;

} // namespace

Stream gameplay = {kGameplayStart};
Stream cosmetic = {kCosmeticStart};

namespace
{

FrameCondition g_frame_condition = nullptr;   ///< N1: the per-frame advance's rule (nullptr: off).
uint32_t g_frame_advances = 0;                ///< N1: its draws so far.
bool g_press_seeding = true;                  ///< U5: the first press reseeds the streams.
AdvanceClock g_advance_clock = AdvanceClock::Blank;   ///< C13b: the per-frame advance's clock.
uint32_t g_spent_blanks = 0;                  ///< C13d: the extra blanks spend_blanks() spent.
BlankHook g_blank_hook = nullptr;             ///< C14a: what else steps at a modelled blank (the audio clock).

} // namespace

void set_advance_clock(AdvanceClock clock)
{
   g_advance_clock = clock;
}

AdvanceClock advance_clock()
{
   return g_advance_clock;
}

void set_frame_advance(FrameCondition condition)
{
   g_frame_condition = condition;
}

void frame_advance()
{
   if (g_frame_condition != nullptr && g_frame_condition())
   {
      next16(gameplay);
      ++g_frame_advances;
   }
}

uint32_t frame_advances()
{
   return g_frame_advances;
}

void spend_blanks(uint32_t blanks)
{
   // C13d: each modelled blank is one more per-frame advance (the reference's interrupt draws at every blank)
   for (uint32_t i = 0; i < blanks; ++i)
   {
      frame_advance();
   }
   g_spent_blanks += blanks;
   // C14a (D-34): the audio clock steps the same blanks (the hook the sound backend installed)
   if (blanks != 0 && g_blank_hook != nullptr)
   {
      g_blank_hook(blanks);
   }
}

void set_blank_hook(BlankHook hook)
{
   g_blank_hook = hook;
}

uint32_t spent_blanks()
{
   return g_spent_blanks;
}

void set_press_seeding(bool on)
{
   g_press_seeding = on;
}

bool press_seeding()
{
   return g_press_seeding;
}

void seed(uint32_t entropy)
{
   mix(gameplay, entropy);
   mix(cosmetic, entropy ^ kCosmeticSalt);   // different salt: the streams stay apart
   next16(gameplay);                       // spread the new bits through the state
   next16(cosmetic);
}

} // namespace capsule::rng
