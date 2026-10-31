/// @file
/// The engine's default nursery script port (capsule/script/nursery_port.h, B2): the statements over
/// combine::nursery and the attached party; `nursery pick` is the party menu (an egg shows the eggs' name), `nursery
/// name` the nickname prompt: "Give NAME a nickname?" with yes / no, then a letter menu (up to the layout's nickname
/// codes). C9: the menu's first row is the name so far ("Name: AB", choosing it ends the name), then Del, then the
/// letters: the menu box replaces the text box (one box), so the name lives in the menu, and after each pick the
/// menu opens on its first page again with the name in view. A game replaces this file's functions by defining
/// them.
#include "capsule/script/nursery_port.h"

#include "capsule/battle/data.h"
#include "capsule/combine/eggs.h"
#include "capsule/combine/nursery.h"
#include "capsule/core/memory.h"
#include "capsule/script/menu.h"
#include "capsule/script/port.h"

namespace capsule::script::nursery
{

namespace
{

namespace cn = capsule::combine::nursery;

/// Codes of the text box line (the question, the name so far).
constexpr int kLineCodes = 64;
/// The letters of the nickname menu (the engine port's ASCII page).
constexpr auto kLetters = text::encode("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
/// How many letters.
constexpr int kLetterCount = static_cast<int>(sizeof(kLetters.codes)) - 1;
/// The letter menu's entry that shows the name so far and ends it (C9: the first row, always in view).
constexpr int kEndEntry = 0;
/// The letter menu's entry that drops the last letter.
constexpr int kDelEntry = 1;
/// The letter menu's first letter entry.
constexpr int kFirstLetter = 2;
/// Entries of the letter menu.
constexpr int kLetterEntries = kFirstLetter + kLetterCount;
/// The question's start.
constexpr auto kGive = text::encode("Give ");
/// The question's end.
constexpr auto kNickQuestion = text::encode(" a nickname?");
/// The name line's start.
constexpr auto kNameIs = text::encode("Name: ");
/// The drop-a-letter option.
constexpr auto kDelText = text::encode("Del");

/// Where the running statement stands.
enum class Step : uint8_t
{
   Idle,      ///< Nothing runs.
   Pick,      ///< The party menu is open.
   Ask,       ///< The nickname question is typing.
   Answer,    ///< Its yes / no is open.
   Letters,   ///< The letter menu is open.
   Finished,  ///< Over: the next poll answers the result.
};

/// The defaults' state (zero at start-up; EWRAM on the GBA: not speed-critical).
struct Port
{
   monster::Party* party;                       ///< attach()'s party.
   menu::Paged menu;                            ///< The open menu.
   text::Code line[kLineCodes];                 ///< The text box line.
   text::Code name[monster::kMaxNicknameCodes]; ///< The name so far.
   uint8_t name_codes;                          ///< Its codes.
   uint8_t member;                              ///< Name: the member.
   Step step;                                   ///< Where it stands.
   int16_t result;                              ///< The result once Finished.
};

/// The state.
CAPSULE_EWRAM_BSS Port g;

/// A record's name on screen: the eggs' name for an egg, else its species'.
/// @param rec  the record
/// @return the name, or nullptr
const text::Code* shown_name(const monster::Record& rec)
{
   if (combine::is_egg(rec))
   {
      return combine::egg_name();
   }
   const uint16_t sp = battle::find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
   return sp < battle::kData.species_count ? battle::kData.species[sp].name : nullptr;
}

/// Copies text into a menu option.
/// @param from  the text, or nullptr
/// @param out   menu::kOptionCodes codes
void option(const text::Code* from, text::Code* out)
{
   int k = 0;
   for (; from != nullptr && from[k] != text::kCodeEnd && k < menu::kOptionCodes - 1; ++k)
   {
      out[k] = from[k];
   }
   out[k] = text::kCodeEnd;
}

/// Appends text to the line (up to its end).
/// @param at    where the line ends now (moved)
/// @param from  text ending with kCodeEnd, or nullptr
/// @param most  most codes to take
void append(int& at, const text::Code* from, int most = kLineCodes)
{
   for (int k = 0; from != nullptr && k < most && from[k] != text::kCodeEnd && at < kLineCodes - 1; ++k)
   {
      g.line[at++] = from[k];
   }
   g.line[at] = text::kCodeEnd;
}

/// The party menu's entry texts.
/// @param index  entry (a party slot)
/// @param out    its option text
void fill_party(int index, text::Code* out)
{
   option(shown_name(g.party->members[index]), out);
}

/// The letter menu's entry texts.
/// @param index  entry
/// @param out    its option text
void fill_letters(int index, text::Code* out)
{
   if (index == kEndEntry)
   {
      // "Name: " and the name so far (within the option's codes)
      int at = 0;
      for (int k = 0; kNameIs.codes[k] != text::kCodeEnd && at < menu::kOptionCodes - 1; ++k)
      {
         out[at++] = kNameIs.codes[k];
      }
      for (int k = 0; k < g.name_codes && at < menu::kOptionCodes - 1; ++k)
      {
         out[at++] = g.name[k];
      }
      out[at] = text::kCodeEnd;
      return;
   }
   if (index == kDelEntry)
   {
      option(kDelText.codes, out);
      return;
   }
   out[0] = kLetters.codes[index - kFirstLetter];
   out[1] = text::kCodeEnd;
}

/// Most codes the nickname takes (the layout's Nickname role).
/// @return codes
int name_room()
{
   const int n = monster::count(monster::Role::Nickname);
   return n < monster::kMaxNicknameCodes ? n : monster::kMaxNicknameCodes;
}

/// Ends the statement.
/// @param result  its result
void finish(int16_t result)
{
   port::close_message();
   g.result = result;
   g.step = Step::Finished;
}

/// Opens the letter menu on its first page (its first row: the name so far).
void open_letters()
{
   menu::open(g.menu, kLetterEntries, fill_letters);
   g.step = Step::Letters;
}

/// Writes the name into the member's Nickname role (the codes, then ends).
void store_name()
{
   monster::Record& rec = g.party->members[g.member];
   for (int k = 0; k < name_room(); ++k)
   {
      monster::set(rec, monster::Role::Nickname, k, k < g.name_codes ? g.name[k] : text::kCodeEnd);
   }
}

} // namespace

void attach(monster::Party* party)
{
   g.party = party;
}

void pick_open()
{
   g.step = Step::Finished;
   g.result = 0;
   if (g.party == nullptr || g.party->count == 0)
   {
      return;
   }
   menu::open(g.menu, g.party->count, fill_party);
   g.step = Step::Pick;
}

void name_open(uint8_t member)
{
   g.step = Step::Finished;
   g.result = 0;
   if (g.party == nullptr || member >= g.party->count || name_room() == 0)
   {
      return;
   }
   g.member = member;
   g.name_codes = 0;
   int at = 0;
   g.line[0] = text::kCodeEnd;
   append(at, kGive.codes);
   append(at, shown_name(g.party->members[member]));
   append(at, kNickQuestion.codes);
   port::message(g.line);
   g.step = Step::Ask;
}

int16_t poll()
{
   switch (g.step)
   {
   case Step::Pick:
   {
      const int32_t a = menu::poll(g.menu);
      if (a == menu::kWaiting)
      {
         return kNurseryRunning;
      }
      finish(a == menu::kBack ? int16_t{0} : static_cast<int16_t>(a + 1));
      break;
   }
   case Step::Ask:
      if (port::message_busy())
      {
         return kNurseryRunning;
      }
      port::yesno_open();
      g.step = Step::Answer;
      return kNurseryRunning;
   case Step::Answer:
   {
      const int8_t a = port::yesno_poll();
      if (a < 0)
      {
         return kNurseryRunning;
      }
      if (a == 0)
      {
         finish(0);   // no: the species' name stays
         break;
      }
      open_letters();
      return kNurseryRunning;
   }
   case Step::Letters:
   {
      const int32_t a = menu::poll(g.menu);
      if (a == menu::kWaiting)
      {
         return kNurseryRunning;
      }
      if (a == menu::kBack || a == kEndEntry)
      {
         // an empty name keeps the species' name
         if (g.name_codes > 0)
         {
            store_name();
         }
         finish(g.name_codes > 0 ? int16_t{1} : int16_t{0});
         break;
      }
      if (a == kDelEntry)
      {
         g.name_codes = static_cast<uint8_t>(g.name_codes > 0 ? g.name_codes - 1 : 0);
      }
      else if (g.name_codes < name_room())
      {
         g.name[g.name_codes++] = kLetters.codes[a - kFirstLetter];
      }
      open_letters();
      return kNurseryRunning;
   }
   default:
      break;
   }
   g.step = Step::Idle;
   return g.result;
}

int32_t query(NurseryOp kind, int32_t arg)
{
   const uint8_t at = static_cast<uint8_t>(arg >= 0 && arg <= combine::kNone ? arg : combine::kNone);
   switch (kind)
   {
   case NurseryOp::Deposit:
      return g.party != nullptr && cn::deposit(*g.party, at) == cn::Result::Ok ? 1 : 0;
   case NurseryOp::Count:
      return cn::count();
   case NurseryOp::Levels:
      return cn::levels_gained(at);
   case NurseryOp::Fee:
      return cn::fee(at);
   case NurseryOp::Ready:
      return cn::result_ready() ? 1 : 0;
   case NurseryOp::Odds:
      return cn::odds();
   case NurseryOp::Take:
      return g.party != nullptr && cn::take_result(*g.party) == cn::Result::Ok ? 1 : 0;
   default:
      return 0;
   }
}

int32_t withdraw(int32_t slot, int32_t money, int32_t& paid)
{
   paid = 0;
   if (g.party == nullptr || slot < 0 || slot >= cn::slot_count())
   {
      return 0;
   }
   const auto at = static_cast<uint8_t>(slot);
   const int32_t cost = money >= 0 ? cn::fee(at) : 0;
   if (cn::record(at) != nullptr && money >= 0 && money < cost)
   {
      return kNurseryPoor;
   }
   if (cn::withdraw(*g.party, at) != cn::Result::Ok)
   {
      return 0;
   }
   paid = cost;
   return 1;
}

} // namespace capsule::script::nursery
