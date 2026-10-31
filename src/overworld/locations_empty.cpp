/// @file
/// The empty default of overworld::kLocations, for games whose world has no
/// locations (a generated world.cpp defines its own and this file is not
/// linked then).
#include "capsule/overworld/locations.h"

namespace capsule::overworld
{

const Locations kLocations = {nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 1, {0, 0}};

} // namespace capsule::overworld
