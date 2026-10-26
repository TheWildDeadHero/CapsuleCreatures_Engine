/// @file
/// Creatures (C-08): the permanent stats and their stages, species with their learnsets and
/// traits.
///
/// The stats are the manifest's (D-20 C): BattleData::stats describes stat_count of them (at most
/// kMaxStats), one pool stat (hit points) and one order stat (turn order). Arrays hold kMaxStats
/// entries, so one engine library serves every game of a platform; the count is read at run time.
/// Stages (D-22 A7) are the manifest's `stages` records (StageData), in manifest order: a stat's stage, accuracy,
/// evasion or the critical-hit stage, each with its range and its table (or ruleset formula).
///
/// Part of the battle data (capsule/battle/data.h, which includes every part).
#pragma once

#include "capsule/battle/data/chart.h"
#include "capsule/battle/data/common.h"
#include "capsule/text/encode.h"

namespace capsule::battle
{

/// @name Caps (C-08a, REF-17)
///@{
inline constexpr int kStageLimit = 15;   ///< A stage's range lies within [-kStageLimit, kStageLimit] (the
                                         ///< generated kStages static_asserts it).
#if defined(CAPSULE_PLATFORM_GBA)
inline constexpr int kMaxStats = 10;   ///< Stats per creature (GBA: RAM; build_settings.PLATFORM_LIMITS).
inline constexpr int kMaxPassiveSlots = 4;   ///< Passive slots per species (GBA: ROM per species;
                                             ///< build_settings.PLATFORM_LIMITS max_passive_slots).
#else
inline constexpr int kMaxStats = 32;   ///< Stats per creature (build_settings.PLATFORM_LIMITS).
inline constexpr int kMaxPassiveSlots = 8;   ///< Passive slots per species (build_settings.PLATFORM_LIMITS).
#endif
///@}
static_assert(kMaxPassiveSlots <= 8, "BattleData::passive_rolled holds one bit per passive slot");

/// What a stage is (StageData::kind): the engine passes the stages of these kinds to the pipelines.
enum class StageKind : uint8_t
{
   Stat,       ///< A stat's stage (StageData::stat; the offense, defense and speed inputs).
   Accuracy,   ///< The user's accuracy stage (accuracy pipeline; accuracy_staged()).
   Evasion,    ///< The target's evasion stage (accuracy pipeline).
   Crit,       ///< The critical-hit stage (crit pipeline; crit_chance() / crit_out_of() read its odds).
   NUM_KINDS
};

/// @name Stages
///@{

/// Stages of the kinds that are not a stat's: accuracy, evasion and the critical-hit stage (at most one each).
inline constexpr int kSpecialStages = static_cast<int>(StageKind::NUM_KINDS) - 1;
/// Most stages: one per stat (any stat may have one, the pools included) and the special ones.
inline constexpr int kMaxStages = kMaxStats + kSpecialStages;
///@}
static_assert(kMaxStages < kNone8, "a stage index (and kNone8 for none) fits in 8 bits");

/// One stage (ROM, manifest `stages[]`): its range and how a value is scaled at each stage of it.
struct StageData
{
   const text::Code* name;   ///< In-game name (stage messages' {stat}).
   const Ratio* table;       ///< hi - lo + 1 multipliers (stage lo first), or nullptr (formula, or none: x1).
   const Odds* odds;         ///< Crit stage: hi - lo + 1 odds (stage lo first); nullptr otherwise.
   int8_t lo;                ///< Lowest stage (<= 0).
   int8_t hi;                ///< Highest stage (>= 0).
   uint8_t kind;             ///< StageKind.
   uint8_t stat;             ///< The stat of a Stat stage, or kNone8.
   uint8_t formula;          ///< The stage's ruleset formula (rules::stage_formula() id), or kNone8.
   uint8_t reserved[kPadAfterOneByte];   ///< Padding; 0.
};

/// StatInfo flag bits (P-9 bit positions).
enum StatInfoFlag : uint8_t
{
   STATINFO_TRAIT,   ///< Traits may raise or lower the stat.
   NUM_STATINFO_FLAGS
};
static_assert(fits_in<uint8_t>(NUM_STATINFO_FLAGS));

/// One permanent stat (ROM), in the manifest's order.
struct StatInfo
{
   const text::Code* label;   ///< In-game name ({stat}).
   uint8_t stage;             ///< Its stage (BattleData::stages index), or kNone8 for a stat without one.
   uint8_t flags;             ///< StatInfoFlag bits.
   uint8_t reserved[kPadAfterTwoBytes];   ///< Padding; 0.
};

/// A learnset entry (ROM).
struct LearnEntry
{
   uint8_t level;    ///< Level it is learned at (1 = known from the start).
   uint8_t reserved; ///< Padding; 0.
   uint16_t move;     ///< Move index.
};
CAPSULE_POD(LearnEntry, 4);

/// A creature species (ROM).
struct SpeciesData
{
   const text::Code* name;      ///< Display name.
   const LearnEntry* learnset;  ///< Sorted by level.
   AssetKey key;                ///< "species:NAME" (records store it, Role::SpeciesKey).
   AssetKey front;              ///< Front sprite asset ("sprite:..."), or 0.
   AssetKey back;               ///< Back sprite asset, or 0.
   uint16_t base[kMaxStats];   ///< Base stats (BattleData::stat_count used; 16 bits: a game's bases may pass 255).
   uint8_t types[kMaxTypesPer];         ///< Types (BattleData::species_types used; kNone8 for none).
   uint8_t passives[kMaxPassiveSlots];  ///< Its passive per manifest passive slot (BattleData::passive_slots used;
                                        ///< kNone8 = none: the first slot's passive).
   uint8_t growth;             ///< Growth table index.
   uint16_t exp_yield;          ///< Base experience yield.
   uint8_t learn_count;        ///< Learnset entries.
   uint8_t reserved;           ///< Padding; 0.
};

/// A trait (ROM): percent per stat, 100 = neutral (every stat without STATINFO_TRAIT).
struct TraitData
{
   const text::Code* name;          ///< Display name.
   uint8_t pct[kMaxStats];          ///< Percent per stat (BattleData::stat_count used).
   uint8_t reserved[kPadAfterOneByte];             ///< Padding; 0.
};

} // namespace capsule::battle
