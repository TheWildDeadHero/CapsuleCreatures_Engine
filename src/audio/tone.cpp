/// @file
/// The tone channels (D-34, M4 B1a; inventory sections 4.6 and 6): once a frame, before the mix, each sounding tone
/// channel steps its software envelope and writes its registers through hal::audio::tone_write(), in the reference
/// driver's order and only when something changed.
///
/// The envelope clock counts 14..0; on the frame it reads 0 every channel steps twice (the hardware envelope runs at
/// 64 Hz). A start computes the volume goal ((left + right) / 16, at most 15 when panned hard to one side), the
/// sustain level ((goal x sustain + 15) >> 4; the goal a byte: (left + right) & 0xFF before the division), writes the sweep, duty / length, the wave pattern (when it changed)
/// or the noise width, then attack (+1 per attack frames up to the goal), decay (-1 per decay frames down to the
/// sustain level), sustain (the level refreshed every 7 frames), release (-1 per release frames), then pseudo-echo
/// ((goal x echo volume + 255) >> 8 for echo length frames) or off. The hardware envelope bits in NRx2 follow the
/// software phase so the hardware ramps between the software's steps.
///
/// Runs from ROM on the GBA (C14a, R10 M2 item 6: once per sounding tone channel per frame, so IWRAM is kept for the
/// per-sample mix loops; the arithmetic is unchanged).
#include "audio_internal.h"

#include "capsule/hal/audio.h"

