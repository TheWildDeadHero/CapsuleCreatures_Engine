/// @file
/// Audio test (D-34 M4 B1b): plays the audio tour (apps/common/audio_tour.h) over the build's audio table, the
/// engine's generated test songs and clips (tests/data/audio), on the GBA and on desktop, and shows the players, the
/// tour's position and the GBA's cost of each vertical blank's audio step (timer 1).
///
/// The same tour runs headless in apps/audio_render, so a GBA run read every frame with gba_run.py (--read of the
/// core's ring capsule::audio::detail::g_ring, the backend's capsule::hal::audio::g_probe and test_audio::g_record)
/// compares with the headless run step for step (format §11.4, L1 / L2; the debug build's tone log, L3). The
/// program calls system::update() once right after the core's init() and before the tour's first calls, as
/// audio_render does.
///
/// C14a (the audio clock, capsule/hal/audio.h): the tour's modelled lag frame spends its blanks with
/// rng::spend_blanks() (the GBA runs those steps while the DMA plays its hold buffer), and on the GBA the main loop
/// also lags one blank of its own every kOwnLagPeriod frames, which must step nothing: the GBA's states then equal
/// audio_render's step for step.
///
/// Controls: A restarts the tour once it is done; SELECT switches mono / stereo (off the tour: it changes the
/// output, so only after the tour).
#include "audio_tour.h"
#include "test_kit.h"

#include "capsule/audio/audio.h"
#include "capsule/audio/state.h"
#include "capsule/core/memory.h"
#include "capsule/core/random.h"
#include "capsule/hal/audio.h"
#include "capsule/hal/system.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace test_audio
{

/// Bytes after Record::phase that keep the snapshot word-aligned.
constexpr int kRecordPad = 3;

/// What the test runner reads each frame (gba_run.py --read test_audio::g_record): the core's state after the
/// latest vertical blank the main loop saw, with that blank's step count.
struct Record
{
   uint32_t steps;                  ///< hal::audio::budget().steps when the snapshot was taken.
   uint32_t tour_frame;             ///< The tour's frame.
   uint8_t phase;                   ///< audio_tour::Phase.
   uint8_t pad[kRecordPad];         ///< 0.
   capsule::audio::Snapshot state;  ///< The core's state.
};

/// The record (EWRAM; also shown on screen, so it is kept).
CAPSULE_EWRAM_BSS Record g_record;

} // namespace test_audio

