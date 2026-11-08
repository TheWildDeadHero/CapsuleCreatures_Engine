/// @file
/// The field script VM's module dispatch (D-36 package 2, step B; engine-internal). The ops of the engine's
/// gameplay modules (the bag, the boxes, party roles, growth, combination, the nursery and, since C1, battles) run
/// through a table instead of the VM's own switch, and so do the waits they set. Each game links its own table,
/// written by `tools/script_vm.py dispatch` from its scripts' usage record with only the ops they use: a module no
/// script reaches is then not linked. src/script/script_dispatch_all.cpp is the table with every op (the default,
/// used by programs without a generated one). src/script/script_vm.json names the ops ("module", "handler") and the
/// waits ("polls").
#pragma once

#include "capsule/script/ops.h"
#include "capsule/script/script.h"

#include <array>
#include <cstdint>

namespace capsule::script::detail
{

/// What one instruction ended with.
enum class Outcome : uint8_t
{
   Continue,   ///< More instructions.
   Waiting,    ///< The context waits.
   Ended,      ///< The context has no more frames.
   True,       ///< TriggerTrue (conditions).
};

/// A module op: runs one instruction (the context, the opcode, the position after it, the script's first byte).
using OpHandler = Outcome (*)(Context& c, Op op, const uint8_t* p, const uint8_t* base);
/// A module wait: true while the context still waits (it writes the result variable when the wait ends).
using WaitPoll = bool (*)(Context& c);

/// Opcodes in a dispatch table.
inline constexpr int kOpCount = static_cast<int>(Op::NUM_OPS);
/// Wait kinds in a dispatch table.
inline constexpr int kWaitCount = static_cast<int>(Wait::NUM_WAITS);

/// The game's module ops by opcode (nullptr: not a module op, or one its scripts never use).
extern const OpHandler kModuleOps[kOpCount];
/// The game's module waits by wait kind (nullptr: not a module wait, or one no op of the table sets).
extern const std::array<WaitPoll, kWaitCount> kModulePolls;

/// @name The module ops (src/script/script.cpp)
///@{
Outcome op_role_get(Context& c, Op op, const uint8_t* p, const uint8_t* base);      ///< RoleGet.
Outcome op_role_set(Context& c, Op op, const uint8_t* p, const uint8_t* base);      ///< RoleSet.
Outcome op_item_give(Context& c, Op op, const uint8_t* p, const uint8_t* base);     ///< ItemGive / ItemTake.
Outcome op_item_count(Context& c, Op op, const uint8_t* p, const uint8_t* base);    ///< ItemCount.
Outcome op_item_query(Context& c, Op op, const uint8_t* p, const uint8_t* base);    ///< C7: ItemQuery.
Outcome op_party(Context& c, Op op, const uint8_t* p, const uint8_t* base);         ///< C7: Party.
Outcome op_bag_open(Context& c, Op op, const uint8_t* p, const uint8_t* base);      ///< BagOpen.
Outcome op_shop(Context& c, Op op, const uint8_t* p, const uint8_t* base);          ///< Shop.
Outcome op_mart(Context& c, Op op, const uint8_t* p, const uint8_t* base);          ///< Mart.
Outcome op_box_count(Context& c, Op op, const uint8_t* p, const uint8_t* base);     ///< BoxCount.
Outcome op_box_deposit(Context& c, Op op, const uint8_t* p, const uint8_t* base);   ///< BoxDeposit / BoxWithdraw.
Outcome op_boxes_open(Context& c, Op op, const uint8_t* p, const uint8_t* base);    ///< BoxesOpen.
Outcome op_transform(Context& c, Op op, const uint8_t* p, const uint8_t* base);     ///< Transform.
Outcome op_combine(Context& c, Op op, const uint8_t* p, const uint8_t* base);       ///< Combine.
Outcome op_nursery(Context& c, Op op, const uint8_t* p, const uint8_t* base);       ///< Nursery.
Outcome op_encounter(Context& c, Op op, const uint8_t* p, const uint8_t* base);     ///< Encounter (C1).
Outcome op_battle(Context& c, Op op, const uint8_t* p, const uint8_t* base);        ///< Battle (C1).
///@}

/// @name The module waits (src/script/script.cpp)
///@{
bool poll_bag(Context& c);         ///< Wait::Bag.
bool poll_shop(Context& c);        ///< Wait::Shop.
bool poll_mart(Context& c);        ///< Wait::Mart.
bool poll_boxes(Context& c);       ///< Wait::Boxes.
bool poll_transform(Context& c);   ///< Wait::Transform.
bool poll_combine(Context& c);     ///< Wait::Combine.
bool poll_nursery(Context& c);     ///< Wait::Nursery.
bool poll_battle(Context& c);      ///< Wait::Battle (C1).
bool poll_approach(Context& c);    ///< Wait::Approach (C1b).
///@}

/// The encounter record's current side's object (C1: kObjectEncounter, kFaceEncounter; src/script/script.cpp).
/// @return its local ID, or kNoSelf
uint8_t encounter_object();

}   // namespace capsule::script::detail
