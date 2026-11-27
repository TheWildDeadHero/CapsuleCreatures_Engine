# Inventory, item use and the save image tests (G2, D-26 section 2): the C++ program on the neutral fixture
# tests/data/inventory and the compiler's Python tests (tools/battlec/inventory.py, tools/save_layout.py).
# Included from CMakeLists.txt's test section.

# The bag, the field context's item use and the save image (RAM medium, empty script bank).
capsule_battle_test(inventory_tests ${CAPSULE_ROOT}/tests/data/inventory/battle.json
                    SOURCES tests/inventory_tests.cpp src/inventory/use.cpp src/save/save.cpp
                            src/hal/common/save_memory.cpp src/script/script_empty.cpp
                    DIR inventory
                    ARGS ${CAPSULE_ROOT}/tests/data/inventory/save_image.bin)
add_test(NAME battle_inventory_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_battle_inventory.py)

# D-36 §3.1 G1: the pockets' rules and stores on an inventory-only fixture (tests/data/bag_rules, compiled by
# tools/inventory_compiler.py) and the save image's Bag version 2, against the engine library (its RAM medium)
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/bag_rules_inventory.cpp ${CAPSULE_GENERATED_DIR}/bag_rules_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/inventory_compiler.py
         ${CAPSULE_ROOT}/tests/data/bag_rules/inventory.json ${CAPSULE_GENERATED_DIR}/bag_rules_inventory.cpp
         --ids ${CAPSULE_GENERATED_DIR}/bag_rules_ids.h --platform ${CAPSULE_PLATFORM}
   DEPENDS ${CAPSULE_ROOT}/tools/inventory_compiler.py ${CAPSULE_ROOT}/tools/battlec/inventory.py
         ${CAPSULE_ROOT}/tests/data/bag_rules/inventory.json
   COMMENT "Compiling the bag rules fixture's inventory")
add_executable(capsule_bag_rules_tests tests/bag_rules_tests.cpp ${CAPSULE_GENERATED_DIR}/bag_rules_inventory.cpp
               ${CAPSULE_GENERATED_DIR}/bag_rules_ids.h)
target_include_directories(capsule_bag_rules_tests PRIVATE ${CAPSULE_GENERATED_DIR})
target_link_libraries(capsule_bag_rules_tests PRIVATE capsule_engine)
add_test(NAME bag_rules_tests COMMAND capsule_bag_rules_tests)
