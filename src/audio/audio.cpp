/// @file
/// The audio system's start, its two backend steps and its state (D-34, M4 B1a): init() from the build's table,
/// core::vsync() (the DMA counter) and core::vblank() (players in table order, then the clip players, the tone
/// channels and the mix), the output routing, snapshot() and the sequence port's link.
///
/// A busy player (an API call under way when the step interrupts it, GBA) is skipped for the frame together with
/// every player before it: the original chains each player's step through the one opened before it and returns at
/// once from a busy one. A driver-wide change under way skips the whole frame. vsync_off() stops both steps.
#include "audio_internal.h"

#include "capsule/audio/state.h"
#include "capsule/core/memory.h"
#include "capsule/hal/audio.h"
#include "capsule/seq/sequence_audio.h"

namespace capsule::audio::detail
{

CAPSULE_EWRAM_BSS Driver g_driver;
CAPSULE_EWRAM_BSS Player g_players[kPlayerSlots];
CAPSULE_EWRAM_BSS Track g_tracks[kMaxTracks];
CAPSULE_EWRAM_BSS Channel g_channels[kChannelSlots];

namespace
{

inline constexpr uint32_t kFrameRateScaled = 597275;   ///< The screen's refresh rate, Hz x 10,000.
inline constexpr uint32_t kRateRound = 5000;           ///< (597275 x samples + 5000) / 10000.
inline constexpr uint32_t kRateDivisor = 10000;        ///< ... / 10000.
inline constexpr uint32_t kCpuClock = 16777216;        ///< 2^24 Hz.
inline constexpr uint8_t kToneMaskBase = 0x11;         ///< NR51 bits of tone 1 (both sides); x2 per channel.
inline constexpr uint8_t kDefaultChannels = 5;         ///< A table with 0 channels gets the original's 5.
inline constexpr uint16_t kFadeSteps = 16;             ///< A fade's steps.
inline constexpr uint8_t kStartRate = 3;               ///< The table's rate index when out of range (13,379 Hz).

bool g_started = false;              ///< init() ran.
PlayerId g_seq_player = kNoPlayer;   ///< The player of the sequence port's song.

/// Opens a player: paused, its tracks cleared.
/// @param p      the player
/// @param first  its first track
/// @param count  its tracks
/// @param check  its start check
void open_player(Player& p, uint8_t first, uint8_t count, uint8_t check)
{
   p = Player{};
   p.first_track = first;
   p.track_count = count;
   p.start_check = check;
   p.status = kPaused;
   p.sound = kNoSound;
   for (uint8_t i = 0; i < count; ++i)
   {
      Track& t = g_tracks[first + i];
      t = Track{};
      t.chan = kNoChannel;
   }
}

/// Frames per step of a fade over a length.
/// @param frames  the fade's length
/// @return frames per step, at least 1
uint16_t fade_speed(uint16_t frames)
{
   const uint16_t s = static_cast<uint16_t>((frames + kFadeSteps - 1) / kFadeSteps);
   return s != 0 ? s : 1;
}

}   // namespace

}   // namespace capsule::audio::detail

