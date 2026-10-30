# Architecture and modules

How the engine is laid out and how its core systems work. The folder-by-folder
code map is [CODE_MAP.md](CODE_MAP.md).

One codebase builds a Game Boy Advance ROM (devkitARM + libgba, with the
engine's own register-level HAL), desktop builds for Linux, macOS and Windows
(SDL2), and Dreamcast builds (KallistiOS). Every target other than the GBA runs
the same software emulation of the GBA video model, so all of them show the
same image.

This repository is the **engine core** only (spec P-13): generic mechanisms,
the platform layer (HAL) and the asset compilers; the editors are in the
toolkit repository. The reference ruleset and game content live in separate
repositories, so this one contains no third-party terms, rules or assets (D-2).

## Layout

```
include/capsule/core/   principle headers: POD lock, fixed point and strong types,
                        saturating math, ENGINE_ASSERT, random streams (A-4)
include/capsule/hal/    platform interface: plain functions, one implementation per target
include/capsule/text/   fonts, build-time text encoding, fixed-cell printing, text layers and text box
include/capsule/input/  logical actions, per-target input maps, shoulder intent filter (B-11)
include/capsule/asset/  asset table: backgrounds, sprites, text and lists looked up by key (A-5)
include/capsule/gfx/    graphics modules: palettes with software fade, background loader, sprite slots
include/capsule/seq/    sequence player: title flows, cutscenes, credits (data-driven), its presenter
include/capsule/monster/ creature records: frozen role registry, game-defined bit layout (C-07a)
include/capsule/battle/ battle engine (C-08): data tables, ruleset formula slots, pipelines, state, turn loop
include/capsule/encounter/ encounters (C-03d, D-12): table bank layout, slot/method lookup, rolls
include/capsule/script/ scripts (B-1, B-2): bank layout, VM, flags and variables, map triggers, port
src/asset/              asset lookup, empty default table
src/battle/             battle modules (state, turn, move, damage, effects, conditions, progress, parties, ai, text,
                        pipeline, effect VM and its host), empty default data and formulas
src/core/               global random streams, the default generator (random_default.cpp)
src/encounter/          encounter rolls (step methods via the field module, script methods via roll_at())
src/script/             script VM, the text-box port, default natives, empty default bank
src/monster/            role access over packed records
src/gfx/                graphics module implementations
src/input/              input implementation
src/text/               text implementation (glyph expansion, printing, text layers and box)
src/seq/                sequence player, its port onto the graphics modules, default game hooks
src/hal/gba/            GBA HAL: direct hardware registers via libgba
src/hal/soft/           software GBA video model (registers, VRAM, palette, compositor)
                        shared by every non-GBA HAL
src/hal/sdl2/           desktop HAL: window, frame pacing, keyboard (SDL2)
src/hal/dc/             Dreamcast HAL: full-screen PowerVR scaling, Maple controller (KallistiOS)
src/hal/headless/       headless HAL: video model only, no display or input (CI, big-endian check)
src/hal/common/         HAL parts shared by every target: the VRAM upload queue (A-5),
                        first-press seeding of the random streams (A-4), the audio step hook and
                        the sound output model of the desktop and headless audio backends (M4)
apps/<app>/             apps laid out like games: reference/project.json, data/, src/ (docs/CONTRIBUTING.md)
apps/hello/             hello world: greeting on BG0, D-pad scrolls it
apps/input_viewer/      live view of buttons -> actions and shoulder events for the target
apps/test_video/        test app: palettes, tile flips, layer priority/scroll, map sizes
apps/test_vram/         test app: the VRAM upload queue's budget, splitting and statistics; seeding
apps/test_script/       test app: the script VM with the text-box port, flags, variables and a map trigger
apps/test_text/         test app: font glyph table, inks, newlines and layer wrapping
apps/test_sequence/     test app: the sequence player's logic with a recording port and live state
apps/sandbox_sequence/  module sandbox: the sequence player full-screen with shape-art assets
apps/sandbox_map/       module sandbox: overworld traversal and map events on a shape-art world (HUD, collision view, noclip)
apps/test_battle/       test app: the battle engine's logic stepped one unit at a time, state and event log
apps/starter_battle/    the battle module's sandbox: the Starter sample played, unowned and rival battles (1v1, 2v2)
apps/test_audio/        test app: the audio tour over the test songs and clips (players, the GBA's cost per blank)
apps/sandbox_audio/     the audio module's sandbox: sounds and clips by hand, fades, pause, mono / stereo
apps/audio_render/      CLI: a sound, clip or the tour rendered headless -> ring segments, tone writes, state, WAV
samples/starter/        the Starter sample: the engine's neutral sample game and reference fixture, which new games copy
apps/common/            test_kit: the on-screen console the test and sandbox apps share
Doxyfile                API reference config; fails on anything undocumented (docs/CONTRIBUTING.md)
assets/input/           engine default input map (actions per target, chord timings)
assets/fonts/           engine default font (original artwork)
cmake/                  toolchain files (toolchains/: GBA, Dreamcast, big-endian check) and
                        the pinned SDL2 for desktop (sdl2.cmake)
.github/workflows/      CI: every platform, plus a big-endian run, on every push
tests/                  core header checks, input, sequence, software-video, VRAM queue and seeding,
                        asset, graphics, map, field, encounter, script and battle tests
tools/font_compiler.py  compiles a font asset into a packed glyph table at build time
tools/input_compiler.py compiles the input manifest into one target's InputMap table
tools/sequence_compiler.py compiles a sequence manifest into ROM tables
tools/asset_compiler.py compiles an asset manifest (shape art, PNG sprite frames, text, lists) into the asset table
tools/bitmap.py         PNG reading/writing and 15-color palettes (pure Python; shared with capsule-studio's editor)
tools/script_compiler.py compiles .cscript sources into the script bank and script_ids.h
tools/map_compiler.py   compiles a world (tilesets, maps, terrain classes, encounters) into the map tables
tools/encounter_compiler.py checks and builds a world's encounter tables (run by the map compiler)
tools/ruleset_compiler.py checks ruleset formulas and pipelines (integer expressions, overflow analysis) and emits them as C++
tools/battle_compiler.py compiles battle data (battle.json content + structure.json structure) with its ruleset
tools/battle_split.py   splits a single-file battle.json (before E5s) into battle.json + structure.json
tools/battle_resources.py migrates a battle with built-in PP (before E8) to manifest resources (D-25)
tools/setup/            one-time machine setup for Dreamcast (KallistiOS); GBA and desktop: the toolkit's installer
tools/dreamcast/        kos-run.sh: runs compiles and links inside the KallistiOS environment
tools/ci/               principle checks (no virtual dispatch, no floats, no reference code)
submodules/             libgba sources (pinned for reference; the build links devkitPro's installed libgba)
docs/                   these topic pages and CODE_MAP.md (where each part of the code lives)
```

## Video uploads, wait states and seeding (A-2, A-4, A-5)

**VRAM upload queue** (`capsule/hal/vram_queue.h`, `src/hal/common/`).
Uploads made during play are queued and land at the start of the next VBlank,
in the same VBlank as the display registers, so the screen never shows a
half-written ring edge, tile animation or palette. On the GBA the VBlank
interrupt lands them (by DMA channel 3), together with the registers and OAM,
as soon as VBlank begins: `system::update()` marks the finished frame ready
and waits, so the next frame's logic starts with every upload in place. A
frame whose logic overran is not ready in time; that VBlank changes nothing,
and the frame lands at the following one. The other targets land the queue at
the end of `system::update()` into the software video model.

- Two rings: **must-land** (palettes, tilemap rows; all of it lands at the
  next VBlank) and **streaming** (tiles, animation frames, large loads; in
  4 KB pieces, oldest first, while the VBlank budget lasts). Adjacent requests
  merge into one.
- Each request's cost is estimated in cycles from its size, access width and
  source memory (ROM, IWRAM, EWRAM). The estimates follow the GBA's
  documented timings and still need measuring on hardware; every target uses
  them, so uploads land on the same frames everywhere.
- Nothing is dropped: a full ring lands its oldest requests at once (counted
  as early landings). Must-land requests beyond the budget still land and are
  counted as overruns.
- The immediate `hal::video` functions remain for scene setup. One that
  overlaps a queued request lands the queue first, so writes keep program
  order.
- Queued data is read when it lands: it must stay valid and unchanged until
  then (ROM, or a static buffer whose latest contents should land).
- Users so far: the palette manager (the exact entries a call changes), the
  map renderer (its ring rows, mirrored in EWRAM, and its tile animations).

Build settings are per game: the `"build"` object of the game's
`reference/project.json`, checked by `tools/build_settings.py` when CMake
configures the game (the studio's project settings window edits it). The
queue and the GBA HAL are compiled into each game with its values. A
non-empty CMake cache variable of the same name overrides every game's value
(and applies to the tests that build the queue).

```json
"build": {"vram_must_land_slots": 32, "vram_streaming_slots": 64,
          "vram_budget_cycles": 67392, "gba_waitcnt": "0x4014",
          "screen_arena_bytes": 32768}
```

| Key | Cache variable | Default | Meaning |
|---|---|---|---|
| `vram_must_land_slots` | `CAPSULE_VRAM_MUST_LAND_SLOTS` | 32 | must-land ring size (power of two, 4–128) |
| `vram_streaming_slots` | `CAPSULE_VRAM_STREAMING_SLOTS` | 64 | streaming ring size (power of two, 4–128) |
| `vram_budget_cycles` | `CAPSULE_VRAM_BUDGET_CYCLES` | 83,776 − 16,384 | cycles of each VBlank for the queue; the rest is kept for register and OAM copies and, later, the audio mix |
| `gba_waitcnt` | `CAPSULE_GBA_WAITCNT` | `0x4014` | GBA ROM wait states (prefetch on, 3/1, as commercial GBA games set them); flashcarts that need slower ROM access override it |
| `screen_arena_bytes` | `CAPSULE_SCREEN_ARENA_BYTES` | 32,768 | the screen arena (`capsule/screen/arena.h`, EWRAM on the GBA): the most the screens open at once borrow (a multiple of 4, 1 KB–192 KB); an unused arena links no storage |

`CAPSULE_EWRAM_BSS` (`capsule/core/memory.h`) places a large static buffer in
the GBA's EWRAM instead of IWRAM; data there is queued with
`Source::Ewram`.

Memory placement on the GBA (32 KB IWRAM, 256 KB EWRAM):

- EWRAM (`CAPSULE_EWRAM_BSS`): large state touched a little per frame: the
  battle `State` and the `monster::Party` records (see `battle.h`, "Where State
  lives"; starter_battle and test_battle), every `seq::Player` (the
  overworld's location preview, the presenter view's, the sequence apps'),
  the palette manager's loaded colors, the map ring mirrors.
- IWRAM (the default): what the DMA or the VBlank interrupt reads every
  frame (OAM shadow, register shadow, visible palettes, the VRAM queue rings)
  and small hot state.
- IWRAM code (`CAPSULE_IWRAM_CODE`, ARM, never inlined): only the VBlank work
  that runs every frame: the VRAM queue's `flush()` / `land()` and the GBA
  HAL's `commit_registers()` (about 1.2 KB together). Check a function's size
  in the map file before adding another.
- No static constructors: field's start-up state is `constinit`, desktop's SDL
  owner is constant-initialised; the build uses `-fno-threadsafe-statics`.
- Screen state (UM, D-33): a screen's large buffers come from the screen
  arena (`capsule/screen/arena.h`: `screen::Block<T>`, last in first out, tied
  to the screen stack: a screen's blocks go back when it leaves for good)
  instead of statics, as the reference games allocate a screen's buffers from
  their heap only while it is open.
- Link checks (UM): every GBA link fails when EWRAM (`.ewram` + `.sbss`) passes
  256 KB (devkitARM's `gba_cart.ld` does not bind `.sbss` to the region, so
  objects past the end would alias low EWRAM) or IWRAM leaves less than
  `CAPSULE_GBA_STACK_RESERVE` bytes (default 2048) below `__sp_usr` for the
  stack (`cmake/gba_memory.ld`, an input script after the default one), and
  prints both regions' use (`cmake/gba_memory_report.cmake`).
  `-DCAPSULE_GBA_MEMORY_CHECKS=OFF` keeps only the report, to measure a
  program that does not fit.

**Seeding (A-4).** `system::init()` seeds the gameplay and cosmetic streams
from the platform's entropy. The first key press of the session mixes in fresh
entropy again: on the GBA the keypad interrupt reads Timer 1 (16.78 MHz,
started at init) at the cycle the key went down, so every session differs even
without a real-time clock (at boot itself, before any press, the GBA has
nothing that varies yet); a
title sequence's seed stage (`STAGE_SEED_ON_PRESS`) adds its own timing
through the default game hook. Desktop runs are reproducible with the
`CAPSULE_SEED` environment variable; headless builds use the frame count, so
tests stay deterministic. The battle stream is still seeded per battle. The
RTC joins in once its driver exists (C-05).

**Generator (A-4, D-22 decision 4).** `rng::next16()` is replaced at link
time; the ranges (`below`, `between`, `chance`), `mix` and `seed` are built on
it and the stream state is 32 bits whatever the generator. The engine default
(`src/core/random_default.cpp`, in the engine library) is a neutral permuted
LCG (PCG RXS-M-XS 32/32: full-period LCG, xorshift-multiply output, top 16
bits). Every engine test and app uses it. `capsule_rng(<target> default)` or
`capsule_rng(<target> SOURCE <file>)` (`cmake/parts/rng.cmake`) links another
generator into one program, and `capsule_rng_test(<name> SOURCE <file> SPEC
<header>)` runs the generator tests (`tests/random_tests.cpp`) on it: the
reference-ruleset repo links its compat module's Method-1 LCG (the engine's
generator before D-22) that way, so the classic ruleset's goldens keep their numbers
there (E11).

## Input targets (B-11)

The game reads logical actions; `assets/input/default_input.json` says which
buttons drive them on each target, and each build compiles its own target's
table. GBA builds use `gba`, desktop builds use `snes_layout`, Dreamcast
builds use `dreamcast`.

| Action | SNES layout (desktop) | GBA | Dreamcast |
|---|---|---|---|
| Accept | A | A | A |
| Back / run / walk | B | B | B |
| Main menu | X | Start | X |
| Device menu | Y | Select | Y |
| Toggle run | Start | main-menu entry | Start |
| Help | Select | main-menu entry | main-menu entry |
| Wheel 1 / 2 | R / L (hold) | R / L (hold) | right / left trigger (hold) |

The Dreamcast pad has no Select; its analog triggers count as R and L past
half travel. Sleep wake keys are set per target.

Holding L+R+Start for 60 frames is the sleep chord, and adding Select makes it
soft reset. Start and Select pressed inside a chord never trigger their
actions. The input compiler rejects maps that can't work on a target, for
example X or Y on GBA, a button bound twice, or a wheel that isn't on a
shoulder.

Desktop keyboard (SNES face-button positions): X = A, Z = B, S = X, A = Y,
Q = L, W = R, Enter = Start, Backspace = Select, arrows = D-pad.

## Text and code pages (C-2, D-20)

Text is Unicode in every manifest and 8-bit codes in ROM. Each language has a
**code page** (`capsule.codepage/1`) that gives each character a glyph code
0x00-0xF9, and a font with a glyph for every mapped character (a missing glyph
is a build error). The engine's default page is printable ASCII
(`assets/text/ascii.codepage.json`, codes = ASCII) with
`assets/fonts/default_8x8.txt`.

```json
{"schema": "capsule.codepage/1", "id": "en", "name": "English (ASCII)",
 "ranges": [{"first": "0x20", "chars": " !\"#$%..."}],
 "map": {"0x80": "é"}, "aliases": {"’": "'"}}
```

Every page keeps the space at 0x20 and the digits at 0x30-0x39. Text is
NFC-normalised and matched longest first (a character may be several code
points); aliases are explicit folds onto mapped characters. Codes above the
glyphs: FA prompt (typed text waits for a button), FB page break (waits, then
clears the box and goes on at its first row), FC nn control (skipped when
unknown), FD nn placeholder, FE newline, FF end. Placeholder arguments come in
two ranges that never meet: 00-7F are battle placeholders (`{user}`,
`{move}`, ... in battle texts; `capsule::text::Arg`, replaced by
`battle::format_text()`, which keeps field placeholders for the printer), 80-FE are a game's **field
placeholders** (below).
Printers that cannot wait (`text::print()`, untyped `text::layer::print()`)
skip FA and continue an FB on the next row; the script port resumes typed
text on A or B (`text::layer::waiting()` / `resume()`).

**Field placeholders** (D-22 decision 6). A game declares them in its scripts
manifest, the one manifest every field-text compiler already reads (the
script compiler; the map compiler through `--scripts` / the world's
`"scripts"`, for location names and event scripts). Each is bound to a name
slot, a variable, a term or a party record field:

```json
"placeholders": {"player": {"name": 7}, "coins": {"var": "coins"},
                 "rival": {"term": "rival"}, "lead": {"record": "nickname", "slot": 0}},
"terms": {"rival": "Rowan"}
```

With `"placeholders"` (even `{}`), script texts and location names are markup:
`{player}` encodes as FD (0x80 + its declaration index), `{prompt}` as FA,
`{page}` as FB, `{@concept}` expands a term; without it braces stay plain
characters and texts encode exactly as before. Text assets (`text:` keys, which
sequences show) and battle texts are field text too when capsule_game() passes
the game's scripts manifest (`asset_compiler.py` / `battle_compiler.py
--scripts`). At run time the printers
expand each field placeholder through the **field resolver** the game
registers with `text::set_field_resolver()`: `script::placeholder_text()`
prints name slots (`script::set_name()`, saved with the flags), variables in
decimal and terms, and record bindings through `script::party::role_get()`
(the engine's port reads the party given to `script::party::attach()`; a
text role prints its codes, any other role its value; the manifest's
`"battle"` lets them name custom roles). The engine names no
placeholder. `script_ids.h` has `PLACEHOLDER_<NAME>`; the map sandbox shows
`{player}` and `{berries}`.

`tools/text_encoder.py` is the one encoder; the asset, script, map (location
names, event scripts, previews), battle, sequence (checks only) and font
compilers take `--code-page PATH`. A game picks its language in
`reference/project.json` (paths relative to the app; one language per build):

```json
"text": {"language": "en", "code_pages": {"en": "data/text/en.codepage.json"},
         "fonts": {"en": ["../../assets/fonts/default_8x8.txt", "data/text/extra_8x8.txt"]}}
```

`fonts` is optional (default: the engine's font); with a list, the later files
add glyphs. The game then compiles its own `kDefaultFont`, which replaces the
engine's at link time; without `text`, everything uses the ASCII page. Text on
BG0-BG2 may use all 250 glyphs; text on BG3 is refused at build time when the
font has more than 96 (BG3's character block holds the maps from tile 256).
The text test app (`apps/test_text`) uses a test page with a non-ASCII glyph at
0x80. Creature records keep the nickname's language in the `text_language`
role (no transcoding). The GBA assert screen prints its ASCII message through
`kDefaultFont`: with a game font whose page does not keep the ASCII letters at
their ASCII codes, it shows other glyphs (spaces and digits still read right).
