#!/usr/bin/env bash
# Rejects engine code that breaks P-8 (no virtual dispatch) or P-6 (no floats),
# battle, monster, text, field, map, encounter, overworld and script core code
# and the map sample data (apps/sandbox_map, tests/data/maps) that use franchise
# terms (P-13, R4), spec citations that collide with document
# IDs (D-22 decision 8), reference-ruleset data in the engine tree (the
# Method-1 constants and the Gen-3 sample, E11), franchise terms in the battle
# bench, the tools and the neutral battle data (C1), and any
# source or build file that refers to a ref_submodules/ folder of reference
# decompilations (D-2: reference only, never built or copied from; since D-37
# the engine has none: they are the private importer's submodules).
# Scans include/, src/ and apps/; comments are not stripped, so keep those words
# out of comments too. Exit status 1 on any hit.
set -euo pipefail
cd "$(dirname "$0")/../.."

status=0
check() {
   local label=$1 pattern=$2
   if grep -rnwE --include='*.h' --include='*.hpp' --include='*.cpp' "$pattern" include src apps; then
      echo "error: $label" >&2
      status=1
   fi
}

check "P-8: 'virtual' is not allowed in engine code" 'virtual'
check "P-6: floating-point types are not allowed in engine code" 'float|double|long double'

# P-13: the generic battle core names no franchise terms or series rules (they live in manifests). Whole
# words, any case, comments included, so keep them out of comments too.
p13_words='trainers?|pok(e|é)mon|natures?|abilit(y|ies)|prizes?|stab|wild'
if grep -rniwE --include='*.h' --include='*.hpp' --include='*.cpp' "$p13_words" \
   include/capsule/battle include/capsule/monster include/capsule/text src/battle; then
   echo "error: P-13: franchise terms are not allowed in the battle, monster and text core (use the generic names)" >&2
   status=1
fi

