/// @file
/// The audio tour (audio_tour.h).
#include "audio_tour.h"

#include "capsule/audio/audio.h"
#include "capsule/audio/rom.h"

namespace capsule::audio_tour
{

namespace
{

/// Frames a sound must have played before ended() counts (a call takes effect at the next vertical blank).
constexpr uint16_t kSettleFrames = 2;

/// @name The controls part's timeline (frames from its start)
///@{
constexpr uint16_t kAtVolume = 30;          ///< Half volume.
constexpr uint16_t kAtPitch = 45;           ///< One semitone up.
constexpr uint16_t kAtPan = 60;             ///< Pan left.
constexpr uint16_t kAtTempo = 75;           ///< Double tempo.
constexpr uint16_t kAtNormal = 90;          ///< Everything back.
constexpr uint16_t kAtPause = 100;          ///< Pause.
constexpr uint16_t kAtResume = 115;         ///< Resume.
constexpr uint16_t kAtMono = 130;           ///< Mono.
constexpr uint16_t kAtStereo = 160;         ///< Stereo.
constexpr uint16_t kAtLag = 170;            ///< The modelled lag frame.
constexpr uint16_t kAtFadePaused = 180;     ///< Fade out, keep the tracks (speed 1: 16 frames).
constexpr uint16_t kAtFadeIn = 200;         ///< Fade in (speed 2: 32 frames).
constexpr uint16_t kAtFadeOut = 240;        ///< Fade out to the end (speed 2).
constexpr uint16_t kAtReplay = 280;         ///< Play again.
constexpr uint16_t kAtStopSong = 300;       ///< stop_song().
constexpr uint16_t kAtReplay2 = 310;        ///< Play again.
constexpr uint16_t kAtStopAll = 320;        ///< stop_all().
constexpr uint16_t kAtResumeAll = 330;      ///< resume_all().
constexpr uint16_t kAtStopAll2 = 345;       ///< stop_all().
constexpr uint16_t kControlsEnd = 350;      ///< The part ends.
///@}

/// @name Control values
///@{
constexpr uint16_t kHalfVolume = 128;               ///< set_volume(): 0.5.
constexpr int16_t kSemitoneUp = 256;                ///< set_pitch(): +1 semitone.
constexpr int8_t kPanLeft = -32;                    ///< set_pan().
constexpr uint16_t kDoubleTempo = 512;              ///< set_tempo_scale(): 2.0.
constexpr uint16_t kFadeFast = 1;                   ///< Frames per fade step.
constexpr uint16_t kFadeSlow = 2;                   ///< Frames per fade step.
///@}

/// @name Clip variants
///@{
constexpr uint8_t kClipVariants = 3;                                ///< Variants per clip voice.
constexpr uint8_t kChorus[kClipVariants] = {0, 20, 192};            ///< Chorus per variant (192: one semitone down).
constexpr int16_t kBasePitch = 60 * 256;                            ///< Key 60: the voice's own pitch.
constexpr int16_t kPitchStep = 2 * 256;                             ///< Two semitones up per variant.
constexpr uint8_t kClipVolume = 120;                                ///< Clip volume.
constexpr uint16_t kClipHold = 60;                                  ///< Ticks before the release.
constexpr uint8_t kClipRelease = 200;                               ///< Release.
constexpr uint8_t kClipPriority = 10;                               ///< Priority.
constexpr uint8_t kPairClips = 3;                                   ///< Clips started for the "two at once" step.
///@}

/// Starts the next item of a part.
/// @param t  the tour
void next_item(Tour& t)
{
   ++t.index;
   t.wait = 0;
}

/// Parameters of one clip variant.
/// @param clip     the clip voice
/// @param variant  0..kClipVariants - 1
/// @return the parameters
audio::ClipParams clip_params(uint16_t clip, uint8_t variant)
{
   audio::ClipParams c{};
   c.clip = clip;
   c.volume = kClipVolume;
   c.pan = 0;
   c.pitch = static_cast<int16_t>(kBasePitch + variant * kPitchStep);
   c.hold_ticks = kClipHold;
   c.release = kClipRelease;
   c.chorus = kChorus[variant];
   c.priority = kClipPriority;
   c.start_offset = 0;
   return c;
}

/// One frame of the songs part.
/// @param t  the tour
void songs_frame(Tour& t)
{
   const uint16_t count = audio::sound_count();
   if (t.index >= count)
   {
      t.phase = Phase::Controls;
      t.index = 0;
      t.wait = 0;
      return;
   }
   const audio::PlayerId p = audio::player_of(t.index);
   if (p == audio::kNoPlayer)
   {
      next_item(t);
      return;
   }
   if (t.wait == 0)
   {
      audio::play(t.index);
      ++t.sounds;
   }
   else if (t.wait >= kSongFrames + kGapFrames)
   {
      next_item(t);
      return;
   }
   else if (t.wait < kSongFrames && ((t.wait >= kSettleFrames && audio::ended(p)) || t.wait == kSongFrames - 1))
   {   // ended (or played its most): note the length, stop, then the gap
      if (t.wait > t.longest_frames)
      {
         t.longest_frames = t.wait;
         t.longest_sound = t.index;
      }
      audio::stop_all();
      t.wait = kSongFrames;
   }
   ++t.wait;
}

/// One frame of the controls part.
/// @param t  the tour
/// @return the frame's vertical blanks
Blanks controls_frame(Tour& t)
{
   const uint16_t s = t.longest_sound;
   const audio::PlayerId p = audio::player_of(s);
   const uint16_t at = t.wait++;
   if (p == audio::kNoPlayer || at >= kControlsEnd)
   {
      t.phase = Phase::Clips;
      t.index = 0;
      t.wait = 0;
      return Blanks::One;
   }
   switch (at)
   {
   case 0:
   case kAtReplay:
   case kAtReplay2:
      audio::play(s);
      break;
   case kAtVolume:
      audio::set_volume(p, audio::kAllTracks, kHalfVolume);
      break;
   case kAtPitch:
      audio::set_pitch(p, audio::kAllTracks, kSemitoneUp);
      break;
   case kAtPan:
      audio::set_pan(p, audio::kAllTracks, kPanLeft);
      break;
   case kAtTempo:
      audio::set_tempo_scale(p, kDoubleTempo);
      break;
   case kAtNormal:
      audio::set_tempo_scale(p, audio::kTempoNormal);
      audio::set_volume(p, audio::kAllTracks, audio::kVolumeFull);
      audio::set_pitch(p, audio::kAllTracks, 0);
      audio::set_pan(p, audio::kAllTracks, 0);
      break;
   case kAtPause:
      audio::pause(p);
      break;
   case kAtResume:
      audio::resume(p);
      break;
   case kAtMono:
      audio::set_output(audio::Output::Mono);
      break;
   case kAtStereo:
      audio::set_output(audio::Output::Stereo);
      break;
   case kAtLag:
      return Blanks::Lag;
   case kAtFadePaused:
      audio::fade_out_paused(p, kFadeFast);
      break;
   case kAtFadeIn:
      audio::fade_in(p, kFadeSlow);
      break;
   case kAtFadeOut:
      audio::fade_out(p, kFadeSlow);
      break;
   case kAtStopSong:
      audio::stop_song(s);
      break;
   case kAtStopAll:
   case kAtStopAll2:
      audio::stop_all();
      break;
   case kAtResumeAll:
      audio::resume_all();
      break;
   default:
      break;
   }
   return Blanks::One;
}

/// One frame of the clips part: each voice's variants one after another, then kPairClips started on consecutive
/// frames (the third replaces the oldest).
/// @param t  the tour
void clips_frame(Tour& t)
{
   const uint16_t voices = audio::rom::kTable.clip_count < kTourClips ? audio::rom::kTable.clip_count : kTourClips;
   const uint16_t singles = static_cast<uint16_t>(voices * kClipVariants);
   if (voices == 0 || t.index > singles)
   {
      audio::clear_clips();
      t.phase = Phase::Done;
      return;
   }
   const uint16_t at = t.wait++;
   if (t.index < singles)
   {
      const auto voice = static_cast<uint16_t>(t.index / kClipVariants);
      const auto variant = static_cast<uint8_t>(t.index % kClipVariants);
      if (at == 0)
      {
         t.clip = audio::play_clip(clip_params(voice, variant));
         ++t.clips;
      }
      else if ((at >= kSettleFrames && !audio::clip_playing(t.clip)) || at >= kClipFrames)
      {
         audio::stop_clip(t.clip);
         next_item(t);
      }
      return;
   }
   // the pair: clips on consecutive frames, then wait for both players to end
   if (at < kPairClips)
   {
      audio::play_clip(clip_params(static_cast<uint16_t>(at % voices), 0));
      ++t.clips;
   }
   else if ((!audio::clip_playing(0) && !audio::clip_playing(1)) || at >= kClipFrames)
   {
      next_item(t);
   }
}

} // namespace

void start(Tour& t)
{
   t = Tour{};
   t.phase = Phase::Songs;
}

Blanks frame(Tour& t)
{
   Blanks blanks = Blanks::One;
   switch (t.phase)
   {
   case Phase::Songs:
      songs_frame(t);
      break;
   case Phase::Controls:
      blanks = controls_frame(t);
      break;
   case Phase::Clips:
      clips_frame(t);
      break;
   case Phase::Done:
      return Blanks::None;
   }
   ++t.frame;
   return blanks;
}

const char* phase_name(Phase p)
{
   switch (p)
   {
   case Phase::Songs:
      return "songs";
   case Phase::Controls:
      return "controls";
   case Phase::Clips:
      return "clips";
   case Phase::Done:
      return "done";
   }
   return "?";
}

} // namespace capsule::audio_tour
