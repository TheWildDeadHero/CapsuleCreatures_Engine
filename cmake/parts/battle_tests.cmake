# Battle tests (desktop and headless): the battle engine and creature records
# against the engine's neutral battle data sets (each built into its own test
# program: kData is per program; cmake/capsule_battle.cmake), the Starter
# sample and its goldens, and the battle compiler's Python tests. Included from
# CMakeLists.txt inside its test section (enable_testing() has run). The Gen-3
# reference sample, its goldens and its tests live in the reference-ruleset
# repository (D-23 section 4, E11).

# The battle compiler and the ruleset compiler (formulas, pipelines, points) on the Starter sample and the neutral
# data sets.
add_test(NAME battle_compiler_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_battle_compiler.py)
# The split battle manifest (battle.json + structure.json, E5s) and the migration tool tools/battle_split.py.
add_test(NAME battle_split_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_battle_split.py)
# Manifest resources (D-25, E8): battlec/resources.py, the bench's resource keys and the migration helper
# tools/battle_resources.py.
add_test(NAME battle_resources_compiler_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_battle_resources.py)
# Ruleset pipelines and points (D-20 D): the Python evaluator against the Starter sample's pipelines.
add_test(NAME ruleset_pipeline_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_ruleset_pipelines.py)
# The battle compiler's .ceffect sources with editors' unsaved text (battlec/logic.py load_sources).
add_test(NAME battle_logic_sources_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_battle_logic_sources.py)

# Manifest-defined stats (D-20 C): three stats and 4-bit genes (tests/data/battle_stats3).
capsule_battle_test(battle_stats3_tests ${CAPSULE_ROOT}/tests/data/battle_stats3/battle.json
                    SOURCES tests/battle_stats3_tests.cpp DIR battle_stats3)

# Layout v2 (D-23 section 2): the three-stat data again with two move slots, a 5-code nickname and 7-bit levels
# (tests/data/battle_stats3/slots2.json).
capsule_battle_test(battle_stats3_slots2_tests ${CAPSULE_ROOT}/tests/data/battle_stats3/slots2.json
                    SOURCES tests/battle_stats3_slots2_tests.cpp DIR battle_slots2)

# The effect VM inside battles (D-23 section 1, E4b): moves and a passive as effect code (tests/data/effects/battle:
# lock-in, switch pass, credit, script targeting, ask / answer, a yielding multi-hit, nested hooks past the depth
# limit).
capsule_battle_test(battle_vm_tests ${CAPSULE_ROOT}/tests/data/effects/battle/battle.json
                    SOURCES tests/battle_vm_tests.cpp DIR battle_vm)

# Conditions as manifest records (D-22 A3-A5, E5): volatiles (a word, a lock-in, a charge with HUD events, a tag),
# side conditions with layers and turns, a field condition, the taken record and statuses with escalation and a
# length (tests/data/effects/conditions; its ruleset is the VM set's).
capsule_battle_test(battle_conditions_tests ${CAPSULE_ROOT}/tests/data/effects/conditions/battle.json
                    SOURCES tests/battle_conditions_tests.cpp DIR battle_conditions
                    DEPENDS ${CAPSULE_ROOT}/tests/data/effects/battle/ruleset.json)

# Manifest stages and type chart tiers (D-22 A6 / A7, E6a): a sequential chart with non-power-of-two and decisive
# tiers, stages with their own ranges, tables, odds and a formula (tests/data/battle_tiers).
capsule_battle_test(battle_tiers_tests ${CAPSULE_ROOT}/tests/data/battle_tiers/battle.json
                    SOURCES tests/battle_tiers_tests.cpp DIR battle_tiers)

# The turn structure from the manifest (D-22 A11, E6b): a `turn` that differs from the default in every part
# (brackets, position ties, end-of-turn phase order and sources, sides once, dispatch order, start send-outs;
# tests/data/battle_turn; its ruleset is the VM set's).
capsule_battle_test(battle_turn_tests ${CAPSULE_ROOT}/tests/data/battle_turn/battle.json
                    SOURCES tests/battle_turn_tests.cpp DIR battle_turn
                    DEPENDS ${CAPSULE_ROOT}/tests/data/effects/battle/ruleset.json)

# E13b: the battle primitives (move / side / volatile points, the move_type point, silent and reset stages, typed
# before_damage, trapping, redirection, delayed effects, rewards, ending the battle, targeted_by, species fields, the
# transcript's owner texts) on tests/data/battle_moves.
capsule_battle_test(battle_moves_tests ${CAPSULE_ROOT}/tests/data/battle_moves/battle.json
                    SOURCES tests/battle_moves_tests.cpp DIR battle_moves)

