/// @file
/// The engine's default inventory tables: no pockets, items or shops. A build with battle data links its
/// generated table (tools/battlec/inventory.py, in the generated battle.cpp) instead, which defines the symbol in
/// this file and so keeps it out of the link.
#include "capsule/inventory/data.h"

namespace capsule::inventory
{

const InventoryData kInventory = {};

} // namespace capsule::inventory
