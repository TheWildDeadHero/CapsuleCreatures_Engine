/// @file
/// The engine's default storage rules: no boxes. A build with battle data links its generated table
/// (tools/battlec/storage.py, in the generated battle.cpp) instead, which defines the symbol in this file and so
/// keeps it out of the link.
#include "capsule/storage/storage.h"

namespace capsule::storage
{

const StorageData kStorage = {};

} // namespace capsule::storage
