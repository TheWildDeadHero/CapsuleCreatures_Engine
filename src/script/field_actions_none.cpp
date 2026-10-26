/// @file
/// The game state field action requirements read, in a program with the battle modules but without a script bank
/// (the tests and the bench: capsule_battle_library()): there are no references, so every `flag.NAME` and
/// `var.NAME` reads 0 (see capsule/script/field_actions.h). A program that builds the script VM lists
/// src/script/field_actions.cpp itself, which wins.
#include "capsule/growth/data.h"

namespace capsule::rules
{

int32_t field_state(int32_t ref)
{
   (void)ref;
   return 0;
}

} // namespace capsule::rules
