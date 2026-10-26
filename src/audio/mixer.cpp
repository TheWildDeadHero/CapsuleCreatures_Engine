/// @file
/// The mixer (D-34, M4 B1a; inventory section 5): each frame it writes one segment of the 8-bit ring, both sides.
/// The segment starts as reverb (from the segment 7 frames old and the one after it) or silence; then every
/// sampled channel adds (volume x sample) >> 8 to each output byte, wrapping mod 256 (no clamping anywhere).
///
/// Sample stepping: a channel's step is 627 x its frequency (fixed-rate voices: one sample per output sample) in
/// units of 2^-23 samples. Per output sample: out = s0 + (fraction x (s1 - s0)) >> 23, then the fraction grows by
/// the step; its whole part advances the position (bits 23-29 cleared, as the original's mask) and counts down the
/// samples left; at zero a looping sample goes on at its loop start plus the overshoot, any other stops. s1 is the
/// sample after the current one (reverse: before it), so the last sample interpolates towards the byte after the
/// sample (reverse: the byte before the data, the top byte of the header's length). DPCM samples are decoded a
/// 64-sample block at a time into a cache. Reverse voices start at the end and never loop.
///
/// Speed (C14a and C15a, R10 M2), same bytes and state: the loops work on 32-bit ring words, 4 output bytes at a
/// time, with the reference driver's lane arithmetic (see "Word packing" below). Forward PCM (the instruments) keeps
/// its end as a pointer and runs the words that cannot reach the end with 4 samples written out and no end test;
/// fixed-rate forward PCM (the drum kits) reads the sample itself without interpolation; forward DPCM (the cries)
/// runs the same words over its decoded block; a channel whose two output volumes are 0 only advances, 4 steps at a
/// time; the reverb (or the clear) runs a word at a time. Reverse voices, and states the forward loops do not take,
/// go through a general cursor with the same arithmetic, a lane at a time.
///
/// The per-sample loops run as ARM code from IWRAM on the GBA (CAPSULE_IWRAM_CODE); mix() itself, the per-channel
/// dispatch, the envelope and the general forward cursors (never used with the engine's own state) run from ROM.
/// The optional high-quality desktop mode
/// (CAPSULE_AUDIO_HQ) renders the same channels from the same frame-start state at 4x the samples with 16-bit sums
/// and clamping, handing them to hal::audio::hq_block(); the 8-bit ring and every state stay as without it.
#include "audio_internal.h"

#include "capsule/core/memory.h"
#include "capsule/hal/audio.h"

