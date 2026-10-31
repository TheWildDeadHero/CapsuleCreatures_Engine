/// @file
/// The inventory's sandbox (D-26 section 2, package G2): the Starter sample's party and bag with a minimal
/// presentation (the test kit's console; bars of plain cells), item use out of battle through the field context,
/// the Starter's shops and the engine save image on this platform's medium (the GBA's flash, a desktop file).
///
/// Pages (L / R): Party (the members' pools and status; A shows a member's moves), Bag (Left / Right: pocket, A:
/// use the item, then choose the member and, for a move-slot item, the slot; a move offer asks which move to
/// forget, B keeps them), Shop (Left / Right: shop, A: buy one), Save (A: write the image, START: read it back,
/// SELECT: a fresh party and bag), Growth (G4, D-26 section 4: Up / Down member, A: one level up and the level_up
/// trigger's transformation (A goes on, B stops it, a move offer asks), B: the after_battle trigger (Glimmet's
/// Radiant form at night), START: Glow Charm (its form back and forth), SELECT: Drill Notes (attack training); the
/// member's training values and form shown). The time of day comes from this app's clock::time_of_day() (a game's
/// port function: here a period every few seconds). Bloom Charm on the Bag page transforms Sproutle.
///
/// Laid out like a game: the engine library, reference/project.json naming the Starter's battle data by relative
/// path, and this scene code.
#include "capsule/battle/battle.h"
#include "capsule/battle/data.h"
#include "capsule/core/clock.h"
#include "capsule/core/memory.h"
#include "capsule/core/random.h"
#include "capsule/gfx/background.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/growth/growth.h"
#include "capsule/hal/system.h"
#include "capsule/inventory/bag.h"
#include "capsule/inventory/use.h"
#include "capsule/save/save.h"
#include "test_kit.h"

#include "battle_ids.h"

#include <cstdint>

