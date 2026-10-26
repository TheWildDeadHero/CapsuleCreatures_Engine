/// @file
/// The battle state (C-08): events, battler queries, battlers built from records and member data,
/// HP changes and the start of a battle (the other pools: resources.cpp).
/// See capsule/battle/battle.h; the modules share internal.h.
#include "capsule/growth/growth.h"

#include "capsule/combine/eggs.h"

#include "internal.h"

namespace capsule::battle::detail
{

/// Queues a presentation event.
/// @param s        battle
/// @param kind     kind
/// @param battler  subject
/// @param target   target battler / position
/// @param extra    kind-specific
/// @param a        kind-specific
/// @param b        kind-specific
void emit(State& s, EventKind kind, uint8_t battler, uint8_t target, uint8_t extra, uint16_t a,
          uint16_t b)
{
   ENGINE_ASSERT(s.ev_count < kEventCapacity);
   if (s.ev_count >= kEventCapacity)
   {   // a unit outgrew kEventsPerStep: keep the state right, count the loss
      s.events_lost = static_cast<uint8_t>(s.events_lost < kCountMax8 ? s.events_lost + 1 : kCountMax8);
      return;
   }
   const int at = (s.ev_head + s.ev_count) & (kEventCapacity - 1);
   s.events[at] = Event{kind, battler, target, extra, a, b};
   ++s.ev_count;
}
static_assert((kEventCapacity & (kEventCapacity - 1)) == 0, "ring index uses a mask");
static_assert(kEventCapacity <= 0x8000 && kEventsPerStep < kEventCapacity);

/// Queues a message.
/// @param s       battle
/// @param m       message
/// @param user    {user}
/// @param target  {target}
/// @param b       {value} / {move} / {passive} / {item} / {party}
/// @param extra   {stat}
void say(State& s, Msg m, uint8_t user, uint8_t target, uint16_t b, uint8_t extra)
{
   emit(s, EventKind::Message, user, target, extra, static_cast<uint16_t>(m), b);
}

/// Whether a battler is on the field and able to act.
/// @param s  battle
/// @param i  battler
/// @return true if active and not fainted
bool alive_active(const State& s, uint8_t i)
{
   return i < s.battler_count && s.battlers[i].position != kBench && !has_flag(s.battlers[i].flags, BATTLER_FAINTED);
}

/// Whether a battler can still fight (bench or field).
/// @param b  battler
/// @return true if not fainted
bool usable(const Battler& b)
{
   return !has_flag(b.flags, BATTLER_FAINTED) && !has_flag(b.flags, BATTLER_EGG) && b.hp > 0;
}

/// Side of a battler.
/// @param s  battle
/// @param i  battler
/// @return side index
uint8_t side_of(const State& s, uint8_t i)
{
   return s.parties[s.battlers[i].party].side;
}

/// Whether a battler has a type.
/// @param b  battler
/// @param t  type
/// @return true if any of its types is t
bool has_type(const Battler& b, uint8_t t)
{
   if (t == kNone8)
   {
      return false;
   }
   for (uint8_t x : b.types)
   {
      if (x == t)
      {
         return true;
      }
   }
   return false;
}

/// Permanent stats from inputs (each stat's ruleset formula).
/// @param sp        species
/// @param level     level
/// @param genes     genes
/// @param training  training values
/// @param awaken    awakening values
/// @param trait     trait index
/// @param out       stats (stat_count written)
void compute_stats(const SpeciesData& sp, uint8_t level, const uint8_t* genes, const uint8_t* training,
                   const uint8_t* awaken, uint8_t trait, uint16_t* out)
{
   // The compiler keeps every stat formula's range inside [1, 0xFFFF]; the clamp only guards the narrowing.
   // A trait's table holds 100 (neutral) for the stats it cannot change.
   for (int st = 0; st < data().stat_count; ++st)
   {
      const int pct = trait < data().trait_count ? data().traits[trait].pct[st] : kPercent;
      out[st] = static_cast<uint16_t>(clamp_to<int32_t>(
         rules::stat(st, sp.base[st], genes[st], training[st], awaken[st], level, pct), 1, kMaxU16));
   }
}

/// Copies a name, at most 12 codes.
/// @param dst  13-code buffer
/// @param src  codes ending with kCodeEnd, or nullptr
void copy_name(text::Code* dst, const text::Code* src)
{
   int n = 0;
   while (src != nullptr && n < kNameCodes && src[n] != text::kCodeEnd)
   {
      dst[n] = src[n];
      ++n;
   }
   dst[n] = text::kCodeEnd;
}

/// The passive of the first rolled slot (BattleData::passive_rolled) a species fills: what an empty slot gives.
/// A slot only given (not rolled, a hidden passive) is never the fallback (C4).
/// @param sp  species
/// @return passive index, or kNone8 when the species fills no rolled slot
uint8_t first_rolled_passive(const SpeciesData& sp)
{
   for (uint8_t k = 0; k < data().passive_slots && k < kMaxPassiveSlots; ++k)
   {
      if (((data().passive_rolled >> k) & 1u) != 0 && sp.passives[k] != kNone8)
      {
         return sp.passives[k];
      }
   }
   return kNone8;
}

/// Resolves the passive a species slot gives (the manifest's passive slots, BattleData::passive_slots).
/// @param sp    species
/// @param slot  passive slot (one past the manifest's slots: as an empty slot)
/// @return passive index (an empty slot: the first filled rolled slot's, first_rolled_passive()), or kNone8
uint8_t passive_for(const SpeciesData& sp, uint8_t slot)
{
   const uint8_t a = slot < data().passive_slots ? sp.passives[slot] : kNone8;
   return a != kNone8 ? a : first_rolled_passive(sp);
}

/// Sets the parts of a battler shared by records and member data.
/// @param b        battler to fill
/// @param species  species index
/// @param level    level
void init_common(Battler& b, uint16_t species, uint8_t level)
{
   const SpeciesData& sp = data().species[species];
   b.species = species;
   b.level = level;
   b.form = growth::kBaseForm;   // G4: a record's form is set by from_record()
   for (int k = 0; k < kMaxTypesPer; ++k)
   {
      // the species' types (the compiler pads them with kNone8), no added type
      b.types[k] = k < data().species_types ? sp.types[k] : kNone8;
   }
   b.position = kBench;
   b.last_hit_by = kNone16;
   b.last_move = kNone16;
   b.targeted_by = kNone16;
   b.lock_move = kNone16;
   b.lock_target = kBench;
   b.taken = kNoTaken;
   // Every slot and pool starts empty; the layout's move_slots() and the manifest's resources are filled from
   // the record or the data (load_resources()).
   for (int i = 0; i < kMaxMoveSlots; ++i)
   {
      b.moves[i] = kNone16;
      for (int m = 0; m < kMaxMoveResources; ++m)
      {
         b.move_pool[m][i] = 0;
         b.raises[m][i] = 0;
      }
   }
   for (uint16_t& v : b.pool)
   {
      v = 0;
   }
   copy_name(b.name, sp.name);
   for (int8_t& st : b.stages)
   {
      st = 0;
   }
}

/// A record's per-stat values (genes, training, awakening), each read through record_value() (a derived role the
/// layout does not store included), for the manifest's stats.
/// @param rec       the record
/// @param genes     kMaxStats gene values (out)
/// @param training  kMaxStats training values (out)
/// @param awaken    kMaxStats awakening values (out)
void record_stat_inputs(const monster::Record& rec, uint8_t genes[kMaxStats], uint8_t training[kMaxStats],
                        uint8_t awaken[kMaxStats])
{
   using monster::Role;
   for (int i = 0; i < data().stat_count; ++i)
   {
      genes[i] = static_cast<uint8_t>(record_value(rec, monster::id_of(Role::StatGenes), i));
      training[i] = static_cast<uint8_t>(record_value(rec, monster::id_of(Role::StatTraining), i));
      awaken[i] = static_cast<uint8_t>(record_value(rec, monster::id_of(Role::StatAwaken), i));
   }
}

/// Builds a battler from a party record.
/// @param b    battler
/// @param rec  record
/// @return false if the record's species is unknown
bool from_record(Battler& b, const monster::Record& rec)
{
   using monster::Role;
   const uint16_t species = find_species(AssetKey{monster::get(rec, Role::SpeciesKey)});
   if (species == kNone16)
   {
      return false;
   }
   const uint8_t level =
      static_cast<uint8_t>(clamp_to<uint32_t>(monster::get(rec, Role::Level), 1, data().rules.level_max));
   init_common(b, species, level);
   // G4: the record's form (its form role, a derived one included) gives the base stats, types and passives
   b.form = growth::record_form(rec);
   const SpeciesData& sp = growth::species_view(species, b.form);
   for (int k = 0; k < kMaxTypesPer && b.form != growth::kBaseForm; ++k)
   {
      b.types[k] = k < data().species_types ? sp.types[k] : kNone8;
   }
   // the per-stat values, trait and passive slot may be derived roles a layout does not store (generation
   // `derived`): record_stats() reads them the same way
   record_stat_inputs(rec, b.genes, b.training, b.awaken);
   b.trait = static_cast<uint8_t>(record_value(rec, monster::id_of(Role::Trait)));
   compute_stats(sp, level, b.genes, b.training, b.awaken, b.trait, b.stats);
   for (int i = 0; i < move_slots(); ++i)
   {
      const uint32_t m = monster::get(rec, Role::Moves, i);
      b.moves[i] = m < data().move_count ? static_cast<uint16_t>(m) : kNone16;
   }
   // the pools (D-25): the persisted ones from the record (the hit points too), the others full
   load_resources(b, &rec);
   // PrimaryStatus: the status id in the layout's status_bits, its counter above
   const uint32_t st = monster::get(rec, Role::PrimaryStatus);
   b.status = static_cast<uint8_t>((st & status_id_mask()) < data().status_count ? st & status_id_mask() : 0);
   b.status_counter = static_cast<uint8_t>(st >> status_id_bits());
   // the passive selector's low bits name the slot (BattleData::passive_selector_mask); the rest are the game's
   b.passive = passive_for(sp, static_cast<uint8_t>(record_value(rec, monster::id_of(Role::PassiveSelector)) &
                                                    data().passive_selector_mask));
   const uint32_t item = monster::get(rec, Role::HeldItem);
   b.item = static_cast<uint16_t>(item < data().item_count ? item : 0);
   b.exp = monster::get(rec, Role::Experience);
   // A nickname that is not empty replaces the species name (the layout's Nickname role holds up to
   // kNameCodes codes; whether a player chose it is a game's custom role, e.g. "nicknamed").
   int n = 0;
   const int nick_codes = monster::count(Role::Nickname);
   for (; n < nick_codes && n < kNameCodes; ++n)
   {
      const text::Code code = static_cast<text::Code>(monster::get(rec, Role::Nickname, n));
      if (code == text::kCodeEnd || code == 0)
      {
         break;
      }
      b.name[n] = code;
   }
   if (n > 0)
   {
      b.name[n] = text::kCodeEnd;
   }
   // B2: an egg the battle refuses is never usable (it stays on the bench)
   const uint32_t egg = combine::refuses(rec, combine::EggUse::Battle) ? bit(BATTLER_EGG) : 0;
   b.flags = static_cast<uint8_t>(bit(BATTLER_FROM_RECORD) | (b.hp == 0 ? bit(BATTLER_FAINTED) : 0) | egg);
   return true;
}

/// Builds a battler from member data.
/// @param b  battler
/// @param m  party member
/// @return false if the species is invalid
bool from_data(Battler& b, const MemberData& m)
{
   if (m.species >= data().species_count)
   {
      return false;
   }
   const uint8_t level = static_cast<uint8_t>(clamp_to<int>(m.level, 1, data().rules.level_max));
   init_common(b, m.species, level);
   const SpeciesData& sp = data().species[m.species];
   for (int i = 0; i < data().stat_count; ++i)
   {
      b.genes[i] = m.genes;
      b.training[i] = 0;
      b.awaken[i] = 0;
   }
   b.trait = m.trait;
   compute_stats(sp, level, b.genes, b.training, b.awaken, b.trait, b.stats);
   bool any = false;
   for (int i = 0; i < move_slots(); ++i)
   {
      any = any || m.moves[i] != kNone16;
   }
   if (any)
   {
      for (int i = 0; i < move_slots(); ++i)
      {
         b.moves[i] = m.moves[i] < data().move_count ? m.moves[i] : kNone16;
      }
   }
   else
   {
      default_moves(sp, level, b.moves);
   }
   load_resources(b, nullptr);   // every pool full, no raises (the hit points too)
   b.status = 0;
   b.passive = passive_for(sp, m.passive_slot);
   b.item = m.item != kNone16 && m.item < data().item_count ? m.item : 0;
   b.exp = exp_for(sp, level);
   b.flags = 0;
   return true;
}

/// Sets a battler's HP, queues the Hp event and runs OnUpdate.
/// @param s   battle
/// @param i   battler
/// @param hp  new HP (clamped to 0..max)
void set_hp(State& s, uint8_t i, int32_t hp)
{
   Battler& b = s.battlers[i];
   const uint16_t old = b.hp;
   b.hp = static_cast<uint16_t>(clamp_to<int32_t>(hp, 0, max_hp(b)));
   if (b.hp == old)
   {
      return;
   }
   emit(s, EventKind::Hp, i, kBench, 0, b.hp, old);
   // update reacts to HP changes, but not to the ones its own handlers make (the dispatch guard)
   if (b.hp > 0)
   {
      hook_simple(s, effect::Hook::Update, i, kBench);
   }
}

/// Whether a party still has a battler that can fight.
/// @param s  battle
/// @param t  party
/// @return true if any is usable
bool party_alive(const State& s, int t)
{
   const PartyState& tr = s.parties[t];
   for (int i = 0; i < tr.count; ++i)
   {
      if (usable(s.battlers[tr.first_battler + i]))
      {
         return true;
      }
   }
   return false;
}

}   // namespace capsule::battle::detail

