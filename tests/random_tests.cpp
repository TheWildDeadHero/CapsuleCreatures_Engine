/// @file
/// Tests for the random generators (A-4, D-22 decision 4). Built per generator
/// (cmake/parts/rng.cmake capsule_rng_test()): the engine's default
/// (tests/support/random_default_spec.h), or a ruleset repository's own
/// generator with its description header (CAPSULE_RNG_TEST_SPEC).
/// Each build checks the linked next16() against a reference of its formula
/// and known first draws (determinism), bucket counts with an integer
/// chi-square bound (distribution), that the state does not return to its
/// start over a long sample (no short cycle), and the ranges built on top.
#include "capsule/core/random.h"

#include <cstdint>
#include <cstdio>

namespace
{

using namespace capsule;

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.

/// Records one check.
/// @param cond  condition that must hold
#define CHECK(cond)                                                                                                    \
   do                                                                                                                  \
   {                                                                                                                   \
      ++g_checks;                                                                                                      \
      if (!(cond))                                                                                                     \
      {                                                                                                                \
         ++g_failures;                                                                                                 \
         std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)

/// Draws checked against known values per seed.
constexpr int kKnownDraws = 8;

/// Seeds the tests start from (state 0, an arbitrary pattern, a high-bit pattern).
constexpr uint32_t kSeeds[] = {0x00000000u, 0x12345678u, 0xDEADBEEFu};
/// Number of seeds.
constexpr int kSeedCount = sizeof(kSeeds) / sizeof(kSeeds[0]);

/// One seed's known first draws and the state after them.
struct Known
{
   uint32_t seed;                ///< Start state.
   uint16_t draws[kKnownDraws];  ///< next16() results, in order.
   uint32_t state_after;         ///< State after the draws.
};

/// The linked generator's description: kName, reference() and kKnown (cmake/parts/rng.cmake passes
/// CAPSULE_RNG_TEST_SPEC; the engine's own is its default generator's).
#if defined(CAPSULE_RNG_TEST_SPEC)
#include CAPSULE_RNG_TEST_SPEC
#else
#include "support/random_default_spec.h"
#endif

/// The linked next16() returns the known first draws and leaves the known state.
void test_known_draws()
{
   for (const Known& k : kKnown)
   {
      rng::Stream s = {k.seed};
      for (int i = 0; i < kKnownDraws; ++i)
      {
         CHECK(rng::next16(s) == k.draws[i]);
      }
      CHECK(s.state == k.state_after);
   }
}

/// Draws compared with the reference formula per seed.
constexpr int kReferenceDraws = 1 << 16;

/// The linked next16() matches the reference formula over a long run, and two
/// streams from the same seed (or a copied stream) replay the same draws.
void test_reference_and_replay()
{
   for (int k = 0; k < kSeedCount; ++k)
   {
      rng::Stream s = {kSeeds[k]};
      uint32_t ref = kSeeds[k];
      int mismatches = 0;
      for (int i = 0; i < kReferenceDraws; ++i)
      {
         if (rng::next16(s) != reference(ref) || s.state != ref)
         {
            ++mismatches;
         }
      }
      CHECK(mismatches == 0);

      // A copy is a snapshot: both continue identically.
      rng::Stream copy = s;
      int diverged = 0;
      for (int i = 0; i < kKnownDraws; ++i)
      {
         if (rng::next16(s) != rng::next16(copy))
         {
            ++diverged;
         }
      }
      CHECK(diverged == 0);
   }
}

/// Draws per distribution sample.
constexpr uint32_t kSampleDraws = 1u << 16;
/// Buckets of the single-draw test (16: df 15).
constexpr uint32_t kBuckets = 16;
/// Chi-square bound for df 15, rounded up from 37.70 (p = 0.001).
constexpr uint64_t kChiBound15 = 38;
/// Buckets of the pair test (16 x 16: df 255).
constexpr uint32_t kPairBuckets = kBuckets * kBuckets;
/// Chi-square bound for df 255, rounded up from 330.5 (p = 0.001).
constexpr uint64_t kChiBound255 = 331;
/// Fewest distinct values in 65536 draws: a random function gives about
/// 63.2 % (41427 +- 110); 62.5 % catches stuck or short-cycled output.
constexpr int kMinDistinct = 40960;
/// Most distinct values: 64 %; more means the output is a permutation-like
/// counter rather than random draws.
constexpr int kMaxDistinct = 41943;

/// Integer chi-square test: sum((o - e)^2) / e < bound, as
/// sum((o - e)^2) * buckets < bound * draws (e = draws / buckets exactly).
/// @param counts   bucket counts
/// @param buckets  number of buckets
/// @param draws    total of the counts
/// @param bound    chi-square bound (rounded up)
/// @return true when the counts pass
bool chi_square_ok(const uint32_t* counts, uint32_t buckets, uint32_t draws, uint64_t bound)
{
   const int64_t expected = draws / buckets;
   uint64_t sum = 0;
   for (uint32_t b = 0; b < buckets; ++b)
   {
      const int64_t d = static_cast<int64_t>(counts[b]) - expected;
      sum += static_cast<uint64_t>(d * d);
   }
   return sum * buckets < bound * draws;
}

/// High buckets (below), low bits of the draw, consecutive pairs and the
/// number of distinct values all look uniform.
void test_distribution()
{
   static uint8_t seen[1u << rng::kDrawBits];
   for (int k = 0; k < kSeedCount; ++k)
   {
      rng::Stream s = {kSeeds[k]};
      uint32_t high[kBuckets] = {};
      uint32_t low[kBuckets] = {};
      uint32_t pairs[kPairBuckets] = {};
      for (uint8_t& v : seen)
      {
         v = 0;
      }
      int distinct = 0;

      for (uint32_t i = 0; i < kSampleDraws; i += 2)
      {
         const uint16_t a = rng::next16(s);
         const uint16_t b = rng::next16(s);
         const uint16_t both[] = {a, b};
         for (uint16_t v : both)
         {
            ++high[(static_cast<uint32_t>(v) * kBuckets) >> rng::kDrawBits];
            ++low[v & (kBuckets - 1)];
            if (!seen[v])
            {
               seen[v] = 1;
               ++distinct;
            }
         }
         const uint32_t ha = (static_cast<uint32_t>(a) * kBuckets) >> rng::kDrawBits;
         const uint32_t hb = (static_cast<uint32_t>(b) * kBuckets) >> rng::kDrawBits;
         ++pairs[ha * kBuckets + hb];
      }
      CHECK(chi_square_ok(high, kBuckets, kSampleDraws, kChiBound15));
      CHECK(chi_square_ok(low, kBuckets, kSampleDraws, kChiBound15));
      CHECK(chi_square_ok(pairs, kPairBuckets, kSampleDraws / 2, kChiBound255));
      CHECK(distinct >= kMinDistinct);
      CHECK(distinct <= kMaxDistinct);
   }
}

/// Steps of the no-short-cycle check per seed (2^22 of the 2^32 period).
constexpr uint32_t kCycleSteps = 1u << 22;

/// The state does not come back to its start (or get stuck) within a long
/// sample, so no seed sits on a short cycle.
void test_no_short_cycle()
{
   for (int k = 0; k < kSeedCount; ++k)
   {
      rng::Stream s = {kSeeds[k]};
      uint32_t returned = 0;
      uint32_t stuck = 0;
      for (uint32_t i = 0; i < kCycleSteps; ++i)
      {
         const uint32_t before = s.state;
         rng::next16(s);
         if (s.state == kSeeds[k])
         {
            ++returned;
         }
         if (s.state == before)
         {
            ++stuck;
         }
      }
      CHECK(returned == 0);
      CHECK(stuck == 0);
   }
}

/// Range draws on top of next16() (shared by every generator).
void test_ranges()
{
   constexpr uint32_t kOutcomes = 6;
   constexpr int32_t kLo = -3;
   constexpr int32_t kHi = 4;
   constexpr int kRangeDraws = 4096;
   rng::Stream s = {kSeeds[1]};
   int out_of_range = 0;
   for (int i = 0; i < kRangeDraws; ++i)
   {
      if (rng::below(s, kOutcomes) >= kOutcomes)
      {
         ++out_of_range;
      }
      const int32_t v = rng::between(s, kLo, kHi);
      if (v < kLo || v > kHi)
      {
         ++out_of_range;
      }
   }
   CHECK(out_of_range == 0);

   // below(n) is (draw * n) >> 16 of the same draw.
   rng::Stream a = {kSeeds[2]};
   rng::Stream b = a;
   const uint32_t r = rng::next16(b);
   CHECK(rng::below(a, kOutcomes) == ((r * kOutcomes) >> rng::kDrawBits));

   // Certain and impossible odds and below(0) (treated as one outcome: 0) draw nothing.
   rng::Stream c = {kSeeds[1]};
   CHECK(!rng::chance(c, 0, kOutcomes));
   CHECK(rng::chance(c, kOutcomes, kOutcomes));
   CHECK(c.state == kSeeds[1]);
   CHECK(rng::below(c, 0) == 0);
   CHECK(c.state == kSeeds[1]);
   CHECK(rng::below(c, 1) == 0);
   CHECK(c.state != kSeeds[1]);
}

/// mix() is the generator-independent hash-combine it always was.
void test_mix()
{
   rng::Stream s = {0x12345678u};
   rng::mix(s, 0xCAFEF00Du);
   const uint32_t start = 0x12345678u;
   const uint32_t expected = start ^ (0xCAFEF00Du + 0x9E3779B9u + (start << 6) + (start >> 2));
   CHECK(s.state == expected);
}

} // namespace

int main()
{
   test_known_draws();
   test_reference_and_replay();
   test_distribution();
   test_no_short_cycle();
   test_ranges();
   test_mix();
   std::printf("random_tests (%s): %d checks, %d failures\n", kName, g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
