/// @file
/// The battle transcript format of the WP-2 goldens (tests/data/battle/golden/transcripts), shared by the golden
/// program (tests/battle_golden_tests.cpp) and the battle bench (apps/battle_bench), so that a bench scenario
/// reproduces a golden transcript byte for byte.
///
/// A transcript holds one line per formatted battle text ("> ..."), per Hp, Faint, SwitchIn, Level, Turn and End
/// event, per refused choice, and a summary at the end (outcome, turns, reward, the battle stream's final state,
/// every battler and every party record). Nothing in it depends on enum numbering, message indices, hook ids, record
/// layouts or event encodings.
///
/// Battle data with battler resources besides the hit points (D-25: MP, EP; the Starter sample's goldens, E9) adds a
/// line per Resource event and those pools (by label) to the summary's battler and record lines; data without them
/// (the WP-2 goldens) writes exactly what it wrote before.
///
/// Owner texts and condition changes (E13b) are opt-in: drain() with kOwnerTexts also writes each Text event (effect
/// `text`) as a "> " line, each Volatile, Side and Field event as a line of its own. Without it (every golden recorded
/// before E13b, and a bench scenario without "owner_texts") the output is what it was.
///
/// The Item action and recruitment (G3) add a line per UseItem and Recruit event, the outcome Recruited, item choices
/// and, when the storage boxes hold records, a "boxed" summary line each; a battle without them writes what it wrote
/// before.
///
/// Growth (G4) adds a "form" line per form change in battle (effect `form`); a battle without one writes what it
/// wrote before.
///
/// Header only: it reads the program's battle data (capsule::battle::kData), which every program links itself.
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/growth/growth.h"
#include "capsule/monster/monster.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace capsule::battle::transcript
{

/// Longest text one put() call appends.
inline constexpr int kPutCap = 512;
/// Codes a formatted battle text may take.
inline constexpr int kTextCap = 160;

/// @name Record seeds of the goldens' creatures
///@{
inline constexpr uint32_t kSeedPerParty = 7919;     ///< Seed step per party index.
inline constexpr uint32_t kSeedPerMember = 977;     ///< Seed step per member index.
inline constexpr uint32_t kSeedPerSpecies = 31;     ///< Seed step per species index.
///@}

/// Appends printf-formatted text.
/// @param out  text
/// @param f    format
inline void put(std::string& out, const char* f, ...)
{
   char buf[kPutCap];
   va_list args;
   va_start(args, f);
   std::vsnprintf(buf, sizeof(buf), f, args);
   va_end(args);
   out += buf;
}

/// Appends encoded text readably: printable ASCII as itself, a newline as
/// "\n", anything else as {XX}.
/// @param out    text
/// @param codes  encoded text (kCodeEnd-terminated), or nullptr
inline void put_codes(std::string& out, const text::Code* codes)
{
   if (codes == nullptr)
   {
      out += "-";
      return;
   }
   for (int k = 0; codes[k] != text::kCodeEnd; ++k)
   {
      const text::Code c = codes[k];
      if (c == text::kCodeNewline)
      {
         out += "\\n";
      }
      else if (c >= text::kFirstPrintable && c <= text::kLastPrintable)
      {
         out += static_cast<char>(c);
      }
      else
      {
         put(out, "{%02X}", c);
      }
   }
}

/// Encoded text as a string (see put_codes()).
/// @param codes  encoded text, or nullptr
/// @return readable text
inline std::string str(const text::Code* codes)
{
   std::string s;
   put_codes(s, codes);
   return s;
}

/// A move's name.
/// @param m  move index
/// @return name
inline std::string move_name(uint16_t m)
{
   return m < kData.move_count ? str(kData.moves[m].name) : std::string("-");
}

/// A status name ("none" for 0).
/// @param st  status index
/// @return name
inline std::string status_name(uint8_t st)
{
   return st != 0 && st < kData.status_count ? str(kData.statuses[st].name) : std::string("none");
}

/// A held item's name ("none" for 0).
/// @param it  item index
/// @return name
inline std::string item_name(uint16_t it)
{
   return it != 0 && it < kData.item_count ? str(kData.items[it].name) : std::string("none");
}

/// A passive's (ability's) name.
/// @param a  index
/// @return name
inline std::string passive_name(uint8_t a)
{
   return a < kData.passive_count ? str(kData.passives[a].name) : std::string("none");
}

/// A weather's name ("none" for kNone8).
/// @param w  index
/// @return name
inline std::string weather_name(uint8_t w)
{
   return w < kData.weather_count ? str(kData.weathers[w].name) : std::string("none");
}

/// A species' name.
/// @param sp  species index
/// @return name
inline std::string species_name(uint16_t sp)
{
   return str(kData.species[sp].name);
}

/// An outcome's name (by enumerator, not by value).
/// @param o  outcome
/// @return name
inline const char* outcome_name(Outcome o)
{
   switch (o)
   {
   case Outcome::Victory:
      return "Victory";
   case Outcome::Defeat:
      return "Defeat";
   case Outcome::Stalemate:
      return "Stalemate";
   case Outcome::Escaped:
      return "Escaped";
   case Outcome::Recruited:
      return "Recruited";
   default:
      return "None";
   }
}

/// The record seed of a scripted creature: member @p member of party @p party (deterministic, so a transcript
/// replays from its scenario).
/// @param party    party index (the participant's)
/// @param member   member index in the party
/// @param species  species index
/// @param level    level
/// @return the seed of the creature's record stream
inline uint32_t member_seed(int party, int member, uint16_t species, uint8_t level)
{
   return static_cast<uint32_t>(party) * kSeedPerParty + static_cast<uint32_t>(member) * kSeedPerMember +
          static_cast<uint32_t>(species) * kSeedPerSpecies + level;
}

/// Sets a record's moves with full move pools (PP; slots past @p count are emptied).
/// @param rec    record
/// @param moves  move indices (kNone16 = empty)
/// @param count  entries in @p moves
inline void set_moves(monster::Record& rec, const uint16_t* moves, int count)
{
   for (int i = 0; i < move_slots(); ++i)
   {
      const uint16_t m = i < count ? moves[i] : kNone16;
      monster::set(rec, monster::Role::Moves, i, m);
      fill_move_slot(rec, i);
   }
}

/// The role of the first persisted move resource (the "move/value" of a summary's record line, e.g. PP).
/// @return its role, or monster::kNoRole
inline monster::RoleId shown_move_role()
{
   for (uint8_t r = 0; r < kData.resource_count; ++r)
   {
      const ResourceData& d = kData.resources[r];
      if (d.scope == static_cast<uint8_t>(ResourceScope::Move) && d.role != monster::kNoRole)
      {
         return d.role;
      }
   }
   return monster::kNoRole;
}

/// Appends a resource's label (its in-game text).
/// @param out  text
/// @param r    resource index
inline void put_resource_label(std::string& out, uint8_t r)
{
   put_codes(out, kData.resources[r].label);
}

/// Whether a resource is a battler pool besides the hit points (those the extra transcript lines show).
/// @param r  resource index
/// @return true for such a pool
inline bool shown_pool(uint8_t r)
{
   const ResourceData& d = kData.resources[r];
   return d.scope == static_cast<uint8_t>(ResourceScope::Battler) && r != kData.faint_resource;
}

/// Appends a battler's pools besides the hit points: " LABEL value/max" each (nothing when the data has none).
/// @param out  text
/// @param b    the battler
inline void put_pools(std::string& out, const Battler& b)
{
   for (uint8_t r = 0; r < kData.resource_count; ++r)
   {
      if (shown_pool(r))
      {
         out += " ";
         put_resource_label(out, r);
         put(out, " %d/%d", static_cast<int>(resource_value(b, r)), static_cast<int>(resource_max(b, r)));
      }
   }
}

/// Appends a record's persisted pools besides the hit points: " LABEL value" each.
/// @param out  text
/// @param rec  the record
inline void put_record_pools(std::string& out, const monster::Record& rec)
{
   for (uint8_t r = 0; r < kData.resource_count; ++r)
   {
      if (shown_pool(r) && kData.resources[r].role != monster::kNoRole)
      {
         out += " ";
         put_resource_label(out, r);
         put(out, " %u", static_cast<unsigned>(monster::get(rec, kData.resources[r].role)));
      }
   }
}

/// Appends a battler as "#i Name".
/// @param out  text
/// @param s    battle
/// @param i    battler
inline void put_battler(std::string& out, const State& s, uint8_t i)
{
   if (i >= s.battler_count)
   {
      out += "-";
      return;
   }
   put(out, "#%d ", i);
   put_codes(out, s.battlers[i].name);
}

/// Appends an event's formatted text as a "> " line ("> (no text)" when it has none).
/// @param out  transcript
/// @param s    battle
/// @param e    a text event
inline void put_text(std::string& out, const State& s, const Event& e)
{
   text::Code buf[kTextCap];
   if (format_text(s, e, buf, kTextCap))
   {
      out += "> ";
      put_codes(out, buf);
      out += "\n";
   }
   else
   {
      out += "> (no text)\n";
   }
}

/// A recruit moment's name (RecruitMoment, G3).
/// @param m  the moment
/// @return its name
inline const char* recruit_moment_name(uint8_t m)
{
   static const char* const kNames[] = {"check", "recruited", "broke_free", "refused", "blocked", "no_room",
                                        "to_party", "to_storage"};
   static_assert(sizeof(kNames) / sizeof(kNames[0]) == kRecruitMoments);
   return m < kRecruitMoments ? kNames[m] : "?";
}

/// Sees every event drain() takes from the queue, before it is written (the bench's event trace).
using EventObserver = void (*)(const State& s, const Event& e, void* user);

/// @name drain() extras (E13b; bit flags)
///@{
inline constexpr uint8_t kPlainTranscript = 0;   ///< The transcript as the goldens have it.
inline constexpr uint8_t kOwnerTexts = 1;        ///< Also owner texts and volatile / side / field condition changes.
///@}

/// Appends a condition's display name ("?" without one).
/// @param out   text
/// @param name  its name, or nullptr
inline void put_name(std::string& out, const text::Code* name)
{
   if (name == nullptr)
   {
      out += "?";
      return;
   }
   put_codes(out, name);
}

/// Writes one of the opt-in events (kOwnerTexts): an owner text, or a volatile / side / field condition change.
/// @param out  transcript
/// @param s    battle
/// @param e    the event
inline void put_owner_event(std::string& out, const State& s, const Event& e)
{
   switch (e.kind)
   {
   case EventKind::Text:
      put_text(out, s, e);
      break;
   case EventKind::Volatile:
      // battler's volatile a went on (extra 1) or off
      out += "volatile ";
      put_battler(out, s, e.battler);
      out += " ";
      put_name(out, e.a < kData.volatile_count ? kData.volatiles[e.a].name : nullptr);
      out += e.extra != 0 ? " on\n" : " off\n";
      break;
   case EventKind::Side:
      // side condition a of side target: extra = turns, b = layers (0: it ended)
      put(out, "side %d ", e.target);
      put_name(out, e.a < kData.side_cond_count ? kData.side_conds[e.a].name : nullptr);
      put(out, " turns %d layers %d\n", e.extra, e.b);
      break;
   case EventKind::Field:
      // field condition a: extra = turns, b = 1 while on
      out += "field ";
      put_name(out, e.a < kData.field_cond_count ? kData.field_conds[e.a].name : nullptr);
      put(out, " turns %d %s\n", e.extra, e.b != 0 ? "on" : "off");
      break;
   default:
      break;
   }
}

/// Drains the event queue into the transcript.
/// @param s        battle
/// @param out      transcript
/// @param observe  called for every event first, or nullptr
/// @param user     passed to @p observe
/// @param extras   kOwnerTexts: also owner texts and condition changes (E13b); kPlainTranscript: as the goldens
inline void drain(State& s, std::string& out, EventObserver observe = nullptr, void* user = nullptr,
                  uint8_t extras = kPlainTranscript)
{
   Event e;
   while (pop_event(s, e))
   {
      if (observe != nullptr)
      {
         observe(s, e, user);
      }
      switch (e.kind)
      {
      case EventKind::Message:
      case EventKind::StatusText:
      case EventKind::WeatherText:
      case EventKind::PresenterText:
         put_text(out, s, e);
         break;
      case EventKind::Hp:
         out += "hp ";
         put_battler(out, s, e.battler);
         put(out, " %d (was %d)\n", e.a, e.b);
         break;
      case EventKind::Faint:
         out += "faint ";
         put_battler(out, s, e.battler);
         out += "\n";
         break;
      case EventKind::SwitchIn:
         out += "switch_in ";
         put_battler(out, s, e.battler);
         put(out, " at %d\n", e.target);
         break;
      case EventKind::Level:
         out += "level ";
         put_battler(out, s, e.battler);
         put(out, " %d\n", e.a);
         break;
      case EventKind::Turn:
         put(out, "turn %d\n", e.a);
         break;
      case EventKind::Resource:
         // a battler pool besides the hit points changed (costs, regen, effect logic)
         out += "resource ";
         put_battler(out, s, e.battler);
         out += " ";
         put_resource_label(out, static_cast<uint8_t>(e.extra));
         put(out, " %d (was %d)\n", e.a, e.b);
         break;
      case EventKind::End:
         put(out, "end %s\n", outcome_name(static_cast<Outcome>(e.a)));
         break;
      case EventKind::UseItem:
         // G3: battler uses item a on target
         out += "use_item ";
         put_battler(out, s, e.battler);
         put(out, " %s on ", item_name(e.a).c_str());
         put_battler(out, s, e.target);
         out += "\n";
         break;
      case EventKind::Recruit:
         // G3: a recruit moment of battler (extra = RecruitMoment, a = its value)
         out += "recruit ";
         put_battler(out, s, e.battler);
         put(out, " %s %d\n", recruit_moment_name(e.extra), e.a);
         break;
      case EventKind::Transform:
         // G4: a form change in battle (a = the form now, b = before); the other extras are a field context's
         if (e.extra == growth::kTransformForm)
         {
            out += "form ";
            put_battler(out, s, e.battler);
            put(out, " %d (was %d)\n", e.a, e.b);
         }
         else
         {
            out += "transform ";
            put_battler(out, s, e.battler);
            put(out, " %d: %d %d\n", e.extra, e.a, e.b);
         }
         break;
      case EventKind::Text:
      case EventKind::Volatile:
      case EventKind::Side:
      case EventKind::Field:
         if ((extras & kOwnerTexts) != 0)
         {
            put_owner_event(out, s, e);
         }
         break;
      default:
         break;
      }
   }
}

/// @name Choices of human battlers
///@{

/// What a scripted human battler does on its turn.
enum class ChoiceKind : uint8_t
{
   First,    ///< The first usable move slot (else the fallback move).
   Move,     ///< Choice::slot (on Choice::target).
   Run,      ///< Run away.
   Switch,   ///< Switch to member Choice::member of its party.
   Item,     ///< Use Choice::item (G3) on Choice::target (a battler target's position), member Choice::member
             ///< (a member target) and its move slot Choice::slot (a move slot target).
   NUM_KINDS ///< Number of kinds.
};

/// One scripted choice.
struct Choice
{
   ChoiceKind kind;   ///< What.
   uint8_t slot;      ///< Move slot (Move).
   uint8_t target;    ///< Target position (Move; kBench = the default).
   uint8_t member;    ///< Party member to send in (Switch), or an item's member (Item).
   uint16_t item = 0;   ///< Item (Item; G3).
};

/// Chooses the first usable move, else the struggle move.
/// @param s        battle
/// @param battler  battler
inline void choose_first(State& s, uint8_t battler)
{
   for (uint8_t k = 0; k < move_slots(); ++k)
   {
      if (choose(s, battler, Action{ActionKind::Move, k, kBench, 0}))
      {
         return;
      }
   }
   choose(s, battler, Action{ActionKind::Move, kFallbackSlot, kBench, 0});
}

/// Answers a Replace request with the first battler the battle accepts ("no replacement" when none).
/// @param s    battle
/// @param r    the request
/// @param out  transcript
inline void replace(State& s, const Request& r, std::string& out)
{
   for (uint8_t i = 0; i < s.battler_count; ++i)
   {
      if (choose(s, r.battler, Action{ActionKind::Switch, 0, kBench, i}))
      {
         return;
      }
   }
   out += "no replacement\n";
}

/// Makes a scripted choice; a refused one goes into the transcript (a refused run with the text the sandbox shows)
/// and falls back to the first usable move. C5: a battler that try_switch hooks trap (is_trapped()) is refused
/// "(trapped)": the trapping handler's own text is the battle's event (choose() says it once a turn), not the
/// sandbox's no-escape text.
/// @param s    battle
/// @param r    the request
/// @param c    the choice
/// @param out  transcript
inline void act(State& s, const Request& r, const Choice& c, std::string& out)
{
   const bool trapped = (c.kind == ChoiceKind::Run || c.kind == ChoiceKind::Switch) && is_trapped(s, r.battler);
   if (c.kind == ChoiceKind::Run)
   {
      if (choose(s, r.battler, Action{ActionKind::Run, 0, kBench, 0}))
      {
         return;
      }
      out += trapped ? "refused run (trapped) " : "refused run ";
      put_battler(out, s, r.battler);
      out += "\n";
      const Event e{EventKind::Message, r.battler, kBench, 0, static_cast<uint16_t>(Msg::EscapeBlocked), 0};
      text::Code buf[kTextCap];
      if (!trapped && format_text(s, e, buf, kTextCap))
      {
         out += "> ";
         put_codes(out, buf);
         out += "\n";
      }
   }
   else if (c.kind == ChoiceKind::Switch)
   {
      const int t = s.battlers[r.battler].party;
      const uint8_t to = static_cast<uint8_t>(s.parties[t].first_battler + c.member);
      if (choose(s, r.battler, Action{ActionKind::Switch, 0, kBench, to}))
      {
         return;
      }
      out += trapped ? "refused switch (trapped) " : "refused switch ";
      put_battler(out, s, r.battler);
      out += "\n";
   }
   else if (c.kind == ChoiceKind::Item)
   {
      // G3: the item on its target (the member is an index into the battler's party)
      const int t = s.battlers[r.battler].party;
      Action a{ActionKind::Item, c.slot, c.target, static_cast<uint8_t>(s.parties[t].first_battler + c.member)};
      a.item = c.item;
      if (choose(s, r.battler, a))
      {
         return;
      }
      put(out, "refused item %s ", item_name(c.item).c_str());
      put_battler(out, s, r.battler);
      out += "\n";
   }
   else if (c.kind == ChoiceKind::Move)
   {
      if (choose(s, r.battler, Action{ActionKind::Move, c.slot, c.target, 0}))
      {
         return;
      }
      put(out, "refused move slot %d ", c.slot);
      put_battler(out, s, r.battler);
      out += "\n";
   }
   choose_first(s, r.battler);
}
///@}

/// Appends the end-of-battle summary.
/// @param s    battle
/// @param out  transcript
inline void summary(const State& s, std::string& out)
{
   put(out, "-- outcome %s, turns %d, reward %u, rng %08X\n", outcome_name(s.outcome), s.turn,
       static_cast<unsigned>(s.reward), static_cast<unsigned>(s.rng.state));
   for (uint8_t i = 0; i < s.battler_count; ++i)
   {
      const Battler& b = s.battlers[i];
      out += "battler ";
      put_battler(out, s, i);
      put(out, " lv %d hp %d/%d status %s item %s exp %u", b.level, b.hp, max_hp(b),
          status_name(b.status).c_str(), item_name(b.item).c_str(), static_cast<unsigned>(b.exp));
      put_pools(out, b);
      out += "\n";
   }
   for (int t = 0; t < s.party_count; ++t)
   {
      const monster::Party* p = s.parties[t].records;
      for (int k = 0; p != nullptr && k < p->count; ++k)
      {
         const monster::Record& rec = p->members[k];
         const uint16_t sp = find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
         // the PrimaryStatus role: the status id in the layout's status_bits low bits, its counter above
         const uint32_t st = monster::get(rec, monster::Role::PrimaryStatus);
         const uint32_t status_mask = (1u << kData.status_bits) - 1u;
         put(out, "record %d.%d %s lv %u exp %u hp %u status %s/%u item %s moves", t, k,
             sp != kNone16 ? str(kData.species[sp].name).c_str() : "?",
             static_cast<unsigned>(monster::get(rec, monster::Role::Level)),
             static_cast<unsigned>(monster::get(rec, monster::Role::Experience)),
             static_cast<unsigned>(monster::get(rec, monster::Role::CurrentHp)),
             status_name(static_cast<uint8_t>(st & status_mask)).c_str(),
             static_cast<unsigned>(st >> kData.status_bits),
             item_name(static_cast<uint16_t>(monster::get(rec, monster::Role::HeldItem))).c_str());
         const monster::RoleId shown = shown_move_role();
         for (int m = 0; m < move_slots(); ++m)
         {
            const uint32_t mv = monster::get(rec, monster::Role::Moves, m);
            if (mv >= kData.move_count)
            {
               continue;
            }
            put(out, " %s", move_name(static_cast<uint16_t>(mv)).c_str());
            if (shown != monster::kNoRole)
            {
               // the first persisted move resource's value (PP); data without one shows the moves only
               put(out, "/%u", static_cast<unsigned>(monster::get(rec, shown, m)));
            }
         }
         put_record_pools(out, rec);
         out += "\n";
      }
   }
   // G3: the records the storage boxes hold (none: no line)
   for (int k = 0; s.storage != nullptr && k < storage::slot_count(); ++k)
   {
      monster::Record rec{};
      if (!storage::read(*s.storage, k, rec))
      {
         continue;
      }
      const uint16_t sp = find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
      put(out, "boxed %d %s lv %u hp %u\n", k, sp != kNone16 ? str(kData.species[sp].name).c_str() : "?",
          static_cast<unsigned>(monster::get(rec, monster::Role::Level)),
          static_cast<unsigned>(monster::get(rec, monster::Role::CurrentHp)));
   }
}

} // namespace capsule::battle::transcript