// -- public ------------------------------------------------------------------------------------------------

namespace capsule::battle
{

using namespace detail;

const FormatData& format(const State& s)
{
   return data().formats[s.format];
}

uint16_t find_species(AssetKey key)
{
   int lo = 0;
   int hi = static_cast<int>(data().species_count) - 1;
   while (lo <= hi)
   {
      const int mid = (lo + hi) >> 1;
      const uint32_t k = raw(data().species[mid].key);
      if (k == raw(key))
      {
         return static_cast<uint16_t>(mid);
      }
      if (k < raw(key))
      {
         lo = mid + 1;
      }
      else
      {
         hi = mid - 1;
      }
   }
   return kNone16;
}

uint8_t player_side(const State& s)
{
   for (int t = 0; t < s.party_count; ++t)
   {
      if (s.parties[t].controller == Controller::Human)
      {
         return s.parties[t].side;
      }
   }
   return 0;
}

bool start(State& s, const Setup& setup)
{
   s = State{};
   s.mv.move = kNone16;
   s.mv.chain_move = kNone16;
   s.mv.ask = kNone8;
   s.learn.battler = kBench;
   s.owner_id = setup.owner_id;
   s.rng.state = setup.seed;
   s.weather = kNone8;
   s.winning_side = kNoSide;
   s.landing = kBench;
   // G3: the storage boxes a recruited creature may go to; no Item action in progress
   s.storage = setup.storage;
   s.recruit = RecruitState{kNone16, kBench, kBench, 0, Placed::Nowhere, 0};
   if (setup.format >= data().format_count || setup.count == 0 || setup.count > kMaxParties)
   {
      return false;
   }
   s.format = setup.format;
   s.flags = setup.flags;
   const FormatData& f = data().formats[setup.format];
   for (uint8_t p = 0; p < kMaxPositions; ++p)
   {
      s.at[p] = kBench;
   }
   // Battlers.
   for (int t = 0; t < setup.count; ++t)
   {
      const Participant& pa = setup.participants[t];
      if (pa.side >= f.sides)
      {
         return false;
      }
      PartyState& tr = s.parties[t];
      const PartyData* pd = pa.data;
      tr.data = pd;
      tr.records = pa.records;
      tr.name = pa.name != nullptr ? pa.name : (pd != nullptr ? pd->name : nullptr);
      tr.controller = pa.controller;
      tr.side = pa.side;
      tr.flags = static_cast<uint8_t>(pa.flags | (pd != nullptr ? pd->flags : 0));
      // G1: the participant's profile, else the party data's, else the data's default one
      const uint8_t profile = pa.ai_profile != kAiProfileOfData ? static_cast<uint8_t>(pa.ai_profile - 1)
                            : (pd != nullptr ? pd->ai_profile : data().ai_default);
      tr.ai_profile = profile < data().ai_profile_count ? profile : data().ai_default;
      tr.rank = static_cast<uint8_t>(clamp_to<int>(pd != nullptr ? pd->rank : pa.rank, 0, data().rules.rank_max));
      // G3: the bag its Item actions use, and the party data's battle items (one use each)
      tr.bag = pa.bag;
      for (int k = 0; k < kMaxPartyItems; ++k)
      {
         tr.items[k] = pd != nullptr ? pd->items[k] : 0;
      }
      tr.first_battler = s.battler_count;
      // Members: the records when given, else the data's.
      // (a records party never brings more than its storage holds)
      const int count = pa.records != nullptr ? (pa.records->count < monster::kMaxPartySize ? pa.records->count
                                                                                            : monster::kMaxPartySize)
                                              : (pd != nullptr ? pd->count : 0);
      for (int k = 0; k < count && k < f.party_max; ++k)
      {
         if (s.battler_count >= kMaxBattlers)
         {
            return false;
         }   // more creatures than the battle holds
         Battler& b = s.battlers[s.battler_count];
         b = Battler{};
         const bool ok = pa.records != nullptr ? from_record(b, pa.records->members[k]) : from_data(b, pd->members[k]);
         if (!ok)
         {
            continue;
         }
         b.party = static_cast<uint8_t>(t);
         b.party_slot = static_cast<uint8_t>(k);
         ++s.battler_count;
         ++tr.count;
      }
      if (tr.count == 0)
      {
         return false;
      }   // nothing to send out
      s.party_count = static_cast<uint8_t>(t + 1);
   }
   // Positions: each side's positions split between its parties in order.
   for (uint8_t sd = 0; sd < f.sides; ++sd)
   {
      int parties_on_side = 0;
      for (int t = 0; t < s.party_count; ++t)
      {
         parties_on_side += s.parties[t].side == sd ? 1 : 0;
      }
      if (parties_on_side == 0)
      {
         return false;
      }
      uint8_t np_side = 0;
      for (uint8_t p = 0; p < f.positions; ++p)
      {
         np_side = static_cast<uint8_t>(np_side + (f.side_of[p] == sd ? 1 : 0));
      }
      if (parties_on_side > np_side)
      {
         return false;
      }   // every party needs a position
      uint8_t positions[kMaxPositions];
      int np = 0;
      for (uint8_t p = 0; p < f.positions; ++p)
      {
         if (f.side_of[p] == sd)
         {
            positions[np++] = p;
         }
      }
      int k = 0;
      const int per = np / parties_on_side;
      for (int t = 0; t < s.party_count; ++t)
      {
         if (s.parties[t].side != sd)
         {
            continue;
         }
         for (int j = 0; j < per && k < np; ++j)
         {
            s.parties[t].positions = static_cast<uint8_t>(s.parties[t].positions | bit(positions[k++]));
         }
      }
      // Leftover positions go to the side's last party.
      for (int t = s.party_count - 1; t >= 0 && k < np; --t)
      {
         if (s.parties[t].side != sd)
         {
            continue;
         }
         while (k < np)
         {
            s.parties[t].positions = static_cast<uint8_t>(s.parties[t].positions | bit(positions[k++]));
         }
      }
   }
   // Unowned creatures never replace each other: each needs its own position.
   for (int t = 0; t < s.party_count; ++t)
   {
      const PartyState& tr = s.parties[t];
      int held = 0;
      for (uint8_t p = 0; p < kMaxPositions; ++p)
      {
         held += (tr.positions >> p) & 1;
      }
      if (has_flag(tr.flags, PARTY_UNOWNED) && tr.count > held)
      {
         return false;
      }
   }
   if (setup.weather != kNone8 && setup.weather < data().weather_count)
   {
      s.weather = setup.weather;
      s.weather_turns = 0;
   }
   s.phase = Phase::Intro;
   s.sub = 0;   // the first party's intro
   return true;
}

bool pop_event(State& s, Event& out)
{
   if (s.ev_count == 0)
   {
      return false;
   }
   out = s.events[s.ev_head];
   s.ev_head = static_cast<uint16_t>((s.ev_head + 1) & (kEventCapacity - 1));
   --s.ev_count;
   return true;
}

bool record_stats(const monster::Record& rec, uint16_t out[kMaxStats])
{
   using monster::Role;
   const uint16_t species = find_species(AssetKey{monster::get(rec, Role::SpeciesKey)});
   if (species == kNone16)
   {
      return false;
   }
   // the same inputs as a battle's (from_record()): derived roles through record_value()
   uint8_t genes[kMaxStats];
   uint8_t training[kMaxStats];
   uint8_t awaken[kMaxStats];
   record_stat_inputs(rec, genes, training, awaken);
   const uint8_t level =
      static_cast<uint8_t>(clamp_to<uint32_t>(monster::get(rec, Role::Level), 1, data().rules.level_max));
   compute_stats(growth::species_view(species, growth::record_form(rec)), level, genes, training, awaken,
                 static_cast<uint8_t>(record_value(rec, monster::id_of(Role::Trait))), out);
   return true;
}

}   // namespace capsule::battle
