# Field rules tests (F1, D-27 sections 1 and 2): the C++ program on the neutral fixture tests/data/field (the lead
# rule, the lead rules of an encounter roll, the creature made, the timed field effects' effect logic) and the
# compiler's Python tests (tools/battlec/field.py, the encounter pipelines, the effect primitives, the script
# statements). Included from CMakeLists.txt's test section.

# The rolls need the encounter module and what it reads of the map module (world_empty: no world; the bank is the
# test's own, encounter::set_bank()).
capsule_battle_test(field_tests ${CAPSULE_ROOT}/tests/data/field/battle.json
                    SOURCES tests/field_tests.cpp src/battle/encounter_lead.cpp src/encounter/encounter.cpp
                            src/map/map.cpp src/map/world_empty.cpp src/gfx/background.cpp src/gfx/palette.cpp
                            src/hal/common/vram_queue.cpp src/hal/soft/soft_video.cpp
                    DIR field)
add_test(NAME field_rules_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_field_rules.py)

# The timed field effects in the save image: the fixture's script bank (its implicit variables) with the save image
# on the RAM medium, no script VM.
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/field_script.cpp ${CAPSULE_GENERATED_DIR}/field_script_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_compiler.py
         ${CAPSULE_ROOT}/tests/data/field/scripts.json ${CAPSULE_GENERATED_DIR}/field_script.cpp
         --ids ${CAPSULE_GENERATED_DIR}/field_script_ids.h --depfile ${CAPSULE_GENERATED_DIR}/field_script.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/save_shape.py
         ${CAPSULE_BATTLE_COMPILER_DEPENDS} ${CAPSULE_ROOT}/tests/data/field/scripts.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/field_script.d
   COMMENT "Compiling the field fixture's scripts")
add_executable(capsule_field_save_tests tests/field_save_tests.cpp src/save/save.cpp src/hal/common/save_memory.cpp
               ${CAPSULE_GENERATED_DIR}/field_script.cpp ${CAPSULE_GENERATED_DIR}/field_script_ids.h)
target_link_libraries(capsule_field_save_tests PRIVATE capsule_field_tests_data)
target_include_directories(capsule_field_save_tests PRIVATE ${CAPSULE_GENERATED_DIR})
add_test(NAME field_save_tests COMMAND capsule_field_save_tests)

# F2 (D-27 section 3): the field actions on the fixture (its world.json's targets): the requirements, the member,
# the dispatcher, the menu actions, the flight points; the script VM, the field and the world are the test's
# stand-ins (tests/field_actions_tests.cpp).
add_executable(capsule_field_actions_tests tests/field_actions_tests.cpp src/battle/field_actions.cpp
               src/script/field_actions.cpp
               ${CAPSULE_GENERATED_DIR}/field_script.cpp ${CAPSULE_GENERATED_DIR}/field_script_ids.h)
target_link_libraries(capsule_field_actions_tests PRIVATE capsule_field_tests_data)
target_include_directories(capsule_field_actions_tests PRIVATE ${CAPSULE_GENERATED_DIR} ${CAPSULE_ROOT}/src/battle)
add_test(NAME field_actions_tests COMMAND capsule_field_actions_tests)
add_test(NAME field_actions_compiler_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_field_actions.py)

# F3 (D-27 section 4): the step effects on their own neutral fixture tests/data/steps (statuses with the survive and
# faint rules, a bond role, a hatch counter, a passive starting a timed field effect, a run's script): the counters,
# the runs over a party, the messages; the script VM's variables and the overworld runner are the test's stand-ins
# (tests/step_effects_tests.cpp), the timed field effects the real module. The compiler sides in Python.
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/steps_script.cpp ${CAPSULE_GENERATED_DIR}/steps_script_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_compiler.py
         ${CAPSULE_ROOT}/tests/data/steps/scripts.json ${CAPSULE_GENERATED_DIR}/steps_script.cpp
         --ids ${CAPSULE_GENERATED_DIR}/steps_script_ids.h --depfile ${CAPSULE_GENERATED_DIR}/steps_script.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/save_shape.py
         ${CAPSULE_BATTLE_COMPILER_DEPENDS} ${CAPSULE_ROOT}/tests/data/steps/scripts.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/steps_script.d
   COMMENT "Compiling the step effects fixture's scripts")
capsule_battle_test(step_effects_tests ${CAPSULE_ROOT}/tests/data/steps/battle.json
                    SOURCES tests/step_effects_tests.cpp src/battle/step_effects.cpp src/script/step_effects.cpp
                            src/script/field_effects.cpp src/save/save.cpp src/hal/common/save_memory.cpp
                            ${CAPSULE_GENERATED_DIR}/steps_script.cpp ${CAPSULE_GENERATED_DIR}/steps_script_ids.h
                    DIR steps)
target_include_directories(capsule_step_effects_tests PRIVATE ${CAPSULE_GENERATED_DIR})
add_test(NAME step_effects_compiler_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_step_effects.py)
