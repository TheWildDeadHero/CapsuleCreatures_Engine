# Growth tests (G4, D-26 section 4): the C++ program on the neutral fixture tests/data/growth and the compiler's
# Python tests (tools/battlec/growth.py). Included from CMakeLists.txt's test section.

# Rules, the Change scope, item use, forms and training values (the field context, the bag, item use).
capsule_battle_test(growth_tests ${CAPSULE_ROOT}/tests/data/growth/battle.json
                    SOURCES tests/growth_tests.cpp src/inventory/use.cpp
                    DIR growth)
add_test(NAME battle_growth_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_battle_growth.py)
