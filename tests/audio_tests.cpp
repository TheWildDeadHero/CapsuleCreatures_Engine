/// @file
/// The audio core's tests (D-34, M4 B1a), against the compiled test content (tests/data/audio/audio.json) and the
/// recording test backend (tests/support/audio_test_hal.h).
///
/// Without arguments: unit checks of the arithmetic, the API's predicates, priorities, clips, the DMA counter and
/// the busy-player rule. With `--run SCRIPT OUT`: runs a script (lines "frame op args...", the first "frames N")
/// and writes per frame the core's state (L1), both sides of the segment mixed (L2) and the tone register writes
/// (L3) as text; tests/test_audio_reference.py compares that with the Python reference model and the golden hashes.
#include "capsule/audio/audio.h"
#include "capsule/audio/core.h"
#include "capsule/audio/state.h"
#include "capsule/seq/sequence_audio.h"

#include "audio/audio_internal.h"
#include "support/audio_test_hal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace audio = capsule::audio;
namespace core = capsule::audio::core;
namespace test_hal = capsule::audio::test_hal;
using capsule::AssetKey;

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                                                                    \
   do                                                                                                                  \
   {                                                                                                                   \
      if (!(cond))                                                                                                     \
      {                                                                                                                \
         std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                          \
         ++g_failures;                                                                                                 \
      }                                                                                                                \
   } while (0)

/// One frame as the backends run it.
void frame()
{
   core::vsync();
   core::vblank();
}

/// FNV-1a over a string (the Asset Key v1 hash), for the find_sound() check.
uint32_t key_of(const char* text)
{
   uint32_t h = 2166136261u;
   for (const char* p = text; *p != 0; ++p)
   {
      h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
   }
   return h;
}

// --- unit checks ---------------------------------------------------------------------------------------------------

void test_arithmetic()
{
   namespace d = capsule::audio::detail;
   // Key 60 with no fine pitch: the sample's rate field / 1024.
   const capsule::audio::rom::SampleHeader h = {0, {0, 0}, 0, 13379u * 1024u, 0, 1};
   CHECK(d::key_to_freq(&h, 60, 0) == 13379u);
   CHECK(d::key_to_freq(&h, 72, 0) == 2u * 13379u);
   CHECK(d::key_to_freq(&h, 200, 0) == d::key_to_freq(&h, 178, 255));
   // Tone: key 69 (440 Hz) -> 2048 + (-1192 >> 2) = 1750 (2048 - 131072 / 440).
   CHECK(d::key_to_tone(1, 69, 0) == 1750u);
   CHECK(d::key_to_tone(2, 35, 200) == d::key_to_tone(2, 0, 0));
   CHECK(d::key_to_tone(4, 20, 0) == 0xD7u);
   CHECK(d::key_to_tone(4, 200, 0) == 0x00u);
   CHECK(d::kLengths[48] == 96 && d::kDeltas[8] == -64);
}

void test_players()
{
   audio::init();
   CHECK(audio::player_count() == 4 && audio::sound_count() == 21);
   CHECK(audio::player_of(8) == 1 && audio::player_of(999) == audio::kNoPlayer);
   CHECK(audio::find_sound(AssetKey{key_of("sound:BEEP")}) == 19);
   CHECK(audio::find_sound(AssetKey{key_of("song:BASIC")}) == 0);
   CHECK(audio::find_sound(AssetKey{key_of("song:NOPE")}) == audio::kNoSound);
   CHECK(audio::paused_or_ended(0) && audio::status(0).paused);
   audio::play(0);
   // Before the next step: started, nothing has run.
   CHECK(audio::status(0).tracks == 0 && !audio::status(0).paused && !audio::playing(0));
   frame();
   CHECK(audio::status(0).tracks == 3 && audio::playing(0) && audio::current_song(0) != nullptr);
   audio::pause(0);
   CHECK(audio::status(0).paused && audio::paused_or_ended(0));
   audio::resume(0);
   CHECK(audio::playing(0));
   audio::play_or_keep(0);
   frame();
   CHECK(audio::status(0).tracks == 3);

   // Start check: sfx1 plays the priority-5 song; the priority-1 one is refused, an equal one is not.
   audio::play(9);
   frame();
   const audio::rom::SongHeader* high = audio::current_song(1);
   audio::play(8);
   CHECK(audio::current_song(1) == high);
   audio::play(9);
   CHECK(audio::status(1).tracks == 0);   // restarted (not yet stepped)
   frame();
   audio::stop_song(8);                   // not its song: nothing
   CHECK(audio::playing(1));
   audio::stop_song(9);
   CHECK(audio::status(1).paused);
   audio::play(8);                        // paused: no longer refused
   frame();
   CHECK(audio::current_song(1) != high);
   audio::stop_all();
   CHECK(audio::status(0).paused && audio::status(1).paused);
   audio::resume_all();
   CHECK(!audio::status(0).paused);
   audio::set_var(3, 77);
   CHECK(audio::var(3) == 77 && audio::var(19) == 77);
}