namespace capsule::audio
{

using namespace detail;

void init()
{
   const rom::Table& table = rom::kTable;
   g_driver = Driver{};
   for (Channel& ch : g_channels)
   {
      ch = Channel{};
      ch.prev = kNoChannel;
      ch.next = kNoChannel;
   }
   for (uint8_t i = 0; i < kToneChannels; ++i)
   {
      Channel& ch = g_channels[kFirstTone + i];
      ch.kind = static_cast<uint8_t>(static_cast<uint8_t>(rom::VoiceKind::SquareSweep) + i);
      ch.pan_mask = static_cast<uint8_t>(kToneMaskBase << i);
   }
   const uint8_t rate = table.rate_index < kRateCount ? table.rate_index : kStartRate;
   g_driver.frame_samples = kRateSamples[rate];
   g_driver.dma_period = static_cast<uint8_t>(core::kRingBytes / g_driver.frame_samples);
   const uint32_t pcm_freq = (kFrameRateScaled * g_driver.frame_samples + kRateRound) / kRateDivisor;
   g_driver.div_freq = (kCpuClock / pcm_freq + 1) >> 1;
   g_driver.channels = table.channels == 0 ? kDefaultChannels
                       : table.channels > kMaxSampledChannels ? kMaxSampledChannels
                                                              : table.channels;
   g_driver.master_volume = table.master_volume;
   g_driver.mono = table.stereo != 0 ? 0 : 1;
   g_driver.player_count = table.player_count < kMaxPlayers ? table.player_count : kMaxPlayers;

   // Players in table order, then the two clip players; tracks follow in the same order (their ranks).
   uint8_t first = 0;
   for (uint8_t i = 0; i < g_driver.player_count; ++i)
   {
      uint8_t count = table.players[i].tracks;
      count = count > kMaxPlayerTracks ? kMaxPlayerTracks : count;
      if (first + count + kClipPlayers * kClipTracks > kMaxTracks)
      {
         count = static_cast<uint8_t>(kMaxTracks - kClipPlayers * kClipTracks - first);
      }
      open_player(g_players[i], first, count, table.players[i].start_check);
      first = static_cast<uint8_t>(first + count);
   }
   for (uint8_t i = 0; i < kClipPlayers; ++i)
   {
      open_player(g_players[kMaxPlayers + i], first, kClipTracks, 0);
      first = static_cast<uint8_t>(first + kClipTracks);
   }
   g_driver.track_count = first;
   clear_ring();
   g_seq_player = kNoPlayer;
   g_started = true;
   hal::audio::init(g_ring);
   hal::audio::set_output(g_driver.mono == 0);
}

void set_output(Output o)
{
   g_driver.mono = o == Output::Mono ? 1 : 0;
   hal::audio::set_output(o == Output::Stereo);
}

void vsync_off()
{
   if (g_driver.output_off == 0)
   {
      g_driver.output_off = 1;
      clear_ring();
   }
}

void vsync_on()
{
   if (g_driver.output_off != 0)
   {
      g_driver.output_off = 0;
      g_driver.dma_counter = 0;
      hal::audio::restart_dma();
   }
}

const int8_t* last_block(Side side)
{
   const int8_t* base = side == Side::Right ? g_ring.right : g_ring.left;
   return base + g_driver.segment * g_driver.frame_samples;
}

void snapshot(Snapshot& out)
{
   out = Snapshot{};
   for (uint8_t i = 0; i < kPlayerSlots; ++i)
   {
      const bool clip = i >= kMaxPlayers;
      if (!clip && i >= g_driver.player_count)
      {
         continue;
      }
      const Player& p = g_players[i];
      PlayerState& s = out.players[clip ? g_driver.player_count + (i - kMaxPlayers) : i];
      s = {p.status, p.clock, p.sound, p.tempo_d, p.tempo_u, p.tempo_i, p.tempo_c, p.fade_oi, p.fade_oc, p.fade_ov,
           p.priority, p.track_count, p.first_track, p.start_check};
   }
   for (uint8_t i = 0; i < g_driver.track_count; ++i)
   {
      const Track& t = g_tracks[i];
      TrackState& s = out.tracks[i];
      s.events = t.events;
      s.start_offset = t.start_offset;
      s.offset = t.offset;
      s.hold = t.hold;
      for (uint8_t k = 0; k < kMaxCallDepth; ++k)
      {
         s.stack[k] = t.stack[k];
      }
      s.flags = t.flags;
      s.wait = t.wait;
      s.call_depth = t.call_depth;
      s.repeat = t.repeat;
      s.gate = t.gate;
      s.key = t.key;
      s.velocity = t.velocity;
      s.running = t.running;
      s.program = t.program;
      s.voice_kind = t.voice.kind;
      s.voice_key = t.voice.key;
      s.key_m = t.key_m;
      s.pit_m = t.pit_m;
      s.key_shift = t.key_shift;
      s.key_shift_x = t.key_shift_x;
      s.tune = t.tune;
      s.pit_x = t.pit_x;
      s.bend = t.bend;
      s.bend_range = t.bend_range;
      s.vol_mr = t.vol_mr;
      s.vol_ml = t.vol_ml;
      s.vol = t.vol;
      s.vol_x = t.vol_x;
      s.pan = t.pan;
      s.pan_x = t.pan_x;
      s.mod_m = t.mod_m;
      s.mod = t.mod;
      s.mod_type = t.mod_type;
      s.lfo_speed = t.lfo_speed;
      s.lfo_phase = t.lfo_phase;
      s.lfo_delay = t.lfo_delay;
      s.lfo_delay_count = t.lfo_delay_count;
      s.priority = t.priority;
      s.echo_volume = t.echo_volume;
      s.echo_length = t.echo_length;
      s.channel = t.chan;
   }
   for (uint8_t i = 0; i < kChannelSlots; ++i)
   {
      const Channel& c = g_channels[i];
      ChannelState& s = out.channels[i];
      const bool tone = i >= kFirstTone;
      s.frequency = c.frequency;
      s.position = c.position;
      s.count = c.count;
      s.fraction = c.fraction;
      s.status = c.status;
      s.kind = c.kind;
      s.track = c.track;
      s.key = c.key;
      s.midi_key = c.midi_key;
      s.velocity = c.velocity;
      s.priority = c.priority;
      s.gate = c.gate;
      s.rhythm_pan = c.rhythm_pan;
      s.right = c.right;
      s.left = c.left;
      s.env = c.env;
      s.env_right = tone ? c.env_goal : c.env_right;
      s.env_left = tone ? c.env_counter : c.env_left;
      s.attack = c.attack;
      s.decay = c.decay;
      s.sustain = c.sustain;
      s.release = c.release;
      s.echo_volume = c.echo_volume;
      s.echo_length = c.echo_length;
      s.sustain_goal = c.sustain_goal;
      s.pan = c.pan;
      s.n4 = c.n4;
      s.length = c.length;
      s.sweep = c.sweep;
      s.next = c.next;
   }
   for (uint8_t i = 0; i < kVarBytes; ++i)
   {
      out.var[i] = g_driver.var[i];
   }
   out.player_count = g_driver.player_count;
   out.track_count = g_driver.track_count;
   out.sampled_channels = g_driver.channels;
   out.reverb = g_driver.reverb;
   out.dma_counter = g_driver.dma_counter;
   out.segment = g_driver.segment;
   out.master_volume = g_driver.master_volume;
   out.mono = g_driver.mono;
   out.envelope_clock = g_driver.envelope_clock;
}

}   // namespace capsule::audio

