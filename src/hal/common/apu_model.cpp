/// @file
/// The integer sound output model (apu_model.h): tone channels, frame sequencer, final mix.
#include "apu_model.h"

#include <cstdint>

namespace capsule::hal::apu
{

namespace
{

using audio::ToneReg;

/// A register's offset in the block.
/// @param r  the register
/// @return its offset
constexpr uint8_t at(ToneReg r)
{
   return static_cast<uint8_t>(r);
}

/// @name Register offsets the model reads besides hal::audio::ToneReg
///@{
constexpr uint8_t kSoundcntHLow = kSoundcntH;            ///< SOUNDCNT_H, low byte.
constexpr uint8_t kSoundcntHHigh = kSoundcntH + 1;       ///< SOUNDCNT_H, high byte.
constexpr uint8_t kSoundbiasLow = at(ToneReg::SoundbiasH) - 1;   ///< SOUNDBIAS, low byte.
///@}

/// Each channel's registers: duty / length, envelope, frequency low, control (frequency high, length enable,
/// trigger). The wave channel's "envelope" is its volume register.
struct ChannelRegs
{
   uint8_t length;    ///< NRx1 (wave: NR31).
   uint8_t envelope;  ///< NRx2 (wave: NR32).
   uint8_t freq_low;  ///< NRx3 (noise: NR43).
   uint8_t control;   ///< NRx4.
};
/// The four channels' registers, by channel.
constexpr ChannelRegs kChannelRegs[kToneChannels] = {
   {at(ToneReg::Nr11), at(ToneReg::Nr12), at(ToneReg::Nr13), at(ToneReg::Nr14)},
   {at(ToneReg::Nr21), at(ToneReg::Nr22), at(ToneReg::Nr23), at(ToneReg::Nr24)},
   {at(ToneReg::Nr31), at(ToneReg::Nr32), at(ToneReg::Nr33), at(ToneReg::Nr34)},
   {at(ToneReg::Nr41), at(ToneReg::Nr42), at(ToneReg::Nr43), at(ToneReg::Nr44)},
};
constexpr uint8_t kWaveChannel = 2;    ///< Channel index of the wave channel.
constexpr uint8_t kNoiseChannel = 3;   ///< Channel index of the noise channel.

/// @name Register fields
///@{
constexpr uint8_t kTrigger = 0x80;           ///< NRx4 bit 7: start the channel.
constexpr uint8_t kLengthEnable = 0x40;      ///< NRx4 bit 6: stop when the length counter runs out.
constexpr uint8_t kFreqHighMask = 0x07;      ///< NRx4 bits 0-2: frequency bits 8-10.
constexpr int kFreqHighShift = 8;            ///< Where those bits go.
constexpr uint8_t kLengthMask = 0x3F;        ///< NRx1 bits 0-5: length (64 - counter).
constexpr uint16_t kSquareLength = 64;       ///< Square / noise length counter range.
constexpr uint16_t kWaveLength = 256;        ///< Wave length counter range.
constexpr int kDutyShift = 6;                ///< NRx1 bits 6-7: duty.
constexpr uint8_t kDacMask = 0xF8;           ///< NRx2: initial volume and direction; all 0: the DAC is off.
constexpr int kVolumeShift = 4;              ///< NRx2 bits 4-7: initial volume.
constexpr uint8_t kEnvelopeUp = 0x08;        ///< NRx2 bit 3: the envelope rises.
constexpr uint8_t kEnvelopePeriod = 0x07;    ///< NRx2 bits 0-2: envelope period, 64 Hz ticks (0: none).
constexpr uint8_t kVolumeMax = 15;           ///< Highest level.
constexpr uint8_t kSweepShiftMask = 0x07;    ///< NR10 bits 0-2: sweep shift.
constexpr uint8_t kSweepNegate = 0x08;       ///< NR10 bit 3: the sweep lowers the frequency.
constexpr int kSweepTimeShift = 4;           ///< NR10 bits 4-6: sweep period, 128 Hz ticks.
constexpr uint8_t kSweepTimeMask = 0x07;     ///< Mask of that field.
constexpr uint8_t kSweepTimeZero = 8;        ///< A period of 0 counts as 8.
constexpr uint32_t kFreqMax = 2047;          ///< Largest 11-bit frequency.
constexpr uint32_t kFreqRange = 2048;        ///< Periods count from 2048 - frequency.
constexpr uint8_t kWaveDacOn = 0x80;         ///< NR30 bit 7: the wave channel's DAC.
constexpr int kWaveBankShift = 6;            ///< NR30 bit 6: the bank that plays.
constexpr uint8_t kWaveTwoBanks = 0x20;      ///< NR30 bit 5: both banks play (64 samples).
constexpr uint8_t kWaveForce75 = 0x80;       ///< NR32 bit 7: 75 % volume.
constexpr int kWaveVolumeShift = 5;          ///< NR32 bits 5-6: 0 mute, 1 100 %, 2 50 %, 3 25 %.
constexpr uint8_t kWaveVolumeMask = 0x03;    ///< Mask of that field.
constexpr uint8_t kWaveSamples = 32;         ///< Samples in one bank.
constexpr uint8_t kNoiseDivisorMask = 0x07;  ///< NR43 bits 0-2: divisor code r.
constexpr uint8_t kNoiseNarrow = 0x08;       ///< NR43 bit 3: 7-bit register.
constexpr int kNoiseShiftShift = 4;          ///< NR43 bits 4-7: clock shift s.
constexpr uint8_t kNoiseShiftStop = 14;      ///< Shifts from 14 up do not clock the register.
constexpr uint16_t kLfsrStart = 0x7FFF;      ///< The register at a trigger.
constexpr int kLfsrTopBit = 14;              ///< Feedback goes into bit 14 ...
constexpr int kLfsrNarrowBit = 6;            ///< ... and bit 6 too in 7-bit mode.
constexpr uint8_t kMasterOn = 0x80;          ///< NR52 bit 7.
constexpr int kNr50LeftShift = 4;            ///< NR50 bits 4-6: left volume (bits 0-2 right).
constexpr uint8_t kNr50VolumeMask = 0x07;    ///< Mask of one side's volume.
constexpr int kNr51LeftShift = 4;            ///< NR51 bits 4-7: channels to the left (bits 0-3 right).
///@}

/// @name SOUNDCNT_H fields
///@{
constexpr uint16_t kToneRatioMask = 0x0003;   ///< Bits 0-1: tone ratio 0 25 %, 1 50 %, 2 100 %.
constexpr uint16_t kToneRatioFull = 2;        ///< The 100 % ratio (3 is not allowed: treated as 100 %).
constexpr uint16_t kFifoAFull = 0x0004;       ///< Bit 2: FIFO A at 100 % (else 50 %).
constexpr uint16_t kFifoBFull = 0x0008;       ///< Bit 3: FIFO B at 100 %.
constexpr uint16_t kFifoARight = 0x0100;      ///< Bit 8: FIFO A to the right.
constexpr uint16_t kFifoALeft = 0x0200;       ///< Bit 9: FIFO A to the left.
constexpr uint16_t kFifoBRight = 0x1000;      ///< Bit 12: FIFO B to the right.
constexpr uint16_t kFifoBLeft = 0x2000;       ///< Bit 13: FIFO B to the left.
///@}

/// @name Timing (2^24 Hz cycles)
///@{
constexpr uint32_t kSequencerCycles = 32768;   ///< One 512 Hz frame sequencer step.
constexpr uint8_t kSequencerSteps = 8;         ///< Steps in one sequencer round.
constexpr uint8_t kSweepStepA = 2;             ///< Sequencer steps that clock the sweep ...
constexpr uint8_t kSweepStepB = 6;             ///< ... (128 Hz).
constexpr uint8_t kEnvelopeStep = 7;           ///< The step that clocks the envelopes (64 Hz).
constexpr uint32_t kSquareStepCycles = 16;     ///< A duty step lasts (2048 - frequency) x 16 cycles.
constexpr uint32_t kWaveStepCycles = 8;        ///< A wave sample lasts (2048 - frequency) x 8 cycles.
constexpr uint32_t kNoiseHalfDivisor = 32;     ///< Noise: divisor code 0 gives 32 cycles ...
constexpr uint32_t kNoiseDivisorCycles = 64;   ///< ... code r gives r x 64, then << shift.
constexpr uint8_t kDutySteps = 8;              ///< Steps in a duty pattern.
///@}

/// @name Mix
///@{
constexpr int32_t kToneGain = 2;               ///< Tone sum x 2 x (NR50 volume + 1) at the 100 % ratio.
constexpr int32_t kFifoFullGain = 4;           ///< A FIFO byte at 100 % spans the 10-bit range.
constexpr int32_t kFifoHalfGain = 2;           ///< At 50 %.
constexpr int32_t kDacMax = 0x3FF;             ///< 10-bit output.
constexpr uint16_t kBiasMask = 0x3FE;          ///< SOUNDBIAS bits 1-9: the bias level.
constexpr int kResolutionShift = 6;            ///< SOUNDBIAS_H bits 6-7: amplitude resolution.
constexpr int kDacTo16 = 6;                    ///< A 10-bit step is 64 in 16-bit output.
constexpr int32_t kOutMax = 32767;             ///< Signed 16-bit range.
constexpr int32_t kOutMin = -32768;            ///< Signed 16-bit range.
constexpr int kHqHalf = 1;                     ///< HQ: a 50 % FIFO is the 16-bit value >> 1.
///@}

/// @name Start-up state (as audio_gba.cpp's init() writes it)
///@{
constexpr uint32_t kStartMaster = 0x8F;       ///< SOUNDCNT_X: master enable and the channels' bits.
constexpr uint32_t kStartEnvelope = 0x08;     ///< NRx2: level 0, rising.
constexpr uint32_t kStartNr50 = 0x77;         ///< NR50: full tone volume on both sides.
constexpr uint8_t kBiasKeep = 0x3F;           ///< SOUNDBIAS_H bits kept.
constexpr uint8_t kBias8Bit = 0x40;           ///< SOUNDBIAS_H resolution 1: 8 bits.
constexpr uint32_t kRingSides = 2;            ///< The ring's sides.
///@}

/// Duty patterns (12.5, 25, 50, 75 %): bit 7 - step is the level of step `step`.
constexpr uint8_t kDutyPatterns[] = {0x01, 0x81, 0x87, 0x7E};
/// Output sides (left, right), interleaved in the output.
constexpr uint32_t kSides = 2;
/// Wave samples per byte, and the length counters' clock: every second sequencer step.
constexpr uint8_t kPerTwo = 2;
/// The resolution field keeps the top (9 - field) bits of the 10: the low (field + 1) bits are cut.
constexpr int32_t kResolutionBase = 2;
/// The highest bit of a duty pattern (step 0).
constexpr int kDutyTopBit = 7;
/// Bits in a byte.
constexpr int kByteBits = 8;
/// Bytes in a wave RAM word.
constexpr uint8_t kWordBytes = 4;
/// A wave byte holds two samples, high nibble first.
constexpr int kNibbleShift = 4;
/// Mask of a nibble.
constexpr uint8_t kNibbleMask = 0x0F;
/// 75 % of a 4-bit sample: x 3 >> 2.
constexpr int kWave75Mul = 3;
/// Divider of the 75 % level, as a shift.
constexpr int kWave75Shift = 2;

/// The 11-bit frequency in a channel's registers.
/// @param regs  the register block
/// @param c     the channel
/// @return the frequency
uint16_t reg_frequency(const uint8_t* regs, const ChannelRegs& c)
{
   return static_cast<uint16_t>(regs[c.freq_low] | ((regs[c.control] & kFreqHighMask) << kFreqHighShift));
}

/// Clamps to the signed 16-bit range.
/// @param v  the value
/// @return it, clamped
int16_t clamp16(int32_t v)
{
   return static_cast<int16_t>(v > kOutMax ? kOutMax : (v < kOutMin ? kOutMin : v));
}

} // namespace

void Model::reset()
{
   state_ = State{};
   state_.regs[kSoundbiasLow] = static_cast<uint8_t>(kSoundbiasPowerOn & UINT8_MAX);
   state_.regs[at(ToneReg::SoundbiasH)] = static_cast<uint8_t>(kSoundbiasPowerOn >> kByteBits);
   state_.noise.lfsr = kLfsrStart;
}

void Model::apply(const Write& w)
{
   uint8_t* regs = state_.regs;
   if (w.kind == WriteKind::Output)
   {
      regs[kSoundcntHLow] = static_cast<uint8_t>(w.value & UINT8_MAX);
      regs[kSoundcntHHigh] = static_cast<uint8_t>((w.value >> kByteBits) & UINT8_MAX);
      return;
   }
   if (w.kind == WriteKind::Wave)
   {
      // The CPU writes the bank NR30 does not select for playback.
      const uint8_t bank = static_cast<uint8_t>(((regs[at(ToneReg::Nr30)] >> kWaveBankShift) & 1) ^ 1);
      for (uint8_t i = 0; i < kWordBytes; ++i)
      {
         const uint32_t index = static_cast<uint32_t>(w.reg % audio::kWaveWords) * kWordBytes + i;
         state_.wave_ram[bank][index] = static_cast<uint8_t>((w.value >> (i * kByteBits)) & UINT8_MAX);
      }
      return;
   }

   const uint8_t offset = w.reg;
   if (offset >= kRegisterBytes)
   {
      return;
   }
   const auto value = static_cast<uint8_t>(w.value);
   regs[offset] = value;
   for (uint8_t ch = 0; ch < kToneChannels; ++ch)
   {
      const ChannelRegs& c = kChannelRegs[ch];
      if (offset == c.length)
      {   // the length counter loads from the register
         const uint16_t counter = ch == kWaveChannel ? static_cast<uint16_t>(kWaveLength - value)
                                                     : static_cast<uint16_t>(kSquareLength - (value & kLengthMask));
         if (ch < kSquareChannels)
         {
            state_.square[ch].length = counter;
         }
         else if (ch == kWaveChannel)
         {
            state_.wave.length = counter;
         }
         else
         {
            state_.noise.length = counter;
         }
      }
      else if (offset == c.envelope && ch != kWaveChannel && (value & kDacMask) == 0)
      {   // the DAC is off: the channel stops
         if (ch < kSquareChannels)
         {
            state_.square[ch].on = false;
         }
         else
         {
            state_.noise.on = false;
         }
      }
      else if ((offset == c.control || offset == c.freq_low) && ch != kNoiseChannel)
      {   // a new frequency applies from the next period
         const uint16_t f = reg_frequency(regs, c);
         if (ch < kSquareChannels)
         {
            state_.square[ch].frequency = f;
         }
         else
         {
            state_.wave.frequency = f;
         }
      }
      if (offset == c.control && (value & kTrigger) != 0)
      {
         trigger(ch);
      }
   }
   if (offset == at(ToneReg::Nr30) && (value & kWaveDacOn) == 0)
   {
      state_.wave.on = false;
   }
}

uint8_t Model::read(uint8_t offset) const
{
   if (offset >= kRegisterBytes)
   {
      return 0;
   }
   if (offset == at(ToneReg::Nr52))
   {   // bit 7 as written, bits 0-3 the channels that play
      uint8_t status = static_cast<uint8_t>(state_.regs[offset] & kMasterOn);
      const bool on[kToneChannels] = {state_.square[0].on, state_.square[1].on, state_.wave.on, state_.noise.on};
      for (uint8_t ch = 0; ch < kToneChannels; ++ch)
      {
         if (on[ch])
         {
            status = static_cast<uint8_t>(status | (1u << ch));
         }
      }
      return status;
   }
   return state_.regs[offset];
}

void Model::trigger(uint8_t channel)
{
   const uint8_t* regs = state_.regs;
   const ChannelRegs& c = kChannelRegs[channel];
   const uint8_t env = regs[c.envelope];
   if (channel < kSquareChannels)
   {
      Square& s = state_.square[channel];
      s.on = (env & kDacMask) != 0;
      if (s.length == 0)
      {
         s.length = kSquareLength;
      }
      s.volume = static_cast<uint8_t>(env >> kVolumeShift);
      s.envelope_timer = static_cast<uint8_t>(env & kEnvelopePeriod);
      s.frequency = reg_frequency(regs, c);
      s.timer = (kFreqRange - s.frequency) * kSquareStepCycles;
      if (channel == 0)
      {   // tone 1's sweep starts from the trigger's frequency
         const uint8_t nr10 = regs[at(ToneReg::Nr10)];
         const uint8_t time = static_cast<uint8_t>((nr10 >> kSweepTimeShift) & kSweepTimeMask);
         const uint8_t shift = static_cast<uint8_t>(nr10 & kSweepShiftMask);
         s.sweep_shadow = s.frequency;
         s.sweep_timer = time != 0 ? time : kSweepTimeZero;
         s.sweep_on = time != 0 || shift != 0;
         if (shift != 0 && sweep_next() > kFreqMax)
         {
            s.on = false;
         }
      }
      return;
   }
   if (channel == kWaveChannel)
   {
      WaveChannel& w = state_.wave;
      w.on = (regs[at(ToneReg::Nr30)] & kWaveDacOn) != 0;
      if (w.length == 0)
      {
         w.length = kWaveLength;
      }
      w.position = 0;
      w.frequency = reg_frequency(regs, c);
      w.timer = (kFreqRange - w.frequency) * kWaveStepCycles;
      return;
   }
   Noise& n = state_.noise;
   n.on = (env & kDacMask) != 0;
   if (n.length == 0)
   {
      n.length = kSquareLength;
   }
   n.volume = static_cast<uint8_t>(env >> kVolumeShift);
   n.envelope_timer = static_cast<uint8_t>(env & kEnvelopePeriod);
   n.lfsr = kLfsrStart;
   const uint8_t poly = regs[c.freq_low];
   const uint8_t r = static_cast<uint8_t>(poly & kNoiseDivisorMask);
   n.timer = (r == 0 ? kNoiseHalfDivisor : r * kNoiseDivisorCycles) << (poly >> kNoiseShiftShift);
}

uint32_t Model::sweep_next() const
{
   const Square& s = state_.square[0];
   const uint8_t nr10 = state_.regs[at(ToneReg::Nr10)];
   const uint32_t delta = s.sweep_shadow >> (nr10 & kSweepShiftMask);
   if ((nr10 & kSweepNegate) != 0)
   {
      return s.sweep_shadow - delta;   // never below 0: delta <= shadow
   }
   return s.sweep_shadow + delta;
}

void Model::step_sequencer()
{
   const uint8_t step = state_.sequencer_step;
   const uint8_t* regs = state_.regs;
   state_.sequencer_step = static_cast<uint8_t>((step + 1) % kSequencerSteps);

   if (step % kPerTwo == 0)
   {   // length counters at 256 Hz
      auto clock_length = [](bool& on, uint16_t& length, uint8_t control)
      {
         if ((control & kLengthEnable) != 0 && length > 0)
         {
            --length;
            if (length == 0)
            {
               on = false;
            }
         }
      };
      for (uint8_t ch = 0; ch < kSquareChannels; ++ch)
      {
         clock_length(state_.square[ch].on, state_.square[ch].length, regs[kChannelRegs[ch].control]);
      }
      clock_length(state_.wave.on, state_.wave.length, regs[kChannelRegs[kWaveChannel].control]);
      clock_length(state_.noise.on, state_.noise.length, regs[kChannelRegs[kNoiseChannel].control]);
   }

   if (step == kSweepStepA || step == kSweepStepB)
   {   // tone 1's sweep at 128 Hz
      Square& s = state_.square[0];
      const uint8_t nr10 = regs[at(ToneReg::Nr10)];
      const uint8_t time = static_cast<uint8_t>((nr10 >> kSweepTimeShift) & kSweepTimeMask);
      if (s.sweep_timer > 0)
      {
         --s.sweep_timer;
      }
      if (s.sweep_timer == 0)
      {
         s.sweep_timer = time != 0 ? time : kSweepTimeZero;
         if (s.sweep_on && time != 0)
         {
            const uint32_t next = sweep_next();
            if (next > kFreqMax)
            {
               s.on = false;
            }
            else if ((nr10 & kSweepShiftMask) != 0)
            {
               s.sweep_shadow = static_cast<uint16_t>(next);
               s.frequency = static_cast<uint16_t>(next);
               if (sweep_next() > kFreqMax)
               {
                  s.on = false;
               }
            }
         }
      }
   }

   if (step == kEnvelopeStep)
   {   // envelopes at 64 Hz
      auto clock_envelope = [](uint8_t& volume, uint8_t& timer, uint8_t env)
      {
         const uint8_t period = static_cast<uint8_t>(env & kEnvelopePeriod);
         if (period == 0)
         {
            return;
         }
         if (timer > 0)
         {
            --timer;
         }
         if (timer == 0)
         {
            timer = period;
            if ((env & kEnvelopeUp) != 0 && volume < kVolumeMax)
            {
               ++volume;
            }
            else if ((env & kEnvelopeUp) == 0 && volume > 0)
            {
               --volume;
            }
         }
      };
      for (uint8_t ch = 0; ch < kSquareChannels; ++ch)
      {
         clock_envelope(state_.square[ch].volume, state_.square[ch].envelope_timer, regs[kChannelRegs[ch].envelope]);
      }
      clock_envelope(state_.noise.volume, state_.noise.envelope_timer, regs[kChannelRegs[kNoiseChannel].envelope]);
   }
}

void Model::run(uint32_t cycles)
{
   const uint8_t* regs = state_.regs;
   while (cycles > 0)
   {
      // Up to the next sequencer step at most, so the channels see each step at its cycle.
      const uint32_t left_in_step = kSequencerCycles - state_.sequencer_cycles;
      const uint32_t n = cycles < left_in_step ? cycles : left_in_step;

      for (Square& s : state_.square)
      {
         if (s.on)
         {   // whole periods at once: the duty step advances by their count
            const uint32_t period = (kFreqRange - s.frequency) * kSquareStepCycles;
            if (n >= s.timer)
            {
               const uint32_t over = n - s.timer;
               s.duty_step = static_cast<uint8_t>((s.duty_step + 1 + over / period) % kDutySteps);
               s.timer = period - over % period;
            }
            else
            {
               s.timer -= n;
            }
         }
      }
      WaveChannel& w = state_.wave;
      if (w.on)
      {
         const uint32_t period = (kFreqRange - w.frequency) * kWaveStepCycles;
         const uint32_t samples = (regs[at(ToneReg::Nr30)] & kWaveTwoBanks) != 0 ? uint32_t{kPerTwo} * kWaveSamples : kWaveSamples;
         if (n >= w.timer)
         {
            const uint32_t over = n - w.timer;
            w.position = static_cast<uint8_t>((w.position + 1 + over / period) % samples);
            w.timer = period - over % period;
         }
         else
         {
            w.timer -= n;
         }
      }
      Noise& z = state_.noise;
      const uint8_t poly = regs[kChannelRegs[kNoiseChannel].freq_low];
      const uint8_t shift = static_cast<uint8_t>(poly >> kNoiseShiftShift);
      if (z.on && shift < kNoiseShiftStop)
      {   // the register shifts once per period (one at a time: its feedback depends on each)
         const uint8_t r = static_cast<uint8_t>(poly & kNoiseDivisorMask);
         const uint32_t period = (r == 0 ? kNoiseHalfDivisor : r * kNoiseDivisorCycles) << shift;
         uint32_t remaining = n;
         while (remaining >= z.timer)
         {
            remaining -= z.timer;
            z.timer = period;
            const auto bit = static_cast<uint16_t>((z.lfsr ^ (z.lfsr >> 1)) & 1);
            z.lfsr = static_cast<uint16_t>((z.lfsr >> 1) | (bit << kLfsrTopBit));
            if ((poly & kNoiseNarrow) != 0)
            {
               z.lfsr = static_cast<uint16_t>((z.lfsr & ~(1u << kLfsrNarrowBit)) | (bit << kLfsrNarrowBit));
            }
         }
         z.timer -= remaining;
      }

      cycles -= n;
      state_.sequencer_cycles += n;
      if (state_.sequencer_cycles == kSequencerCycles)
      {
         state_.sequencer_cycles = 0;
         step_sequencer();
      }
   }
}

int32_t Model::tone_mix(bool left) const
{
   const uint8_t* regs = state_.regs;
   const uint8_t routing = static_cast<uint8_t>(regs[at(ToneReg::Nr51)] >> (left ? kNr51LeftShift : 0));
   int32_t levels[kToneChannels] = {};

   for (uint8_t ch = 0; ch < kSquareChannels; ++ch)
   {
      const Square& s = state_.square[ch];
      const uint8_t duty = static_cast<uint8_t>(regs[kChannelRegs[ch].length] >> kDutyShift);
      if (s.on && ((kDutyPatterns[duty] >> (kDutyTopBit - s.duty_step)) & 1) != 0)
      {
         levels[ch] = s.volume;
      }
   }
   const WaveChannel& w = state_.wave;
   if (w.on)
   {
      const uint8_t nr30 = regs[at(ToneReg::Nr30)];
      const uint8_t playing = static_cast<uint8_t>((nr30 >> kWaveBankShift) & 1);
      // With both banks, samples 32-63 come from the other bank.
      const uint8_t bank = static_cast<uint8_t>(w.position < kWaveSamples ? playing : playing ^ 1);
      const uint8_t index = static_cast<uint8_t>(w.position % kWaveSamples);
      const uint8_t byte = state_.wave_ram[bank][index / kPerTwo];
      const int32_t sample = (index % kPerTwo == 0) ? (byte >> kNibbleShift) : (byte & kNibbleMask);
      const uint8_t vol = regs[at(ToneReg::Nr32)];
      if ((vol & kWaveForce75) != 0)
      {
         levels[kWaveChannel] = (sample * kWave75Mul) >> kWave75Shift;
      }
      else
      {
         const uint8_t code = static_cast<uint8_t>((vol >> kWaveVolumeShift) & kWaveVolumeMask);
         levels[kWaveChannel] = code == 0 ? 0 : sample >> (code - 1);
      }
   }
   const Noise& z = state_.noise;
   if (z.on && (z.lfsr & 1) == 0)
   {
      levels[kNoiseChannel] = z.volume;
   }

   int32_t sum = 0;
   for (uint8_t ch = 0; ch < kToneChannels; ++ch)
   {
      if (((routing >> ch) & 1) != 0)
      {
         sum += levels[ch];
      }
   }
   const int32_t volume = ((regs[at(ToneReg::Nr50)] >> (left ? kNr50LeftShift : 0)) & kNr50VolumeMask) + 1;
   const uint16_t soundcnt = static_cast<uint16_t>(regs[kSoundcntHLow] | (regs[kSoundcntHHigh] << kByteBits));
   uint16_t ratio = static_cast<uint16_t>(soundcnt & kToneRatioMask);
   ratio = ratio > kToneRatioFull ? kToneRatioFull : ratio;
   return (sum * kToneGain * volume) >> (kToneRatioFull - ratio);
}

void Model::render(const int8_t* right, const int8_t* left, int16_t* out, uint16_t samples)
{
   const uint8_t* regs = state_.regs;
   uint32_t done = 0;
   for (uint16_t i = 0; i < samples; ++i)
   {
      const uint32_t instant = (i + 1u) * kCyclesPerFrame / samples;
      run(instant - done);
      done = instant;

      const uint16_t soundcnt = static_cast<uint16_t>(regs[kSoundcntHLow] | (regs[kSoundcntHHigh] << kByteBits));
      const uint16_t bias_reg = static_cast<uint16_t>(regs[kSoundbiasLow] | (regs[at(ToneReg::SoundbiasH)] << kByteBits));
      const int32_t bias = bias_reg & kBiasMask;
      const int resolution = regs[at(ToneReg::SoundbiasH)] >> kResolutionShift;
      const int32_t a = right != nullptr ? right[i] * ((soundcnt & kFifoAFull) != 0 ? kFifoFullGain : kFifoHalfGain) : 0;
      const int32_t b = left != nullptr ? left[i] * ((soundcnt & kFifoBFull) != 0 ? kFifoFullGain : kFifoHalfGain) : 0;
      for (uint32_t side = 0; side < kSides; ++side)
      {   // out[2i] left, out[2i + 1] right
         const bool is_left = side == 0;
         int32_t v = bias + tone_mix(is_left);
         if ((soundcnt & (is_left ? kFifoALeft : kFifoARight)) != 0)
         {
            v += a;
         }
         if ((soundcnt & (is_left ? kFifoBLeft : kFifoBRight)) != 0)
         {
            v += b;
         }
         v = v < 0 ? 0 : (v > kDacMax ? kDacMax : v);
         v &= ~((kResolutionBase << resolution) - 1);   // the resolution keeps 9 - resolution bits of the 10
         out[kSides * i + side] = clamp16((v - bias) * (1 << kDacTo16));
      }
   }
}

void Model::render_hq(const int16_t* right, const int16_t* left, int16_t* out, uint16_t samples)
{
   const uint8_t* regs = state_.regs;
   uint32_t done = 0;
   for (uint16_t i = 0; i < samples; ++i)
   {
      const uint32_t instant = (i + 1u) * kCyclesPerFrame / samples;
      run(instant - done);
      done = instant;

      const uint16_t soundcnt = static_cast<uint16_t>(regs[kSoundcntHLow] | (regs[kSoundcntHHigh] << kByteBits));
      const int32_t a = right != nullptr ? ((soundcnt & kFifoAFull) != 0 ? right[i] : right[i] >> kHqHalf) : 0;
      const int32_t b = left != nullptr ? ((soundcnt & kFifoBFull) != 0 ? left[i] : left[i] >> kHqHalf) : 0;
      for (uint32_t side = 0; side < kSides; ++side)
      {
         const bool is_left = side == 0;
         int32_t v = tone_mix(is_left) * (1 << kDacTo16);
         if ((soundcnt & (is_left ? kFifoALeft : kFifoARight)) != 0)
         {
            v += a;
         }
         if ((soundcnt & (is_left ? kFifoBLeft : kFifoBRight)) != 0)
         {
            v += b;
         }
         out[kSides * i + side] = clamp16(v);
      }
   }
}

void Player::start(const int8_t* ring, uint16_t side_bytes, uint16_t samples)
{
   model_.reset();
   ring_ = ring;
   side_bytes_ = side_bytes;
   samples_ = samples > audio::kMaxFrameSamples ? audio::kMaxFrameSamples : samples;
   read_position_ = 0;
   pending_ = FrameOut{};
   last_ = FrameOut{};
   hq_pending_ = false;
   frames_ = 0;
   // The GBA backend's start-up writes (audio_gba.cpp init()), applied at once and not recorded as a frame's.
   const Write start_writes[] = {
      {WriteKind::Tone, at(ToneReg::Nr52), 0, kStartMaster},
      {WriteKind::Output, 0, 0, kSoundcntStereo},
      {WriteKind::Tone, at(ToneReg::SoundbiasH), 0,
       static_cast<uint32_t>((model_.read(at(ToneReg::SoundbiasH)) & kBiasKeep) | kBias8Bit)},
      {WriteKind::Tone, at(ToneReg::Nr12), 0, kStartEnvelope},
      {WriteKind::Tone, at(ToneReg::Nr22), 0, kStartEnvelope},
      {WriteKind::Tone, at(ToneReg::Nr42), 0, kStartEnvelope},
      {WriteKind::Tone, at(ToneReg::Nr14), 0, kTrigger},
      {WriteKind::Tone, at(ToneReg::Nr24), 0, kTrigger},
      {WriteKind::Tone, at(ToneReg::Nr44), 0, kTrigger},
      {WriteKind::Tone, at(ToneReg::Nr30), 0, 0},
      {WriteKind::Tone, at(ToneReg::Nr50), 0, kStartNr50},
   };
   for (const Write& w : start_writes)
   {
      model_.apply(w);
   }
   for (uint8_t i = 0; i < kRegisterBytes; ++i)
   {
      shadow_[i] = model_.state().regs[i];
   }
}

void Player::record(const Write& w)
{
   if (pending_.write_count < kMaxWrites)
   {
      pending_.writes[pending_.write_count++] = w;
   }
   else
   {
      ++pending_.dropped_writes;
      model_.apply(w);   // too many to keep for the frame's start: applied now, in order with the ones dropped
   }
}

void Player::restart_dma()
{
   read_position_ = 0;
   pending_.restarted = true;
}

void Player::set_output(bool stereo)
{
   const uint16_t value = stereo ? kSoundcntStereo : kSoundcntMono;
   shadow_[kSoundcntHLow] = static_cast<uint8_t>(value & UINT8_MAX);
   shadow_[kSoundcntHHigh] = static_cast<uint8_t>(value >> kByteBits);
   record(Write{WriteKind::Output, 0, 0, value});
}

void Player::tone_write(uint8_t offset, uint8_t value)
{
   if (offset < kRegisterBytes)
   {
      shadow_[offset] = value;
   }
   record(Write{WriteKind::Tone, offset, 0, value});
}

uint8_t Player::tone_read(uint8_t offset) const
{
   if (offset >= kRegisterBytes)
   {
      return 0;
   }
   if (offset == at(ToneReg::Nr52))
   {
      return static_cast<uint8_t>((shadow_[offset] & kMasterOn) | (model_.read(offset) & ~kMasterOn));
   }
   return shadow_[offset];
}

void Player::wave_write(const uint32_t* words)
{
   for (uint8_t i = 0; i < audio::kWaveWords; ++i)
   {
      record(Write{WriteKind::Wave, i, 0, words[i]});
   }
}

void Player::hq_block(const int16_t* right, const int16_t* left)
{
   for (uint32_t i = 0; i < uint32_t{samples_} * audio::kHqFactor; ++i)
   {
      hq_right_[i] = right[i];
      hq_left_[i] = left[i];
   }
   hq_pending_ = true;
}

int8_t Player::fifo_byte(bool left, uint32_t position) const
{
   const uint32_t at_byte = (left ? side_bytes_ : 0u) + position;
   return at_byte < kRingSides * uint32_t{side_bytes_} ? ring_[at_byte] : 0;
}

const FrameOut& Player::finish_frame(uint8_t segment)
{
   // The writes of the frame, in order, at its start (D-34 answer 5).
   const uint16_t recorded = pending_.write_count;
   for (uint16_t i = 0; i < recorded; ++i)
   {
      model_.apply(pending_.writes[i]);
   }

   FrameOut& f = pending_;
   f.index = ++frames_;
   f.segment = segment;
   f.read_position = static_cast<uint16_t>(read_position_);
   const uint32_t first = uint32_t{segment} * samples_;
   int8_t right[audio::kMaxFrameSamples];
   int8_t left[audio::kMaxFrameSamples];
   f.segment_samples = samples_;
   for (uint16_t i = 0; i < samples_; ++i)
   {
      f.right[i] = fifo_byte(false, first + i);
      f.left[i] = fifo_byte(true, first + i);
      right[i] = fifo_byte(false, read_position_ + i);
      left[i] = fifo_byte(true, read_position_ + i);
   }
   f.hq = hq_pending_;
   if (hq_pending_)
   {
      f.output_samples = static_cast<uint16_t>(samples_ * audio::kHqFactor);
      model_.render_hq(hq_right_, hq_left_, f.output, f.output_samples);
   }
   else
   {
      f.output_samples = samples_;
      model_.render(right, left, f.output, samples_);
   }
   hq_pending_ = false;
   read_position_ += samples_;   // the DMA moves on; restart_dma() brings it back

   last_ = f;
   pending_ = FrameOut{};
   return last_;
}

} // namespace capsule::hal::apu
