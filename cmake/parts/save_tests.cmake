# The save image's migrations (C6, review4 findings 1, 2, 5, 6): the C++ program loads the older-shape images
# tests/data/save/*.bin (written by tests/test_save_migration.py from changed copies of the recruit fixture) into the
# recruit fixture's build with the save fixture's script bank (tests/data/save/scripts.json and its save history),
# on a flash-like test medium (4 KB sectors, AND programming, cut writes and failing reads); the Python test checks
# the writer and tools/save_shape.py. Included from CMakeLists.txt's test section. C7: save_stage_tests (below).

# The save fixture's script bank (its recorded save shapes give the remaps).
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/save_script.cpp ${CAPSULE_GENERATED_DIR}/save_script_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_compiler.py
         ${CAPSULE_ROOT}/tests/data/save/scripts.json ${CAPSULE_GENERATED_DIR}/save_script.cpp
         --ids ${CAPSULE_GENERATED_DIR}/save_script_ids.h --depfile ${CAPSULE_GENERATED_DIR}/save_script.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/save_shape.py
         ${CAPSULE_ROOT}/tests/data/save/scripts.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/save_script.d
   COMMENT "Compiling the save fixture's scripts")
capsule_battle_test(save_tests ${CAPSULE_ROOT}/tests/data/recruit/battle.json
                    SOURCES tests/save_tests.cpp src/save/save.cpp ${CAPSULE_GENERATED_DIR}/save_script.cpp
                            ${CAPSULE_GENERATED_DIR}/save_script_ids.h
                    DIR save
                    ARGS ${CAPSULE_ROOT}/tests/data/save)
target_include_directories(capsule_save_tests PRIVATE ${CAPSULE_GENERATED_DIR})
add_test(NAME save_migration_tests
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_save_migration.py)
# C7: the same tests with a small built-in stage (64 bytes: the bag fits, the other sections need the caller's
# Stage), as on the GBA where the storage boxes do not fit its 2 KB stage
add_executable(capsule_save_stage_tests tests/save_tests.cpp src/save/save.cpp ${CAPSULE_GENERATED_DIR}/save_script.cpp
               ${CAPSULE_GENERATED_DIR}/save_script_ids.h)
target_link_libraries(capsule_save_stage_tests PRIVATE capsule_save_tests_data)
target_include_directories(capsule_save_stage_tests PRIVATE ${CAPSULE_GENERATED_DIR})
target_compile_definitions(capsule_save_stage_tests PRIVATE CAPSULE_SAVE_STAGE_BYTES=0x40u)
add_test(NAME save_stage_tests COMMAND capsule_save_stage_tests ${CAPSULE_ROOT}/tests/data/save)