namespace capsule::audio::core
{

using namespace detail;

const Ring& ring()
{
   return g_ring;
}

void vsync()
{
   if (g_driver.output_off != 0)
   {
      return;
   }
   // The counter is a byte; a step from 1 (or 0) reloads it and restarts the DMA at the ring's start.
   const int32_t left = int32_t(g_driver.dma_counter) - 1;
   g_driver.dma_counter = static_cast<uint8_t>(left);
   if (left > 0)
   {
      return;
   }
   g_driver.dma_counter = g_driver.dma_period;
   hal::audio::restart_dma();
}

void vblank()
{
   if (!g_started || g_driver.output_off != 0 || g_driver.busy != 0)
   {
      return;
   }
   // Players in order; a busy one skips itself and every player before it (the original's chain).
   uint8_t from = 0;
   const uint8_t count = g_driver.player_count;
   for (uint8_t i = 0; i < count + kClipPlayers; ++i)
   {
      const Player& p = g_players[i < count ? i : kMaxPlayers + (i - count)];
      if (p.busy != 0)
      {
         from = static_cast<uint8_t>(i + 1);
      }
   }
   for (uint8_t i = from; i < count + kClipPlayers; ++i)
   {
      player_main(g_players[i < count ? i : kMaxPlayers + (i - count)]);
   }
   tone_step();
   mix();
}

uint8_t segment()
{
   return g_driver.segment;
}

uint16_t frame_samples()
{
   return g_driver.frame_samples;
}

}   // namespace capsule::audio::core

namespace capsule::seq::audio_link
{

using namespace capsule::audio;

bool start_song(AssetKey song, uint16_t fade_in_frames)
{
   if (!detail::g_started)
   {
      return false;
   }
   const SoundId id = find_sound(song);
   if (id == kNoSound)
   {
      return false;
   }
   const PlayerId p = player_of(id);
   play(id);
   if (fade_in_frames != 0)
   {
      // The reference games' fade-in of a new song: started, reset at once, silenced, then 16 steps up.
      apply_start(p);
      set_volume(p, kAllTracks, 0);
      fade_in(p, detail::fade_speed(fade_in_frames));
   }
   detail::g_seq_player = p;
   return true;
}

void stop_song()
{
   if (detail::g_seq_player != kNoPlayer)
   {
      pause(detail::g_seq_player);
   }
}

bool fade_song(uint16_t frames)
{
   if (detail::g_seq_player == kNoPlayer)
   {
      return false;
   }
   fade_out(detail::g_seq_player, detail::fade_speed(frames));
   return true;
}

bool song_sounding()
{
   return detail::g_seq_player != kNoPlayer && !paused_or_ended(detail::g_seq_player);
}

bool play_sound(AssetKey sound)
{
   if (!detail::g_started)
   {
      return false;
   }
   const SoundId id = find_sound(sound);
   if (id == kNoSound)
   {
      return false;
   }
   play(id);
   return true;
}

}   // namespace capsule::seq::audio_link
