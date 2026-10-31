# Map Sandbox (sample project)

The overworld: walk a small world of shape-drawn maps (a showcase of every map feature, a meadow, a hut and a
cave) with scripts, signs, people, warps, encounters, timed field effects and field actions, a debug overlay and a
collision view. It has no battles: an encounter is shown on the overlay. The engine builds it as
`capsule_sandbox_map`, one of the release's sample downloads; [the engine's docs/APPS.md](https://github.com/TheWildDeadHero/CapsuleCreatures_Engine/blob/main/docs/APPS.md#map-sandbox)
describes it in full.

## Controls

| GBA button | Desktop key | What it does |
|---|---|---|
| D-pad | arrow keys | walk |
| A | X | talk, read a sign, a field action |
| B | Z | hold to run |
| START | Enter | the menu (go to a map, night, the vehicle class, the Ward charm, field actions, flight, escape) |
| SELECT | Backspace | the debug overlay while walking; the About page in the START menu |
| L / R | Q / W | collision view / noclip |

## As a project of your own

In the Studio, **Open a sample...** (start window or the Studio menu) copies this sample into a folder you choose
and opens it on its World page; on the command line, `capsule-studio sample copy sandbox_map DEST`. The copy is a
game like any other:

- `reference/project.json`: the game's name (the folder's name, in build form), its ROM header (title from the
  name, game code `CNEW`), `src/sandbox_map.cpp` and the engine library it links (`test_kit`, which the engine
  builds for every game);
- `data/world.json` with `data/tilesets/`, `data/maps/` and `data/encounters/`: the world (the Studio's World
  page) and `data/banners/`; `data/scripts/`: the events' scripts (the Scripts page); `data/assets.json`: the
  walker's and the people's sprites, the hut's door and the cave's preview (the Assets page);
- `data/starter/`: the copy's own Starter battle data (the party, the Ward charm, the field actions), copied from
  the engine's `samples/starter/`; the world, the scripts and the project manifest point at it;
- `src/sandbox_map.cpp`: the scene (the walker, the overlay, the menu).

Build and run it from the umbrella checkout with `./build gba-release --game DEST` (the ROM is
`DEST/build/gba-release/capsule_<name>.gba`) or `./build desktop-release --game DEST`.
