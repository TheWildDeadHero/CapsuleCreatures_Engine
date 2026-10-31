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
