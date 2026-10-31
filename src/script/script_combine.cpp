/// @file
/// The engine's default combine script port (capsule/script/combine_port.h, B1): `combine open` picks the inputs
/// from the party through the text box's paged menu (script::menu; "Combine" ends the picks once the method has
/// enough), asks "A + B -> C?" with yes / no from the preview, then (yes) builds the combine::Combination over the
/// attached State, lets the player pick the moves when the method's chooser is the player's (the candidates and
/// "Done"), places the result and shows the State's queued texts (the logic's, the method's messages) one by one.
/// `combine run` skips the input menu and the question. A game replaces this file's functions by defining them.
/// C9: the draws come only after yes (no reroll by declining); eggs show the eggs' name in the menu and question.
#include "capsule/script/combine_port.h"

#include "capsule/battle/data.h"
#include "capsule/combine/combine.h"
#include "capsule/combine/eggs.h"
#include "capsule/core/memory.h"
#include "capsule/script/menu.h"
#include "capsule/script/port.h"

#include <new>

namespace capsule::script::combine
{

namespace
{

namespace cb = capsule::combine;

/// Codes of the question line ("A + B + C -> D?") and of one shown text.
constexpr int kLineCodes = 128;
/// Text cells of one message box row: the default HAL port's box is 30 cells wide less its two frame cells
/// (script_port_hal.cpp kBoxW); the message port prints a row as given, so the shown lines are broken to it here.
constexpr int kBoxTextColumns = 28;
/// The menu entry that ends the picks.
constexpr uint8_t kCombineEntry = 0xFE;
/// The menu entry that ends the move picks.
constexpr uint8_t kDoneEntry = 0xFD;
/// Most entries of a menu (the party and Combine, the move candidates and Done).
constexpr int kMaxEntries = cb::kMaxCandidates + 1;
/// The joiners of the question and the menus' own options (the engine port's ASCII page, like menu::kMoreText).
constexpr auto kPlus = text::encode(" + ");
/// " -> ".
constexpr auto kArrow = text::encode(" -> ");
/// The question mark.
constexpr auto kAsk = text::encode("?");
/// The option that ends the picks.
constexpr auto kCombineText = text::encode("Combine");
/// The option that ends the move picks.
constexpr auto kDoneText = text::encode("Done");
/// The question's result while a random pick is not drawn yet (C9).
constexpr auto kUnknownText = text::encode("?");

/// Where the running statement stands.
enum class Step : uint8_t
{
   Idle,       ///< Nothing runs.
   Pick,       ///< The input menu is open.
   Ask,        ///< The question is typing.
   Answer,     ///< The yes / no is open.
   Moves,      ///< The move menu is open.
   Texts,      ///< The queued texts are shown.
   Finished,   ///< Over: the next poll answers the result.
};

/// The defaults' state: what attach() gave, the running Combination (built in place: no heap), the menus.
struct Port
{
   battle::State* lent;                                              ///< attach()'s State.
   monster::Party* party;                                            ///< The party.
   storage::Storage* boxes;                                          ///< The boxes.
   rng::Stream* rng;                                                 ///< The stream.
   alignas(cb::Combination) unsigned char combo[sizeof(cb::Combination)];   ///< The running Combination.
   menu::Paged menu;                                                 ///< The open menu.
   text::Code line[kLineCodes];                                      ///< The question / the text shown.
   uint8_t entries[kMaxEntries];                                     ///< Menu entry -> party slot / candidate.
   uint8_t picked[cb::kMaxInputs];                                   ///< The inputs picked (party slots).
   uint8_t entry_count;                                              ///< Menu entries.
   uint8_t count;                                                    ///< Inputs picked.
   uint8_t method;                                                   ///< The method.
   Step step;                                                        ///< Where it stands.
   bool live;                                                        ///< A Combination lives in combo.
   bool ask;                                                         ///< Open: ask before combining.
   bool menus_off;                                                   ///< set_menus(false).
   bool shown;                                                       ///< Texts: a text is in the box.
   int16_t result;                                                   ///< The result once Finished.
   int16_t after_texts;                                              ///< Texts: the result after them.
};

/// The state (zero at start-up; EWRAM on the GBA: not speed-critical).
CAPSULE_EWRAM_BSS Port g;

/// The running Combination.
/// @return it (g.live must be true)
cb::Combination& running()
{
   return *std::launder(reinterpret_cast<cb::Combination*>(g.combo));
}

/// Ends the running Combination (nothing is placed unless place() ran).
void drop()
{
   if (g.live)
   {
      running().~Combination();
      g.live = false;
   }
}

/// Ends the statement.
/// @param result  its result
void finish(int16_t result)
{
   drop();
   port::close_message();
   g.result = result;
   g.step = Step::Finished;
}

/// Appends codes to the line (up to its end).
/// @param at     where the line ends now (moved)
/// @param codes  text ending with kCodeEnd, or nullptr
void append(int& at, const text::Code* codes)
{
   for (int k = 0; codes != nullptr && codes[k] != text::kCodeEnd && at < kLineCodes - 1; ++k)
   {
      g.line[at++] = codes[k];
   }
   g.line[at] = text::kCodeEnd;
}

/// Breaks the line at its spaces so that no row is wider than the message box (a word wider than a row stays whole;
/// escapes count as one cell each, which only breaks earlier).
void wrap_line()
{
   int column = 0;        // cells on the current row
   int last_space = -1;   // the current row's last space, or -1
   for (int k = 0; k < kLineCodes && g.line[k] != text::kCodeEnd; ++k)
   {
      const text::Code c = g.line[k];
      if (c == text::kCodeNewline || c == text::kCodePageBreak)
      {
         column = 0;
         last_space = -1;
         continue;
      }
      if (c == text::kCodeSpace)
      {
         last_space = k;
      }
      ++column;
      if (column > kBoxTextColumns && last_space >= 0)
      {
         // the row ends at its last space: what follows the space starts the next row
         g.line[last_space] = text::kCodeNewline;
         column = k - last_space;
         last_space = -1;
      }
   }
}

/// A record's name on screen: its species' name (C9: an egg's is the eggs' name, never its species).
/// @param rec  the record
/// @return its name, or nullptr
const text::Code* species_name(const monster::Record& rec)
{
   if (cb::is_egg(rec))
   {
      return cb::egg_name();
   }
   const uint16_t sp = battle::find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});
   return sp < battle::kData.species_count ? battle::kData.species[sp].name : nullptr;
}

