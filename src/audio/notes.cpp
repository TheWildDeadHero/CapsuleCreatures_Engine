/// @file
/// Notes and channels (D-34, M4 B1a; inventory sections 3, 4.3, 4.4): NOTE with its optional arguments, the voice
/// it resolves to (key splits, drum sets), channel allocation with priorities and rank ties, the channel's start,
/// NOTE_OFF, END and the stop of a track, the track channel lists, and the key -> frequency arithmetic of both kinds
/// of channel.
///
/// Allocation (sampled): scan the sampled channels in order; a silent one is taken at once. Otherwise releasing
/// channels win: the first releasing one becomes the candidate whatever its priority, a later releasing one replaces
/// it with a lower priority, or an equal one and a rank at least the candidate's. With none releasing, a sounding
/// channel replaces the best so far (initially the new note's own priority and track) the same way. No candidate:
/// the note is dropped. Tone notes use their kind's fixed channel when it is silent or releasing, of lower priority,
/// or of equal priority and a rank at least the new note's track's.
#include "audio_internal.h"

#include "capsule/core/memory.h"

namespace capsule::audio::detail
{
namespace
{

/// @name Arithmetic of the reference driver
///@{
inline constexpr uint32_t kMaxPriority = 255;        ///< Song + track priority cap.
inline constexpr int32_t kRhythmPanScale = 2;        ///< Drum pan: (pan_sweep - 0xC0) x 2.
inline constexpr int32_t kRightPanBias = 128;        ///< Right: velocity x (128 + rhythm pan).
inline constexpr int32_t kLeftPanBias = 127;         ///< Left: velocity x (127 - rhythm pan).
inline constexpr uint32_t kChannelVolumeShift = 14;  ///< ... x track side >> 14.
inline constexpr uint32_t kChannelVolumeMax = 255;   ///< Channel volume cap.
inline constexpr uint8_t kMaxSampleKey = 178;        ///< Sampled keys clamp here (fine 255).
inline constexpr uint8_t kMaxFine = 255;             ///< The fine part at a clamp.
inline constexpr uint32_t kFineShift = 24;           ///< fine << 24: the interpolation fraction.
inline constexpr uint8_t kTopOctaveShift = 14;       ///< freq(k) = F[k % 12] >> (14 - k / 12).
inline constexpr uint32_t kHighWordShift = 32;       ///< The upper word of a 32 x 32 product.
inline constexpr uint8_t kNoiseNumber = 4;           ///< Tone channel 4: noise.
inline constexpr uint8_t kNoiseLowKey = 20;          ///< Noise keys up to 20 read entry 0.
inline constexpr uint8_t kNoiseKeyBase = 21;         ///< Noise entry = key - 21.
inline constexpr uint8_t kNoiseLastEntry = 59;       ///< Noise entries clamp here.
inline constexpr uint8_t kToneLowKey = 35;           ///< Tone keys up to 35 read key 0, fine 0.
inline constexpr uint8_t kToneKeyBase = 36;          ///< Tone entry = key - 36.
inline constexpr uint8_t kToneLastKey = 130;         ///< Tone keys clamp here (fine 255).
inline constexpr uint32_t kToneFineShift = 8;        ///< fine x (C(k + 1) - C(k)) >> 8.
inline constexpr int32_t kToneRegisterBase = 2048;   ///< The register value's +2048.
///@}

/// @name Key / 12 without a division (C14a, R10 M4: the ARM7TDMI has none; the Thumb build called __aeabi_uidiv)
///@{
inline constexpr uint32_t kDiv12Multiplier = 0xAB;   ///< k / 12 = (k x 171) >> 11 ...
inline constexpr uint32_t kDiv12Shift = 11;          ///< ... for every key a byte holds (checked below).

/// A key's octave, k / 12, as a multiply and a shift.
/// @param k  a key, 0-255
/// @return k / 12
constexpr uint8_t octave_of(uint8_t k)
{
   return static_cast<uint8_t>((uint32_t{k} * kDiv12Multiplier) >> kDiv12Shift);
}

/// Whether octave_of() equals the division for every byte.
/// @return true when it does
constexpr bool octave_of_exact()
{
   for (uint32_t k = 0; k <= UINT8_MAX; ++k)
   {
      if (octave_of(static_cast<uint8_t>(k)) != k / kNotes)
      {
         return false;
      }
   }
   return true;
}
static_assert(octave_of_exact(), "(k x 171) >> 11 is k / 12 for every key");

/// A key's note within its octave, k % 12.
/// @param k       a key, 0-255
/// @param octave  octave_of(k)
/// @return k - 12 x octave
constexpr uint8_t note_of(uint8_t k, uint8_t octave)
{
   return static_cast<uint8_t>(k - octave * kNotes);
}
///@}

/// The sampled channel's rate from its two octave values. Two 32 x 32 -> 64-bit high words: on the GBA this is ARM
/// code (CAPSULE_ARM_CODE), so each is one umull instead of a call to __aeabi_lmul (the reference driver's umull
/// helper does the same).
/// @param pitch     the sample's pitch
/// @param v1        freq(k)
/// @param v2        freq(k + 1)
/// @param fraction  the fine part << 24
/// @return high(pitch x (v1 + high((v2 - v1) x fraction)))
CAPSULE_ARM_CODE uint32_t scale_freq(uint32_t pitch, uint32_t v1, uint32_t v2, uint32_t fraction)
{
   const uint32_t step = static_cast<uint32_t>((uint64_t{v2 - v1} * fraction) >> kHighWordShift);
   return static_cast<uint32_t>((uint64_t{pitch} * (v1 + step)) >> kHighWordShift);
}

/// freq(k) of the sampled channels' table.
/// @param k  0..179
/// @return the octave value shifted to the key's octave
uint32_t octave_value(uint8_t k)
{
   const uint8_t octave = octave_of(k);
   return kOctave[note_of(k, octave)] >> (kTopOctaveShift - octave);
}

/// C(k) of the tone channels' table.
/// @param k  0..131
/// @return the (negative) period value
int32_t tone_value(uint8_t k)
{
   const uint8_t octave = octave_of(k);
   return int32_t(kToneOctave[note_of(k, octave)]) >> octave;
}

/// Takes the note's sampled channel.
/// @param prio  the note's priority
/// @param rank  its track's rank
/// @return the channel, or kNoChannel (the note is dropped)
uint8_t allocate_sampled(uint32_t prio, uint8_t rank)
{
   bool releasing = false;
   uint32_t best_prio = prio;
   uint8_t best_rank = rank;
   uint8_t found = kNoChannel;
   for (uint8_t i = 0; i < g_driver.channels; ++i)
   {
      const Channel& ch = g_channels[i];
      if ((ch.status & kChanOn) == 0)
      {
         return i;
      }
      if ((ch.status & kChanStop) != 0)
      {
         if (!releasing)
         {
            // The first releasing channel: the candidate, whatever its priority.
            releasing = true;
            best_prio = ch.priority;
            best_rank = ch.track;
            found = i;
            continue;
         }
      }
      else if (releasing)
      {
         continue;   // once one releases, sounding ones are not candidates
      }
      if (ch.priority < best_prio)
      {
         best_prio = ch.priority;
         best_rank = ch.track;
         found = i;
      }
      else if (ch.priority == best_prio && ch.track >= best_rank)
      {
         best_rank = ch.track;
         found = i;
      }
   }
   return found;
}

/// Takes the note's tone channel.
/// @param number  1-4
/// @param prio    the note's priority
/// @param rank    its track's rank
/// @return the channel, or kNoChannel
uint8_t allocate_tone(uint8_t number, uint32_t prio, uint8_t rank)
{
   const uint8_t c = static_cast<uint8_t>(kFirstTone + number - 1);
   const Channel& ch = g_channels[c];
   if ((ch.status & kChanOn) == 0 || (ch.status & kChanStop) != 0 || ch.priority < prio)
   {
      return c;
   }
   if (ch.priority == prio && ch.track >= rank)
   {
      return c;
   }
   return kNoChannel;
}

}   // namespace

void unlink(uint8_t c)
{
   Channel& ch = g_channels[c];
   if (ch.track == 0)
   {
      return;
   }
   // The channel keeps its own prev / next (stale), as the original's do.
   if (ch.prev != kNoChannel)
   {
      g_channels[ch.prev].next = ch.next;
   }
   else
   {
      track_of(ch.track).chan = ch.next;
   }
   if (ch.next != kNoChannel)
   {
      g_channels[ch.next].prev = ch.prev;
   }
   ch.track = 0;
}

void track_end(Track& t)
{
   for (uint8_t c = t.chan; c != kNoChannel;)
   {
      Channel& ch = g_channels[c];
      if ((ch.status & kChanOn) != 0)
      {
         ch.status |= kChanStop;
      }
      unlink(c);
      c = ch.next;
   }
   t.flags = 0;
}

void track_stop(Track& t)
{
   if ((t.flags & kTrackExist) == 0)
   {
      return;
   }
   for (uint8_t c = t.chan; c != kNoChannel;)
   {
      Channel& ch = g_channels[c];
      if (ch.status != 0)
      {
         if (is_tone(ch.kind))
         {
            tone_off(tone_number(ch.kind));
         }
         ch.status = 0;
      }
      ch.track = 0;
      c = ch.next;
   }
   t.chan = kNoChannel;
}

void channel_volume(Channel& ch, const Track& t)
{
   const int32_t rp = ch.rhythm_pan;
   const uint32_t right = static_cast<uint32_t>((ch.velocity * (kRightPanBias + rp) * t.vol_mr) >> kChannelVolumeShift);
   const uint32_t left = static_cast<uint32_t>((ch.velocity * (kLeftPanBias - rp) * t.vol_ml) >> kChannelVolumeShift);
   ch.right = static_cast<uint8_t>(right > kChannelVolumeMax ? kChannelVolumeMax : right);
   ch.left = static_cast<uint8_t>(left > kChannelVolumeMax ? kChannelVolumeMax : left);
}

uint32_t key_to_freq(const rom::SampleHeader* sample, uint8_t key, uint8_t fine)
{
   uint32_t fraction = uint32_t(fine) << kFineShift;
   if (key > kMaxSampleKey)
   {
      key = kMaxSampleKey;
      fraction = uint32_t(kMaxFine) << kFineShift;
   }
   const uint32_t v1 = octave_value(key);
   const uint32_t v2 = octave_value(static_cast<uint8_t>(key + 1));
   return scale_freq(sample->pitch, v1, v2, fraction);
}

uint32_t key_to_tone(uint8_t number, uint8_t key, uint8_t fine)
{
   if (number == kNoiseNumber)
   {
      if (key <= kNoiseLowKey)
      {
         key = 0;
      }
      else
      {
         key = static_cast<uint8_t>(key - kNoiseKeyBase);
         if (key > kNoiseLastEntry)
         {
            key = kNoiseLastEntry;
         }
      }
      return kNoise[key];
   }
   if (key <= kToneLowKey)
   {
      fine = 0;
      key = 0;
   }
   else
   {
      key = static_cast<uint8_t>(key - kToneKeyBase);
      if (key > kToneLastKey)
      {
         key = kToneLastKey;
         fine = kMaxFine;
      }
   }
   const int32_t v1 = tone_value(key);
   const int32_t v2 = tone_value(static_cast<uint8_t>(key + 1));
   return static_cast<uint32_t>(v1 + ((fine * (v2 - v1)) >> kToneFineShift) + kToneRegisterBase);
}

void note_on(const Player& p, Track& t, uint8_t index)
{
   // The optional key, velocity and gate extra (each an argument byte < 0x80).
   t.gate = kLengths[index];
   if (p.data[t.offset] < kCmdFirst)
   {
      t.key = p.data[t.offset++];
      if (p.data[t.offset] < kCmdFirst)
      {
         t.velocity = p.data[t.offset++];
         if (p.data[t.offset] < kCmdFirst)
         {
            t.gate = static_cast<uint8_t>(t.gate + p.data[t.offset++]);
         }
      }
   }

   // The voice: a key split or drum set entry, played at the track's key (split) or the entry's (drums).
   int8_t rhythm_pan = 0;
   const rom::Voice* v = &t.voice;
   uint8_t key = t.key;
   const rom::VoiceKind kind = kind_of(v->kind);
   if (kind == rom::VoiceKind::Split || kind == rom::VoiceKind::Drums)
   {
      const uint8_t entry = kind == rom::VoiceKind::Split ? v->env.keymap[t.key] : t.key;
      const rom::Voice* sub = &v->ref.set[entry];
      const rom::VoiceKind sub_kind = kind_of(sub->kind);
      if (sub_kind == rom::VoiceKind::Split || sub_kind == rom::VoiceKind::Drums)
      {
         return;   // a split or drum set inside one drops the note
      }
      if (kind == rom::VoiceKind::Drums)
      {
         if ((sub->pan_sweep & kPanSweepPan) != 0)
         {
            rhythm_pan = static_cast<int8_t>((sub->pan_sweep - kRhythmPanCentre) * kRhythmPanScale);
         }
         key = sub->key;
      }
      v = sub;
   }
   if (kind_of(v->kind) == rom::VoiceKind::None)
   {
      return;
   }

   uint32_t prio = uint32_t(p.priority) + t.priority;
   if (prio > kMaxPriority)
   {
      prio = kMaxPriority;
   }
   const bool tone = is_tone(v->kind);
   const uint8_t rank = rank_of(t);
   const uint8_t c = tone ? allocate_tone(tone_number(v->kind), prio, rank) : allocate_sampled(prio, rank);
   if (c == kNoChannel)
   {
      return;
   }

   // Link the channel at the head of the track's list.
   Channel& ch = g_channels[c];
   unlink(c);
   ch.prev = kNoChannel;
   ch.next = t.chan;
   if (t.chan != kNoChannel)
   {
      g_channels[t.chan].prev = c;
   }
   t.chan = c;
   ch.track = rank;

   t.lfo_delay_count = t.lfo_delay;
   if (t.lfo_delay != 0)
   {
      clear_mod(t);
   }
   track_vol_pitch(t);
   ch.gate = t.gate;
   ch.midi_key = t.key;
   ch.velocity = t.velocity;
   ch.priority = static_cast<uint8_t>(prio);
   ch.key = key;
   ch.rhythm_pan = rhythm_pan;
   ch.kind = v->kind;
   ch.ref = v->ref;
   ch.attack = v->env.adsr[rom::kAttack];
   ch.decay = v->env.adsr[rom::kDecay];
   ch.sustain = v->env.adsr[rom::kSustain];
   ch.release = v->env.adsr[rom::kRelease];
   ch.echo_volume = t.echo_volume;
   ch.echo_length = t.echo_length;
   channel_volume(ch, t);
   int32_t played = ch.key + t.key_m;
   if (played < 0)
   {
      played = 0;
   }
   if (tone)
   {
      ch.length = v->tone_length;
      const uint8_t ps = v->pan_sweep;
      ch.sweep = (ps & kPanSweepPan) != 0 || (ps & kSweepTimeBits) == 0 ? kSweepOff : ps;
      ch.frequency = key_to_tone(tone_number(v->kind), static_cast<uint8_t>(played), t.pit_m);
   }
   else
   {
      ch.sample = v->ref.sample;
      ch.count = static_cast<int32_t>(t.start_offset);   // the start offset until the envelope's start
      ch.frequency = key_to_freq(ch.sample, static_cast<uint8_t>(played), t.pit_m);
   }
   ch.status = kChanStart;
   t.flags &= kTrackKeep;
}

void note_off(const Player& p, Track& t)
{
   uint8_t key = t.key;
   if (p.data[t.offset] < kCmdFirst)
   {
      key = p.data[t.offset++];
      t.key = key;
   }
   for (uint8_t c = t.chan; c != kNoChannel; c = g_channels[c].next)
   {
      Channel& ch = g_channels[c];
      if ((ch.status & (kChanStart | kChanEnv)) != 0 && (ch.status & kChanStop) == 0 && ch.midi_key == key)
      {
         ch.status |= kChanStop;
         return;
      }
   }
}

}   // namespace capsule::audio::detail
