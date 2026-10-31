/// @file
/// The engine's default world: no maps. A build with maps links its generated
/// world (tools/map_compiler.py) instead, which keeps this file out of the link.
#include "capsule/map/map.h"

namespace capsule::map
{

// No elevation or vehicle class table: the default rule and kDefaultVehicle (one class, on foot).
const World kWorld = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                      0,       0,       0,       1,       0,       {0, 0, 0, 0, 0}, nullptr};

} // namespace capsule::map
