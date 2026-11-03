# Usage reports

Every game build records what it uses: the compilers write **usage records**, one build step merges them into the
game's **usage report**, and a GBA link can be compared against the report to find code that is linked but that
nothing in the game's data uses. This is the first step of the exclusion manifest (D-36 §5): a later step will
generate `capsule.exclude/1` from these reports so unused engine parts are never compiled.

Nothing here changes what a game builds: the generated sources are the same with and without the records.

## In the build folder

| File | What it is |
|---|---|
| `usage/<game>/<compiler>.json` | one record per compiler run (`capsule.usage/1`), or per VM bank (`capsule.vm-usage/1`) |
| `usage/<game>.json` | the merged report (`capsule.usage-report/1`), target `capsule_<game>_usage` (part of the default build) |
| `capsule_<game>.map`, `capsule_<game>.nm` | GBA only: the linker map and the sized symbol listing beside the ROM |
| `usage/<game>.memory.json` | GBA only: the program's memory by owner (`capsule.usage-memory/1`), written at every link |

`capsule_game()` passes `--usage` to every compiler it runs and merges the records at the end of the calling folder,
so a caller that compiles more for a game after `capsule_game()` (its audio table, its VM banks) adds its records with
`capsule_usage_add(capsule_<game> <record>...)`. `capsule_usage_dir(<var> <game>)` gives the records' folder.
`capsule_script_bank()`, `capsule_anim_bank()`, `capsule_audio_table()` and `capsule_sequence()` take `USAGE <record>`.

## What each compiler records

| Compiler | Categories |
|---|---|
| `script_compiler.py` | `script.op` (opcodes emitted), `script.native` (natives called), `route.op` (move routes' steps) |
| `asset_compiler.py` | `asset.kind` (assets per kind) |
| `map_compiler.py` | `map.map`, `map.event` (event kinds), `map.object` (objects, pages, routes), `map.encounter` (methods), `map.feature` (tile animations, doors, step hooks, override sets, dark maps, vehicle classes, locations, draw order), `route.op` |
| `sequence_compiler.py` | `seq.op` |
| `audio_compiler.py` | `audio.kind` (manifest sections), `audio.voice` (voice kinds), `audio.command` (song commands) |
| `battle_compiler.py` | `battle.feature` (types, moves, species, items, formats, parties, ... and the sections present), `effect.op` (effect program instructions) |
| `script_bank.py`, `anim_bank.py` | their `capsule.vm-usage/1` records, merged as `vm.<vm>` (`vm.<vm>.<table>`) |

The route steps and effect instructions are counted by the shared compilers themselves (`usage.note()`), so every
compiler that runs them (scripts, maps, battle data) records them without code of its own.

## The report

```json
{"schema": "capsule.usage-report/1", "game": "starter_battle", "platform": "gba",
 "records": [{"file": "battle.json", "source": "battle"}, ...],
 "libraries": ["battle_kit", "test_kit"], "hal": ["src/hal/gba/hal_gba.cpp", ...],
 "modules": {"battle": {"used": true, "by": ["battle.feature", "effect.op"]}, "seq": {"used": false, "by": []}, ...},
 "used": {"effect.op": {"push": 40, ...}, ...}}
```

`modules` has one entry per folder of `src/`. A module is used when some category (or a single entry, such as the
script op `ItemGive` for the inventory) names it; `core`, `hal`, `gfx` and `input` always are. The mapping is
`CATEGORY_MODULES` and `NAME_MODULES` in `tools/usage.py`.

A game's own C++ that calls an engine module directly (a sandbox's scene code, a battle kit) is not seen by any
compiler: such a module shows as unused in the report and as linked in the comparison below. That is what the
comparison is for.

## Commands

    python3 tools/usage.py show build/usage/<game>.json [--json]
    python3 tools/usage.py linkmap build/usage/<game>.json capsule_<game>.nm [--json] [--out cmp.json]
    python3 tools/usage.py merge RECORD... --game NAME --out REPORT [--platform P] [--libraries L...] [--hal SRC...]

`linkmap` takes the linker map or the `.nm` listing. It sums the bytes each module links, by the symbols' mangled
namespaces (`capsule::<module>::`), and lists the modules that link code while the report says nothing uses them,
largest first (`capsule.usage-linkmap/1`). Prefer the `.nm` listing: with link-time optimization (the release builds)
the map keeps only global symbols, so most code there falls back to the merged objects it came from.

The studio has the same as `capsule-studio usage REPORT [--link FILE] [--json]`.

## GBA memory by owner

    python3 tools/usage.py memory capsule_<game>.nm [--project reference/project.json] [--stack-reserve BYTES]
                                  [--game NAME] [--out FILE] [--json] [--check] [--demangler arm-none-eabi-c++filt]

`memory` reads the sized symbol listing and sums each **owner**'s bytes in four columns: IWRAM code, IWRAM data,
EWRAM and ROM. An owner is a symbol's first two namespaces from its mangled name (`capsule::audio`, `pocket::battle`,
a game's own namespace); the engine's namespaces of another module count as that module (`capsule::rng` is
`capsule::core`), a linker veneer counts as its target's, and names outside every namespace (C code, the C library,
code in the global anonymous namespace) are `(global)`. The regions' totals come from the linker's `__iheap_start`
and `__sbss_end__` when the listing has them.

The checks: IWRAM (code and data) against 32 KB less the stack reserve, EWRAM against 256 KB, then the game's
budgets, the `"memory_budgets"` object of its `"build"` settings (`tools/build_settings.py`):

```json
"build": {"memory_budgets": {"iwram": 30720, "ewram": 262144,
                             "owners": {"capsule::audio": {"iwram_code": 16384},
                                        "pocket": {"ewram": 32768}}}}
```

Regions: `iwram` (code and data), `iwram_code`, `iwram_data`, `ewram`. An owner budget may name a namespace at any
depth and covers every symbol below it. Each failed check names the owners that hold the most of its region and their
largest symbols (demangled with `--demangler`). `--check` exits 1 when one fails.

**At every GBA link.** `capsule_game()` links GBA programs through `tools/gba_link.py` (the target's linker
launcher). After a link that succeeds it prints the table and writes `usage/<game>.memory.json`; with
`CAPSULE_GBA_MEMORY_CHECKS` on, a passed budget fails the build and removes the program, so the next build links and
checks it again. When the link fails because the program does not fit, the launcher links once more beside it,
without `cmake/gba_memory.ld` and with `--noinhibit-exec`, into `capsule_<game>.does_not_fit.elf` (and its `.map`),
prints that program's table with the largest contributors and exits with the first link's failure. That program is
for measuring only; no ROM is made from it.
