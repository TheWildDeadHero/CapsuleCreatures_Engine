/// @file
/// Field actions (D-27 section 3, package F2): which party member may use one, and the dispatcher the overworld
/// asks when the walker faces a target (capsule/field/field.h set_action_rules()).
///
/// An action's requirement is one more growth rule of the battle data (kFieldRules.first_action_rule + the action's
/// index; tools/battlec/field.py): the growth `when` language over a member's record plus `knows.MOVE`, `passive`
/// and the game state `flag.NAME` / `var.NAME` (the script bank's references, capsule/script/field_actions.h). The
/// member that uses it is the first in party order for which the requirement holds.
///
/// **Facing a target** (an ActionScope attached): the first action whose target is the faced behavior or object
/// kind and that some member may use runs: the dispatcher writes the member's party slot to the script bank's
/// `field_action_member` and gives the field its script (an asking action's wrapper asks first).
///
/// **Menu actions** (target none, or a map tag the shown map carries): menu_actions() lists the ones a member may
/// use now, start_action() writes the member and gives the script for overworld::run_script().
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/field/field.h"
#include "capsule/monster/monster.h"
#include "capsule/script/data.h"

#include <cstdint>

namespace capsule::battle
{

/// One menu action a member may use now.
struct MenuAction
{
   uint8_t action;   ///< Field action index (battle_ids.h FIELD_ACTION_<NAME>).
   uint8_t member;   ///< The party slot that uses it.
};

/// The first party member a field action's requirement holds for.
/// @param action  field action index
/// @param party   the party
/// @return its party slot, or kBench (none, or no such action)
uint8_t action_member(uint8_t action, const monster::Party& party);

/// The menu actions usable now: target none, or a tag of the map (each with its member).
/// @param party  the party
/// @param map    the shown map's index (its tags)
/// @param out    room for @p most entries
/// @param most   capacity
/// @return entries written
int menu_actions(const monster::Party& party, uint16_t map, MenuAction* out, int most);

/// Prepares an action's script: writes the member (script::actions::set_member()).
/// @param action  field action index
/// @param member  its party slot
/// @return the ScriptId to run (an asking action's wrapper), or script::kNoScriptId
uint16_t start_action(uint8_t action, uint8_t member);

/// Attaches the field action dispatcher and the scripts' `field_action available` check to a party while it lives
/// (RAII: field::set_action_rules() and script::actions::attach(), then what was attached before). One at a time.
class ActionScope
{
public:
   /// Attaches them.
   /// @param party  the player's party
   explicit ActionScope(monster::Party& party);

   /// Gives back what was attached before.
   ~ActionScope();

   ActionScope(const ActionScope&) = delete;
   ActionScope& operator=(const ActionScope&) = delete;

private:
   monster::Party* previous_party_;           ///< The party attached before.
   field::ActionRules previous_rules_;        ///< The dispatcher attached before.
};

/// The dispatcher an ActionScope attaches (public for tests): the first action on the faced target a member of the
/// attached party may use.
/// @param face  what the walker faces
/// @return its ScriptId (the member written), or field::kNoScript
uint16_t dispatch_action(const field::ActionFace& face);

} // namespace capsule::battle
