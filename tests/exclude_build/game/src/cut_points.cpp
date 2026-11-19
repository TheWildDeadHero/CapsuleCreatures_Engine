/// @file
/// D-36 package 3 step 3B: references the always-compiled engine code that has cut points (the save codec's boxes,
/// the overworld's location preview, the party role port), so the fixture's link pulls them in without the modules
/// they cut (tests/exclude_build). Nothing calls touch(); being linked is the test.

#include "capsule/field/field.h"
#include "capsule/overworld/overworld.h"
#include "capsule/save/save.h"
#include "capsule/script/port.h"

namespace capsule::exclude_build
{

/// Reaches each cut point's code.
void touch();

void touch()
{
   (void)save::write(save::Sources{});
   overworld::update(field::Intent{});
   (void)script::party::role_get(0, 0, 0);
}

} // namespace capsule::exclude_build
