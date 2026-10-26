/// @file
/// Recruitment (G3, D-26 section 3): effect `recruit WHO BONUS` (Op::Recruit) in an item's use_battle logic.
/// The manifest's rules (capsule/battle/data/recruit.h kRecruit) refuse a battler first (the battle forbids it, an
/// owned one, no room in the party or the storage boxes: the run fails, the item stays); else the ruleset pipeline
/// recruit_odds gives the odds of one check and the engine draws the manifest's number of checks (an event per
/// check that passes; the first that fails ends them). Every check passed: the battler's record, with the battle
/// changes the rules keep, joins the user's party (else the storage boxes, Setup::storage) and the battle ends with
/// Outcome::Recruited.
/// See capsule/battle/battle.h; the modules share internal.h.
#include "capsule/combine/eggs.h"

#include "internal.h"

namespace capsule::battle::detail
{

namespace
{

/// Shows a recruit moment: its event and, when the rules give one, its message.
/// @param s        battle
/// @param m        the moment
/// @param target   the battler to recruit
/// @param value    the moment's value (checks passed, box from 1, party slot)
/// @param message  its message id, or kNone8
void moment(State& s, RecruitMoment m, uint8_t target, uint16_t value, uint8_t message)
{
   emit(s, EventKind::Recruit, target, s.recruit.user, static_cast<uint8_t>(m), value, s.recruit.item);
   if (message < data().message_count)
   {
      say(s, static_cast<Msg>(message), s.recruit.user, target, value);
   }
}

/// The message the rules give a moment.
/// @param m  the moment
/// @return its id, or kNone8
uint8_t message_of(RecruitMoment m)
{
   return kRecruit.messages[static_cast<int>(m)];
}

/// The record a recruited battler's party keeps for the user (the player's party's records).
/// @param s     battle
/// @param user  the item's user
/// @return the records, or nullptr when the user's party brings none
monster::Party* user_records(State& s, uint8_t user)
{
   PartyState& p = s.parties[s.battlers[user].party];
   return has_flag(p.flags, PARTY_UNOWNED) ? nullptr : p.records;
}

/// Whether a recruited record has somewhere to go: a free member of the user's party, else a free storage slot.
/// @param s     battle
/// @param user  the item's user
/// @return true if it has
bool has_room(State& s, uint8_t user)
{
   const monster::Party* party = user_records(s, user);
   if (party != nullptr && party->count < monster::kLayout.party_size)
   {
      return true;
   }
   return s.storage != nullptr && storage::count(*s.storage) < storage::slot_count();
}

/// Copies the elements of one record role from another record.
/// @param to    the record written
/// @param from  the record read
/// @param role  the role
/// @param n     its elements
void copy_role(monster::Record& to, const monster::Record& from, monster::RoleId role, int n)
{
   for (int k = 0; k < n; ++k)
   {
      monster::set(to, role, k, monster::get(from, role, k));
   }
}

/// A fresh record for a battler given as data takes what the battle used (review4 finding 9): its genes, trait and
/// passive slot, not the generation's new rolls; the roles the layout derives are worked out again from them (a
/// role derived from a roll the battler never had, e.g. a trait from a seed, follows the record's roll).
/// @param b    the battler
/// @param out  its new record
void keep_battle_self(const Battler& b, monster::Record& out)
{
   using monster::Role;
   if (monster::has(Role::StatGenes))
   {
      for (int k = 0; k < data().stat_count && k < monster::count(Role::StatGenes); ++k)
      {
         monster::set(out, Role::StatGenes, k, b.genes[k]);
      }
   }
   if (monster::has(Role::Trait))
   {
      monster::set(out, Role::Trait, b.trait);
   }
   if (monster::has(Role::PassiveSelector))
   {
      const SpeciesData& sp = data().species[b.species];
      for (uint8_t slot = 0; slot < data().passive_slots; ++slot)
      {
         if (sp.passives[slot] == b.passive && b.passive != kNone8)
         {
            const uint32_t mask = data().passive_selector_mask;
            const uint32_t sel = monster::get(out, Role::PassiveSelector);
            monster::set(out, Role::PassiveSelector, (sel & ~mask) | slot);
            break;
         }
      }
   }
   rederive(out);
}

/// The record of a battler as the recruit rules keep it: its party record (or a fresh one for a battler given as
/// data, keep_battle_self()), its moves, level, experience and held item from the battle, its pools (the hit points
/// and the others, never above the record's maximum) and status as the rules keep them (else full / none), the
/// player as its owner and the item in the item role.
/// @param s    battle
/// @param i    the battler
/// @param out  the record
/// @return false when no record can be made (an unknown species)
bool recruited_record(State& s, uint8_t i, monster::Record& out)
{
   using monster::Role;
   const Battler& b = s.battlers[i];
   const PartyState& p = s.parties[b.party];
   if (has_flag(b.flags, BATTLER_FROM_RECORD) && p.records != nullptr)
   {
      out = p.records->members[b.party_slot];
   }
   else
   {
      rng::Stream draws = s.rng;   // its own copy: the battle's stream stays as it is
      if (!make_monster(out, b.species, b.level, draws))
      {
         return false;
      }
      keep_battle_self(b, out);
   }
   monster::set(out, Role::Experience, b.exp);
   monster::set(out, Role::Level, b.level);
   monster::set(out, Role::HeldItem, b.item);
   for (int m = 0; m < move_slots(); ++m)
   {
      monster::set(out, Role::Moves, m, b.moves[m]);
   }
   // the battle's pools, then what the rules do not keep back to full
   store_resources(b, out);
   monster::Record full = out;
   fill_resources(full);
   const bool keep_hp = has_flag(kRecruit.keep, RECRUIT_KEEP_HP);
   const bool keep_rest = has_flag(kRecruit.keep, RECRUIT_KEEP_RESOURCES);
   for (uint8_t r = 0; r < data().resource_count; ++r)
   {
      const ResourceData& d = data().resources[r];
      const bool keep = is_faint_resource(d) ? keep_hp : keep_rest;
      if (d.role == monster::kNoRole)
      {
         continue;
      }
      const int n = is_move_resource(d) ? move_slots() : 1;
      if (!keep)
      {
         copy_role(out, full, d.role, n);
         continue;
      }
      // review4 finding 9: a kept pool is never above the record's own maximum
      for (int k = 0; k < n; ++k)
      {
         const uint32_t most = monster::get(full, d.role, k);
         if (monster::get(out, d.role, k) > most)
         {
            monster::set(out, d.role, k, most);
         }
      }
   }
   const bool keep_status = has_flag(kRecruit.keep, RECRUIT_KEEP_STATUS);
   const uint32_t counter_max = monster::max_value(Role::PrimaryStatus) >> status_id_bits();
   const uint32_t counter = b.status_counter < counter_max ? b.status_counter : counter_max;
   monster::set(out, Role::PrimaryStatus,
                keep_status ? static_cast<uint32_t>(b.status) | counter << status_id_bits() : 0u);
   if (monster::has(Role::OwnerId))
   {
      monster::set(out, Role::OwnerId, s.owner_id);
   }
   if (kRecruit.item_role != kNone8)
   {
      monster::set(out, static_cast<monster::RoleId>(kRecruit.item_role), 0, s.recruit.item);
   }
   return true;
}

/// Puts a recruited record where it goes: the user's party, else the storage boxes.
/// @param s       battle
/// @param target  the recruited battler
/// @param rec     its record
void place(State& s, uint8_t target, const monster::Record& rec)
{
   monster::Party* party = user_records(s, s.recruit.user);
   if (party != nullptr && party->count < monster::kLayout.party_size && party->count < monster::kMaxPartySize)
   {
      party->members[party->count] = rec;
      s.recruit.slot = party->count;
      s.recruit.placed = Placed::Party;
      ++party->count;
      moment(s, RecruitMoment::ToParty, target, s.recruit.slot, message_of(RecruitMoment::ToParty));
      return;
   }
   const uint16_t slot = s.storage != nullptr ? storage::deposit(*s.storage, rec) : storage::kNoSlot;
   if (slot != storage::kNoSlot)
   {
      s.recruit.slot = slot;
      s.recruit.placed = Placed::Storage;
      moment(s, RecruitMoment::ToStorage, target, static_cast<uint16_t>(storage::box_of(slot) + 1),
             message_of(RecruitMoment::ToStorage));
   }
}

/// The odds of one check (the ruleset pipeline recruit_odds).
/// @param s       battle
/// @param rng     the stream its rolls draw from, or nullptr (a preview: the rolls are fixed at 0)
/// @param user    the item's user
/// @param target  the battler to recruit
/// @param bonus   the item logic's bonus (clamped to a byte)
/// @return sure and the odds
ChanceResult check_odds(const State& s, rng::Stream* rng, uint8_t user, uint8_t target, int32_t bonus)
{
   const Battler& t = s.battlers[target];
   const Battler& u = s.battlers[user];
   const int32_t rate = kRecruit.rate_field != kNone8 ? rules::species_field_of(t.species, kRecruit.rate_field) : 0;
   const int32_t status = kRecruit.status_bonus != nullptr && t.status < data().status_count
                        ? kRecruit.status_bonus[t.status] : 0;
   PipelineCtx c = pipeline_ctx(s, rng, user, target, kNone16, rng != nullptr ? kNoFixedRoll : 0);
   const RecruitOddsInputs in{t.hp, max_hp(t), rate, clamp_to<int32_t>(bonus, 0, kCountMax8), status, t.level,
                              u.level, s.turn};
   return rules::recruit_odds(c, in);
}

} // namespace

/// Effect `recruit WHO BONUS` (Op::Recruit): the refusals, the checks and, when every check passes, the record placed
/// and the battle ended with Outcome::Recruited.
/// @param s       battle
/// @param target  the battler to recruit
/// @param bonus   the item logic's bonus (fed to recruit_odds)
/// @return pushes 1 when recruited, 0 when it broke free; fails the run when refused (the item stays)
effect::Outcome recruit_battler(State& s, uint8_t target, int32_t bonus)
{
   constexpr effect::Outcome kRefused{0, false, false, true};   // the run fails: no effect, the item stays
   // C6 (review4 finding 11): a refusal the rules say uses the item up goes on as a failed check does (pushes 0)
   constexpr effect::Outcome kConsumed{0, false, false, false};
   const uint8_t user = s.recruit.user;
   if (kRecruit.checks == 0 || s.recruit.item == kNone16 || user >= s.battler_count || !alive_active(s, target) ||
       side_of(s, target) == side_of(s, user))
   {
      return kRefused;   // no recruit rules, not the Item action, or nobody (of the other side) to recruit
   }
   if (has_flag(s.flags, SETUP_NO_RECRUIT))
   {
      moment(s, RecruitMoment::Blocked, target, 0, message_of(RecruitMoment::Blocked));
      return has_flag(kRecruit.flags, RECRUIT_CONSUME_BLOCKED) ? kConsumed : kRefused;
   }
   // B2: an egg (a battler from a record whose egg role is set) is refused unless recruit.allow_eggs
   const monster::Record* held = record_of(s, target);
   const bool egg_refused = held != nullptr && combine::refuses(*held, combine::EggUse::Recruit);
   if (egg_refused || (!has_flag(s.parties[s.battlers[target].party].flags, PARTY_UNOWNED) &&
                       !has_flag(kRecruit.flags, RECRUIT_OWNED)))
   {
      moment(s, RecruitMoment::Refused, target, 0, message_of(RecruitMoment::Refused));
      return has_flag(kRecruit.flags, RECRUIT_CONSUME_REFUSED) ? kConsumed : kRefused;
   }
   if (!has_room(s, user))
   {
      moment(s, RecruitMoment::NoRoom, target, 0, message_of(RecruitMoment::NoRoom));
      return has_flag(kRecruit.flags, RECRUIT_CONSUME_NO_ROOM) ? kConsumed : kRefused;
   }
   // the checks: all must pass (sure: no draw); the first that fails ends them
   const ChanceResult odds = check_odds(s, &s.rng, user, target, bonus);
   uint8_t passed = 0;
   while (passed < kRecruit.checks && passed < kMaxRecruitChecks)
   {
      const bool pass = odds.result != 0 ||
                        rng::chance(s.rng, static_cast<uint32_t>(rules::max32(odds.chance, 0)),
                                    static_cast<uint32_t>(rules::max32(odds.out_of, 1)));
      if (!pass)
      {
         break;
      }
      ++passed;
      moment(s, RecruitMoment::Check, target, passed, message_of(RecruitMoment::Check));
   }
   s.recruit.passed = passed;
   monster::Record rec{};
   if (passed < kRecruit.checks || !recruited_record(s, target, rec))
   {
      // it broke free: the message for the checks it passed (the last one given for more)
      const int k = kRecruit.break_free_count == 0 ? -1
                  : (passed < kRecruit.break_free_count ? passed : kRecruit.break_free_count - 1);
      moment(s, RecruitMoment::BrokeFree, target, passed, k < 0 ? kNone8 : kRecruit.break_free[k]);
      return effect::Outcome{0, false, false, false};
   }
   moment(s, RecruitMoment::Recruited, target, passed, message_of(RecruitMoment::Recruited));
   s.recruit.battler = target;
   place(s, target, rec);
   end_battle(s, Outcome::Recruited);
   return effect::Outcome{1, false, true, false};   // the battle is over: the run goes no further
}

} // namespace capsule::battle::detail

namespace capsule::battle
{

using namespace detail;

RecruitOdds recruit_odds_preview(const State& s, uint8_t user, uint8_t target, int32_t bonus)
{
   if (user >= s.battler_count || target >= s.battler_count)
   {
      return RecruitOdds{0, 0, 0};
   }
   const ChanceResult r = check_odds(s, nullptr, user, target, bonus);
   return RecruitOdds{static_cast<uint8_t>(r.result != 0 ? 1 : 0), static_cast<uint32_t>(rules::max32(r.chance, 0)),
                      static_cast<uint32_t>(rules::max32(r.out_of, 0))};
}

} // namespace capsule::battle
