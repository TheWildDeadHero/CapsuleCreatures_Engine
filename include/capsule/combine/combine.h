/// @file
/// Combination (D-28 section 1, package B1): N records go in, the method's rules pick a result species, the
/// `on combine` effect logic builds the result record and the engine delivers it. The rules are the manifest's
/// (capsule/combine/data.h, tools/battlec/combine.py); this module evaluates them.
///
/// - preview() runs a method's checks and result rules on records without their random parts (nothing changes, no
///   draw): the menus' and the studio's "A + B -> C". A random pick answers its group instead of a species, and
///   (C9) the highest fee over the group's candidates its where holds for (the level stays the expression's without
///   a result species).
/// - Combination is a scope over a State the game lends (as growth::Change): it checks the inputs (count, each
///   slot's requirement, the odds), picks the result (random picks draw from the stream given), makes the result
///   record (battle::make_monster() at the level expression's level), runs the `on combine` handlers of the game's
///   global rules in a field context (the result as battler 0, the inputs' copies as battlers 1..N: read only; role
///   writes and the inherit primitives write the result's record; a handler that fails or blocks refuses the
///   combination), gathers the moves of the method's sources and lets its chooser pick them (Stage::Choosing waits
///   for the player's picks when the chooser is the player's and there are more candidates than it takes). place()
///   then delivers the result (into the party slot of the first consumed party input, else the party's end, else
///   the storage boxes) and only after that removes the consumed inputs. Its texts (the method's messages, the
///   logic's `say` / `text`) queue in the lent State as in battle: take them with pop_event() / format_text().
///
/// B2: a method delivering eggs places its result as an egg (capsule/combine/eggs.h make_egg()); a nursery method
/// runs only over the nursery's records (inputs From::Nursery: capsule/combine/nursery.h, which keeps the result
/// instead of placing it), an immediate one only over party and storage records (else Status::NotImmediate).
/// preview() takes either kind. An egg input is unfit unless the method allows eggs.
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/combine/data.h"
#include "capsule/core/random.h"
#include "capsule/monster/monster.h"
#include "capsule/storage/storage.h"

#include <cstdint>

namespace capsule::combine
{

/// What a check or a combination says (tools/battlec/combine.py STATUSES).
enum class Status : uint8_t
{
   Ok,             ///< It combines (or combined).
   WrongCount,     ///< Fewer or more inputs than the method takes.
   Unfit,          ///< An input fails its slot's requirement (Preview::slot).
   Incompatible,   ///< The odds are 0.
   Refused,        ///< No result rule matched, or the `on combine` logic refused.
   NotImmediate,   ///< A nursery method outside the nursery, or an immediate one over nursery records.
   Unsupported,    ///< Not used since B2 (egg delivery runs); kept for the order.
   NoRoom,         ///< Neither the party nor the boxes have room for the result.
   BadInput,       ///< An unknown method, an input that is not there, or one given twice.
   NUM_STATUSES    ///< Number of statuses.
};

/// No slot or rule (Preview::slot / rule).
inline constexpr uint8_t kNone = 0xFF;
/// No group (a pick that is not random).
inline constexpr uint16_t kNoGroup = 0xFFFF;

/// What a method would give.
struct Preview
{
   Status status;      ///< Ok, or why not.
   uint8_t slot;       ///< Unfit: the input slot, else kNone.
   uint8_t rule;       ///< The rule that matched, or kNone.
   uint8_t level;      ///< The result's level (Ok).
   uint16_t species;   ///< The result's species, or kNoSpecies (a random pick, or not Ok).
   uint16_t group;     ///< A random pick's group (its table's cell), else kNoGroup.
   int32_t odds;       ///< The odds (1 without an odds expression).
   int32_t fee;        ///< The fee (0 without a fee expression; C9: a random pick: the highest over its
                       ///< candidates, the most it can cost before the draw).
};

/// Where an input record is.
enum class From : uint8_t
{
   Party,     ///< A party member (InputRef::index: its slot).
   Storage,   ///< A storage slot (InputRef::index).
   Nursery,   ///< A nursery slot (InputRef::index; B2: only for the nursery's method).
};

/// One input of a combination.
struct InputRef
{
   From from;          ///< Where it is.
   uint8_t reserved;   ///< 0.
   uint16_t index;     ///< Its party slot or storage slot.
};
CAPSULE_POD(InputRef, 4);

/// How many methods the build has.
/// @return 0 to kMaxMethods
uint8_t method_count();

/// A method's data.
/// @param method  method index (battle_ids.h COMBINE_METHOD_<NAME>)
/// @return it, or nullptr
const MethodData* method_at(uint8_t method);

/// What a method would give for some records, without the random parts (no draw, nothing changes; the context
/// values through growth::make_context()).
/// @param method  method index
/// @param inputs  the input records, in slot order
/// @param count   how many
/// @return the preview
Preview preview(uint8_t method, const monster::Record* const* inputs, uint8_t count);

/// The fee of a combination whose result is known.
/// @param method   method index
/// @param inputs   the input records
/// @param count    how many
/// @param species  the result's species
/// @param level    its level
/// @return the fee (0 without a fee expression)
int32_t fee_of(uint8_t method, const monster::Record* const* inputs, uint8_t count, uint16_t species, uint8_t level);

/// Where a Combination stands.
enum class Stage : uint8_t
{
   Refused,    ///< It does not combine (result().status says why); nothing changed.
   Choosing,   ///< The result is built; the player picks its moves (choose(), done_choosing()).
   Ready,      ///< The result is built; place() delivers it.
   Placed,     ///< Delivered (and the consumed inputs removed).
   NUM_STAGES  ///< Number of stages.
};

/// One combination, as a scope over a State the game lends (no battle may run in it meanwhile).
class Combination
{
public:
   /// Checks the inputs, picks the result, builds its record and runs the `on combine` logic (see the file
   /// comment); Choosing or Ready when it combines, else Refused.
   /// @param s       a State the game lends (its texts queue there)
   /// @param party   the player's party
   /// @param boxes   the storage boxes, or nullptr
   /// @param method  method index
   /// @param inputs  where the inputs are, in slot order
   /// @param count   how many
   /// @param rng     the draws' stream (random picks, inheritance, the random chooser)
   Combination(battle::State& s, monster::Party& party, storage::Storage* boxes, uint8_t method,
               const InputRef* inputs, uint8_t count, rng::Stream& rng);
   Combination(const Combination&) = delete;              ///< Not copied.
   Combination& operator=(const Combination&) = delete;   ///< Not assigned.
   /// Lets go of the lent State (nothing is placed unless place() was called).
   ~Combination() = default;

