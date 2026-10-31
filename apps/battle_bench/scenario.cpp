/// @file
/// Reading a `capsule.bench/1` scenario (see scenario.h and tools/bench_scenario.py for the format).
#include "scenario.h"

#include "bench_data.h"
#include "output.h"

#include <cstring>

namespace capsule::bench
{

namespace
{

using namespace capsule::battle;
using json::Kind;
using json::Value;
using monster::Role;
using transcript::ChoiceKind;

/// The scenario format this bench reads.
constexpr const char* kSchema = "capsule.bench/1";
/// Default step limit (the goldens' kStepLimit).
constexpr int kDefaultStepLimit = 20000;
/// Most names an "unknown name" message lists.
constexpr int kNamesShown = 12;
/// Largest record seed, level, HP and experience a scenario may give (32-bit roles).
constexpr int64_t kMaxU32 = UINT32_MAX;
/// Largest byte value.
constexpr int64_t kMaxU8 = UINT8_MAX;
/// Largest 16-bit value.
constexpr int64_t kMaxU16 = UINT16_MAX;

/// The parties' bags (G3: a party's "bag"; static storage).
inventory::Bag g_bags[kMaxParties];
/// The storage boxes a recruited creature goes to when the party is full (G3; empty at the start of each run).
storage::Buffer<storage::kMaxStorageBytes, storage::kUsedBytes> g_box_bytes;   ///< Their bytes (host: the most).
storage::Storage g_boxes = storage::view(g_box_bytes);                          ///< Their view.

/// Reads a scenario, collecting errors with the path where they are.
struct Loader
{
   monster::Party* parties;   ///< Record storage.
   Scenario& sc;              ///< The result.
   int errors = 0;            ///< Errors written.

   /// Writes an error.
   /// @param where    path in the scenario
   /// @param message  what is wrong
   /// @return false
   bool fail(const std::string& where, const std::string& message)
   {
      ++errors;
      error(message, where);
      return false;
   }

   /// Checks a value's kind.
   /// @param v      the value
   /// @param k      expected kind
   /// @param where  its path
   /// @return false (error written) when it differs
   bool expect(const Value& v, Kind k, const std::string& where)
   {
      if (v.kind == k)
      {
         return true;
      }
      return fail(where, std::string("expected ") + json::kind_name(k) + ", got " + json::kind_name(v.kind));
   }

   /// Rejects members an object may not have.
   /// @param v      an object
   /// @param keys   the allowed names (nullptr-terminated)
   /// @param where  its path
   /// @return false when one is unknown
   bool only(const Value& v, const char* const* keys, const std::string& where)
   {
      bool ok = true;
      for (const auto& m : v.members)
      {
         bool known = false;
         for (int k = 0; keys[k] != nullptr; ++k)
         {
            known = known || m.first == keys[k];
         }
         if (!known)
         {
            ok = fail(where, "unknown member \"" + m.first + "\"");
         }
      }
      return ok;
   }

   /// Reads an integer in a range.
   /// @param v      the value
   /// @param lo     lowest
   /// @param hi     highest
   /// @param where  its path
   /// @param out    the integer
   /// @return false on an error
   bool integer(const Value& v, int64_t lo, int64_t hi, const std::string& where, int64_t& out)
   {
      if (!expect(v, Kind::Number, where))
      {
         return false;
      }
      if (v.number < lo || v.number > hi)
      {
         return fail(where, "out of range " + std::to_string(lo) + ".." + std::to_string(hi));
      }
      out = v.number;
      return true;
   }

   /// Resolves a manifest name.
   /// @param list   the list key ("species", "moves", ...)
   /// @param v      the value (a string)
   /// @param where  its path
   /// @param out    the data index
   /// @return false on an error (an unknown name lists the known ones)
   bool name(const char* list, const Value& v, const std::string& where, int& out)
   {
      if (!expect(v, Kind::String, where))
      {
         return false;
      }
      for (int l = 0; l < kBenchData.list_count; ++l)
      {
         const NameList& nl = kBenchData.lists[l];
         if (std::strcmp(nl.list, list) != 0)
         {
            continue;
         }
         std::string known;
         for (int k = 0; k < nl.count; ++k)
         {
            if (v.text == nl.names[k])
            {
               out = nl.first + k;
               return true;
            }
            if (k < kNamesShown)
            {
               known += (k > 0 ? ", " : "") + std::string(nl.names[k]);
            }
         }
         if (nl.count > kNamesShown)
         {
            known += ", ...";
         }
         return fail(where, "\"" + v.text + "\" is not in the data's " + list + " (known: " +
                               (known.empty() ? "none" : known) + ")");
      }
      return fail(where, std::string("the data has no ") + list + " list");
   }

