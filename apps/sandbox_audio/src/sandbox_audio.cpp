/// @file
/// Audio sandbox (D-34 M4 B1b): the audio module on its own, played by hand over the build's audio table (the
/// engine's generated test songs and clips, tests/data/audio). A sound effect tester in miniature: pick a sound
/// number or a clip voice, play it, fade, pause, change volume, pitch and tempo, switch mono / stereo, and watch
/// every player's tracks, pause bit, sound, clock and fade, the reverb and the DMA counter.
///
/// - Up / Down: the sound number; Left / Right: the clip voice.
/// - A: play the sound; START: play the clip (chorus 0); B: stop everything.
/// - L: fade the sound's player out (4 frames a step); R: fade it in.
/// - SELECT: mono / stereo.
/// - A while B is held: pause / resume the sound's player.
#include "test_kit.h"

#include "capsule/audio/audio.h"
#include "capsule/audio/rom.h"
#include "capsule/audio/state.h"
#include "capsule/core/memory.h"
#include "capsule/hal/audio.h"
#include "capsule/hal/system.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace
{

using namespace capsule;
using namespace capsule::testkit;
using hal::input::KEY_A;
using hal::input::KEY_B;
using hal::input::KEY_DOWN;
using hal::input::KEY_L;
using hal::input::KEY_LEFT;
using hal::input::KEY_R;
using hal::input::KEY_RIGHT;
using hal::input::KEY_SELECT;
using hal::input::KEY_START;
using hal::input::KEY_UP;

constexpr auto kApp = text::encode("AUDIO SANDBOX");                  ///< App title.
constexpr auto kHint1 = text::encode("UD sound A play B stop LR clip");   ///< Controls, line 1.
constexpr auto kHint2 = text::encode("START clip L/R fade SEL mono");     ///< Controls, line 2.

/// @name Layout
///@{
constexpr int kLabelCol = 1;          ///< Labels.
constexpr int kValueCol = 9;          ///< Values.
constexpr int kSoundRow = 2;          ///< "sound".
constexpr int kClipRow = 3;           ///< "clip".
constexpr int kOutputRow = 4;         ///< "output".
constexpr int kMixRow = 5;            ///< "reverb" / "dma".
constexpr int kPlayersRow = 7;        ///< First player row.
constexpr int kMaxPlayerRows = 8;     ///< Player rows shown.
constexpr int kCostRow = 16;          ///< "cycles".
constexpr int kNumberWidth = 5;       ///< Digits of a number.
constexpr int kTrackDigits = 4;       ///< Hex digits of a track mask.
constexpr int kTracksCol = 4;         ///< Player's track mask.
constexpr int kPausedCol = 9;         ///< "P" when paused.
constexpr int kSoundCol = 11;         ///< Player's sound.
constexpr int kClockCol = 17;         ///< Player's clock.
constexpr int kFadeCol = 24;          ///< Player's fade volume.
constexpr int kDmaCol = 20;           ///< The DMA counter.
///@}

constexpr uint16_t kFadeSpeed = 4;             ///< Frames per fade step.
constexpr uint32_t kPausedBit = 0x80000000u;   ///< PlayerState::status: paused.
constexpr uint32_t kTracksMask = 0xFFFFu;      ///< PlayerState::status: tracks.
constexpr int16_t kClipPitch = 60 * 256;       ///< Key 60: the voice's own pitch.
constexpr uint8_t kClipVolume = 127;           ///< Clip volume.
constexpr uint16_t kClipHold = 60;             ///< Clip ticks before the release.
constexpr uint8_t kClipPriority = 10;          ///< Clip priority.

uint16_t g_sound = 0;     ///< The chosen sound number.
uint16_t g_clip = 0;      ///< The chosen clip voice.
bool g_stereo = true;     ///< The output.
CAPSULE_EWRAM_BSS audio::Snapshot g_state;   ///< The state shown.

/// Handles the pad.
/// @param pad  the pad
void control(const Pad& pad)
{
   const uint16_t sounds = audio::sound_count();
   const uint16_t clips = audio::rom::kTable.clip_count;
   if (sounds > 0 && pad.repeat(KEY_UP))
   {
      g_sound = static_cast<uint16_t>((g_sound + 1) % sounds);
   }
   if (sounds > 0 && pad.repeat(KEY_DOWN))
   {
      g_sound = static_cast<uint16_t>((g_sound + sounds - 1) % sounds);
   }
   if (clips > 0 && pad.repeat(KEY_RIGHT))
   {
      g_clip = static_cast<uint16_t>((g_clip + 1) % clips);
   }
   if (clips > 0 && pad.repeat(KEY_LEFT))
   {
      g_clip = static_cast<uint16_t>((g_clip + clips - 1) % clips);
   }
   const audio::PlayerId p = sounds > 0 ? audio::player_of(g_sound) : audio::kNoPlayer;
   if (pad.hit(KEY_A) && pad.down(KEY_B) && p != audio::kNoPlayer)
   {
      if (audio::status(p).paused)
      {
         audio::resume(p);
      }
      else
      {
         audio::pause(p);
      }
   }
   else if (pad.hit(KEY_A) && sounds > 0)
   {
      audio::play(g_sound);
   }
   else if (pad.hit(KEY_B))
   {
      audio::stop_all();
   }
   if (pad.hit(KEY_START) && clips > 0)
   {
      audio::ClipParams c{};
      c.clip = g_clip;
      c.volume = kClipVolume;
      c.pitch = kClipPitch;
      c.hold_ticks = kClipHold;
      c.priority = kClipPriority;
      audio::play_clip(c);
   }
   if (pad.hit(KEY_L) && p != audio::kNoPlayer)
   {
      audio::fade_out(p, kFadeSpeed);
   }
   if (pad.hit(KEY_R) && p != audio::kNoPlayer)
   {
      audio::fade_in(p, kFadeSpeed);
   }
   if (pad.hit(KEY_SELECT))
   {
      g_stereo = !g_stereo;
      audio::set_output(g_stereo ? audio::Output::Stereo : audio::Output::Mono);
   }
}

/// Parts of the page draw() cycles through, one a frame (the settings rows, each player row, the cost row): the
/// console's writes stay small, so the frame never runs past its vertical blank.
constexpr int kDrawParts = kMaxPlayerRows + 2;

int g_draw_part = 0;   ///< The part draw() draws next.

/// Draws one part of the page.
void draw()
{
   const int part = g_draw_part;
   g_draw_part = (g_draw_part + 1) % kDrawParts;
   if (part == 0)
   {
      audio::snapshot(g_state);   // once a round: every part of the round shows the same state
      blank(kValueCol, kCols, kSoundRow);
      int x = put_uint(INK_ON, kValueCol, kSoundRow, g_sound, kNumberWidth);
      x = put_ascii(INK_DIM, x + 1, kSoundRow, "of");
      put_uint(INK_DIM, x + 1, kSoundRow, audio::sound_count());
      blank(kValueCol, kCols, kClipRow);
      x = put_uint(INK_ON, kValueCol, kClipRow, g_clip, kNumberWidth);
      x = put_ascii(INK_DIM, x + 1, kClipRow, "of");
      put_uint(INK_DIM, x + 1, kClipRow, audio::rom::kTable.clip_count);
      blank(kValueCol, kCols, kOutputRow);
      put_ascii(INK_ON, kValueCol, kOutputRow, g_state.mono != 0 ? "mono" : "stereo");
      put_uint(INK_ON, kValueCol, kMixRow, g_state.reverb, kNumberWidth);
      put_ascii(INK_DIM, kDmaCol - kNumberWidth + 1, kMixRow, "dma");
      put_uint(INK_ON, kDmaCol, kMixRow, g_state.dma_counter);
      return;
   }
   if (part == kDrawParts - 1)
   {
      const hal::audio::Budget b = hal::audio::budget();
      blank(kValueCol, kCols, kCostRow);
      const int x = put_uint(INK_ON, kValueCol, kCostRow, b.last_cycles, kNumberWidth + 1);
      put_uint(INK_DIM, x + 1, kCostRow, b.max_cycles, kNumberWidth + 1);
      return;
   }
   const int i = part - 1;
   if (i >= g_state.player_count + audio::kClipPlayers)
   {
      return;
   }
   const audio::PlayerState& p = g_state.players[i];
   const int row = kPlayersRow + i;
   blank(0, kCols, row);
   put_ascii(INK_DIM, kLabelCol, row, i < g_state.player_count ? "P" : "C");
   put_uint(INK_DIM, kLabelCol + 1, row, static_cast<uint32_t>(i < g_state.player_count ? i : i - g_state.player_count));
   put_hex(INK_ON, kTracksCol, row, p.status & kTracksMask, kTrackDigits);
   put_ascii((p.status & kPausedBit) != 0 ? INK_OFF : INK_DIM, kPausedCol, row, (p.status & kPausedBit) != 0 ? "P" : "-");
   put_uint(INK_ON, kSoundCol, row, p.sound, kNumberWidth);
   put_uint(INK_DIM, kClockCol, row, p.clock, kNumberWidth + 1);
   put_uint(INK_DIM, kFadeCol, row, p.fade_ov, kNumberWidth);
}

} // namespace

/// Runs the sandbox until the platform quits.
/// @return 0
int main()
{
   hal::system::init();
   init_console();
   title(kApp.codes, 0, 0, nullptr);
   put_ascii(INK_DIM, kLabelCol, kSoundRow, "sound");
   put_ascii(INK_DIM, kLabelCol, kClipRow, "clip");
   put_ascii(INK_DIM, kLabelCol, kOutputRow, "output");
   put_ascii(INK_DIM, kLabelCol, kMixRow, "reverb");
   put_ascii(INK_DIM, kLabelCol, kCostRow, "cycles");
   controls(kHint1.codes, kHint2.codes);
   audio::init();
   Pad pad;
   while (hal::system::update())
   {
      pad.update();
      control(pad);
      draw();
   }
   return 0;
}
