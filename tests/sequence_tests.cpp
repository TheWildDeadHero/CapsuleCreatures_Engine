// Runtime tests for the sequence player: title flows, credits and a cutscene,
// compiled from tests/data/sequences/*.json and run against a recording mock
// of the port and game hooks. Pure logic: no HAL linked.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "capsule/seq/sequence.h"
#include "capsule/seq/sequence_port.h"

#include "seq_title_timeout.inc"
#include "seq_title_song_end.inc"
#include "seq_credits_map.inc"
#include "seq_credits_showcase.inc"
#include "seq_cutscene.inc"

using namespace capsule;
using namespace capsule::seq;

// Asset Key v1 (C-02d), for checking which asset was loaded.
constexpr uint32_t Key(const char* s)
{
   uint32_t h = 0x811C9DC5u;
   for (; *s; ++s)
   {
      h ^= static_cast<uint8_t>(*s);
      h *= 0x01000193u;
   }
   return h;
}

namespace mock
{
int brightness = 0;
bool white = false;
bool sprite_shown[kMaxSprites];
int sprite_toggles[kMaxSprites];
uint32_t sprite_asset[kMaxSprites];
int sprite_loads = 0;
uint16_t sprite_scale[kMaxSprites];
int bg_x[4], bg_y[4], bg_moves = 0;
int map_x = 0, map_y = 0;
uint32_t map = 0;
int win[2][4];
int pal_coef = -1;
uint32_t pal_mask = 0;
int blend_src = 0, blend_tgt = 0;
int sfx = 0, seeds = 0, mode = -1, emitters = 0, texts = 0, text_busy_frames = 0;
bool box_open = false;
bool song_on = false;
int song_left = 0;
uint32_t song_frames = 0;
bool cond[8];
uint16_t list_n = 0;
void reset()
{
   brightness = 0;
   white = false;
   std::memset(sprite_shown, 0, sizeof sprite_shown);
   std::memset(sprite_toggles, 0, sizeof sprite_toggles);
   std::memset(sprite_asset, 0, sizeof sprite_asset);
   std::memset(sprite_scale, 0, sizeof sprite_scale);
   std::memset(bg_x, 0, sizeof bg_x);
   std::memset(bg_y, 0, sizeof bg_y);
   std::memset(win, 0, sizeof win);
   std::memset(cond, 0, sizeof cond);
   sprite_loads = bg_moves = map_x = map_y = 0;
   map = 0;
   pal_coef = -1;
   pal_mask = 0;
   blend_src = blend_tgt = sfx = seeds = emitters = texts = text_busy_frames = 0;
   mode = -1;
   box_open = false;
   song_on = false;
   song_left = 0;
   song_frames = 0;
   list_n = 0;
}
void tick()
{
   if (song_on)
   {
      ++song_frames;
      if (song_left > 0 && --song_left == 0)
      {
         song_on = false;
      }
   }
   if (text_busy_frames > 0)
   {
      --text_busy_frames;
   }
}
} // namespace mock

