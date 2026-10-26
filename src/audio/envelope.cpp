/// @file
/// The sampled channels' envelope (D-34, M4 B1a; inventory section 4.6), stepped once a frame by the mixer before
/// it mixes the channel.
///
/// - Start: a channel also releasing stops at once; else attack, the position at the start offset (reverse: from
///   the end), the count to the end, envelope 0, then one attack step.
/// - Attack: env += attack, at 255 decay. Decay: env = env x decay >> 8, at the sustain level sustain (a sustain of 0
///   goes to the echo test). Sustain: unchanged. Release: env = env x release >> 8 while above the pseudo-echo
///   volume, then the echo test: no echo volume stops the channel, else env = echo volume for echo length frames.
/// - Output: v = (master volume + 1) x env >> 4; each side = side volume x v >> 8.
#include "audio_internal.h"

namespace capsule::audio::detail
{
namespace
{

inline constexpr uint32_t kEnvelopeShift = 8;     ///< env x rate >> 8.
inline constexpr uint32_t kMasterShift = 4;       ///< (master + 1) x env >> 4.
inline constexpr uint32_t kSideShift = 8;         ///< side x v >> 8.

/// The phase of a status byte.
/// @param status  the channel's status
/// @return its envelope phase
uint8_t phase_of(uint8_t status)
{
   return status & kChanEnv;
}

}   // namespace

bool sampled_envelope(Channel& ch)
{
   uint8_t status = ch.status;
   uint32_t env = ch.env;
   enum class Next : uint8_t
   {
      Output,   ///< The envelope level is final.
      Attack,   ///< One attack step.
      Echo,     ///< The echo test.
   };
   Next next = Next::Output;
   if ((status & kChanStart) != 0)
   {
      if ((status & kChanStop) != 0)
      {
         ch.status = 0;
         return false;
      }
      const rom::SampleHeader* s = ch.sample;
      const uint32_t offset = static_cast<uint32_t>(ch.count);
      if (offset >= s->length)
      {
         ch.status = 0;   // nothing to play (the original would read past the sample)
         return false;
      }
      status = kEnvAttack;
      ch.position = static_cast<int32_t>((ch.kind & rom::kReverse) != 0 ? s->length - offset - 1 : offset);
      ch.count = static_cast<int32_t>(s->length - offset);
      ch.fraction = 0;
      env = 0;
      if ((s->flags & kSampleLoopBits) != 0)
      {
         status |= kChanLoop;
      }
      ch.status = status;
      next = Next::Attack;
   }
   else if ((status & kChanEcho) != 0)
   {
      // The echo counts down; a count of 1 (or 0) ends the channel.
      const uint8_t left = ch.echo_length--;
      if (left <= 1)
      {
         ch.status = 0;
         return false;
      }
   }
   else if ((status & kChanStop) != 0)
   {
      env = (env * ch.release) >> kEnvelopeShift;
      if (env <= ch.echo_volume)
      {
         next = Next::Echo;
      }
   }
   else if (phase_of(status) == kEnvDecay)
   {
      env = (env * ch.decay) >> kEnvelopeShift;
      if (env <= ch.sustain)
      {
         env = ch.sustain;
         if (env == 0)
         {
            next = Next::Echo;
         }
         else
         {
            --status;   // sustain
            ch.status = status;
         }
      }
   }
   else if (phase_of(status) == kEnvAttack)
   {
      next = Next::Attack;
   }

   if (next == Next::Attack)
   {
      env += ch.attack;
      if (env >= kMaxEnvelope)
      {
         env = kMaxEnvelope;
         --status;   // decay
         ch.status = status;
      }
   }
   else if (next == Next::Echo)
   {
      env = ch.echo_volume;
      if (env == 0)
      {
         ch.status = 0;
         return false;
      }
      status |= kChanEcho;
      ch.status = status;
   }

   ch.env = static_cast<uint8_t>(env);
   const uint32_t v = ((uint32_t(g_driver.master_volume) + 1) * env) >> kMasterShift;
   ch.env_right = static_cast<uint8_t>((ch.right * v) >> kSideShift);
   ch.env_left = static_cast<uint8_t>((ch.left * v) >> kSideShift);
   return true;
}

}   // namespace capsule::audio::detail
