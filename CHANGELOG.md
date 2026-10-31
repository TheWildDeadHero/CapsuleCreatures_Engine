# Changelog

All notable changes to the CapsuleCreatures Engine. Versions are calendar
versions, `YYYY.MM.DD.MMMM` (the UTC date and the minutes since midnight).

## 2026.10.31.0990: first public release

The first public release of the engine, licensed under the Mozilla Public
License 2.0.

### Targets

- Game Boy Advance ROMs (devkitARM and libgba, with the engine's own
  register-level platform layer), desktop programs for Linux, Windows and
  macOS (SDL2, linked statically), a headless build for tests, a big-endian
  check, and Dreamcast builds (KallistiOS; source only in this release).
- Every target outside the GBA runs the same software model of the GBA's
  video hardware, so all of them show the same picture.
- CMake presets for every target, VS Code tasks, and a ROM size check
  against the cartridge size a game names.
- Games outside the engine (such as the toolkit's New game) build with the
  same presets through the cache list `CAPSULE_GAMES`
  ([Building a game](docs/BUILDING.md#building-a-game)).

### Systems

- Core: fixed-point and strong types, saturating values, little-endian data
  access, random streams with a replaceable generator, no heap and no
  floating point in engine code.
- Graphics: palettes with fades, backgrounds, sprites, the VRAM upload queue,
  windows and blending; text with code pages, fonts, text layers and the
  text box; screens and their transitions.
- Overworld: tile maps with up to four layers and an attribute layer,
  elevations, movement modes, encounters, map events, map objects with move
  routes, locations with name banners, warp transitions, field actions,
  darkness, flight points and escape points.
- Scripts: a compiled script language with flags, variables, map triggers
  and placeholders; the sequence player for titles, cutscenes and credits.
- Battle: data-driven battles (1 vs 1, 2 vs 2, 3 vs 3) with the game's own
  ruleset, formulas, pipelines and effect logic; creature records and their
  transfer; recruiting and storage boxes; the bag, item use and the save
  image; growth (transformations, forms, training values); AI.
- Audio: songs, instruments, samples and sound effects on the GBA's sound
  hardware, with an integer model of it on the desktop and in tests.
- Input: logical actions with a button map per target.

### Samples and tools

- The **Starter** sample (`samples/starter`): a complete neutral battle game
  with its own creatures, moves, rules and simple-shape art, which new games
  copy. Its battle program, Starter Battle, has an About page with the
  credits.
- Sandboxes and test apps for every module, including the Map Sandbox.
- Starter Battle and the Map Sandbox are also sample projects (`sample.json`
  in their folders): the toolkit copies one into a folder of the user's as a
  game of its own, with its own copy of the Starter data, which builds with
  `CAPSULE_GAMES` ([Sample projects](docs/APPS.md#sample-projects)). A test
  keeps them copyable.
- Asset, map, script, battle, audio and sequence compilers in Python, and the
  battle bench for scripted battle scenarios.

### Documentation and checks

- The documentation is split into topic pages under `docs/`, indexed from the
  README.
- CI on every push: principles, style, code map, Doxygen, headless tests,
  Linux (GCC and Clang), macOS universal, Windows (MinGW-w64), big-endian,
  GBA and Dreamcast.
- `LICENSE` (MPL-2.0) and `THIRD_PARTY_NOTICES.md` with the notices of every
  part a build links.