/// What the question names as the result (C9): the eggs' name for a method delivering eggs, "?" for a random pick
/// (its draw comes after yes), else the species the preview gives.
/// @param p  the preview of the picked inputs
/// @return the name, or nullptr
const text::Code* result_name(const cb::Preview& p)
{
   const cb::MethodData* m = cb::method_at(g.method);
   if (m != nullptr && m->delivery == cb::Delivery::Egg && cb::egg_name() != nullptr)
   {
      return cb::egg_name();
   }
   if (p.species >= battle::kData.species_count)
   {
      return kUnknownText.codes;
   }
   return battle::kData.species[p.species].name;
}

/// Copies a name into a menu option.
/// @param name  the name, or nullptr
/// @param out   menu::kOptionCodes codes
void option(const text::Code* name, text::Code* out)
{
   int k = 0;
   for (; name != nullptr && name[k] != text::kCodeEnd && k < menu::kOptionCodes - 1; ++k)
   {
      out[k] = name[k];
   }
   out[k] = text::kCodeEnd;
}

/// The input menu's entry texts.
/// @param index  entry
/// @param out    its option text
void fill_pick(int index, text::Code* out)
{
   const uint8_t e = g.entries[index];
   option(e == kCombineEntry ? kCombineText.codes : species_name(g.party->members[e]), out);
}

/// The move menu's entry texts.
/// @param index  entry
/// @param out    its option text
void fill_moves(int index, text::Code* out)
{
   const uint8_t e = g.entries[index];
   const uint16_t mv = e == kDoneEntry ? monster::kNoMove : running().candidate(e);
   option(e == kDoneEntry ? kDoneText.codes : mv < battle::kData.move_count ? battle::kData.moves[mv].name : nullptr,
          out);
}

/// Shows the State's queued texts, then ends with a result.
/// @param result  the result after them
void texts_then(int16_t result)
{
   g.after_texts = result;
   g.shown = false;
   g.step = Step::Texts;
}

/// Places the result; then the texts.
void place_now()
{
   const cb::Status st = running().place();
   texts_then(st == cb::Status::Ok ? kCombineDone : kCombineRefused);
}