# C1: the battle bench (apps/battle_bench) and the engine's Python tools (tools/*.py, tools/battlec/*.py) name no
# franchise terms either (the same words, comments included), and the bench keeps no Gen-3 poke aliases (the
# volatiles confusion / protect_chain are the reference ruleset's names; pokes use "counters").
if grep -rniwE --include='*.h' --include='*.cpp' "$p13_words" apps/battle_bench ||
   grep -niwE "$p13_words" tools/*.py tools/battlec/*.py; then
   echo "error: P-13: franchise terms are not allowed in the battle bench or the engine's tools" >&2
   status=1
fi
# C10 (R7 finding 18): every engine tool, its docstrings included (tools/ and its subfolders, any Python file),
# names no franchise term or series item (pk / pkmn glyph names, poke-prefixed series words); "poke" alone is the
# engine's own effect word. This script names the words, so it is not scanned.
tool_words='pk|pkmn|pok(e|é)(mon|block|ball|dex|nav|mart|rus|flute|puff)s?|trainers?'
if grep -rniwE --include='*.py' --exclude-dir=__pycache__ "$tool_words" tools; then
   echo "error: C10: franchise terms are not allowed in the engine's tools, docstrings included" >&2
   status=1
fi
if grep -rniwE 'confusion|protect_chain' apps/battle_bench tools/bench_scenario.py; then
   echo "error: C1: the bench keeps only the generic poke keys (counters, status_counter, status_aux)" >&2
   status=1
fi
# C1: the neutral battle data (the engine's fixtures, bench scenarios and samples) uses neutral terms (Rival,
# Temperament, Talent, ...). JSON and effect sources, any case, whole words. C3: "wild" too (the neutral data says
# "unowned" and "{user} blocks the way!"; a game's own texts may say otherwise).
data_words='trainers?|pok(e|é)mon|natures?|abilit(y|ies)|prizes?|wild'
if grep -rniwE --include='*.json' --include='*.ceffect' "$data_words" tests/data/battle_* tests/data/effects \
   tests/data/combine tests/bench samples; then
   echo "error: P-13: franchise terms are not allowed in the engine's neutral battle data (use neutral terms)" >&2
   status=1
fi

# C6 (review4 finding 20): the creature systems' core (inventory, storage boxes, growth, the save image) names no
# franchise terms either, and no engine code or tool names a series' creature, item or ability as the model of a
# mechanic ("X-style": say what the mechanic does). Whole words, any case, comments included.
if grep -rniwE --include='*.h' --include='*.hpp' --include='*.cpp' "$p13_words" \
   include/capsule/inventory include/capsule/storage include/capsule/growth include/capsule/save \
   include/capsule/combine src/inventory src/storage src/growth src/save src/combine; then
   echo "error: P-13: franchise terms are not allowed in the inventory, storage, growth, save and combination core" >&2
   status=1
fi
series_words='shedinja|wurmple|tyrogue|castform|feebas|deoxys|pok(e|é)rus|everstone|cloud nine|macho brace'
if grep -rniwE --include='*.h' --include='*.hpp' --include='*.cpp' "$series_words" include src apps ||
   grep -rniwE --include='*.py' --exclude-dir=__pycache__ "$series_words" tools; then
   echo "error: C6: a series' creature, item or ability names no engine mechanic (describe what it does)" >&2
   status=1
fi

# B1 (D-28): combination is one generic mechanism; its core and its neutral fixture name no series' combination
# terms (the fixture's races and families are invented). Whole words, any case, comments included.
combine_words='demons?|personas?|day ?care|pok(e|é)mon|digimon|scouts?|dittos?'
if grep -rniwE --include='*.h' --include='*.cpp' "$combine_words" include/capsule/combine src/combine \
   src/battle/combine_battle.cpp src/script/script_combine.cpp ||
   grep -rniwE --include='*.json' --include='*.ceffect' --include='*.py' "$combine_words" tests/data/combine \
   tools/battlec/combine.py; then
   echo "error: B1: series' combination terms are not allowed in the combination core and its fixture" >&2
   status=1
fi

# D-22 (b) words: the field, map, encounter, overworld and script core name no series' mechanics either (E7).
field_words='wild|trainers?|pok(e|é)mon|repel|item ?balls?|fishing|surf(ing)?'
if grep -rniwE --include='*.h' --include='*.hpp' --include='*.cpp' "$field_words" \
   include/capsule/field include/capsule/map include/capsule/encounter include/capsule/overworld \
   include/capsule/script src/field src/map src/encounter src/overworld src/script; then
   echo "error: P-13: franchise terms are not allowed in the field, map, encounter, overworld and script core" >&2
   status=1
fi

# R4: the map sample data (the map sandbox and the map test fixture) keeps neutral names too. Any case, and
# underscores separate words here (beat_trainer, ride_surf), so no -w; binary files (sheets, banners) are skipped.
sample_words='(^|[^a-z])(surf(ing)?|trainers?)([^a-z]|$)'
if grep -rniIE "$sample_words" apps/sandbox_map tests/data/maps; then
   echo "error: R4: franchise terms are not allowed in the map sample data (swim, rival, ...)" >&2
   status=1
fi

# D-22 decision 8: battle concepts cite their documents (D-18 AI, D-20 parties and controllers, D-22 / D-23 effects
# and hooks), not the design spec's D-1 / D-3 / D-4, which collide with the documents' own IDs.
if grep -rnwE --include='*.h' --include='*.hpp' --include='*.cpp' --include='*.py' 'D-(1|3|4)' \
   include src apps tools; then
   echo "error: D-22: cite battle concepts by their document IDs (D-18, D-20, D-22, D-23), not D-1 / D-3 / D-4" >&2
   status=1
fi

# D-22 decisions 3 and 4, E11: the engine tree holds no reference-ruleset data. The Method-1 LCG constants
# (the multiplier and increment, decimal or hex) live only in the reference repo's compat module, and the Gen-3
# sample (its creatures, parties and the D-23 section 1.7 examples' names) only in the reference repo; the engine's
# fixtures are the Starter sample and the neutral data sets. Binary files are skipped; this script names the
# patterns, so it is not scanned.
engine_tree='include src apps tests tools cmake samples assets docs CMakeLists.txt CMakePresets.json README.md'
method1='(^|[^0-9A-Za-z_])(1103515245|0x41C64E6D|24691|0x6073)u?([^0-9A-Za-z_]|$)'
gen3_names='Cindercub|Rippleray|Fernling|Zappit|Pebblor|Gustling|Murkmoth|Lumbird|Rookie Ana|Ranger Bo|Wanderer Eli'
gen3_names="$gen3_names|Swift Swim|Leftovers|Intimidate|Levitate|Baton Pass|Flame Wheel"
# shellcheck disable=SC2086
if grep -rniIE --exclude=check_principles.sh --exclude-dir=__pycache__ "$method1" $engine_tree; then
   echo "error: E11: the reference ruleset's Method-1 LCG constants are not allowed in the engine (compat module)" >&2
   status=1
fi
# shellcheck disable=SC2086
if grep -rnIE --exclude=check_principles.sh --exclude-dir=__pycache__ "$gen3_names" $engine_tree; then
   echo "error: E11: Gen-3 sample data belongs to the reference-ruleset repo (use the Starter or a neutral set)" >&2
   status=1
fi
for gone in tests/data/battle apps/sandbox_battle tests/support/random_method1.cpp; do
   if [ -e "$gone" ]; then
      echo "error: E11: $gone is the reference repo's (the Gen-3 sample, its sandbox or the Method-1 generator)" >&2
      status=1
   fi
done

# D-2: nothing may include or build from the reference decompilations (D-37: this repository has none; they are
# the private importer's ref_submodules/).
# One exception (E13b), matched exactly: tests/test_code_map.py (added outside the packages, sync35) names
# "ref_submodules/pret" as a folder the code map generator must skip (the generator is shared with the importer,
# which has that folder); it creates the folder in a temporary directory and never reads anything real.
ref_allowed='^tests/test_code_map\.py:[0-9]+:.*"ref_submodules/pret"'
ref_hits=$(grep -rn 'ref_submodules' include src apps tests cmake CMakeLists.txt CMakePresets.json \
   $(find platform -name Makefile -o -name '*.mak' 2>/dev/null) 2>/dev/null | grep -vE "$ref_allowed" || true)
if [ -n "$ref_hits" ]; then
   echo "$ref_hits"
   echo "error: D-2: engine sources and build files must not refer to ref_submodules/" >&2
   status=1
fi

if [ "$status" -eq 0 ]; then echo "principles check passed"; fi
exit "$status"
