/// @file
/// Random numbers (A-4): a 32-bit generator in separate streams, ranges
/// without division.
///
/// The generator is replaceable at link time (D-22 decision 4): this header
/// declares next16() and builds every range on top of it; one source file
/// defines it. The engine default is src/core/random_default.cpp (a neutral
/// permuted LCG); a game or test that needs another sequence links its own
/// definition instead (cmake/parts/rng.cmake, capsule_rng()). Every generator
/// keeps its whole state in Stream::state (32 bits), so streams stay plain
/// data and snapshots stay the same size.
///
/// The ARM7TDMI has no hardware divide, so ranges are (r16 * n) >> 16 and
/// integer odds "N in M" (P-5) are (r16 * M) >> 16 < N; nothing here uses %
/// or /.
///
/// Streams keep outcomes independent of each other:
/// - gameplay: advanced every frame by the game loop;
/// - battle: seeded once per battle, so a battle replays exactly from its
///   seed and its inputs (link lockstep, recorded battles, tests);
/// - cosmetic: effects only; never read by anything that changes an outcome.
#pragma once

#include <cstdint>

namespace capsule::rng
{

/// One generator stream (plain data: copy it to snapshot it).
struct Stream
{
   uint32_t state;   ///< Generator state (its meaning belongs to the linked generator).
};

/// Bits in one draw (next16() returns 0-65535).
inline constexpr int kDrawBits = 16;
/// mix(): the 32-bit golden ratio constant (2^32 / phi), which spreads the mixed-in value.
inline constexpr uint32_t kMixGolden = 0x9E3779B9u;
/// mix(): the state's left shift (hash-combine style).
inline constexpr int kMixShiftLeft = 6;
/// mix(): the state's right shift (hash-combine style).
inline constexpr int kMixShiftRight = 2;

/// Advances a stream and returns 16 random bits. Defined by the linked
/// generator (engine default: src/core/random_default.cpp).
/// @param s  the stream
/// @return 0-65535
uint16_t next16(Stream& s);

/// A value in [0, @p n) without division.
/// @param s  the stream
/// @param n  number of outcomes, 1-65536 (0 is treated as 1)
/// @return 0 to n - 1
inline uint32_t below(Stream& s, uint32_t n)
{
   // Scales a 16-bit draw to [0, n): (r16 * n) / 65536.
   return n == 0 ? 0 : (static_cast<uint32_t>(next16(s)) * n) >> kDrawBits;
}

/// A value in [@p lo, @p hi].
/// @param s   the stream
/// @param lo  lowest value
/// @param hi  highest value (>= lo; the span is at most 65536)
/// @return lo to hi
inline int32_t between(Stream& s, int32_t lo, int32_t hi)
{
   return lo + static_cast<int32_t>(below(s, static_cast<uint32_t>(hi - lo + 1)));
}

/// Rolls integer odds "@p n in @p m" (P-5).
/// @param s  the stream
/// @param n  successes (n >= m always succeeds, 0 never does)
/// @param m  outcomes, 1-65536
/// @return true with probability n / m
inline bool chance(Stream& s, uint32_t n, uint32_t m)
{
   if (n == 0)
   {
      return false;
   }
   if (n >= m)
   {
      return true;
   }
   return below(s, m) < n;
}

/// Mixes extra entropy into a stream (boot seeding: RTC, timers, frame counts).
/// @param s      the stream
/// @param value  value to mix in
constexpr void mix(Stream& s, uint32_t value)
{
   s.state ^= value + kMixGolden + (s.state << kMixShiftLeft) + (s.state >> kMixShiftRight);
}

/// Seeds the global gameplay and cosmetic streams with entropy (A-4): at
/// start-up and again at the first button press. Mixes into the current
/// state, so calling it more than once only adds entropy. The battle stream
/// is seeded per battle and is not touched.
/// @param entropy  boot timers, frame counts, RTC
void seed(uint32_t entropy);

/// @name The global streams
///@{

/// Gameplay stream (advance once per frame from the game loop).
extern Stream gameplay;
/// Cosmetic stream (effects only).
extern Stream cosmetic;
///@}

/// @name Per-frame advance (N1, D-32: a game whose reference draws once every vertical blank)
///@{

/// Whether the gameplay stream advances at this frame's vertical blank (the game's rule, e.g. "not in a link
/// battle").
using FrameCondition = bool (*)();

/// Turns the per-frame advance on (a condition) or off (nullptr, the default): from then on every HAL's
/// system::update() draws once from the gameplay stream after the frame's vertical blank, when the condition
/// holds, as a reference game whose vertical blank interrupt calls its generator does (between one frame's work
/// and the next).
/// @param condition  the game's rule, or nullptr
void set_frame_advance(FrameCondition condition);

/// The per-frame advance (system::update() calls it once for each vertical blank since its last call: C11, a lag
/// frame's blanks each draw; C13b: once per update() with AdvanceClock::Frame): one gameplay draw when it is on and
/// its condition holds.
void frame_advance();

/// Draws the per-frame advance made so far.
/// @return the count
uint32_t frame_advances();

/// C13b: which vertical blanks make the per-frame advance on hardware whose blanks can outnumber the frames (a lag
/// frame: the frame's work took longer than a blank).
enum class AdvanceClock : uint8_t
{
   Blank,   ///< Every hardware blank draws, a lag frame's too (C11; the default).
   Frame,   ///< One draw per system::update() (a game frame), however many blanks it took: every platform then
            ///< draws the same, and a reference game whose own frames do not lag where this build's do is matched.
   NUM_ADVANCE_CLOCKS   ///< Number of clocks.
};

/// Chooses the per-frame advance's clock (desktop and headless builds pace their frames and always draw once a
/// frame, whichever is chosen).
/// @param clock  Blank or Frame
void set_advance_clock(AdvanceClock clock);

/// The per-frame advance's clock.
/// @return Blank unless set_advance_clock() chose Frame
AdvanceClock advance_clock();

/// C13d: a game frame that also spends @p blanks hardware vertical blanks beyond its own: a reference game whose
/// frame outlasts a blank at a known point (a load, a blocking write) and draws at every blank it spans, where this
/// build's frame does not lag. Makes the per-frame advance @p blanks more times now, each under the condition as
/// the reference's interrupt checks it. The count is the game's data (measured on the reference); nothing here
/// knows a point. Meant for AdvanceClock::Frame: with Blank, this build's own lag blanks draw as well.
/// @param blanks  the extra blanks (0: nothing)
void spend_blanks(uint32_t blanks);

/// The extra blanks spend_blanks() has spent so far (the condition's skips included).
/// @return the count
uint32_t spent_blanks();
///@}

/// @name Press seeding (U5, D-33: a game whose reference never reseeds its generator after power-on)
///@{

/// Turns the first-press reseed of the streams (hal::system::seed_on_first_press(): on by default) on or off. A
/// game whose generator must follow its reference's from power-on turns it off before the first frame.
/// @param on  reseed at the first press
void set_press_seeding(bool on);

/// Whether the first press reseeds the streams.
/// @return true unless set_press_seeding(false) was called
bool press_seeding();
///@}

} // namespace capsule::rng