   /// Sets a record role, if the layout has it.
   /// @param rec    record
   /// @param role   role
   /// @param value  value
   /// @param where  path (for the error)
   /// @return false when the layout has no such role
   bool set_role(monster::Record& rec, Role role, uint32_t value, const std::string& where)
   {
      if (!monster::has(role))
      {
         return fail(where, "the record layout has no such role");
      }
      monster::set(rec, role, value);
      return true;
   }

   /// Reads a "resources" object: {resource name: value} for a battler resource, {name: [value per move slot]}
   /// for a move resource (D-25).
   /// @param v      the object
   /// @param where  its path
   /// @param out    the values (appended)
   /// @return false on an error
   bool resource_values(const Value& v, const std::string& where, std::vector<ResourcePoke>& out)
   {
      if (!expect(v, Kind::Object, where))
      {
         return false;
      }
      bool ok = true;
      for (const auto& m : v.members)
      {
         Value key;
         key.kind = Kind::String;
         key.text = m.first;
         const std::string at = where + "." + m.first;
         int r = 0;
         if (!name("resources", key, at, r))
         {
            ok = false;
            continue;
         }
         int64_t n = 0;
         if (kData.resources[r].scope != static_cast<uint8_t>(ResourceScope::Move))
         {
            ok = integer(m.second, 0, kMaxPoolValue, at, n) && ok;
            out.push_back(ResourcePoke{static_cast<uint8_t>(r), -1, static_cast<uint16_t>(n)});
            continue;
         }
         // a move resource: one value per move slot
         if (!expect(m.second, Kind::Array, at) || static_cast<int>(m.second.items.size()) > move_slots())
         {
            ok = fail(at, "one value per move slot at most (a list)");
            continue;
         }
         for (size_t k = 0; k < m.second.items.size(); ++k)
         {
            ok = integer(m.second.items[k], 0, kMaxMovePoolValue, at + "[" + std::to_string(k) + "]", n) && ok;
            out.push_back(ResourcePoke{static_cast<uint8_t>(r), static_cast<int8_t>(k), static_cast<uint16_t>(n)});
         }
      }
      return ok;
   }