namespace capsule::audio::detail
{

core::Ring g_ring;   // IWRAM .bss on the GBA (the DMA reads it; the mixer writes every byte of a segment)

namespace
{

inline constexpr uint32_t kVolumeShift = 8;          ///< (volume x sample) >> 8.
inline constexpr uint32_t kReverbShift = 9;          ///< (sum x reverb) >> 9.
inline constexpr int32_t kReverbRoundBit = 0x80;     ///< Bit 7 of the reverb value set: + 1.
inline constexpr uint8_t kReverbWrapCounter = 2;     ///< At this counter the next segment is segment 0.
inline constexpr uint32_t kLengthTopShift = 24;      ///< The length's top byte: reverse playback's last read.
inline constexpr uint32_t kBlockIndexNone = 0xFFFFFFFFu;   ///< No block decoded.
inline constexpr uint8_t kNibbleShift = 4;           ///< High nibble.
inline constexpr uint8_t kNibbleMask = 0x0F;         ///< Low nibble.
inline constexpr uint8_t kBlockLast = kDpcmBlockSamples - 1;   ///< Sample index bits within a block.
inline constexpr uint32_t kPairSamples = 2;          ///< Samples one delta byte gives (after the block's first two).
inline constexpr uint32_t kPairStart = 2;            ///< The first sample a whole delta byte gives.

/// The DPCM decoder's cache (IWRAM on the GBA, shared: one channel mixes at a time): one 64-sample block, then the
/// next block's first sample (its raw first byte), so a position in the block always finds its next sample here.
int8_t g_block[kDpcmBlockSamples + 1];
/// The block in g_block.
uint32_t g_block_index = kBlockIndexNone;

/// Decodes one 64-sample block: byte 0 is sample 0; byte 1's low nibble the delta to sample 1; each later byte the
/// next two deltas (high nibble first). The level wraps in 8 bits. The next block's byte 0 (its sample 0) follows.
/// @param data    the sample's bytes
/// @param block   the block number
/// @param deltas  the delta table (kDeltas, or a copy of it in faster memory)
CAPSULE_IWRAM_CODE void decode_block(const int8_t* data, uint32_t block, const int8_t* deltas)
{
   const uint8_t* src = reinterpret_cast<const uint8_t*>(data) + block * kDpcmBlockBytes;
   // the level wraps in 8 bits: only its low byte is stored, so it is kept in a word without masking
   uint32_t level = *src++;
   g_block[0] = static_cast<int8_t>(level);
   uint32_t pair = *src++;
   level += static_cast<uint32_t>(deltas[pair & kNibbleMask]);
   g_block[1] = static_cast<int8_t>(level);
   for (uint32_t out = kPairStart; out < kDpcmBlockSamples; out += kPairSamples)
   {
      pair = *src++;
      level += static_cast<uint32_t>(deltas[pair >> kNibbleShift]);
      g_block[out] = static_cast<int8_t>(level);
      level += static_cast<uint32_t>(deltas[pair & kNibbleMask]);
      g_block[out + 1] = static_cast<int8_t>(level);
   }
   g_block[kDpcmBlockSamples] = static_cast<int8_t>(*src);   // src is at the next block's first byte
   g_block_index = block;
}

/// The channel's step per output sample.
/// @param ch  the channel
/// @return 2^-23 sample units
uint32_t step_of(const Channel& ch)
{
   return (ch.kind & rom::kFixedRate) != 0 ? kFixedStep : g_driver.div_freq * ch.frequency;
}

/// One sample of a source that is not before its start (the hot loop's read).
/// @tparam kDpcm  DPCM blocks
/// @param data  the sample's bytes
/// @param i     its index (0 or more)
/// @return the value
template <bool kDpcm>
[[gnu::always_inline]] inline int32_t fetch(const int8_t* data, int32_t i)
{
   if constexpr (kDpcm)
   {
      const uint32_t block = static_cast<uint32_t>(i) >> kDpcmBlockShift;
      if (block != g_block_index)
      {
         decode_block(data, block, kDeltas);
      }
      return g_block[i & kBlockLast];
   }
   else
   {
      return data[i];
   }
}

/// @name Word packing (C14a, R10 M2): the reference driver's lane arithmetic
/// A ring word holds 4 output bytes. Each sample rotates the word right by 8 (the next byte comes to the top) and
/// adds (volume << 16) x value with bits 0-23 cleared: bits 24-31 of that product are bits 8-15 of volume x value,
/// which is the byte loop's (volume x value) >> 8 mod 256, and the carry out of bit 31 is lost as the byte's wrap is.
/// After 4 samples the word is back in place; a channel that ends after lane k rotates it by 8 x (3 - k) more.
///@{
inline constexpr uint32_t kLanes = 4;                     ///< Output bytes per ring word.
inline constexpr uint32_t kLaneBits = 8;                  ///< Bits per lane.
inline constexpr uint32_t kWordBits = 32;                 ///< Bits per ring word.
inline constexpr uint32_t kLaneTop = 0xFF000000u;         ///< The top lane: where a sample's byte is added.
inline constexpr uint32_t kLaneVolumeShift = 16;          ///< Volume << 16: the product's top byte is the lane's.
inline constexpr uint32_t kLaneMask = 0xFFu;              ///< One lane's bits.
inline constexpr uint32_t kLeftWords = core::kRingBytes / kLanes;   ///< From a right-side word to its left one.
static_assert(core::kRingBytes % kLanes == 0, "the left side starts on a ring word");
///@}

/// A 32-bit product whose second factor is small (a signed byte, a sample delta). On the GBA the per-sample loops are
/// ARM code, and an ARM7TDMI multiply takes 1 to 4 extra cycles by the size of its Rs operand; GCC orders the
/// factors its own way, so the multiply is written out with the small factor as Rs (1 extra cycle where the other
/// would take 3). The same product (mod 2^32) everywhere. Only for ARM code (the IWRAM loops, CAPSULE_ARM_CODE).
/// @param big    the large factor (a volume << 16, a 23-bit fraction)
/// @param small  the small factor (sign-extended to 32 bits)
/// @return big x small mod 2^32
[[gnu::always_inline]] inline uint32_t mul_small(uint32_t big, uint32_t small)
{
#if defined(CAPSULE_PLATFORM_GBA)
   uint32_t out;
   // MUL Rd, Rm, Rs: the ARM7TDMI needs Rd to differ from Rm (the early-clobber output). The second alternative
   // lets Rd be Rs when the small factor is not needed afterwards (GCC's own pattern for this core allows it): one
   // register less in the loops that keep everything in registers
   asm("mul %0, %1, %2" : "=&r,&r"(out) : "r,r"(big), "r,0"(small));
   return out;
#else
   return big * small;
#endif
}

/// A ring word rotated right by one lane (ARM: one ror).
/// @param w  the word
/// @return w rotated right by 8
[[gnu::always_inline]] inline uint32_t ror_lane(uint32_t w)
{
   return (w >> kLaneBits) | (w << (kWordBits - kLaneBits));
}

/// A ring word rotated right by @p lanes lanes (1-3).
/// @param w      the word
/// @param lanes  lanes to rotate
/// @return the rotated word
[[gnu::always_inline]] inline uint32_t ror_lanes(uint32_t w, uint32_t lanes)
{
   const uint32_t bits = lanes * kLaneBits;
   return (w >> bits) | (w << (kWordBits - bits));
}

/// Adds one sample to the top lane of a ring word, after rotating the next lane up.
/// @param w       the word
/// @param volume  the side's volume << 16
/// @param v       the sample value (any 32-bit value: only bits 8-15 of volume x v reach the lane)
/// @return the word
[[gnu::always_inline]] inline uint32_t add_lane(uint32_t w, uint32_t volume, int32_t v)
{
   return ror_lane(w) + (mul_small(volume, static_cast<uint32_t>(v)) & kLaneTop);
}

/// The loop length of a channel's sample (0: it does not loop; reverse voices never loop).
/// @param ch  the channel
/// @return the length from the loop start to the end
int32_t loop_length(const Channel& ch)
{
   return (ch.status & kChanLoop) != 0 && (ch.kind & rom::kReverse) == 0
             ? static_cast<int32_t>(ch.sample->length) - static_cast<int32_t>(ch.sample->loop_start)
             : 0;
}

/// The step of the original's position mask after a whole step: count down, at zero go on at the loop start plus
/// the overshoot (a sample that does not loop ends).
/// @param count     samples left (after the step's advance was taken off)
/// @param loop_len  the loop's length (0: no loop)
/// @return the overshoot past the loop start (count updated), or -1 when the sample ended
[[gnu::always_inline]] inline int32_t loop_overshoot(int32_t& count, int32_t loop_len)
{
   if (loop_len == 0)
   {
      return -1;
   }
   int32_t over = -count;
   count += loop_len;
   while (count <= 0)
   {
      over -= loop_len;
      count += loop_len;
   }
   return over;
}

/// A sampled channel's playback position while it mixes (DPCM and reverse voices: the cries).
/// @tparam kDpcm     DPCM blocks
/// @tparam kReverse  plays backwards (never loops; its last read is the byte before the data)
template <bool kDpcm, bool kReverse>
struct Cursor
{
   const int8_t* data;   ///< The sample's bytes.
   int32_t before;       ///< The byte before them (the top byte of the header's length).
   uint32_t step;        ///< Step per output sample, 2^-23 samples.
   int32_t loop_start;   ///< Loop start.
   int32_t loop_len;     ///< Loop length (0: no loop).
   uint32_t fw;          ///< Fraction.
   int32_t pos;          ///< Current sample.
   int32_t count;        ///< Samples left.
   int32_t s0;           ///< The current sample's value.
   int32_t delta;        ///< The next sample's value minus s0.