/// Opens the move menu: the candidates not chosen yet, then Done.
void open_moves()
{
   cb::Combination& c = running();
   g.entry_count = 0;
   for (uint8_t k = 0; k < c.candidate_count() && g.entry_count < kMaxEntries - 1; ++k)
   {
      if (!c.chosen(k))
      {
         g.entries[g.entry_count++] = k;
      }
   }
   g.entries[g.entry_count++] = kDoneEntry;
   menu::open(g.menu, g.entry_count, fill_moves);
   g.step = Step::Moves;
}

/// After the player said yes (or without a question): the move picks, else the placement.
void go_on()
{
   cb::Combination& c = running();
   if (c.stage() == cb::Stage::Choosing)
   {
      if (g.menus_off)
      {
         c.choose_random();   // the fallback without a menu
      }
      else
      {
         open_moves();
         return;
      }
   }
   place_now();
}

/// Builds the Combination of the picked inputs (its draws happen here): refused (its texts), else on.
void build_combination()
{
   cb::InputRef refs[cb::kMaxInputs] = {};
   for (uint8_t k = 0; k < g.count; ++k)
   {
      refs[k] = cb::InputRef{cb::From::Party, 0, g.picked[k]};
   }
   ::new (static_cast<void*>(g.combo)) cb::Combination(*g.lent, *g.party, g.boxes, g.method, refs, g.count, *g.rng);
   g.live = true;
   if (running().stage() == cb::Stage::Refused)
   {
      texts_then(kCombineRefused);
      return;
   }
   go_on();
}

/// The picked inputs: the question first (open), else the Combination at once.
///
/// C9: the question comes from the preview (no draw): a random pick reads "?", an egg delivery the eggs' name, and
/// the Combination with its draws is built only after yes. Declining leaves the stream as it was, so opening the
/// statement again with the same inputs gives the same draw: the player cannot reroll by saying no. A preview that
/// does not combine builds the Combination at once for its refusal texts (no draw is made before a refusal).
void start_combination()
{
   const monster::Record* recs[cb::kMaxInputs] = {};
   for (uint8_t k = 0; k < g.count; ++k)
   {
      recs[k] = &g.party->members[g.picked[k]];
   }
   const cb::Preview p = cb::preview(g.method, recs, g.count);
   if (!g.ask || p.status != cb::Status::Ok)
   {
      build_combination();
      return;
   }
   // "A + B -> C?"
   int at = 0;
   g.line[0] = text::kCodeEnd;
   for (uint8_t k = 0; k < g.count; ++k)
   {
      if (k > 0)
      {
         append(at, kPlus.codes);
      }
      append(at, species_name(g.party->members[g.picked[k]]));
   }
   append(at, kArrow.codes);
   append(at, result_name(p));
   append(at, kAsk.codes);
   wrap_line();
   port::message(g.line);
   g.step = Step::Ask;
}

/// Opens the input menu: the members not picked yet, and Combine once the method has enough.
void open_pick()
{
   const cb::MethodData* m = cb::method_at(g.method);
   g.entry_count = 0;
   for (uint8_t s = 0; s < g.party->count && g.entry_count < kMaxEntries - 1; ++s)
   {
      bool taken = false;
      for (uint8_t k = 0; k < g.count; ++k)
      {
         taken = taken || g.picked[k] == s;
      }
      if (!taken)
      {
         g.entries[g.entry_count++] = s;
      }
   }
   if (g.count >= m->min_inputs)
   {
      g.entries[g.entry_count++] = kCombineEntry;
   }
   if (g.entry_count == 0)
   {
      finish(kCombineBack);
      return;
   }
   menu::open(g.menu, g.entry_count, fill_pick);
   g.step = Step::Pick;
}

/// Shows the next queued text, or ends.
/// @return true while texts are shown
bool next_text()
{
   battle::Event e{};
   while (battle::pop_event(*g.lent, e))
   {
      if (battle::format_text(*g.lent, e, g.line, kLineCodes))
      {
         wrap_line();
         port::message(g.line);
         g.shown = true;
         return true;
      }
   }
   return false;
}

/// Whether a method can run here (attached, an immediate method).
/// @param method  method index
/// @return true when it can
bool usable(uint8_t method)
{
   const cb::MethodData* m = cb::method_at(method);
   // C9: a nursery method runs in the nursery only (its question would name a result the combination refuses)
   return g.lent != nullptr && g.party != nullptr && g.rng != nullptr && m != nullptr &&
          m->kind == cb::MethodKind::Immediate;
}