namespace capsule::seq::port
{
void set_video_mode(uint8_t m)
{
   mock::mode = m;
}
void bg_load(uint8_t, AssetKey)
{
}
void bg_show(uint8_t, bool)
{
}
void bg_offset(uint8_t bg, int16_t x, int16_t y)
{
   mock::bg_x[bg] = x;
   mock::bg_y[bg] = y;
   ++mock::bg_moves;
}
void blend(uint8_t, uint8_t, uint8_t)
{
}
void blend_weights(uint8_t s, uint8_t t)
{
   mock::blend_src = s;
   mock::blend_tgt = t;
}
void brightness(uint8_t level, bool white)
{
   mock::brightness = level;
   mock::white = white;
}
void palette_fade(uint32_t mask, uint16_t, uint8_t coef)
{
   mock::pal_mask = mask;
   mock::pal_coef = coef;
}
void window(uint8_t w, uint16_t, uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
   mock::win[w][0] = x0;
   mock::win[w][1] = y0;
   mock::win[w][2] = x1;
   mock::win[w][3] = y1;
}
void sprite_load(uint8_t s, AssetKey a)
{
   mock::sprite_asset[s] = raw(a);
   ++mock::sprite_loads;
}
void sprite_show(uint8_t s, bool on)
{
   if (mock::sprite_shown[s] != on)
   {
      ++mock::sprite_toggles[s];
   }
   mock::sprite_shown[s] = on;
}
void sprite_pos(uint8_t, int16_t, int16_t)
{
}
void sprite_anim(uint8_t, uint16_t)
{
}
void sprite_obj_window(uint8_t, bool)
{
}
void sprite_affine(uint8_t s, uint16_t scale, uint16_t)
{
   mock::sprite_scale[s] = scale;
}
void sprite_blend(uint8_t, bool)
{
}
void sprites_clear()
{
}
void emitter(uint8_t, bool on, AssetKey)
{
   if (on)
   {
      ++mock::emitters;
   }
}
void palette_anim(uint8_t, bool, AssetKey)
{
}
void scanline(uint8_t, bool, AssetKey)
{
}
void effects_clear()
{
}
void text_box(bool open, uint8_t, uint8_t, uint8_t, uint8_t)
{
   mock::box_open = open;
}
void text_print(uint8_t, uint8_t, uint8_t, uint8_t, bool typed, uint8_t, AssetKey)
{
   ++mock::texts;
   if (typed)
   {
      mock::text_busy_frames = 40;
   }
}
void text_clear(uint8_t, uint8_t, uint8_t)
{
}
bool text_busy()
{
   return mock::text_busy_frames > 0;
}
void text_clear_all()
{
}
void map_show(AssetKey m)
{
   mock::map = raw(m);
}
void map_camera(int16_t x, int16_t y)
{
   mock::map_x = x;
   mock::map_y = y;
}
void song_play(AssetKey a, uint16_t)
{
   mock::song_on = raw(a) != 0;
   mock::song_frames = 0;
   if (mock::song_left == 0)
   {
      mock::song_left = 1000000;
   }
}
void song_fade(uint16_t)
{
}
bool song_playing()
{
   return mock::song_on;
}
uint32_t song_frame()
{
   return mock::song_frames;
}
void sfx_play(AssetKey)
{
   ++mock::sfx;
}
} // namespace capsule::seq::port

namespace capsule::seq::game
{
bool cond(uint8_t id)
{
   return id < 8 && mock::cond[id];
}
uint16_t list_count(uint8_t)
{
   return mock::list_n;
}
AssetKey list_asset(uint8_t, uint16_t i)
{
   return static_cast<AssetKey>(0x1000u + i);
}
void rng_mix_entropy(uint32_t)
{
   ++mock::seeds;
}
} // namespace capsule::seq::game

