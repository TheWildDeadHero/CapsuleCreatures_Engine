/// @file
/// Paged menus over the script port's choice menu (see capsule/script/menu.h).
#include "capsule/script/menu.h"

#include "capsule/script/port.h"

namespace capsule::script::menu
{

namespace
{

/// Entries a page holds when the list does not fit one menu (the last option turns the page).
constexpr int kPageEntries = kMaxChoices - 1;

} // namespace

void show(Paged& m, int first)
{
   const bool paged = m.total > kMaxChoices;
   m.first = paged ? (first >= 0 && first < m.total ? first - first % kPageEntries : 0) : 0;
   const int room = paged ? kPageEntries : kMaxChoices;
   const int left = m.total - m.first;
   m.shown = static_cast<uint8_t>(left < room ? left : room);
   for (uint8_t k = 0; k < m.shown; ++k)
   {
      m.fill(m.first + k, m.codes[k]);
      m.options[k] = m.codes[k];
   }
   m.more = paged;
   uint8_t count = m.shown;
   if (m.more)
   {
      m.options[count++] = kMoreText.codes;
   }
   m.open = true;
   port::choice_open(m.options, count, count);   // Back: the index past the options
}

void open(Paged& m, int total, Fill fill)
{
   m.fill = fill;
   m.total = total;
   m.open = false;
   if (total > 0 && fill != nullptr)
   {
      show(m, 0);
   }
}

int32_t poll(Paged& m)
{
   if (!m.open)
   {
      return kBack;
   }
   const int16_t a = port::choice_poll();
   if (a < 0)
   {
      return kWaiting;
   }
   if (m.more && a == m.shown)
   {
      // the next page (after the last: the first)
      const int next = m.first + kPageEntries;
      show(m, next < m.total ? next : 0);
      return kWaiting;
   }
   m.open = false;
   return a < m.shown ? m.first + a : kBack;
}

} // namespace capsule::script::menu
