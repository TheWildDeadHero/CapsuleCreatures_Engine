/// @file
/// The engine's default items port (capsule/script/port.h namespace items, G2): the bag statements work on the
/// bag given to attach(); the bag menu, shop list and learn question are the port's choice menu (the text box)
/// with the items' names and counts or prices, a page at a time when they are longer than one menu (C6:
/// capsule/script/menu.h). A game replaces any of them with its own screens by defining them. N1: give(), take() and
/// count() are weak (CAPSULE_WEAK): a game with its own bag rules (pockets, stack limits) defines them and keeps the
/// default menus.
#include "capsule/battle/battle.h"
#include "capsule/battle/data.h"
#include "capsule/core/memory.h"
#include "capsule/inventory/bag.h"
#include "capsule/script/menu.h"
#include "capsule/script/port.h"
#include "capsule/script/script.h"

namespace capsule::script::items
{

namespace
{

/// Codes per menu option (name, gap, number, end).
constexpr int kOptionCodes = menu::kOptionCodes;
/// Codes between a name and its number.
constexpr int kGapCodes = 2;
/// Decimal base of the numbers shown.
constexpr uint32_t kDecimalBase = 10;
/// Most digits a number shown has (a 32-bit value).
constexpr int kMaxDigits = 10;

/// Which menu is open.
enum class Menu : uint8_t
{
   None,    ///< Nothing.
   Bag,     ///< The bag.
   Shop,    ///< A shop list.
   Learn,   ///< The learn question.
};

/// The defaults' state.
struct Port
{
   inventory::Bag* bag;                              ///< attach()'s bag.
   Menu menu;                                        ///< Open menu.
   uint8_t shop;                                     ///< Open shop.
   uint8_t member;                                   ///< The learn question's party member.
   VarId money;                                      ///< The shop's money variable.
   menu::Paged page;                                 ///< The open menu's pages (C6).
};

/// The state (one bag, one menu at a time; zero at start-up; EWRAM on the GBA: menus are not speed-critical).
CAPSULE_EWRAM_BSS Port g;

/// Copies a name into an option and appends a number after a gap.
/// @param out     the option's codes
/// @param name    the name (nullptr: none)
/// @param number  the number (shown when @p show)
/// @param show    whether to show it
void option_text(text::Code* out, const text::Code* name, uint32_t number, bool show)
{
   int n = 0;
   for (; name != nullptr && name[n] != text::kCodeEnd && n < kOptionCodes - kMaxDigits - kGapCodes - 1; ++n)
   {
      out[n] = name[n];
   }
   if (show)
   {
      for (int k = 0; k < kGapCodes; ++k)
      {
         out[n++] = text::kCodeSpace;
      }
      text::Code digits[kMaxDigits];
      int d = 0;
      do
      {
         digits[d++] = static_cast<text::Code>(text::kCodeDigitZero + number % kDecimalBase);
         number /= kDecimalBase;
      }
      while (number > 0 && d < kMaxDigits);
      while (d > 0)
      {
         out[n++] = digits[--d];
      }
   }
   out[n] = text::kCodeEnd;
}

/// An item's display name.
/// @param item  item id
/// @return its codes, or nullptr
const text::Code* item_name(uint16_t item)
{
   return item < battle::kData.item_count ? battle::kData.items[item].name : nullptr;
}

/// The bag's entry at an index (pocket by pocket, slot by slot).
/// @param index  the entry
/// @return its slot, or nullptr past the end
const inventory::BagSlot* bag_entry(int index)
{
   for (uint8_t p = 0; g.bag != nullptr && p < inventory::kInventory.pocket_count; ++p)
   {
      int used = 0;
      const inventory::BagSlot* s = inventory::pocket_slots(*g.bag, p, used);
      if (index < used)
      {
         return &s[index];
      }
      index -= used;
   }
   return nullptr;
}

/// Writes a bag entry's option: its name and count.
/// @param index  the entry
/// @param out    the option's codes
void bag_option(int index, text::Code* out)
{
   const inventory::BagSlot* s = bag_entry(index);
   option_text(out, s != nullptr ? item_name(s->item) : nullptr, s != nullptr ? s->count : 0, s != nullptr);
}

/// The open shop's item at an index.
/// @param index  the entry
/// @return item id
uint16_t shop_item(int index)
{
   const inventory::ShopData& sh = inventory::kInventory.shops[g.shop];
   return inventory::kInventory.shop_items[sh.first + index];
}

/// Writes a shop entry's option: its name and price.
/// @param index  the entry
/// @param out    the option's codes
void shop_option(int index, text::Code* out)
{
   const uint16_t item = shop_item(index);
   const inventory::ItemInfo* in = inventory::info(item);
   option_text(out, item_name(item), in != nullptr ? in->price : 0, true);
}

/// The move a learn question's slot holds.
/// @param slot  move slot
/// @return its name, or nullptr for an empty slot
const text::Code* slot_move(int slot)
{
   const uint8_t moves_role = monster::id_of(monster::Role::Moves);
   const int32_t m = party::role_get(g.member, moves_role, static_cast<uint8_t>(slot));
   return m >= 0 && m < battle::kData.move_count ? battle::kData.moves[m].name : nullptr;
}

/// Writes a learn question's option: the slot's move.
/// @param index  move slot
/// @param out    the option's codes
void learn_option(int index, text::Code* out)
{
   option_text(out, slot_move(index), 0, false);
}

} // namespace

void attach(inventory::Bag* bag)
{
   g.bag = bag;
}

inventory::Bag* bag()
{
   return g.bag;
}

CAPSULE_WEAK bool give(uint16_t item, int32_t n)
{
   return g.bag != nullptr && n > 0 && inventory::add(*g.bag, item, static_cast<uint32_t>(n));
}

CAPSULE_WEAK bool take(uint16_t item, int32_t n)
{
   return g.bag != nullptr && n > 0 && inventory::remove(*g.bag, item, static_cast<uint32_t>(n));
}

CAPSULE_WEAK int32_t count(uint16_t item)
{
   return g.bag != nullptr ? static_cast<int32_t>(inventory::count(*g.bag, item)) : 0;
}

void bag_open()
{
   int total = 0;
   for (uint8_t p = 0; g.bag != nullptr && p < inventory::kInventory.pocket_count; ++p)
   {
      int used = 0;
      (void)inventory::pocket_slots(*g.bag, p, used);
      total += used;
   }
   g.menu = total != 0 ? Menu::Bag : Menu::None;   // nothing to choose: bag_poll() answers "none" at once
   if (total != 0)
   {
      menu::open(g.page, total, bag_option);
   }
}

int32_t bag_poll()
{
   if (g.menu != Menu::Bag)
   {
      return 0;
   }
   const int32_t a = menu::poll(g.page);
   if (a == menu::kWaiting)
   {
      return -1;
   }
   g.menu = Menu::None;
   const inventory::BagSlot* s = a >= 0 ? bag_entry(a) : nullptr;
   return s != nullptr ? s->item : 0;
}

void shop_open(uint8_t shop, VarId money)
{
   if (shop >= inventory::kInventory.shop_count || inventory::kInventory.shops[shop].count == 0)
   {
      g.menu = Menu::None;
      return;
   }
   g.shop = shop;
   g.money = money;
   g.menu = Menu::Shop;
   menu::open(g.page, inventory::kInventory.shops[shop].count, shop_option);
}

bool shop_poll()
{
   if (g.menu != Menu::Shop)
   {
      return false;
   }
   const int32_t a = menu::poll(g.page);
   if (a == menu::kWaiting)
   {
      return true;
   }
   if (a == menu::kBack)
   {
      g.menu = Menu::None;   // left the shop
      return false;
   }
   // one of the item, when the money covers it and the bag has room; the list stays open on its page
   const uint16_t item = shop_item(a);
   const inventory::ItemInfo* in = inventory::info(item);
   const int32_t price = in != nullptr ? static_cast<int32_t>(in->price) : 0;
   if (in != nullptr && var(g.money) >= price && give(item, 1))
   {
      add_var(g.money, -price);
   }
   menu::show(g.page, a);
   return true;
}

void learn_open(uint8_t member, uint16_t move)
{
   (void)move;
   g.member = member;
   g.menu = Menu::Learn;
   menu::open(g.page, battle::move_slots(), learn_option);   // Back: keep every move
}

int16_t learn_poll()
{
   if (g.menu != Menu::Learn)
   {
      return battle::kKeepMoves;
   }
   const int32_t a = menu::poll(g.page);
   if (a == menu::kWaiting)
   {
      return -1;
   }
   g.menu = Menu::None;
   return a >= 0 ? static_cast<int16_t>(a) : static_cast<int16_t>(battle::kKeepMoves);
}

} // namespace capsule::script::items