void test_dma_counter()
{
   audio::init();
   const uint32_t before = test_hal::dma_restarts();
   for (int i = 0; i < 70; ++i)
   {
      frame();
   }
   CHECK(test_hal::dma_restarts() - before == 10);
   audio::Snapshot s;
   audio::snapshot(s);
   CHECK(s.dma_counter >= 1 && s.dma_counter <= 7);
   CHECK(core::segment() == (s.dma_counter <= 1 ? 0 : 8 - s.dma_counter));
   CHECK(audio::last_block(audio::Side::Left) == core::ring().left + core::segment() * core::frame_samples());
}

void test_busy()
{
   namespace d = capsule::audio::detail;
   audio::init();
   audio::play(0);    // music
   audio::play(17);   // sfx2
   frame();
   const uint32_t music = d::g_players[0].clock;
   const uint32_t sfx2 = d::g_players[2].clock;
   {
      // An interrupt inside a call on sfx1: sfx1 and every player before it skip the frame.
      const d::PlayerLock lock(d::g_players[1]);
      frame();
   }
   CHECK(d::g_players[0].clock == music);
   CHECK(d::g_players[2].clock == sfx2 + 1);
   d::g_driver.busy = 1;
   frame();
   d::g_driver.busy = 0;
   CHECK(d::g_players[2].clock == sfx2 + 1);
   frame();
   CHECK(d::g_players[0].clock == music + 1);
}

void test_clips()
{
   audio::init();
   audio::ClipParams c = {0, 127, 0, 60 * 256, 30, 0, 0, 10, 0};
   const audio::ClipHandle a = audio::play_clip(c);
   CHECK(a == 0);
   CHECK(!audio::clip_playing(a));   // not stepped yet
   frame();
   CHECK(audio::clip_playing(a));
   const audio::ClipHandle b = audio::play_clip(c);
   CHECK(b == 1);
   frame();
   frame();
   // Both busy: the one that has played longest (the first) is replaced.
   CHECK(audio::play_clip(c) == 0);
   audio::stop_clip(1);
   CHECK(!audio::clip_playing(1));
   for (int i = 0; i < 40; ++i)
   {
      frame();
   }
   CHECK(!audio::clip_playing(0));
   CHECK(audio::play_clip(c) == 0);
   c.clip = 999;   // unknown: nothing
   audio::play_clip(c);
}

void test_vsync_off()
{
   audio::init();
   audio::play(11);
   for (int i = 0; i < 10; ++i)
   {
      frame();
   }
   audio::vsync_off();
   const uint32_t clock = capsule::audio::detail::g_players[0].clock;
   frame();
   CHECK(capsule::audio::detail::g_players[0].clock == clock);
   bool silent = true;
   for (int i = 0; i < core::kRingBytes; ++i)
   {
      silent = silent && core::ring().right[i] == 0 && core::ring().left[i] == 0;
   }
   CHECK(silent);
   audio::vsync_on();
   frame();
   CHECK(capsule::audio::detail::g_players[0].clock == clock + 1);
}

