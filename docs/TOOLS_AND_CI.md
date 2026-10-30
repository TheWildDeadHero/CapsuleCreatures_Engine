# Tools and CI

The battle bench, using the engine as a subproject, and the checks that run on every push.

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
builds the classic sample's bench with its Method-1 generator for the WP-2 scenarios.

Only the bench has the debug pc -> source line table:
`tools/effect_compiler.py` records positions under `track_lines(True)` and
`emit_line_table()` writes an `effect::LineTable` (declared in
`include/capsule/battle/effect.h`); the battle data and every game build are
unchanged. The traces are link-time seams: the bench compiles the generator
and `src/battle/effect_host.cpp` under renamed functions and defines the
originals itself, so no engine source carries trace hooks.

## The reference-ruleset repository (E11)

The classic prototype ruleset (a rules preset in the style of the GBA-era
handheld games: its structure, rules, formulas, effect logic and invented test
content), its WP-2 goldens and bench scenarios, the compatibility module (the
Method-1 generator) and structure-only fixtures of other battler shapes live in
a separate, private reference-ruleset repository that is not part of this
release (D-22 decision 3, D-23 section 4, D-37). Such a repository's CMake adds
this engine as a subproject (found by `-DCAPSULE_ENGINE=<path>` or the
`CAPSULE_ENGINE` environment variable); the engine then
skips its own tests and apps (`CAPSULE_ENGINE_TESTS`, on only when the
engine is the top-level project) and exports `capsule_game()`,
`capsule_battle_data()`, `capsule_battle_library()`, `capsule_battle_test()`
(`cmake/capsule_battle.cmake`), `capsule_battle_bench()`, `capsule_rng()` and
`capsule_rng_test()`. Its test programs may include the engine's test support
headers (`tests/support/battle_transcript.h`, `tests/support/effect_mock_host.h`).
`tools/ci/check_principles.sh` keeps the Method-1 constants and the classic
sample out of this tree.

## Checks and CI

The static checks live in `tools/ci/` and run in CI on every push and pull
request (`.github/workflows/build.yml`); the umbrella repository's `./check`
runs them together with the toolkit's checks.

| Check | What it does |
|---|---|
| `tools/ci/check_principles.sh` | the engine's principles: no virtual dispatch, no floating-point types, no reference code, and the word lists below |
| `tools/ci/term_check.py` | word lists stored as salted SHA-256 digests (`tools/ci/terms/*.sha256`), so the public tree never spells the words it refuses; `check` prints `file:line:text` per hit, `hash` adds a word to a list |
| `tools/ci/check_style.py` | the code style: 3-space indents, braces on their own line in C and C++, snake_case functions and members, CamelCase types, PEP-8 Python with type annotations, no magic numbers |
| `tools/ci/code_map.py` | `build` writes `CODE_MAP.md` (the generated map of every source file); `check` fails when it is out of date |
| `Doxyfile` | the API reference (`cmake --build --preset <any> --target docs`); fails on anything undocumented |

The CI jobs (`.github/workflows/build.yml`):

| Job | What it builds and runs |
|---|---|
| checks | principles, style, code map, ShellCheck on the shell scripts, Doxygen |
| headless | the engine core and its tests on the software video model (no SDL) |
| linux | desktop debug (with tests) and release, GCC and Clang; checks that SDL2 found every Linux video and audio backend |
| macos | desktop debug (with tests) and release, universal `arm64` + `x86_64`, checked with `lipo` |
| windows | desktop debug (with tests) and release, MinGW-w64 GCC in MSYS2's UCRT64 environment |
| big-endian | the headless engine and tests cross-built for s390x and run under QEMU |
| gba | debug and release ROMs in devkitPro's devkitARM container; the ROMs are kept as build artifacts |
| dreamcast | KallistiOS from `tools/setup/setup_kallistios.sh`, debug and release; never fails the run (not a shipped target) |

Release downloads (the sample programs and the studio bundles) are built by
the umbrella repository's `release` workflow when a version tag is pushed.

