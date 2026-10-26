/// @file
/// The engine's default asset table: empty. A build that has assets links its
/// generated table (tools/asset_compiler.py) instead, which keeps this file out
/// of the link.
#include "capsule/asset/asset.h"

namespace capsule::asset
{

const Table kTable = {nullptr, nullptr, nullptr, nullptr, nullptr, 0};

} // namespace capsule::asset
