/// @file
/// Eggs (D-28 section 3, package B2): an egg is an ordinary record whose egg role is set, with a hatch counter role
/// holding the steps left (capsule/combine/data.h EggData, structure `eggs`; tools/battlec/nursery.py). Party, boxes
/// and the save image need no record type of their own.
///
/// - A combination method that delivers eggs makes its result one (make_egg(): the counter starts at the result
///   species' hatch cycles field times the method's cycle_steps).
/// - The egg cycle is a step effect (EggData::step_effect): each run takes its `every` steps off every egg's
///   counter, or what the `hatch` effect-logic hook makes of them (capsule/battle/step_effects.h runs it: a Flame
///   Body-style passive doubles them), and an egg whose counter reaches 0 hatches: hatch() clears its egg role and
///   counter, derives its stats (full resources) and the game's hatch script runs (Bank::hatch_script; the
///   nickname prompt through the script port).
/// - The systems that use a record refuse an egg unless their manifest flag allows it (allowed(); items carry
///   capsule::inventory::kItemEggs, field actions their own bits, combination methods kAllowEggs). C9: the encounter
///   lead rules (EggUse::Lead), transformations (EggUse::Transform) and each step effect's handlers
///   (step_allows_eggs(); the egg cycle counts eggs down anyway); a transferred egg a layout without an egg role
///   receives is refused unless transfer_hatches_eggs().
/// - On screen an egg shows the global name and sprite, or a species field's look (egg_look()).
#pragma once

#include "capsule/combine/data.h"
#include "capsule/monster/monster.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace capsule::combine
{

/// A system that may take eggs (its manifest flag).
enum class EggUse : uint8_t
{
   Battle,      ///< Battle setup (structure rules.allow_eggs): an egg is never sent in.
   Recruit,     ///< Recruitment (structure recruit.allow_eggs).
   Lead,        ///< C9: the encounter lead and the hooks' member (structure lead.allow_eggs).
   Transform,   ///< C9: growth::Change, a script's transform (structure transformation.allow_eggs).
};

/// Whether the build has eggs.
/// @return true when the structure declares them
bool eggs_on();

/// Whether a record is an egg.
/// @param rec  the record
/// @return true when its egg role is set
bool is_egg(const monster::Record& rec);

/// The steps until an egg hatches.
/// @param rec  the record
/// @return its hatch counter (0 for a record that is not an egg)
uint32_t hatch_left(const monster::Record& rec);

/// The hatch counter an egg of a species starts with for a method.
/// @param species      species index
/// @param cycle_steps  the method's steps per cycle
/// @return the species' hatch cycles times them, within the counter role's range
uint32_t hatch_steps(uint16_t species, uint16_t cycle_steps);

/// Makes a record an egg (its egg role set, its counter at hatch_steps()); nothing without eggs.
/// @param rec          the record (its species set)
/// @param cycle_steps  the method's steps per cycle
/// @return false when the build has no eggs or the species is unknown
bool make_egg(monster::Record& rec, uint16_t cycle_steps);

/// Sets an egg's counter (the studio's test saves, a game's own rules).
/// @param rec    an egg
/// @param steps  steps left (clamped to the role's range)
void set_hatch_left(monster::Record& rec, uint32_t steps);

/// Takes steps off an egg's counter.
/// @param rec    the record
/// @param steps  steps
/// @return true when it is an egg whose counter reached 0 (it hatches)
bool count_down(monster::Record& rec, uint32_t steps);

/// Hatches an egg: its egg role and counter cleared, its resources full (the stats follow its level).
/// @param rec  the record
/// @return false when it was not an egg
bool hatch(monster::Record& rec);

/// Whether a system takes eggs.
/// @param use  the system
/// @return true when its flag allows them
bool allowed(EggUse use);

/// Whether an egg may do a field action (field_actions[].allow_eggs).
/// @param action  field action index
/// @return true when its flag allows eggs
bool action_allows_eggs(uint8_t action);

/// Whether a step effect's handlers run on eggs (C9: step_effects[].allow_eggs; the egg cycle itself always counts
/// eggs down).
/// @param effect  step effect index
/// @return true when its flag allows eggs
bool step_allows_eggs(uint8_t effect);

/// Whether a build without eggs receives a transferred egg hatched (C9: structure `transfer_eggs: "hatch"`).
/// @return true when it does (else such an import is refused)
bool transfer_hatches_eggs();

/// Whether a system refuses a record (an egg, and the system's flag does not allow them).
/// @param rec  the record
/// @param use  the system
/// @return true when refused
inline bool refuses(const monster::Record& rec, EggUse use)
{
   return is_egg(rec) && !allowed(use);
}

/// What an egg is called on screen.
/// @return the name, or nullptr without eggs
const text::Code* egg_name();

/// The default egg sprite.
/// @return its AssetKey value, or 0
uint32_t egg_sprite();

/// An egg's look (the egg rules' sprite_field of its species).
/// @param rec  the record
/// @return the field's value, or 0 without one
int32_t egg_look(const monster::Record& rec);

} // namespace capsule::combine
