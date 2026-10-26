/// @file
/// The Starter sample played (E9, D-25): the battle engine (capsule::battle) on the engine's neutral sample game
/// (samples/starter) with the battle sandbox kit's minimal presentation (apps/common/battle_kit.h), without the
/// rest of a game.
///
/// Laid out like a game: the engine library, reference/project.json naming the sample's files by relative path
/// (battle.json + structure.json + ruleset.json + effects/: nine stats, HP / MP / EP, nine types, invented
/// creatures, moves, talents, charms, rivals and a presenter; assets.json: creature and presenter sprites and the
/// backdrop, shapes only) and this configuration: the starting party, the formats and the unowned scenarios.
/// Everything else the kit shows comes from the data (8 move slots paged four at a time, each move's EP or MP
/// cost, the battler's MP and EP, the free fallback Strike). G3: a bag of Orbs and Tonics (BAG; an Orb recruits an
/// unowned creature into the party, else the storage boxes the menu's "Storage boxes" page shows).
#include "battle_kit.h"

#include "battle_ids.h"
#include "capsule/core/memory.h"
#include "capsule/storage/storage.h"

namespace
{

using capsule::battle_kit::GameConfig;
using capsule::battle_kit::StartItem;
using capsule::battle_kit::StartMember;

/// @name The starting party's levels (the lead a little ahead of the rest)
///@{
constexpr uint8_t kLeadLevel = 14;     ///< The first member.
constexpr uint8_t kSecondLevel = 13;   ///< The second member.
constexpr uint8_t kBenchLevel = 12;    ///< The others.
///@}

/// @name Unowned creatures' levels
///@{
constexpr int32_t kUnownedMinLevel = 5;    ///< Lowest.
constexpr int32_t kUnownedMaxLevel = 13;   ///< Highest.
///@}

/// Unowned creatures the AI vs AI exhibition meets (the 2 vs 2 format's foes).
constexpr uint8_t kExhibitionFoes = 2;

/// The starting party: one creature of four of the Starter's types.
constexpr StartMember kStart[] = {{battle_ids::SPECIES_EMBERKIT, kLeadLevel},
                                  {battle_ids::SPECIES_TIDEPUP, kSecondLevel},
                                  {battle_ids::SPECIES_VOLTMINK, kBenchLevel},
                                  {battle_ids::SPECIES_STARMOTE, kBenchLevel}};

/// @name The starting bag (G3: Orbs recruit, Tonics heal in battle)
///@{
constexpr uint16_t kStartOrbs = 10;     ///< Orbs.
constexpr uint16_t kStartTonics = 5;    ///< Tonics.
///@}

/// The starting bag.
constexpr StartItem kBag[] = {{battle_ids::ITEM_ORB, kStartOrbs}, {battle_ids::ITEM_TONIC, kStartTonics}};

/// The storage boxes' bytes, sized from the Starter's manifest (8 boxes of 30; C6: not the platform's 32 KB).
CAPSULE_EWRAM_BSS capsule::storage::Buffer<battle_ids::STORAGE_BYTES, battle_ids::STORAGE_USED_BYTES> g_box_bytes;
/// Their view.
capsule::storage::Storage g_boxes = capsule::storage::view(g_box_bytes);

/// The Starter's sandbox.
constexpr GameConfig kGame = {"STARTER BATTLE",
                              kStart,
                              sizeof(kStart) / sizeof(kStart[0]),
                              battle_ids::PARTY_DUO_ASH_BLY,
                              battle_ids::FORMAT_SINGLE,
                              battle_ids::FORMAT_DOUBLE,
                              battle_ids::FORMAT_DOUBLE,
                              kExhibitionFoes,
                              kUnownedMinLevel,
                              kUnownedMaxLevel,
                              "Unowned battle",
                              "Unowned battle, 2 vs 2",
                              "Double: AI vs AI",
                              kBag,
                              sizeof(kBag) / sizeof(kBag[0]),
                              &g_boxes};

} // namespace

/// Runs the sandbox.
/// @return 0
int main()
{
   return capsule::battle_kit::run(kGame);
}