   /// Reads one record member and creates its record.
   /// @param v       the member
   /// @param party   participant index
   /// @param member  member index
   /// @param where   its path
   /// @return false on an error
   bool record_member(const Value& v, int party, int member, const std::string& where)
   {
      static const char* const kKeys[] = {"species", "level", "seed", "moves", "resources", "passive_slot", "trait",
                                          "item", "hp", "status", "status_counter", "experience", nullptr};
      if (v.find("pp") != nullptr)
      {
         return fail(where + ".pp", "PP is a resource now (D-25): \"resources\": {\"pp\": [value per move slot]}");
      }
      if (!expect(v, Kind::Object, where) || !only(v, kKeys, where))
      {
         return false;
      }
      const Value* sv = v.find("species");
      const Value* lv = v.find("level");
      if (sv == nullptr || lv == nullptr)
      {
         return fail(where, "a member needs \"species\" and \"level\"");
      }
      int species = 0;
      int64_t level = 0;
      bool ok = name("species", *sv, where + ".species", species);
      ok = integer(*lv, 1, kMaxU8, where + ".level", level) && ok;
      if (!ok)
      {
         return false;
      }
      int64_t seed = transcript::member_seed(party, member, static_cast<uint16_t>(species),
                                             static_cast<uint8_t>(level));
      if (const Value* x = v.find("seed"))
      {
         ok = integer(*x, 0, kMaxU32, where + ".seed", seed) && ok;
      }
      monster::Record& rec = parties[party].members[member];
      rec = monster::Record{};
      rng::Stream r{static_cast<uint32_t>(seed)};
      if (!make_monster(rec, static_cast<uint16_t>(species), static_cast<uint8_t>(level), r))
      {
         return fail(where, "make_monster refused this species and level");
      }

      // Moves first (full move pools), then the resource values and the other roles.
      if (const Value* x = v.find("moves"))
      {
         if (expect(*x, Kind::Array, where + ".moves"))
         {
            uint16_t moves[monster::kMaxMoveSlots];
            const int n = static_cast<int>(x->items.size());
            if (n > move_slots())
            {
               ok = fail(where + ".moves", "more moves than the layout's " + std::to_string(move_slots()) + " slots");
            }
            for (int k = 0; k < n && k < move_slots(); ++k)
            {
               int m = 0;
               const std::string at = where + ".moves[" + std::to_string(k) + "]";
               if (x->items[k].kind == Kind::Null)
               {
                  moves[k] = kNone16;
                  continue;
               }
               ok = name("moves", x->items[k], at, m) && ok;
               moves[k] = static_cast<uint16_t>(m);
            }
            if (ok)
            {
               transcript::set_moves(rec, moves, n);
            }
         }
         else
         {
            ok = false;
         }
      }
      if (const Value* x = v.find("resources"))
      {
         // a record holds the persisted pools only (the others are full every battle)
         std::vector<ResourcePoke> values;
         ok = resource_values(*x, where + ".resources", values) && ok;
         for (const ResourcePoke& rp : values)
         {
            const ResourceData& d = kData.resources[rp.resource];
            if (d.role == monster::kNoRole)
            {
               ok = fail(where + ".resources", "a resource without a persist role is refilled every battle (poke it "
                                               "with pokes[].resources)");
               continue;
            }
            monster::set(rec, d.role, rp.slot < 0 ? 0 : rp.slot, rp.value);
         }
      }
      int64_t n = 0;
      int idx = 0;
      if (const Value* x = v.find("passive_slot"))
      {
         ok = integer(*x, 0, battle::kData.passive_slots - 1, where + ".passive_slot", n) &&
              set_role(rec, Role::PassiveSelector, static_cast<uint32_t>(n), where + ".passive_slot") && ok;
      }
      if (const Value* x = v.find("trait"))
      {
         ok = name("traits", *x, where + ".trait", idx) &&
              set_role(rec, Role::Trait, static_cast<uint32_t>(idx), where + ".trait") && ok;
      }
      if (const Value* x = v.find("item"))
      {
         idx = 0;
         ok = (x->kind == Kind::Null || name("items", *x, where + ".item", idx)) &&
              set_role(rec, Role::HeldItem, static_cast<uint32_t>(idx), where + ".item") && ok;
      }
      if (const Value* x = v.find("experience"))
      {
         ok = integer(*x, 0, kMaxU32, where + ".experience", n) &&
              set_role(rec, Role::Experience, static_cast<uint32_t>(n), where + ".experience") && ok;
      }
      if (const Value* x = v.find("hp"))
      {
         ok = integer(*x, 0, kMaxU16, where + ".hp", n) &&
              set_role(rec, Role::CurrentHp, static_cast<uint32_t>(n), where + ".hp") && ok;
      }
      if (v.find("status") != nullptr || v.find("status_counter") != nullptr)
      {
         // The PrimaryStatus role: the status in its low bits, its counter above.
         idx = 0;
         int64_t counter = 0;
         if (const Value* x = v.find("status"))
         {
            ok = (x->kind == Kind::Null || name("statuses", *x, where + ".status", idx)) && ok;
         }
         if (const Value* x = v.find("status_counter"))
         {
            ok = integer(*x, 0, kMaxU8, where + ".status_counter", counter) && ok;
         }
         ok = set_role(rec, Role::PrimaryStatus,
                       static_cast<uint32_t>(idx) | static_cast<uint32_t>(counter) << battle::kData.status_bits,
                       where + ".status") &&
              ok;
      }
      return ok;
   }

   /// Reads a party's bag ({item name: count}).
   /// @param v      the bag
   /// @param t      participant index
   /// @param where  its path
   /// @return false on an error
   bool bag(const Value& v, int t, const std::string& where)
   {
      inventory::clear(g_bags[t]);
      if (!expect(v, Kind::Object, where))
      {
         return false;
      }
      bool ok = true;
      for (const auto& m : v.members)
      {
         const std::string at = where + "." + m.first;
         Value key{};
         key.kind = Kind::String;
         key.text = m.first;
         int idx = 0;
         int64_t n = 0;
         if (!name("items", key, at, idx) || !integer(m.second, 1, kMaxU16, at, n))
         {
            ok = false;
            continue;
         }
         if (!inventory::add(g_bags[t], static_cast<uint16_t>(idx), static_cast<uint32_t>(n)))
         {
            ok = fail(at, "the bag has no room for them (its pocket's capacity or quantity_max)");
         }
      }
      return ok;
   }

