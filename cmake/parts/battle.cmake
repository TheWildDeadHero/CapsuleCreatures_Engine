# Battle engine (C-08): its sources and the battle compiler's files, shared by
# the engine library (CMakeLists.txt), capsule_game() and the battle tests
# (cmake/parts/battle_tests.cmake). Included from CMakeLists.txt before the
# engine library; a package that adds a battle module adds it here.

# The battle modules (src/battle; they share src/battle/internal.h), the effect VM and its host, and the bag and the
# storage boxes the Item action and a recruit use (G3). F1: encounter_plan.cpp (the Encounter op's plan; the rolls
# themselves, src/battle/encounter_lead.cpp, need the encounter module: the engine library and the tests that list
# them link them); effect_host.cpp reads the timed field effects of capsule/script/field_effects.h (the engine library
# links src/script/field_effects.cpp, a battle-library program the empty src/script/field_effects_none.cpp). The engine library adds battle_empty.cpp (the empty default
# data) and
# presenter_view.cpp (the optional presentation helper).
set(CAPSULE_BATTLE_SOURCES
   src/battle/ai.cpp
   src/battle/conditions.cpp
   src/battle/damage.cpp
   src/battle/effect_host.cpp
   src/battle/effect_vm.cpp
   src/battle/effects.cpp
   src/battle/encounter_plan.cpp
   src/battle/field_context.cpp
   # F2: what a field action's requirement reads (rules::member_knows() / member_passive())
   src/battle/field_requirement.cpp
   src/battle/item_action.cpp
   src/battle/move.cpp
   src/battle/parties.cpp
   src/battle/pipeline.cpp
   src/battle/progress.cpp
   src/battle/recruit.cpp
   src/battle/resources.cpp
   src/battle/state.cpp
   src/battle/teach.cpp
   src/battle/text.cpp
   src/battle/turn.cpp
   # G3: the Item action takes from the bag and a recruit may deposit into the storage boxes
   src/inventory/bag.cpp
   src/storage/storage.cpp
   # G4: growth's battle side and its rule lookup / Change scope (cmake/parts/growth.cmake lists its defaults)
   src/battle/growth_battle.cpp
   src/growth/growth.cpp
   # B1: combination (D-28 section 1): its methods' evaluation and Combination scope, the combine hook's Inherit op
   src/battle/combine_battle.cpp
   src/combine/combine.cpp
   # B2: eggs and the nursery (D-28 sections 2 and 3)
   src/combine/eggs.cpp
   src/combine/nursery.cpp)

# What a battle data compile depends on: the driver, its sections
# (tools/battlec, every module), the compilers it calls (rulesets, the point
# handlers' effect compiler and the script compiler it builds on, presenter
# sequences) and the platform limits (build_settings.py).
file(GLOB CAPSULE_BATTLEC_MODULES CONFIGURE_DEPENDS ${CAPSULE_ROOT}/tools/battlec/*.py)
set(CAPSULE_BATTLE_COMPILER_DEPENDS
   ${CAPSULE_ROOT}/tools/battle_compiler.py
   ${CAPSULE_BATTLEC_MODULES}
   ${CAPSULE_ROOT}/tools/ruleset_compiler.py
   ${CAPSULE_ROOT}/tools/effect_compiler.py
   ${CAPSULE_ROOT}/tools/script_compiler.py
   ${CAPSULE_ROOT}/tools/asset_compiler.py
   ${CAPSULE_ROOT}/tools/route_compiler.py
   ${CAPSULE_ROOT}/tools/build_settings.py
   ${CAPSULE_ROOT}/tools/sequence_compiler.py)