// No HAL is linked: player asserts report here and abort the test.
namespace capsule::hal::system
{
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   std::printf("ASSERT %s:%d: %s\n", file, line, expr);
   std::abort();
}
} // namespace capsule::hal::system

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                                    \
   do                                                                                                                  \
   {                                                                                                                   \
      ++g_checks;                                                                                                      \
      if (!(cond))                                                                                                     \
      {                                                                                                                \
         ++g_fail;                                                                                                     \
         std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)

constexpr KeyBits A = 1u << 0, B = 1u << 1, SELECT = 1u << 2, START = 1u << 3, LEFT = 1u << 5, UP = 1u << 6;

static void Frame(Player& p, KeyBits held = 0, KeyBits pressed = 0)
{
   step(p, held, pressed);
   mock::tick();
}

// Frames until the player is in `stage` (counted when the stage change is taken), or -1.
static int RunUntilStage(Player& p, uint8_t stage, int max_frames, KeyBits held = 0)
{
   for (int f = 1; f <= max_frames; ++f)
   {
      Frame(p, held);
      if (p.status == Status::Exited)
      {
         return -1;
      }
      if (p.stage == stage)
      {
         return f;
      }
   }
   return -1;
}
static int RunUntilExit(Player& p, int max_frames, KeyBits held = 0)
{
   for (int f = 1; f <= max_frames; ++f)
   {
      Frame(p, held);
      if (p.status == Status::Exited)
      {
         return f;
      }
   }
   return -1;
}
static void Press(Player& p, KeyBits k)
{
   Frame(p, k, k);
   Frame(p);
}

static void TitleTests()
{
   enum
   {
      K_COPY,
      K_INTRO,
      K_ENTER,
      K_SNAP,
      K_RUN,
      K_CONFIRM
   };
   enum
   {
      H_COPY,
      H_INTRO,
      H_LOGO,
      H_BANNERS,
      H_SNAP,
      H_IDLE,
      H_CONFIRM
   };
   {   // unattended: reaches the title, 2700-frame idle returns to copyright
      mock::reset();
      Player p;
      start(p, kSeqTitleTimeout);
      CHECK(RunUntilStage(p, K_INTRO, 400) > 0);
      CHECK(RunUntilStage(p, K_ENTER, 1000) > 0);
      CHECK(RunUntilStage(p, K_RUN, 200) > 0);
      Frame(p);
      CHECK(mock::emitters == 1);
      CHECK(RunUntilStage(p, K_COPY, 3000) == 2699);
      CHECK(mock::sprite_toggles[1] >= 2 * (2700 / 90) - 2);
   }
   {   // skip intro + title enter, confirm -> cry -> main menu
      mock::reset();
      Player p;
      start(p, kSeqTitleTimeout);
      RunUntilStage(p, K_INTRO, 400);
      Frame(p);
      Press(p, A);
      CHECK(p.stage == K_ENTER);
      Frame(p);
      Press(p, START);
      RunUntilStage(p, K_RUN, 5);
      CHECK(mock::bg_x[2] == 0);
      Frame(p);
      Press(p, A);
      CHECK(p.stage == K_CONFIRM && mock::seeds == 1);
      CHECK(RunUntilExit(p, 500) >= 90 + 16);
      CHECK(p.exit_code == kSeqTitleTimeoutExit_MAIN_MENU && mock::sfx == 1 && mock::brightness == 16 && mock::white);
   }
   {   // clear-save combo
      mock::reset();
      Player p;
      start(p, kSeqTitleTimeout);
      RunUntilStage(p, K_RUN, 2000);
      Frame(p);
      Frame(p, B | SELECT | UP, UP);
      CHECK(p.status == Status::Exited && p.exit_code == kSeqTitleTimeoutExit_CLEAR_SAVE);
   }
   {   // title screen: song end loops back; skip snaps; guarded RTC combo
      mock::reset();
      Player p;
      start(p, kSeqTitleSongEnd);
      RunUntilStage(p, H_LOGO, 2000);
      Frame(p);
      CHECK(mock::mode == 1);
      CHECK(RunUntilStage(p, H_IDLE, 500) > 0);
      mock::song_left = 300;
      const int back = RunUntilStage(p, H_COPY, 1000);
      CHECK(back >= 299 && back <= 301);
   }
   {
      mock::reset();
      Player p;
      start(p, kSeqTitleSongEnd);
      RunUntilStage(p, H_LOGO, 2000);
      Frame(p);
      Frame(p);
      Press(p, B);
      RunUntilStage(p, H_IDLE, 5);
      CHECK(p.stage == H_IDLE && mock::bg_y[2] == 36 && mock::brightness == 0);
      CHECK(!mock::sprite_shown[0] && mock::sprite_shown[1]);
      Frame(p);
      Frame(p, B | SELECT | LEFT, LEFT);
      CHECK(p.status == Status::Running);              // condition false
      mock::cond[kSeqTitleSongEndCond_RTC_RESETTABLE] = true;
      Frame(p, B | SELECT | LEFT, 0);
      CHECK(p.status == Status::Exited && p.exit_code == kSeqTitleSongEndExit_RESET_RTC);
   }
}

static void CreditsTests()
{
   enum
   {
      RIDE1,
      SHOW,
      RIDE2,
      END,
      LEAVE
   };
   int normal_ride = 0;
   {   // full run, male player, 3 showcase entries
      mock::reset();
      mock::list_n = 3;
      Player p;
      start(p, kSeqCreditsShowcase);
      Frame(p);
      CHECK(mock::sprite_asset[4] == Key("sprite:CREDITS RIDER MALE"));
      for (int i = 0; i < 80; ++i)
      {
         Frame(p);
      }
      CHECK(mock::pal_mask == (1u << 8) && mock::pal_coef < 16);   // page 1 fading in
      normal_ride = RunUntilStage(p, SHOW, 5000);
      CHECK(normal_ride + 81 >= 2 * (72 + 115 + 16));   // 2 pages; the 16 f fade-in overlaps the hold
      normal_ride += 80;
      int loads = mock::sprite_loads;
      CHECK(RunUntilStage(p, RIDE2, 5000) > 0);
      CHECK(mock::sprite_loads - loads >= 3);           // one load per list item
      CHECK(mock::sprite_scale[0] == 256);              // zoom ramp ended at 1.0x
      CHECK(mock::sprite_asset[0] == 0x1002u);          // last item
      CHECK(RunUntilStage(p, END, 5000) > 0);
      CHECK(RunUntilStage(p, LEAVE, 8000) == 7200);     // THE END timeout
      CHECK(RunUntilExit(p, 100) > 0 && p.exit_code == kSeqCreditsShowcaseExit_SOFT_RESET);
   }
   {   // female branch + fast-forward needs the Hall of Fame condition
      mock::reset();
      mock::cond[kSeqCreditsShowcaseCond_PLAYER_FEMALE] = true;
      Player p;
      start(p, kSeqCreditsShowcase);
      Frame(p);
      CHECK(mock::sprite_asset[4] == Key("sprite:CREDITS RIDER FEMALE"));
      CHECK(mock::sprite_asset[5] == Key("sprite:CREDITS PARTNER MALE"));
      CHECK(RunUntilStage(p, SHOW, 5000, B) == normal_ride);   // condition false: B does nothing
   }
   {
      mock::reset();
      mock::cond[kSeqCreditsShowcaseCond_FAST_FORWARD_ALLOWED] = true;
      Player p;
      start(p, kSeqCreditsShowcase);
      Frame(p, B);
      const int fast = RunUntilStage(p, SHOW, 5000, B);
      CHECK(fast > 0 && fast <= normal_ride / 2 + 2);
   }
   {   // empty showcase list skips the loop body
      mock::reset();
      mock::list_n = 0;
      Player p;
      start(p, kSeqCreditsShowcase);
      RunUntilStage(p, SHOW, 5000);
      int loads = mock::sprite_loads;
      CHECK(RunUntilStage(p, RIDE2, 5000) > 0);
      CHECK(mock::sprite_loads == loads);               // loop body never ran
      Frame(p);
      CHECK(mock::sprite_loads - loads == 2);           // next scene's riders
   }
   {   // credits: window slit, map camera pan, showcase cry, THE END on A
      enum
      {
         OPEN,
         TOWNS,
         SHOWC,
         COPY,
         THEEND
      };
      mock::reset();
      Player p;
      start(p, kSeqCreditsMap);
      for (int i = 0; i < 45; ++i)
      {
         Frame(p);
      }
      CHECK(mock::win[0][1] == 0 && mock::win[0][3] == 160);
      CHECK(RunUntilStage(p, TOWNS, 2000) > 0);
      for (int i = 0; i < 200; ++i)
      {
         Frame(p);
      }
      CHECK(mock::map == Key("map:CREDITS TOWN 1") && mock::map_x > 100 && mock::map_y == 64);
      CHECK(RunUntilStage(p, SHOWC, 3000) > 0);
      CHECK(mock::map_y < 320);                         // second scene panned upward
      CHECK(RunUntilStage(p, COPY, 1000) > 0 && mock::sfx == 1);
      CHECK(RunUntilStage(p, THEEND, 1000) > 0);
      Frame(p);
      Frame(p);
      Press(p, A);
      CHECK(RunUntilExit(p, 100) > 0 && p.exit_code == kSeqCreditsMapExit_SOFT_RESET);
   }
}

static void CutsceneTests()
{
   {   // A advances each line only after the typed text finishes
      mock::reset();
      Player p;
      start(p, kSeqCutscene);
      int f = 0;
      while (!mock::box_open && f < 1000)
      {
         Frame(p);
         ++f;
      }
      CHECK(mock::box_open && mock::bg_moves >= 16);    // 8 shakes x 2 + reset
      CHECK(mock::sprite_scale[0] == 256);
      for (int line = 1; line <= 3; ++line)
      {
         Frame(p);
         CHECK(mock::texts == line);
         Press(p, A);                                  // too early: text still typing
         CHECK(mock::texts == line);
         for (int i = 0; i < 45; ++i)
         {
            Frame(p);
         }
         Press(p, A);
      }
      CHECK(RunUntilExit(p, 200) > 0 && p.exit_code == kSeqCutsceneExit_DONE);
      CHECK(!mock::box_open);
   }
   {   // B skips the whole scene
      mock::reset();
      Player p;
      start(p, kSeqCutscene);
      for (int i = 0; i < 50; ++i)
      {
         Frame(p);
      }
      Press(p, B);
      CHECK(RunUntilExit(p, 100) > 0 && p.exit_code == kSeqCutsceneExit_SKIPPED);
   }
}

int main()
{
   TitleTests();
   CreditsTests();
   CutsceneTests();
   std::printf("sizeof(Player) = %zu B\n", sizeof(Player));
   std::printf("sequence tests: %d checks, %d failures\n", g_checks, g_fail);
   return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