   /// Reads one party.
   /// @param v      the party
   /// @param t      participant index
   /// @param where  its path
   /// @return false on an error
   bool party(const Value& v, int t, const std::string& where)
   {
      static const char* const kKeys[] = {"side", "controller", "data", "members", "flags", "rank", "ai", "bag",
                                          nullptr};
      if (!expect(v, Kind::Object, where) || !only(v, kKeys, where))
      {
         return false;
      }
      Participant p{nullptr, nullptr, nullptr, Controller::Ai, 0, 0, 0};
      bool ok = true;
      int64_t n = 0;
      if (const Value* x = v.find("bag"))
      {
         // G3: the bag its Item actions use ({item: count})
         ok = bag(*x, t, where + ".bag") && ok;
         p.bag = &g_bags[t];
      }
      if (const Value* x = v.find("side"))
      {
         ok = integer(*x, 0, kMaxSides - 1, where + ".side", n) && ok;
         p.side = static_cast<uint8_t>(n);
      }
      else
      {
         ok = fail(where, "a party needs \"side\"");
      }
      if (const Value* x = v.find("controller"))
      {
         if (expect(*x, Kind::String, where + ".controller") && (x->text == "human" || x->text == "ai"))
         {
            p.controller = x->text == "human" ? Controller::Human : Controller::Ai;
         }
         else
         {
            ok = fail(where + ".controller", "\"human\" or \"ai\"");
         }
      }
      if (const Value* x = v.find("rank"))
      {
         ok = integer(*x, 0, kMaxU8, where + ".rank", n) && ok;
         p.rank = static_cast<uint8_t>(n);
      }
      if (const Value* x = v.find("ai"))
      {
         // G1: the party's AI profile by name (else the data party's, else the default one)
         int idx = 0;
         if (name("ai_profiles", *x, where + ".ai", idx))
         {
            p.ai_profile = static_cast<uint8_t>(idx + 1);
         }
         else
         {
            ok = false;
         }
      }
      if (const Value* x = v.find("flags"))
      {
         if (expect(*x, Kind::Array, where + ".flags"))
         {
            for (size_t k = 0; k < x->items.size(); ++k)
            {
               const Value& f = x->items[k];
               const std::string at = where + ".flags[" + std::to_string(k) + "]";
               if (f.kind == Kind::String && f.text == "unowned")
               {
                  p.flags |= 1u << PARTY_UNOWNED;
               }
               else if (f.kind == Kind::String && f.text == "no_escape")
               {
                  p.flags |= 1u << PARTY_NO_ESCAPE;
               }
               else
               {
                  ok = fail(at, "a party flag is \"unowned\" or \"no_escape\"");
               }
            }
         }
         else
         {
            ok = false;
         }
      }
      const Value* data = v.find("data");
      const Value* members = v.find("members");
      if ((data == nullptr) == (members == nullptr))
      {
         return fail(where, "a party has either \"data\" (a party of the data) or \"members\" (records)");
      }
      if (data != nullptr)
      {
         int idx = 0;
         if (name("parties", *data, where + ".data", idx))
         {
            p.data = &kData.parties[idx];
         }
         else
         {
            ok = false;
         }
      }
      else if (expect(*members, Kind::Array, where + ".members"))
      {
         const int count = static_cast<int>(members->items.size());
         if (count < 1 || count > monster::kLayout.party_size)
         {
            ok = fail(where + ".members", "1 to " + std::to_string(monster::kLayout.party_size) + " members");
         }
         else
         {
            parties[t] = monster::Party{};
            for (int k = 0; k < count; ++k)
            {
               ok = record_member(members->items[k], t, k, where + ".members[" + std::to_string(k) + "]") && ok;
               parties[t].count = static_cast<uint8_t>(k + 1);
            }
            p.records = &parties[t];
         }
      }
      else
      {
         ok = false;
      }
      sc.setup.participants[t] = p;
      return ok;
   }

