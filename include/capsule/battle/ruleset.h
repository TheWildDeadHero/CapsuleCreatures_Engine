/// @file
/// Ruleset formula slots and pipeline helpers (P-13, P-14, P-18).
///
/// The formulas behind stats, experience, rewards, escape and resources (D-25) are not engine
/// code: a game's ruleset manifest (`capsule.ruleset/2`) gives them as
/// restricted integer expressions, and tools/ruleset_compiler.py turns them
/// into the functions below (in the build's generated battle data). The
/// compiler bounds every intermediate value over the declared input ranges
/// and refuses formulas that could overflow int32 or divide by zero; each
/// function clamps its result to its declared output range. The engine
/// library carries trivial defaults that a build's generated ones replace at
/// link time.
///
/// The slots and their inputs are the engine's contract with rulesets
/// (ruleset_compiler.FORMULA_SLOTS lists the same names in the same order).
/// stat() calls the stat formula the battle manifest names for each stat
/// (ruleset stat_formulas, inputs ruleset_compiler.STAT_INPUTS). Damage,
/// speed, accuracy and critical hits are pipelines (capsule/battle/pipeline.h);
/// the named formulas they call (damage_base in the sample) stay inside the
/// generated file. derive() is not a ruleset slot: the battle manifest's
/// `generation` list gives its expressions (tools/battlec/progression.py).
#pragma once

#include <cstdint>

namespace capsule::monster
{
struct Record;   // capsule/monster/monster.h (derive() reads one)
} // namespace capsule::monster

namespace capsule::rules
{

/// @name Helpers used by generated formulas
///@{

/// isqrt(): the highest even bit an int32_t value can have set (bit 30).
inline constexpr uint32_t kIsqrtTopBit = 1u << 30;
/// isqrt(): bits the trial bit moves per step (one result bit is two value bits).
inline constexpr int kIsqrtStepBits = 2;

/// Clamps @p v to [@p lo, @p hi].
/// @param v   value
/// @param lo  lowest
/// @param hi  highest
/// @return clamped value
constexpr int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}
/// Smaller of two values.
/// @param a  value
/// @param b  value
/// @return min(a, b)
constexpr int32_t min32(int32_t a, int32_t b)
{
   return a < b ? a : b;
}
/// Larger of two values.
/// @param a  value
/// @param b  value
/// @return max(a, b)
constexpr int32_t max32(int32_t a, int32_t b)
{
   return a > b ? a : b;
}
/// Absolute value (the compiler proved it cannot overflow).
/// @param a  value
/// @return |a|
constexpr int32_t abs32(int32_t a)
{
   return a < 0 ? -a : a;
}
/// Floor square root (bit by bit, no division).
/// @param v  value >= 0
/// @return floor(sqrt(v))
constexpr int32_t isqrt(int32_t v)
{
   uint32_t x = static_cast<uint32_t>(v);
   uint32_t r = 0;
   uint32_t bit = kIsqrtTopBit;
   while (bit > x)
   {
      bit >>= kIsqrtStepBits;
   }
   while (bit != 0)
   {
      if (x >= r + bit)
      {
         x -= r + bit;
         r = (r >> 1) + bit;
      }
      else
      {
         r >>= 1;
      }
      bit >>= kIsqrtStepBits;
   }
   return static_cast<int32_t>(r);
}
/// Ceiling square root.
/// @param v  value >= 0
/// @return ceil(sqrt(v))
constexpr int32_t isqrt_ceil(int32_t v)
{
   const int32_t r = isqrt(v);
   return r * r == v ? r : r + 1;
}
static_assert(isqrt(0) == 0 && isqrt(15) == 3 && isqrt(16) == 4 && isqrt(2147395600) == 46340);
static_assert(isqrt_ceil(15) == 4 && isqrt_ceil(16) == 4);

/// mulq8(): bits a Q8 factor is scaled by.
inline constexpr int kMulQ8Shift = 8;
/// pow2(): most doublings.
inline constexpr int32_t kPow2MaxUp = 8;
/// pow2(): most halvings.
inline constexpr int32_t kPow2MaxDown = 16;