/// Starts a statement.
/// @param method  method index
/// @param ask     whether the question comes first
void begin(uint8_t method, bool ask)
{
   if (g.live || g.step != Step::Idle)
   {
      drop();   // a statement left running: it ends here
   }
   g.method = method;
   g.ask = ask;
   g.count = 0;
   g.result = kCombineBack;
   g.step = Step::Finished;
}

} // namespace

void attach(battle::State* lent, monster::Party* party, storage::Storage* boxes, rng::Stream* rng)
{
   g.lent = lent;
   g.party = party;
   g.boxes = boxes;
   g.rng = rng;
}

void set_menus(bool on)
{
   g.menus_off = !on;
}

void open(uint8_t method)
{
   begin(method, true);
   if (usable(method))
   {
      open_pick();
   }
}

void run(uint8_t method, const uint8_t* slots, uint8_t count)
{
   begin(method, false);
   if (!usable(method) || slots == nullptr || count > cb::kMaxInputs)
   {
      return;
   }
   for (uint8_t k = 0; k < count; ++k)
   {
      g.picked[k] = slots[k];
   }
   g.count = count;
   start_combination();
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
         return kCombineRunning;
      }
      if (a == menu::kBack)
      {
         finish(kCombineBack);
         break;
      }
      const uint8_t e = g.entries[a];
      if (e != kCombineEntry)
      {
         g.picked[g.count++] = e;
      }
      const cb::MethodData* m = cb::method_at(g.method);
      if (e == kCombineEntry || g.count >= m->max_inputs)
      {
         start_combination();
      }
      else
      {
         open_pick();
      }
      return g.step == Step::Finished ? g.result : kCombineRunning;
   }
   case Step::Ask:
      if (port::message_busy())
      {
         return kCombineRunning;
      }
      port::yesno_open();
      g.step = Step::Answer;
      return kCombineRunning;
   case Step::Answer:
   {
      const int8_t a = port::yesno_poll();
      if (a < 0)
      {
         return kCombineRunning;
      }
      if (a == 0)
      {
         finish(kCombineBack);   // no: nothing changes (no draw was made)
         break;
      }
      build_combination();   // C9: its draws only now
      return kCombineRunning;
   }
   case Step::Moves:
   {
      const int32_t a = menu::poll(g.menu);
      if (a == menu::kWaiting)
      {
         return kCombineRunning;
      }
      cb::Combination& c = running();
      if (a == menu::kBack || g.entries[a] == kDoneEntry)
      {
         c.done_choosing();
      }
      else
      {
         c.choose(g.entries[a]);
      }
      if (c.stage() == cb::Stage::Choosing)
      {
         open_moves();
      }
      else
      {
         place_now();
      }
      return kCombineRunning;
   }
   case Step::Texts:
      if (g.shown && port::message_busy())
      {
         return kCombineRunning;
      }
      if (g.shown && !port::button())
      {
         return kCombineRunning;   // the text waits for a button, as msgbox does
      }
      g.shown = false;
      if (next_text())
      {
         return kCombineRunning;
      }
      finish(g.after_texts);
      break;
   default:
      break;
   }
   g.step = Step::Idle;
   return g.result;
}

int32_t query(CombineOp kind, uint8_t method, const uint8_t* slots, uint8_t count)
{
   if (g.party == nullptr || slots == nullptr || count > cb::kMaxInputs)
   {
      return 0;
   }
   const monster::Record* recs[cb::kMaxInputs] = {};
   for (uint8_t k = 0; k < count; ++k)
   {
      if (slots[k] >= g.party->count)
      {
         return kind == CombineOp::Status ? static_cast<int32_t>(cb::Status::BadInput) : 0;
      }
      recs[k] = &g.party->members[slots[k]];
   }
   const cb::Preview p = cb::preview(method, recs, count);
   switch (kind)
   {
   case CombineOp::Status:
      return static_cast<int32_t>(p.status);
   case CombineOp::Fee:
      return p.status == cb::Status::Ok ? p.fee : 0;
   default:
      if (p.status != cb::Status::Ok)
      {
         return 0;
      }
      return p.species != cb::kNoSpecies ? int32_t{p.species} + 1 : kCombineRandom;
   }
}

} // namespace capsule::script::combine