void test_sequence_link()
{
   namespace link = capsule::seq::audio_link;
   audio::init();
   CHECK(!link::start_song(AssetKey{key_of("song:NOPE")}, 0));
   CHECK(link::start_song(AssetKey{key_of("song:FADE")}, 32));   // sound 10 on the music player, fading in
   CHECK(capsule::audio::detail::g_players[0].fade_ov == capsule::audio::detail::kFadeIn);
   frame();
   CHECK(link::song_sounding());
   CHECK(link::fade_song(16));
   for (int i = 0; i < 17; ++i)
   {
      frame();
   }
   CHECK(!link::song_sounding() && audio::ended(0));
   CHECK(link::play_sound(AssetKey{key_of("sound:BEEP")}));
   CHECK(!link::play_sound(AssetKey{key_of("sound:NOPE")}));
   CHECK(link::start_song(AssetKey{key_of("song:BASIC")}, 0));
   link::stop_song();
   CHECK(audio::status(0).paused);
}

// --- the script runner -----------------------------------------------------------------------------------------------

constexpr int kMaxArgs = 10;     ///< Arguments of a script line.
constexpr int kLineBytes = 256;  ///< A script line.

/// One script event.
struct Event
{
   int frame;                ///< When.
   char op[32];              ///< What.
   long args[kMaxArgs];      ///< Its arguments.
   int count;                ///< Their number.
};

/// Runs one event (an API call).
/// @param e  the event
/// @param locked  players to keep busy through this frame's step (set)
/// @param driver_locked  the driver kept busy (set)
/// @return false for an unknown op
bool apply(const Event& e, uint32_t& locked, bool& driver_locked)
{
   const long* a = e.args;
   const auto u8 = [](long v) { return static_cast<uint8_t>(v); };
   const auto u16 = [](long v) { return static_cast<uint16_t>(v); };
   const char* op = e.op;
   if (std::strcmp(op, "play") == 0) { audio::play(u16(a[0])); }
   else if (std::strcmp(op, "play_or_keep") == 0) { audio::play_or_keep(u16(a[0])); }
   else if (std::strcmp(op, "stop_song") == 0) { audio::stop_song(u16(a[0])); }
   else if (std::strcmp(op, "stop_all") == 0) { audio::stop_all(); }
   else if (std::strcmp(op, "resume_all") == 0) { audio::resume_all(); }
   else if (std::strcmp(op, "pause") == 0) { audio::pause(u8(a[0])); }
   else if (std::strcmp(op, "resume") == 0) { audio::resume(u8(a[0])); }
   else if (std::strcmp(op, "fade_out") == 0) { audio::fade_out(u8(a[0]), u16(a[1])); }
   else if (std::strcmp(op, "fade_out_paused") == 0) { audio::fade_out_paused(u8(a[0]), u16(a[1])); }
   else if (std::strcmp(op, "fade_in") == 0) { audio::fade_in(u8(a[0]), u16(a[1])); }
   else if (std::strcmp(op, "apply_start") == 0) { audio::apply_start(u8(a[0])); }
   else if (std::strcmp(op, "set_volume") == 0) { audio::set_volume(u8(a[0]), u16(a[1]), u16(a[2])); }
   else if (std::strcmp(op, "set_pitch") == 0) { audio::set_pitch(u8(a[0]), u16(a[1]), static_cast<int16_t>(a[2])); }
   else if (std::strcmp(op, "set_pan") == 0) { audio::set_pan(u8(a[0]), u16(a[1]), static_cast<int8_t>(a[2])); }
   else if (std::strcmp(op, "set_tempo_scale") == 0) { audio::set_tempo_scale(u8(a[0]), u16(a[1])); }
   else if (std::strcmp(op, "set_mod_depth") == 0) { audio::set_mod_depth(u8(a[0]), u16(a[1]), u8(a[2])); }
   else if (std::strcmp(op, "set_lfo_speed") == 0) { audio::set_lfo_speed(u8(a[0]), u16(a[1]), u8(a[2])); }
   else if (std::strcmp(op, "clip") == 0)
   {
      const audio::ClipParams c = {u16(a[0]), u8(a[1]), static_cast<int8_t>(a[2]), static_cast<int16_t>(a[3]),
                                   u16(a[4]), u8(a[5]), u8(a[6]), u8(a[7]), static_cast<uint32_t>(a[8])};
      audio::play_clip(c);
   }
   else if (std::strcmp(op, "stop_clip") == 0) { audio::stop_clip(u8(a[0])); }
   else if (std::strcmp(op, "clear_clips") == 0) { audio::clear_clips(); }
   else if (std::strcmp(op, "output") == 0) { audio::set_output(a[0] != 0 ? audio::Output::Stereo : audio::Output::Mono); }
   else if (std::strcmp(op, "set_var") == 0) { audio::set_var(u8(a[0]), u8(a[1])); }
   else if (std::strcmp(op, "vsync_off") == 0) { audio::vsync_off(); }
   else if (std::strcmp(op, "vsync_on") == 0) { audio::vsync_on(); }
   else if (std::strcmp(op, "lock") == 0) { locked |= 1u << a[0]; }
   else if (std::strcmp(op, "lock_driver") == 0) { driver_locked = true; }
   else
   {
      return false;
   }
   return true;
}

