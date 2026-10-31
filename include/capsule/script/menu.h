/// @file
/// Paged menus over the script port's choice menu (C6, review4 finding 12): a list longer than one menu holds
/// (kMaxChoices) is shown a page at a time. A page holds kMaxChoices - 1 entries and a last option (kMoreText) that
/// turns to the next page (after the last page: the first); a list that fits shows whole, without it. Back answers
/// kBack. The default items and boxes ports (the bag menu, shop lists, the learn question, the boxes menu) use it;
/// a game's own port may too.
///
/// The list is the caller's: Paged::fill writes entry k's option text (a plain function pointer).
#pragma once

#include "capsule/script/data.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace capsule::script::menu
{

/// Codes per option (its text and end).
inline constexpr int kOptionCodes = 24;
/// poll(): still waiting.
inline constexpr int32_t kWaiting = -1;
/// poll(): Back was pressed.
inline constexpr int32_t kBack = -2;
/// The text of the "next page" option (the engine port's ASCII page, like its YES / NO; a game's port may draw its
/// own).
inline constexpr auto kMoreText = text::encode("More...");

/// Writes one entry's option text.
/// @param index  the entry (0 to Paged::total - 1)
/// @param out    kOptionCodes codes (ends with kCodeEnd)
using Fill = void (*)(int index, text::Code* out);

/// One paged menu (the caller's storage; one open at a time per menu).
struct Paged
{
   Fill fill;                                     ///< Writes an entry's text.
   int total;                                     ///< Entries in the list.
   int first;                                     ///< The page's first entry.
   uint8_t shown;                                 ///< Entries on the page.
   bool more;                                     ///< The page ends with the "next page" option.
   bool open;                                     ///< Waiting for an answer.
   text::Code codes[kMaxChoices][kOptionCodes];   ///< The page's option texts.
   const text::Code* options[kMaxChoices];        ///< Pointers to them.
};

/// Opens a menu on a list's first page (nothing opens for an empty list: poll() answers kBack).
/// @param m      the menu
/// @param total  entries
/// @param fill   writes an entry's text
void open(Paged& m, int total, Fill fill);

/// Shows the page holding an entry again (after the list changed, e.g. a shop's purchase).
/// @param m      the menu
/// @param first  the page's first entry
void show(Paged& m, int first);

/// The answer.
/// @param m  the menu
/// @return kWaiting, kBack, or the chosen entry (0 to total - 1)
int32_t poll(Paged& m);

} // namespace capsule::script::menu
