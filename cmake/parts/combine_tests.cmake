# Combination tests (B1, D-28 section 1): the C++ program on the neutral fixture tests/data/combine and the
# compiler's Python tests (tools/battlec/combine.py, the effect and script compilers' combine parts). Included from
# CMakeLists.txt's test section.

# Previews, the Combination scope, inheritance, moves, consumed and kept inputs (the field context, the boxes).
capsule_battle_test(combine_tests ${CAPSULE_ROOT}/tests/data/combine/battle.json
                    SOURCES tests/combine_tests.cpp
                    DIR combine)
add_test(NAME battle_combine_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_battle_combine.py)

# B2 (D-28 sections 2 and 3): eggs, the nursery, the egg cycle and the checks through a StepScope, the nursery
# port and the save image's Nursery section on the same fixture with its scripts (tests/data/combine/scripts.json:
# the step effects' counters, egg_member, the hatch script); the script VM's variables, the overworld runner and the
# text box are the test's stand-ins (tests/nursery_tests.cpp). The compiler sides in Python. C9: the default combine
# port (`combine open` draws after yes) and the encounter lead (eggs never lead) too.
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/combine_script.cpp ${CAPSULE_GENERATED_DIR}/combine_script_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_compiler.py
         ${CAPSULE_ROOT}/tests/data/combine/scripts.json ${CAPSULE_GENERATED_DIR}/combine_script.cpp
         --ids ${CAPSULE_GENERATED_DIR}/combine_script_ids.h --depfile ${CAPSULE_GENERATED_DIR}/combine_script.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/save_shape.py
         ${CAPSULE_BATTLE_COMPILER_DEPENDS} ${CAPSULE_ROOT}/tests/data/combine/scripts.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/combine_script.d
   COMMENT "Compiling the combination fixture's scripts")
capsule_battle_test(nursery_tests ${CAPSULE_ROOT}/tests/data/combine/battle.json
                    SOURCES tests/nursery_tests.cpp src/battle/step_effects.cpp src/script/step_effects.cpp
                            src/script/field_effects.cpp src/script/script_nursery.cpp src/script/script_menu.cpp
                            src/script/script_combine.cpp
                            # C9: the encounter lead (eggs never lead) with what its rolls read
                            src/battle/encounter_lead.cpp src/encounter/encounter.cpp src/map/map.cpp
                            src/map/world_empty.cpp src/gfx/background.cpp src/gfx/palette.cpp
                            src/hal/common/vram_queue.cpp src/hal/soft/soft_video.cpp
                            src/inventory/use.cpp src/save/save.cpp src/hal/common/save_memory.cpp
                            ${CAPSULE_GENERATED_DIR}/combine_script.cpp ${CAPSULE_GENERATED_DIR}/combine_script_ids.h
                    DIR combine_nursery)
target_include_directories(capsule_nursery_tests PRIVATE ${CAPSULE_GENERATED_DIR})
add_test(NAME battle_nursery_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_battle_nursery.py)

# C9 (the D-28 review): the Starter's Grove (two slots, same kin, the first held record's base stage as the egg) on
# the Starter's data library (cmake/parts/battle_tests.cmake capsule_starter_data).
add_executable(capsule_starter_grove_tests tests/starter_grove_tests.cpp)
target_link_libraries(capsule_starter_grove_tests PRIVATE capsule_starter_data)
add_test(NAME starter_grove_tests COMMAND capsule_starter_grove_tests)
