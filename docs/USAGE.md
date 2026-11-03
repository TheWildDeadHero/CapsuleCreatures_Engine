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
