/// @file
/// The field context (D-26 section 2, package G2): effect logic that runs out of battle, on a party member, with
/// the battle's own effect host. A field context is a one-battler battle state built in a State the game lends
/// (its battle state when no battle is on): the member's record as battler 0 of party 0 (a human party, on no
/// position, without its held item's and passive's hooks: out of battle they do not react), so every primitive,
/// read and text works as in battle (`restore`, `raise`, `cure`, `teach`, `text`, `say` ...). Closing it writes the
/// battler back into the record: its persisted resources and raises, its status and its moves.
///
/// Used by the inventory's item use (capsule/inventory/use.h: the item's use_field hook); the transformation
/// rules of a later package (G4) use the same context. Events (texts) queue in the State as in battle:
/// take them with pop_event() and format_text().
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/battle/effect.h"
#include "capsule/monster/monster.h"

#include <cstdint>

namespace capsule::battle
{

/// The battler a field context's member is (battler 0), or kBench for a context without one (target none).
inline constexpr uint8_t kFieldBattler = 0;

/// Opens a field context in @p s (everything in it is replaced).
/// @param s       a State the game lends (not a battle in progress)
/// @param party   the party (records written back by field_close())
/// @param member  party slot of the member, or kBench for none
/// @return false for a member that is not in the party or whose species is unknown
bool field_open(State& s, monster::Party& party, uint8_t member);

/// Runs one owner's handlers for a hook in the field context, in handler order, until one ends the run.
/// @param s      the context
/// @param owner  owner index (BattleData::effects; an item's HolderData::owner)
/// @param h      the hook (Hook::UseField)
/// @param move   the context move (a chosen move slot's move), or kNone16
/// @return Done when every handler finished, else how the first one that did not ended (Failed: no effect)
effect::Status field_run(State& s, uint16_t owner, effect::Hook h, uint16_t move);

/// Whether a move offer (effect `teach` with every move slot taken) waits for its answer.
/// @param s  the context
/// @return true while it waits
bool field_offer_waiting(const State& s);

/// Answers the waiting move offer: the move slot to forget, or kKeepMoves (it is not learned).
/// @param s     the context
/// @param slot  move slot, or kKeepMoves
/// @return true if the move was learned
bool field_answer(State& s, uint8_t slot);

/// What a field run may change, taken before it (C6, review4 finding 10: an item whose logic fails part-way leaves
/// nothing behind): the member's battler and record, the move offer and the queued events.
struct FieldSnapshot
{
   Battler battler;          ///< Battler kFieldBattler.
   monster::Record record;   ///< The member's record (when the context has one).
   LearnOffer learn;         ///< The move offer.
   uint16_t ev_count;        ///< Events queued.
   bool has_record;          ///< The context has a member.
};

/// Takes a snapshot of a field context.
/// @param s    the context
/// @param out  the snapshot
void field_snapshot(const State& s, FieldSnapshot& out);

/// Puts a field context back as a snapshot found it (the events queued since are dropped).
/// @param s     the context
/// @param snap  field_snapshot()'s
void field_restore(State& s, const FieldSnapshot& snap);

/// The battler a field context's other creature is (F1: the creature an encounter made; field_add_other()).
inline constexpr uint8_t kFieldOther = 1;

/// Adds a second battler to an open field context with its member (F1, D-27 section 1): member 0 of @p other as
/// battler kFieldOther of a second party (an unowned AI party on no position, its held item's and passive's hooks
/// left out as the member's are). Role writes on it (`target.role(NAME) = ...`) go into its record.
/// @param s      the context (field_open() with a member)
/// @param other  the creature's party (its member 0)
/// @return false without a member, without a creature, or when its species is unknown
bool field_add_other(State& s, monster::Party& other);

/// Runs one owner's handlers for a hook in the field context with the hook's value and the other battler (F1: the
/// encounter hooks; holder = user = the member, other = target = @p other).
/// @param s      the context
/// @param owner  owner index (BattleData::effects)
/// @param h      the hook
/// @param value  the hook's value (ctx.value)
/// @param other  kFieldOther, or kBench
/// @param dealt  the context's dealt register (B1: the combine hook's input count, `field.combine_inputs`)
/// @return Done when every handler finished, else how the first one that did not ended
effect::Status field_hook(State& s, uint16_t owner, effect::Hook h, int32_t value, uint8_t other,
                          int32_t dealt = 0);

/// Runs one owner's handlers for a hook with a value its handlers may modify (B2: the hatch hook; holder = user = the
/// member): each handler sees what the one before left.
/// @param s      the context
/// @param owner  owner index (BattleData::effects)
/// @param h      the hook
/// @param value  the hook's value in, what the handlers made of it out
/// @return Done when every handler finished, else how the first one that did not ended
effect::Status field_hook_value(State& s, uint16_t owner, effect::Hook h, int32_t& value);

/// Adds every member of a party to an open field context with its member (B1, D-28: a combination's inputs): member
/// k as battler kFieldOther + k of a second party (an unowned AI party on no position; passives' hooks left out as
/// the member's are), read by effect logic as `input.k`. C9: each keeps its held item (`input.k.item == NAME`
/// reads it) flagged BATTLER_QUIET_ITEM: the item's hooks, points and tags stay out of the run.
/// @param s       the context (field_open() with a member, nothing added yet)
/// @param others  the party (its records: role writes would go there)
/// @return false without a member, when something was added already, when a species is unknown or when the party
///         does not fit the State's battlers
bool field_add_party(State& s, monster::Party& others);

/// Queues one of the battle's messages about a record (B1: a combination's moments): the record becomes battler
/// kFieldBattler ({user}) without its records attached; what the State holds besides (its queued texts) stays.
/// @param s        the State (a field context, open or detached, or a fresh one)
/// @param message  message id (BattleData::messages)
/// @param about    the record it is about
void field_say(State& s, uint8_t message, const monster::Record& about);

/// Marks the field context of a combination while it lives (RAII, B1): the Inherit op (the combine hook's
/// inheritance primitives) works only inside one, with the result as kFieldBattler and the inputs from
/// kFieldOther on (src/battle/combine_battle.cpp).
class CombineScope
{
public:
   /// Marks it.
   /// @param inputs  how many inputs the context holds
   explicit CombineScope(uint8_t inputs);
   /// Unmarks it.
   ~CombineScope();
   CombineScope(const CombineScope&) = delete;              ///< Not copied.
   CombineScope& operator=(const CombineScope&) = delete;   ///< Not assigned.
};

/// Detaches the context's records without writing the member back (F1: an encounter hook changes no member; role
/// writes already went into their records).
/// @param s  the context
void field_detach(State& s);

/// Writes the member back into its record (persisted resources and raises, status, moves) and detaches the
/// record; the queued texts stay readable (format_text() names the battler).
/// @param s  the context
void field_close(State& s);

} // namespace capsule::battle
