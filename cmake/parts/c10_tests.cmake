# C10 tests (the R7 review's engine findings) on the neutral fixture tests/data/c10: the script VM's options, the
# selected object, session flags, immediate triggers, on_resume before spawning, object levels, removal and return,
# a stopped spotter, a text box style and two-language text tables (the C++ program tests/c10_tests.cpp) and the
# compilers' Python tests (tests/test_c10_formats.py). Included from CMakeLists.txt's test section.

set(CAPSULE_C10_DATA ${CAPSULE_ROOT}/tests/data/c10)
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/c10_world.cpp ${CAPSULE_GENERATED_DIR}/c10_world_ids.h
           ${CAPSULE_GENERATED_DIR}/c10_encounter_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/map_compiler.py
         ${CAPSULE_C10_DATA}/world.json ${CAPSULE_GENERATED_DIR}/c10_world.cpp
         --depfile ${CAPSULE_GENERATED_DIR}/c10_world.d --ids ${CAPSULE_GENERATED_DIR}/c10_encounter_ids.h
         --world-ids ${CAPSULE_GENERATED_DIR}/c10_world_ids.h
         --scripts ${CAPSULE_C10_DATA}/scripts/scripts.json
   DEPENDS ${CAPSULE_TEXT_DEPENDS}
         ${CAPSULE_ROOT}/tools/map_compiler.py ${CAPSULE_ROOT}/tools/encounter_compiler.py
         ${CAPSULE_ROOT}/tools/event_compiler.py ${CAPSULE_ROOT}/tools/object_compiler.py
         ${CAPSULE_ROOT}/tools/route_compiler.py ${CAPSULE_ROOT}/tools/location_compiler.py
         ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/asset_compiler.py
         ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_C10_DATA}/world.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/c10_world.d
   COMMENT "Compiling the C10 fixture's world")
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/c10_scripts.cpp ${CAPSULE_GENERATED_DIR}/c10_script_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_compiler.py
         ${CAPSULE_C10_DATA}/scripts/scripts.json ${CAPSULE_GENERATED_DIR}/c10_scripts.cpp
         --ids ${CAPSULE_GENERATED_DIR}/c10_script_ids.h --depfile ${CAPSULE_GENERATED_DIR}/c10_scripts.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/route_compiler.py
         ${CAPSULE_ROOT}/tools/asset_compiler.py ${CAPSULE_ROOT}/tools/sequence_compiler.py
         ${CAPSULE_C10_DATA}/scripts/scripts.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/c10_scripts.d
   COMMENT "Compiling the C10 fixture's scripts")
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/c10_assets.cpp
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/asset_compiler.py
         ${CAPSULE_C10_DATA}/assets.json ${CAPSULE_GENERATED_DIR}/c10_assets.cpp
         --depfile ${CAPSULE_GENERATED_DIR}/c10_assets.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/asset_compiler.py ${CAPSULE_ROOT}/tools/font_compiler.py
         ${CAPSULE_ROOT}/tools/sequence_compiler.py ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_C10_DATA}/assets.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/c10_assets.d
   COMMENT "Compiling the C10 fixture's assets")
add_executable(capsule_c10_tests
   tests/c10_tests.cpp
   src/asset/asset.cpp
   ${CAPSULE_TEST_RNG_SOURCES}
   src/encounter/encounter.cpp
   src/field/doors.cpp
   src/field/field.cpp
   src/field/field_route_default.cpp
   src/gfx/background.cpp
   src/gfx/palette.cpp
   src/gfx/sprites.cpp
   src/map/map.cpp
   src/overworld/overworld.cpp
   src/script/script.cpp
   src/script/field_actions.cpp
   src/script/script_i4_default.cpp
   ${CAPSULE_SCRIPT_ITEMS_SOURCES}
   src/seq/sequence.cpp
   src/seq/sequence_port_hal.cpp
   src/seq/sequence_game_default.cpp
   src/text/font.cpp
   src/text/print.cpp
   src/text/text_layer.cpp
   src/hal/soft/soft_video.cpp
   src/hal/common/vram_queue.cpp
   ${CAPSULE_GENERATED_DIR}/c10_assets.cpp
   ${CAPSULE_GENERATED_DIR}/c10_world.cpp
   ${CAPSULE_GENERATED_DIR}/default_font.cpp
   ${CAPSULE_GENERATED_DIR}/c10_world_ids.h
   ${CAPSULE_GENERATED_DIR}/c10_scripts.cpp
   ${CAPSULE_GENERATED_DIR}/c10_script_ids.h)
target_include_directories(capsule_c10_tests PRIVATE include ${CAPSULE_GENERATED_DIR})
target_link_libraries(capsule_c10_tests PRIVATE capsule_options)
# R7 finding 12: the test's port definitions are qualified; -Werror=missing-declarations keeps them so
set_source_files_properties(tests/c10_tests.cpp PROPERTIES COMPILE_OPTIONS -Werror=missing-declarations)
add_test(NAME c10_tests COMMAND capsule_c10_tests)
add_test(NAME c10_formats_tests COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_c10_formats.py)