/// A value times a Q8 factor, truncated (the compiler proved both non-negative and the product inside int32).
/// @param a  value
/// @param b  Q8 factor
/// @return (a * b) >> 8
constexpr int32_t mulq8(int32_t a, int32_t b)
{
   return (a * b) >> kMulQ8Shift;
}
/// A value times 2^e, truncating, with the exponent capped at kPow2MaxUp doublings and kPow2MaxDown halvings
/// (type chart exponents; the compiler proved the value non-negative and the result inside int32).
/// @param v  value
/// @param e  exponent
/// @return v * 2^e
constexpr int32_t pow2(int32_t v, int32_t e)
{
   if (e >= 0)
   {
      return v << (e > kPow2MaxUp ? kPow2MaxUp : e);
   }
   return v >> (-e > kPow2MaxDown ? kPow2MaxDown : -e);
}
static_assert(mulq8(100, 384) == 150 && pow2(10, 2) == 40 && pow2(10, -1) == 5 && pow2(1, 20) == 256);
///@}

/// @name Formula slots (generated per game)
///@{

/// A permanent stat: the ruleset stat formula the manifest names for that stat (D-20 C).
/// @param index       stat index (BattleData::stats)
/// @param base        species base stat
/// @param gene        gene
/// @param training    training value
/// @param awaken      awakening value
/// @param level       level
/// @param trait_pct   trait percent for this stat (100 = neutral; stats without STATINFO_TRAIT pass 100)
/// @return the stat
int32_t stat(int32_t index, int32_t base, int32_t gene, int32_t training, int32_t awaken, int32_t level,
             int32_t trait_pct);

/// Experience for one participant when a foe faints.
/// @param yield         fainted species' experience yield
/// @param level         fainted battler's level
/// @param victor_level  receiving battler's level
/// @param participants  battlers sharing the award (>= 1)
/// @param foe_rank      the fainted battler's party rank (0..rank_max; parties without data: their
///                      participant's rank)
/// @param outsider      1 if the receiver's original owner is not the player
/// @return experience points
int32_t exp_gain(int32_t yield, int32_t level, int32_t victor_level, int32_t participants, int32_t foe_rank,
                 int32_t outsider);

/// Reward for one defeated party (summed over the defeated parties).
/// @param reward_base  the party's reward base (PartyData::reward)
/// @param level        level of the party's last member
/// @return reward
int32_t reward(int32_t reward_base, int32_t level);

/// Outcomes escape_odds() counts out of (that many or more always escapes).
inline constexpr int32_t kEscapeOutcomes = 256;

/// Escape odds out of 256 (256 or more always escapes).
/// @param speed      fleeing battler's Speed
/// @param foe_speed  fastest foe's Speed
/// @param attempts   earlier attempts this battle
/// @return odds
int32_t escape_odds(int32_t speed, int32_t foe_speed, int32_t attempts);

/// A resource formula (D-25; the battle compiler's dispatch over the ruleset's named formulas the manifest's
/// resources name, tools/battlec/resources.py formula_ids()): a move resource's maximum (base, raises), a regen
/// or a cost (value, max of the pool). The engine clamps the result to the pool.
/// @param formula  ResourceData::max_formula / regen_formula or MoveCost::formula
/// @param a        first argument (base, or the pool's value)
/// @param b        second argument (raises, or the pool's maximum)
/// @return the formula's value (@p a for an unknown id)
int32_t resource_formula(int32_t formula, int32_t a, int32_t b);

/// A derived role's value (manifest `generation` entry {"role", "derived": expression}; D-23 section 2): the
/// expression over the record's single-element roles, generated per game. make_monster() stores it when the
/// layout holds the role; battle::record_value() computes it when the layout does not.
/// @param index    the derived entry (GenData::derived, in generation order)
/// @param element  the role element it is asked for
/// @param rec      the record (as far as it is built)
/// @return the value, clamped to the entry's range
int32_t derive(int32_t index, int32_t element, const monster::Record& rec);

/// A species field (structure `species_fields`, E13a) of a species, generated per game: what effect logic reads as
/// `holder.species_field(NAME)` (E13b).
/// @param species  species index (BattleData::species)
/// @param field    species_fields index
/// @return the value (the field's default for a species the data does not have; 0 for no such field)
int32_t species_field_of(int32_t species, int32_t field);
///@}

} // namespace capsule::rules
