/// @file
/// Using a bag item out of battle (D-26 section 2, package G2): the item's effect logic hook use_field runs in a
/// field context (capsule/battle/field_context.h) on the chosen target, and a use that had an effect takes one
/// item from the bag when the item is consumed. What an item does is its logic: restores (`restore`), resource
/// raises (`raise` on the move slot's move: PP-Up style through the resource's raises role), move teaching
/// (`teach`: with every move slot taken the use waits for a learn answer, which the script port asks the player
/// for: capsule::script::items::learn_open()); evolution items run `transform` (G4, capsule/growth/growth.h: the
/// item_use transformation rules with this item; the new species' moves may wait for a learn answer too, the item
/// being used up already). `fail` in the logic means "no effect": nothing is consumed.
///
/// A FieldUse is a scope object: it opens the field context on construction and writes the member back on
/// destruction (every path). Keep it for one use and, when it asks, its learn answer; read its texts before it
/// ends (texts()).
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/inventory/bag.h"
#include "capsule/monster/monster.h"

#include <cstdint>

namespace capsule::inventory
{

/// No move slot chosen (an item whose target is not a move slot).
inline constexpr uint8_t kNoMoveSlot = 0xFF;
/// No party member chosen (an item whose target is none).
inline constexpr uint8_t kNoMember = 0xFF;

/// How a use went.
enum class UseResult : uint8_t
{
   Used,        ///< It had an effect (one taken from the bag if it is consumed).
   NoEffect,    ///< Its logic failed (or faulted): nothing changed, nothing is consumed.
   NotUsable,   ///< Not an item used out of battle, or none in the bag.
   BadTarget,   ///< The target its `target` field needs is missing or empty (no member, an empty move slot).
   LearnOffer,  ///< Every move slot is taken: answer_learn() decides (the item waits).
   NUM_RESULTS  ///< Number of results.
};

/// One item use on a party member, as a scope: the field context lives as long as it does.
struct FieldUse
{
   battle::State& state;    ///< The lent State holding the field context.
   monster::Party& party;   ///< The party.
   uint8_t member;          ///< The member (kNoMember: none).
   bool open;               ///< The context opened (the member exists).
   uint16_t waiting_item;   ///< The item whose move offer waits (consumed when the move is learned), or 0.
   bool consumed;           ///< The waiting item is used up already (G4: a transformation offers the moves).

   /// Opens the field context on a member.
   /// @param s       a State the game lends (no battle in progress)
   /// @param p       the party
   /// @param target  party slot of the member, or kNoMember for an item with target none
   FieldUse(battle::State& s, monster::Party& p, uint8_t target);
   FieldUse(const FieldUse&) = delete;
   FieldUse& operator=(const FieldUse&) = delete;
   /// Writes the member back into its record.
   ~FieldUse();

   /// Uses an item from the bag on the member (and a move slot of it).
   /// @param bag   the bag (the item must be in it)
   /// @param item  item id
   /// @param slot  move slot for target move_slot, else kNoMoveSlot
   /// @return how it went
   UseResult use(Bag& bag, uint16_t item, uint8_t slot = kNoMoveSlot);

   /// Answers a LearnOffer: the move slot to forget, or battle::kKeepMoves to keep every move (the item stays).
   /// @param bag   the bag
   /// @param slot  move slot or kKeepMoves
   /// @return Used (learned, the item consumed when it is) or NoEffect (kept every move)
   UseResult answer_learn(Bag& bag, uint8_t slot);

   /// The move a LearnOffer offers.
   /// @return move id, or battle::kNone16 when none waits
   uint16_t offered_move() const;

   /// The next text the use produced (messages, owner texts, status texts), formatted.
   /// @param out  codes (ends with text::kCodeEnd)
   /// @param cap  capacity of @p out
   /// @return false when no text is left
   bool next_text(text::Code* out, int cap);
};

} // namespace capsule::inventory