   /// Where it stands.
   /// @return the stage
   Stage stage() const { return m_stage; }

   /// What it gives: the status, the rule, the species (a random pick's draw included), the level, odds and fee.
   /// @return the preview of this combination
   const Preview& result() const { return m_result; }

   /// The result record (Choosing / Ready / Placed).
   /// @return it
   const monster::Record& record() const { return m_result_party.members[0]; }

   /// The moves the sources gathered (Choosing: the menu's entries).
   /// @return how many
   uint8_t candidate_count() const { return m_candidate_count; }

   /// One candidate move.
   /// @param k  0 to candidate_count() - 1
   /// @return its move, or monster::kNoMove
   uint16_t candidate(uint8_t k) const;

   /// Whether a candidate is chosen already.
   /// @param k  candidate
   /// @return true when chosen
   bool chosen(uint8_t k) const;

   /// How many moves the chooser takes (MethodData::move_limit).
   /// @return the limit
   uint8_t move_limit() const;

   /// The player takes a candidate (Choosing); at the limit it is Ready.
   /// @param k  candidate
   /// @return false when it is not Choosing, k is out of range or already chosen
   bool choose(uint8_t k);

   /// The player is done picking (Choosing -> Ready; the moves picked so far, the result's own fill the rest).
   void done_choosing();

   /// Picks the rest at random (the fallback of a game without a menu; Choosing -> Ready).
   void choose_random();

   /// Delivers the result and removes the consumed inputs (Ready -> Placed); queues the method's combined or
   /// no-room message.
   /// @return Ok, NoRoom (nothing changed: it stays Ready) or Refused (not Ready)
   Status place();

   /// Where the result went (Placed).
   /// @return From::Party (index: its party slot) or From::Storage (index: the storage slot)
   InputRef placed() const { return m_placed; }

private:
   /// Refuses with a status (and the method's message for it).
   /// @param status  why
   /// @param slot    Unfit: the slot
   void refuse(Status status, uint8_t slot = kNone);
   /// Gathers the moves of the method's sources.
   void gather_moves();
   /// Gathers one source's moves from its one input (C9: `from: ROLE`): that input's moves in slot order that the
   /// source's list holds (Known: all of them; CombineMoves: the result's combine_moves; SharedLevelUp: its
   /// learnset; TeachableKnown: its teachable moves).
   /// @param src  the source (from set)
   /// @param sp   the result's species
   /// @param add  adds one candidate
   template <typename Add>
   void gather_from(const MoveSourceData& src, uint16_t sp, Add& add) const;
   /// Writes the chosen moves (then the result's own ones when the method fills) into the result.
   void apply_moves();
   /// C9, kPushMoves: the result's own moves, then each chosen one in order (a known one skipped; when full the
   /// first goes, the rest move up, the new one is last).
   /// @param moves  receives the moves (monster::kMaxMoveSlots)
   /// @param n      receives their count
   void push_moves(uint16_t* moves, int& n) const;
   /// Queues a method's message about a record ({user}).
   /// @param moment  which
   /// @param about   the record
   void say(Moment moment, const monster::Record& about);

   battle::State& m_state;                       ///< The lent State.
   monster::Party& m_party;                      ///< The player's party.
   storage::Storage* m_boxes;                    ///< The boxes, or nullptr.
   rng::Stream& m_rng;                           ///< The draws.
   monster::Party m_result_party;                ///< The result (member 0).
   monster::Party m_inputs;                      ///< The inputs' copies, slot order.
   InputRef m_refs[kMaxInputs];                  ///< Where the inputs are.
   uint16_t m_candidates[kMaxCandidates];        ///< The gathered moves.
   uint8_t m_order[kMaxCandidates];              ///< The chosen candidates, in pick order.
   Preview m_result;                             ///< What it gives.
   InputRef m_placed;                            ///< Where the result went.
   uint8_t m_method;                             ///< The method.
   uint8_t m_count;                              ///< Inputs.
   uint8_t m_candidate_count;                    ///< Gathered moves.
   uint8_t m_chosen_count;                       ///< Chosen ones.
   Stage m_stage;                                ///< Where it stands.
};

} // namespace capsule::combine
