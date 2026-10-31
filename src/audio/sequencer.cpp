/// @file
/// The players and their tracks (D-34, M4 B1a; inventory sections 2.2, 4.2, 4.4, 4.5, 4.7): one frame of a player
/// (fade, ticks at its tempo, the volume / pitch pass), the track commands of format section 3, song starts with
/// the start check, the track reset and the volume / pitch arithmetic.
///
/// A frame: tempo_c += tempo_i; while tempo_c >= 150 one tick runs (each existing track: its channels' gates count
/// down, a START track is reset, its commands run until it waits, its wait counts down, its LFO steps), the clock
/// counts it and the status becomes the tracks that existed; with none left the player pauses and the frame ends.
/// Then every track with a change recomputes its volume and pitch and pushes them to its channels.
#include "audio_internal.h"

namespace capsule::audio::detail
{
namespace
{

/// @name Arithmetic of the reference driver
///@{
inline constexpr uint32_t kVolXShift = 5;          ///< vol x volX >> 5.
inline constexpr uint32_t kModVolShift = 7;        ///< Volume modulation: x (modM + 128) >> 7.
inline constexpr int32_t kModBias = 128;           ///< modM + 128.
inline constexpr int32_t kPanLow = -128;           ///< Pan clamp, left.
inline constexpr int32_t kPanHigh = 127;           ///< Pan clamp, right.
inline constexpr int32_t kPanRightBias = 128;      ///< Right volume: (y + 128) x >> 8.
inline constexpr int32_t kPanLeftBias = 127;       ///< Left volume: (127 - y) x >> 8.
inline constexpr uint32_t kSideShift = 8;          ///< Side volume shift.
inline constexpr int32_t kPanScale = 2;            ///< Track pan counts twice.
inline constexpr int32_t kTuneScale = 4;           ///< (tune + bend x range) x 4.
inline constexpr int32_t kKeyScale = 256;          ///< A key is 256 pitch units.
inline constexpr int32_t kModPitchScale = 16;      ///< Pitch modulation: 16 x modM.
inline constexpr int32_t kKeyShift = 8;            ///< Pitch >> 8: the key offset.
inline constexpr uint8_t kLfoQuarter = 64;         ///< The triangle rises below phase 64 and above 191.
inline constexpr int32_t kLfoHalf = 128;           ///< Falling half: 128 - phase.
inline constexpr int32_t kModShift = 6;            ///< depth x triangle >> 6.
inline constexpr uint32_t kTempoScaleShift = 8;    ///< tempo_d x tempo_u >> 8.
inline constexpr uint16_t kTempoPerValue = 2;      ///< TEMPO v: tempo_d = 2v.
inline constexpr uint16_t kVolumeQuarter = 4;      ///< set_volume(): volX = volume / 4.
inline constexpr uint8_t kAddressBytes = 2;        ///< A jump target (u16).
inline constexpr uint8_t kTrackStartKind = static_cast<uint8_t>(rom::VoiceKind::SquareSweep);   ///< Reset voice.
inline constexpr uint8_t kVarMask = kVarBytes - 1; ///< Variable index bits.
inline constexpr uint8_t kModTypePitch = 0;       ///< MOD_TYPE: pitch.
inline constexpr uint8_t kModTypeVolume = 1;      ///< MOD_TYPE: volume.
inline constexpr uint8_t kModTypePan = 2;         ///< MOD_TYPE: pan.
inline constexpr uint8_t kStartOffsetBytes = 4;    ///< EXT 4's u32.
inline constexpr uint32_t kByteBits = 8;           ///< Bits per byte (little-endian reads).
///@}

/// VAR operations.
enum VarOp : uint8_t
{
   kVarSet = 0,          ///< area = data.
   kVarAdd,              ///< area += data.
   kVarSub,              ///< area -= data.
   kVarCopy,             ///< area = area[data].
   kVarAddVar,           ///< area += area[data].
   kVarSubVar,           ///< area -= area[data].
   kVarEq,               ///< Jump if area == data.
   kVarNe,               ///< Jump if area != data.
   kVarGt,               ///< Jump if area > data.
   kVarGe,               ///< Jump if area >= data.
   kVarLe,               ///< Jump if area <= data.
   kVarLt,               ///< Jump if area < data.
   kVarEqVar,            ///< Jump if area == area[data].
   kVarNeVar,            ///< Jump if area != area[data].
   kVarGtVar,            ///< Jump if area > area[data].
   kVarGeVar,            ///< Jump if area >= area[data].
   kVarLeVar,            ///< Jump if area <= area[data].
   kVarLtVar,            ///< Jump if area < area[data].
};

/// A little-endian u16 of the song data.
/// @param d   the data
/// @param at  its offset
/// @return the value
uint16_t read16(const uint8_t* d, uint16_t at)
{
   return static_cast<uint16_t>(d[at] | (d[at + 1] << kByteBits));
}

/// A little-endian u32 of the song data.
/// @param d   the data
/// @param at  its offset
/// @return the value
uint32_t read32(const uint8_t* d, uint16_t at)
{
   uint32_t v = 0;
   for (uint32_t i = kStartOffsetBytes; i > 0; --i)
   {
      v = (v << kByteBits) | d[at + i - 1];
   }
   return v;
}

/// The next argument byte of a track.
/// @param p  its player
/// @param t  the track
/// @return the byte (the position moves past it)
uint8_t arg(const Player& p, Track& t)
{
   return p.data[t.offset++];
}

/// The player's tracks.
/// @param p  the player
/// @return its first track
Track* tracks_of(Player& p)
{
   return &g_tracks[p.first_track];
}

/// FadeOutBody: one frame of a player's fade.
/// @param p  the player
void fade_step(Player& p)
{
   if (p.fade_oi == 0)
   {
      return;
   }
   if (--p.fade_oc != 0)
   {
      return;
   }
   p.fade_oc = p.fade_oi;
   Track* t = tracks_of(p);
   if ((p.fade_ov & kFadeIn) != 0)
   {
      p.fade_ov = static_cast<uint16_t>(p.fade_ov + kFadeStep);
      if (p.fade_ov >= kFadeFull)
      {
         p.fade_ov = kFadeFull;
         p.fade_oi = 0;
      }
   }
   else
   {
      p.fade_ov = static_cast<uint16_t>(p.fade_ov - kFadeStep);
      if (static_cast<int16_t>(p.fade_ov) <= 0)
      {
         // The end: every track stopped; a plain fade clears them (ended), a temporary one keeps them (paused).
         for (uint8_t i = 0; i < p.track_count; ++i)
         {
            track_stop(t[i]);
            if ((p.fade_ov & kFadeKeep) == 0)
            {
               t[i].flags = 0;
            }
         }
         if ((p.fade_ov & kFadeKeep) != 0)
         {
            p.status |= kPaused;
         }
         else
         {
            p.status = kPaused;
         }
         p.fade_oi = 0;
         return;
      }
   }
   for (uint8_t i = 0; i < p.track_count; ++i)
   {
      if ((t[i].flags & kTrackExist) != 0)
      {
         t[i].vol_x = static_cast<uint8_t>(p.fade_ov >> kFadeShift);
         t[i].flags |= kTrackVolChange;
      }
   }
}

/// A conditional VAR jump: taken, or the target skipped.
/// @param p     the player
/// @param t     the track
/// @param cond  the comparison's result
void var_jump(const Player& p, Track& t, bool cond)
{
   if (cond)
   {
      t.offset = read16(p.data, t.offset);
   }
   else
   {
      t.offset = static_cast<uint16_t>(t.offset + kAddressBytes);
   }
}

/// VAR op addr data [target]: the shared variable area.
/// @param p  the player
/// @param t  the track
void var_command(const Player& p, Track& t)
{
   const uint8_t op = arg(p, t);
   uint8_t& a = g_driver.var[arg(p, t) & kVarMask];
   const uint8_t data = arg(p, t);
   const uint8_t other = g_driver.var[data & kVarMask];
   switch (op)
   {
   case kVarSet: a = data; break;
   case kVarAdd: a = static_cast<uint8_t>(a + data); break;
   case kVarSub: a = static_cast<uint8_t>(a - data); break;
   case kVarCopy: a = other; break;
   case kVarAddVar: a = static_cast<uint8_t>(a + other); break;
   case kVarSubVar: a = static_cast<uint8_t>(a - other); break;
   case kVarEq: var_jump(p, t, a == data); break;
   case kVarNe: var_jump(p, t, a != data); break;
   case kVarGt: var_jump(p, t, a > data); break;
   case kVarGe: var_jump(p, t, a >= data); break;
   case kVarLe: var_jump(p, t, a <= data); break;
   case kVarLt: var_jump(p, t, a < data); break;
   case kVarEqVar: var_jump(p, t, a == other); break;
   case kVarNeVar: var_jump(p, t, a != other); break;
   case kVarGtVar: var_jump(p, t, a > other); break;
   case kVarGeVar: var_jump(p, t, a >= other); break;
   case kVarLeVar: var_jump(p, t, a <= other); break;
   case kVarLtVar: var_jump(p, t, a < other); break;
   default: break;   // unknown operation: nothing (the compiler refuses it)
   }
}

/// EXT sub args: the extended commands.
/// @param p  the player
/// @param t  the track
void ext_command(const Player& p, Track& t)
{
   const uint8_t sub = arg(p, t);
   // A key split's envelope bytes hold its keymap: the envelope commands leave it alone.
   const bool split = kind_of(t.voice.kind) == rom::VoiceKind::Split;
   switch (sub)
   {
   case kExtEchoVolume: t.echo_volume = arg(p, t); break;
   case kExtEchoLength: t.echo_length = arg(p, t); break;
   case kExtRelease:
   case kExtAttack:
   case kExtDecay:
   case kExtSustain:
   {
      const uint8_t v = arg(p, t);
      if (!split)
      {
         const uint8_t slot = sub == kExtRelease ? rom::kRelease
                              : sub == kExtAttack ? rom::kAttack
                              : sub == kExtDecay  ? rom::kDecay
                                                  : rom::kSustain;
         t.voice.env.adsr[slot] = v;
      }
      break;
   }
   case kExtHold:
   {
      // The hold re-runs itself (the position goes back to the EXT byte, a wait of 1) until its counter reaches n.
      const uint16_t n = read16(p.data, t.offset);
      if (t.hold < n)
      {
         ++t.hold;
         t.offset = static_cast<uint16_t>(t.offset - kAddressBytes);
         t.wait = 1;
      }
      else
      {
         t.hold = 0;
         t.offset = static_cast<uint16_t>(t.offset + kAddressBytes);
      }
      break;
   }
   case kExtStartOffset:
      t.start_offset = read32(p.data, t.offset);
      t.offset = static_cast<uint16_t>(t.offset + kStartOffsetBytes);
      break;
   case kExtToneLength: t.voice.tone_length = arg(p, t); break;
   case kExtSweep: t.voice.pan_sweep = arg(p, t); break;
   default: track_end(t); break;   // unknown: ends the track as the original's empty slots do
   }
}

/// One command of the non-note, non-wait range.
/// @param p    the player
/// @param t    the track
/// @param cmd  the command byte
void command(Player& p, Track& t, uint8_t cmd)
{
   switch (cmd)
   {
   case kCmdEnd: track_end(t); break;
   case kCmdJump: t.offset = read16(p.data, t.offset); break;
   case kCmdCall:
      if (t.call_depth < kMaxCallDepth)
      {
         t.stack[t.call_depth++] = static_cast<uint16_t>(t.offset + kAddressBytes);
         t.offset = read16(p.data, t.offset);
      }
      else
      {
         track_end(t);   // a fourth level acts as END
      }
      break;
   case kCmdReturn:
      if (t.call_depth != 0)
      {
         t.offset = t.stack[--t.call_depth];
      }
      break;
   case kCmdRepeat:
   {
      const uint8_t n = arg(p, t);
      if (n == 0)
      {
         t.offset = read16(p.data, t.offset);   // 0: always
         break;
      }
      ++t.repeat;
      if (t.repeat < n)
      {
         t.offset = read16(p.data, t.offset);
      }
      else
      {
         t.repeat = 0;
         t.offset = static_cast<uint16_t>(t.offset + kAddressBytes);
      }
      break;
   }
   case kCmdVar: var_command(p, t); break;
   case kCmdPriority: t.priority = arg(p, t); break;
   case kCmdTempo:
      p.tempo_d = static_cast<uint16_t>(arg(p, t) * kTempoPerValue);
      p.tempo_i = static_cast<uint16_t>((uint32_t(p.tempo_d) * p.tempo_u) >> kTempoScaleShift);
      break;
   case kCmdTranspose:
      t.key_shift = static_cast<int8_t>(arg(p, t));
      t.flags |= kTrackPitChange;
      break;
   case kCmdProgram:
      t.program = arg(p, t);
      t.voice = p.voices[t.program];
      break;
   case kCmdVolume:
      t.vol = arg(p, t);
      t.flags |= kTrackVolChange;
      break;
   case kCmdPan:
      t.pan = static_cast<int8_t>(arg(p, t) - kCentre);
      t.flags |= kTrackVolChange;
      break;
   case kCmdBend:
      t.bend = static_cast<int8_t>(arg(p, t) - kCentre);
      t.flags |= kTrackPitChange;
      break;
   case kCmdBendRange:
      t.bend_range = arg(p, t);
      t.flags |= kTrackPitChange;
      break;
   case kCmdLfoSpeed:
      t.lfo_speed = arg(p, t);
      if (t.lfo_speed == 0)
      {
         clear_mod(t);
      }
      break;
   case kCmdLfoDelay: t.lfo_delay = arg(p, t); break;
   case kCmdModDepth:
      t.mod = arg(p, t);
      if (t.mod == 0)
      {
         clear_mod(t);
      }
      break;
   case kCmdModType:
   {
      const uint8_t v = arg(p, t);
      if (v != t.mod_type)
      {
         t.mod_type = v;
         t.flags |= kTrackVolChange | kTrackPitChange;
      }
      break;
   }
   case kCmdFineTune:
      t.tune = static_cast<int8_t>(arg(p, t) - kCentre);
      t.flags |= kTrackPitChange;
      break;
   case kCmdExt: ext_command(p, t); break;
   case kCmdNoteOff: note_off(p, t); break;
   default: track_end(t); break;   // reserved bytes end the track
   }
}

/// The track's commands of a tick, until it waits or ends.
/// @param p  the player
/// @param t  the track
/// @return false when the track ended
bool run_commands(Player& p, Track& t)
{
   while (t.wait == 0)
   {
      if (t.cleared != 0)
      {
         track_end(t);   // clear_clips(): the sequence's bytes are gone
         return false;
      }
      const uint8_t b = p.data[t.offset];
      uint8_t cmd = t.running;
      if (b >= kCmdFirst)
      {
         ++t.offset;
         cmd = b;
         if (b <= kCmdNoteLast || b >= kCmdProgram)
         {
            t.running = b;   // the running set: NOTE, PROGRAM .. NOTE_OFF
         }
      }
      ++t.events;
      if (cmd < kCmdFirst)
      {
         track_end(t);   // an argument with no running command (the compiler refuses it)
         return false;
      }
      if (cmd <= kCmdNoteLast)
      {
         note_on(p, t, static_cast<uint8_t>(cmd - kCmdNote));
         continue;
      }
      if (cmd <= kCmdWaitLast)
      {
         t.wait = kLengths[cmd - kCmdWait];
         continue;
      }
      command(p, t, cmd);
      if (t.flags == 0)
      {
         return false;
      }
   }
   return true;
}

/// The LFO step after a tick's wait decrement.
/// @param t  the track
void lfo_step(Track& t)
{
   if (t.lfo_speed == 0 || t.mod == 0)
   {
      return;
   }
   if (t.lfo_delay_count != 0)
   {
      --t.lfo_delay_count;
      return;
   }
   t.lfo_phase = static_cast<uint8_t>(t.lfo_phase + t.lfo_speed);
   // A triangle: the phase as a signed byte below 64 and from 192, 128 - phase between.
   int32_t tri = static_cast<int8_t>(t.lfo_phase);
   if (static_cast<int8_t>(t.lfo_phase - kLfoQuarter) >= 0)
   {
      tri = kLfoHalf - t.lfo_phase;
   }
   const int32_t m = (int32_t(t.mod) * tri) >> kModShift;
   if (static_cast<int8_t>(m) != t.mod_m)
   {
      t.mod_m = static_cast<int8_t>(m);
      t.flags |= t.mod_type == kModTypePitch ? kTrackPitChange : kTrackVolChange;
   }
}

/// The START reset at a tick (the original clears the first 64 bytes, then sets these).
/// @param t  the track
void start_reset(Track& t)
{
   reset_track(t);
   t.flags = kTrackExist;
   t.bend_range = kStartBendRange;
   t.vol_x = kStartVolX;
   t.lfo_speed = kStartLfoSpeed;
   t.voice.kind = kTrackStartKind;
}

/// One tick of a player.
/// @param p  the player
/// @return false when no track was left (the player paused)
bool tick(Player& p)
{
   uint32_t mask = 0;
   uint32_t bit = 1;
   Track* tracks = tracks_of(p);
   for (uint8_t i = 0; i < p.track_count; ++i, bit <<= 1)
   {
      Track& t = tracks[i];
      if ((t.flags & kTrackExist) == 0)
      {
         continue;
      }
      mask |= bit;
      // Gates: a sounding channel's gate counts down to its release; a silent one leaves the list.
      for (uint8_t c = t.chan; c != kNoChannel;)
      {
         Channel& ch = g_channels[c];
         const uint8_t next = ch.next;
         if ((ch.status & kChanOn) != 0)
         {
            if (ch.gate != 0 && --ch.gate == 0)
            {
               ch.status |= kChanStop;
            }
         }
         else
         {
            unlink(c);
         }
         c = next;
      }
      if ((t.flags & kTrackStart) != 0)
      {
         start_reset(t);
      }
      if (run_commands(p, t))
      {
         --t.wait;
         lfo_step(t);
      }
   }
   ++p.clock;
   if (mask == 0)
   {
      p.status = kPaused;
      return false;
   }
   p.status = mask;
   return true;
}

/// The volume / pitch pass after a frame's ticks.
/// @param p  the player
void vol_pitch_pass(Player& p)
{
   Track* tracks = tracks_of(p);
   for (uint8_t i = 0; i < p.track_count; ++i)
   {
      Track& t = tracks[i];
      if ((t.flags & kTrackExist) == 0 || (t.flags & kTrackChanges) == 0)
      {
         continue;
      }
      track_vol_pitch(t);
      for (uint8_t c = t.chan; c != kNoChannel;)
      {
         Channel& ch = g_channels[c];
         const uint8_t next = ch.next;
         if ((ch.status & kChanOn) == 0)
         {
            unlink(c);
            c = next;
            continue;
         }
         const bool tone = is_tone(ch.kind);
         if ((t.flags & kTrackVolChange) != 0)
         {
            channel_volume(ch, t);
            if (tone)
            {
               ch.modify |= kModifyVolume;
            }
         }
         if ((t.flags & kTrackPitChange) != 0)
         {
            int32_t key = ch.key + t.key_m;
            if (key < 0)
            {
               key = 0;
            }
            if (tone)
            {
               ch.frequency = key_to_tone(tone_number(ch.kind), static_cast<uint8_t>(key), t.pit_m);
               ch.modify |= kModifyPitch;
            }
            else
            {
               ch.frequency = key_to_freq(ch.sample, static_cast<uint8_t>(key), t.pit_m);
            }
         }
         c = next;
      }
      t.flags &= kTrackKeep;
   }
}

}   // namespace

PlayerLock::PlayerLock(Player& p) : m_player(p)
{
   m_player.busy = 1;
   compiler_barrier();   // C14a (R10 M3): the call's stores stay after the lock is taken
}

PlayerLock::~PlayerLock()
{
   compiler_barrier();   // ... and before it is released
   m_player.busy = 0;
}

void reset_track(Track& t)
{
   const uint16_t offset = t.offset;
   uint16_t stack[kMaxCallDepth];
   for (uint8_t i = 0; i < kMaxCallDepth; ++i)
   {
      stack[i] = t.stack[i];
   }
   const uint8_t cleared = t.cleared;
   t = Track{};
   t.offset = offset;
   for (uint8_t i = 0; i < kMaxCallDepth; ++i)
   {
      t.stack[i] = stack[i];
   }
   t.cleared = cleared;
   t.chan = kNoChannel;
}

void clear_mod(Track& t)
{
   t.lfo_phase = 0;
   t.mod_m = 0;
   t.flags |= t.mod_type == kModTypePitch ? kTrackPitChange : kTrackVolChange;
}

void track_vol_pitch(Track& t)
{
   if ((t.flags & kTrackVolSet) != 0)
   {
      uint32_t x = (uint32_t(t.vol) * t.vol_x) >> kVolXShift;
      if (t.mod_type == kModTypeVolume)
      {
         x = (x * static_cast<uint32_t>(t.mod_m + kModBias)) >> kModVolShift;
      }
      int32_t y = kPanScale * t.pan + t.pan_x;
      if (t.mod_type == kModTypePan)
      {
         y += t.mod_m;
      }
      if (y < kPanLow)
      {
         y = kPanLow;
      }
      else if (y > kPanHigh)
      {
         y = kPanHigh;
      }
      t.vol_mr = static_cast<uint8_t>((static_cast<uint32_t>(y + kPanRightBias) * x) >> kSideShift);
      t.vol_ml = static_cast<uint8_t>((static_cast<uint32_t>(kPanLeftBias - y) * x) >> kSideShift);
   }
   if ((t.flags & kTrackPitSet) != 0)
   {
      const int32_t bend = t.bend * t.bend_range;
      int32_t x = (t.tune + bend) * kTuneScale + t.key_shift * kKeyScale + t.key_shift_x * kKeyScale + t.pit_x;
      if (t.mod_type == kModTypePitch)
      {
         x += kModPitchScale * t.mod_m;
      }
      t.key_m = static_cast<int8_t>(x >> kKeyShift);
      t.pit_m = static_cast<uint8_t>(x);
   }
   t.flags = static_cast<uint8_t>(t.flags & ~(kTrackPitSet | kTrackVolSet));
}

void player_main(Player& p)
{
   if ((p.status & kPaused) != 0)
   {
      return;
   }
   fade_step(p);
   if ((p.status & kPaused) != 0)
   {
      return;
   }
   // The accumulator is compared before its store truncates it, as the original does.
   uint32_t c = uint32_t(p.tempo_c) + p.tempo_i;
   for (;;)
   {
      p.tempo_c = static_cast<uint16_t>(c);
      if (c < kTempoTick)
      {
         break;
      }
      if (!tick(p))
      {
         return;
      }
      c = uint32_t(p.tempo_c) - kTempoTick;
   }
   vol_pitch_pass(p);
}

void start_song(Player& p, const rom::SongHeader* song, uint16_t sound)
{
   if (song == nullptr || p.busy != 0)
   {
      return;
   }
   Track* t = tracks_of(p);
   const bool idle = (p.song == nullptr || (t[0].flags & kTrackStart) == 0) &&
                     ((p.status & kTrackBits) == 0 || (p.status & kPaused) != 0);
   if (p.start_check != 0 && !idle && p.priority > song->priority)
   {
      return;   // the start check: a lower-priority song is refused
   }
   {
      const PlayerLock lock(p);
      p.status = 0;
      p.song = song;
      p.voices = song->voices;
      p.data = song->data;
      p.sound = sound;
      p.priority = song->priority;
      p.clock = 0;
      p.tempo_d = kTempoStart;
      p.tempo_i = kTempoStart;
      p.tempo_u = kTempoNormal;
      p.tempo_c = 0;
      p.fade_oi = 0;
      uint8_t i = 0;
      for (; i < song->track_count && i < p.track_count; ++i)
      {
         track_stop(t[i]);
         t[i].flags = kTrackExist | kTrackStart;
         t[i].chan = kNoChannel;
         t[i].offset = song->tracks[i];
         t[i].cleared = 0;
      }
      for (; i < p.track_count; ++i)
      {
         track_stop(t[i]);
         t[i].flags = 0;
         t[i].cleared = 0;
      }
      if ((song->reverb & rom::kReverbSet) != 0)
      {
         // The driver-wide reverb (sticky): the vertical blank step skips a frame that lands inside the change.
         g_driver.busy = 1;
         compiler_barrier();   // C14a (R10 M3): the reverb store stays inside the driver lock
         g_driver.reverb = static_cast<uint8_t>(song->reverb & ~rom::kReverbSet);
         compiler_barrier();
         g_driver.busy = 0;
      }
   }
}

void pause_player(Player& p)
{
   const PlayerLock lock(p);
   p.status |= kPaused;
   Track* t = tracks_of(p);
   for (uint8_t i = 0; i < p.track_count; ++i)
   {
      track_stop(t[i]);
   }
}

// --- the player API (capsule/audio/audio.h) --------------------------------------------------------------------

namespace
{

/// A game player, or nullptr past the table.
/// @param id  the player
/// @return it
Player* game_player(PlayerId id)
{
   return id < g_driver.player_count ? &g_players[id] : nullptr;
}

/// Applies a change to the existing tracks of a player selected by a bit mask.
/// @tparam F       void(Track&)
/// @param id       the player
/// @param tracks   bit per track
/// @param change   the change
template <typename F>
void each_track(PlayerId id, uint16_t tracks, F change)
{
   Player* p = game_player(id);
   if (p == nullptr)
   {
      return;
   }
   const PlayerLock lock(*p);
   Track* t = tracks_of(*p);
   uint32_t bit = 1;
   for (uint8_t i = 0; i < p->track_count; ++i, bit <<= 1)
   {
      if ((tracks & bit) != 0 && (t[i].flags & kTrackExist) != 0)
      {
         change(t[i]);
      }
   }
}

/// Starts a fade.
/// @param id     the player
/// @param speed  frames per step
/// @param ov     the starting volume and flags
/// @param unpause  clears the pause (fade in)
void begin_fade(PlayerId id, uint16_t speed, uint16_t ov, bool unpause)
{
   Player* p = game_player(id);
   if (p == nullptr)
   {
      return;
   }
   const PlayerLock lock(*p);
   p->fade_oc = speed;
   p->fade_oi = speed;
   p->fade_ov = ov;
   if (unpause)
   {
      p->status &= ~kPaused;
   }
}

}   // namespace

}   // namespace capsule::audio::detail