namespace capsule::audio::detail
{
namespace
{

using hal::audio::ToneReg;

/// One tone channel's registers.
struct Regs
{
   ToneReg x0;   ///< NR10 / NR30 (tone 2 and noise have none; never used for them).
   ToneReg x1;   ///< Duty / length.
   ToneReg x2;   ///< Envelope (wave: volume).
   ToneReg x3;   ///< Frequency low (noise: NR43).
   ToneReg x4;   ///< Frequency high, length enable, trigger.
};

/// The four channels' registers.
constexpr Regs kRegs[kToneChannels] = {
   {ToneReg::Nr10, ToneReg::Nr11, ToneReg::Nr12, ToneReg::Nr13, ToneReg::Nr14},
   {ToneReg::Nr10, ToneReg::Nr21, ToneReg::Nr22, ToneReg::Nr23, ToneReg::Nr24},
   {ToneReg::Nr30, ToneReg::Nr31, ToneReg::Nr32, ToneReg::Nr33, ToneReg::Nr34},
   {ToneReg::Nr30, ToneReg::Nr41, ToneReg::Nr42, ToneReg::Nr43, ToneReg::Nr44},
};

inline constexpr uint8_t kSweepNumber = 1;      ///< Tone 1.
inline constexpr uint8_t kSquareNumber = 2;     ///< Tone 2.
inline constexpr uint8_t kWaveNumber = 3;       ///< Tone 3.
inline constexpr uint8_t kNoiseNumber = 4;      ///< Tone 4.
inline constexpr uint8_t kEnvOff = 8;           ///< NRx2 = 8: silent, increasing (the off value).
inline constexpr uint8_t kTriggerOff = 0x80;    ///< NRx4 = 0x80 (the off value).
inline constexpr uint8_t kEnvIncrease = 0x08;   ///< NRx2 direction bit: increase.
inline constexpr uint8_t kEnvBitsMask = 0x0F;   ///< NRx2: direction and step.
inline constexpr uint8_t kEnvShift = 4;         ///< NRx2: the volume in the high nibble.
inline constexpr uint8_t kDutyShift = 6;        ///< NRx1: the duty in bits 6-7.
inline constexpr uint8_t kNoiseWidthShift = 3;  ///< NR43 bit 3: 7-bit noise.
inline constexpr uint8_t kNoiseWidthBit = 0x08; ///< NR43 bit 3.
inline constexpr uint8_t kLengthEnable = 0x40;  ///< NRx4 bit 6.
inline constexpr uint8_t kTrigger = 0x80;       ///< NRx4 bit 7.
inline constexpr uint8_t kN4Keep = 0xC0;        ///< n4 bits a frequency write keeps.
inline constexpr uint8_t kN4Clear = 0x7F;       ///< Wave: the trigger is written once.
inline constexpr uint8_t kWaveOn = 0x80;        ///< NR30: playback on.
inline constexpr uint8_t kWaveBankLoad = 0x40;  ///< NR30: the other bank selected (the CPU writes this one).
inline constexpr uint8_t kSweepNegate = 0x08;   ///< NR10 bit 3: sweep down.
inline constexpr uint8_t kBias32k = 0x40;       ///< SOUNDBIAS_H below: 32,768 Hz PWM.
inline constexpr uint8_t kBias64k = 0x80;       ///< SOUNDBIAS_H below: 65,536 Hz PWM.
inline constexpr uint32_t kEven32kAdd = 2;      ///< 32 kHz: (f + 2) & 0x7FC.
inline constexpr uint32_t kEven32kMask = 0x7FC; ///< 32 kHz mask.
inline constexpr uint32_t kEven64kAdd = 1;      ///< 64 kHz: (f + 1) & 0x7FE.
inline constexpr uint32_t kEven64kMask = 0x7FE; ///< 64 kHz mask.
inline constexpr uint8_t kHighByteShift = 8;    ///< The frequency's second byte.
inline constexpr uint8_t kVolumeDivisor = 16;   ///< Goal = (left + right) / 16.
inline constexpr uint8_t kMaxGoal = 15;         ///< Goal clamp when panned hard.
inline constexpr uint8_t kSustainRound = 15;    ///< (goal x sustain + 15) >> 4.
inline constexpr uint8_t kSustainShift = 4;     ///< ... >> 4.
inline constexpr uint32_t kEchoRound = 255;     ///< (goal x echo + 255) >> 8.
inline constexpr uint32_t kEchoShift = 8;       ///< ... >> 8.
inline constexpr uint8_t kPanRight = 0x0F;      ///< NR51: right only.
inline constexpr uint8_t kPanLeft = 0xF0;       ///< NR51: left only.
inline constexpr uint8_t kPanBoth = 0xFF;       ///< NR51: both sides.
inline constexpr uint8_t kHardPanRatio = 2;     ///< Hard pan: one side at least twice the other.
inline constexpr uint8_t kWaveVolumeMask = kWaveVolumeCount - 1;   ///< Wave volume table index bits.
inline constexpr uint8_t kByteShift = 8;        ///< Wave words: bytes in order.
inline constexpr uint32_t kByteMask = 0xFF;     ///< A byte.
inline constexpr uint8_t kWordBytes = 4;        ///< Bytes per wave word.

/// Writes a tone register.
/// @param r  the register
/// @param v  the value
[[gnu::always_inline]] inline void write(ToneReg r, uint32_t v)
{
   hal::audio::tone_write(r, static_cast<uint8_t>(v));
}

/// Hard pan: sets the routing to one side when one side's volume is at least twice the other's.
/// @param ch  the channel
/// @return true when panned hard
[[gnu::always_inline]] inline bool hard_pan(Channel& ch)
{
   if (ch.right >= ch.left)
   {
      if (ch.right / kHardPanRatio >= ch.left)
      {
         ch.pan = kPanRight;
         return true;
      }
   }
   else if (ch.left / kHardPanRatio >= ch.right)
   {
      ch.pan = kPanLeft;
      return true;
   }
   return false;
}

/// The volume goal, sustain level and routing from the channel's volumes.
/// @param ch  the channel
[[gnu::always_inline]] inline void mod_vol(Channel& ch)
{
   // The goal is a byte: the sum wraps before the division, as the original's does.
   const uint32_t sum = (uint32_t(ch.left) + ch.right) & kByteMask;
   if (g_driver.mono != 0 || !hard_pan(ch))
   {
      ch.pan = kPanBoth;
      ch.env_goal = static_cast<uint8_t>(sum / kVolumeDivisor);
   }
   else
   {
      ch.env_goal = static_cast<uint8_t>(sum / kVolumeDivisor);
      if (ch.env_goal > kMaxGoal)
      {
         ch.env_goal = kMaxGoal;
      }
   }
   ch.sustain_goal = static_cast<uint8_t>((ch.env_goal * ch.sustain + kSustainRound) >> kSustainShift);
   ch.pan &= ch.pan_mask;
}

/// Loads a wave pattern into the wave RAM bank the CPU sees.
/// @param wave  kWaveBytes bytes
void load_wave(const uint8_t* wave)
{
   uint32_t words[hal::audio::kWaveWords];
   for (uint8_t w = 0; w < hal::audio::kWaveWords; ++w)
   {
      uint32_t v = 0;
      for (uint8_t b = kWordBytes; b > 0; --b)
      {
         v = (v << kByteShift) | wave[w * kWordBytes + b - 1];
      }
      words[w] = v;
   }
   hal::audio::wave_write(words);
}

/// Where a channel's envelope step goes next.
enum class At : uint8_t
{
   Repeat,         ///< The step proper (counter at 0: the phase's change).
   DecayStart,     ///< Attack done: the decay begins.
   SustainStart,   ///< Decay done: the sustain begins.
   Sustain,        ///< The sustain level.
   EchoStart,      ///< Release done: pseudo-echo or off.
   StepComplete,   ///< The counter counts; a second step on the clock's 0 frame.
   EnvComplete,    ///< The registers.
   Off,            ///< The channel stops.
};

/// One tone channel's frame.
/// @param n   1-4
/// @param ch  the channel
void tone_channel(uint8_t n, Channel& ch)
{
   const Regs& r = kRegs[n - 1];
   int32_t clock = g_driver.envelope_clock;
   uint32_t dir = hal::audio::tone_read(r.x2);
   At at = At::Repeat;
   if ((ch.status & kChanStart) != 0)
   {
      if ((ch.status & kChanStop) != 0)
      {
         at = At::Off;
      }
      else
      {
         ch.status = kEnvAttack;
         ch.modify = kModifyPitch | kModifyVolume;
         mod_vol(ch);
         switch (n)
         {
         case kSweepNumber:
            write(r.x0, ch.sweep);
            [[fallthrough]];
         case kSquareNumber:
            write(r.x1, (ch.ref.value << kDutyShift) + ch.length);
            dir = uint32_t(ch.attack) + kEnvIncrease;
            ch.n4 = ch.length != 0 ? kLengthEnable : 0;
            break;
         case kWaveNumber:
            if (ch.ref.wave != ch.loaded_wave)
            {
               write(r.x0, kWaveBankLoad);
               load_wave(ch.ref.wave);
               ch.loaded_wave = ch.ref.wave;
            }
            write(r.x0, 0);
            write(r.x1, ch.length);
            ch.n4 = static_cast<uint8_t>(ch.length != 0 ? kTrigger | kLengthEnable : kTrigger);
            break;
         default:
            write(r.x1, ch.length);
            write(r.x3, ch.ref.value << kNoiseWidthShift);
            dir = uint32_t(ch.attack) + kEnvIncrease;
            ch.n4 = ch.length != 0 ? kLengthEnable : 0;
            break;
         }
         ch.env_counter = ch.attack;
         if (ch.attack != 0)
         {
            ch.env = 0;
            at = At::StepComplete;
         }
         else
         {
            at = At::DecayStart;   // no attack: straight to the decay
         }
      }
   }
   else if ((ch.status & kChanEcho) != 0)
   {
      --ch.echo_length;
      at = static_cast<int8_t>(ch.echo_length) <= 0 ? At::Off : At::EnvComplete;
   }
   else if ((ch.status & kChanStop) != 0 && (ch.status & kChanEnv) != 0)
   {
      ch.status &= static_cast<uint8_t>(~kChanEnv);
      ch.env_counter = ch.release;
      if (ch.release != 0)
      {
         ch.modify |= kModifyVolume;
         if (n != kWaveNumber)
         {
            dir = ch.release;   // decrease
         }
         at = At::StepComplete;
      }
      else
      {
         at = At::EchoStart;
      }
   }

   for (;;)
   {
      switch (at)
      {
      case At::Repeat:
         if (ch.env_counter != 0)
         {
            at = At::StepComplete;
            break;
         }
         if (n == kWaveNumber)
         {
            ch.modify |= kModifyVolume;
         }
         mod_vol(ch);
         switch (ch.status & kChanEnv)
         {
         case kEnvRelease:
            --ch.env;
            if (static_cast<int8_t>(ch.env) <= 0)
            {
               at = At::EchoStart;
            }
            else
            {
               ch.env_counter = ch.release;
               at = At::StepComplete;
            }
            break;
         case kEnvSustain: at = At::Sustain; break;
         case kEnvDecay:
            --ch.env;
            if (static_cast<int8_t>(ch.env) <= static_cast<int8_t>(ch.sustain_goal))
            {
               at = At::SustainStart;
            }
            else
            {
               ch.env_counter = ch.decay;
               at = At::StepComplete;
            }
            break;
         default:   // attack
            ++ch.env;
            if (ch.env >= ch.env_goal)
            {
               at = At::DecayStart;
            }
            else
            {
               ch.env_counter = ch.attack;
               at = At::StepComplete;
            }
            break;
         }
         break;
      case At::DecayStart:
         --ch.status;
         ch.env_counter = ch.decay;
         if (ch.env_counter != 0)
         {
            ch.modify |= kModifyVolume;
            ch.env = ch.env_goal;
            if (n != kWaveNumber)
            {
               dir = ch.decay;   // decrease
            }
            at = At::StepComplete;
         }
         else
         {
            at = At::SustainStart;
         }
         break;
      case At::SustainStart:
         if (ch.sustain == 0)
         {
            ch.status &= static_cast<uint8_t>(~kChanEnv);
            at = At::EchoStart;
         }
         else
         {
            --ch.status;
            ch.modify |= kModifyVolume;
            if (n != kWaveNumber)
            {
               dir = kEnvIncrease;
            }
            at = At::Sustain;
         }
         break;
      case At::Sustain:
         ch.env = ch.sustain_goal;
         ch.env_counter = kSustainRefresh;
         at = At::StepComplete;
         break;
      case At::EchoStart:
         ch.env = static_cast<uint8_t>((uint32_t(ch.env_goal) * ch.echo_volume + kEchoRound) >> kEchoShift);
         if (ch.env != 0)
         {
            ch.status |= kChanEcho;
            ch.modify |= kModifyVolume;
            if (n != kWaveNumber)
            {
               dir = kEnvIncrease;
            }
            at = At::EnvComplete;
         }
         else
         {
            at = At::Off;
         }
         break;
      case At::StepComplete:
         --ch.env_counter;
         if (clock == 0)
         {
            --clock;   // the clock's 0 frame: one more step
            at = At::Repeat;
         }
         else
         {
            at = At::EnvComplete;
         }
         break;
      case At::Off:
         tone_off(n);
         ch.status = 0;
         ch.modify = 0;
         return;
      case At::EnvComplete:
      {
         if ((ch.modify & kModifyPitch) != 0)
         {
            if (n < kNoiseNumber && (ch.kind & rom::kEvenPitch) != 0)
            {
               const uint8_t bias = hal::audio::tone_read(ToneReg::SoundbiasH);
               if (bias < kBias32k)
               {
                  ch.frequency = (ch.frequency + kEven32kAdd) & kEven32kMask;
               }
               else if (bias < kBias64k)
               {
                  ch.frequency = (ch.frequency + kEven64kAdd) & kEven64kMask;
               }
            }
            if (n != kNoiseNumber)
            {
               write(r.x3, ch.frequency);
            }
            else
            {
               write(r.x3, (hal::audio::tone_read(r.x3) & kNoiseWidthBit) | ch.frequency);
            }
            ch.n4 = static_cast<uint8_t>((ch.n4 & kN4Keep) + static_cast<uint8_t>(ch.frequency >> kHighByteShift));
            write(r.x4, ch.n4);
         }
         if ((ch.modify & kModifyVolume) != 0)
         {
            const uint8_t routing = hal::audio::tone_read(ToneReg::Nr51);
            write(ToneReg::Nr51, (routing & static_cast<uint8_t>(~ch.pan_mask)) | ch.pan);
            if (n == kWaveNumber)
            {
               write(r.x2, kWaveVolume[ch.env & kWaveVolumeMask]);
               if ((ch.n4 & kTrigger) != 0)
               {
                  write(r.x0, kWaveOn);
                  write(r.x4, ch.n4);
                  ch.n4 &= kN4Clear;
               }
            }
            else
            {
               write(r.x2, (dir & kEnvBitsMask) + (uint32_t(ch.env) << kEnvShift));
               write(r.x4, ch.n4 | kTrigger);
               // Tone 1 writes the trigger again when its sweep goes up (the original's second write).
               if (n == kSweepNumber && (hal::audio::tone_read(r.x0) & kSweepNegate) == 0)
               {
                  write(r.x4, ch.n4 | kTrigger);
               }
            }
         }
         ch.modify = 0;
         return;
      }
      }
   }
}

}   // namespace

void tone_off(uint8_t number)
{
   const Regs& r = kRegs[number - 1];
   if (number == kWaveNumber)
   {
      write(r.x0, 0);
   }
   else
   {
      write(r.x2, kEnvOff);
      write(r.x4, kTriggerOff);
   }
}

void tone_step()
{
   if (g_driver.envelope_clock != 0)
   {
      --g_driver.envelope_clock;
   }
   else
   {
      g_driver.envelope_clock = kToneEnvelopeFrames;
   }
   for (uint8_t n = 1; n <= kToneChannels; ++n)
   {
      Channel& ch = g_channels[kFirstTone + n - 1];
      if ((ch.status & kChanOn) != 0)
      {
         tone_channel(n, ch);
      }
   }
}

}   // namespace capsule::audio::detail
