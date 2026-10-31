/// @file
/// Presenter view (capsule/battle/presenter_view.h) on its neutral battle data
/// (tests/data/battle_presenters):
/// presenters that fade share the one video blend, so it stays on while any
/// of them fades, goes off when the last fade ends, and always goes off in
/// reset() or when a fading presenter is replaced.
#include "capsule/asset/asset.h"
#include "capsule/battle/battle.h"
#include "capsule/battle/presenter_view.h"
#include "capsule/gfx/background.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/text/text_layer.h"

#include "../src/hal/soft/soft_video.h"
#include "battle_ids.h"

#include <cstdio>
#include <cstdlib>

namespace capsule::hal::system
{
/// Test build: report and abort.
/// @param expr  failed condition
/// @param file  file
/// @param line  line
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   std::printf("ASSERT %s:%d: %s\n", file, line, expr);
   std::abort();
}

/// Test build: fixed entropy (sequences mix it on a seed stage).
/// @return 0
uint32_t entropy()
{
   return 0;
}
} // namespace capsule::hal::system

namespace capsule::hal::input
{
/// Test build: no keys.
/// @return 0
KeyBits held()
{
   return 0;
}
} // namespace capsule::hal::input

namespace capsule::asset
{
namespace
{
const uint32_t kTiles[8] = {0x11111111, 0x11111111, 0x11111111, 0x11111111,
                            0x11111111, 0x11111111, 0x11111111, 0x11111111};   ///< One solid 8x8 tile.
const hal::video::Color kColors[16] = {0, hal::video::rgb5(31, 31, 31)};       ///< Transparent, white.
const uint8_t kFrames[1] = {0};                                                ///< Frame 0 only.
const Animation kAnimations[1] = {{kFrames, nullptr, 1, 1, 1}};                ///< Its one animation.
const Sprite kSprites[1] = {{kTiles, kColors, kAnimations, hal::video::OBJ_SQUARE, 0, 1, 1}};   ///< 8x8.
const Entry kEntries[1] = {{key_v1("sprite:PRESENTER"), Kind::Sprite, 0, 0}};   ///< The presenters' sprite.
} // namespace

/// The test's asset table: the sprite every test presenter uses.
const Table kTable = {kEntries, nullptr, kSprites, nullptr, nullptr, 1};
} // namespace capsule::asset

namespace
{

using namespace capsule;
namespace bt = capsule::battle;
namespace view = capsule::battle::presenter_view;
namespace video = hal::video;

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.

/// Records one check.
/// @param cond  condition that must hold
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

bt::State g_state{};   ///< Only the fields presenter_view reads: party count, sides, format.

/// The blend mode as last set.
/// @return the mode
video::BlendMode blend_mode()
{
   return static_cast<video::BlendMode>((hal::soft::pending_bldcnt() >> 6) & 3);
}

/// A presenter cue as the battle queues it.
/// @param presenter  presenter index (battle_ids PRESENTER_*)
/// @param party      party index
/// @param slot       the presenter's slot in its party
/// @param cue        kCueEnter, kCueExit or kCueReturn
/// @return the event
bt::Event cue_of(uint16_t presenter, uint8_t party, uint16_t slot, uint8_t cue)
{
   bt::Event e{};
   e.kind = bt::EventKind::Presenter;
   e.target = party;
   e.a = presenter;
   e.b = slot;
   e.extra = cue;
   return e;
}

/// Runs presenter frames.
/// @param n  frames
void frames(int n)
{
   for (int i = 0; i < n; ++i)
   {
      view::frame();
   }
}

/// Fresh video state, a two-party battle on format 0, the view reset.
void fresh()
{
   hal::soft::reset();
   hal::vram::reset();
   gfx::palette::reset();
   gfx::sprites::reset();
   g_state = bt::State{};
   g_state.party_count = 2;
   g_state.parties[1].side = 1;
   view::reset();
}

/// Two fades at once: the shorter one ending leaves the blend on for the longer.
void overlapping_fades()
{
   using namespace battle_ids;
   fresh();
   CHECK(blend_mode() == video::BlendMode::Off);
   CHECK(bt::kData.presenters[PRESENTER_SAGE].enter.style == bt::Motion::Fade);
   CHECK(bt::kData.presenters[PRESENTER_GUIDE].exit.style == bt::Motion::Fade);
   const int sage_frames = bt::kData.presenters[PRESENTER_SAGE].enter.frames;   // 32
   const int guide_frames = bt::kData.presenters[PRESENTER_GUIDE].exit.frames;  // 16
   CHECK(guide_frames < sage_frames);
   view::begin(g_state, cue_of(PRESENTER_SAGE, 0, 0, bt::kCueEnter));   // slot 6 fades in
   CHECK(view::busy() && blend_mode() == video::BlendMode::Alpha);
   view::begin(g_state, cue_of(PRESENTER_GUIDE, 0, 1, bt::kCueExit));    // slot 7 fades out
   frames(guide_frames);
   // Guide's fade is over, Sage's is not: the blend stays (it used to go off here, cutting Sage's fade).
   CHECK(view::busy() && blend_mode() == video::BlendMode::Alpha);
   frames(sage_frames - guide_frames);
   CHECK(!view::busy() && blend_mode() == video::BlendMode::Off);
}

/// reset() always turns the blend off, even mid-fade; a fading presenter
/// replaced by another lets go of the blend too.
void reset_and_replace()
{
   using namespace battle_ids;
   fresh();
   view::begin(g_state, cue_of(PRESENTER_SAGE, 1, 0, bt::kCueEnter));
   frames(3);
   CHECK(blend_mode() == video::BlendMode::Alpha);
   view::reset();
   CHECK(!view::busy() && blend_mode() == video::BlendMode::Off);
   // Party 1's first presenter slot: Sage fades in, then the Herald takes the slot (a slide).
   view::begin(g_state, cue_of(PRESENTER_SAGE, 1, 0, bt::kCueEnter));
   CHECK(blend_mode() == video::BlendMode::Alpha);
   view::begin(g_state, cue_of(PRESENTER_HERALD, 1, 0, bt::kCueEnter));
   CHECK(view::busy() && blend_mode() == video::BlendMode::Off);
   // A new fade starts after that one: the count is back to one, so it ends cleanly.
   view::begin(g_state, cue_of(PRESENTER_SAGE, 0, 0, bt::kCueEnter));
   CHECK(blend_mode() == video::BlendMode::Alpha);
   frames(bt::kData.presenters[PRESENTER_SAGE].enter.frames);
   CHECK(blend_mode() == video::BlendMode::Off);
}

} // namespace

/// Runs every check.
/// @return 0 when all pass
int main()
{
   overlapping_fades();
   reset_and_replace();
   std::printf("presenter view tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
