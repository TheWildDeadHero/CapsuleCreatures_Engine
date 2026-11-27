/// @file
/// The engine's default record layout: no roles held (D-36 §3.1 G0: split from battle_empty.cpp). A build with
/// battle data (tools/battle_compiler.py) or a project's own "monster_layout" (tools/layout_compiler.py) links its
/// generated kLayout instead, which keeps this file out of the link.
#include "capsule/monster/monster.h"

namespace capsule::monster
{

namespace
{
/// No roles held.
constexpr uint16_t kNoOffsets[static_cast<int>(Role::NUM_ROLES)] = {
   kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent,
   kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent};
static_assert(sizeof(kNoOffsets) / sizeof(kNoOffsets[0]) == static_cast<int>(Role::NUM_ROLES));
} // namespace

const Layout kLayout = {kNoOffsets, nullptr, nullptr, nullptr, 0, 0, 0, 0, 0, 0, 0, 0, {0}, 0};

} // namespace capsule::monster