namespace
{

using namespace capsule;
using namespace capsule::testkit;
using hal::input::KEY_A;
using hal::input::KEY_SELECT;

constexpr auto kApp = text::encode("AUDIO TEST");                         ///< App title.
constexpr auto kHint1 = text::encode("The tour plays by itself");         ///< Controls, line 1.
constexpr auto kHint2 = text::encode("A again  SELECT mono (done)");      ///< Controls, line 2.

/// @name Layout
///@{
constexpr int kLabelCol = 1;        ///< Column of the labels.
constexpr int kValueCol = 9;        ///< Column of the values.
constexpr int kPhaseRow = 2;        ///< "phase".
constexpr int kItemRow = 3;         ///< "item".
constexpr int kFrameRow = 4;        ///< "frame".
constexpr int kCountRow = 5;        ///< "played".
constexpr int kPlayersRow = 7;      ///< First player row.
constexpr int kMaxPlayerRows = 6;   ///< Player rows shown.
constexpr int kStepsRow = 14;       ///< "steps".
constexpr int kCostRow = 15;        ///< "cost".
constexpr int kMaxCostRow = 16;     ///< "max".
constexpr int kNumberWidth = 6;     ///< Digits of a counter.
constexpr int kTrackDigits = 4;     ///< Hex digits of a track mask.
constexpr int kPlayerValueCol = 4;  ///< Column of a player's track mask.
constexpr int kPausedCol = 10;      ///< Column of a player's "paused".
constexpr int kSoundCol = 17;       ///< Column of a player's sound number (after "paused").
constexpr int kClockCol = 23;       ///< Column of a player's clock.
///@}

constexpr uint32_t kPausedBit = 0x80000000u;   ///< PlayerState::status: paused.
constexpr uint32_t kTracksMask = 0xFFFFu;      ///< PlayerState::status: tracks.

/// GBA: frames between two forced lag blanks of the program's own (a prime, so they fall on every kind of frame).
constexpr uint32_t kOwnLagPeriod = 97;

audio_tour::Tour g_tour;     ///< The tour.
bool g_stereo = true;        ///< Output after the tour.

/// GBA: waits through one vertical blank with the frame unfinished, a lag frame of this build's own (C14a: the
/// audio clock does not step; the DMA repeats the segment it played). Elsewhere frames do not lag: nothing.
void own_lag_blank()
{
#if defined(CAPSULE_PLATFORM_GBA)
   const uint32_t before = hal::system::vblanks();
   while (hal::system::vblanks() == before)
   {
   }
#endif
}

/// Copies the state the latest blank left into the record.
void record()
{
   test_audio::g_record.steps = hal::audio::budget().steps;
   test_audio::g_record.tour_frame = g_tour.frame;
   test_audio::g_record.phase = static_cast<uint8_t>(g_tour.phase);
   audio::snapshot(test_audio::g_record.state);
}

/// Draws the fixed labels.
void draw_labels()
{
   clear_console();
   title(kApp.codes, 0, 0, nullptr);
   put_ascii(INK_DIM, kLabelCol, kPhaseRow, "phase");
   put_ascii(INK_DIM, kLabelCol, kItemRow, "item");
   put_ascii(INK_DIM, kLabelCol, kFrameRow, "frame");
   put_ascii(INK_DIM, kLabelCol, kCountRow, "played");
   put_ascii(INK_DIM, kLabelCol, kStepsRow, "steps");
   put_ascii(INK_DIM, kLabelCol, kCostRow, "cycles");
   put_ascii(INK_DIM, kLabelCol, kMaxCostRow, "max");
   controls(kHint1.codes, kHint2.codes);
}

/// Parts of the page draw() cycles through, one a frame (the tour's rows, each player row, the budget rows), so
/// the console's writes stay small and the main loop never lags (its only lag blanks are the forced ones).
constexpr int kDrawParts = kMaxPlayerRows + 2;

int g_draw_part = 0;   ///< The part draw() draws next.

/// Draws one part of the values.
void draw()
{
   const test_audio::Record& r = test_audio::g_record;
   const int part = g_draw_part;
   g_draw_part = (g_draw_part + 1) % kDrawParts;
   if (part == 0)
   {
      blank(kValueCol, kCols, kPhaseRow);
      put_ascii(INK_ON, kValueCol, kPhaseRow, audio_tour::phase_name(g_tour.phase));
      put_uint(INK_ON, kValueCol, kItemRow, g_tour.index, kNumberWidth);
      put_uint(INK_ON, kValueCol, kFrameRow, r.tour_frame, kNumberWidth);
      const int x = put_uint(INK_ON, kValueCol, kCountRow, g_tour.sounds, kNumberWidth);
      put_uint(INK_DIM, x + 1, kCountRow, g_tour.clips, kNumberWidth);
      return;
   }
   if (part == kDrawParts - 1)
   {
      const hal::audio::Budget b = hal::audio::budget();
      put_uint(INK_ON, kValueCol, kStepsRow, b.steps, kNumberWidth);
      put_uint(INK_ON, kValueCol, kCostRow, b.last_cycles, kNumberWidth);
      put_uint(INK_ON, kValueCol, kMaxCostRow, b.max_cycles, kNumberWidth);
      return;
   }
   const int i = part - 1;
   const int players = r.state.player_count + audio::kClipPlayers;
   if (i >= players)
   {
      return;
   }
   const audio::PlayerState& p = r.state.players[i];
   const int row = kPlayersRow + i;
   blank(0, kCols, row);
   put_ascii(INK_DIM, kLabelCol, row, i < r.state.player_count ? "P" : "C");
   put_uint(INK_DIM, kLabelCol + 1, row, static_cast<uint32_t>(i < r.state.player_count ? i : i - r.state.player_count));
   put_hex(INK_ON, kPlayerValueCol, row, p.status & kTracksMask, kTrackDigits);
   if ((p.status & kPausedBit) != 0)
   {
      put_ascii(INK_OFF, kPausedCol, row, "paused");
   }
   put_uint(INK_ON, kSoundCol, row, p.sound, kNumberWidth - 1);
   put_uint(INK_DIM, kClockCol, row, p.clock, kNumberWidth);
}

} // namespace

/// Runs the audio test until the platform quits.
/// @return 0
int main()
{
   hal::system::init();
   init_console();
   draw_labels();
   audio::init();
   hal::system::update();   // the first blank, before any call (audio_render does the same)
   record();
   audio_tour::start(g_tour);
   Pad pad;
   while (true)
   {
      const audio_tour::Blanks blanks = audio_tour::frame(g_tour);
      if (blanks == audio_tour::Blanks::Lag)
      {
         rng::spend_blanks(audio_tour::kLagBlanks);   // the reference's lag: steps now (the audio clock)
      }
      if (g_tour.frame % kOwnLagPeriod == 0)
      {
         own_lag_blank();
      }
      if (blanks == audio_tour::Blanks::None)
      {
         if (pad.hit(KEY_A))
         {
            audio::stop_all();
            audio_tour::start(g_tour);
         }
         if (pad.hit(KEY_SELECT))
         {
            g_stereo = !g_stereo;
            audio::set_output(g_stereo ? audio::Output::Stereo : audio::Output::Mono);
         }
      }
      draw();
      if (!hal::system::update())
      {
         break;
      }
      record();
      pad.update();
   }
   return 0;
}
