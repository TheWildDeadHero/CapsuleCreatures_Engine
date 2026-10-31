/// @file
/// Resources (D-25): the pools a battler spends or loses, declared by the manifest (structure.json "resources",
/// tools/battlec/resources.py), and the move costs that spend them.
///
/// A **battler resource** (ResourceScope::Battler) is one pool per battler whose maximum is a stat. Exactly one of
/// them faints its battler when it is empty (ResourceEmpty::Faint): the hit points, stored in Battler::hp with the
/// pool stat (BattleData::pool_stat) as their maximum. The others live in Battler::pool (ResourceData::slot).
/// A **move resource** (ResourceScope::Move) is one pool per move slot (Battler::move_pool) whose maximum is the
/// ruleset formula ResourceData::max_formula of the move's base (MoveData::base) and the slot's raises
/// (Battler::raises, read from the ResourceData::raises role), or the base without a formula.
///
/// A resource with a persist role (ResourceData::role) is read from the party record when a battle starts and
/// written back when it ends; without one it starts full every battle. A move pays its costs (MoveCost) in the
/// manifest's resource order when it is used; a move whose costs cannot all be paid is not usable (move_usable()),
/// and Rules::fallback_move, which pays nothing, runs when no move is usable. A cost on the faint resource needs
/// more than its amount, so paying never faints the user (C1; the compiler refuses a cost no pool can reach). A
/// persist role holds its pool's highest value (the compiler checks it; the write-back clamps and asserts).
///
/// Part of the battle data (capsule/battle/data.h, which includes every part).
#pragma once

#include "capsule/battle/data/common.h"
#include "capsule/core/pod.h"
#include "capsule/monster/monster.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace capsule::battle
{

/// @name Caps (D-25; build_settings.PLATFORM_LIMITS max_battler_resources / max_move_resources)
/// Every battler holds this much storage, whatever the manifest declares (the counts are read at run time):
/// on the GBA a further battler resource costs 2 bytes per battler slot and a move resource 16 (its pool and
/// raises for kMaxMoveSlots slots), times kMaxBattlers.
///@{
#if defined(CAPSULE_PLATFORM_GBA)
inline constexpr int kMaxBattlerResources = 4;   ///< Battler resources, the hit points included (GBA: RAM).
inline constexpr int kMaxMoveResources = 2;      ///< Move resources (GBA: RAM).
#else
inline constexpr int kMaxBattlerResources = 8;   ///< Battler resources, the hit points included.
inline constexpr int kMaxMoveResources = 4;      ///< Move resources.
#endif
inline constexpr int kMaxPools = kMaxBattlerResources - 1;   ///< Battler::pool entries (the hit points are hp).
inline constexpr int kMaxResources = kMaxBattlerResources + kMaxMoveResources;   ///< Resources in all.
inline constexpr uint16_t kMaxPoolValue = 0xFFFF;   ///< Highest battler pool value (Battler::pool is 16 bits).
inline constexpr uint8_t kMaxMovePoolValue = 0xFF;  ///< Highest move pool value (Battler::move_pool is 8 bits).
///@}

/// What a resource's pools belong to (tools/battlec/resources.py SCOPES).
enum class ResourceScope : uint8_t
{
   Battler,   ///< One pool per battler; its maximum is a stat.
   Move,      ///< One pool per move slot; its maximum is a formula of the move's base and the slot's raises.
   NUM_SCOPES ///< Number of scopes.
};

/// What an empty pool means (tools/battlec/resources.py EMPTY_RULES).
enum class ResourceEmpty : uint8_t
{
   Nothing,   ///< Nothing (the pool is just empty).
   Faint,     ///< The battler faints (the hit points; exactly one battler resource).
   NUM_RULES  ///< Number of rules.
};

/// One resource (ROM; BattleData::resources in the manifest's order, which is the order costs are paid in).
struct ResourceData
{
   const text::Code* label;   ///< In-game name ("PP", "MP").
   int16_t regen;             ///< Amount the regen end-of-turn phase adds (regen_formula kNone8; negative drains).
   uint8_t scope;             ///< ResourceScope.
   uint8_t empty;             ///< ResourceEmpty.
   uint8_t slot;              ///< Battler::pool (battler) or Battler::move_pool (move) index; kNone8 for the faint
                              ///< resource (Battler::hp).
   uint8_t stat;              ///< Battler scope: the stat that is its maximum (BattleData::stats index); else kNone8.
   monster::RoleId role;      ///< The record role it persists in, or monster::kNoRole (refilled every battle).
   monster::RoleId raises;    ///< Move scope: the record role of each slot's raises, or monster::kNoRole.
   uint8_t max_formula;       ///< Move scope: rules::resource_formula() id of its maximum (base, raises), or kNone8
                              ///< (the move's base).
   uint8_t regen_formula;     ///< rules::resource_formula() id of its regen (value, max), or kNone8 (regen).
   uint8_t reserved[kPadAfterTwoBytes];   ///< Padding; 0.
};

/// One cost of a move (ROM; MoveData::cost_first / cost_count, in resource order).
struct MoveCost
{
   uint16_t amount;    ///< Amount (formula kNone8).
   uint8_t resource;   ///< The resource (BattleData::resources index).
   uint8_t formula;    ///< rules::resource_formula() id of the amount (value, max of the user's pool), or kNone8.
};
CAPSULE_POD(MoveCost, 4);

} // namespace capsule::battle