   /// The next sample's value (reverse: the one before; at index 0 the byte before the data).
   /// @param at  the current sample
   /// @return its neighbour's value
   [[gnu::always_inline]] int32_t next_of(int32_t at) const
   {
      if constexpr (kReverse)
      {
         return at > 0 ? fetch<kDpcm>(data, at - 1) : before;
      }
      else
      {
         return fetch<kDpcm>(data, at + 1);
      }
   }

   /// The interpolated output value: s0 + (fraction x delta) >> 23.
   /// @return the value
   [[gnu::always_inline]] int32_t value() const
   {
      return s0 + (static_cast<int32_t>(mul_small(fw, static_cast<uint32_t>(delta))) >> kFractionBits);
   }

   /// Steps one output sample: the fraction grows by the step, its whole part advances the position (bits 23-29
   /// cleared, as the original's mask) and counts down the samples left; at zero a looping sample goes on at its
   /// loop start plus the overshoot.
   /// @return false when the sample ended (it does not loop)
   [[gnu::always_inline]] bool advance()
   {
      constexpr int32_t kDir = kReverse ? -1 : 1;
      fw += step;
      const uint32_t adv = fw >> kFractionBits;
      if (adv == 0)
      {
         return true;
      }
      fw &= ~kFractionClear;
      count -= static_cast<int32_t>(adv);
      if (count <= 0)
      {
         const int32_t over = loop_overshoot(count, loop_len);
         if (over < 0)
         {
            return false;
         }
         pos = loop_start + over;
         s0 = fetch<kDpcm>(data, pos);
      }
      else if (adv == 1)
      {
         pos += kDir;
         s0 += delta;   // the previous next sample
      }
      else
      {
         pos += kDir * static_cast<int32_t>(adv);
         s0 = fetch<kDpcm>(data, pos);
      }
      delta = next_of(pos) - s0;
      return true;
   }
};

/// One lane of a word: the cursor's value into both sides, then its step.
/// @tparam Source  a Cursor
/// @param k   the lane (0-3)
/// @param c   the cursor
/// @param r   the right side's word
/// @param l   the left side's word
/// @param vr  right volume << 16
/// @param vl  left volume << 16
/// @return false when the sample ended after this lane (r and l are then rotated back into place)
template <typename Source>
[[gnu::always_inline]] inline bool lane(uint32_t k, Source& c, uint32_t& r, uint32_t& l, uint32_t vr, uint32_t vl)
{
   const int32_t v = c.value();
   r = add_lane(r, vr, v);
   l = add_lane(l, vl, v);
   if (c.advance())
   {
      return true;
   }
   if (k + 1 < kLanes)
   {
      r = ror_lanes(r, kLanes - 1 - k);   // the rest of the word is untouched
      l = ror_lanes(l, kLanes - 1 - k);
   }
   return false;
}

/// Mixes one sampled channel into a segment through a cursor, 4 output samples per ring word in a loop over the
/// lanes (the reverse voices, and the states the forward loops below do not take). At the end of a sample that does
/// not loop the channel stops and the rest of the segment is untouched; its position stays as it was at the frame's
/// start (the original's).
/// @tparam Source  a Cursor
/// @param c      the cursor (its first sample read)
/// @param ch     the channel
/// @param out    the segment's first right-side word (its left side is kLeftWords on)
/// @param words  ring words per side (samples / 4)
/// @return false when the channel ended (the caller keeps its position)
template <typename Source>
[[gnu::always_inline]] inline bool mix_words(Source& c, const Channel& ch, uint32_t* out, uint32_t words)
{
   const uint32_t vr = uint32_t{ch.env_right} << kLaneVolumeShift;
   const uint32_t vl = uint32_t{ch.env_left} << kLaneVolumeShift;
   uint32_t* const end = out + words;
   for (; out != end; ++out)
   {
      uint32_t r = out[0];
      uint32_t l = out[kLeftWords];
      bool on = true;
      for (uint32_t k = 0; on && k < kLanes; ++k)
      {
         on = lane(k, c, r, l, vr, vl);
      }
      out[0] = r;
      out[kLeftWords] = l;
      if (!on)
      {
         return false;
      }
   }
   return true;
}

/// Mixes a channel through a cursor (the rolled loop).
/// @tparam kDpcm     DPCM blocks
/// @tparam kReverse  plays backwards
/// @param ch     the channel
/// @param out    the segment's first right-side word
/// @param words  ring words per side
template <bool kDpcm, bool kReverse>
[[gnu::always_inline]] inline void mix_cursor(Channel& ch, uint32_t* out, uint32_t words)
{
   Cursor<kDpcm, kReverse> c{rom::sample_data(ch.sample), static_cast<int8_t>(ch.sample->length >> kLengthTopShift),
                             step_of(ch), static_cast<int32_t>(ch.sample->loop_start), loop_length(ch),
                             ch.fraction, ch.position, ch.count, 0, 0};
   g_block_index = kBlockIndexNone;   // each channel decodes its own blocks (the cache is shared)
   c.s0 = fetch<kDpcm>(c.data, c.pos);
   c.delta = c.next_of(c.pos) - c.s0;
   if (!mix_words(c, ch, out, words))
   {
      ch.status = 0;   // the end
      return;
   }
   ch.fraction = c.fw;
   ch.position = c.pos;
   ch.count = c.count;
}

// The copies of the mix loop (IWRAM ARM code on the GBA): the cursor ones (DPCM, reverse), then forward PCM.

/// PCM reverse.
/// @param ch     the channel
/// @param out    the segment's first right-side word
/// @param words  ring words per side
CAPSULE_IWRAM_CODE void mix_pcm_reverse(Channel& ch, uint32_t* out, uint32_t words)
{
   mix_cursor<false, true>(ch, out, words);
}

/// DPCM reverse.
/// @param ch     the channel
/// @param out    the segment's first right-side word
/// @param words  ring words per side
CAPSULE_IWRAM_CODE void mix_dpcm_reverse(Channel& ch, uint32_t* out, uint32_t words)
{
   mix_cursor<true, true>(ch, out, words);
}

/// @name Forward PCM (C15a, R10 M2 items 1 and 2): the instruments' hot path
/// For a forward PCM channel the count of samples left is always its sample's length minus its position (the
/// envelope's start sets both, every step changes both by the same amount), so these loops keep the end as a
/// pointer: `stop - at` is the count. The reference driver's rule "count down; at zero or below go on at the loop
/// start plus the overshoot" is then "at or past the end, go back by the loop length until before it". A word whose
/// four steps cannot reach the end (more than kLanes x the largest step left) runs without the end test.
///@{

/// Steps above this go through the general cursor: below it the fraction stays under 2^23 after every whole step,
/// so one step advances at most (step >> 23) + 1 samples (the original's bits 30-31, which the position mask keeps,
/// are never set).
inline constexpr uint32_t kFastStepLimit = 1u << 29;

/// A forward PCM channel while it mixes: what the word loop keeps in registers.
struct ForwardPcm
{
   const int8_t* at;   ///< The current sample.
   uint32_t fw;        ///< Fraction.
   int32_t s0;         ///< The current sample's value.
   int32_t delta;      ///< The next sample's value minus s0.
   uint32_t step;      ///< Step per output sample, 2^-23 samples.
};

/// One output sample of a word that cannot reach the end: the interpolated value s0 + (fraction x delta) >> 23 into
/// both sides, then the step.
/// @param c   the channel
/// @param r   the right side's word
/// @param l   the left side's word
/// @param vr  right volume << 16
/// @param vl  left volume << 16
[[gnu::always_inline]] inline void pcm_lane(ForwardPcm& c, uint32_t& r, uint32_t& l, uint32_t vr, uint32_t vl)
{
   const int32_t v = c.s0 + (static_cast<int32_t>(mul_small(c.fw, static_cast<uint32_t>(c.delta))) >> kFractionBits);
   r = add_lane(r, vr, v);
   l = add_lane(l, vl, v);
   c.fw += c.step;
   const uint32_t adv = c.fw >> kFractionBits;
   if (adv != 0)
   {
      c.fw &= ~kFractionClear;
      if (adv == 1)
      {
         ++c.at;
         c.s0 += c.delta;   // the previous next sample
      }
      else
      {
         c.at += adv;
         c.s0 = c.at[0];
      }
      c.delta = c.at[1] - c.s0;
   }
}

/// One output sample of a word near the end: as pcm_lane(), then the end test and the loop.
/// @param c         the channel
/// @param r         the right side's word
/// @param l         the left side's word
/// @param vr        right volume << 16
/// @param vl        left volume << 16
/// @param stop      the sample's end
/// @param loop_len  the loop's length (0: no loop)
/// @return false when the sample ended
[[gnu::always_inline]] inline bool pcm_lane_end(ForwardPcm& c, uint32_t& r, uint32_t& l, uint32_t vr, uint32_t vl,
                                                const int8_t* stop, int32_t loop_len)
{
   const int32_t v = c.s0 + (static_cast<int32_t>(mul_small(c.fw, static_cast<uint32_t>(c.delta))) >> kFractionBits);
   r = add_lane(r, vr, v);
   l = add_lane(l, vl, v);
   c.fw += c.step;
   const uint32_t adv = c.fw >> kFractionBits;
   if (adv == 0)
   {
      return true;
   }
   c.fw &= ~kFractionClear;
   c.at += adv;
   if (c.at >= stop)
   {
      if (loop_len == 0)
      {
         return false;
      }
      do
      {
         c.at -= loop_len;
      } while (c.at >= stop);
      c.s0 = c.at[0];
   }
   else if (adv == 1)
   {
      c.s0 += c.delta;
   }
   else
   {
      c.s0 = c.at[0];
   }
   c.delta = c.at[1] - c.s0;
   return true;
}

/// Rotates a word whose channel ended after lane k back into place (the lanes after it are untouched).
/// @param w  the word
/// @param k  the last lane written (0-3)
/// @return the word
[[gnu::always_inline]] inline uint32_t end_rotate(uint32_t w, uint32_t k)
{
   return k + 1 < kLanes ? ror_lanes(w, kLanes - 1 - k) : w;
}

/// Mixes whole words without the end test while the position stays below a limit (the words that cannot reach the
/// sample's end, or a DPCM block's end): the loop the instruments spend their time in.
/// @param c      the channel
/// @param out    the next right-side word (advanced past the words mixed)
/// @param end    the segment's end
/// @param base   the position the limit counts from
/// @param limit  a word starts only while c.at - base is below this
/// @param vr     right volume << 16
/// @param vl     left volume << 16
[[gnu::always_inline]] inline void fast_words(ForwardPcm& c, uint32_t*& out, const uint32_t* end, const int8_t* base,
                                              int32_t limit, uint32_t vr, uint32_t vl)
{
   for (; out != end && c.at - base < limit; ++out)
   {
      uint32_t r = out[0];
      uint32_t l = out[kLeftWords];
      pcm_lane(c, r, l, vr, vl);
      pcm_lane(c, r, l, vr, vl);
      pcm_lane(c, r, l, vr, vl);
      pcm_lane(c, r, l, vr, vl);
      out[0] = r;
      out[kLeftWords] = l;
   }
}

/// Whether a forward PCM channel can take the word loops below (else the general cursor mixes it).
/// @param ch    the channel
/// @param step  its step
/// @return true when its count is its length minus its position and the step is below kFastStepLimit
bool pcm_fast(const Channel& ch, uint32_t step)
{
   return step < kFastStepLimit && ch.fraction < kFixedStep && ch.position >= 0 &&
          static_cast<uint32_t>(ch.position) + static_cast<uint32_t>(ch.count) == ch.sample->length;
}

/// PCM forward, resampled (the instruments): 4 lanes written out per word; words near the end in a loop over the
/// lanes with the end test.
/// @param ch     the channel (pcm_fast())
/// @param out    the segment's first right-side word (its left side is kLeftWords on)
/// @param words  ring words per side
CAPSULE_IWRAM_CODE void mix_pcm(Channel& ch, uint32_t* out, uint32_t words)
{
   const int8_t* const data = rom::sample_data(ch.sample);
   const int8_t* const stop = data + ch.sample->length;
   const int32_t loop_len = loop_length(ch);
   const uint32_t vr = uint32_t{ch.env_right} << kLaneVolumeShift;
   const uint32_t vl = uint32_t{ch.env_left} << kLaneVolumeShift;
   ForwardPcm c{data + ch.position, ch.fraction, 0, 0, step_of(ch)};
   // samples a word may advance at most (see kFastStepLimit)
   const int32_t guard = static_cast<int32_t>(kLanes * ((c.step >> kFractionBits) + 1));
   c.s0 = c.at[0];
   c.delta = c.at[1] - c.s0;
   const uint32_t* const end = out + words;
   for (;;)
   {
      fast_words(c, out, end, stop, -guard, vr, vl);   // while stop - at > guard
      if (out == end)
      {
         break;
      }
      // a word near the end
      uint32_t r = out[0];
      uint32_t l = out[kLeftWords];
      for (uint32_t k = 0; k < kLanes; ++k)
      {
         if (!pcm_lane_end(c, r, l, vr, vl, stop, loop_len))
         {
            out[0] = end_rotate(r, k);
            out[kLeftWords] = end_rotate(l, k);
            ch.status = 0;   // the end; the position stays as at the frame's start
            return;
         }
      }
      out[0] = r;
      out[kLeftWords] = l;
      ++out;
   }
   ch.fraction = c.fw;
   ch.position = static_cast<int32_t>(c.at - data);
   ch.count = static_cast<int32_t>(stop - c.at);
}

/// PCM forward through the general cursor, for a state the word loops do not take (pcm_fast() false: a step of
/// 2^29 or more, or a count that is not the length minus the position; neither happens with the engine's own
/// state). ARM code in ROM (mul_small() is an ARM instruction; IWRAM is kept for the paths that run).
/// @param ch     the channel
/// @param out    the segment's first right-side word
/// @param words  ring words per side
CAPSULE_ARM_CODE void mix_pcm_general(Channel& ch, uint32_t* out, uint32_t words)
{
   mix_cursor<false, false>(ch, out, words);
}

/// DPCM forward (the cries): the position as a sample index, its block decoded (with the next block's first sample
/// after it) when the position enters the block. The values read are the cursor's: the next sample of a block's last
/// sample is the next block's first, which is its raw first byte.
/// @param ch     the channel (pcm_fast())
/// @param out    the segment's first right-side word
/// @param words  ring words per side
CAPSULE_IWRAM_CODE void mix_dpcm(Channel& ch, uint32_t* out, uint32_t words)
{
   const int8_t* const data = rom::sample_data(ch.sample);
   const int32_t length = static_cast<int32_t>(ch.sample->length);
   const int32_t loop_len = loop_length(ch);
   const uint32_t vr = uint32_t{ch.env_right} << kLaneVolumeShift;
   const uint32_t vl = uint32_t{ch.env_left} << kLaneVolumeShift;
   const uint32_t step = step_of(ch);
   uint32_t fw = ch.fraction;
   int32_t pos = ch.position;
   // the delta table on the stack (IWRAM): every decoded sample reads it
   int8_t deltas[kDeltaCount];
   for (uint8_t i = 0; i < kDeltaCount; ++i)
   {
      deltas[i] = kDeltas[i];
   }
   uint32_t block = static_cast<uint32_t>(pos) >> kDpcmBlockShift;
   decode_block(data, block, deltas);
   int32_t s0 = g_block[pos & kBlockLast];
   int32_t delta = g_block[(pos & kBlockLast) + 1] - s0;
   // samples a word may advance at most (see kFastStepLimit)
   const int32_t guard = static_cast<int32_t>(kLanes * ((step >> kFractionBits) + 1));
   const uint32_t* const end = out + words;
   for (;;)
   {
      // the words that stay inside the decoded block and before the end: forward PCM over the cache
      const int32_t base = static_cast<int32_t>(block << kDpcmBlockShift);
      const int32_t room = length - base < static_cast<int32_t>(kDpcmBlockSamples) ? length - base
                                                                                  : kDpcmBlockSamples;
      ForwardPcm c{g_block + (pos - base), fw, s0, delta, step};
      fast_words(c, out, end, g_block, room - guard, vr, vl);
      pos = base + static_cast<int32_t>(c.at - g_block);
      fw = c.fw;
      s0 = c.s0;
      delta = c.delta;
      if (out == end)
      {
         break;
      }
      // a word that may leave the block or reach the end
      uint32_t r = out[0];
      uint32_t l = out[kLeftWords];
      for (uint32_t k = 0; k < kLanes; ++k)
      {
         const int32_t v = s0 + (static_cast<int32_t>(mul_small(fw, static_cast<uint32_t>(delta))) >> kFractionBits);
         r = add_lane(r, vr, v);
         l = add_lane(l, vl, v);
         fw += step;
         uint32_t adv = fw >> kFractionBits;
         if (adv == 0)
         {
            continue;
         }
         fw &= ~kFractionClear;
         pos += static_cast<int32_t>(adv);
         if (pos >= length)
         {
            if (loop_len == 0)
            {
               out[0] = end_rotate(r, k);
               out[kLeftWords] = end_rotate(l, k);
               ch.status = 0;   // the end; the position stays as at the frame's start
               return;
            }
            do
            {
               pos -= loop_len;
            } while (pos >= length);
            adv = 0;   // the loop start's value is read below
         }
         const uint32_t b = static_cast<uint32_t>(pos) >> kDpcmBlockShift;
         if (b != block)
         {
            decode_block(data, b, deltas);
            block = b;
         }
         s0 = adv == 1 ? s0 + delta : g_block[pos & kBlockLast];   // one step: the previous next sample
         delta = g_block[(pos & kBlockLast) + 1] - s0;
      }
      out[0] = r;
      out[kLeftWords] = l;
      ++out;
   }
   ch.fraction = fw;
   ch.position = pos;
   ch.count = length - pos;
}

/// DPCM forward through the general cursor, for a state mix_dpcm() does not take (as mix_pcm_general()).
/// @param ch     the channel
/// @param out    the segment's first right-side word
/// @param words  ring words per side
CAPSULE_ARM_CODE void mix_dpcm_general(Channel& ch, uint32_t* out, uint32_t words)
{
   mix_cursor<true, false>(ch, out, words);
}

/// One output sample of a fixed-rate word: the sample itself (the fraction stays 0: no interpolation), one sample
/// per output sample.
/// @param at  the current sample (advanced)
/// @param r   the right side's word
/// @param l   the left side's word
/// @param vr  right volume << 16
/// @param vl  left volume << 16
[[gnu::always_inline]] inline void fixed_lane(const int8_t*& at, uint32_t& r, uint32_t& l, uint32_t vr, uint32_t vl)
{
   const int32_t v = *at++;
   r = add_lane(r, vr, v);
   l = add_lane(l, vl, v);
}

/// The fixed-rate fast path (R10 M2 item 2): a forward PCM voice played at one sample per output sample with no
/// fraction. The resampled loop gives the same bytes and state: its step is exactly one sample, so the fraction stays
/// 0 (bits 23-29 cleared after each whole step), the value is s0 and every step advances one sample.
/// @param ch     the channel (fixed rate, fraction 0, pcm_fast())
/// @param out    the segment's first right-side word
/// @param words  ring words per side
CAPSULE_IWRAM_CODE void mix_pcm_fixed(Channel& ch, uint32_t* out, uint32_t words)
{
   const int8_t* const data = rom::sample_data(ch.sample);
   const int8_t* const stop = data + ch.sample->length;
   const int32_t loop_len = loop_length(ch);
   const uint32_t vr = uint32_t{ch.env_right} << kLaneVolumeShift;
   const uint32_t vl = uint32_t{ch.env_left} << kLaneVolumeShift;
   const int8_t* at = data + ch.position;
   uint32_t* const end = out + words;
   for (; out != end; ++out)
   {
      uint32_t r = out[0];
      uint32_t l = out[kLeftWords];
      if (stop - at > static_cast<int32_t>(kLanes))
      {
         fixed_lane(at, r, l, vr, vl);
         fixed_lane(at, r, l, vr, vl);
         fixed_lane(at, r, l, vr, vl);
         fixed_lane(at, r, l, vr, vl);
      }
      else
      {
         for (uint32_t k = 0; k < kLanes; ++k)
         {
            fixed_lane(at, r, l, vr, vl);
            if (at >= stop)
            {
               if (loop_len == 0)
               {
                  out[0] = end_rotate(r, k);
                  out[kLeftWords] = end_rotate(l, k);
                  ch.status = 0;   // the end
                  return;
               }
               at -= loop_len;   // a step of one sample overshoots by 0: the loop start
            }
         }
      }
      out[0] = r;
      out[kLeftWords] = l;
   }
   ch.position = static_cast<int32_t>(at - data);
   ch.count = static_cast<int32_t>(stop - at);
}
///@}


/// The fraction's bits below the whole samples.
inline constexpr uint32_t kFractionLow = kFixedStep - 1;

/// A silent channel (both output volumes 0: fades, ducking, an attack of 0) (R10 M2 item 5): only the position,
/// fraction and count advance, exactly as the mix loop advances them; the ring is not touched (every lane would add
/// 0). No sample is read. Below kFastStepLimit four steps that neither end nor loop are taken at once: the fraction
/// then stays under 2^23, so their whole parts add up to (fraction + 4 x step) >> 23 and the fraction keeps the low
/// 23 bits of that sum.
/// @param ch       the channel
/// @param samples  samples per side
CAPSULE_IWRAM_CODE void skip_samples(Channel& ch, uint32_t samples)
{
   const bool reverse = (ch.kind & rom::kReverse) != 0;
   const uint32_t step = step_of(ch);
   const bool whole = step < kFastStepLimit;
   const int32_t loop_start = static_cast<int32_t>(ch.sample->loop_start);
   const int32_t loop_len = loop_length(ch);
   uint32_t fw = ch.fraction;
   int32_t pos = ch.position;
   int32_t count = ch.count;
   for (uint32_t i = 0; i < samples; ++i)
   {
      if (whole && fw < kFixedStep && samples - i >= kLanes)
      {
         const uint32_t sum = fw + kLanes * step;
         const int32_t adv = static_cast<int32_t>(sum >> kFractionBits);
         if (count - adv > 0)
         {
            fw = sum & kFractionLow;
            count -= adv;
            pos = reverse ? pos - adv : pos + adv;
            i += kLanes - 1;
            continue;
         }
      }
      fw += step;
      const uint32_t adv = fw >> kFractionBits;
      if (adv == 0)
      {
         continue;
      }
      fw &= ~kFractionClear;
      count -= static_cast<int32_t>(adv);
      if (count <= 0)
      {
         if (loop_len == 0)
         {
            ch.status = 0;   // the end
            return;
         }
         int32_t over = -count;
         count += loop_len;
         while (count <= 0)
         {
            over -= loop_len;
            count += loop_len;
         }
         pos = loop_start + over;
      }
      else
      {
         pos = reverse ? pos - static_cast<int32_t>(adv) : pos + static_cast<int32_t>(adv);
      }
   }
   ch.fraction = fw;
   ch.position = pos;
   ch.count = count;
}

/// @name The reverb a word at a time (C14a)
/// Each byte gets 128 added (an exclusive or), so the four bytes of one lane sum without sign extension; the even
/// lanes (0, 2) and the odd ones (1, 3) sum in two 16-bit fields of one word each (at most 4 x 255: no carry
/// between the fields); 4 x 128 comes off each sum.
///@{
inline constexpr uint32_t kByteBias = 0x80808080u;   ///< 128 in every lane.
inline constexpr uint32_t kFieldLanes = 0x00FF00FFu; ///< Lanes 0 and 2, one per 16-bit field.
inline constexpr uint32_t kFieldMask = 0xFFFFu;      ///< One 16-bit field.
inline constexpr uint32_t kFieldBits = 16;           ///< Bits per field.
inline constexpr int32_t kByteHalf = 0x80;            ///< The bias of one byte.
inline constexpr int32_t kSumBias = static_cast<int32_t>(kLanes) * kByteHalf;   ///< The bias of a four-byte sum.
///@}

/// The reverb of one lane: (sum x reverb) >> 9, plus 1 when bit 7 of the result is set (the original's rounding).
/// @param sum     the four bytes' sum
/// @param reverb  the reverb, 1-127
/// @return the lane's byte (low 8 bits)
[[gnu::always_inline]] inline uint32_t reverb_lane(int32_t sum, uint32_t reverb)
{
   int32_t v = static_cast<int32_t>(mul_small(static_cast<uint32_t>(sum), reverb)) >> kReverbShift;
   if ((v & kReverbRoundBit) != 0)
   {
      ++v;
   }
   return static_cast<uint32_t>(v) & kLaneMask;
}

/// The sums of the even lanes (0 and 2) of four words, one per 16-bit field (the words biased: kByteBias).
/// @param a  a biased word
/// @param b  a biased word
/// @param c  a biased word
/// @param d  a biased word
/// @return lane 0's sum in bits 0-15, lane 2's in bits 16-31
[[gnu::always_inline]] inline uint32_t field_sums(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
   return (a & kFieldLanes) + (b & kFieldLanes) + (c & kFieldLanes) + (d & kFieldLanes);
}

/// Starts the segment (C14a: a word at a time): mono reverb from both sides of this segment (7 frames old) and of
/// the next, scaled, on both sides; or silence.
/// @param out     the segment's first right-side word (its left side is kLeftWords on)
/// @param next    the next segment's first right-side word
/// @param words   ring words per side
/// @param reverb  the reverb, 0-127 (0: silence)
CAPSULE_IWRAM_CODE void begin_segment(uint32_t* out, const uint32_t* next, uint32_t words, uint8_t reverb)
{
   uint32_t* const end = out + words;
   if (reverb == 0)
   {
      for (; out != end; ++out)
      {
         out[0] = 0;
         out[kLeftWords] = 0;
      }
      return;
   }
   for (; out != end; ++out, ++next)
   {
      const uint32_t a = out[0] ^ kByteBias;
      const uint32_t b = out[kLeftWords] ^ kByteBias;
      const uint32_t c = next[0] ^ kByteBias;
      const uint32_t d = next[kLeftWords] ^ kByteBias;
      const uint32_t even = field_sums(a, b, c, d);
      const uint32_t odd = field_sums(a >> kLaneBits, b >> kLaneBits, c >> kLaneBits, d >> kLaneBits);
      const uint32_t w = reverb_lane(static_cast<int32_t>(even & kFieldMask) - kSumBias, reverb) |
                         reverb_lane(static_cast<int32_t>(odd & kFieldMask) - kSumBias, reverb) << kLaneBits |
                         reverb_lane(static_cast<int32_t>(even >> kFieldBits) - kSumBias, reverb) << kFieldBits |
                         reverb_lane(static_cast<int32_t>(odd >> kFieldBits) - kSumBias, reverb)
                            << (kFieldBits + kLaneBits);
      out[0] = w;
      out[kLeftWords] = w;
   }
}

/// Mixes one sampled channel with the copy for its source (silent channels only advance; forward fixed-rate PCM
/// takes the fast path).
/// @param ch       the channel
/// @param out      the segment's first right-side word
/// @param samples  samples per side
void mix_channel(Channel& ch, uint32_t* out, uint32_t samples)
{
   if (ch.env_right == 0 && ch.env_left == 0)
   {
      skip_samples(ch, samples);
      return;
   }
   const uint32_t words = samples / kLanes;
   const bool dpcm = ch.sample->format != rom::kPcm8;
   const bool reverse = (ch.kind & rom::kReverse) != 0;
   if (dpcm && reverse)
   {
      mix_dpcm_reverse(ch, out, words);
   }
   else if (dpcm)
   {
      pcm_fast(ch, step_of(ch)) ? mix_dpcm(ch, out, words) : mix_dpcm_general(ch, out, words);
   }
   else if (reverse)
   {
      mix_pcm_reverse(ch, out, words);
   }
   else if (!pcm_fast(ch, step_of(ch)))
   {
      mix_pcm_general(ch, out, words);   // a state the word loops do not take (never in practice)
   }
   else if ((ch.kind & rom::kFixedRate) != 0 && ch.fraction == 0)
   {
      mix_pcm_fixed(ch, out, words);
   }
   else
   {
      mix_pcm(ch, out, words);
   }
}

#if defined(CAPSULE_AUDIO_HQ)

inline constexpr uint32_t kHqSamples = uint32_t(hal::audio::kHqFactor) * hal::audio::kFrameSamples;   ///< Per side.
inline constexpr uint32_t kHqFractionBits = kFractionBits + 2;   ///< 4 sub-samples per output sample.
inline constexpr uint32_t kHqFractionMask = (1u << kHqFractionBits) - 1;   ///< The sub-sample fraction.
inline constexpr uint32_t kFractionMask = (1u << kFractionBits) - 1;       ///< The 23-bit fraction.
inline constexpr uint32_t kHqToFraction = kHqFractionBits - kFractionBits; ///< 23 -> 25 bits.
inline constexpr uint32_t kHqScaleShift = 8;   ///< Values in 1/256 of an 8-bit step.
inline constexpr uint32_t kHqInterpShift = kHqFractionBits - kHqScaleShift;   ///< fraction x delta >> 17.
inline constexpr int32_t kHqMax = 32767;       ///< Clamp.
inline constexpr int32_t kHqMin = -32768;      ///< Clamp.
inline constexpr uint8_t kSides = 2;           ///< Right, then left.

/// What the high-quality loop reads of a channel's sample.
struct Source
{
   const int8_t* data;   ///< The sample's bytes.
   int32_t before;       ///< The byte before them (the length's top byte).
   bool dpcm;            ///< DPCM blocks.
};

/// A sample value.
/// @param s  the source
/// @param i  its index (-1: the byte before the data)
/// @return the value
[[gnu::always_inline]] inline int32_t sample_at(const Source& s, int32_t i)
{
   if (i < 0)
   {
      return s.before;
   }
   if (!s.dpcm)
   {
      return s.data[i];
   }
   const uint32_t block = static_cast<uint32_t>(i) >> kDpcmBlockShift;
   if (block != g_block_index)
   {
      decode_block(s.data, block, kDeltas);
   }
   return g_block[i & kBlockLast];
}

/// The source of a channel's sample.
/// @param ch  the channel
/// @return it
Source source_of(const Channel& ch)
{
   return {rom::sample_data(ch.sample), static_cast<int8_t>(ch.sample->length >> kLengthTopShift),
           ch.sample->format != rom::kPcm8};
}

int32_t g_hq_right[kHqSamples];   ///< The frame's sums, right.
int32_t g_hq_left[kHqSamples];    ///< The frame's sums, left.
int16_t g_hq_history[core::kSegments][kSides][kHqSamples];   ///< Every segment's output (reverb), right then left.
int16_t g_hq_out[kSides][kHqSamples];  ///< The frame handed to the backend.

/// Starts the frame's high-quality sums: reverb from the history (as the 8-bit mixer's), or silence.
/// @param seg   the segment written
/// @param next  the segment after it
void hq_begin(uint8_t seg, uint8_t next)
{
   const uint8_t r = g_driver.reverb;
   for (uint32_t i = 0; i < kHqSamples; ++i)
   {
      int32_t v = 0;
      if (r != 0)
      {
         const int32_t s = g_hq_history[seg][0][i] + g_hq_history[seg][1][i] + g_hq_history[next][0][i] +
                           g_hq_history[next][1][i];
         v = (s * r) >> kReverbShift;
      }
      g_hq_right[i] = v;
      g_hq_left[i] = v;
   }
}

/// Renders one channel from its frame-start state (the state itself does not change).
/// @param ch  the channel (after its envelope step)
void hq_channel(const Channel& ch)
{
   const Source src = source_of(ch);
   const bool reverse = (ch.kind & rom::kReverse) != 0;
   const int32_t dir = reverse ? -1 : 1;
   const uint32_t step = step_of(ch);
   const int32_t loop_start = static_cast<int32_t>(ch.sample->loop_start);
   const int32_t loop_len = (ch.status & kChanLoop) != 0 && !reverse
                                ? static_cast<int32_t>(ch.sample->length) - loop_start
                                : 0;
   g_block_index = kBlockIndexNone;
   uint32_t fw = (ch.fraction & kFractionMask) << kHqToFraction;
   int32_t pos = ch.position;
   int32_t count = ch.count;
   int32_t s0 = sample_at(src, pos);
   int32_t s1 = sample_at(src, pos + dir);
   for (uint32_t i = 0; i < kHqSamples; ++i)
   {
      const int64_t delta = s1 - s0;
      const int32_t v = static_cast<int32_t>((int64_t(s0) << kHqScaleShift) + ((int64_t(fw) * delta) >> kHqInterpShift));
      g_hq_right[i] += (ch.env_right * v) >> kVolumeShift;
      g_hq_left[i] += (ch.env_left * v) >> kVolumeShift;
      fw += step;
      const uint32_t adv = fw >> kHqFractionBits;
      if (adv == 0)
      {
         continue;
      }
      fw &= kHqFractionMask;
      count -= static_cast<int32_t>(adv);
      if (count <= 0)
      {
         if (loop_len == 0)
         {
            return;
         }
         int32_t over = -count;
         count += loop_len;
         while (count <= 0)
         {
            over -= loop_len;
            count += loop_len;
         }
         pos = loop_start + over;
      }
      else
      {
         pos += dir * static_cast<int32_t>(adv);
      }
      s0 = sample_at(src, pos);
      s1 = sample_at(src, pos + dir);
   }
}

/// Clamps the frame, keeps it for the reverb and hands it to the backend.
/// @param seg  the segment written
void hq_end(uint8_t seg)
{
   for (uint32_t i = 0; i < kHqSamples; ++i)
   {
      const int32_t r = g_hq_right[i] > kHqMax ? kHqMax : (g_hq_right[i] < kHqMin ? kHqMin : g_hq_right[i]);
      const int32_t l = g_hq_left[i] > kHqMax ? kHqMax : (g_hq_left[i] < kHqMin ? kHqMin : g_hq_left[i]);
      g_hq_history[seg][0][i] = static_cast<int16_t>(r);
      g_hq_history[seg][1][i] = static_cast<int16_t>(l);
      g_hq_out[0][i] = static_cast<int16_t>(r);
      g_hq_out[1][i] = static_cast<int16_t>(l);
   }
   hal::audio::hq_block(g_hq_out[0], g_hq_out[1]);
}

#endif

}   // namespace

void mix()
{
   const uint32_t samples = g_driver.frame_samples;
   const uint8_t counter = g_driver.dma_counter;
   // The segment after the one the DMA plays: 0 at a counter of 1 or 0, else period - (counter - 1).
   const uint8_t seg = counter <= 1 ? 0 : static_cast<uint8_t>(g_driver.dma_period - (counter - 1));
   const uint8_t next = counter == kReverbWrapCounter ? 0 : static_cast<uint8_t>(seg + 1);
   // Every rate's frame is a whole number of ring words (kRateSamples: multiples of 4), the ring is word-aligned.
   uint32_t* const out = reinterpret_cast<uint32_t*>(g_ring.right + seg * samples);
   const uint32_t* const next_out = reinterpret_cast<const uint32_t*>(g_ring.right + next * samples);
   begin_segment(out, next_out, samples / kLanes, g_driver.reverb);
#if defined(CAPSULE_AUDIO_HQ)
   hq_begin(seg, next);
#endif
   for (uint8_t c = 0; c < g_driver.channels; ++c)
   {
      Channel& ch = g_channels[c];
      if ((ch.status & kChanOn) == 0 || !sampled_envelope(ch))
      {
         continue;
      }
      if (((ch.kind & rom::kReverse) != 0 || ch.sample->format != rom::kPcm8) && (ch.status & kChanDecoder) == 0)
      {
         ch.status |= kChanDecoder;   // the original's reverse / compressed set-up flag
      }
#if defined(CAPSULE_AUDIO_HQ)
      hq_channel(ch);
#endif
      mix_channel(ch, out, samples);
   }
#if defined(CAPSULE_AUDIO_HQ)
   hq_end(seg);
#endif
   g_driver.segment = seg;
}

void clear_ring()
{
   for (int i = 0; i < core::kRingBytes; ++i)
   {
      g_ring.right[i] = 0;
      g_ring.left[i] = 0;
   }
#if defined(CAPSULE_AUDIO_HQ)
   for (auto& seg : g_hq_history)
   {
      for (auto& side : seg)
      {
         for (int16_t& v : side)
         {
            v = 0;
         }
      }
   }
#endif
}

}   // namespace capsule::audio::detail
