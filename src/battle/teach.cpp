/// @file
/// Effect `teach WHO MOVE` (Op::Teach, D-26 section 2, G2): an item that teaches a move offers it the way a
/// level-up does. A free move slot takes it at once; with every slot taken a human party gets a move offer
/// (State::learn, answered like a level-up's: in battle by choose(), out of battle by the field context,
/// include/capsule/inventory/use.h); a battler that knows the move, or an AI party's full one, cannot take it and
/// the run fails (the item has no effect). C6: when the manifest lists species' teachable moves, a species that does
/// not list the move cannot take it either (the run fails, the item stays).
/// See capsule/battle/effect.h; the modules share internal.h.
#include "internal.h"

namespace capsule::battle::detail
{

/// @name Teach results (the value Op::Teach pushes)
///@{
constexpr int32_t kTaught = 1;    ///< Learned into a free slot.
constexpr int32_t kOffered = 2;   ///< Every slot taken: a move offer waits for its answer (State::learn).
///@}

/// The learnset entry an offer goes on from once answered: past every learnset (a taught move is not a level's
/// learning, so answer_offer() learns nothing more).
constexpr uint8_t kNoNextEntry = 0xFF;

namespace
{

/// Whether a species may be taught a move (C6, review4 finding 23): any move when no species lists its teachable
/// moves, else one of its list (sorted: a short search).
/// @param species  species index
/// @param move     move id
/// @return true if it may
bool teachable(uint16_t species, uint16_t move)
{
   const BattleData& d = data();
   if (d.teach_first == nullptr || d.teach_moves == nullptr)
   {
      return true;
   }
   if (species >= d.species_count)
   {
      return false;
   }
   for (uint16_t k = d.teach_first[species]; k < d.teach_first[species + 1]; ++k)
   {
      if (d.teach_moves[k] == move)
      {
         return true;
      }
   }
   return false;
}

} // namespace

/// Effect `teach WHO MOVE`: the move into a free slot, or a move offer (State::learn) for a human party.
/// @param s     battle (or field context)
/// @param i     battler
/// @param move  move id
/// @return pushes kTaught or kOffered; fails the run when the battler knows the move, an AI party's slots are
///         full, an offer already waits, or the battler or move does not exist
effect::Outcome teach_move(State& s, uint8_t i, int32_t move)
{
   const effect::Outcome fail{0, false, false, true};
   if (i >= s.battler_count || move < 0 || move >= data().move_count || s.learn.battler != kBench)
   {
      return fail;   // nobody, no such move, or an offer already waits
   }
   const auto m = static_cast<uint16_t>(move);
   Battler& b = s.battlers[i];
   if (!teachable(b.species, m))
   {
      return fail;   // C6: its species cannot learn it (species `teachable`)
   }
   int free = -1;
   for (int k = 0; k < move_slots(); ++k)
   {
      if (b.moves[k] == m)
      {
         return fail;   // knows it already
      }
      if (b.moves[k] == kNone16 && free < 0)
      {
         free = k;
      }
   }
   if (free >= 0)
   {
      learn_into(s, i, free, m);
      return effect::Outcome{kTaught, false, false, false};
   }
   if (s.parties[b.party].controller != Controller::Human)
   {
      return fail;   // an AI party keeps its moves
   }
   s.learn = LearnOffer{m, i, kNoNextEntry, 0, 0};
   return effect::Outcome{kOffered, false, false, false};
}

}   // namespace capsule::battle::detail
