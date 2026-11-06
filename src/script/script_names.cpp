/// @file
/// C4: the engine's default for the Name op's kinds (capsule/script/port.h game::name_text()): the battle data's item,
/// species and move names (battle::kData; an item's count changes nothing: the engine has no plural rule), a party
/// slot's nickname (party::role_get() of the Nickname role), and nothing for a game's own kinds (kFirstGameNameKind
/// on). Weak (CAPSULE_WEAK): a game with its own tables or kinds defines name_text() itself.
#include "capsule/battle/data.h"
#include "capsule/core/memory.h"
#include "capsule/monster/monster.h"
#include "capsule/script/port.h"

namespace capsule::script::game
{
namespace
{

/// Copies a name's codes (no kCodeEnd).
/// @param t         the name, or nullptr (none)
/// @param out       where they go
/// @param capacity  most codes
/// @return codes written
int copy_name(const text::Code* t, text::Code* out, int capacity)
{
   int n = 0;
   for (; t != nullptr && t[n] != text::kCodeEnd && n < capacity; ++n)
   {
      out[n] = t[n];
   }
   return n;
}

/// A party slot's nickname through the party port (its Nickname role, code by code until an end code or 0).
/// @param slot      party slot
/// @param out       where the codes go
/// @param capacity  most codes
/// @return codes written
int nickname(int32_t slot, text::Code* out, int capacity)
{
   if (slot < 0 || slot > UINT8_MAX)
   {
      return 0;
   }
   const auto role = static_cast<uint8_t>(monster::Role::Nickname);
   int n = 0;
   for (; n < capacity && n < monster::kMaxNicknameCodes; ++n)
   {
      const int32_t code = party::role_get(static_cast<uint8_t>(slot), role, static_cast<uint8_t>(n));
      if (code == 0 || code == text::kCodeEnd)
      {
         break;
      }
      out[n] = static_cast<text::Code>(code);
   }
   return n;
}

} // namespace

CAPSULE_WEAK int name_text(uint8_t kind, int32_t id, int32_t count, bool counted, text::Code* out, int capacity)
{
   (void)count;     // the engine's names have no plural form
   (void)counted;
   const auto& d = battle::kData;
   switch (static_cast<NameKind>(kind))
   {
   case NameKind::Item:
      return copy_name(id >= 0 && id < d.item_count ? d.items[id].name : nullptr, out, capacity);
   case NameKind::Species:
      return copy_name(id >= 0 && id < d.species_count ? d.species[id].name : nullptr, out, capacity);
   case NameKind::Move:
      return copy_name(id >= 0 && id < d.move_count ? d.moves[id].name : nullptr, out, capacity);
   case NameKind::Nickname:
      return nickname(id, out, capacity);
   case NameKind::NUM_NAME_KINDS:
      break;
   }
   return 0;   // a game's own kind: the game names it
}

} // namespace capsule::script::game
