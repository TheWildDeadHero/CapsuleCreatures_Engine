/// @file
/// The battle sandbox kit: a game's battle data played with a minimal presentation (scenario menu, HUD, text box,
/// command / move / target / party menus, sprites, presenter cues through battle::presenter_view), without the rest
/// of a game. The engine's apps/starter_battle (the Starter sample) and the reference repo's sandbox_battle (the
/// Classic sample) are this kit with their own GameConfig; everything else they show comes from the battle data
/// (parties, species, moves, the resources and their costs, move slots).
///
/// The game's data sets the shape of the menus: battler pools besides the hit points (MP, EP, ...) are shown in
/// the party and command menus; the move menu pages its moves four at a time when the layout has more move slots
/// and shows the cursor move's first move resource pool (PP 10/15, ...), its battler costs (EP 5; "*" for a cost
/// formula; "free" for none), type and power.
///
/// Screen: BG3 backdrop (priority 3), sprites (priority 2), BG1 panels (priority 1), BG0 text (priority 0). One
/// sprite slot per format position (0-5); presenter_view owns slots 6-15 (presenters in front). Palette banks: 3
/// backdrop, 8-11 text inks, 12/14/15 HP bars, 13 panels; OBJ banks 0-5 creatures, 6-15 presenters.
///
/// G3: the command menu has BAG (the player's bag: the items used in battle, on a battler, a member or the user;
/// a recruit item's checks wobble the creature's sprite) and a recruited creature joins the party, else the storage
/// boxes; the menu's "Storage boxes" page lists them (L / R change the box).
///
/// D-40: a game may give About lines (its credits; the Starter's name the engine's licence and devkitPro's work);
/// SELECT on the menu shows them, B goes back.
///
/// Controls: Menu: Up/Down choose, A start; the party keeps its pools and experience between battles ("Heal
/// party" restores them and restocks the bag). Battle: A advances text (hold to speed it up); in the command menu D-pad + A choose,
/// B back; SELECT toggles fast text; START leaves the battle (back to the menu).
///
/// The kit is the library capsule_battle_kit (a game's project.json lists "battle_kit" with "test_kit"); it reads
/// the game's battle data through battle::kData and its assets through the asset table, so one build of it serves
/// every game of a platform.
#pragma once

#include <cstdint>

namespace capsule::storage
{
struct Storage;
} // namespace capsule::storage

namespace capsule::battle_kit
{

/// One member of the starting party.
struct StartMember
{
   uint16_t species;   ///< Species index (the game's battle_ids SPECIES_).
   uint8_t level;      ///< Its level.
};

/// One item of the starting bag (G3).
struct StartItem
{
   uint16_t item;    ///< Item id (the game's battle_ids ITEM_).
   uint16_t count;   ///< How many.
};

/// What a game gives the kit: its title, starting party, formats, the unowned scenarios and its starting bag.
/// Formats, parties and items are the game's battle_ids values.
struct GameConfig
{
   const char* title;                ///< Menu title.
   const StartMember* start;         ///< The starting party, in order.
   uint8_t start_count;              ///< Its members.
   uint16_t double_party;            ///< The data party fought 2 vs 2 (shown "(2 vs 2)"), or 0xFFFF for none.
   uint8_t single_format;            ///< Format of 1 vs 1 battles.
   uint8_t double_format;            ///< Format of 2 vs 2 battles.
   uint8_t exhibition_format;        ///< Format of the AI vs AI exhibition.
   uint8_t exhibition_count;         ///< Unowned creatures the exhibition's party meets.
   int32_t unowned_min_level;        ///< Lowest level of an unowned creature.
   int32_t unowned_max_level;        ///< Highest level of an unowned creature.
   const char* unowned_label;        ///< Menu label of the 1 vs 1 unowned battle.
   const char* unowned_double_label; ///< Menu label of the 2 vs 2 unowned battle.
   const char* exhibition_label;     ///< Menu label of the exhibition.
   const StartItem* bag = nullptr;   ///< The starting bag (G3; "Heal party" restocks it), or nullptr: empty.
   uint8_t bag_count = 0;            ///< Its entries.
   /// The storage boxes recruited creatures go to, sized from the game's manifest (C6: the game declares a
   /// storage::Buffer<battle_ids::STORAGE_BYTES, battle_ids::STORAGE_USED_BYTES> and its view), or nullptr: none.
   storage::Storage* boxes = nullptr;
   /// D-40: the About page's lines (credits; at most kCols characters each, at most battle_kit::kAboutMaxLines), or
   /// nullptr: no About page.
   const char* const* about = nullptr;
   uint8_t about_count = 0;          ///< Its lines.
};

/// D-40: most About lines a page shows (the rows between the title and the controls).
inline constexpr uint8_t kAboutMaxLines = 15;

/// Runs the sandbox until the platform asks to quit.
/// @param game  the game's configuration (must outlive the call)
/// @return 0
int run(const GameConfig& game);

} // namespace capsule::battle_kit