namespace capsule::audio
{

using namespace detail;

void play(SoundId id)
{
   if (id >= rom::kTable.song_count)
   {
      return;
   }
   const rom::SongEntry& e = rom::kTable.songs[id];
   if (e.player < g_driver.player_count)
   {
      start_song(g_players[e.player], e.song, id);
   }
}

void play_or_keep(SoundId id)
{
   if (id >= rom::kTable.song_count)
   {
      return;
   }
   const rom::SongEntry& e = rom::kTable.songs[id];
   if (e.player >= g_driver.player_count)
   {
      return;
   }
   Player& p = g_players[e.player];
   if (p.song != e.song || (p.status & kTrackBits) == 0 || (p.status & kPaused) != 0)
   {
      start_song(p, e.song, id);
   }
}

void stop_song(SoundId id)
{
   if (id >= rom::kTable.song_count)
   {
      return;
   }
   const rom::SongEntry& e = rom::kTable.songs[id];
   if (e.player < g_driver.player_count && g_players[e.player].song == e.song)
   {
      pause_player(g_players[e.player]);
   }
}

void stop_all()
{
   for (uint8_t i = 0; i < g_driver.player_count; ++i)
   {
      pause_player(g_players[i]);
   }
   for (uint8_t i = 0; i < kClipPlayers; ++i)
   {
      pause_player(g_players[kMaxPlayers + i]);
   }
}

void resume_all()
{
   for (uint8_t i = 0; i < g_driver.player_count; ++i)
   {
      resume(i);
   }
   for (uint8_t i = 0; i < kClipPlayers; ++i)
   {
      Player& p = g_players[kMaxPlayers + i];
      const PlayerLock lock(p);
      p.status &= ~kPaused;
   }
}

void pause(PlayerId id)
{
   if (Player* p = game_player(id))
   {
      pause_player(*p);
   }
}

void resume(PlayerId id)
{
   if (Player* p = game_player(id))
   {
      const PlayerLock lock(*p);
      p->status &= ~kPaused;
   }
}

void fade_out(PlayerId p, uint16_t frames_per_step)
{
   begin_fade(p, frames_per_step, kFadeFull, false);
}

void fade_out_paused(PlayerId p, uint16_t frames_per_step)
{
   begin_fade(p, frames_per_step, kFadeFull | kFadeKeep, false);
}

void fade_in(PlayerId p, uint16_t frames_per_step)
{
   begin_fade(p, frames_per_step, kFadeIn, true);
}

void apply_start(PlayerId id)
{
   Player* p = game_player(id);
   if (p == nullptr)
   {
      return;
   }
   Track* t = tracks_of(*p);
   for (uint8_t i = 0; i < p->track_count; ++i)
   {
      if ((t[i].flags & kTrackExist) != 0 && (t[i].flags & kTrackStart) != 0)
      {
         start_reset(t[i]);
      }
   }
}

void set_volume(PlayerId p, uint16_t tracks, uint16_t volume)
{
   each_track(p, tracks, [volume](Track& t)
   {
      t.vol_x = static_cast<uint8_t>(volume / kVolumeQuarter);
      t.flags |= kTrackVolChange;
   });
}

void set_pitch(PlayerId p, uint16_t tracks, int16_t pitch)
{
   each_track(p, tracks, [pitch](Track& t)
   {
      t.key_shift_x = static_cast<int8_t>(pitch >> kKeyShift);
      t.pit_x = static_cast<uint8_t>(pitch);
      t.flags |= kTrackPitChange;
   });
}

void set_pan(PlayerId p, uint16_t tracks, int8_t pan)
{
   each_track(p, tracks, [pan](Track& t)
   {
      t.pan_x = pan;
      t.flags |= kTrackVolChange;
   });
}

void set_tempo_scale(PlayerId id, uint16_t scale)
{
   if (Player* p = game_player(id))
   {
      const PlayerLock lock(*p);
      p->tempo_u = scale;
      p->tempo_i = static_cast<uint16_t>((uint32_t(p->tempo_d) * p->tempo_u) >> kTempoScaleShift);
   }
}

void set_mod_depth(PlayerId p, uint16_t tracks, uint8_t depth)
{
   each_track(p, tracks, [depth](Track& t)
   {
      t.mod = depth;
      if (t.mod == 0)
      {
         clear_mod(t);
      }
   });
}

void set_lfo_speed(PlayerId p, uint16_t tracks, uint8_t speed)
{
   each_track(p, tracks, [speed](Track& t)
   {
      t.lfo_speed = speed;
      if (t.lfo_speed == 0)
      {
         clear_mod(t);
      }
   });
}

PlayerStatus status(PlayerId id)
{
   const Player* p = game_player(id);
   if (p == nullptr)
   {
      return {0, true};
   }
   return {static_cast<uint16_t>(p->status & kTrackBits), (p->status & kPaused) != 0};
}

const rom::SongHeader* current_song(PlayerId id)
{
   const Player* p = game_player(id);
   return p != nullptr ? p->song : nullptr;
}

bool playing(PlayerId p)
{
   const PlayerStatus s = status(p);
   return !s.paused && s.tracks != 0;
}

bool ended(PlayerId p)
{
   return status(p).tracks == 0;
}

bool paused_or_ended(PlayerId p)
{
   const PlayerStatus s = status(p);
   return s.paused || s.tracks == 0;
}

uint8_t player_count()
{
   return g_driver.player_count;
}

uint16_t sound_count()
{
   return rom::kTable.song_count;
}

PlayerId player_of(SoundId id)
{
   return id < rom::kTable.song_count ? rom::kTable.songs[id].player : kNoPlayer;
}

SoundId find_sound(AssetKey key)
{
   if (rom::kTable.keys == nullptr || raw(key) == 0)
   {
      return kNoSound;
   }
   for (uint16_t i = 0; i < rom::kTable.song_count; ++i)
   {
      if (rom::kTable.keys[i] == raw(key))
      {
         return i;
      }
   }
   return kNoSound;
}

uint8_t var(uint8_t index)
{
   return g_driver.var[index & kVarMask];
}

void set_var(uint8_t index, uint8_t value)
{
   g_driver.var[index & kVarMask] = value;
}

}   // namespace capsule::audio