/// Writes one frame's dump.
/// @param out    the file
/// @param frame  its number
void dump(std::FILE* out, int frame_no)
{
   audio::Snapshot s;
   audio::snapshot(s);
   std::fprintf(out, "F %d\n", frame_no);
   for (int i = 0; i < s.player_count + audio::kClipPlayers; ++i)
   {
      const audio::PlayerState& p = s.players[i];
      std::fprintf(out, "P %d %lu %lu %u %u %u %u %u %u %u %u %u %u %u %u\n", i, static_cast<unsigned long>(p.status),
                   static_cast<unsigned long>(p.clock), p.sound, p.tempo_d, p.tempo_u, p.tempo_i, p.tempo_c, p.fade_oi,
                   p.fade_oc, p.fade_ov, p.priority, p.track_count, p.first_track, p.start_check);
   }
   for (int i = 0; i < s.track_count; ++i)
   {
      const audio::TrackState& t = s.tracks[i];
      std::fprintf(out,
                   "T %d %lu %lu %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %d %u %d %d %d %u %d %u %u %u %u %u "
                   "%d %d %d %u %u %u %u %u %u %u %u %u %u\n",
                   i, static_cast<unsigned long>(t.events), static_cast<unsigned long>(t.start_offset), t.offset,
                   t.hold, t.stack[0], t.stack[1], t.stack[2], t.flags, t.wait, t.call_depth, t.repeat, t.gate, t.key,
                   t.velocity, t.running, t.program, t.voice_kind, t.voice_key, t.key_m, t.pit_m, t.key_shift,
                   t.key_shift_x, t.tune, t.pit_x, t.bend, t.bend_range, t.vol_mr, t.vol_ml, t.vol, t.vol_x, t.pan,
                   t.pan_x, t.mod_m, t.mod, t.mod_type, t.lfo_speed, t.lfo_phase, t.lfo_delay, t.lfo_delay_count,
                   t.priority, t.echo_volume, t.echo_length, t.channel);
   }
   for (int i = 0; i < audio::kMaxSampledChannels + audio::kToneChannels; ++i)
   {
      const audio::ChannelState& c = s.channels[i];
      std::fprintf(out,
                   "C %d %lu %ld %ld %lu %u %u %u %u %u %u %u %u %d %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u "
                   "%u\n",
                   i, static_cast<unsigned long>(c.frequency), static_cast<long>(c.position),
                   static_cast<long>(c.count), static_cast<unsigned long>(c.fraction), c.status, c.kind, c.track,
                   c.key, c.midi_key, c.velocity, c.priority, c.gate, c.rhythm_pan, c.right, c.left, c.env,
                   c.env_right, c.env_left, c.attack, c.decay, c.sustain, c.release, c.echo_volume, c.echo_length,
                   c.sustain_goal, c.pan, c.n4, c.length, c.sweep, c.next);
   }
   std::fprintf(out, "D %u %u %u %u %u %u", s.reverb, s.dma_counter, s.segment, s.master_volume, s.mono,
                s.envelope_clock);
   for (uint8_t v : s.var)
   {
      std::fprintf(out, " %u", v);
   }
   std::fprintf(out, "\n");
   const uint16_t n = core::frame_samples();
   const int8_t* sides[2] = {audio::last_block(audio::Side::Right), audio::last_block(audio::Side::Left)};
   const char tags[2] = {'R', 'L'};
   for (int side = 0; side < 2; ++side)
   {
      std::fprintf(out, "%c ", tags[side]);
      for (uint16_t i = 0; i < n; ++i)
      {
         std::fprintf(out, "%02x", static_cast<uint8_t>(sides[side][i]));
      }
      std::fprintf(out, "\n");
   }
   std::fprintf(out, "W");
   for (uint16_t i = 0; i < test_hal::write_count() && i < test_hal::kMaxWrites; ++i)
   {
      std::fprintf(out, " %02x=%02x", test_hal::writes()[i].reg, test_hal::writes()[i].value);
   }
   std::fprintf(out, "\n");
#if defined(CAPSULE_AUDIO_HQ)
   if (const int16_t* hq = test_hal::hq(false))
   {
      long sum = 0;
      for (uint32_t i = 0; i < 4u * n; ++i)
      {
         sum += hq[i] < 0 ? -hq[i] : hq[i];
      }
      std::fprintf(out, "H %ld\n", sum);
   }
#endif
}