   /// Reads a battler poke.
   /// @param v      the poke
   /// @param where  its path
   /// @return false on an error
   bool poke(const Value& v, const std::string& where)
   {
      static const char* const kKeys[] = {"party", "member", "hp", "status", "status_counter", "status_aux",
                                          "counters", "item", "volatiles", "stages",
                                          "resources", nullptr};
      if (!expect(v, Kind::Object, where) || !only(v, kKeys, where))
      {
         return false;
      }
      Poke pk{};
      bool ok = true;
      int64_t n = 0;
      int idx = 0;
      const Value* pv = v.find("party");
      const Value* mv = v.find("member");
      if (pv == nullptr || mv == nullptr)
      {
         return fail(where, "a poke needs \"party\" and \"member\"");
      }
      ok = integer(*pv, 0, sc.setup.count - 1, where + ".party", n) && ok;
      pk.party = static_cast<uint8_t>(n);
      ok = integer(*mv, 0, kMaxBattlers - 1, where + ".member", n) && ok;
      pk.member = static_cast<uint8_t>(n);

      // Plain counters: HP and the status counter and scratch.
      struct Counter
      {
         const char* key;   ///< Member name.
         PokeField field;   ///< Its bit.
         int64_t max;       ///< Largest value.
      };
      const Counter counters[] = {{"hp", POKE_HP, kMaxU16},
                                  {"status_counter", POKE_STATUS_COUNTER, kMaxU8},
                                  {"status_aux", POKE_STATUS_AUX, kMaxU8}};
      for (const Counter& c : counters)
      {
         const Value* x = v.find(c.key);
         if (x == nullptr)
         {
            continue;
         }
         if (!integer(*x, 0, c.max, where + "." + c.key, n))
         {
            ok = false;
            continue;
         }
         pk.fields |= 1u << c.field;
         switch (c.field)
         {
         case POKE_HP:
            pk.hp = static_cast<uint16_t>(n);
            break;
         case POKE_STATUS_COUNTER:
            pk.status_counter = static_cast<uint8_t>(n);
            break;
         default:
            pk.status_aux = static_cast<uint8_t>(n);
            break;
         }
      }
      // Volatile counters, by the volatile that owns the slot: "counters": {volatile: n}.
      const auto set_counter = [&](const std::string& volatile_name, const Value& x, const std::string& at)
      {
         Value vname;
         vname.kind = Kind::String;
         vname.text = volatile_name;
         int vi = 0;
         if (!integer(x, 0, kMaxU8, at, n) || !name("volatiles", vname, at, vi))
         {
            return false;
         }
         const uint8_t slot = battle::kData.volatiles[vi].counter;
         if (slot >= battle::kMaxVolatileCounters)
         {
            return fail(at, "the volatile " + volatile_name + " has no counter");
         }
         pk.counters[slot] = static_cast<uint8_t>(n);
         pk.counter_mask = static_cast<uint8_t>(pk.counter_mask | 1u << slot);
         return true;
      };
      if (const Value* x = v.find("counters"))
      {
         if (expect(*x, Kind::Object, where + ".counters"))
         {
            for (const auto& m : x->members)
            {
               ok = set_counter(m.first, m.second, where + ".counters." + m.first) && ok;
            }
         }
         else
         {
            ok = false;
         }
      }
      if (const Value* x = v.find("status"))
      {
         idx = 0;
         ok = (x->kind == Kind::Null || name("statuses", *x, where + ".status", idx)) && ok;
         pk.status = static_cast<uint8_t>(idx);
         pk.fields |= 1u << POKE_STATUS;
      }
      if (const Value* x = v.find("item"))
      {
         idx = 0;
         ok = (x->kind == Kind::Null || name("items", *x, where + ".item", idx)) && ok;
         pk.item = static_cast<uint16_t>(idx);
         pk.fields |= 1u << POKE_ITEM;
      }
      if (const Value* x = v.find("volatiles"))
      {
         if (expect(*x, Kind::Array, where + ".volatiles"))
         {
            for (size_t k = 0; k < x->items.size(); ++k)
            {
               if (name("volatiles", x->items[k], where + ".volatiles[" + std::to_string(k) + "]", idx))
               {
                  pk.volatiles |= 1u << idx;
               }
               else
               {
                  ok = false;
               }
            }
            pk.fields |= 1u << POKE_VOLATILES;
         }
         else
         {
            ok = false;
         }
      }
      if (const Value* x = v.find("stages"))
      {
         if (expect(*x, Kind::Object, where + ".stages"))
         {
            for (const auto& m : x->members)
            {
               Value key;
               key.kind = Kind::String;
               key.text = m.first;
               const std::string at = where + ".stages." + m.first;
               // a value within that stage's own range (the manifest's stages[].range)
               if (!name("stages", key, at, idx) ||
                   !integer(m.second, battle::kData.stages[idx].lo, battle::kData.stages[idx].hi, at, n))
               {
                  ok = false;
                  continue;
               }
               pk.stages[idx] = static_cast<int8_t>(n);
               pk.stage_mask |= uint64_t{1} << idx;
            }
         }
         else
         {
            ok = false;
         }
      }
      if (const Value* x = v.find("resources"))
      {
         ok = resource_values(*x, where + ".resources", pk.resources) && ok;
      }
      sc.pokes.push_back(pk);
      return ok;
   }

   /// Reads an item choice (G3): the item by name, its target position, member and move slot.
   /// @param v      the choice
   /// @param it     its "item"
   /// @param where  its path
   /// @param out    the pick
   /// @return false on an error
   bool item_pick(const Value& v, const Value& it, const std::string& where, Pick& out)
   {
      out.choice.kind = ChoiceKind::Item;
      int idx = 0;
      bool ok = name("items", it, where + ".item", idx);
      out.choice.item = static_cast<uint16_t>(idx);   // the items list's ids count from 1
      int64_t n = 0;
      if (const Value* t = v.find("target"))
      {
         ok = integer(*t, 0, kMaxPositions - 1, where + ".target", n) && ok;
         out.choice.target = static_cast<uint8_t>(n);
      }
      if (const Value* m = v.find("member"))
      {
         ok = integer(*m, 0, kMaxBattlers - 1, where + ".member", n) && ok;
         out.choice.member = static_cast<uint8_t>(n);
      }
      if (const Value* sl = v.find("slot"))
      {
         ok = integer(*sl, 0, move_slots() - 1, where + ".slot", n) && ok;
         out.choice.slot = static_cast<uint8_t>(n);
      }
      return ok;
   }

