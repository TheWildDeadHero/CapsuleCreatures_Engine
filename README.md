# CapsuleCreatures Engine

Clean-room engine for monster-capture RPGs. One codebase builds a Game Boy
Advance ROM (devkitARM + libgba, with the engine's own register-level HAL),
desktop builds for Linux, macOS and Windows (SDL2), and Dreamcast builds
(KallistiOS). Every target other than the GBA runs the same software emulation
of the GBA video model, so all of them show the same image.

This repository is the **engine core** only (spec P-13): generic mechanisms,
the platform layer (HAL) and, later, the asset compiler and editor. The
reference ruleset and game content live in separate repositories, so this one
contains no franchise terms, rules or assets (D-2).

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
apps/<app>/             apps laid out like games: reference/project.json, data/, src/ (see Apps)
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
Doxyfile                API reference config; fails on anything undocumented (see Conventions)
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
tools/setup/            one-time machine setup (devkitARM, KallistiOS)
tools/dreamcast/        kos-run.sh: runs compiles and links inside the KallistiOS environment
tools/ci/               principle checks (no virtual dispatch, no floats, no reference code)
submodules/             libgba sources (pinned)
```

## Reference decompilations

This repository holds no decompilation and does not need one to build or
test (D-37). The decompilation projects that original-game behavior is
checked against (spec P-11) are submodules of the private importer
repository, `CapsuleCreatures_Pret_Importer` (its own `ref_submodules/`).
They are **reference only**: no code, data, graphics, audio or text from them
may be copied into this repository or compiled into any build (D-2). Engine
code is written clean-room from the design spec.
`tools/ci/check_principles.sh` fails if any engine source or build file
refers to a `ref_submodules/` folder.

## First-time setup

```
git submodule update --init submodules
```

GBA toolchain: devkitPro's `gba-dev` package group (devkitARM, libgba,
gbafix). Install it with

```
tools/setup/setup_devkitarm.sh            # --dry-run first to see every step
```

It follows devkitPro's official instructions
(https://devkitpro.org/wiki/devkitPro_pacman). On Arch-based systems such as
CachyOS it adds devkitPro's own pacman repositories (not the AUR); on
Debian-based systems it runs devkitPro's official installer script. It never
upgrades the rest of your system: on Arch it syncs only devkitPro's package
databases. If pacman then can't fetch a package or satisfy a dependency, it
stops and suggests `sudo pacman -Syu` for you to run. It asks before changing
anything, backs up `/etc/pacman.conf`, can be re-run safely, and finishes by
test-compiling C++20 for the GBA's CPU. The GBA toolchain file expects the
standard `/opt/devkitpro` location; set the `DEVKITPRO` environment variable
(or `-DDEVKITPRO=...`) if yours differs. Optional: `mgba-qt` to run the ROM.

Desktop needs CMake 3.24+, Python 3.10+ and a C/C++ compiler. SDL2 is not
installed separately: the build downloads a pinned release (version and
SHA-256 in `cmake/sdl2.cmake`) on first configure and links it statically.
Offline, pass `-DFETCHCONTENT_SOURCE_DIR_SDL2=/path/to/SDL2-<version>`.

| OS | Compiler | Notes |
|---|---|---|
| Linux | GCC or Clang | SDL builds its X11/Wayland backends only if their headers are present (Arch-based systems ship them with the libraries; Debian-based need `libx11-dev`, `libwayland-dev`, `libxkbcommon-dev` and friends, see `.github/workflows/build.yml`) |
| macOS | Xcode Command Line Tools (Apple Clang) | universal binaries (`arm64` + `x86_64`) for macOS 10.15+ by default; override with `-DCMAKE_OSX_ARCHITECTURES=...` / `-DCMAKE_OSX_DEPLOYMENT_TARGET=...` |
| Windows | MinGW-w64 GCC via MSYS2 | from an MSYS2 **MINGW64** shell: `pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-python`; executables are linked statically and need no DLLs next to them |

Dreamcast toolchain: KallistiOS. Install it with

```
tools/setup/setup_kallistios.sh           # --dry-run first to see every step
```

It follows KallistiOS's own instructions: build prerequisites from your
distribution's official repositories (`pacman -S --needed` on Arch-based
systems, no AUR, no system upgrade; `apt-get` on Debian-based), then
KallistiOS and kos-ports cloned at pinned commits into `/opt/toolchains/dc`,
the SH-4 toolchain built from source with KallistiOS's `stable` profile
(GCC 15.2), KallistiOS itself, and mkdcdisc for bootable `.cdi` images.
Building the toolchain takes from about 20 minutes to over an hour. Nothing
needs sourcing afterwards: the `dc-*` presets load `environ.sh` themselves
(set `KOS_ENVIRON` if yours lives elsewhere).

## Building

In VS Code, **Terminal → Run Task** lists everything:

| Task | What it does |
|---|---|
| GBA: build ROM | CMake preset `gba-debug`: every app's ROM in `build/gba-debug/` (`capsule_hello.gba`, ...) |
| GBA: build ROM (release) | preset `gba-release`: ROMs in `build/gba-release/`, `ENGINE_ASSERT` compiled out |
| GBA: run in mGBA | builds, then opens the ROM in mGBA |
| GBA: run input viewer in mGBA | builds `capsule_input_viewer.gba` and opens it |
| Desktop: build (debug) | CMake preset `desktop-debug` |
| Desktop: run hello world | opens the hello world in a 240×160 window (×3) |
| Desktop: run input viewer | opens the input viewer |
| Check: all tests + principles | header checks, input tests, input compiler tests, principle scan |

CMake builds every platform from one `CMakeLists.txt`. The presets also appear
in the CMake Tools extension; each OS lists only the ones it can build:

| Preset | Host | Builds |
|---|---|---|
| `desktop-debug`, `desktop-release` | Linux, macOS | desktop apps (+ tests on debug) |
| `windows-debug`, `windows-release` | Windows (MSYS2 MINGW64) | desktop apps with MinGW-w64 and Ninja (+ tests on debug) |
| `gba-debug`, `gba-release` | any host with devkitARM | GBA ROMs through `cmake/toolchains/gba-devkitarm.cmake` |
| `dc-debug`, `dc-release` | Linux, macOS with KallistiOS | Dreamcast `.elf` (dc-tool, emulators) and selfboot `.cdi`, through `cmake/toolchains/dreamcast-kos.cmake` |
| `headless` | any | engine core and tests on the software video model; no SDL, no apps |
| `big-endian-check` | Linux with `g++-s390x-linux-gnu`, `qemu-user` | headless build for big-endian s390x, tests run under QEMU (P-19) |

From a terminal:

```
cmake --preset gba-debug && cmake --build --preset gba-debug
cmake --preset desktop-debug && cmake --build --preset desktop-debug && ctest --preset desktop-debug
```

GitHub Actions (`.github/workflows/build.yml`) builds Linux, macOS and Windows
(debug with tests, and release), the big-endian check, both GBA ROM builds in
devkitPro's devkitARM container, and both Dreamcast builds (KallistiOS
installed by the setup script and cached by its pinned commit) on every push,
and keeps the ROMs, ELFs and disc images as build artifacts.

Each app in `apps/<name>/` builds as `capsule_<name>`; on GBA that is an
`.elf`, a header-fixed `.gba` and a linker `.map` side by side; on Dreamcast an
`.elf` and, when mkdcdisc is installed, a `.cdi`. Both GBA builds use `-O2 -g`
with LTO; release only adds `NDEBUG`. Tests build and run in the desktop and
headless configurations. Release builds define `NDEBUG`, which compiles
`ENGINE_ASSERT` out. A failed assert aborts with a message on desktop and halts
with the expression, file and line on screen on GBA and Dreamcast.

The Dreamcast stretches the 240×160 frame to fill the whole 640×480 screen,
with no border. The 3:2 image is shown at 4:3, so about 11% narrower than on a
GBA. Scaling is "sharp bilinear": the CPU doubles the frame to 480×320, and the
PowerVR draws it as a full-screen textured quad with bilinear filtering for the
remaining 4/3 × 3/2. Pixels stay crisp and evenly sized, with only their edges
blended. The PowerVR's vertex coordinates are written as precomputed IEEE-754
bit patterns, so the HAL stays integer-only (P-6). It runs at the display's
refresh (60 Hz NTSC/VGA, 50 Hz PAL) rather than the GBA's 59.73 Hz.

## Byte order and alignment (P-19)

Data that crosses a boundary (ROM blobs, saves, link and trade records, the
emulated GBA VRAM) has a fixed little-endian layout and is read and written
only through `capsule/core/endian.h` (`load_le16/32`, `store_le16/32`), never
by casting a byte pointer to a multi-byte type. That keeps big-endian hosts
(PowerPC) on the GBA's bytes, and avoids misaligned loads, which fault on the
Dreamcast's SH-4 and silently return rotated data on the GBA's ARM7TDMI. The
`big-endian-check` preset and CI job run the tests on a big-endian target.

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
| `gba_waitcnt` | `CAPSULE_GBA_WAITCNT` | `0x4014` | GBA ROM wait states (prefetch on, 3/1, as pret); flashcarts that need slower ROM access override it |
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
generator before D-22) that way, so the Gen-3 goldens keep their numbers
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
their `apps/test_SYSTEM/` apps (see Test apps).

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
builds everything it lists; generated files go to `generated/APP/`. Test art
is only ever simple shapes described in the manifest, so no third-party or
copyrighted material is involved.

### Sprite frames from images

Besides shape lists, a sprite frame can be a PNG file (path relative to the
manifest, exactly the sprite's size). `"palette": "auto"` picks up to 15
colors from the sprite's images (exact when they use 15 or fewer, a
median-cut reduction otherwise); with an explicit palette, pixels map to the
nearest entry. Alpha below 128 is transparent. An animation either shows
every frame for `"period"` display frames or gives one value per frame in
`"durations"`:

```json
{"key": "sprite:BALL", "kind": "sprite", "width": 32, "height": 32, "palette": "auto",
 "frames": [{"image": "frames/ball/ball_0.png"}, {"image": "frames/ball/ball_1.png"}],
 "animations": [{"frames": [0, 1, 0], "durations": [6, 12, 6]}]}
```

The compiler writes a dependency file, so editing an image rebuilds the
assets. PNGs are read with the engine's own reader (no imaging library); it
takes 8-bit and palette images, not 16-bit or interlaced ones. The scene
editor in capsule-studio writes frames in this form.

## Overworld maps

`capsule::map` (`include/capsule/map/map.h`) holds compiled maps and draws
them; `capsule::field` (`include/capsule/field/field.h`) moves a walker on
them. Both follow the design spec (C-03a/b/c, A-9, E-7):

- **Layers and cells (C-01b).** A map has up to four tile layers, each on
  its own background (BG0-BG3, chosen per map) with its own draw priority,
  plus one attribute layer. A metatile is 16x16 pixels of graphics only (one
  layer of 2x2 tiles) and can be painted on any tile layer, the way RPG
  Maker XP layers work; a chunk is 8x8 metatiles. A map is a sparse set of
  chunks; per cell it stores one metatile id per tile layer and a 32-bit
  attribute cell: behavior, environment tag, collision, elevation, the
  encounter flag and the encounter slot. Outside the chunks, and on
  unpainted cells, a border pattern (one per tile layer) shows; it is
  always impassable and anchored to world coordinates.
- **Draw order.** Lower priority numbers are in front; on a tie the lower
  BG is. Sprites take a priority per elevation (the world's `elevations`)
  and cover tile layers of the same or a higher priority number, so
  a canopy layer at a lower number covers the walker. Only tile layers
  exist so far; parallax and other layer contents come later.
- **Terrain classes are data.** The world manifest names them and says what
  each does: solid, passable only by some vehicle classes (water: swim),
  slide (ice), ledge (jump in one direction), push (forced step).
- **Rendering.** The map owns the backgrounds its layers name, character
  blocks 0-1 (1,024 tiles) and palette banks 0-12; block 2 and banks 13-15
  stay free for text and HUDs. While a map is shown, `gfx::background` runs
  the **map plan**: the map's BGs are reserved, and BG0, when the map leaves
  it free, moves to character block 2 and screen block 31, so text and the
  engine's text box (and `script_port_hal.cpp`) show over the map. Text
  styles whose bank the map uses (maps with 13 palettes) print in style 0's
  bank. The text box borrowing a map layer (C-01c) comes later. Each layer's
  32x32 screen is a ring around the camera; moving redraws
  only the metatile rows and columns that came into view. Tile animations
  swap runs of tiles in VRAM on their period, staggered by the compiler.
- **Movement.** Cell by cell at the speeds of the walker's vehicle class
  (on foot by default walking 1 and running 2 pixels per frame); a new
  direction turns first. Collision, elevation and terrain decide each step;
  ledges jump two cells with an arc.
- **Field rules are data (D-22 decision 5).** The world's `elevations` give
  each of the 16 levels the levels it meets (`"all"`, `"same"` or a list;
  two levels meet when either names the other), what arriving does
  (`"take"` the level, or `"keep"` the mover's, as on bridges) and a sprite
  priority. Without them the default rule applies: 0 meets every level
  (stairs), 15 meets every level and keeps the mover's (bridges), 1-14 meet
  themselves. The world's `vehicle_classes` are movement modes: walking and
  running speeds, whether the class may run, the sprite animation its set
  starts at and the behaviors it may enter. `field::set_vehicle()` and the
  script statement `vehicle NAME;` change the walker's class; a map with
  `"can_run": false` forbids running. The compiler's
  `compile_elevations()`, `default_elevations()` and
  `compile_vehicle_classes()` work in-process for editors.

One deliberate difference from the spec: the chunk directory entry is 20
bytes with natural alignment (it also points at the chunk's tile layers)
instead of a packed record, so the GBA reads it with aligned loads.

Not in this milestone: map connections,
per-chunk palette and tile streaming (C-02c; maps use fixed palettes for
now), ZX0 compression. Encounters: see Encounters below.

### World, tileset and map manifests

A game lists its world in `reference/project.json` (`"maps":
"data/world.json"`, plus `"debug_names": true` for sandboxes);
`tools/map_compiler.py` compiles it at build time with a dependency file, so
editing a tileset, map or sheet image rebuilds the tables.

```json
{"schema": "capsule.world/1",
 "vehicle_classes": ["foot", {"name": "swim", "speeds": {"walk": 2, "run": 2}, "can_run": false,
                              "first_animation": 12, "passable": ["water"]}],
 "behaviors": [{"name": "normal"}, {"name": "water", "passable": ["swim"]},
               {"name": "ice", "slide": true}, {"name": "ledge_down", "ledge": "down"}],
 "elevations": [{"name": "any", "meets": "all"}, {"name": "low"}, ...,
                {"name": "bridge", "meets": "all", "on_arrive": "keep", "priority": 2}],
 "tilesets": {"outdoor": "tilesets/outdoor.json"}, "maps": {"meadow": "maps/meadow.json"},
 "start": {"map": "meadow", "x": 16, "y": 11, "facing": "down"}}
```

The world may also organise its maps in `"map_groups"`, a tree of groups
(`{"name", "maps", "groups"}`, each map in at most one group) that editors
show as folders. The compiler checks it; the runtime does not use it yet.

Tilesets (`capsule.tileset/2`) have sheets of 8x8 tiles (drawn from shapes,
or PNG images with their palette picked automatically and an optional
transparent color), metatiles (`{"name", "tiles": [4 tile references]}`,
references are `"sheet:index"` with `h` / `v` flips) and animations. Maps
(`capsule.map/2`) list the tilesets they draw from (`"tilesets"`), their
tile layers (`{"bg", "priority", "purpose"}`), a dictionary of the
`"tileset/metatile"` names they use, a dictionary of the world behaviors
their cells use, the border (`"size"` and one pattern per layer) and the
chunks: `"cells"` are 8x8 attribute cells (`null` = unpainted, or `behavior
| env << 8 | collision << 16 | elevation << 18 | encounter << 22 | slot <<
23`) and `"layers"` holds 8x8 dictionary indices (or `null`) per tile layer.
A map may be blank (no metatiles, chunks or border; blank cells show the
backdrop and are impassable). The compiler deduplicates tiles with flips and
checks the limits (1,024 tiles, 13 palettes, 4,095 metatiles, 32 tile
animations and 256 behaviors per map). The map and tileset editors in
capsule-studio read and write these files.

Imported graphics (I2) whose tilemap entries already name VRAM tiles and
palette banks use **fixed-placement tilesets**: `"place": {"sheet", "tile",
"tiles", "palette", "palettes"}` loads the sheet's tiles at VRAM tile
`tile` and the tileset's `"palettes"` (16 colors each) at bank `palette`;
its sheets are indexed PNGs read as they are (`"indexed": true`), its
metatiles are raw entries (`{"name", "entries": [4 x tile | flips |
bank << 12]}`) and its animations name `"at"` (a tile of its place). A map
drawing from fixed tilesets loads all of its `"tilesets"` at their places
(no renumbering, places may not overlap); `"metatiles": "all"` is every
metatile of the map's tilesets in order. Any animation may give a
`"phase"` (frames into its period at map entry); a behavior may give
`"walls"`, the sides of its cell a step may not leave or enter through.
Maps sharing tilesets or cells share their compiled arrays.

### Encounters

`capsule::encounter` (`include/capsule/encounter/encounter.h`, ROM layout in
`data.h`; design C-03c/d/e, D-12) rolls encounters from tables the map
compiler builds (`tools/encounter_compiler.py`, run as part of it):

- **Slots.** An encounter cell (the encounter flag) also has a **slot**
  (0-15, bits 23-26 of the attribute cell); a map has one table per slot
  and **method**.
- **Methods are data.** The world names them (`"encounter_methods"`): step
  methods roll when the walker arrives on an encounter cell while in one of
  their vehicle classes (walking, swimming); script methods (one per tool or
  interaction, ...) roll when game code calls `encounter::roll_at()`.
  Default: one step method, `step`.
- **Tables** are weighted pools (C-03d): a rate (P-5 odds that a trigger
  starts an encounter) and up to 64 entries, each a species (from the
  battle manifest the world names, `"battle"`), a level range, a weight,
  the **conditions** it requires or excludes and weight multipliers while
  conditions are active (Q8, truncating). Condition names come from the
  world (`"encounter_conditions"`, up to 32); game code sets their bits
  with `encounter::set_conditions()`.
- **Sharing (C-03e).** The world may name a file of shared tables
  (`"encounter_tables"`) that maps use by name; identical tables are stored
  once in the global bank, whoever wrote them.
- **Field.** Arriving on an encounter cell rolls the first step method that
  applies (on `rng::gameplay`); when an encounter starts, the walker stops
  there (no slide or push) until the game takes it with
  `field::take_encounter()`. `field::set_encounters(false)` turns rolls off.
- `encounter_ids.h` (generated next to the world) names the methods and
  conditions for game code: `ENCOUNTER_METHOD_SURF`,
  `ENCOUNTER_CONDITION_NIGHT`, ...

```json
{"schema": "capsule.encounters/1",
 "tables": [
   {"slot": 0, "method": "walk", "rate": "1/10", "entries": [
     {"species": "Sproutle", "levels": [3, 5], "weight": 40, "excludes": ["night"]},
     {"species": "Duskowl", "levels": 4, "weight": 30, "requires": ["night"]},
     {"species": "Voltmink", "levels": [4, 6], "weight": 10, "modifiers": [{"when": "rain", "multiply": 3}]}]},
   {"slot": 1, "method": "swim", "table": "pond"}]}
```

A map names its file with `"encounters"` (relative to the map file).
`tools/encounter_compiler.py <world.json>` checks a world's encounters on
its own; capsule-studio's encounter editor and the map editor's Encounters
tab edit them. Not built yet: encounter modifier sets (genes, traits),
hidden nodes (Feebas-style), ambushes, chains, swarms' triggers.

### Encounter leads and timed field effects (F1, D-27 sections 1 and 2)

The tables, methods and conditions above stay as they are; a game layers
its rules on top of them (every rule is manifest data, effect logic or
ruleset):

- **The lead** (`battle::lead_of()`, `include/capsule/battle/encounter_lead.h`):
  the structure's `"lead": {"when": EXPR}` (a growth-style expression over a
  member's record: its roles, stats, species fields) names the first member
  for which it holds; without it, the first member with hit points.
- **Lead rules.** `battle::LeadScope scope(state, party);` attaches them to
  every roll (`encounter::set_rules()`; the scope gives the rolls back when
  it ends). A roll then runs the `encounter` hook (value = the method) on
  the lead in a field context: the game's `global` owners, the lead's
  passive, its held item. Their primitives change the plan, each operand
  interval-checked by the effect compiler: `encounter_rate N D` (the odds x
  N / D), `encounter_levels LO HI` (the entry's bounds moved),
  `encounter_level_bias N`, `encounter_skip_below LEVEL`,
  `encounter_type TYPE N D` / `encounter_field FIELD V N D` (with odds N in
  D the pick keeps to that type's / that species field value's entries);
  `block` stops the roll. Then the ruleset pipelines `encounter_rate`
  (inputs rate_n, rate_m, rate_scale (Q8), lead_level, method; outputs sure,
  chance, out_of) and `encounter_level` (min_level, max_level, lead_level,
  level_bias, method; output level) decide the odds and the level; both are
  optional (the defaults draw exactly what the tables draw on their own) and
  refuse point steps. `battle::last_encounter()` traces each roll (odds,
  lead, pick, why it ended) for debug lines.
- **The creature.** After `field::take_encounter()`,
  `battle::make_encounter(r, wild, rng::gameplay)` makes the record and runs
  the `encounter_made` hook (holder = the lead, target = the creature: e.g.
  `target.role(trait) = holder.role(trait);`; value = its level):
  `encounter_reroll` picks again (at most 3 times), `block` means no
  encounter.
- **Timed field effects** (`include/capsule/script/field_effects.h`): the
  structure's `"field_effects"` [{name, label, unit `steps` | `minutes`,
  stack `refresh` | `add` | `refuse`, expiry message, persists}]. Item
  `use_field` logic starts one (`field_effect start NAME N;`: refused, the run
  fails and the item stays), encounter hooks and other logic read it
  (`field.effect_active(NAME)`, `field.effect_remaining(NAME)`), scripts use
  `field_effect start NAME N [-> var(ok)];`, `field_effect active NAME ->
  var(v);`, `field_effect remaining NAME -> var(v);`. The script compiler
  keeps each in an implicit permanent u16 variable `field_effect_<name>`
  (so the save image's Script section holds it under the save shape and
  remap rules; record the shape: `tools/save_shape.py record scripts.json`)
  and gives an effect with an expiry message the script
  `field_effect_end_<name>`. The overworld runner counts the walker's steps
  and `clock::minutes()` (the game's port, default: never moves) and runs
  the expiry script when an effect ends; a loaded save stops the effects
  that do not persist.
- A Repel-style ward is pure data: an item whose `use_field` starts the
  effect, and a global rule `on encounter { if field.effect_active(ward) {
  encounter_skip_below holder.level; } }` (the Starter's `effects/field.ceffect`).

### Field actions, darkness, flight points and the escape point (F2, D-27 section 3)

Moves used outside battle (crossing water, clearing a bush, lighting a dark
cave, flying, escaping) are data: the engine provides the dispatch and a few
generic mechanisms, every rule and name is manifest data and script.

- **Field actions** (structure.json `"field_actions"`,
  `tools/battlec/field.py`): {name, label, `requires`, `target`, `ask`
  (default true), `prompt` (default "Use LABEL?"), `script`}. `requires` is
  the growth `when` language over a party member (roles, `stat.NAME`,
  `species.NAME`) plus `knows.MOVE` (the move's lower-case identifier),
  `passive` (`passive == passive.NAME`) and the game state `flag.NAME` /
  `var.NAME` (flags and variables of the scripts manifest: a badge, a story
  step); it compiles to one more growth rule each
  (`kFieldRules.first_action_rule` + the action's index) and the battle
  data reads the game state through the script bank's references
  (`rules::field_state()`). The member that uses an action is the first in
  party order its requirement holds for. `target` is `{"behavior": NAME}`
  (a world behavior), `{"object": KIND}` (a world `object_kinds` name a map
  object's `"kind"` gives), `{"tag": TAG}` (a menu action on maps carrying a
  world `map_tags` name) or none (a menu action anywhere). The script
  compiler resolves the targets against the world, the script, the flags and
  variables, gives an asking action the implicit script
  `field_action_ask_<name>` (the prompt as yes / no, then the script) and
  every action the implicit variable `field_action_member` (u8, map: the
  member's party slot).
- **Dispatch** (`capsule/battle/field_actions.h`): a `battle::ActionScope
  scope(party);` attaches the dispatcher to the field
  (`field::set_action_rules()`). Pressing A facing an object of a kind
  without a script of its own, or a cell whose behavior the walker cannot
  enter as it is (water on foot), asks it: the first action on that target
  a member may use runs (`field::Source::Action`; the object is the
  script's `self`, so `hide self;` clears a bush). `battle::menu_actions()`
  lists the menu actions usable on the shown map, `battle::start_action()`
  writes the member and gives the script for `overworld::run_script()`.
  Scripts ask `field_action available NAME -> var(v);` (the member + 1, 0
  for none).
- **Vehicle classes** (exists, wired): a world vehicle class with
  `"dismount": true` that steps onto a cell only class 0 enters becomes
  class 0 (leaving the water); a wade script is `vehicle swim;` and one
  scripted step.
- **Objects** (exist, wired): `hide self;` / `move self DIR;` from the
  action's script.
- **Darkness** (new): a map's `"darkness"` (0 lit to 4) on entering, a
  script's `darkness N;` until the next map. The overworld runner draws a lit
  box around the walker (window 0) and darkens the map layers and sprites
  outside it with the Darken blend; BG0 stays bright when it is the text
  layer. Register writes only (WIN0H / WIN0V / WININ / WINOUT / BLDCNT /
  BLDY each frame of a dark map), the same on every target: the soft video
  model of the desktop, headless and Dreamcast builds has windows and
  blends; the GBA build's code grows by the runner's apply_darkness() and
  the statement (see the package report for the sizes).
- **Flight points** (new): the world's `"flight_points"` [{name, label,
  map, x, y, facing}] each get an implicit permanent flag `flight_<name>`
  (so a save keeps them); scripts `flight record NAME;`, `flight visit NAME
  [transition];`, `flight menu -> var(v);` (the port's paged menu of the
  recorded ones; v = 1 when it flew).
- **The escape point** (new): a map with `"escape_point": true` is
  outdoors; a warp from it to a map without records where the walker stood
  (one step back, facing away) in the implicit permanent variables
  `escape_map` / `escape_x` / `escape_y` / `escape_facing`; scripts `escape
  [transition] [-> var(ok)];` warp back there.
- The Starter's actions (D-27 decision 3, each asking): `wade` (Tide Slam or
  Torrent, on water: swim), `clear` (Vine Lash or Spore Burst, on a bush
  object: it goes), `light` (Mend Light or Prism Beam, a menu action on dark
  maps: `darkness 0;`). The reference-ruleset repository has the Gen-3 set
  (its README).

## Battles

`capsule::battle` (`include/capsule/battle/battle.h`) runs battles; what a
battle does that is content is data (`include/capsule/battle/data.h`), and
the formulas behind stats, experience, rewards and escape and the pipelines
of damage, speed, accuracy and critical hits are the game's ruleset
(`include/capsule/battle/ruleset.h`, `pipeline.h`). Following the spec
(C-08, D-18, D-20, D-22, D-23, P-13, P-18):

- **Generic core (P-13).** The engine names no franchise terms: it knows
  parties, members, presenters, traits, passives, categories and rewards;
  a game's words for them are its manifest's `terms` (the Starter sample
  says Rival, Temperament, Talent, Charm, Coins) and its message texts. A
  `Participant` brings party data (`PartyData`: members, presenters, AI
  flags, rank, `PARTY_NO_ESCAPE`), records (`monster::Party`), or both;
  `PARTY_UNOWNED` parties (creatures met without an owner) announce each
  creature (`appeared`), need a position per creature, are never replaced
  and are not written back. `can_escape()` refuses running when the setup
  has `SETUP_NO_ESCAPE` or a party on another side has `PARTY_NO_ESCAPE`.
  `tools/ci/check_principles.sh` rejects the franchise words in the battle,
  monster and text core, the battle bench, the tools and the neutral battle
  data (tests/data/battle_*, tests/data/effects, tests/bench, samples).
- **Messages.** The engine says the messages of `battlec.texts`
  `REQUIRED_MESSAGES` itself; every manifest gives them. The others
  (`OPTIONAL_MESSAGES`: super_effective, not_very_effective, hit_count, the
  confusion / flinch / protect texts, recoil, drained, healed, hp_full) are
  only named by data and logic (chart and tier messages, condition texts,
  effect `say`): a manifest may leave them out, and then cannot name them
  (the Starter leaves out hit_count, protected, protect_start and recoil).
- **Passive slots** (D-25 decision 6): structure `passive_slots` lists the
  slots per species, `[{"name", "rolled"}]` (default one rolled slot, at
  most 4 on the GBA and 8 elsewhere). A species names up to one passive per
  slot (an empty slot gives the first slot's passive), a member its slot by
  index or name, and the generation entry `passive_selector` `[0, "last"]`
  rolls among the rolled slots up to the species' last filled one. A slot
  that is not rolled (`"rolled": false`) is given only by data or logic: the
  Gen-3 sample's `hidden` slot is its manifest's choice, not the engine's
  shape. The PassiveSelector role keeps the slot in its low bits (enough for
  the slot count; the compiler checks the width).
- **Presenters** (who shows a party on screen): each party names up to two.
  A battle opens with `Phase::Intro`, one party per step: its presenters
  enter (`EventKind::Presenter` cues), speak their intro
  (`EventKind::PresenterText`; without one a party given as data says
  `party_challenge`), and those that leave after the intro go; the rest
  leave right before the party's first send-out unless they stay. When a
  party is defeated its returning presenters come back and say their defeat
  text; the winners say their victory text. The engine only queues cues;
  `battle::presenter_view` (`presenter_view.h`) plays them with sprites,
  the video blend and the sequence player (slide, fade, sequence, stay).
- **One State, driven in steps.** `start()` copies the parties in and
  refuses setups that could never finish (a party with nothing to send out
  or no position, an unowned party larger than its positions). Each
  `step()` runs one small unit of work (one party's intro, the opening send-outs, one battler's
  switch-in effects, one target's accuracy check, one effect or one hit of a
  move, one experience award, one level-up, one battler's end-of-turn
  effects, one replacement) and records what happened as presentation events
  in a 256-entry ring (text, HP, status, stages, faints, switches, weather,
  experience, levels). A move runs to its end before anything else; its
  experience is shown after it. The presentation plays events at its own
  pace; the engine never waits for it. `request()` / `choose()` take human
  choices (move and target, switch, run, replacements) and refuse invalid
  ones.
- **Formats are data.** Positions, sides, reach between positions (`all`,
  or `adjacent` for triples) and the screen layout; the Starter has 1 vs 1
  and 2 vs 2. A side's positions are split between its parties;
  `stands` say where each side's presenters stand.
- **The turn structure is data** (D-22 A11, E6b; `turn`,
  `capsule::battle::TurnRules`): the order brackets of running, switching,
  items and moves (`"move": "priority"` uses each move's priority, a number
  puts every move in one bracket; then speed, then the `tie` rule:
  `random_byte`, a byte drawn per battler each turn, or `position`), the
  end-of-turn phases in order (`weather`: countdown and residual damage;
  `battlers`: each battler's residual hooks, optionally only some sources
  and in their own order; `sides` / `field`: each condition's residual for
  each of its battlers or `"each": "once"`, then its countdown; `regen`: the
  resources' regeneration; `delayed`, E13b: each position's pending effect of
  effect `delay` counts down and lands, needed by logic that uses `delay`), the hook
  dispatch order of the sources (`effect_order`: status, volatile, side,
  field, weather, passive, item, each once) and the battle start
  (`switching`: send-outs foes first or in position order, switch-in hooks
  by speed or in send-out order). Every key is optional; the defaults are
  the engine's former fixed order, which the sample writes out.
- **Logic is effect code** (D-22 decision 1, D-23 section 1;
  `include/capsule/battle/effect.h`): every move, passive, held item,
  status, volatile, weather, side and field condition is an *owner* of one
  bytecode bank
  (`BattleData::effects`, compiled by `tools/effect_compiler.py`) with
  handlers for engine hooks (`switch_in`, `switch_out`, `faint`,
  `turn_start`, `before_move`, `used_move`, `before_accuracy`, `try_hit`,
  `before_damage`, `damaging_hit`, `after_hit`, `after_move`, `set_status`,
  `stage_change`, `update`, `residual`, `order`, the condition-own
  `try_start` / `start` / `already` / `end`, `side_residual`,
  `field_residual`; E13b: `try_switch`, which may refuse a battler's chosen
  switch or run (trapping; a forced switch does not ask; C5: the battler's
  own owners, then the `per_foe` handlers of each active foe's owners, so a
  foe can trap it; `is_trapped()` asks without texts, `choose()` says why
  once a turn), and `redirect`, which may retarget another battler's chosen
  single-target move (C5: `selected` / `selected_foe` moves, a target
  phase's single target too; value 255 in, `modify set BATTLER` claims it,
  the first valid claim in position order wins) and move phases
  (`use`, `target`, `try_hit`, `hit`, `after_move`; E13b: `land`, a pending
  effect's arrival). E13b's primitives: `stage ... silent` (no text),
  `reset_stages WHO` (every stage to 0, no texts, no stage_change hooks),
  `delay WHO amount turns` (the move's land phase on that position later),
  `reward amount` (the reward pool), `end_battle` (the battle ends, nobody
  wins; C5: the unit stops there and the experience earned is paid before the
  End event); refs `targeted_by` (the last move that reached a battler past
  protection and accuracy since it entered), `delayed_turns`,
  `human`, `unowned`, `gene(STAT)` and `species_field(NAME)`; the `crit` flag
  is set before the `before_damage` hooks run, and a typed `damage_hp` runs
  them too (C5: without the flag). C5: the type a move hits a target with
  (the `move_type` point) is worked out once per target and is what hooks,
  phases and point handlers read as `move.type`; an unstored record role
  cannot be written. The move skeleton stays engine code (before_move, costs,
  "used", use, targets, per target before_accuracy / accuracy / try_hit /
  hit, after_move); a phase that yields (each hit of a `repeat`ed `damage`)
  or asks (`ask`, `switch ... pass`) is suspended in `State::mv.run` and
  goes on in the next step, a human's answer coming through `request()`
  (`SwitchTarget`, `LearnMove`, `Choice`) and `choose()` with
  `ActionKind::Answer`. Hooks run the holder's owners in the manifest's
  dispatch order (`turn.effect_order`; the sample: its status, its
  volatiles, its side's and the field's conditions, the weather, then its
  passive and held item); a
  battler never re-enters a hook it runs (a berry fires once), and
  past `kMaxEffectDepth` nested runs a dispatch is skipped and
  `State::vm.faults` counts it. `src/battle/effect_host.cpp` implements the
  VM's host functions (reads, writes, record roles, draws on the battle
  stream, the primitives). Logic comes from the `.ceffect` sources the
  manifest names (`"effects": ["effects/moves.ceffect", ...]`, with owner
  `"texts"`, `"cues"` and `"tags"`); the manifest keeps only data. A
  damaging move without logic hits each target once; `on switch_in per_foe`
  marks a switch-in handler that also runs once per foe on the field. The
  legacy effect lists, hook handlers and modify fields (before E4c) are
  unknown keys. Point handlers (below) are part of their owner.
- **Stats are data** (D-20 C): the manifest's `stats` list names each
  permanent stat, its in-game label and the ruleset stat formula that
  computes it (`rules::stat(index, ...)`); one is the pool stat (hit points,
  `max_hp()`), one the order stat (turn order and escape), and at most one
  the odds stat (`"odds": true`, E9: the ruleset's optional `effect_odds`
  pipeline reads it for both battlers when effect logic rolls `odds(n, m[,
  by, on])`; see "Starting a new game from the Starter sample"). Up to 10 stats on
  the GBA and 32 elsewhere (`kMaxStats`; the compiler checks the target's
  limits, `--platform`); arrays hold `kMaxStats` and the count is read at run
  time. Traits may change the stats with the `trait` flag.
- **Stages are data** (D-22 A7, E6a): the manifest's `stages` records, in
  stage order (`STAGE_<NAME>` ids): a stat's stage, accuracy, evasion or the
  critical-hit stage, each with its label, its range (within +-15) and its
  table (`[num, den]` per stage; odds for the crit stage) or a ruleset
  formula of (value, stage). Stage changes clamp to the stage's range; the
  pipelines read them through `stage_scale(id, v, stage)` (the inputs
  `offense_stage_id`, `defense_stage_id`, `speed_stage_id`), `staged()` (the
  table every stat stage shares), `accuracy_staged()` and `crit_chance()` /
  `crit_out_of()`.
- **The type chart's tiers are data** (D-22 A6, E6a): `chart.tiers` (up to
  8: a Q8 factor, or decisive `zero` / `ko` with a message), the combine mode
  of a defender's types (`exponent`: the pipelines' `type_exp` with
  `pow2()`; `multiply`: one combined factor; `sequential`: each factor in
  turn; `type_scale(v)` applies whichever), `messages.above` / `below`,
  `types_per_species` and `added_types` (at most 3 types per battler), and
  the matchups (tier names or multipliers; the rest take `default`).
- **Categories are data**: up to four, each naming the attacker's and the
  defender's stat by name (none: a non-damaging category); damage and the
  self-hit (`rules.self_hit.category`) take their stats from it.
- **Pipelines** (D-20 D, P-14): damage, a self-hit (effect `self_hit`),
  speed, accuracy and critical hits are the ruleset's pipelines, lists of
  steps (`let` a value with an optional clamp, run a modifier `point` on it,
  `roll` a draw at exactly that position) compiled to straight-line C++
  (`rules::damage()` ... `rules::crit()`, `include/capsule/battle/pipeline.h`).
  The engine gathers the inputs (stats, stages, crit, spread, same type,
  type exponent, ...) and uses the outputs; the type chart's decisive tiers
  end a matchup before the pipeline, the first in type order winning (C-08h). The Starter's
  ruleset writes out its own steps (critical, same type, type chart, spread,
  weather, a 92-108 % roll last, status, passive / item) with Q8
  truncations and one draw (README "Starter rules").
- **Points**: a point (`offense`, `power`, `weather`, `damage`, `speed` in
  the sample) names its sources in order (user / target passive, item,
  status; the weather; E13b: the move's own handlers, each side condition on
  a battler's side and each volatile it has); `battle::run_point()` runs their
  handlers in that order, each seeing the value clamped to 16 bits, then the
  point's range clamps it; a point nobody handles costs a mask test. A point
  named `move_type` (E13b) is run by the engine on a hit's type before the
  chart, the same-type check and `type_scale()`; point handlers then read the
  type the hit has as `move.type` (hooks still read the move's own). Point handlers are
  the pure part of the effect language (`point power when move.type ==
  "Flame" { modify by 1.2; }`: reads, arithmetic, `if`, `modify by | add |
  set`; no writes, events or draws, so previews never draw), compiled to one
  generated `rules::point_handler()` switch. Statuses and weathers carry
  point handlers too (Scorch's damage, Shock's speed, Storm's type
  multipliers in the Starter), and so do moves (variable power, a type by
  weather or genes), volatiles and side conditions (screens; E13b).
- **Conditions are records** (D-22 A3-A5, D-23 section 2): `statuses`
  (at most 63), `volatiles` (32: a bit per battler, with up to 8 counters
  and 4 words in all, cleared at the end of the turn, on switch-out or on
  faint as the record says, handed on by a passing switch), `weathers`,
  `side_conditions` (turns and layers per side) and `field_conditions`
  (turns), plus `move_flags` (16; `move.has(flag)`) and `tags` (16). A
  record's declarative fields are templates the battle compiler expands
  into effect code in front of the condition's own `.ceffect` handlers
  (`tools/battlec/conditions.py`): residual damage (optionally escalating),
  lost turns (odds or a length), cure odds, thawing by a move flag or a
  type, immune types, durations, chains (`chain {den, cap, kept_by}`) and
  texts; the engine keeps only names, texts, owners and the volatiles'
  slots and clear rules. Each battler also keeps the last damage it took
  from a move (`taken.*`). The Starter's flinch and daze are such records.
- **AI (D-18)** decides at the start of each turn from that moment's state,
  before any human choice exists, with one draw for the turn that every
  estimate's damage roll scales;
  flags pick expected damage, avoiding moves with no effect, and status moves.
- **Progression is data** (D-22 A8 / A12, E6c; `capsule::battle::ProgressRules`).
  Experience is the ruleset's `exp_gain` (with the foe's party rank as
  `foe_rank`, clamped to `rules.rank_max`), shared by `exp_policy`:
  `share` `faced` (battlers that were on the field with the foe), `active`
  or `party`, `receivers` `human_side` or `opposing_sides` (AI parties too),
  `outsider` `never` or `owner_id` (1 for records whose owner_id is not
  `Setup::owner_id`). Level-ups recompute stats and learn the level's moves
  into free slots; with every slot taken `learn.on_full` skips the move,
  replaces the oldest (`replace_oldest`) or asks (`request`: step() returns
  Input, request() gives `RequestKind::LearnMove`, `State::learn.move` is the
  move, the answer is the slot to forget or `kKeepMoves`; AI parties keep
  their moves). `learn.default_moves` (`last_n_by_level` or `first_n`) picks
  the moves of creatures built without any. A move resource's maximum (PP)
  is its `max` formula of the move's base and the slot's raises (see
  Resources). `make_monster()` runs the
  `generation` list in order, which is its draw order: `random_range`
  ([lo, hi]; `"max"` = the role's width; `"last"` = the last trait or the
  species' last rolled passive slot; one draw up to 65536 values, two for a
  power of two above, none for a single value), `constant`, or `derived`
  (an expression over the record's single-element roles, 32-bit ones as
  `NAME_hi` / `NAME_lo`, generated as `rules::derive()`; a role the layout
  does not store is read through `battle::record_value()`). The defaults
  are neutral (D-22 decision 8): `learn.default_moves` and
  `exp_policy.share` have none and must be given, `learn.on_full` is
  `skip`, `exp_policy` gains go to the human side without an outsider bonus,
  and without a `generation` list nothing is drawn (every role keeps its
  default), then fills every persisted resource. The sample writes out the
  engine's former fixed rules (tests/data/battle_progress shows the others).
- **Resources** (D-25, `include/capsule/battle/data/resources.h`,
  `tools/battlec/resources.py`): every pool a battler spends or loses is a
  structure record of `resources`, in the order costs are paid. A `battler`
  resource has one pool per battler with a stat as its maximum; exactly one
  has `"empty": "faint"` on the pool stat: the hit points (`Battler::hp`).
  A `move` resource has one pool per move slot (`Battler::move_pool`): its
  `base` names the move field every move states (e.g. `pp`), its `max` a
  ruleset formula of (base, raises) (default the base) and `raises` the
  record role of each slot's raises. `persist` names the record role a pool
  is kept in between battles (else it is refilled every battle); the role
  must hold the pool's highest value (a battler pool's stat's analysed
  maximum, a move pool's max formula range or largest base), so a
  write-back never truncates a pool (the engine clamps and asserts too).
  `regen` (a number within int16, or a formula of (value, max)) runs in the
  turn's `regen` end-of-turn phase. A move's `cost` is
  `{resource: amount or formula}` (default: 1 of every move resource); a
  move whose cost cannot be paid is not selectable (player and AI; the AI's
  `save_resources` flag also prefers cheaper moves); a number cost no pool
  can ever reach is refused. A cost on the hit points needs more hit points
  than it takes: paying never faints the user (the move is not usable
  instead). `rules.fallback_move` runs, paying nothing and stating no cost,
  when no move can be paid. Effect logic reads and writes
  `holder.resource(NAME)` / `resource_max(NAME)` and `move.base(NAME)` /
  `move.cost(NAME)`, and has `restore WHO NAME [amount] [every_slot]` and
  `raise WHO NAME delta [every_slot]`. Battlers hold 4 battler resources
  (hit points included) and 2 move resources on the GBA, 8 and 4 elsewhere.
  ```json
  "resources": [{"name": "hp", "label": "HP", "scope": "battler", "stat": "hp", "empty": "faint",
                 "persist": "current_hp"},
                {"name": "pp", "label": "PP", "scope": "move", "base": "pp", "max": "pp_max", "raises": "pp_ups",
                 "persist": "pp"},
                {"name": "ep", "label": "EP", "scope": "battler", "stat": "ep", "regen": "ep_regen"}]
  ```
  `python3 tools/battle_resources.py path/to/battle.json` migrates a battle
  from before E8 (the hit points and PP as resources, the ruleset's `pp_max`
  slot as a named formula and the pp resource's max whenever the ruleset
  has it, `move_slots` 4, the message `no_pp` as `no_usable_move` (dropped
  when both are given), `lock ... no_pp` as `lock ... free`); it writes a
  battle's files all or none.
- **Rewards**: a victory adds the ruleset's `reward(reward_base, level)` for
  each defeated party given as data (its `reward` and its last member's
  level) to the pool effect logic may have added to (`reward`, E13b) and shows
  one `reward` message.
- **Deterministic.** All randomness comes from the battle's own stream,
  seeded in the setup (A-4): the same seed, parties and choices replay the
  same battle (the basis for link lockstep and recorded battles).
- At the end, human parties' records get their persisted resources (HP,
  PP) and raises (0 for a move learned in battle), status, experience,
  level, moves and held item back.

Deliberate differences from the spec: a battler slot is larger than the
spec's 60 B (it carries what level-ups need, so the battle never reads the
records or copies ROM tables until it ends); `kMaxPositions` is 6 (REF-17).

Not in this milestone: point handlers of field conditions (no point source
yet), global owners, AI scripts, battle animations,
link and recorded battles, SOS calls, parties joining, Battle Royal scoring,
stackable rule modules (REF-15), bosses, capture, the Bag, the move-forget
prompt, held-item effects beyond the hooks above.

### Creature records

`capsule::monster` (`include/capsule/monster/monster.h`): engine code reads
records by **role**. Engine roles come from a frozen, append-only registry
(species key, seed, owner ID, experience, level, moves, PP, PP raises, genes,
training, awakening, trait, passive selector, held item, current HP, status,
nickname, text language); IDs never move. PP, PP raises and current HP are
plain storage since E8: the battle reads them only where a resource names
them as its `persist` or `raises` role. A game adds **custom roles** (D-22
decision 2: gender, met data, ribbons, friendship, ...): fields the engine
stores and scripts read and write, but no engine code interprets. The game
picks the roles in its battle data's `monster_layout`; the battle compiler
packs them (bytes and larger fields byte-aligned) and emits the layout table
(offset, element width and count per engine role, the custom role table)
and its hash:

```json
"monster_layout": {"party_size": 6, "move_slots": 8,
                   "roles": ["species_key", "level", {"role": "pp", "width": 6}, {"role": "nickname", "count": 10},
                             {"custom": "gender", "width": 2}, {"custom": "ribbons", "width": 1, "count": 32},
                             {"custom": "friendship", "width": 8, "default": 50}, "..."]}
```

- A plain list is the same with 4 move slots and parties of 6 (record
  transfer fixtures); a battle's layout states both.
- A layout chooses an engine role's width up to the engine's maximum (the
  8- and 16-bit fields battlers keep, 32 for seed, owner ID, experience;
  species key and status are fixed) and the nickname's length (1-12); the
  compiler checks the data fits (32 moves need 6-bit moves: all bits set is
  "no move"). The per-stat roles hold one element per stat, moves / PP / PP
  raises one per move slot (`move_slots` 1-8; battlers hold 8 and the engine
  loops over the layout's). `party_size` is up to 6 on the GBA and 8
  elsewhere; party storage is that cap, formats' `party_max` and parties are
  checked against the layout.
- Custom roles (at most 32) have IDs 0x80 + their index; `battle_ids.h`
  names every role `ROLE_<NAME>`, `monster::find_custom(name_hash("gender"))`
  finds one by name, `monster::reset()` gives a new record their defaults
  (`make_monster()` does). Owner secret, owner name, friendship, form,
  capture device and "nicknamed" are custom roles now (A10): the registry
  keeps their IDs reserved, and a bare old name still compiles as the custom
  role of its old shape. A non-empty nickname names a battler.
- The hash folds the engine role IDs in order, a chosen width or count only
  where it differs from the registry, each custom role as (name hash, width,
  count) — one with a retired role's name and old shape folds that role's ID
  — and the party size where it is not 6; defaults are not folded. The
  sample keeps hash 0x62409274 and its 60-byte record.

A record is at most 100 bytes on the GBA and 256 elsewhere. A role a layout
leaves out reads as 0.

#### Record transfer (v2)

`include/capsule/monster/transfer.h` moves a record between games whose
layouts differ (trades, links, a save carried to another game).
`export_record()` writes the **canonical transfer record**, keyed by role and
stat instead of bit offset: a version byte, the engine role count and a
presence mask over the engine role IDs, the custom role keys and the stat
table (sorted 32-bit keys: FNV-1a of `"role:" + name` and of `"stat:" +` the
stat's `transfer` key), each present role's width and count, then the values
bit-packed at those widths (per-stat roles in stat table order, move roles
by slot). It is deterministic and layout-independent: the same values in
another role order give the same bytes. `import_record()` copies matching
roles and stats into the receiver's layout; values too wide are clamped to
the receiver's maximum (flag `kTransferNarrowed`; "no move" and the end code
stay all-ones), incoming roles, stats or non-empty elements without a place
are dropped (`kTransferDropped`), and the receiver's own get their defaults
(custom roles' `default`, "no move", end codes, 0, or the layout's
`transfer_defaults`; `kTransferDefaulted`). Games with the same transfer hash
(layout hash and stat keys) can send the raw record (`form_for()`). Nothing
allocates; `transfer_size()` / `kMaxTransferBytes` size the buffer.

```json
"monster_layout": {"party_size": 6, "roles": ["..."], "transfer_defaults": {"spa": {"stat_genes": 7}, "spd": 3}}
```

A game's table (`capsule::monster::kTransfer` for its `kLayout`) comes from
`capsule_transfer_table(battle.json out.cpp)` (cmake/parts/transfer.cmake;
`tools/record_transfer.py emit`); capsule_game() generates it for every game
with battle data (`generated/<name>/transfer.cpp`, linked only when the game
transfers records). The battle compiler checks the stat transfer keys too. `tools/record_transfer.py` is the same
format in Python (record dicts or bytes; `export` / `import` on the command
line) and is checked byte for byte against the C++ (tests/transfer_tests.cpp,
tests/data/transfer). Move indices, species keys and statuses are copied as
numbers: games with different tables remap them themselves.

### Battle data and rulesets

A game lists its battle data in `reference/project.json` (`"battle":
"data/battle.json"`). `tools/battle_compiler.py` compiles it with the
ruleset it names into `battle::kData`, the formula functions and the record
layout, plus `battle_ids.h` (constexpr indices: `SPECIES_...`, `MOVE_...`,
`PARTY_...`, `PRESENTER_...`, `CATEGORY_...`, `FORMAT_...`, `STAT_...`,
`STAGE_...`, `TIER_...`, `RESOURCE_...`, `ROLE_...`) for game code. CMake passes `--platform` so the data is checked against that
platform's limits (`tools/build_settings.py` `PLATFORM_LIMITS`).
The battle data is two manifests (D-23 section 2, E5s): `battle.json`
(`capsule.battle/1`) holds the content and names the ruleset and the
structure manifest; `structure.json` (`capsule.structure/1`, next to it) holds
the game's battle structure. Each file may only hold its own keys (the
compiler names the right file for a key in the wrong one; the lists are
`tools/battlec/manifest.py` `BATTLE_KEYS` / `STRUCTURE_KEYS`). A single-file
manifest from before the split is refused with the command that migrates
it, `python3 tools/battle_split.py path/to/battle.json` (it writes
`structure.json`, or `<name>.structure.json` for another file name, and
moves the structure keys there unchanged, so the compiled data stays the
same). The compiler's docstring describes every key; in short:

```json
{"schema": "capsule.battle/1", "ruleset": "ruleset.json", "structure": "structure.json",
 "types": [{"name": "Fire"}, {"name": "Cosmic"}],
 "moves": [{"name": "Fireball", "type": "Fire", "category": "elemental", "power": 70, "accuracy": 100,
            "cost": {"mp": 8}, "target": "selected"},
           {"name": "Strike", "type": "Cosmic", "category": "physical", "power": 20, "cost": {}}],
 "passives": [{"name": "Blaze Heart", "handlers": [{"point": "power", "code":
   "when move.type == Fire and holder.max_hp > 0 and holder.hp * 5 <= holder.max_hp * 2 { modify by 1.4; }"}]}],
 "species": [{"name": "Emberkit", "types": ["Fire"], "base": [45, 40, 50, 62, 58, 44, 42, 48, 61],
              "passives": ["Blaze Heart"], "growth": "steady", "exp_yield": 62,
              "learnset": [[1, "Strike"], [5, "Fireball"]]}],
 "traits": [{"name": "Even"}, {"name": "Fierce", "up": "attack", "down": "protection", "pct": [115, 88]}],
 "presenters": [{"name": "Coach", "sprite": "sprite:PRESENTER", "enter": {"style": "slide", "frames": 24},
                 "exit": {"style": "slide", "frames": 16}, "intro": "{party} wants to battle!"}],
 "parties": [{"name": "Novice Kit", "presenters": ["Coach"], "reward": 20, "rank": 1, "escape": false,
              "ai": ["damage"], "members": [{"species": "Emberkit", "level": 6}]}],
 "messages": {"used_move": "{user} used {move}!", "...": "..."},
 "effects": ["effects/moves.ceffect"]}
```

```json
{"schema": "capsule.structure/1",
 "monster_layout": {"party_size": 6, "move_slots": 8, "roles": ["species_key", "level", "..."]},
 "stats": [{"name": "hp", "label": "HP", "formula": "vital", "pool": true},
           {"name": "mp", "label": "MP", "formula": "reserve", "trait": false}, "...",
           {"name": "attack", "label": "Attack", "formula": "core"}, "...",
           {"name": "agility", "label": "Agility", "formula": "core", "order": true}],
 "resources": [{"name": "hp", "scope": "battler", "stat": "hp", "empty": "faint", "persist": "current_hp"},
               {"name": "mp", "scope": "battler", "stat": "mp", "persist": "current_mp"},
               {"name": "ep", "scope": "battler", "stat": "ep", "persist": "current_ep", "regen": "ep_regen"}],
 "stages": [{"name": "accuracy", "kind": "accuracy", "range": [-4, 4], "table": [[60, 100], "...", [140, 100]]},
            {"name": "evasion", "kind": "evasion", "label": "evasion", "range": [-4, 4]},
            {"name": "critical", "kind": "crit", "label": "focus", "range": [0, 3],
             "table": ["1/20", "1/10", "1/5", "3/10"]},
            {"stat": "attack", "range": [-4, 4], "table": [[40, 100], "...", [100, 100], "...", [200, 100]]},
            "..."],
 "chart": {"tiers": [{"name": "none", "decisive": "zero", "message": "no_effect"}, {"name": "weak", "q8": 154},
                     {"name": "normal", "q8": 256}, {"name": "strong", "q8": 448}],
           "combine": "multiply", "messages": {"above": "super_effective", "below": "not_very_effective"},
           "types_per_species": 2, "added_types": 1, "default": "normal",
           "matchups": {"Fire": {"Bio": "strong", "Fire": "weak"}}},
 "categories": [{"name": "physical", "offense": "attack", "defense": "defence"},
                {"name": "elemental", "offense": "magic", "defense": "protection"}, {"name": "status"}],
 "growth": {"brisk": "6*n*n + 24*n", "steady": "8*n*n + 32*n"},
 "learn": {"default_moves": "last_n_by_level"}, "exp_policy": {"share": "faced"},
 "formats": [{"name": "Single", "sides": [[0], [1]], "range": "all", "layout": [[64, 92], [176, 52]]}],
 "turn": {"brackets": {"run": 30, "switch": 20, "item": 25, "move": "priority"}, "tie": "random_byte",
          "end_of_turn": [{"run": "weather"}, {"run": "battlers"}, {"run": "sides"}, {"run": "field"},
                          {"run": "regen"}],
          "effect_order": ["status", "volatile", "side", "field", "weather", "passive", "item"]},
 "terms": {"party": "Rival", "passive": {"one": "Talent", "many": "Talents"}}}
```

A structure whose types decide physical or special (an older generation's
rule) states `"category_by_type": {"<type>": "<damaging category>", ...}`
for every type (E11): a damaging move then takes its type's category and
states none (or a non-damaging one, a status move); a move naming another
damaging category is refused (`tools/battlec/stats.py`
`compile_category_by_type()`). Its table also gives the category of a hit
whose type the ruleset's `move_type` point changed; a structure whose moves
state their categories can give that alone (C5): `"hit_category_by_type":
{"<type>": "<damaging category>", ...}` (every type) makes a damaging move
whose hit type is not its own type hit with that type's category (stats,
`move.category`, `taken.category`; Hidden Power, Weather Ball); both keys at
once are refused.

The structure manifest also holds `rules`, the condition records
(`statuses`, `volatiles`, `weathers`, `side_conditions`,
`field_conditions`), `move_flags`, `tags` and `generation`; the content
manifest also holds `items` and the names logic uses (`texts`, `cues`).

**Neutral defaults** (D-22 decision 8): a key a manifest may leave out
defaults to an identity or the engine's limit, never to a game's numbers:
`rules.stat_cap` 65535, `rules.level_max` 255, `rules.self_hit.power` 1, a
species' `exp_yield` 0, an escalating status's cap 255, no generation list,
the `turn` structure of the engine's former order. Some keys have no default
at all: `monster_layout.party_size`, a trait's `pct` when it names `up` or
`down`, `learn.default_moves`, `exp_policy.share`, `monster_layout.move_slots`
and, since E8, the resources: the faint resource (hit points) must be
declared, every move states each move resource's base, and a move resource
without `max` has its base as maximum. The pool stat stays a stat flag; the
faint resource names it.

A move's logic is `.ceffect` code in the sources `effects` lists (`move "Fireball" { hit { damage; if target.alive
and odds(10, 100) { inflict target Scorch quiet; } } }`; a damaging move without logic hits each target once).

Texts may write `{@concept}` or `{@concept:many}` (messages, status,
weather and presenter texts): the compiler expands the game's terms, so
nothing about terms reaches the ROM. A concept given only in its one form
gets its many form from the terms' plural rule (`"plural": "english"`, the
default: Ability -> Abilities, Status -> Statuses, EXP and Species stay;
`"same"`: the one form); `battle_compiler.plural_of()` is the rule the
studio shows. With the game's scripts manifest (`--scripts`, which
capsule_game() passes) the texts may also use its field placeholders
(`{player}`): `battle::format_text()` keeps them for the printer.
A presenter's entrance or exit may be a sequence (`capsule.sequence/1`, compiled into the battle data); it may
only move sprite slots 14-15 with sprite, wait, blend, sound and loop ops.

A ruleset (`capsule.ruleset/2`) gives the formula slots (`exp_gain`,
`reward`, `escape_odds`), named formulas (with declared `args`, called from
pipelines), the stat formulas (named; each stat names one), the points and
the five pipelines, as integer expressions (`+ - * / %`, comparisons, `min
max clamp abs isqrt if`; in pipelines also `staged`, `accuracy_staged`,
`crit_chance`, `crit_out_of` over the battle's tables, `mulq8`, `pow2`, and
`STAT_CAP` / `LEVEL_MAX` from the battle rules) with a declared range per
input and per result (the sample scales each stat by its own stage,
`stage_scale(offense_stage_id, offense_raw, offense_stage)`, and the type
chart by `type_scale(d)`, so it compiles with differing stat stages and in
every combine mode):

```json
{"schema": "capsule.ruleset/2",
 "constants": {"HP_FLAT": 10, "HP_START": 8, "HP_DIV": 22, "DAMAGE_LEVEL_BIAS": 5, "DEFENCE_PAD": 10,
               "DAMAGE_DIV": 50, "DAMAGE_FLAT": 1, "CRIT": 416, "ROLL_LOW": 92, "ROLL_SPAN": 17, "ROLL_DEN": 100},
 "inputs": {"level": [1, 50], "power": [1, 1023], "attack": [1, 700], "defense": [1, 700], "...": []},
 "formulas": {"damage_base": {"args": ["level", "power", "attack", "defense"],
   "expr": "attack * power * (level + DAMAGE_LEVEL_BIAS) / ((defense + DEFENCE_PAD) * DAMAGE_DIV) + DAMAGE_FLAT",
   "range": [1, 65535]}},
 "stat_formulas": {"vital": {"expr": "HP_FLAT + base / HP_START + level * (base + gene) / HP_DIV",
                             "range": [1, 700]}},
 "points": {"power": {"holder": "user", "sources": ["passive", "item"], "range": [1, 1023]}},
 "pipelines": {"damage": [{"let": "attack", "expr": "stage_scale(offense_stage_id, offense_raw, offense_stage)",
                           "range": [1, "STAT_CAP"]},
                          {"let": "defense",
                           "expr": "if(crit, defense_raw, stage_scale(defense_stage_id, defense_raw, defense_stage))",
                           "range": [1, "STAT_CAP"]},
                          {"let": "power", "expr": "move_power"}, {"point": "power", "on": "power"},
                          {"let": "d", "expr": "damage_base(level, power, attack, defense)", "range": [0, 65535]},
                          {"let": "d", "expr": "if(crit, mulq8(d, CRIT), d)", "range": [0, 65535]},
                          {"roll": "r", "span": "ROLL_SPAN"},
                          {"let": "damage", "expr": "max(d * (ROLL_LOW + r) / ROLL_DEN, 1)"}],
               "...": []}}
```

The compiler bounds every intermediate value over the input ranges and
fails the build if one can leave int32, a divisor's range holds 0, or isqrt
can see a negative value (P-18); the generated function clamps its result to
the declared range, and `ruleset_compiler.py <ruleset.json>` reports whether
the clamp can engage. The battle compiler then checks the ruleset against
the engine: every input range must cover the values the engine can pass
(genes, training and awakening up to what the layout's widths hold, levels
up to `level_max`, the data's base stats and yields, the speed pipeline's
speeds for `escape_odds`, ...), the stat formulas must declare ranges within
16 bits, and every pipeline is analysed over this battle's stats, moves and
tables (`ruleset_compiler.bind()`: a named formula's arguments must stay in
its declared ranges, outputs inside what the engine stores).
`ruleset_compiler.run_pipeline()` evaluates a pipeline in Python with a
point callback (the battle compiler's `point_runner()`, which runs the
handlers with `effect_compiler.run_pure`); the reference-ruleset repo's
`gen3/tests/test_gen3_pipelines.py` reproduces every damage and preview grid
line of the WP-2 goldens with it.
It also keeps each step small: at most 8 effects per handler, 10 hits per
multi-hit move and 4 moves learned at one level, and each text may use only
the placeholders the engine fills for it (`{party}` and `{passive}` among them). Growth rates are expressions in `n` turned into tables
at build time (clamped, never decreasing). The engine itself contains no
game's formulas (P-13): the test and sandbox rulesets are invented.

## Battle bench

`apps/battle_bench` is a headless host program (desktop and headless builds
only, never the GBA) linked against one game's generated battle data. It runs
a `capsule.bench/1` scenario and writes JSON lines: the transcript in the
WP-2 golden format (`tests/support/battle_transcript.h`, shared with
`tests/starter_golden_tests.cpp` and the reference repo's golden tests), and on request every draw from the battle
stream (`rng`), every effect VM host call with its owner, hook, program, pc
and source line and column (`vm`), and every raw battle event (`event`).
A scenario with `"owner_texts": true` (E13b) also has the transcript show
owner texts (effect `text`) and volatile / side / field condition changes;
without it the transcript is the goldens' (`transcript::kOwnerTexts`).
Being host-only tooling, the bench is exempt from the engine's no-heap rule
(RAII and Embedded C++, D-24): its JSON reader / writer and scenario tables
use `std::string` and `std::vector`; nothing of it is built for a console
(`cmake/parts/bench.cmake` guards it), and the battle modules it links stay
heap-free.

A scenario gives the format, seed, field weather, the parties (data parties,
or record members with record pokes: moves, persisted resources by name
(`"resources": {"pp": [5, 10]}`), HP, status, item, trait, passive slot,
experience), battler pokes applied right after the battle starts (HP,
status and its counter and scratch (`status_counter`, `status_aux`), a
volatile's counter (`counters`: {volatile: n}), volatiles, stages, item,
resources by name), scripted choices per turn and answers to effect requests, a
stop condition (`end`, turns or steps) and the traces. Names are the battle
manifest's; `tools/bench_scenario.py` documents the format.

    python3 tools/bench_scenario.py validate tests/bench/starter/*.json
    python3 tools/bench_scenario.py build path/to/battle.json --cache .bench
    python3 tools/bench_scenario.py run tests/bench/cases/pokes_trace.json --bench .bench/capsule_bench_game --trace rng,vm
    python3 tools/bench_scenario.py check tests/bench/starter/unowned_single.json --bench <build>/capsule_starter_bench

A game with its own generator passes its name and source (`--rng NAME
--rng-source FILE`; a scenario's `"rng"` names the generator it was made
with). `build` configures the engine (headless) into the cache folder once, with
`-DCAPSULE_BENCH_MANIFEST`, `-DCAPSULE_BENCH_RNG` and `-DCAPSULE_BENCH_RNG_SOURCE`, and then rebuilds the
`capsule_bench_game` target incrementally. The same functions are the
studio's Python API (`validate`, `build`, `run`, `check_expected`,
`parse_output`). In CMake, `capsule_battle_bench(<target> <manifest>
[RNG <name> RNG_SOURCE <file>])` (`cmake/parts/bench.cmake`) builds a bench; the
engine's own, `capsule_starter_bench`, uses the Starter sample with the
default generator, and the scenarios in `tests/bench/starter/` reproduce its golden
transcripts byte for byte (CTest `bench_starter_*`). The reference-ruleset repo
builds the Gen-3 sample's bench with its Method-1 generator for the WP-2 scenarios.

Only the bench has the debug pc -> source line table:
`tools/effect_compiler.py` records positions under `track_lines(True)` and
`emit_line_table()` writes an `effect::LineTable` (declared in
`include/capsule/battle/effect.h`); the battle data and every game build are
unchanged. The traces are link-time seams: the bench compiles the generator
and `src/battle/effect_host.cpp` under renamed functions and defines the
originals itself, so no engine source carries trace hooks.

## Starting a new game from the Starter sample

`samples/starter/` is the engine's neutral sample game (D-25, E9): a complete battle data set with invented
content that a new game copies and then changes, and the engine's neutral reference fixture (its goldens are
recorded with the engine's default random generator). The studio's "Starter" template copies the same files.

| File | What it holds |
| --- | --- |
| `structure.json` | nine stats (`hp`, `mp`, `ep`, `attack`, `magic`, `defence`, `protection`, `intelligence`, `agility`); the resources `hp` (faints when empty), `mp` and `ep` (persisted in the custom roles `current_mp` / `current_ep` like the hit points; `ep` regenerates in the turn's `regen` phase); the categories physical (Attack vs Defence), elemental (Magic vs Protection) and status; nine types in a tiered `multiply` chart; four statuses, two volatiles, one weather; 8 move slots, party size 6; the terms Rival, Temperament, Talent, Charm, Coins |
| `ruleset.json` | stat formulas `vital` (HP), `reserve` (MP, EP: their own bases and growth) and `core`; `ep_regen`; the damage, speed, accuracy and crit pipelines with points for talents and charms; the `effect_odds` pipeline; the Starter's own numbers ("Starter rules") |
| `battle.json` | the nine types, 28 moves (each type a physical move paying EP, an elemental move paying MP and a status move paying MP; `Strike`, the free and weak fallback move), nine talents (two latent slots per species), three charms, six temperaments, nine creatures (one per type), a presenter, three rivals and the engine messages |
| `effects/*.ceffect` | the moves', talents', charms' and conditions' logic (secondary effects and statuses roll `odds()`) |
| `assets.json` | placeholder art, simple shapes only: a front / back sprite per creature, the presenter, the backdrop |
| `README.md` | the Starter rules table (every number and formula) and how to change them |

**Intelligence and effect odds.** `stats[].odds: true` names the stat (here Intelligence) that the ruleset's
optional `effect_odds` pipeline reads for both battlers (`user_stat`, `target_stat`, besides `effect_chance`,
`effect_out_of` and both levels). Effect logic asks for odds with `odds(n, m)` (the user against the target) or
`odds(n, m, by, on)`; the Starter scales them by `(2 * user_stat + 60) / (user_stat + target_stat + 60)` (see
"Starter rules"), then runs the points
`odds` (the user's talent and charm: Insight, Focus Lens) and `resist` (the target's: Stubborn). A ruleset without
`effect_odds` gets the plain odds, drawn exactly like `chance(n, m)`. `battle::effect_odds_preview()` gives the
odds without drawing (AI, editors).

To start a game from it:

1. Copy `samples/starter/` into the game, e.g. as `data/` beside `reference/project.json`.
2. In `project.json`, name the copies by relative path: `"battle": "data/battle.json"`, `"assets":
   "data/assets.json"` (see `apps/starter_battle/reference/project.json`, which names the sample in place).
3. Rename and change what the game needs: stats, resources and costs (`resources`, each move's `cost`), types and
   the chart, the creatures, moves and logic; replace the shape art. `python3 tools/battle_compiler.py
   data/battle.json out.cpp --platform gba` checks the data against the GBA's limits.
4. Keep scenarios for the bench (`tests/bench/starter/*.json` are examples: each reproduces a golden transcript)
   and record the game's own expectations once its rules settle.

The sample's own checks: `starter_golden_tests` (transcripts and the odds grid in `tests/data/starter/golden`,
plus the resource and odds rules), `starter_sample_tests` (`tests/test_starter_sample.py`: the sample's contract,
every platform) and `bench_starter_*` (the bench `capsule_starter_bench`, default generator).

## Starter rules

The Starter's numbers and formulas are its own (C2, the developer's decision of 2026-10-19): no stage table, odds,
factor or formula shape of another game. Every rule is data in `samples/starter/` (ruleset constants, structure
records, effect logic), so a new game changes them there; the engine only runs the pipelines, points and formulas.
`samples/starter/README.md` has the same table with notes on changing it; `starter_golden_tests` checks the main
numbers directly (caps, stage tables, crit odds and factor, chart tiers, the roll, regen, escape, experience,
reward, the odds bound).

| Rule | Number or formula | Where | Why |
| --- | --- | --- | --- |
| Level cap | 50 | `structure.json` `rules.level_max` | a short game; the growth curves stay small (16 200 / 21 600 EXP) |
| Stat cap | 700 | `rules.stat_cap` | just above the highest HP the formula reaches (654); every stat formula stays within it |
| Staged stats | up to 1400 (`STAGED_CAP`: the stat cap x the top stage's x2) | `ruleset.json` `constants`, the `attack` / `defense` / `speed` lets, the `offense` point, the inputs `attack`, `defense`, `speed`, `foe_speed` | a staged stat is not clamped at the stat cap, so +3 and +4 still count at the top end (C4) |
| Genes | 0..15 per stat | `stat_genes` role, 4 bits | unchanged record layout |
| HP (`vital`) | `10 + base / 8 + level * (base + gene) / 22` (10..654) | `ruleset.json` `stat_formulas` | a small head start from the base, then linear growth: low levels are not one-hit |
| MP, EP (`reserve`) | `6 + level * (base + gene) / 30` (6..456) | `stat_formulas` | pools sized for 2-4 casts at low levels; no temperament |
| Attack .. Agility (`core`) | `(3 + base / 16 + level * (base + gene) / 36) * temperament / 100` (2..451) | `stat_formulas` | the same shape as HP with a smaller head start |
| Temperaments | raised stat x1.15, lowered stat x0.88 | `battle.json` `traits[].pct` | a slight net gain, so no temperament is a pure loss |
| Damage | `Attack * power * (level + 5) / ((Defence + 10) * 50) + 1` | `formulas.damage_base` | attack against padded defence: a low Defence does not multiply damage without bound |
| Damage order | base, crit, same type, chart, spread, weather point, roll, damage / guard points, at least 1 | `pipelines.damage` | the roll comes last so every factor is rolled together |
| Damage roll | 17 outcomes, x0.92 .. x1.08 (`ROLL_LOW` 92, `ROLL_SPAN` 17, `ROLL_DEN` 100) | `pipelines.damage` | a narrow roll, centred on x1 |
| Critical hit | x1.625 (`CRIT` 416 / 256); uses the target's Defence without its stages, the user's stages apply | `pipelines.damage` | a crit pierces the target's guard; boosts the user earned still count |
| Crit odds | stage 0..3: 1/20, 1/10, 1/5, 3/10 | `structure.json` `stages` (critical) | rare by default, a real choice at the top stage |
| Same type | x1.2 (`SAME_TYPE` 307 / 256) | `pipelines.damage` | a small bonus, so off-type moves stay useful |
| Spread | x0.7 (`SPREAD` 180 / 256) when a move hits more than one target | `pipelines.damage` | |
| Type chart | strong x1.75 (448), weak x0.6 (154), none x0 (decisive, "no effect"), `multiply` | `structure.json` `chart.tiers` | double strong x3.06, strong and weak x1.05, double weak x0.36 |
| Stat stages | -4..+4: x0.40, 0.55, 0.70, 0.85, 1, 1.25, 1.50, 1.75, 2.00 | `stages` (one shared table) | linear both ways: +25 % a stage up, -15 % a stage down |
| Accuracy | -4..+4: x0.6 .. x1.4, 10 % a stage, of accuracy stage minus evasion stage (clamped) | `stages` (accuracy, evasion) | |
| Self-hit | power 30, physical, the damage formula and roll without crit or chart | `rules.self_hit`, `pipelines.self_hit` | |
| Dazed | 1-3 moves; each move, 1 in 4 hits itself (power 30) | `volatiles`, `effects/conditions.ceffect` | |
| Messages | the engine's required keys, the optional `super_effective`, `not_very_effective`, `drained`, `healed`, `hp_full`, and its own `dazed`, `daze_hit`, `daze_end`, `became_dazed`, `already_dazed`, `staggered` | `battle.json` `messages` | a battle declares the messages its logic says itself (C4): neutral keys, no borrowed ones |
| Effect odds | `chance * (2 * user Int + 60) / (user Int + target Int + 60)`, then the `odds` / `resist` points | `pipelines.effect_odds` | equal Intelligence keeps the odds asked for; a clever user at most doubles them, a clever target drives them toward 0 |
| EP regen | `1 + (EP max - EP) / 16` each `regen` phase; MP does not regenerate | `formulas.ep_regen`, `resources` | small, and faster when the pool is nearly empty |
| Growth | brisk `6*n*n + 24*n`, steady `8*n*n + 32*n` (EXP to reach level n) | `structure.json` `growth` | quadratic: later levels cost more, but not steeply |
| Experience | `max(1, yield * foe level / 10 * (foe level + 10) / (own level + 10) * (4 + rank) / 4 / participants)` | `formulas.exp_gain` | the level difference counts: a stronger foe gives more, a weaker one less; a rival x1.25 |
| Reward | `base * (level + 8) / 2` (level: the rival's last creature) | `formulas.reward` | |
| Escape | out of 256: `96 + 128 * speed / (speed + foe speed) + 48 * failed attempts` | `formulas.escape_odds` | a share of the combined speed: never hopeless, never certain at first |
| Scorch | 1/12 of max HP a turn (21 / 256); the scorched battler's physical damage x0.7 | `statuses`, `conditions.ceffect` | |
| Blight | 1/10 of max HP a turn (26 / 256) | `statuses` | |
| Shock | loses 1 move in 5; speed x0.7 | `statuses`, `conditions.ceffect` | |
| Doze | 1-3 moves | `statuses` | |
| Storm | 5 turns; Lightning x1.3, Fire x0.7 | `weathers`, `conditions.ceffect` | |
| Talents | Blaze Heart Fire x1.4 at 2/5 HP or less; Quick Feet speed x1.3 at half HP or less; Thick Hide physical damage taken x0.75; Insight odds x1.5; Stubborn odds against it x0.5 | `effects/passives.ceffect` | |
| Charms | Focus Lens odds x1.25; Mana Drop +20 MP and Vigor Bar +20 EP at a quarter or less | `effects/items.ceffect` | |
| Bag (G2) | 4 pockets: Supplies 20 slots of up to 99 (as added), Orbs 8 of 99 (by index), Charms 16 of 9 (by name), Key 16 of 1 | `structure.json` `pockets` | the D-26 decision 2 pockets; the GBA holds 256 slots, the Starter uses 60 |
| Supplies | Tonic +20 HP (out of battle and, from G3, in battle), Mana Tea +15 MP, Spark Snack +15 EP, Remedy cures the status; a full pool, a fainted member or no status: no effect, nothing used | `battle.json` `items`, `effects/items.ceffect` (`use_field`) | small, cheap restores; the charms stay held items |
| Prices and shops | Tonic 30, Mana Tea 40, Spark Snack 25, Remedy 50, Orb 20 (its recruit use is G3's), Mana Drop / Vigor Bar 120, Focus Lens 200; Gate Pass is a key item; Corner Shop (supplies, Orb), Charm Stall (charms) | `items[].price`, `shops` | |
| AI | Scholar Wen adds `save_resources` (prefers cheaper moves); the other rivals do not | `battle.json` `parties[].ai` | the clever rival spends its MP carefully, the novice does not |
| Field rules (F1) | the lead is the first member with HP (no `lead` rule); the Ward charm (Charms pocket, 300 coins, used up) starts the timed field effect `ward`: 120 steps, started over when used again, survives a save, "The Ward charm's glow fades." when it ends; while it runs nothing below the lead's level is met; a lead with Quick Feet meets creatures half as often again (encounter rate x3/2); the encounter pipelines are the engine's defaults | `structure.json` `field_effects`, `battle.json` `items`, `effects/field.ceffect`, `effects/passives.ceffect` | a ward that is neither all nor nothing (strong creatures still come), a talent that only matters when it leads |

The pipeline has no target level, so the level difference enters damage only through the user's `level + 5` and
both battlers' stats; experience compares the two levels directly. All the arithmetic stays inside the ruleset
compiler's interval analysis (int32 everywhere; `damage_base` reaches 143 221 with a staged Attack of 1400 against
Defence 1, and the pipeline clamps it to 65 535).

## Starter battle

`capsule_starter_battle` (`apps/starter_battle`) plays the Starter sample like the battle sandbox: the three
rivals (Duo Ash & Bly in 2 vs 2), unowned battles (1 vs 1, 2 vs 2), a 2 vs 2 played by the AI on both sides and
"Heal party". The party menu and the command menu show MP and EP; the move menu shows four of up to eight moves at
a time (Up / Down past a page's edge turns it), each with its cost, type and power, and greys out a move whose cost
cannot be paid (with none payable, FIGHT uses Strike). It links the engine's default generator.

## Battle sandbox

The battle module's sandbox is `capsule_starter_battle` (above). The Gen-3
sample's sandbox (`sandbox_battle`: unowned creatures and data parties in
1 vs 1, 2 vs 2 and 3 vs 3, presenters with slides, fades and an entrance
sequence) moved with that sample to the reference-ruleset repo (E11), which
builds it against this engine with its Method-1 generator.

## Inventory, item use and the save image (G2, D-26 section 2)

**Pockets and item fields** are manifest data (`tools/battlec/inventory.py`,
emitted as `capsule::inventory::kInventory` beside the battle tables;
`battle_ids.h` names `POCKET_<NAME>` and `SHOP_<NAME>`):

```json
"pockets": [{"name": "supplies", "label": "Supplies", "capacity": 20, "quantity_max": 99, "sort": "added"},
            {"name": "key", "label": "Key", "capacity": 16, "quantity_max": 1}]
"items": [{"name": "Tonic", "pocket": "supplies", "price": 30, "use": "both", "target": "member"},
          {"name": "Gate Pass", "pocket": "key", "key": true}]
"shops": [{"name": "Corner Shop", "items": ["Tonic", "Mana Tea"]}]
```

- `pockets` (structure.json): each a slice of the bag's slots (`capacity`
  distinct items of up to `quantity_max`, default 99), ordered by `sort`
  (`added`, `index`: item id, `name`: display name). At most 8 pockets and
  256 slots on the GBA (1 KB of slots), 16 and 1024 elsewhere
  (`build_settings` PLATFORM_LIMITS `max_pockets` / `max_bag_slots`).
- Item fields (battle.json): `pocket` (none: the item cannot go into the bag),
  `price`, `use` (`none` | `field` | `battle` | `both`), `target` (`none` |
  `member` | `move_slot` | `battler`, battler only for use `battle`), `key`
  (never consumed or sold), `consumed` and `holdable` (both default to not
  key). A use needs its effect logic hook (`on use_field` / `on use_battle`)
  and a hook of a use the item does not have is refused. A party member
  holds only holdable items. `shops`: what the script statement `shop`
  offers, at the items' prices (each needs a price and a pocket).

**The bag** (`capsule/inventory/bag.h`, `src/inventory/bag.cpp`): fixed
`kMaxBagSlots` storage laid out at run time by `kInventory`; `add()` /
`remove()` are all or nothing (no pocket, a full pocket, past quantity_max),
`count()`, `room_for()`, `sort()` / `sort_all()`, `valid()`. No heap.

**Item use out of battle** (`capsule/inventory/use.h`): a `FieldUse` scope
opens a field context (`capsule/battle/field_context.h`: the party member's
record as battler 0 of a one-battler State the game lends, its held item and
passive left out) and runs the item's `use_field` handlers with the battle's
own effect host, so `restore`, `raise` (PP-Up style through the resource's
`raises` role), `cure`, `text` and `say` work as in battle; `fail` is "no
effect" and nothing is consumed. The new primitive `teach WHO MOVE` offers a
move as a level-up does: into a free slot at once, else a move offer the
game answers (`UseResult::LearnOffer`, then `answer_learn(slot)`; the item
is consumed only when learned; the default learn question is
`script::items::learn_open()`); it fails when the move is known. The scope's
end writes the member back (persisted resources and raises, status, moves).
`use_battle` compiles now; the battle's Item action runs it from G3 on.
Evolution items run `transform` (G4, below).

**Scripts**: `give_item("Tonic", 2) -> var(ok);`, `take_item(...)`,
`count_item(...) -> var(n);` (an item name of the manifest's `battle`, or an
operand holding its id), `open_bag -> var(item);`, `shop "Corner Shop" money
var(coins);` and `time_of_day -> var(t);` (statements, not keywords: older
scripts may use the words). They go through `capsule::script::items` in
`capsule/script/port.h`; the engine's defaults (`src/script/script_items.cpp`)
use the bag given to `items::attach()` and the text box's choice menu.

**The save image** (`capsule/save/save.h`, `src/save/save.cpp`; D-26
decision 1): one engine image whose sections are sized from the manifests:
the bag (4 bytes per manifest slot, 2 per pocket), the party (a count, then
party_size records), the storage boxes (0 bytes until G3) and the script
banks (flags, variables, name slots). A 32-byte versioned header with the
record layout's hash and a shape hash (an image of another shape is
refused), one FNV-1a checksum over the whole image, two copies when two fit
(a write goes to the older one). `save::write()`, `save::read()` (nothing in
RAM changes unless the checksum holds) and `save::load()` (read, then the
script VM sees the new flags). The medium is `capsule/hal/save.h`: the GBA
cartridge's 64 KB flash (`src/hal/gba/save_gba.cpp`, or 32 KB SRAM with
`CAPSULE_SAVE_SRAM`), a desktop file `<game>.sav` in the working folder or
`$CAPSULE_SAVE_DIR` (`src/hal/sdl2/save_file.cpp`, compiled per game), RAM
elsewhere. `tools/save_layout.py` works out the same sizes at build time:
`capsule_game()` writes `generated/<game>/save_layout.txt` and fails the
build when the image is over the platform's budget (`max_save_bytes`: the
GBA's 64 KB); its `image()` writes the same bytes as the engine (the studio's
test saves; `tests/data/inventory/save_image.bin` is checked against both).

**Time of day** (`capsule/core/clock.h`, D-26 decision 3): a context value
the game supplies by defining `clock::time_of_day()` (the default,
`src/core/clock_default.cpp`, answers 0), so carts without a clock work.

**Sandbox**: `capsule_sandbox_party` (`apps/sandbox_party`): the Starter's
party and bag (pools and status per member, the four pockets, item use with
member and move-slot choice and a learn question, the shops, the save image
on the platform's medium). L / R change page; the Save page writes (A), reads
(START) and resets (SELECT).

## Growth: transformations, forms and training values (G4, D-26 section 4)

Every rule, number and name is manifest data (`tools/battlec/growth.py`,
emitted as `capsule::growth::kGrowth` with the generated
`rules::growth_when()` and `rules::train_gain()`; `battle_ids.h` names
`TRIGGER_<NAME>`, `CONTEXT_<NAME>` (and `CONTEXT_<NAME>_<VALUE>`) and
`FORM_<NAME>`):

```json
"transformation": {"context_values": [{"name": "time_of_day", "values": ["day", "night"]},
                                      {"name": "game_version", "values": ["standard", "attack"]}],
                   "cancel": ["level_up"], "blocked": "held_item == still_stone && trigger != item_use",
                   "form_role": "form"},
"training": {"gain": "ev_gain", "stat_cap": 255, "total_cap": 510, "yields": {"hp": "ev_hp", "atk": "ev_atk"}}
"species": [{"name": "Puddlekit", "transforms": [
               {"into": "Puddlecrown", "when": "friendship >= 220 && time_of_day == day", "on": ["level_up"]},
               {"into": "Puddlemonk", "when": "friendship >= 220 && time_of_day == night", "on": ["level_up"]}]},
            {"name": "Strataknight", "forms": [
               {"name": "Attack", "base": [50, 180, 20, 180, 20, 150], "when": "game_version == attack"},
               {"name": "base", "when": "game_version == standard"}]}]
```

- **Rules** (`species[].transforms`: `into`, `when`, `on`, `takes_item`,
  `also`): the first whose triggers include the trigger and whose `when`
  holds wins. `when` is the ruleset expression language over the record's
  single-element roles (stored or unstored, through `battle::record_value()`;
  32-bit ones as `NAME_hi` / `NAME_lo`), per-stat roles as `ROLE.STAT`, the
  permanent stats as `stat.NAME`, the species' fields as `species.NAME`,
  `trigger`, `item` (item_use: the item used; trade: the held item; else 0;
  items by their lower-case identifier or `item.NAME`) and the context values
  (their value names are words). Triggers: `level_up`, `item_use`, `trade`,
  `after_battle`, `script`; without `on` a rule fires on every trigger its
  expression can hold for (worked out by the interval analysis; a rule that
  can never hold is an error). `takes_item` uses the held item up (a
  trade-with-item rule); `also` names a species made beside it into a free
  party slot (a copy without held item, nickname or status).
- **Context values**: `time_of_day` is the game's clock port
  (`capsule/core/clock.h`); every other one is the game's port function
  `growth::context_value(index)` (default: its manifest `default`;
  `src/growth/growth_port_default.cpp`, replaced at link time). Values are
  clamped to their declared ranges.
- **Cancelling**: by data (`blocked`: while it holds nothing changes) and by
  the player for the triggers `cancel` names (never `item_use`: the item's
  logic transforms or fails).
- **Forms** (`species[].forms`: `name`, `types`, `base`, `passives`, `front`,
  `back`, `when`, `on`; an entry named `base` with only `when` / `on` is the
  rule back to the species' own form): form 0 is the species itself; each
  form is a full species view the battle reads through
  `growth::species_view()`. The record's form is the custom role
  `transformation.form_role` (stored for forms chosen by rules; it may be
  derived and unstored for forms only data gives). In battle the effect
  primitive `form WHO NAME` changes a battler's form (types, passive when the
  form gives others, stats re-derived): battle only, the record keeps its
  own; in a field context (an item's use_field) the record takes it.
- **Training values** (`training`): each battler exp_policy shares a defeated
  foe's experience with gains, stat by stat, `rules::train_gain(yield, value,
  total, level)` of the foe species' yield field (a stat whose yield is 0
  gains nothing), capped by the total and then the stat cap; written back
  with the record; the battle's stats follow at the next level-up, a
  record's at once. Items change them with `train WHO STAT AMOUNT` (the caps
  apply; negative lowers).

**Trigger points**: once a battle is over the game looks at each member
(`growth::levelled_members(state, party)` says whose battlers levelled up:
their trigger is `level_up`, the others' `after_battle`); an item's
`use_field` logic runs `transform holder;` (the item_use rules with this
item; none holds: the logic fails, nothing is used up); `trade` after a
transfer import with the held item as the context's item; the script
statement `transform(<member>) -> var(v);` (0 nothing, 1 changed, 2 stopped)
through `capsule/script/growth_port.h` (defaults: a Change over the State
given to `script::growth::attach()`, the choice menu as the question, the
items port's learn question).

**A Change** (`capsule/growth/growth.h`) is a scope over a field context in
a State the game lends: it finds the rule (`Stage::Asking`, a
`EventKind::Transform` event `kTransformStart`), `go()` applies it (species,
its base form, types, passive, stats and resource maxima re-derived: the
hit points and pools gain what their maxima gained; the stored derived roles
that read the species or form re-derived, the others kept) and offers the
new species' moves of its level as a level-up does (`Stage::Learning`,
`answer_learn()`), `stop()` stops a cancellable one; the scope's end writes
the member back. `growth::find()` only looks.

**Sandbox**: `capsule_sandbox_party`'s Growth page (a level up with its
transformation, the after-battle form by the time of day, Glow Charm's form
toggle, Drill Notes' training) and Bloom Charm on the Bag page.

## The reference-ruleset repository (E11)

The Gen-3 prototype ruleset (its structure, rules, formulas, effect logic and
invented test content), its WP-2 goldens and bench scenarios, the
compatibility module (the Method-1 generator) and structure-only fixtures of
other battler shapes live in a separate repository, the `rules/` folder of
the private `CapsuleCreatures_PocketProject` (D-22 decision 3, D-23 section 4,
D-37). Its CMake adds this engine as a subproject (found by
`-DCAPSULE_ENGINE=<path>`, the `CAPSULE_ENGINE` environment variable, the
umbrella's `engine/` in `../../CapsuleCreatures-work` or
`../../CapsuleCreatures`, or a standalone `../../CapsuleCreatures_Engine-work`
or `../../CapsuleCreatures_Engine` checkout); the engine then
skips its own tests and apps (`CAPSULE_ENGINE_TESTS`, on only when the
engine is the top-level project) and exports `capsule_game()`,
`capsule_battle_data()`, `capsule_battle_library()`, `capsule_battle_test()`
(`cmake/capsule_battle.cmake`), `capsule_battle_bench()`, `capsule_rng()` and
`capsule_rng_test()`. Its test programs may include the engine's test support
headers (`tests/support/battle_transcript.h`, `tests/support/effect_mock_host.h`).
`tools/ci/check_principles.sh` keeps the Method-1 constants and the Gen-3
sample out of this tree.

### Map events (B-3)

A map's `"events"` hold its warps, triggers and signs; `"area"` names its
area root (B-2: the outdoor map whose area an interior is in). The world
names the scripts manifest they draw scripts, flags and variables from
(`"scripts"`; the game's project.json must name the same file) and the step
dispatcher's hooks (`"step_hooks"`). `tools/event_compiler.py` documents
every key; the map compiler calls it.

```json
"area": "town",
"events": {
  "warps":    [{"at": [4, 9], "to": "house", "warp": 0},
               {"at": [0, 12], "to": "route1", "warp": 0, "trigger": "press", "direction": "left"}],
  "triggers": [{"at": [5, 9], "script": "rival_stops_you", "var": "rival_state", "value": 0}],
  "signs":    [{"at": [3, 3], "script": "town_sign", "facing": "up"},
               {"at": [12, 10], "script": "hidden_potion", "flag": "found_potion"}]
}
```

- Records are bucketed per chunk: the chunk directory names each chunk's
  first event and count, and an event (12 B) holds its chunk-local cell, so
  a step looks at one chunk's events (`map::find_event()`). Warps (8 B)
  hold their landing cell, resolved by the compiler. An event on elevation 0
  matches any level.
- **Field.** Arriving on a cell checks, in pret's order, a trigger (with a
  variable condition, only while the variable holds its value), a step warp,
  the step hooks that are due, then the encounter roll; pressing A
  checks a sign on the faced cell (its facing rule, its flag clear);
  pressing a press warp's direction while standing on it warps. The first
  that fires stops the walker until the game takes it with
  `field::take_event()`: run its script, or call `field::warp()`.
- `field::enter(map, x, y, facing)` shows a map with the walker (counters
  kept), clears the map bank and, across areas, the area bank
  (`script::change_map()`), and runs the map's load scripts
  (`script::enter_map()`).
- **Step hooks:** every period-th step (a power of two; the compiler spreads
  phases) `field::game::step_hook(id)` runs in game code and may ask for a
  script. `world_ids.h` (generated with the world) names the hooks
  (`STEP_HOOK_...`) and the map indices (`MAP_...`).

Not built yet: weather and other coord-event kinds, secret bases and
counters (interacting across a counter tile).

### Map objects (B-21, pages D-17)

A map's `"objects"` (at most 15: sprite slots 1-15, the walker has slot 0)
are its NPCs, spotters (opponents who walk up to the player) and pickups. `tools/object_compiler.py` documents
every key.

```json
"objects": [
  {"name": "guard", "at": [8, 4], "sprite": "sprite:NPC_GUARD", "facing": "down",
   "script": "guard_talk", "flag": "guard_gone",
   "pages": [{"when": {"flag": "badge_1"}, "movement": "wander", "range": [2, 2], "script": "guard_after"}]},
  {"name": "youngster", "at": [12, 9], "sprite": "sprite:YOUNGSTER", "facing": "left",
   "movement": "look_around", "script": "youngster_battle",
   "spotter": {"sight": 4, "until": "beat_youngster"}}
]
```

- **Records** (ROM): a 32 B template per object (sprite key, cell,
  scripts and flags as 16-bit IDs, movement, range, sight, trigger, speed,
  frequency, options, its route and pages) and 28 B pages.
  `world_ids.h` names local IDs `OBJ_<MAP>_<NAME>`.
- **Spawning:** `field::enter()` spawns the map's objects whose hide flag is
  clear, each with its active page (see Eventing below).
- **Movement** (Gen III's set plus RPG Maker XP's): `static`, `look_around`
  (random turns), `wander` (random steps in its range box), `pace_vertical`
  / `pace_horizontal`, `approach` (steps toward the walker) and `custom` (its
  move route). Random choices use `rng::gameplay`. `schedule` waits for the
  world clock (C-05).
- **Collision:** objects block the walker and each other, on the cell they
  stand on and, while moving, the one they leave (`Blocked::Object`), unless
  they have the `through` option.
- **Talking:** A on a standing object with a script turns it to the walker
  and requests the script (`Source::Object`, `FieldEvent::object`); objects
  come before signs.
- **Spotters (E-8):** while the walker stands, a spotter whose sight is on
  (its until flag, which every spotter needs, clear) that faces it within its sight, with every cell
  between passable and free, spots it, waits 30 frames, walks up, and its
  script is requested (`Source::Spotter`). The first in local-ID order
  wins; there is no "!" emote yet.
- **Locking:** once a script is requested, objects stop moving on their own
  until the script ends (`field::release()`; the overworld runner calls it,
  and `field::lock()` for the scripts it starts itself); scripted walks and
  move routes still run. Scripts start with the object as `self`.
- **Scripts:** `move <obj> <dir> [steps];` (through anything, as pret's
  applymovement; waits until it stands), `face <obj> <dir>|player|self;`,
  `hide <obj>;`, `show <obj>;`, with `<obj>` = `self`, `player` or a local
  ID (the list index, as a number or a script constant). The overworld
  runner implements `script::world` (`capsule/script/port.h`).
- **Order on entering a map:** the map's on_transition and on_load scripts
  run first, then the objects spawn, so load scripts change objects
  through their hide and page flags (as pret).
- **After a script:** pages are chosen again (so a beaten spotter gets its
  new line at once) and Static objects turn back to their facing. Over a map
  whose layers use BG0 there is no text box.

Not built yet: schedules (C-05 world clock), the "!" emote, several
spotters approaching together (battle formats), the cheap per-chunk spotter
candidate list of E-8 (15 objects are scanned), and objects that are not
16 pixels wide on the cell grid.

### Eventing (D-17)

RPG Maker XP's eventing, on top of Gen III's scripts (design: D-17).

- **Pages:** an object's `"pages"` replace fields of its base record while
  their conditions hold: `when` {`flag` set, `var` >= `value` (or ==
  with `"equal": true`), `self` switch A-D on}, all of which must hold. The
  last page whose conditions hold wins (as RMXP); none means the base
  record. A page sets only the fields it names (sprite, script, movement,
  range, facing, trigger, speed, frequency, options, route) or hides the
  object (`"hidden": true`). Pages are chosen when the object spawns and
  again whenever a flag, variable or self switch changes.
- **Self switches:** four per object (A-D), kept in the world's RAM
  (`World::self_switches`, permanent like flags); only objects whose scripts or
  pages use them (or that ask with `"self_switches": true`) get bits.
  Scripts: `set self(A);`, `clear self(B);`, `if self(A) { ... }`.
- **Triggers:** `action` (A, the default), `player_touch` (the walker
  bumps into it, or steps onto it when it has `through`), `event_touch`
  (it bumps into the walker, or the walker into it), `autorun` (its script
  runs at once, with the player locked, until a page change stops it) and
  `parallel` (its script runs beside the player in a context of its own,
  up to `overworld::kMaxParallel`, restarted when it ends).
- **Move routes** (`tools/route_compiler.py`): an object's `"route"`
  `{"steps": [...], "repeat": true, "skip": false}` with `movement:
  "custom"`, or a script's `route <obj> [repeat] [skip] [nowait] { steps; }`
  and `wait_moves;`. Steps: `up`/`down`/`left`/`right [n]`, `random`,
  `toward`, `away`, `forward`, `backward`, `jump dx dy`, `wait n`, `face
  <dir>`, `turn right|left|around|random`, `set`/`clear FLAG`, `speed n`
  (1-4), `frequency n` (1-5), `walk_anim`/`step_anim`/`direction_fix`/
  `through`/`on_top on|off`, `sprite sprite:NAME`, `show`, `hide`. `skip`
  skips a step that is blocked instead of waiting; a repeating route
  needs a step that takes time. Routes respect collision unless `through`.
- **Speed, frequency and options** per object: speed 1-4 (2 is the
  walker's walking pace), frequency 1-5 (how often it moves on its own),
  `walk_anim`, `step_anim`, `direction_fix`, `through`, `on_top`.
- **Commands:** `input_number N -> var(v);` (N digits in the text box: Left /
  Right pick a digit, Up / Down change it, A enters, B gives 0), `wait_key -> var(v);`, `msg_pos bottom|middle|top;`,
  variable `*=`, `/=` and `%=`, conditions on `pos_x(obj)`, `pos_y(obj)`,
  `facing(obj)`, `map_index`, `steps` and `timer`, `warp MAP x y [dir]
  [transition];`, `place <obj> x y [dir];`, `scroll <dir> N [speed S];` /
  `scroll back;` (pans the camera), `tint r g b strength [frames];` / `tint
  off [frames];`, `flash r g b frames;`, `shake power frames;`, `timer
  start SECONDS;` / `timer stop;` (shown at the top right while no
  message is up; it counts through transitions) and `encounters
  on|off;`. Screen effects leave the text box's colors alone.

Not built yet (D-17): audio commands, pictures and battle animations,
weather (B-22), shops, name input and menu / save calls, panoramas and fog.
Parallel scripts should not show messages (nothing stops them yet).

### Locations, banners and transitions (D-17)

`world.json` names **locations** and each map says which it belongs to
(`"location": "park"`); maps of one location share its name, so walking
between them shows nothing. `tools/location_compiler.py` documents every
key.

```json
"transition": "fade",
"banner_styles": {"box": {"ink": "#f8f8f8", "shadow": "#303030", "fill": "#284878",
                          "border": "#f8f8f8", "frames": 120, "slide": 8},
                  "sign": {"frame": "banners/sign.png", "ink": "#302010", "shadow": "#c8a070",
                           "fill": "#e8c890", "frames": 120, "slide": 8}},
"locations": {"cave": {"name": "Echo Cave", "banner": "box",
                       "preview": {"show": "first", "sequences": [{"sequence": "sequences/cave_preview.json"}]}}}
```

- **Name banners**: entering a map of another
  location slides its name in at the top left in its banner style (a
  bordered box, or a 24x24 frame image cut into 8x8 corners and edges),
  holds it `frames` frames and slides it out. Banners use the text layer
  (BG0, palette bank 13); the game's HUD steps aside while one shows,
  and a message box closes it.
- **Location previews**: a
  location may play a sequence (C-02) full screen between the two halves
  of the transition, `always` or on the `first` visit (visits are kept in
  RAM, `Locations::visited`), each sequence with its own conditions.
- **Warp transitions:** a warp's `"transition"` (or a script's `warp`)
  picks `none`, `fade` (black), `white`, `mosaic`, `wipe_h`, `wipe_v` or
  `box`; without one the world's `"transition"` is used.
- **The overworld runner** (`capsule/overworld/overworld.h`) plays all of
  this: a game's overworld scene calls `overworld::start()` once and
  `overworld::update(intent)` each frame, and takes encounters with
  `field::take_encounter()`.

## Map sandbox

`capsule_sandbox_map` walks the sandbox world in `apps/sandbox_map/data`
(shapes only): a meadow with grass, flowers, tall grass, trees you walk
behind, an animated pond and river with bridges, ledges, an ice patch with a
rock to stop on, push arrows, a raised plateau with cliffs and stairs
(elevation 4 over ground at 3), an unpainted patch and a left-out chunk
(both show the border), and an L-shaped cave at negative coordinates.

It starts on the **showcase** map, which shows every map feature at once
(built with capsule-studio's core functions, the ones its editor and command
line use):

- Three tile layers whose draw priority is not their BG number: ground on BG1
  at priority 3, geometry (walls, rocks, fences, trunks) on BG3 at priority 2,
  canopy (tree tops, a stone arch, lamp globes, a bridge deck) on BG2 at
  priority 1, so the walker passes under the canopy.
- The attribute layer: water (not passable on foot), ice, ledges, a conveyor,
  tall grass with encounters, environment tags (1 on the grass, 2 on
  paths; see the cell value in the HUD) and walls.
- Two plateaus at elevation 5 with stairs (elevation 0) down to the ground
  at 3, joined by a bridge (15): at level 5 the walker crosses over the deck,
  at level 3 it walks under it along the path (the world gives elevation 5
  sprite priority 1).
- Unpainted cells inside a chunk (the border shows there and blocks), and a
  2x2 border with a pattern on each layer (a tree and animated flowers).
- Tile animations (water, flowers), an image sheet with a transparent color
  (drawn as shapes, `#ff00ff` keyed out) and metatiles mirrored with tile
  flips (the arch halves, the fence end, the deck's lower rail).

- D-pad walk, hold B to run; SELECT toggles the HUD (map, cell, elevation,
  behavior, motion, steps, encounter steps, why the last step was blocked).
- L: collision view (red blocked, blue not passable on foot, yellow moves the
  walker, green encounter; the digit is the elevation). R: noclip.
- START opens the menu: go to a map, night on / off, the vehicle class (A2:
  class 1, swim, crosses water at 2 pixels a frame and cannot run; F2: only
  water, and it steps back onto land on foot), use a Ward charm, swap the
  lead (F1), the first field action usable here, the flight menu, escape
  (F2) or back to the start.
- Encounters: the showcase's west grass is slot 0, the east grass slot 1
  (night-only creatures), the meadow uses a shared table; species come from
  the battle sandbox's invented data. There is no battle here: the HUD's
  bottom rows show the last encounter (species, level, method, slot) and how
  many there were.
- Encounter leads and timed field effects (F1, D-27): the sandbox carries the
  Starter's battle data, a party (Breezle level 5, whose talent Quick Feet
  raises the encounter rate x3/2 while it leads, and Sproutle) and three Ward
  charms; a `battle::LeadScope` attaches the lead rules. The HUD shows the
  lead (row 2: species, level, talent), the Ward's steps left and the last
  roll's skip level (row 3), and a debug line of each roll (row 16: its odds,
  the creature picked and how it ended: miss, skip, met). The menu's "use
  Ward charm" starts the timed field effect (120 steps; nothing below the
  lead's level comes; "The Ward charm's glow fades." when it ends) and "swap
  the lead" puts Sproutle first. Each encounter's creature is made through
  `battle::make_encounter()` (the lead's encounter_made hooks), then dropped.
- Field actions (F2, D-27 section 3): the party also has Tidepup (Tide Slam)
  and Glimmet (Mend Light). A facing the pond asks "The water is calm. Wade
  across?" (yes: swim, one step onto the water; stepping back onto land
  walks again); A facing the bush at 12,9 asks to clear it (it comes back
  when the map is entered again). The cave is dark (level 3); its sign at
  -6,-5 says whether anyone could light it, and the menu's "field action"
  row runs Light there. Walking into the cave from the showcase records the
  escape point (the menu's "escape" returns there); entering a map records
  its flight point (the menu's "flight menu" lists them). The HUD's blocked
  row shows DARK n on a dark map.
- Events (B-3; scripts in `data/scripts/`): a welcome trigger one step south
  of the start (once), a sign north of it (read facing up with A), a hidden
  berry at 12,10, a step warp at the showcase's west edge (0,9) to the cave
  (in the showcase's area) and a press warp at its east edge (39,9, press
  right) to the meadow, which warps back from 0,0 (press left). The step
  hook "pedometer" counts every 16th step and shows a message every 64.
  Messages and answers use the engine's text box (`script_port_hal.cpp`)
  over the map; the HUD hides while a script runs. The HUD's row 17 shows
  the last event, the berries found and the pedometer.
- Objects (B-21): a guide in the plaza (talking to him sends the gatekeeper
  off the meadow exit, with `move` and `hide`), a stroller wandering east of
  it, a sentry pacing between the hedges, a spotter on the path facing west
  (sight 4: it walks up and talks; afterwards its page gives it another
  line) and the gatekeeper standing on the meadow exit.
- Eventing (D-17): a jogger running laps on a custom move route (talk to it
  to race the timer to the meadow), a box with a self switch (a berry, then
  "empty"), a show-off north of the plaza (shake, flash and a tint), a
  lookout who pans the camera as far east as the number you enter, and an
  invisible puddle at 8,9 (`through`, `player_touch`).
- Locations (D-17): "Showcase Park" (box banner), "Sunny Meadow" (a wooden
  sign banner) and "Echo Cave" (box banner and, on the first visit, a
  preview sequence). The cave warps use a mosaic and a vertical wipe, the
  meadow warp a horizontal wipe; others fade.
- START: menu (pick a map, night on / off, or go back to the start).

## Test apps

Each engine system has a small app that exercises it alone, so it can be
checked on any target without the rest of a game. They build with every app
(`capsule_test_*`; `.gba` / `.elf` / `.cdi` on the consoles) unless
`CAPSULE_TEST_APPS` is `OFF`. L / R change page where an app has pages.

| App | System | What it shows | Controls |
|---|---|---|---|
| `capsule_input_viewer` | input (B-11) | buttons, mapped actions, shoulder filter events | — |
| `capsule_test_video` | `hal::video` | 1 palette banks 4-15; 2 tile and 2×2-block flips; 3 BG1-BG3 overlapping | 3: D-pad scroll, A layer, B priority, START on/off |
| | | 4 the four screen sizes across screen-block seams | 4: D-pad scroll (hold A: fast), B size |
| `capsule_test_vram` | VRAM upload queue (A-5), seeding (A-4) | queue statistics (cycles and bytes of the last VBlank, waiting and peak requests, overruns, early landings), how many frames a streaming job took, the gameplay stream's state; 180 streamed tiles on BG1 | A stream 32 KB, R stream it 8 times, B fade the tiles' bank, START flood the must-land ring |
| `capsule_test_script` | `capsule/script` | the demo scripts (messages, yes / no, menus, loops, call tables, a waiting native, a map trigger), the VM state, flags and variables | idle: Up/Down pick, A run, START enter the map, SELECT clear banks, L new day, R new area; running: the script's own prompts |
| `capsule_test_text` | `capsule/text` | 1 every glyph by code; 2 one line per palette bank; 3 newlines, edge columns, a 32×32 layer that wraps | 3: D-pad scroll |
| `capsule_test_sequence` | `capsule/seq` | picks a test manifest, runs it, shows stage, clocks, fade/blend levels, each track's position and wait, and the last port calls | menu: Up/Down, A run or toggle a condition, Left/Right list length; running: L+R pause; paused: A one frame, START resume, B menu |
| `capsule_sandbox_party` | `capsule/inventory`, `capsule/save` | the Starter's party (pools, status), bag pockets with item use, shops, the save image (sizes, copies, write / read) | L/R page; Bag: Left/Right pocket, A use (member, slot), B back; Shop: A buy; Save: A save, START load, SELECT reset |
| `capsule_test_audio` | `capsule/audio`, `hal::audio` | the audio tour (every sound, then fades / pause / volume / pitch / tempo / mono / a lag frame, then clips): its phase, each player's tracks, pause, sound and clock, the audio step's cycles (GBA: timer 1) | runs by itself; when done: A again, SELECT mono / stereo |
| `capsule_sandbox_audio` | `capsule/audio` | sound number and clip voice, output, reverb, the DMA counter, every player (tracks, pause, sound, clock, fade), the step's cycles | Up/Down sound, A play (B held: pause / resume), B stop, Left/Right clip, START play clip, L fade out, R fade in, SELECT mono |
| `capsule_test_battle` | `capsule/battle` | picks a scenario (1v1 unowned and party, 2v2, 3v3 AI vs AI, statuses, weather), steps the engine one unit at a time; positions with HP, status and stages, the event texts | menu: Up/Down, A start; running: A one step, START run / pause, R next seed, B menu |

The sequence app links its own port instead of `sequence_port_hal.cpp`: it
records every call on screen and simulates what flows wait on (a song lasts
600 frames, typed text keeps the printer busy 30 frames), and it answers the
game hooks from the menu. The sequence compiler emits stage, exit, condition
and list names for it (`<symbol>StageNames` etc.); they are `inline
constexpr`, so they reach a ROM only when something uses them.

New engine systems get an `apps/test_SYSTEM/` (reference/project.json and
src/test_SYSTEM.cpp, using `apps/common/test_kit.h`) and an entry in the
`foreach(app ...)` list in `CMakeLists.txt`.

## Audio output

`include/capsule/hal/audio.h` is what the audio core (`capsule/audio/core.h`) calls to play: on the GBA the
DirectSound FIFOs fed by DMA1 / DMA2 from the core's ring, timer 0 at the mix rate, the VCOUNT interrupt at line 150
(the DMA counter) and the core's step on every VBlank, lag frames included; on desktop an integer model of the sound
hardware (`src/hal/common/apu_model.h`: the four tone channels and the final mix, register writes applied at the
frame's start) sent to an SDL audio device (`CAPSULE_AUDIO=0`: none); headless the same model, every frame kept for
tests; Dreamcast silent (the core still steps, so timing is the same). Nothing of it is linked until a program calls
`audio::init()`.

`capsule_audio_render` (desktop and headless builds) renders a sound number (`--sound N`, `--song KEY`), a clip
(`--clip N` with `--clip-*` parameters) or the test apps' audio tour (`--tour`) of a build's audio table and writes
`segments.bin` (the ring segment mixed each frame), `tones.txt` (the ordered tone register writes), `state.jsonl`
(the core's state per frame), `out.wav` and `summary.json` (hashes of the four). `capsule_audio_render_hq` is the
same with the high-quality desktop mix (4x the rate; the ring and state are unchanged). `capsule_audio_render()` in
`cmake/parts/audio_hal.cmake` builds one for another manifest.

## Sequence sandbox

`capsule_sandbox_sequence` plays the five test sequences full-screen through
the engine's real port (`src/seq/sequence_port_hal.cpp`), with placeholder
shape art from `apps/sandbox_sequence/data/assets.json`. Menu: pick a
sequence, toggle its conditions, set the song length (audio is silent; songs
only run for that many frames, so song-timed waits and exits still happen),
A to run. Playing: the pad goes to the sequence; L+R pause, then A steps one
frame, START resumes, B stops. When a sequence ends, the result screen shows
the exit, the stage, the frame count, and which features the port could not
show yet.

What plays today: layers (regular and affine, with each image's declared
priority), sprites with animation, scale and rotation, semi-transparency,
screen fades, alpha blending, windows, palette fades, text pages and the
typed text box. Not yet, and listed on the result screen when used: the
view-only map (C-03), scanline effects (A-7), particle emitters (A-8),
palette animations (A-6/A-9) and audio. The OBJ window is drawn but changes
nothing visible yet, since sequences have no command that sets what it does.

## Scripts, flags and variables (B-1, B-2, B-4)

Game events are written in the engine's script language (`.cscript`, UTF-8)
and compiled ahead of time by `tools/script_compiler.py` into a bank of
bytecode, flags and variables (`capsule/script/data.h`). An app names its
manifest with the `scripts` project key; the build also writes
`script_ids.h` (`SCRIPT_…`, `FLAG_…`, `VAR_…`, `NATIVE_…`) for its code.

```
flag met_guide;                      // permanent unless a lifetime is given
flag lamp_on : map;                  // permanent | daily | area | map (B-2)
var coins : u8 0..99;                // u8 | u16 | u32, optional bank and range
const PRICE = 5;
native add(a, b) -> value;           // game actions, implemented by the game (P-13)
native beep(times) waits;            // natives that can take several frames

script shop {
    msg "Coins buy lamp oil.";       // or text:"Asset name" (a text asset)
    wait_button;
    while var(coins) >= PRICE { var(coins) -= PRICE; set flag(lamp_on); }
    if flag(lamp_on) { msgbox "The lamp is lit."; } else { msgbox "Not enough coins."; }
    choice ["Red", "Green", "None"] cancel 2 -> var(answer);
    close_msg;
}

map_scripts room {                   // a map of the world, by name
    on_load -> room_load;            // run at once on entering (must not wait)
    on_frame var(coins) == 3 -> coin_event;
}
```

Also: labels and `goto`, `call s;`, `call var(v) of [a, b];` (declared
targets), `return;`, `end;`, `switch` with `case 1, 2 { }` and `default { }`,
`var(v) = random(n);`, `yesno -> var(v);`, `delay n;`,
`role_get(slot, role, element) -> var(v);` and `role_set(slot, role, element,
value);` (a party record's role, engine or custom, by name or ID; element 0
when left out), the bag statements `give_item` / `take_item` /
`count_item`, `open_bag`, `shop "Name" money var(v);` and `time_of_day`
(G2: see "Inventory, item use and the save image"), and compile-time
`when [not] game(a, b) { }` for build variants (the manifest's `game` and
`games`). The full grammar is in the compiler's docstring.

**Manifest** (`capsule.scripts/1`): `sources`, `game` / `games`,
`stack_depth` (VM call depth, default 20, at most 32), and optionally
`assets` (checks `text:` references), `world` (checks map names) and
`battle` (the game's battle manifest: its record layout gives role names,
custom roles, element counts, widths and the party size for `role_get` /
`role_set` and record placeholders, and `script_ids.h` `ROLE_<NAME>`).

**Checks** (build errors): undefined or duplicate names, labels and
arities; recursion anywhere in the call graph (the cycle is named) and call
chains deeper than `stack_depth`; on_load / on_transition scripts and
trigger conditions that could wait; constants outside a variable's range
(P-18); trigger limits (16 per map, 8 flags / variables each); scripts over
64 KB. Unreachable statements and unused flags or variables are warnings.

**Runtime** (`capsule/script/script.h`):
- Bytecode uses 16-bit script IDs and 16-bit offsets inside each script (no
  pointers; identical scripts are stored once). Constants and variables are
  distinct operand types.
- A `Context` runs a script and its callers. `update()` runs until the
  script waits (message, button, menu, delay, a native returning Wait), ends,
  or has run 1,000 instructions.
- Variables clamp and saturate at their declared range (P-18). Flags are
  numbered bank by bank; `new_day()` clears the daily bank, and
  `change_map(from_area, to_area)` clears the map bank and, when the area
  root differs, the area bank. Storage is emitted with the bank, sized
  exactly; `save_size()` is the sum.
- `enter_map()` runs on_transition then on_load and arms the on_frame
  triggers. `poll_triggers()` re-checks only triggers whose flags or
  variables changed since they were last false. A true trigger fires again
  until its state changes, as pret's per-frame polling does.
- Presentation goes through `capsule/script/port.h`. The engine's port
  (`src/script/script_port_hal.cpp`) shows messages in the text box, yes / no
  on its last line, and menus in a box that fits them, and reaches party
  records (`script::party::role_get()` / `role_set()`) in the party given to
  `script::party::attach()`. Natives are `script::game::native()`, which the
  game defines.

Map events (B-3) run scripts from the field: see "Map events" under
Overworld maps. Map objects (B-21) add `move`, `face`, `hide` and `show`
(see "Map objects"); flags and variables used only by map events and
objects are not reported as unused when the manifest names the `world`.
Not built yet: sound commands and imported text tables with placeholders
(C-3).

## Sequences

`include/capsule/seq/` plays title flows, non-overworld cutscenes and credits
from `capsule.sequence/1` manifests compiled by `tools/sequence_compiler.py`.
A sequence is a list of stages; each stage runs up to 8 command tracks in
parallel and leaves through data-defined exits (all tracks done, skip keys,
timeout, song end, key combos, or an explicit goto). Commands cover layers,
sprites (including scale/rotation ramps), screen and palette fades, blending,
windows, text pages and text-box dialogue, a view-only map camera, music
sync, loops over game-supplied lists, and branches on game conditions.
Manifests also support macros and a condition-gated fast-forward.

The player talks to the rest of the engine through `sequence_port.h`.
`src/seq/sequence_port_hal.cpp` draws with the graphics modules (asset
table, backgrounds, sprites, palettes, text layers) and the video HAL's
fades, blending and windows; `capsule/seq/presenter.h` is what a scene calls
around it (reset, per-frame update, what could not be shown). Calls whose
system does not exist yet are recorded and otherwise ignored, so flows still
run with correct timing. Game hooks (conditions, lists, RNG
entropy) default to `src/seq/sequence_game_default.cpp`; a game supplies its
own. Test manifests live in `tests/data/sequences/`.

## Credits and licenses

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

Planned: a PowerPC Mac OS X target. SDL 1.2 still supports it but is LGPL
without a static-linking exception, so that target would link it as a dynamic
library (which satisfies the LGPL without extra obligations) or use a native
HAL; it also needs a community GCC 11+ toolchain for PowerPC Darwin.

## Working copies

This repository is the `engine/` submodule of the `CapsuleCreatures`
umbrella repository (with the toolkit as `toolkit/`; see the umbrella's
`docs/WORKSPACE.md`, D-37). The umbrella is checked out twice, side by side in
the workspace folder, and nothing from other projects belongs inside
`engine/`:

- `CapsuleCreatures-work/engine/` is the editable copy. Changes are made and
  pushed from here, after every change has been reviewed by a human before it
  is committed.
- `CapsuleCreatures/engine/` is a read-only reference checkout. It is updated
  only by hand (`git pull`, `git submodule update`) by the developer, after
  review.

A standalone clone (`CapsuleCreatures_Engine-work/` beside the other
repositories) works too: the toolkit, the rules and the importer look for it
after the umbrella's `engine/`.
