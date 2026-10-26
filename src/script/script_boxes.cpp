/// @file
/// The engine's default boxes port (capsule/script/port.h namespace boxes, G3): the box statements move records
/// between the party and the storage given to attach(); the boxes menu is the port's choice menu (the text box)
/// with each box's name and how many records it holds, a page at a time beyond one menu (C6: capsule/script/menu.h).
/// A game replaces any of them with its own screens by defining them.
#include "capsule/core/memory.h"
#include "capsule/script/menu.h"
#include "capsule/script/port.h"
#include "capsule/script/script.h"
#include "capsule/storage/storage.h"

namespace capsule::script::boxes
{

namespace
{

/// Codes per menu option (name, gap, number, end).
constexpr int kOptionCodes = menu::kOptionCodes;
/// Codes between a name and its count.
constexpr int kGapCodes = 2;
/// Decimal base of the counts shown.
constexpr int kDecimalBase = 10;
/// Most digits a count shown has (a box's slots).
constexpr int kMaxDigits = 5;
/// The fewest members a deposit leaves in the party.
constexpr uint8_t kPartyKeeps = 1;

/// The defaults' state.
struct Port
{
   storage::Storage* storage;                       ///< attach()'s storage.
   monster::Party* party;                           ///< attach()'s party.
   bool open;                                       ///< The boxes menu is open.
   menu::Paged page;                                ///< Its pages (C6).
};

/// The state (zero at start-up; EWRAM on the GBA: menus are not speed-critical).
CAPSULE_EWRAM_BSS Port g;

/// A box's option: its name and how many records it holds.
/// @param box  box index
/// @param out  the option's codes
void option_text(int box, text::Code* out)
{
   const text::Code* name = storage::box_name(box);
   int n = 0;
   for (; name != nullptr && name[n] != text::kCodeEnd && n < kOptionCodes - kMaxDigits - kGapCodes - 1; ++n)
   {
      out[n] = name[n];
   }
   for (int k = 0; k < kGapCodes; ++k)
   {
      out[n++] = text::kCodeSpace;
   }
   int number = g.storage != nullptr ? storage::box_count(*g.storage, box) : 0;
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
   out[n] = text::kCodeEnd;
}

} // namespace

void attach(storage::Storage* st, monster::Party* party)
{
   g.storage = st;
   g.party = party;
}

storage::Storage* attached()
{
   return g.storage;
}

int32_t count()
{
   return g.storage != nullptr ? storage::count(*g.storage) : 0;
}

bool deposit(int32_t member)
{
   monster::Party* p = g.party;
   if (g.storage == nullptr || p == nullptr || member < 0 || member >= p->count || p->count <= kPartyKeeps)
   {
      return false;
   }
   if (storage::deposit(*g.storage, p->members[member]) == storage::kNoSlot)
   {
      return false;
   }
   // the members after it move up one
   for (int k = member; k + 1 < p->count; ++k)
   {
      p->members[k] = p->members[k + 1];
   }
   --p->count;
   return true;
}

bool withdraw(int32_t slot)
{
   monster::Party* p = g.party;
   if (g.storage == nullptr || p == nullptr || p->count >= monster::kLayout.party_size ||
       p->count >= monster::kMaxPartySize || !storage::used(*g.storage, slot))
   {
      return false;
   }
   if (!storage::withdraw(*g.storage, slot, p->members[p->count]))
   {
      return false;
   }
   ++p->count;
   return true;
}

void open()
{
   const int boxes = g.storage != nullptr ? storage::kStorage.box_count : 0;
   g.open = boxes != 0;
   if (g.open)
   {
      menu::open(g.page, boxes, option_text);   // every box, a page at a time; Back: none
   }
}

int32_t poll()
{
   if (!g.open)
   {
      return 0;   // no boxes: "none" at once
   }
   const int32_t a = menu::poll(g.page);
   if (a == menu::kWaiting)
   {
      return -1;
   }
   g.open = false;
   return a >= 0 ? a + 1 : 0;
}

} // namespace capsule::script::boxes