   /// Reads one choice.
   /// @param v      the choice
   /// @param where  its path
   /// @param out    the pick
   /// @return false on an error
   bool pick(const Value& v, const std::string& where, Pick& out)
   {
      out = Pick{transcript::Choice{ChoiceKind::First, 0, kBench, 0}, kNone16};
      if (v.kind == Kind::String && (v.text == "first" || v.text == "run"))
      {
         out.choice.kind = v.text == "run" ? ChoiceKind::Run : ChoiceKind::First;
         return true;
      }
      static const char* const kKeys[] = {"move", "target", "switch", "item", "member", "slot", nullptr};
      if (v.kind != Kind::Object || !only(v, kKeys, where))
      {
         return fail(where, "a choice is \"first\", \"run\", {\"move\": slot or name, \"target\": position}, "
                            "{\"switch\": member} or {\"item\": name, \"target\" / \"member\" / \"slot\"}");
      }
      int64_t n = 0;
      if (const Value* it = v.find("item"))
      {
         return item_pick(v, *it, where, out);
      }
      const Value* mv = v.find("move");
      const Value* sw = v.find("switch");
      if ((mv == nullptr) == (sw == nullptr))
      {
         return fail(where, "a choice object has \"move\" or \"switch\"");
      }
      if (sw != nullptr)
      {
         out.choice.kind = ChoiceKind::Switch;
         const bool ok = integer(*sw, 0, kMaxBattlers - 1, where + ".switch", n);
         out.choice.member = static_cast<uint8_t>(n);
         return ok;
      }
      out.choice.kind = ChoiceKind::Move;
      bool ok = true;
      if (mv->kind == Kind::String)
      {
         int idx = 0;
         ok = name("moves", *mv, where + ".move", idx);
         out.move = static_cast<uint16_t>(idx);
      }
      else
      {
         ok = integer(*mv, 0, move_slots() - 1, where + ".move", n);
         out.choice.slot = static_cast<uint8_t>(n);
      }
      if (const Value* t = v.find("target"))
      {
         ok = integer(*t, 0, kMaxPositions - 1, where + ".target", n) && ok;
         out.choice.target = static_cast<uint8_t>(n);
      }
      return ok;
   }

   /// Reads one answer ({"learn_move": slot}, {"switch_target": member} or {"choice": option}).
   /// @param v      the answer
   /// @param where  its path
   /// @return false on an error
   bool answer(const Value& v, const std::string& where)
   {
      if (v.kind != Kind::Object || v.members.size() != 1)
      {
         return fail(where, "an answer is {\"learn_move\": slot}, {\"switch_target\": member} or {\"choice\": n}");
      }
      const std::string& key = v.members[0].first;
      Answer a{RequestKind::Choice, 0};
      if (key == "learn_move")
      {
         a.kind = RequestKind::LearnMove;
      }
      else if (key == "switch_target")
      {
         a.kind = RequestKind::SwitchTarget;
      }
      else if (key != "choice")
      {
         return fail(where, "unknown request \"" + key + "\" (learn_move, switch_target, choice)");
      }
      int64_t n = 0;
      if (!integer(v.members[0].second, 0, kMaxU8, where + "." + key, n))
      {
         return false;
      }
      a.value = static_cast<int32_t>(n);
      sc.answers.push_back(a);
      return true;
   }

