/// @file
/// Clips: one-shot sample cues on the two clip players (D-34, M4 B1a; inventory section 4.8; format section 5).
///
/// play_clip() writes a two-track sequence of format section 3 commands into the clip player's RAM song and starts
/// it (clip players have no start check and keep the starting tempo: one tick a frame):
///
///     track 0: FINE_TUNE tune, JUMP body
///     track 1: FINE_TUNE tune2                       (played only with chorus)
///     body:    PROGRAM 0, VOLUME v, EXT start offset, EXT release, PAN p, NOTE 0 key 127, EXT hold n, NOTE_OFF, END
///
/// The pitch p gives key = (p + 128) >> 8 and tune = ((p + 128) >> 1) & 0x7F; chorus c tunes the second track
/// (c + tune) & 0x7F (a 7-bit wrap: 192 is a semitone down). The song's instrument set is the clip's voice.
#include "audio_internal.h"

#include "capsule/core/memory.h"

namespace capsule::audio::detail
{

CAPSULE_EWRAM_BSS ClipSong g_clip_songs[kClipPlayers];

namespace
{

inline constexpr int32_t kPitchRound = 0x80;      ///< pitch + 128.
inline constexpr uint8_t kPitchKeyShift = 8;      ///< key = b >> 8.
inline constexpr uint8_t kSevenBits = 0x7F;       ///< Argument bytes.
inline constexpr uint8_t kClipVelocity = 127;     ///< The note's velocity.
inline constexpr uint8_t kByteShift = 8;          ///< Little-endian writes.
inline constexpr uint8_t kStartOffsetBytes = 4;   ///< EXT 4's u32.
inline constexpr uint8_t kAddressBytes = 2;       ///< A u16.

/// Writes a clip sequence.
struct Writer
{
   uint8_t* data;   ///< The song's bytes.
   uint16_t at;     ///< Next byte.

   /// One byte.
   /// @param b  the byte
   void put(uint32_t b)
   {
      data[at++] = static_cast<uint8_t>(b);
   }

   /// A little-endian value.
   /// @param v      the value
   /// @param bytes  its size
   void put_le(uint32_t v, uint8_t bytes)
   {
      for (uint8_t i = 0; i < bytes; ++i, v >>= kByteShift)
      {
         put(v);
      }
   }
};

/// The clip player of a handle.
/// @param h  0 or 1
/// @return it
Player& clip_player(ClipHandle h)
{
   return g_players[kMaxPlayers + (h % kClipPlayers)];
}

/// The first track of a clip player.
/// @param p  the clip player
/// @return its track 0
Track& first_track(const Player& p)
{
   return g_tracks[p.first_track];
}

/// Whether a track still has a channel linked to it.
/// @param t  the track
/// @return true while its list's head names it
bool has_channel(const Track& t)
{
   return t.chan != kNoChannel && g_channels[t.chan].track == rank_of(t);
}

}   // namespace

}   // namespace capsule::audio::detail

namespace capsule::audio
{

using namespace detail;

ClipHandle play_clip(const ClipParams& c)
{
   if (c.clip >= rom::kTable.clip_count)
   {
      return 0;
   }
   // A free clip player (track 0 ended and without a channel), else the one with the largest clock.
   uint32_t max_clock = 0;
   ClipHandle h = 0;
   for (ClipHandle i = 0; i < kClipPlayers; ++i)
   {
      const Player& p = clip_player(i);
      const Track& t = first_track(p);
      if (t.flags == 0 && !has_channel(t))
      {
         h = i;
         break;
      }
      if (max_clock < p.clock)
      {
         max_clock = p.clock;
         h = i;
      }
   }
   Player& p = clip_player(h);
   ClipSong& s = g_clip_songs[h];
   {
      const PlayerLock lock(p);
      const int16_t b = static_cast<int16_t>(c.pitch + kPitchRound);
      const uint8_t key = static_cast<uint8_t>((b >> kPitchKeyShift) & kSevenBits);
      const uint8_t tune = static_cast<uint8_t>((b >> 1) & kSevenBits);
      const uint8_t tune2 = static_cast<uint8_t>((c.chorus + tune) & kSevenBits);
      Writer w{s.data, 0};
      // track 0
      w.put(kCmdFineTune);
      w.put(tune);
      w.put(kCmdJump);
      const uint16_t jump_at = w.at;
      w.put_le(0, kAddressBytes);
      // track 1
      const uint16_t second = w.at;
      w.put(kCmdFineTune);
      w.put(tune2);
      // the body both tracks play
      const uint16_t body = w.at;
      w.put(kCmdProgram);
      w.put(0);
      w.put(kCmdVolume);
      w.put(c.volume & kSevenBits);
      w.put(kCmdExt);
      w.put(kExtStartOffset);
      w.put_le(c.start_offset, kStartOffsetBytes);
      w.put(kCmdExt);
      w.put(kExtRelease);
      w.put(c.release);
      w.put(kCmdPan);
      w.put(static_cast<uint32_t>(c.pan + kCentre) & kSevenBits);
      w.put(kCmdNote);
      w.put(key);
      w.put(kClipVelocity);
      w.put(kCmdExt);
      w.put(kExtHold);
      w.put_le(c.hold_ticks, kAddressBytes);
      w.put(kCmdNoteOff);
      w.put(kCmdEnd);
      Writer patch{s.data, jump_at};
      patch.put_le(body, kAddressBytes);
      s.tracks[0] = 0;
      s.tracks[1] = second;
      s.header.track_count = c.chorus != 0 ? kClipTracks : 1;
      s.header.priority = c.priority;
      s.header.reverb = 0;
      s.header.reserved = 0;
      s.header.voices = &rom::kTable.clips[c.clip];
      s.header.data = s.data;
      s.header.tracks = s.tracks;
   }
   start_song(p, &s.header, c.clip);
   return h;
}

bool clip_playing(ClipHandle h)
{
   return has_channel(first_track(clip_player(h)));
}

void stop_clip(ClipHandle h)
{
   pause_player(clip_player(h));
}

void clear_clips()
{
   for (ClipHandle h = 0; h < kClipPlayers; ++h)
   {
      const Player& p = clip_player(h);
      for (uint8_t i = 0; i < p.track_count; ++i)
      {
         g_tracks[p.first_track + i].cleared = 1;
      }
   }
}

}   // namespace capsule::audio
