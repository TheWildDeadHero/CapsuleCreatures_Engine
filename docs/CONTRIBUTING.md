# Contributing

Rules for changes to the engine: clean-room code, conventions and where changes are made.

## Clean-room code

The engine is written clean-room from its own design spec (D-2). It holds no
third-party game code, data, graphics, audio or text, and needs none to build
or test. `tools/ci/check_principles.sh` fails if any engine source or build
file refers to a `ref_submodules/` folder of outside reference code.

## Conventions

**Documentation (Doxygen).** Every file, namespace, type, function, enum
value, member, constant and macro in `include/`, `src/`, `apps/` and `tools/`
carries a Doxygen comment, and every documented function documents its
parameters and return value. `Doxyfile` fails on anything undocumented, and CI
runs it on every push.

- C++: `///` before a declaration (first sentence = brief), a trailing
  `///` followed by `<` after a member or enum value, `/// @file` at the top of every file, `@param` /
  `@return` / `@tparam` for functions and templates, and `/// @name` member
  groups for related declarations. Design IDs (P-6, A-3, ...) stay in the text.
- Python tools: docstrings that start with an exclamation mark (it lets
  Doxygen read `@param`, `@return` and `@throws`) and `##` comments before
  module constants and instance attributes.
- Comments follow the P-6 scan: the words for floating-point types never
  appear, even in prose.
- Build the HTML reference with the `docs` target (any preset, Doxygen 1.9.8
  or later): `build/docs/html/index.html`.

**Module sandboxes.** Each gameplay module (the sequence player for titles,
cutscenes and credits; overworld traversal; the battle system; and so on)
ships, in the same change, with a sandbox app in `apps/sandbox_MODULE/`. A
sandbox fills the module's scene setup (its static context payload) from a
test file instead of the save and game flow, enters that one scene directly,
and adds a debug overlay and controls specific to the module (for example:
walk through walls and tile inspection for the overworld; turn stepping, event
log and seeded replays for battles). Engine systems below the modules keep
their `apps/test_SYSTEM/` apps (see [Test apps](APPS.md#test-apps)).

**Editor support with every core feature.** A new core feature is not done
until it can be authored: the same change adds its editor support in
capsule-studio (a panel in `editor/capsule_studio/ui/editors/`, with the
editing logic in `capsule_studio.core` and command-line access in
`capsule_studio.cli`), next to the engine module and its sandbox app.

**Apps are laid out like games.** Every app, test or sandbox, has the same
structure as a real game built on the engine, matching the three layers
(P-13):

| Part | Where | What |
|---|---|---|
| Engine | `capsule_engine` (linked) | mechanisms only; nothing app-specific |
| Reference manifest | `apps/APP/reference/project.json` | `capsule.project/1`: name, ROM header, sources, helper libraries, the asset manifest and the sequences to compile |
| Data | `apps/APP/data/` | `capsule.assets/1` asset manifests (art drawn from simple shapes, text, lists); sequences may also be referenced from elsewhere, e.g. the engine's test manifests |
| Scene code | `apps/APP/src/APP.cpp` | the app's own code and game hooks |

`capsule_game(<dir>)` in `CMakeLists.txt` reads the project manifest and
builds everything it lists; generated files go to `generated/APP/`. A game
outside the engine (a toolkit New game) is laid out the same way and builds
through the cache list `CAPSULE_GAMES`
([Building a game](BUILDING.md#building-a-game)). Test art
is only ever simple shapes described in the manifest, so no third-party or
copyrighted material is involved.

## Working copies

This repository is the `engine/` submodule of the `CapsuleCreatures`
umbrella repository (with the toolkit as `toolkit/`; the umbrella's
[`docs/WORKSPACE.md`](https://github.com/TheWildDeadHero/CapsuleCreatures/blob/main/docs/WORKSPACE.md) describes the contributor layout, D-37). Nothing from
other projects belongs inside `engine/`. A standalone clone works too: tools
outside this repository find it through `-DCAPSULE_ENGINE=<path>` or the
`CAPSULE_ENGINE` environment variable.

## Before you send a change

From the umbrella repository:

```
./check                  # principles, style and code maps of the engine and the toolkit
./build headless --test  # the engine core and every test, no SDL needed
```

A change that adds, moves or re-purposes a file also updates its line in
[`docs/CODE_MAP.md`](CODE_MAP.md) and regenerates the root map with
`python3 tools/ci/code_map.py build`. The engine is licensed under the Mozilla
Public License 2.0 ([`LICENSE`](../LICENSE)).