   /// Reads the whole document.
   /// @param doc  the document
   /// @return false on an error
   bool load(const Value& doc)
   {
      static const char* const kKeys[] = {"schema", "name", "description", "expect", "rng", "format", "seed",
                                          "no_escape", "field", "parties", "pokes", "choices", "answers", "stop",
                                          "step_limit", "trace", "owner_texts", nullptr};
      if (!expect(doc, Kind::Object, ""))
      {
         return false;
      }
      only(doc, kKeys, "");
      const Value* schema = doc.find("schema");
      if (schema == nullptr || schema->kind != Kind::String || schema->text != kSchema)
      {
         return fail("schema", std::string("not a ") + kSchema + " scenario");
      }
      sc.setup = Setup{};
      sc.setup.weather = kNone8;
      // G3: empty storage boxes for a recruit into a full party
      storage::clear(g_boxes);
      sc.setup.storage = &g_boxes;
      sc.weather_turns_given = false;
      sc.weather_turns = 0;
      sc.stop = StopKind::End;
      sc.stop_value = 0;
      sc.step_limit = kDefaultStepLimit;
      sc.owner_texts = false;
      sc.name = "scenario";
      int64_t n = 0;
      int idx = 0;
      if (const Value* x = doc.find("name"))
      {
         if (expect(*x, Kind::String, "name"))
         {
            sc.name = x->text;
         }
      }
      if (const Value* x = doc.find("rng"))
      {   // the generator the scenario's expectations were made with: a bench linking another one refuses it
         if (expect(*x, Kind::String, "rng") && x->text != CAPSULE_BENCH_RNG)
         {
            return fail("rng", "the scenario needs the " + x->text + " generator; this bench links " +
                                  CAPSULE_BENCH_RNG);
         }
      }
      if (const Value* x = doc.find("format"))
      {
         if (name("formats", *x, "format", idx))
         {
            sc.setup.format = static_cast<uint8_t>(idx);
         }
      }
      if (const Value* x = doc.find("seed"))
      {
         if (integer(*x, 0, kMaxU32, "seed", n))
         {
            sc.setup.seed = static_cast<uint32_t>(n);
         }
      }
      else
      {
         fail("", "a scenario needs \"seed\"");
      }
      if (const Value* x = doc.find("no_escape"))
      {
         if (expect(*x, Kind::Bool, "no_escape") && x->boolean)
         {
            sc.setup.flags |= 1u << SETUP_NO_ESCAPE;
         }
      }
      if (const Value* x = doc.find("owner_texts"))
      {
         // E13b: the transcript also shows owner texts and condition changes (transcript::kOwnerTexts)
         if (expect(*x, Kind::Bool, "owner_texts"))
         {
            sc.owner_texts = x->boolean;
         }
      }
      if (const Value* f = doc.find("field"))
      {
         static const char* const kFieldKeys[] = {"weather", "weather_turns", nullptr};
         if (expect(*f, Kind::Object, "field") && only(*f, kFieldKeys, "field"))
         {
            if (const Value* x = f->find("weather"))
            {
               if (x->kind != Kind::Null && name("weathers", *x, "field.weather", idx))
               {
                  sc.setup.weather = static_cast<uint8_t>(idx);
               }
            }
            if (const Value* x = f->find("weather_turns"))
            {
               if (integer(*x, 0, kMaxU8, "field.weather_turns", n))
               {
                  sc.weather_turns_given = true;
                  sc.weather_turns = static_cast<uint8_t>(n);
               }
            }
         }
      }
      const Value* ps = doc.find("parties");
      if (ps == nullptr)
      {
         fail("", "a scenario needs \"parties\"");
      }
      else if (expect(*ps, Kind::Array, "parties"))
      {
         if (ps->items.empty() || ps->items.size() > static_cast<size_t>(kMaxParties))
         {
            fail("parties", "1 to " + std::to_string(kMaxParties) + " parties");
         }
         else
         {
            sc.setup.count = static_cast<uint8_t>(ps->items.size());
            for (size_t t = 0; t < ps->items.size(); ++t)
            {
               party(ps->items[t], static_cast<int>(t), "parties[" + std::to_string(t) + "]");
            }
         }
      }
      if (const Value* x = doc.find("pokes"))
      {
         if (expect(*x, Kind::Array, "pokes"))
         {
            for (size_t k = 0; k < x->items.size(); ++k)
            {
               poke(x->items[k], "pokes[" + std::to_string(k) + "]");
            }
         }
      }
      if (const Value* x = doc.find("choices"))
      {
         if (expect(*x, Kind::Array, "choices"))
         {
            for (size_t k = 0; k < x->items.size(); ++k)
            {
               const Value& turn = x->items[k];
               const std::string at = "choices[" + std::to_string(k) + "]";
               TurnPicks picks;
               if (turn.kind == Kind::Array)
               {
                  // One choice per field position.
                  for (size_t j = 0; j < turn.items.size(); ++j)
                  {
                     Pick p;
                     pick(turn.items[j], at + "[" + std::to_string(j) + "]", p);
                     picks.push_back(p);
                  }
                  if (picks.empty())
                  {
                     fail(at, "a turn's list of choices is not empty");
                  }
               }
               else
               {
                  Pick p;
                  pick(turn, at, p);
                  picks.push_back(p);
               }
               sc.choices.push_back(picks);
            }
         }
      }
      if (const Value* x = doc.find("answers"))
      {
         if (expect(*x, Kind::Array, "answers"))
         {
            for (size_t k = 0; k < x->items.size(); ++k)
            {
               answer(x->items[k], "answers[" + std::to_string(k) + "]");
            }
         }
      }
      if (const Value* x = doc.find("stop"))
      {
         if (x->kind == Kind::String && x->text == "end")
         {
            sc.stop = StopKind::End;
         }
         else if (x->kind == Kind::Object && x->members.size() == 1 &&
                  (x->members[0].first == "turns" || x->members[0].first == "steps"))
         {
            sc.stop = x->members[0].first == "turns" ? StopKind::Turns : StopKind::Steps;
            if (integer(x->members[0].second, 1, kMaxU16, "stop." + x->members[0].first, n))
            {
               sc.stop_value = static_cast<int>(n);
            }
         }
         else
         {
            fail("stop", "\"end\", {\"turns\": n} or {\"steps\": n}");
         }
      }
      if (const Value* x = doc.find("step_limit"))
      {
         if (integer(*x, 1, kMaxU32 >> 1, "step_limit", n))
         {
            sc.step_limit = static_cast<int>(n);
         }
      }
      if (const Value* x = doc.find("trace"))
      {
         static const char* const kTraceKeys[] = {"rng", "vm", "events", nullptr};
         if (expect(*x, Kind::Object, "trace") && only(*x, kTraceKeys, "trace"))
         {
            bool* flags[] = {&sc.trace.rng, &sc.trace.vm, &sc.trace.events};
            for (int k = 0; kTraceKeys[k] != nullptr; ++k)
            {
               if (const Value* t = x->find(kTraceKeys[k]))
               {
                  if (expect(*t, Kind::Bool, std::string("trace.") + kTraceKeys[k]))
                  {
                     *flags[k] = t->boolean;
                  }
               }
            }
         }
      }
      return errors == 0;
   }
};

} // namespace

bool load_scenario(const json::Value& doc, monster::Party* parties, Scenario& out)
{
   Loader l{parties, out, 0};
   return l.load(doc);
}

bool apply_pokes(battle::State& s, const Scenario& sc)
{
   bool ok = true;
   for (size_t k = 0; k < sc.pokes.size(); ++k)
   {
      const Poke& pk = sc.pokes[k];
      const std::string where = "pokes[" + std::to_string(k) + "]";
      if (pk.party >= s.party_count || pk.member >= s.parties[pk.party].count)
      {
         error("party " + std::to_string(pk.party) + " has no member " + std::to_string(pk.member), where);
         ok = false;
         continue;
      }
      battle::Battler& b = s.battlers[s.parties[pk.party].first_battler + pk.member];
      const auto has = [&pk](PokeField f) { return (pk.fields & (1u << f)) != 0; };
      if (has(POKE_HP))
      {
         b.hp = pk.hp > battle::max_hp(b) ? battle::max_hp(b) : pk.hp;
      }
      if (has(POKE_STATUS))
      {
         b.status = pk.status;
      }
      if (has(POKE_STATUS_COUNTER))
      {
         b.status_counter = pk.status_counter;
      }
      if (has(POKE_STATUS_AUX))
      {
         b.status_aux = pk.status_aux;
      }
      for (int slot = 0; slot < battle::kMaxVolatileCounters; ++slot)
      {
         if ((pk.counter_mask & (1u << slot)) != 0)
         {
            b.vcounter[slot] = pk.counters[slot];
         }
      }
      if (has(POKE_ITEM))
      {
         b.item = pk.item;
      }
      if (has(POKE_VOLATILES))
      {
         b.vol = pk.volatiles;
      }
      for (int st = 0; st < battle::kMaxStages; ++st)
      {
         if ((pk.stage_mask & (uint64_t{1} << st)) != 0)
         {
            b.stages[st] = pk.stages[st];
         }
      }
      // resource pools (D-25), clamped to their maxima; set directly (no events, like the other pokes)
      for (const ResourcePoke& rp : pk.resources)
      {
         const battle::ResourceData& d = battle::kData.resources[rp.resource];
         const int slot = rp.slot < 0 ? 0 : rp.slot;
         const int32_t top = battle::resource_max(b, rp.resource, slot);
         const auto v = static_cast<uint16_t>(rp.value < top ? rp.value : top);
         if (d.empty == static_cast<uint8_t>(battle::ResourceEmpty::Faint))
         {
            b.hp = v;
         }
         else if (d.scope == static_cast<uint8_t>(battle::ResourceScope::Move))
         {
            b.move_pool[d.slot][slot] = static_cast<uint8_t>(v);
         }
         else
         {
            b.pool[d.slot] = v;
         }
      }
   }
   if (sc.weather_turns_given)
   {
      s.weather_turns = sc.weather_turns;
   }
   return ok;
}

} // namespace capsule::bench