/// Runs a script.
/// @param script  its path
/// @param out_path  the dump's path
/// @return exit status
int run(const char* script, const char* out_path)
{
   std::FILE* in = std::fopen(script, "r");
   std::FILE* out = std::fopen(out_path, "w");
   if (in == nullptr || out == nullptr)
   {
      std::printf("cannot open %s or %s\n", script, out_path);
      return 2;
   }
   static Event events[4096];
   int count = 0;
   int frames = 0;
   char line[kLineBytes];
   while (std::fgets(line, sizeof line, in) != nullptr)
   {
      if (std::sscanf(line, "frames %d", &frames) == 1)
      {
         continue;
      }
      Event& e = events[count];
      char* p = line;
      char* end = nullptr;
      e.frame = static_cast<int>(std::strtol(p, &end, 10));
      if (end == p)
      {
         continue;
      }
      p = end;
      int used = 0;
      if (std::sscanf(p, " %31s%n", e.op, &used) != 1)
      {
         continue;
      }
      p += used;
      e.count = 0;
      while (e.count < kMaxArgs)
      {
         const long v = std::strtol(p, &end, 0);
         if (end == p)
         {
            break;
         }
         e.args[e.count++] = v;
         p = end;
      }
      if (count < 4095)
      {
         ++count;
      }
   }
   std::fclose(in);
   audio::init();
   int next = 0;
   for (int f = 0; f < frames; ++f)
   {
      uint32_t locked = 0;
      bool driver_locked = false;
      while (next < count && events[next].frame == f)
      {
         if (!apply(events[next], locked, driver_locked))
         {
            std::printf("unknown op %s\n", events[next].op);
            return 2;
         }
         ++next;
      }
      for (uint8_t i = 0; i < 32; ++i)
      {
         if ((locked & (1u << i)) != 0)
         {
            capsule::audio::detail::g_players[i].busy = 1;
         }
      }
      capsule::audio::detail::g_driver.busy = driver_locked ? 1 : 0;
      frame();
      for (auto& pl : capsule::audio::detail::g_players)
      {
         pl.busy = 0;
      }
      capsule::audio::detail::g_driver.busy = 0;
      dump(out, f);
      test_hal::clear();
   }
   std::fclose(out);
   return 0;
}

}   // namespace

int main(int argc, char** argv)
{
   if (argc == 4 && std::strcmp(argv[1], "--run") == 0)
   {
      return run(argv[2], argv[3]);
   }
   test_arithmetic();
   test_players();
   test_dma_counter();
   test_busy();
   test_clips();
   test_vsync_off();
   test_sequence_link();
   if (g_failures != 0)
   {
      std::printf("%d check(s) failed\n", g_failures);
      return 1;
   }
   std::printf("audio tests passed\n");
   return 0;
}
