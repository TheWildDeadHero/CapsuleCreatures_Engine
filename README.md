# CapsuleCreatures Engine

Clean-room engine for monster-capture RPGs. One codebase builds a Game Boy
Advance ROM (devkitARM + libgba, with the engine's own register-level HAL),
desktop builds for Linux, macOS and Windows (SDL2), and Dreamcast builds
(KallistiOS). Every target other than the GBA runs the same software emulation
of the GBA video model, so all of them show the same image.

This repository is the **engine core** only: generic mechanisms, the platform
layer (HAL) and the asset compilers (`tools/`). The editors are in the
toolkit (`CapsuleCreatures_Toolkit`). A game's rules and content live in the
game's own repository; this one holds no third-party game content, rules or
assets. It ships the **Starter sample** (`samples/starter/`), a small neutral
game with its own creatures, numbers and shape art, which new games copy.

## Start here

New to the engine? The umbrella repository's
[getting-started guide](https://github.com/TheWildDeadHero/CapsuleCreatures/blob/main/docs/GETTING_STARTED.md)
goes from a download or a clean clone to the samples running on a GBA
emulator, a flash cart and the desktop. Changes in each release are in
<a href="CHANGELOG.md">CHANGELOG.md</a>.

## Documentation

| Topic | What it covers |
|---|---|
| <a href="docs/BUILDING.md">Building and targets</a> | toolchains, the CMake presets, VS Code tasks, what each target builds, byte order |
| <a href="docs/ARCHITECTURE.md">Architecture and modules</a> | the folder layout, VRAM uploads, memory placement, random streams, input targets, text and code pages |
| <a href="docs/DATA_FORMATS.md">Data and manifest formats</a> | sprite frames, worlds, tilesets and maps, encounters, field rules and actions, map events, objects, eventing, locations |
| <a href="docs/BATTLE.md">Battle</a> | the battle engine, creature records, battle data and rulesets, the Starter sample and its rules, the bag, item use, the save image, growth |
| <a href="docs/SCRIPTS.md">Scripts and sequences</a> | the script language, flags and variables; the sequence player |
| <a href="docs/AUDIO.md">Audio</a> | sound output per target and the audio renderer |
| <a href="docs/APPS.md">Samples, sandboxes and test apps</a> | the Starter battle, the map, sequence and battle sandboxes, the test apps |
| <a href="docs/TOOLS_AND_CI.md">Tools and CI</a> | the battle bench, using the engine as a subproject, the checks and CI jobs |
| <a href="docs/CONTRIBUTING.md">Contributing</a> | clean-room code, conventions (Doxygen, sandboxes, editor support, app layout), where changes are made |
| <a href="docs/CODE_MAP.md">Code map</a> | where each part of the code lives (`CODE_MAP.md` at the root is the generated file list) |
| <a href="samples/starter/README.md">Starter sample</a> | the Starter's rules table and how to change it |

Older notes and code comments refer to sections of this README by name. They
are now here:

| Section | Now in |
|---|---|
| Layout; Video uploads, wait states and seeding; Input targets; Text and code pages | <a href="docs/ARCHITECTURE.md">Architecture and modules</a> |
| First-time setup; Building; Byte order and alignment | <a href="docs/BUILDING.md">Building and targets</a> |
| Clean-room code; Conventions; Working copies | <a href="docs/CONTRIBUTING.md">Contributing</a> |
| Sprite frames from images; Overworld maps (world, tileset and map manifests, encounters, encounter leads, field actions, map events, map objects, eventing, locations) | <a href="docs/DATA_FORMATS.md">Data and manifest formats</a> |
| Battles (creature records, battle data and rulesets); Starting a new game from the Starter sample; Starter rules; Inventory, item use and the save image; Growth | <a href="docs/BATTLE.md">Battle</a> |
| Battle bench; The reference-ruleset repository | <a href="docs/TOOLS_AND_CI.md">Tools and CI</a> |
| Starter battle; Battle sandbox; Map sandbox; Test apps; Sequence sandbox | <a href="docs/APPS.md">Samples, sandboxes and test apps</a> |
| Scripts, flags and variables; Sequences | <a href="docs/SCRIPTS.md">Scripts and sequences</a> |
| Audio output | <a href="docs/AUDIO.md">Audio</a> |

## Credits and licenses

The engine is licensed under the Mozilla Public License 2.0 (`LICENSE`).
`THIRD_PARTY_NOTICES.md` lists the third-party parts each target uses and
their licences.

Each target links third-party code; this is what a game built on the engine
takes on per target. All are permissive or carry a static-linking exception,
so no target requires shipping object files or relinking support.

| Target | Library | License | What a shipped game must do |
|---|---|---|---|
| GBA | libgba (devkitPro) | LGPL with a static-linking exception | state its use, e.g. "based in part on the work of devkitPro (https://devkitpro.org)" |
| Desktop | SDL2 | zlib | nothing required (credit appreciated) |
| Dreamcast | KallistiOS | KOS License (BSD-style) | attribution (KallistiOS: "attribution is not optional"; see its `doc/LICENSE.md`) |

The GBA build is based in part on the work of devkitPro (https://devkitpro.org):
it links libgba and uses devkitARM's startup code. Desktop builds link SDL2
(https://libsdl.org). Dreamcast builds link KallistiOS
(https://github.com/KallistiOS/KallistiOS).
