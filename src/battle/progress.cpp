/// @file
/// Progression (C-07a, D-22 A8 / A12): experience by the manifest's exp_policy, level-ups, learning moves by
/// its learn policy (with level-up move offers, State::learn), fainting, and new creature records by its
/// generation list (record_value() reads derived roles a layout leaves out).
/// See capsule/battle/battle.h; the modules share internal.h.
#include "internal.h"

namespace capsule::battle::detail
{

/// @name Value ranges
///@{
constexpr unsigned kMaxU16u = 0xFFFFu;        ///< kMaxU16 unsigned; also the low half of a 32-bit value.
constexpr int kHigh16Shift = 16;              ///< Shift of a 32-bit value's high half.
///@}

/// Generation GenKind::PassiveSlot: one of the manifest's rolled passive slots (BattleData::passive_rolled), drawn
/// among the rolled slots up to the last one the species fills (one draw; none when that is the first). The drawn
/// slot is what the record keeps; one the species leaves empty gives the passive of the first rolled slot it fills,
/// never a slot only given (passive_for(), C4).
/// @param sp  species
/// @param r   the record's stream
/// @return the passive slot
uint32_t rolled_passive_slot(const SpeciesData& sp, rng::Stream& r)
{
   uint8_t rolled[kMaxPassiveSlots] = {};
   uint32_t count = 0;
   uint32_t reach = 0;   // rolled slots up to the species' last filled one
   for (uint8_t k = 0; k < data().passive_slots && k < kMaxPassiveSlots; ++k)
   {
      if (((data().passive_rolled >> k) & 1u) == 0)
      {
         continue;
      }
      rolled[count++] = k;
      if (sp.passives[k] != kNone8)
      {
         reach = count;
      }
   }
   if (reach > 1)
   {
      return rolled[rng::below(r, reach)];
   }
   return count > 0 ? rolled[0] : 0;
}

/// Experience needed for a level.
/// @param sp     species
/// @param level  level
/// @return total experience
uint32_t exp_for(const SpeciesData& sp, int level)
{
   if (sp.growth >= data().growth_count || level < 1)
   {
      return 0;
   }
   if (level > data().rules.level_max)
   {
      level = data().rules.level_max;
   }
   return data().growth[sp.growth][level];
}

/// The manifest's progression rules (learn policy, experience sharing, generation list).
/// @return them
const ProgressRules& progress()
{
   return data().rules.progress;
}

/// Fills a battler's moves from the learnset up to a level, by the manifest's learn.default_moves: the last
/// move_slots() distinct moves (each new one past a full list pushes the first out) or the first ones.
/// @param sp     species
/// @param level  level
/// @param moves  kMaxMoveSlots move slots (kNone16 = empty; the ones past move_slots() stay empty)
void default_moves(const SpeciesData& sp, uint8_t level, uint16_t* moves)
{
   for (int i = 0; i < kMaxMoveSlots; ++i)
   {
      moves[i] = kNone16;
   }
   const bool last = static_cast<DefaultMoves>(progress().default_moves) == DefaultMoves::LastByLevel;
   int n = 0;
   for (int i = 0; i < sp.learn_count && sp.learnset[i].level <= level; ++i)
   {
      const uint16_t m = sp.learnset[i].move;
      bool known = false;
      for (int k = 0; k < move_slots(); ++k)
      {
         known = known || moves[k] == m;
      }
      if (known)
      {
         continue;
      }
      if (n < move_slots())
      {
         moves[n++] = m;
      }
      else if (last)
      {
         for (int k = 0; k + 1 < move_slots(); ++k)
         {
            moves[k] = moves[k + 1];
         }
         moves[move_slots() - 1] = m;
      }
      else
      {
         break;   // first_n: the list is full
      }
   }
}

/// Puts a move into a slot with no raises and full move pools (the slot's old move is forgotten).
/// @param s     battle
/// @param i     battler
/// @param slot  move slot
/// @param m     move
void learn_into(State& s, uint8_t i, int slot, uint16_t m)
{
   Battler& b = s.battlers[i];
   b.moves[slot] = m;
   reset_move_slot(b, slot);
   say(s, Msg::Learned, i, kBench, m);
}

/// learn.on_full replace_oldest: the first slot's move is forgotten and the others move up a slot (with their
/// move pools and raises), leaving the last slot to the new move.
/// @param b  battler
void forget_oldest(Battler& b)
{
   for (int k = 0; k + 1 < move_slots(); ++k)
   {
      b.moves[k] = b.moves[k + 1];
   }
   shift_move_slots(b);
}

/// Learns the moves of the battler's level from a learnset entry on: into a free slot, else by learn.on_full
/// (skip, replace the oldest, or offer it: a human party's answer comes through State::learn).
/// @param s     battle
/// @param i     battler
/// @param from  first learnset entry to look at
/// @return true when it stopped at an offer (State::learn) that waits for an answer
bool learn_level_moves(State& s, uint8_t i, int from)
{
   Battler& b = s.battlers[i];
   const SpeciesData& sp = data().species[b.species];
   for (int k = from; k < sp.learn_count; ++k)
   {
      if (sp.learnset[k].level != b.level)
      {
         continue;
      }
      const uint16_t m = sp.learnset[k].move;
      bool known = false;
      int free = -1;
      for (int j = 0; j < move_slots(); ++j)
      {
         known = known || b.moves[j] == m;
         if (b.moves[j] == kNone16 && free < 0)
         {
            free = j;
         }
      }
      if (known)
      {
         continue;
      }
      if (free >= 0)
      {
         learn_into(s, i, free, m);
         continue;
      }
      const auto full = static_cast<LearnFull>(progress().learn_full);
      if (full == LearnFull::ReplaceOldest)
      {
         forget_oldest(b);
         learn_into(s, i, move_slots() - 1, m);
      }
      else if (full == LearnFull::Request && s.parties[b.party].controller == Controller::Human)
      {
         s.learn = LearnOffer{m, i, static_cast<uint8_t>(k + 1), 0, 0};
         return true;
      }
      else
      {
         // skip, or an AI party under request: it keeps its moves
         say(s, Msg::NotLearned, i, kBench, m);
      }
   }
   return false;
}

/// Whether a level-up's move offer waits for an answer (request() / choose()).
/// @param s  battle
/// @return true while State::learn holds an offer without an answer
bool learn_asking(const State& s)
{
   return s.learn.battler != kBench && s.learn.answered == 0;
}

/// Takes the answer to a move offer, then goes on with the rest of that level's learnset (one unit).
/// @param s  battle
void answer_offer(State& s)
{
   const LearnOffer o = s.learn;
   s.learn.battler = kBench;
   if (o.answer < move_slots())
   {
      learn_into(s, o.battler, o.answer, o.move);
   }
   else
   {
      say(s, Msg::NotLearned, o.battler, kBench, o.move);
   }
   learn_level_moves(s, o.battler, o.next);
}

/// Adds earned experience and queues its level-ups (one unit).
/// @param s       battle
/// @param i       receiving battler
/// @param amount  experience
/// @return true if anything was shown
bool credit_exp(State& s, uint8_t i, uint32_t amount)
{
   Battler& b = s.battlers[i];
   if (!usable(b))
   {
      return false;
   }   // fainted before the award was shown
   const SpeciesData& sp = data().species[b.species];
   const uint32_t cap = exp_for(sp, data().rules.level_max);
   if (b.exp >= cap || amount == 0)
   {
      return false;
   }
   const uint32_t gained = amount > cap - b.exp ? cap - b.exp : amount;
   b.exp += gained;
   emit(s, EventKind::Exp, i, kBench, 0, static_cast<uint16_t>(gained & kMaxU16u),
        static_cast<uint16_t>(gained >> kHigh16Shift));
   say(s, Msg::ExpGained, i, kBench, static_cast<uint16_t>(gained > kMaxU16u ? kMaxU16u : gained));
   if (b.level < data().rules.level_max && b.exp >= exp_for(sp, b.level + 1))
   {
      s.level_pending |= bit(i);
   }
   return true;
}

/// Shows one level-up (one unit).
/// @param s  battle
/// @param i  battler
/// @return true if it levelled up
bool level_up_once(State& s, uint8_t i)
{
   Battler& b = s.battlers[i];
   const SpeciesData& sp = battler_species(b);   // its form's base stats (G4)
   if (b.level >= data().rules.level_max || b.exp < exp_for(sp, b.level + 1))
   {
      s.level_pending &= ~bit(i);
      return false;
   }
   ++b.level;
   s.levelled |= bit(i);   // G4: growth's level_up trigger once the battle is over
   const uint16_t old_max = max_hp(b);
   const uint16_t old_hp = b.hp;
   int32_t old_pools[kMaxPools] = {};
   pool_maxima(b, old_pools);
   compute_stats(sp, b.level, b.genes, b.training, b.awaken, b.trait, b.stats);
   const bool alive = usable(b);
   if (alive)
   {
      b.hp = static_cast<uint16_t>(clamp_to<int32_t>(b.hp + max_hp(b) - old_max, 1, max_hp(b)));
   }
   emit(s, EventKind::Level, i, kBench, 0, b.level);
   emit(s, EventKind::Hp, i, kBench, 0, b.hp, old_hp);
   if (alive)
   {
      grow_pools(s, i, old_pools);   // the other battler pools gain what their maxima gained (D-25)
   }
   say(s, Msg::LevelUp, i, kBench, b.level);
   learn_level_moves(s, i, 0);   // may stop at an offer: answer_offer() goes on
   if (b.level >= data().rules.level_max || b.exp < exp_for(sp, b.level + 1))
   {
      s.level_pending &= ~bit(i);
   }
   return true;
}

/// Shows the next queued experience award or level-up (one unit).
/// @param s  battle
/// @return true if something was shown
bool pay_exp(State& s)
{
   // a level-up's move offer that has its answer goes first (step() waits while it has none)
   if (s.learn.battler != kBench)
   {
      answer_offer(s);
      return true;
   }
   for (uint8_t i = 0; i < s.battler_count; ++i)
   {
      if (s.exp_pending[i] == 0)
      {
         continue;
      }
      const uint32_t amount = s.exp_pending[i];
      s.exp_pending[i] = 0;
      if (credit_exp(s, i, amount))
      {
         return true;
      }
   }
   while (s.level_pending != 0)
   {
      uint8_t i = 0;
      while ((s.level_pending & bit(i)) == 0)
      {
         ++i;
      }
      if (level_up_once(s, i))
      {
         return true;
      }
   }
   return false;
}

/// Whether a battler shares a fainted foe's experience (exp_policy receivers and share).
/// @param s        battle
/// @param fainted  the fainted battler
/// @param i        a candidate
/// @return true if it earns a share
bool shares_exp(const State& s, uint8_t fainted, uint8_t i)
{
   const ProgressRules& pr = progress();
   const Battler& f = s.battlers[fainted];
   const Battler& b = s.battlers[i];
   const PartyState& t = s.parties[b.party];
   if (t.side == s.parties[f.party].side || !usable(b))
   {
      return false;
   }
   if (static_cast<ExpReceivers>(pr.exp_receivers) == ExpReceivers::HumanSide && t.controller != Controller::Human)
   {
      return false;
   }
   switch (static_cast<ExpShare>(pr.exp_share))
   {
   case ExpShare::Faced:
      return (f.faced & bit(i)) != 0;
   case ExpShare::Active:
      return b.position != kBench;
   default:
      return true;   // Party: every usable battler of a receiving party
   }
}

/// exp_gain()'s outsider input for a receiver (exp_policy outsider).
/// @param s  battle
/// @param b  the receiver
/// @return 1 when its record's OwnerId is not the player's (Setup::owner_id), else 0
int32_t outsider_of(const State& s, const Battler& b)
{
   using monster::Role;
   const PartyState& t = s.parties[b.party];
   if (static_cast<ExpOutsider>(progress().exp_outsider) != ExpOutsider::OwnerId ||
       !has_flag(b.flags, BATTLER_FROM_RECORD) || t.records == nullptr || !monster::has(Role::OwnerId))
   {
      return 0;
   }
   // the player's id as the layout's OwnerId width holds it
   const uint32_t own = s.owner_id & monster::max_value(Role::OwnerId);
   return monster::get(t.records->members[b.party_slot], Role::OwnerId) != own ? 1 : 0;
}

/// Earns experience for a fainted foe for the battlers exp_policy names;
/// pay_exp() shows it once the current move is over.
/// @param s       battle
/// @param fainted battler that fainted
void award_exp(State& s, uint8_t fainted)
{
   const Battler& f = s.battlers[fainted];
   const PartyState& ft = s.parties[f.party];
   uint8_t list[kMaxBattlers];
   int n = 0;
   for (uint8_t i = 0; i < s.battler_count; ++i)
   {
      if (shares_exp(s, fainted, i))
      {
         list[n++] = i;
      }
   }
   if (n == 0)
   {
      return;
   }
   const SpeciesData& sp = data().species[f.species];
   const uint32_t exp_max = monster::max_value(monster::Role::Experience);
   for (int k = 0; k < n; ++k)
   {
      gain_battler_training(s, list[k], f.species);   // G4: the foe's yields (structure training)
      const Battler& b = s.battlers[list[k]];
      const int32_t amount = rules::exp_gain(sp.exp_yield, f.level, b.level, n, ft.rank, outsider_of(s, b));
      if (amount <= 0)
      {
         continue;
      }
      const uint32_t sum = s.exp_pending[list[k]] + static_cast<uint32_t>(amount);
      // the Experience role holds max_value() at the layout's width
      s.exp_pending[list[k]] = sum > exp_max ? exp_max : sum;
   }
}

/// Marks fainted battlers, queues their events and awards experience.
/// @param s  battle
/// @param attacker  battler credited with knockouts, or kBench
void check_faints(State& s, uint8_t attacker)
{
   const WeatherWatch watch(s, kBench);   // C6: a fainted battler suppresses the weather no more
   for (uint8_t i = 0; i < s.battler_count; ++i)
   {
      Battler& b = s.battlers[i];
      if (b.position == kBench || b.hp != 0 || has_flag(b.flags, BATTLER_FAINTED))
      {
         continue;
      }
      b.flags = static_cast<uint8_t>(b.flags | bit(BATTLER_FAINTED));
      clear_volatiles(b, VOLATILE_FAINT);
      end_bound(s, i);   // G1: the bound volatiles it started end with it
      b.lock_flags = 0;
      b.lock_move = kNone16;
      emit(s, EventKind::Faint, i, b.position);
      say(s, Msg::Fainted, i);
      hook_simple(s, effect::Hook::Faint, i, attacker);
      if (attacker != kBench && attacker < s.battler_count && side_of(s, attacker) != side_of(s, i))
      {
         PartyState& at = s.parties[s.battlers[attacker].party];
         at.knockouts = static_cast<uint16_t>(at.knockouts < kMaxU16 ? at.knockouts + 1 : at.knockouts);
      }
      award_exp(s, i);
   }
}

/// Runs one generation entry over its role's elements (manifest `generation`; D-22 A12, D-23 section 2). A role
/// the layout leaves out still draws, so the draw order is the list's whatever the layout holds.
/// @param rec  record being built
/// @param g    the entry
/// @param sp   its species (passive slots)
/// @param r    stream for the draws
void generate(monster::Record& rec, const GenData& g, const SpeciesData& sp, rng::Stream& r)
{
   for (int e = 0; e < g.count; ++e)
   {
      uint32_t v = g.value;
      switch (static_cast<GenKind>(g.kind))
      {
      case GenKind::Random:
         v += g.span > 1 ? rng::below(r, g.span) : 0;   // one value needs no draw
         break;
      case GenKind::Wide:
      {
         // two draws, the high half first (as the seed always took them)
         const uint32_t high = rng::next16(r);
         const uint32_t low = rng::next16(r);
         v += (high << rng::kDrawBits | low) & g.span;
         break;
      }
      case GenKind::WideLowFirst:
      {
         // two draws, the low half first (manifest "order": "low_first")
         const uint32_t low = rng::next16(r);
         const uint32_t high = rng::next16(r);
         v += (high << rng::kDrawBits | low) & g.span;
         break;
      }
      case GenKind::PassiveSlot:
         v = rolled_passive_slot(sp, r);
         break;
      case GenKind::Derived:
         if (!monster::has(g.role))
         {
            return;   // nothing holds it: record_value() computes it whenever it is read (no draw is skipped)
         }
         v = static_cast<uint32_t>(rules::derive(g.derived, e, rec));
         break;
      default:
         break;   // Constant
      }
      monster::set(rec, g.role, e, v);
   }
}

}   // namespace capsule::battle::detail

