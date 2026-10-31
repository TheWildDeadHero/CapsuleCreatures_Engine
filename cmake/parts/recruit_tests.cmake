# The battle's Item action, recruitment and the storage boxes (G3, D-26 section 3): the C++ program on the neutral
# fixture tests/data/recruit and the compilers' Python tests (tools/battlec/recruit.py, storage.py, the AI's item
# rules, the save image's Boxes section). Included from CMakeLists.txt's test section.

# The Item action, recruits into the party and the boxes, refusals, the AI's item rules, the storage module and the
# Boxes section of the save image (RAM medium, empty script bank).
capsule_battle_test(recruit_tests ${CAPSULE_ROOT}/tests/data/recruit/battle.json
                    SOURCES tests/recruit_tests.cpp src/save/save.cpp src/hal/common/save_memory.cpp
                            src/script/script_empty.cpp
                    DIR recruit)
add_test(NAME battle_recruit_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_battle_recruit.py)