# G1 (D-26 section 1): the battle gaps (a volatile bound to its source, delayed hits with a snapshot of their user,
# use_move refusing a flagged move, a decoy's absorbed hit) and AI profiles (the ai_score hook and pipeline,
# switching rules) on tests/data/battle_gaps; the compiler side (battlec/ai.py, battlec/delayed.py, bound_to,
# tools/battle_ai.py) in tests/test_battle_gaps.py.
capsule_battle_test(battle_gaps_tests ${CAPSULE_ROOT}/tests/data/battle_gaps/battle.json
                    SOURCES tests/battle_gaps_tests.cpp DIR battle_gaps)
add_test(NAME battle_gaps_compiler_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_battle_gaps.py)

# Progression from the manifest (D-22 A8 / A12, E6c): learn policy (request, first_n), experience policy (party
# share, opposing sides, owner_id outsiders), generation list (one-draw seed, constants, derived roles) and a
# move resource's max formula with raises (tests/data/battle_progress).
capsule_battle_test(battle_progress_tests ${CAPSULE_ROOT}/tests/data/battle_progress/battle.json
                    SOURCES tests/battle_progress_tests.cpp DIR battle_progress)

# E13a record features (tests/data/battle_progress/records.json): a wide seed drawn low half first, bit operators
# and a species field in derived expressions, an unstored custom role computed on read, rederive() and the transfer
# import that re-derives the stored derived roles a sender lacks (the data set's own transfer table).
capsule_transfer_table(${CAPSULE_ROOT}/tests/data/battle_progress/records.json
                       ${CAPSULE_GENERATED_DIR}/battle_records/transfer.cpp)
capsule_battle_test(battle_records_tests ${CAPSULE_ROOT}/tests/data/battle_progress/records.json
                    SOURCES tests/battle_records_tests.cpp src/monster/transfer.cpp
                            ${CAPSULE_GENERATED_DIR}/battle_records/transfer.cpp
                    DIR battle_records
                    DEPENDS ${CAPSULE_ROOT}/tests/data/battle_progress/ruleset.json)
add_test(NAME battle_records_compiler_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_battle_records.py)

# Passive slots (D-25 decision 6, C4): a hidden slot only data gives, then two rolled ones; an empty rolled slot
# gives the first filled rolled slot's passive, never the hidden one (tests/data/battle_progress/passives.json).
capsule_battle_test(battle_passive_slots_tests ${CAPSULE_ROOT}/tests/data/battle_progress/passives.json
                    SOURCES tests/battle_passive_slots_tests.cpp DIR battle_passives)

# Resources from the manifest (D-25, E8): hit points, MP (persisted in a custom role) and EP (refilled every battle,
# regenerating in the turn's regen phase), move costs as numbers and a formula, an unpayable move, a free fallback
# move and resource effect logic (tests/data/battle_resources).
capsule_battle_test(battle_resources_tests ${CAPSULE_ROOT}/tests/data/battle_resources/battle.json
                    SOURCES tests/battle_resources_tests.cpp DIR battle_resources)

# The Starter sample (D-25, E9): the engine's neutral sample game and reference fixture (samples/starter: nine
# stats, MP / EP resources with costs and regen, nine types, effect odds through Intelligence), against its neutral
# goldens (tests/data/starter/golden, recorded once in E9) and the sample's contract checks
# (tests/test_starter_sample.py). Its data library is shared with the transfer tests (cmake/parts/transfer.cmake).
capsule_battle_library(capsule_starter_data ${CAPSULE_ROOT}/samples/starter/battle.json DIR starter)
add_executable(capsule_starter_golden_tests tests/starter_golden_tests.cpp)
target_link_libraries(capsule_starter_golden_tests PRIVATE capsule_starter_data)
add_test(NAME starter_golden_tests
         COMMAND capsule_starter_golden_tests ${CAPSULE_ROOT}/tests/data/starter/golden)
add_test(NAME starter_sample_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_starter_sample.py)

# The presenter view (RA1): presenters that fade share the one blend (tests/data/battle_presenters: fading and
# sliding presenters on the resources set's structure). Built with the video model and the text layer.
capsule_battle_library(capsule_presenter_data ${CAPSULE_ROOT}/tests/data/battle_presenters/battle.json
                       DIR battle_presenters
                       DEPENDS ${CAPSULE_ROOT}/tests/data/battle_resources/structure.json
                               ${CAPSULE_ROOT}/tests/data/battle_resources/ruleset.json
                               ${CAPSULE_ROOT}/tests/data/battle_resources/logic.ceffect)