namespace
{

using namespace capsule;
using namespace capsule::testkit;
using hal::input::Key;
namespace bt = capsule::battle;
namespace inv = capsule::inventory;
using monster::Role;

/// The app's pages.
enum class Page : uint8_t
{
   Party,      ///< The members.
   Bag,        ///< The pockets.
   Shop,       ///< The shops.
   Growth,     ///< Transformations, forms and training values (G4).
   Save,       ///< The save image.
   NUM_PAGES   ///< Number of pages.
};
constexpr int kPages = static_cast<int>(Page::NUM_PAGES);   ///< Page count.

/// What the Bag page waits for.
enum class Step : uint8_t
{
   Item,      ///< Choosing an item.
   Member,    ///< Choosing the member it is used on.
   Slot,      ///< Choosing a move slot of the member.
   Forget,    ///< A move offer: which move to forget (a modal loop holds the use open).
};

/// @name The starting party and bag (the Starter's creatures and items)
///@{
constexpr uint8_t kLevels[] = {19, 13, 12, 15, 14};              ///< Levels of the members.
constexpr uint16_t kSpecies[] = {battle_ids::SPECIES_EMBERKIT, battle_ids::SPECIES_TIDEPUP,
                                 battle_ids::SPECIES_VOLTMINK, battle_ids::SPECIES_SPROUTLE,
                                 battle_ids::SPECIES_GLIMMET};    ///< Their species (G4: one by item, two forms).
constexpr int kMembers = sizeof(kSpecies) / sizeof(kSpecies[0]);  ///< Members.
constexpr int kLead = 0;                 ///< The member that is hurt and scorched.
constexpr int kSecond = 1;               ///< The member that spent MP.
constexpr int kThird = 2;                ///< The member that spent EP.
static_assert(sizeof(kLevels) == kMembers);
constexpr uint32_t kHurt = 18;           ///< Hit points the lead lacks.
constexpr uint32_t kSpentMp = 12;        ///< MP the second member spent.
constexpr uint32_t kSpentEp = 9;         ///< EP the third member spent.
constexpr uint32_t kStartCoins = 300;    ///< Coins to spend.
constexpr uint32_t kRecordSeed = 2026;   ///< Seed of the records.
/// Items the bag starts with (item, count).
constexpr inv::BagSlot kStartBag[] = {{battle_ids::ITEM_TONIC, 3},     {battle_ids::ITEM_MANA_TEA, 2},
                                      {battle_ids::ITEM_SPARK_SNACK, 2}, {battle_ids::ITEM_REMEDY, 1},
                                      {battle_ids::ITEM_ORB, 5},         {battle_ids::ITEM_MANA_DROP, 1},
                                      {battle_ids::ITEM_GATE_PASS, 1},   {battle_ids::ITEM_BLOOM_CHARM, 1},
                                      {battle_ids::ITEM_GLOW_CHARM, 1},  {battle_ids::ITEM_DRILL_NOTES, 3}};
///@}

/// @name Layout (console cells)
///@{
constexpr int kListTop = 3;          ///< First list row.
constexpr int kListRows = 9;         ///< Rows a list shows.
constexpr int kMemberRows = 3;       ///< Rows per member on the Party page.
constexpr int kBarCells = 10;        ///< Cells of a pool bar.
constexpr int kBarCol = 7;           ///< Column of the bars.
constexpr int kValueCol = 19;        ///< Column of a pool's numbers.
constexpr int kNameCol = 2;          ///< Column of names.
constexpr int kCountCol = 22;        ///< Column of counts and prices.
constexpr int kSideCol = 16;         ///< Column of the side list (members, moves).
constexpr int kMessageRow = 14;      ///< First message row.
constexpr int kMessageRows = 3;      ///< Message rows.
constexpr int kInfoCol = 16;         ///< Column of the save page's numbers.
constexpr int kNameCells = 12;       ///< Cells of a name.
constexpr int kTextCodes = 64;       ///< Codes of one formatted text.
constexpr int kTimeCol = 22;         ///< Column of the time of day.
constexpr int kTimeValueCol = kTimeCol + 5;   ///< Column of its value (after "time ").
constexpr int kTimeRow = kPageTop;   ///< Row of the time of day (under the title).
constexpr int kLevelCol = kValueCol - 2;      ///< Column of a member's "Lv".
constexpr int kRightCol = kCountCol + 2;      ///< Column of a member's status and of a page's ">".
constexpr int kBagCountCol = kNameCol + kNameCells + 2;   ///< Column of a bag slot's count (after its "x").
constexpr int kMemberMarkCol = kSideCol + 2;  ///< Column of the member list's cursor (the Bag page).
constexpr int kMemberNameCol = kSideCol + 3;  ///< Column of its names.
constexpr int kHpRow = 1;            ///< A member's hit point row (after its name row).
constexpr int kPoolRow = 2;          ///< A member's MP / EP row.
constexpr int kImageRow = kListTop;           ///< Save page: the image's bytes.
constexpr int kBudgetRow = kListTop + 1;      ///< Save page: the budget.
constexpr int kCopiesRow = kListTop + 2;      ///< Save page: the copies.
constexpr int kSectionsRow = kListTop + 3;    ///< Save page: the first section's row.
constexpr int kTitleCodes = kNameCells + 2;   ///< Codes of the app's title (and its end).
constexpr int kEpCol = 16;           ///< Column of a member's EP.
constexpr int kLabelCells = 3;       ///< Cells of a pool label and its gap.
constexpr int kPriceCol = 17;        ///< Column of a shop item's price.
constexpr int kHaveCol = 23;         ///< Column of how many the bag holds.
constexpr int kPriceDigits = 4;      ///< Digits of a price.
///@}

/// @name The time of day (this app's port: a period every few seconds)
///@{
constexpr uint32_t kFramesPerPeriod = 600;   ///< Frames per period (10 s at 60 fps).
constexpr uint8_t kPeriods = 4;              ///< Periods of a day.
///@}

/// The app.
struct App
{
   Page page;                       ///< Shown page.
   Step step;                       ///< Bag page step.
   uint8_t pocket;                  ///< Bag page pocket.
   uint8_t shop;                    ///< Shop page shop.
   int cursor;                      ///< List cursor.
   int member;                      ///< Party page / chosen member.
   uint16_t item;                   ///< The item being used.
   uint32_t coins;                  ///< Money.
   uint32_t frames;                 ///< Frames run.
   save::Status last;               ///< The last save / load result.
   bool saved_once;                 ///< A save or load ran.
   save::Status newest;             ///< The newest copy's status (C6: read once, again after a save or load).
   uint32_t newest_seq;             ///< Its sequence number (newest Ok).
   bool newest_known;               ///< newest / newest_seq are current.
   save::Notes notes;               ///< What the last load migrated.
   bool dirty;                      ///< Redraw.
   text::Code lines[kMessageRows][kTextCodes];   ///< Message rows (texts of the last use).
   int line_count;                  ///< Message rows used.
};

App g{};                                       ///< The app.
CAPSULE_EWRAM_BSS inv::Bag g_bag;              ///< The bag (1 KB of slots on the GBA: EWRAM).
CAPSULE_EWRAM_BSS monster::Party g_party;      ///< The party.
CAPSULE_EWRAM_BSS bt::State g_field;           ///< The State item uses borrow (their field context).

/// Adds a message row from ASCII.
/// @param s  the text
void say(const char* s)
{
   if (g.line_count >= kMessageRows)
   {
      return;
   }
   int n = 0;
   for (; s[n] != '\0' && n < kTextCodes - 1; ++n)
   {
      g.lines[g.line_count][n] = static_cast<text::Code>(s[n]);
   }
   g.lines[g.line_count][n] = text::kCodeEnd;
   ++g.line_count;
}

/// Takes a use's texts into the message rows.
/// @param u  the use
void take_texts(inv::FieldUse& u)
{
   while (g.line_count < kMessageRows && u.next_text(g.lines[g.line_count], kTextCodes))
   {
      ++g.line_count;
   }
}

/// Clips a name to its cells.
/// @param out   kNameCells + 1 codes
/// @param name  the name, or nullptr
/// @return out
const text::Code* clip(text::Code* out, const text::Code* name)
{
   int n = 0;
   for (; name != nullptr && name[n] != text::kCodeEnd && n < kNameCells; ++n)
   {
      out[n] = name[n];
   }
   out[n] = text::kCodeEnd;
   return out;
}

/// A fresh party (three Starter creatures, some pools spent, one with a status) and bag.
void fresh_state()
{
   g_party = monster::Party{};
   rng::Stream r{kRecordSeed};
   for (int k = 0; k < kMembers; ++k)
   {
      bt::make_monster(g_party.members[k], kSpecies[k], kLevels[k], r);
   }
   g_party.count = kMembers;
   monster::Record& lead = g_party.members[kLead];
   monster::set(lead, Role::CurrentHp, monster::get(lead, Role::CurrentHp) - kHurt);
   monster::set(lead, Role::PrimaryStatus, battle_ids::STATUS_SCORCH);
   monster::Record& second = g_party.members[kSecond];
   monster::set(second, battle_ids::ROLE_CURRENT_MP, 0, monster::get(second, battle_ids::ROLE_CURRENT_MP) - kSpentMp);
   monster::Record& third = g_party.members[kThird];
   monster::set(third, battle_ids::ROLE_CURRENT_EP, 0, monster::get(third, battle_ids::ROLE_CURRENT_EP) - kSpentEp);
   inv::clear(g_bag);
   for (const inv::BagSlot& s : kStartBag)
   {
      inv::add(g_bag, s.item, s.count);
   }
   g.coins = kStartCoins;
}

/// Draws a pool: its bar of cells and its numbers.
/// @param y      row
/// @param label  the pool's label
/// @param value  current
/// @param top    maximum
void pool_row(int y, const char* label, uint32_t value, uint32_t top)
{
   put_ascii(INK_DIM, kNameCol + 1, y, label);
   const int filled = top == 0 ? 0 : static_cast<int>(value * kBarCells / top);
   for (int c = 0; c < kBarCells; ++c)
   {
      put_ascii(c < filled ? INK_ON : INK_OFF, kBarCol + c, y, c < filled ? "=" : "-");
   }
   int x = put_uint(INK_ON, kValueCol, y, value);
   x = put_ascii(INK_DIM, x, y, "/");
   put_uint(INK_DIM, x, y, top);
}

/// Draws the Party page.
void draw_party()
{
   for (int k = 0; k < g_party.count; ++k)
   {
      const monster::Record& rec = g_party.members[k];
      const int y = kListTop + k * kMemberRows;
      uint16_t stats[bt::kMaxStats] = {};
      bt::record_stats(rec, stats);
      const uint16_t sp = bt::find_species(AssetKey{monster::get(rec, Role::SpeciesKey)});
      text::Code name[kNameCells + 1];
      put_ascii(k == g.member ? INK_TITLE : INK_DIM, 0, y, k == g.member ? ">" : " ");
      put(k == g.member ? INK_ON : INK_DIM, kNameCol, y, clip(name, sp != bt::kNone16 ? bt::kData.species[sp].name
                                                                                     : nullptr));
      put_ascii(INK_DIM, kLevelCol, y, "Lv");
      put_uint(INK_ON, kValueCol, y, monster::get(rec, Role::Level));
      // the status id in the record's low status bits (the counter above them)
      const uint32_t st = monster::get(rec, Role::PrimaryStatus) & ((1u << bt::kData.status_bits) - 1u);
      if (st != 0 && st < bt::kData.status_count)
      {
         put(INK_TITLE, kRightCol, y, clip(name, bt::kData.statuses[st].name));
      }
      pool_row(y + kHpRow, "HP", monster::get(rec, Role::CurrentHp), stats[battle_ids::STAT_HP]);
      // MP and EP as numbers on the third row
      const int py = y + kPoolRow;
      put_ascii(INK_DIM, kNameCol + 1, py, "MP");
      int x = put_uint(INK_ON, kBarCol, py, monster::get(rec, battle_ids::ROLE_CURRENT_MP));
      x = put_ascii(INK_DIM, x, py, "/");
      put_uint(INK_DIM, x, py, stats[battle_ids::STAT_MP]);
      put_ascii(INK_DIM, kEpCol, py, "EP");
      x = put_uint(INK_ON, kEpCol + kLabelCells, py, monster::get(rec, battle_ids::ROLE_CURRENT_EP));
      x = put_ascii(INK_DIM, x, py, "/");
      put_uint(INK_DIM, x, py, stats[battle_ids::STAT_EP]);
   }
   controls(nullptr, nullptr);
   put_ascii(INK_OFF, 0, kControlsTop, "Up/Down member  A moves");
   put_ascii(INK_OFF, 0, kControlsBottom, "L/R page");
}

/// Draws a member's moves on the side.
/// @param member  party slot
/// @param mark    the row to mark, or -1
void draw_moves(int member, int mark)
{
   const monster::Record& rec = g_party.members[member];
   for (int k = 0; k < bt::move_slots() && k < kListRows; ++k)
   {
      const uint32_t m = monster::get(rec, Role::Moves, k);
      text::Code name[kNameCells + 1];
      put_ascii(k == mark ? INK_TITLE : INK_DIM, kSideCol, kListTop + k, k == mark ? ">" : " ");
      if (m < bt::kData.move_count)
      {
         put(k == mark ? INK_ON : INK_DIM, kSideCol + 1, kListTop + k, clip(name, bt::kData.moves[m].name));
      }
      else
      {
         put_ascii(INK_OFF, kSideCol + 1, kListTop + k, "-");
      }
   }
}

/// Draws the Bag page.
void draw_bag()
{
   const inv::PocketData& p = inv::kInventory.pockets[g.pocket];
   put_ascii(INK_DIM, 0, kListTop - 1, "<");
   put(INK_TITLE, kNameCol, kListTop - 1, p.label);
   put_ascii(INK_DIM, kRightCol, kListTop - 1, ">");
   int used = 0;
   const inv::BagSlot* s = inv::pocket_slots(g_bag, g.pocket, used);
   for (int k = 0; k < used && k < kListRows; ++k)
   {
      text::Code name[kNameCells + 1];
      const bool on = g.step == Step::Item && k == g.cursor;
      put_ascii(on ? INK_TITLE : INK_DIM, 0, kListTop + k, on ? ">" : " ");
      put(inv::field_usable(s[k].item) ? INK_ON : INK_DIM, kNameCol, kListTop + k,
          clip(name, bt::kData.items[s[k].item].name));
      put_ascii(INK_DIM, kNameCol + kNameCells + 1, kListTop + k, "x");
      put_uint(INK_ON, kBagCountCol, kListTop + k, s[k].count);
   }
   if (used == 0)
   {
      put_ascii(INK_OFF, kNameCol, kListTop, "(empty)");
   }
   if (g.step == Step::Member)
   {
      for (int k = 0; k < g_party.count; ++k)
      {
         text::Code name[kNameCells + 1];
         const uint16_t sp = bt::find_species(AssetKey{monster::get(g_party.members[k], Role::SpeciesKey)});
         put_ascii(k == g.member ? INK_TITLE : INK_DIM, kMemberMarkCol, kListTop + k, k == g.member ? ">" : " ");
         put(INK_ON, kMemberNameCol, kListTop + k, clip(name, sp != bt::kNone16 ? bt::kData.species[sp].name : nullptr));
      }
   }
   else if (g.step == Step::Slot || g.step == Step::Forget)
   {
      draw_moves(g.member, g.cursor);
   }
   for (int k = 0; k < g.line_count; ++k)
   {
      put(INK_ON, 0, kMessageRow + k, g.lines[k]);
   }
   controls(nullptr, nullptr);
   put_ascii(INK_OFF, 0, kControlsTop, g.step == Step::Forget ? "A forget this move  B keep" :
                                       g.step == Step::Item   ? "Left/Right pocket  A use" : "A choose  B back");
   put_ascii(INK_OFF, 0, kControlsBottom, "L/R page");
}

/// Draws the Shop page.
void draw_shop()
{
   const inv::ShopData& sh = inv::kInventory.shops[g.shop];
   put_ascii(INK_DIM, 0, kListTop - 1, "<");
   put(INK_TITLE, kNameCol, kListTop - 1, sh.name);
   put_ascii(INK_DIM, kRightCol, kListTop - 1, ">");
   for (int k = 0; k < sh.count && k < kListRows; ++k)
   {
      const uint16_t item = inv::kInventory.shop_items[sh.first + k];
      text::Code name[kNameCells + 1];
      put_ascii(k == g.cursor ? INK_TITLE : INK_DIM, 0, kListTop + k, k == g.cursor ? ">" : " ");
      put(INK_ON, kNameCol, kListTop + k, clip(name, bt::kData.items[item].name));
      put_uint(INK_ON, kPriceCol, kListTop + k, inv::info(item)->price, kPriceDigits);
      put_ascii(INK_DIM, kHaveCol, kListTop + k, "x");
      put_uint(INK_DIM, kHaveCol + 1, kListTop + k, inv::count(g_bag, item));
   }
   put_ascii(INK_DIM, 0, kMessageRow - 1, "Coins");
   put_uint(INK_ON, kBarCol, kMessageRow - 1, g.coins);
   for (int k = 0; k < g.line_count; ++k)
   {
      put(INK_ON, 0, kMessageRow + k, g.lines[k]);
   }
   controls(nullptr, nullptr);
   put_ascii(INK_OFF, 0, kControlsTop, "Left/Right shop  A buy one");
   put_ascii(INK_OFF, 0, kControlsBottom, "L/R page");
}

/// @name Growth page layout (G4)
///@{
constexpr int kGrowthStatCols = 3;      ///< Training values per row.
constexpr int kGrowthStatCells = 9;     ///< Cells of one (label and value).
constexpr int kGrowthTrainingRow = kListTop + kMembers + 1;   ///< First row of the member's training values.
constexpr int kGrowthFormCol = kRightCol;   ///< Column of a member's form.
///@}

void draw();
uint8_t ask_forget(Pad& pad);
void say_result(inv::UseResult r);

/// A message row being written.
struct Line
{
   text::Code* out;   ///< The row's codes.
   int n;             ///< Codes written.
};

/// Appends codes to a row.
/// @param l      the row
/// @param codes  codes ending with kCodeEnd, or nullptr
void append(Line& l, const text::Code* codes)
{
   for (int k = 0; codes != nullptr && codes[k] != text::kCodeEnd && l.n < kTextCodes - 1; ++k)
   {
      l.out[l.n++] = codes[k];
   }
}

/// Appends ASCII to a row.
/// @param l  the row
/// @param t  the text
void append_ascii(Line& l, const char* t)
{
   for (int k = 0; t[k] != '\0' && l.n < kTextCodes - 1; ++k)
   {
      l.out[l.n++] = static_cast<text::Code>(t[k]);
   }
}

/// Appends a species' name to a row.
/// @param l   the row
/// @param sp  species index
void append_species(Line& l, uint16_t sp)
{
   text::Code name[kNameCells + 1];
   append(l, clip(name, sp < bt::kData.species_count ? bt::kData.species[sp].name : nullptr));
}

/// The lent State's growth events as message rows ("A -> B", a form, a second creature).
void take_growth_events()
{
   bt::Event e{};
   while (bt::pop_event(g_field, e))
   {
      if (e.kind != bt::EventKind::Transform || g.line_count >= kMessageRows)
      {
         continue;
      }
      Line l{g.lines[g.line_count], 0};
      switch (e.extra)
      {
      case growth::kTransformStart:
         append_species(l, e.a);
         append_ascii(l, " is changing...");
         break;
      case growth::kTransformDone:
         append_species(l, e.a);
         append_ascii(l, " -> ");
         append_species(l, e.b);
         break;
      case growth::kTransformStopped:
         append_species(l, e.a);
         append_ascii(l, " stayed as it is.");
         break;
      case growth::kTransformForm:
         append_ascii(l, "Form ");
         l.out[l.n++] = static_cast<text::Code>('0' + e.a);
         break;
      default:
         append_ascii(l, "Joined: ");
         append_species(l, e.a);
         break;
      }
      l.out[l.n] = text::kCodeEnd;
      ++g.line_count;
   }
}

/// A transformation's question: frames of its own (the Change's field context stays open) until A (go on) or B
/// (stop, when the manifest allows it).
/// @param pad  the pad
/// @return true to go on
bool ask_go(Pad& pad)
{
   draw();
   clear_rows(kControlsTop, kControlsBottom);
   put_ascii(INK_OFF, 0, kControlsTop, "A go on  B stop");
   while (hal::system::update())
   {
      pad.update();
      if (pad.hit(Key::KEY_A))
      {
         return true;
      }
      if (pad.hit(Key::KEY_B))
      {
         return false;
      }
   }
   return true;
}

/// Runs the member's change for a trigger: the question, the move offers, the write-back at the scope's end.
/// @param pad      the pad
/// @param trigger  the trigger
void change_now(Pad& pad, growth::Trigger trigger)
{
   g.line_count = 0;
   {
      growth::Change c(g_field, g_party, static_cast<uint8_t>(g.member), growth::make_context(trigger));
      if (c.stage != growth::Stage::Asking)
      {
         say(c.found.kind == growth::ChangeKind::Blocked ? "Something stops it." : "Nothing happens.");
         return;
      }
      take_growth_events();
      if (c.cancellable() && !ask_go(pad))
      {
         c.stop();
         g.line_count = 0;
         take_growth_events();
         return;
      }
      growth::Stage st = c.go();
      g.line_count = 0;
      take_growth_events();
      while (st == growth::Stage::Learning)
      {
         say("Forget which move?");
         const uint8_t forget = ask_forget(pad);
         st = c.answer_learn(forget);
         g.line_count = 0;
      }
   }
   take_growth_events();   // a second creature (written when the scope ended)
   g.page = Page::Growth;
   g.step = Step::Item;
}

/// Uses an item of the bag on the member from the Growth page (Glow Charm, Drill Notes).
/// @param item  the item
void use_on_member(uint16_t item)
{
   g.line_count = 0;
   inv::FieldUse u(g_field, g_party, static_cast<uint8_t>(g.member));
   const inv::UseResult r = u.use(g_bag, item);
   take_texts(u);
   say_result(r);
   if (r == inv::UseResult::Used)
   {
      say("Done.");
   }
}

/// Draws the Growth page: each member's species, level and form; the chosen member's training values.
void draw_growth()
{
   static const char* const kStatLabels[] = {"hp", "mp", "ep", "atk", "mag", "def", "pro", "int", "agi"};
   for (int k = 0; k < g_party.count && k < kListRows; ++k)
   {
      const monster::Record& rec = g_party.members[k];
      const uint16_t sp = bt::find_species(AssetKey{monster::get(rec, Role::SpeciesKey)});
      text::Code name[kNameCells + 1];
      const int y = kListTop + k;
      put_ascii(k == g.member ? INK_TITLE : INK_DIM, 0, y, k == g.member ? ">" : " ");
      put(k == g.member ? INK_ON : INK_DIM, kNameCol, y, clip(name, sp != bt::kNone16 ? bt::kData.species[sp].name
                                                                                     : nullptr));
      put_ascii(INK_DIM, kLevelCol, y, "Lv");
      put_uint(INK_ON, kValueCol, y, monster::get(rec, Role::Level));
      if (sp != bt::kNone16 && growth::form_count(sp) > 0)
      {
         put_ascii(INK_DIM, kGrowthFormCol, y, "f");
         put_uint(INK_TITLE, kGrowthFormCol + 1, y, growth::record_form(rec));
      }
   }
   // the chosen member's training values (structure training: caps per stat and in all)
   const monster::Record& rec = g_party.members[g.member];
   put_ascii(INK_DIM, kNameCol, kGrowthTrainingRow - 1, "Training");
   uint32_t total = 0;
   for (int st = 0; st < bt::kData.stat_count && st < static_cast<int>(sizeof(kStatLabels) / sizeof(kStatLabels[0]));
        ++st)
   {
      const uint32_t v = monster::get(rec, Role::StatTraining, st);
      total += v;
      const int x = kNameCol + (st % kGrowthStatCols) * kGrowthStatCells;
      const int y = kGrowthTrainingRow + st / kGrowthStatCols;
      put_ascii(INK_DIM, x, y, kStatLabels[st]);
      put_uint(v > 0 ? INK_ON : INK_OFF, x + kLabelCells + 1, y, v);
   }
   int x = put_ascii(INK_DIM, kInfoCol, kGrowthTrainingRow - 1, "sum ");
   x = put_uint(INK_ON, x, kGrowthTrainingRow - 1, total);
   x = put_ascii(INK_DIM, x, kGrowthTrainingRow - 1, "/");
   put_uint(INK_DIM, x, kGrowthTrainingRow - 1, growth::kGrowth.training.total_cap);
   for (int k = 0; k < g.line_count; ++k)
   {
      put(INK_ON, 0, kMessageRow + k, g.lines[k]);
   }
   controls(nullptr, nullptr);
   put_ascii(INK_OFF, 0, kControlsTop, "A level up  B after battle");
   put_ascii(INK_OFF, 0, kControlsBottom, "START form  SEL train  L/R");
}

/// The Growth page's input.
/// @param pad  the pad
void growth_input(Pad& pad)
{
   if (pad.repeat(Key::KEY_UP) || pad.repeat(Key::KEY_DOWN))
   {
      g.member = pad.repeat(Key::KEY_UP) ? (g.member + g_party.count - 1) % g_party.count
                                         : (g.member + 1) % g_party.count;
      g.line_count = 0;
      g.dirty = true;
   }
   if (pad.hit(Key::KEY_A))
   {
      // one level up out of battle (its experience follows), then the level_up trigger
      monster::Record& rec = g_party.members[g.member];
      const uint32_t level = monster::get(rec, Role::Level);
      const uint16_t sp = bt::find_species(AssetKey{monster::get(rec, Role::SpeciesKey)});
      if (sp != bt::kNone16 && level < bt::kData.rules.level_max)
      {
         monster::set(rec, Role::Level, level + 1);
         monster::set(rec, Role::Experience, bt::kData.growth[bt::kData.species[sp].growth][level + 1]);
      }
      change_now(pad, growth::Trigger::LevelUp);
      g.dirty = true;
   }
   else if (pad.hit(Key::KEY_B))
   {
      change_now(pad, growth::Trigger::AfterBattle);
      g.dirty = true;
   }
   else if (pad.hit(Key::KEY_START))
   {
      use_on_member(battle_ids::ITEM_GLOW_CHARM);
      g.dirty = true;
   }
   else if (pad.hit(Key::KEY_SELECT))
   {
      use_on_member(battle_ids::ITEM_DRILL_NOTES);
      g.dirty = true;
   }
}

/// A save status's words.
/// @param st  the status
/// @return ASCII
const char* status_text(save::Status st)
{
   switch (st)
   {
   case save::Status::Ok:
      return "ok";
   case save::Status::Empty:
      return "no save yet";
   case save::Status::Corrupt:
      return "damaged";
   case save::Status::OtherVersion:
      return "other version";
   case save::Status::OtherShape:
      return "other game data";
   case save::Status::NoRoom:
      return "does not fit";
   case save::Status::MediumError:
      return "medium error";
   default:
      return "?";
   }
}

/// Draws the Save page.
void draw_save()
{
   const char* const names[save::kSectionCount] = {"Bag", "Party", "Boxes", "Script"};
   put_ascii(INK_DIM, kNameCol, kImageRow, "Image bytes");
   put_uint(INK_ON, kInfoCol, kImageRow, save::image_size());
   put_ascii(INK_DIM, kNameCol, kBudgetRow, "Budget");
   put_uint(INK_ON, kInfoCol, kBudgetRow, save::kSaveBudget);
   put_ascii(INK_DIM, kNameCol, kCopiesRow, "Copies");
   put_uint(INK_ON, kInfoCol, kCopiesRow, static_cast<uint32_t>(save::copy_count()));
   for (int k = 0; k < save::kSectionCount; ++k)
   {
      put_ascii(INK_DIM, kNameCol + 1, kSectionsRow + k, names[k]);
      put_uint(INK_ON, kInfoCol, kSectionsRow + k, save::section_size(static_cast<save::Section>(k)));
   }
   // C6 (review4 finding 22): the medium is read once, not on every redraw (both copies whole each time)
   if (!g.newest_known)
   {
      g.newest = save::newest(g.newest_seq);
      g.newest_known = true;
   }
   const int newest_row = kSectionsRow + save::kSectionCount;
   put_ascii(INK_DIM, kNameCol, newest_row, "Newest copy");
   if (g.newest == save::Status::Ok)
   {
      put_uint(INK_ON, kInfoCol, newest_row, g.newest_seq);
   }
   else
   {
      put_ascii(INK_OFF, kInfoCol, newest_row, status_text(g.newest));
   }
   if (g.saved_once)
   {
      put_ascii(INK_DIM, kNameCol, kMessageRow, "Last:");
      put_ascii(INK_ON, kBarCol, kMessageRow, status_text(g.last));
      // what a load of an older shape changed (dropped / clamped bag entries)
      if (g.notes.dropped_items != 0 || g.notes.clamped_items != 0)
      {
         put_uint(INK_ON, kInfoCol, kMessageRow, g.notes.dropped_items + g.notes.clamped_items);
      }
   }
   controls(nullptr, nullptr);
   put_ascii(INK_OFF, 0, kControlsTop, "A save  START load  SEL reset");
   put_ascii(INK_OFF, 0, kControlsBottom, "L/R page");
}

/// Redraws the screen.
void draw()
{
   static const char* const kNames[kPages] = {"Party", "Bag", "Shop", "Growth", "Save"};
   clear_console();
   text::Code app[kTitleCodes];
   text::Code page[kNameCells + 1];
   const char* title_text = "PARTY & BAG";
   int n = 0;
   for (; title_text[n] != '\0' && n < kTitleCodes - 1; ++n)
   {
      app[n] = static_cast<text::Code>(title_text[n]);
   }
   app[n] = text::kCodeEnd;
   n = 0;
   for (const char* s = kNames[static_cast<int>(g.page)]; s[n] != '\0' && n < kNameCells; ++n)
   {
      page[n] = static_cast<text::Code>(s[n]);
   }
   page[n] = text::kCodeEnd;
   title(app, static_cast<int>(g.page), kPages, page);
   put_ascii(INK_DIM, kTimeCol, kTimeRow, "time");
   put_uint(INK_ON, kTimeValueCol, kTimeRow, clock::time_of_day());
   switch (g.page)
   {
   case Page::Party:
      draw_party();
      break;
   case Page::Bag:
      draw_bag();
      break;
   case Page::Shop:
      draw_shop();
      break;
   case Page::Growth:
      draw_growth();
      break;
   default:
      draw_save();
      break;
   }
   g.dirty = false;
}

/// Moves a cursor within a list.
/// @param pad    the pad
/// @param count  list entries
void move_cursor(const Pad& pad, int count)
{
   if (count <= 0)
   {
      g.cursor = 0;
      return;
   }
   if (pad.repeat(Key::KEY_UP))
   {
      g.cursor = g.cursor == 0 ? count - 1 : g.cursor - 1;
      g.dirty = true;
   }
   if (pad.repeat(Key::KEY_DOWN))
   {
      g.cursor = g.cursor + 1 >= count ? 0 : g.cursor + 1;
      g.dirty = true;
   }
}

/// What a use result says (texts of the logic come first).
/// @param r  the result
void say_result(inv::UseResult r)
{
   switch (r)
   {
   case inv::UseResult::NoEffect:
      say("It had no effect.");
      break;
   case inv::UseResult::NotUsable:
      say("It cannot be used here.");
      break;
   case inv::UseResult::BadTarget:
      say("Choose another target.");
      break;
   default:
      break;
   }
}

/// A move offer's answer: frames of its own (the use's field context stays open, a scope of the caller) until the
/// player picks the move to forget (A) or keeps them all (B).
/// @param pad  the pad
/// @return the move slot, or battle::kKeepMoves
uint8_t ask_forget(Pad& pad)
{
   g.step = Step::Forget;
   g.cursor = 0;
   draw();
   while (hal::system::update())
   {
      pad.update();
      move_cursor(pad, bt::move_slots());
      if (pad.hit(Key::KEY_A))
      {
         return static_cast<uint8_t>(g.cursor);
      }
      if (pad.hit(Key::KEY_B))
      {
         return bt::kKeepMoves;
      }
      if (g.dirty)
      {
         draw();
      }
   }
   return bt::kKeepMoves;
}

/// Uses g.item on the chosen member (and slot); the member is written back when the use's scope ends.
/// @param pad   the pad (a move offer reads it)
/// @param slot  move slot, or inv::kNoMoveSlot
void use_now(Pad& pad, uint8_t slot)
{
   g.line_count = 0;
   {
      inv::FieldUse u(g_field, g_party, static_cast<uint8_t>(g.member));
      inv::UseResult r = u.use(g_bag, g.item, slot);
      take_texts(u);
      if (r == inv::UseResult::LearnOffer)
      {
         say("Forget which move?");
         const uint8_t forget = ask_forget(pad);
         g.line_count = 0;
         r = u.answer_learn(g_bag, forget);
         take_texts(u);
      }
      say_result(r);
   }
   g.step = Step::Item;
   g.cursor = 0;
}

/// The Bag page's input.
/// @param pad  the pad
void bag_input(Pad& pad)
{
   int used = 0;
   const inv::BagSlot* s = inv::pocket_slots(g_bag, g.pocket, used);
   switch (g.step)
   {
   case Step::Item:
      if (pad.hit(Key::KEY_LEFT) || pad.hit(Key::KEY_RIGHT))
      {
         const int step = pad.hit(Key::KEY_LEFT) ? inv::kInventory.pocket_count - 1 : 1;
         g.pocket = static_cast<uint8_t>((g.pocket + step) % inv::kInventory.pocket_count);
         g.cursor = 0;
         g.dirty = true;
      }
      move_cursor(pad, used);
      if (pad.hit(Key::KEY_A) && g.cursor < used)
      {
         g.item = s[g.cursor].item;
         g.line_count = 0;
         if (!inv::field_usable(g.item))
         {
            say("It cannot be used here.");
         }
         else if (inv::info(g.item)->target == inv::Target::None)
         {
            g.member = inv::kNoMember;
            use_now(pad, inv::kNoMoveSlot);
         }
         else
         {
            g.step = Step::Member;
            g.member = 0;
         }
         g.dirty = true;
      }
      break;
   case Step::Member:
      if (pad.repeat(Key::KEY_UP) || pad.repeat(Key::KEY_DOWN))
      {
         g.member = pad.repeat(Key::KEY_UP) ? (g.member + g_party.count - 1) % g_party.count
                                            : (g.member + 1) % g_party.count;
         g.dirty = true;
      }
      if (pad.hit(Key::KEY_B))
      {
         g.step = Step::Item;
         g.dirty = true;
      }
      else if (pad.hit(Key::KEY_A))
      {
         if (inv::info(g.item)->target == inv::Target::MoveSlot)
         {
            g.step = Step::Slot;
            g.cursor = 0;
         }
         else
         {
            use_now(pad, inv::kNoMoveSlot);
         }
         g.dirty = true;
      }
      break;
   case Step::Slot:
      move_cursor(pad, bt::move_slots());
      if (pad.hit(Key::KEY_B))
      {
         g.step = Step::Member;
         g.dirty = true;
      }
      else if (pad.hit(Key::KEY_A))
      {
         use_now(pad, static_cast<uint8_t>(g.cursor));
         g.dirty = true;
      }
      break;
   case Step::Forget:
      break;   // ask_forget() reads the pad while it waits
   }
}

/// The Shop page's input.
/// @param pad  the pad
void shop_input(const Pad& pad)
{
   if (inv::kInventory.shop_count == 0)
   {
      return;
   }
   if (pad.hit(Key::KEY_LEFT) || pad.hit(Key::KEY_RIGHT))
   {
      const int step = pad.hit(Key::KEY_LEFT) ? inv::kInventory.shop_count - 1 : 1;
      g.shop = static_cast<uint8_t>((g.shop + step) % inv::kInventory.shop_count);
      g.cursor = 0;
      g.dirty = true;
   }
   const inv::ShopData& sh = inv::kInventory.shops[g.shop];
   move_cursor(pad, sh.count);
   if (pad.hit(Key::KEY_A) && g.cursor < sh.count)
   {
      const uint16_t item = inv::kInventory.shop_items[sh.first + g.cursor];
      const uint32_t price = inv::info(item)->price;
      g.line_count = 0;
      if (g.coins < price)
      {
         say("Not enough coins.");
      }
      else if (!inv::add(g_bag, item, 1))
      {
         say("No room in the bag.");
      }
      else
      {
         g.coins -= price;
         say("Bought one.");
      }
      g.dirty = true;
   }
}

/// The Save page's input.
/// @param pad  the pad
void save_input(const Pad& pad)
{
   // no storage boxes here: the image's Boxes section is copied from the newest copy unchanged (C6), so a save
   // made by a game with boxes keeps them
   const save::Sources src{&g_bag, &g_party, nullptr, nullptr};
   if (pad.hit(Key::KEY_A))
   {
      g.last = save::write(src);
      g.saved_once = true;
      g.newest_known = false;
      g.notes = save::Notes{};
      g.dirty = true;
   }
   else if (pad.hit(Key::KEY_START))
   {
      g.last = save::load(src, &g.notes);
      g.saved_once = true;
      g.newest_known = false;
      g.dirty = true;
   }
   else if (pad.hit(Key::KEY_SELECT))
   {
      fresh_state();
      g.dirty = true;
   }
}

/// One frame of input.
/// @param pad  the pad
void frame(Pad& pad)
{
   const bool busy = g.page == Page::Bag && g.step != Step::Item;
   if (!busy && (pad.hit(Key::KEY_L) || pad.hit(Key::KEY_R)))
   {
      const int step = pad.hit(Key::KEY_L) ? kPages - 1 : 1;
      g.page = static_cast<Page>((static_cast<int>(g.page) + step) % kPages);
      g.cursor = 0;
      g.line_count = 0;
      g.dirty = true;
      return;
   }
   switch (g.page)
   {
   case Page::Party:
      if (pad.repeat(Key::KEY_UP) || pad.repeat(Key::KEY_DOWN))
      {
         g.member = pad.repeat(Key::KEY_UP) ? (g.member + g_party.count - 1) % g_party.count
                                            : (g.member + 1) % g_party.count;
         g.dirty = true;
      }
      if (pad.hit(Key::KEY_A))
      {
         draw();
         clear_rows(kListTop, kPageBottom);
         draw_moves(g.member, -1);
      }
      break;
   case Page::Bag:
      bag_input(pad);
      break;
   case Page::Shop:
      shop_input(pad);
      break;
   case Page::Growth:
      growth_input(pad);
      break;
   default:
      save_input(pad);
      break;
   }
}

} // namespace

namespace capsule::clock
{

uint8_t time_of_day()
{
   // this game's port function (D-26 decision 3): a period every kFramesPerPeriod frames
   return static_cast<uint8_t>(g.frames / kFramesPerPeriod % kPeriods);
}

} // namespace capsule::clock

/// Runs the sandbox.
/// @return 0
int main()
{
   hal::system::init();
   gfx::palette::reset();
   gfx::sprites::reset();
   gfx::background::reset();
   init_console();
   fresh_state();
   g.dirty = true;
   Pad pad;
   uint8_t shown_time = clock::time_of_day();
   while (hal::system::update())
   {
      rng::next16(rng::gameplay);   // A-4: the gameplay stream advances every frame
      ++g.frames;
      pad.update();
      frame(pad);
      if (clock::time_of_day() != shown_time)
      {
         shown_time = clock::time_of_day();
         g.dirty = true;
      }
      if (g.dirty)
      {
         draw();
      }
      gfx::sprites::update();
   }
   return 0;
}
