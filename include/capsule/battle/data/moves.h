/// @file
/// Moves (C-08g): move categories, targets and flags, moves, passives and held items. Their logic is effect
/// code (capsule/battle/effect.h): each is an owner of BattleData::effects.
///
/// Part of the battle data (capsule/battle/data.h, which includes every part).
#pragma once

#include "capsule/battle/data/common.h"
#include "capsule/battle/data/resources.h"
#include "capsule/monster/monster.h"
#include "capsule/text/encode.h"

namespace capsule::battle
{

/// @name Caps (C-08a, REF-17)
///@{
inline constexpr int kMaxMoveSlots = monster::kMaxMoveSlots;   ///< Move slots a battler holds (the layout uses
                                                               ///< move_slots() of them).
inline constexpr int kMaxCategories = 4;   ///< Move categories (the generated table static_asserts it).
///@}

/// Move category flag bits (CategoryData::flags; P-9 bit positions).
enum CategoryFlag : uint8_t
{
   CATEGORY_DAMAGING,        ///< Moves of the category deal formula damage (offense and defense stats are set).
   NUM_CATEGORY_FLAGS        ///< Number of flags.
};
static_assert(fits_in<uint8_t>(NUM_CATEGORY_FLAGS));

/// A move category (ROM): which stats its damage uses. MoveData::category
/// indexes BattleData::categories.
struct CategoryData
{
   uint8_t offense;    ///< Attacker's stat (BattleData::stats index), or kNone8 for a non-damaging category.
   uint8_t defense;    ///< Defender's stat (BattleData::stats index), or kNone8.
   uint8_t flags;      ///< CategoryFlag bits.
   uint8_t reserved;   ///< Padding; 0.
};
CAPSULE_POD(CategoryData, 4);

/// Who a move targets (geometry comes from the format's reach masks).
enum class Target : uint8_t
{
   Selected,     ///< One reachable battler other than the user, chosen (foe by default).
   SelectedFoe,  ///< One reachable foe, chosen.
   Self,         ///< The user.
   Ally,         ///< One reachable ally.
   AllFoes,      ///< Every reachable foe (spread).
   AllOthers,    ///< Every reachable battler other than the user (spread).
   RandomFoe,    ///< One reachable foe at random.
   Field,        ///< The whole field (weather): no battler target.
   NUM_TARGETS   ///< Number of kinds.
};

/// A move (ROM). Its logic (use, target, try_hit, hit and after_move phases) is its owner in
/// BattleData::effects; the AI's estimates read the summary fields. Its costs (D-25) are
/// BattleData::move_costs[cost_first .. cost_first + cost_count - 1].
struct MoveData
{
   const text::Code* name;     ///< Display name.
   uint16_t owner;            ///< Its owner in BattleData::effects (kNone16: none).
   int16_t fixed_amount;      ///< AI: HP its fixed damage takes (amounts).
   uint16_t fixed_fraction;   ///< AI: Q8 share of the target's HP its fixed damage takes (0: none).
   uint16_t flags;            ///< Move flag bits: bit n = the manifest's move_flags[n] (effect `move.has(flag)`).
   uint16_t cost_first;       ///< Its first cost in BattleData::move_costs.
   uint16_t power;            ///< Base power (0 for none; 16 bits: a game's powers may pass 255).
   uint8_t type;              ///< Type index.
   uint8_t category;          ///< Category index (BattleData::categories).
   uint8_t accuracy;          ///< Percent, 1-100 (0 = never misses; a percent needs no more than 8 bits).
   int8_t priority;          ///< Turn-order bracket.
   Target target;            ///< Targeting.
   uint8_t crit_stage;        ///< Added critical-hit stage.
   uint8_t statuses;          ///< AI: statuses it inflicts (effects).
   uint8_t fixed_levels;      ///< AI: fixed damage of the user's level, this many times.
   uint8_t formula;           ///< 1: it deals formula damage (the damage pipeline).
   uint8_t cost_count;        ///< Its costs (0: free).
   uint8_t base[kMaxMoveResources];   ///< Its base per move resource (ResourceData::slot order; e.g. base PP).
};

/// What a delayed hit's landing reads from the snapshot of its user taken when the move was used (DelayedHitData::
/// at_use bits; manifest delayed_hits[].at_use, G1). What a record leaves out is read from the user when it lands.
enum DelayedAtUse : uint8_t
{
   DELAYED_STATS,    ///< The user's stats.
   DELAYED_STAGES,   ///< Its stages.
   DELAYED_LEVEL,    ///< Its level.
   DELAYED_TYPES,    ///< Its types (the same-type check).
   DELAYED_TARGET_DEFENSE,   ///< The target's stat that resists the move's category, and its stage (the battler
                             ///< at the position when the move was used; classic-style damage worked out on use).
   NUM_DELAYED_AT_USE
};

/// Delayed hit flag bits (DelayedHitData::flags; P-9 bit positions).
enum DelayedHitFlag : uint8_t
{
   DELAYED_TYPED,   ///< The hit has the move's type (chart and same-type check); else it has none (neutral).
   DELAYED_CRIT,    ///< It may be a critical hit (the crit pipeline runs); else it never is.
   NUM_DELAYED_HIT_FLAGS
};

/// A delayed hit (ROM, structure delayed_hits, G1): effect `delay_hit` keeps a snapshot of the move's user; when it
/// lands, the move's land phase `damage` works the hit out through the normal damage pipeline (decisive tiers, the
/// crit and damage pipelines, before_damage and damaging_hit hooks), first checking accuracy when it has one.
struct DelayedHitData
{
   uint8_t at_use;     ///< DelayedAtUse bits: what the landing reads from the snapshot.
   uint8_t flags;      ///< DelayedHitFlag bits.
   uint8_t accuracy;   ///< The landing's accuracy percent (the accuracy pipeline; 0: no check).
   uint8_t missed;     ///< The message a missed landing says (Msg id; the record's miss_message), or kNone8.
};
CAPSULE_POD(DelayedHitData, 4);

/// A passive or held item (ROM): its name and its logic (hook and point handlers).
struct HolderData
{
   const text::Code* name;     ///< Display name.
   uint16_t owner;            ///< Its owner in BattleData::effects (kNone16: none).
   uint16_t reserved;         ///< Padding; 0.
};

} // namespace capsule::battle
