# Starter Battle (sample project)

The Starter game's battles, played with the battle sandbox kit's minimal presentation: three rivals (one of them a
duo in 2 vs 2), unowned creatures in 1 vs 1 and 2 vs 2, a 2 vs 2 the AI plays on both sides, "Heal party",
recruiting with Orbs and the storage boxes. The engine builds it as `capsule_starter_battle`, one of the release's
sample downloads; [the engine's docs/APPS.md](https://github.com/TheWildDeadHero/CapsuleCreatures_Engine/blob/main/docs/APPS.md#starter-battle) describes it in full.

## Controls

| GBA button | Desktop key | What it does |
|---|---|---|
| D-pad | arrow keys | move the cursor |
| A | X | choose |
| B | Z | back |
| SELECT | Backspace | the About page (on the first menu) |
| L / R | Q / W | previous / next storage box |

## As a project of your own

In the Studio, **Open a sample...** (start window or the Studio menu) copies this sample into a folder you choose
and opens it; on the command line, `capsule-studio sample copy starter_battle DEST`. The copy is a game like any
other:

- `reference/project.json`: the game's name (the folder's name, in build form), its ROM header (title from the
  name, game code `CNEW`), `src/starter_battle.cpp` and the engine libraries it links (`test_kit`, `battle_kit`,
  which the engine builds for every game);
- `data/starter/`: the copy's own Starter battle data (`battle.json`, `structure.json`, `ruleset.json`,
  `effects/`, `assets.json`), copied from the engine's `samples/starter/`; change it in the Studio's Battle page;
- `src/starter_battle.cpp`: the scene (the starting party, the bag, the menu's battles).

Build and run it from the umbrella checkout with `./build gba-release --game DEST` (the ROM is
`DEST/build/gba-release/capsule_<name>.gba`) or `./build desktop-release --game DEST`.