// -- public ------------------------------------------------------------------------------------------------

namespace capsule::battle
{

using namespace detail;

bool make_monster(monster::Record& rec, uint16_t species, uint8_t level, rng::Stream& r)
{
   using monster::Role;
   if (species >= data().species_count || !monster::has(Role::SpeciesKey) || !monster::has(Role::Level) ||
       !monster::has(Role::Moves))
   {
      return false;
   }
   const SpeciesData& sp = data().species[species];
   level = static_cast<uint8_t>(clamp_to<int>(level, 1, data().rules.level_max));
   monster::reset(rec);   // custom roles take their defaults
   monster::set(rec, Role::SpeciesKey, raw(sp.key));
   monster::set(rec, Role::Level, level);
   monster::set(rec, Role::Experience, exp_for(sp, level));
   uint16_t moves[kMaxMoveSlots];
   default_moves(sp, level, moves);
   for (int i = 0; i < move_slots(); ++i)
   {
      monster::set(rec, Role::Moves, i, moves[i]);
   }
   // the generation list in order: its order is the draw order (derived roles read what is set so far)
   const ProgressRules& pr = progress();
   for (int g = 0; g < pr.generation_count; ++g)
   {
      generate(rec, pr.generation[g], sp, r);
   }
   // every persisted pool full (D-25), the move pools with the raises the generation may have given
   fill_resources(rec);
   return true;
}

uint32_t record_value(const monster::Record& rec, monster::RoleId role, int element)
{
   if (monster::has(role))
   {
      return monster::get(rec, role, element);
   }
   // a role the layout leaves out: what its generation entry gives without a draw
   const ProgressRules& pr = progress();
   for (int g = 0; g < pr.generation_count; ++g)
   {
      const GenData& e = pr.generation[g];
      if (e.role != role)
      {
         continue;
      }
      const auto kind = static_cast<GenKind>(e.kind);
      if (kind == GenKind::Derived)
      {
         return static_cast<uint32_t>(rules::derive(e.derived, element, rec));
      }
      if (kind == GenKind::Constant)
      {
         return e.value;
      }
   }
   return monster::initial(role);
}

void rederive(monster::Record& rec, const monster::RoleSet* keep)
{
   const ProgressRules& pr = progress();
   for (int g = 0; g < pr.generation_count; ++g)
   {
      const GenData& e = pr.generation[g];
      if (static_cast<GenKind>(e.kind) != GenKind::Derived || !monster::has(e.role) ||
          (keep != nullptr && monster::has_role(*keep, e.role)))
      {
         continue;
      }
      for (int k = 0; k < e.count; ++k)
      {
         monster::set(rec, e.role, k, static_cast<uint32_t>(rules::derive(e.derived, k, rec)));
      }
   }
}

}   // namespace capsule::battle
