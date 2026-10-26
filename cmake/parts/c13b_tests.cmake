# C13b tests: the world's cell interaction rules (capsule::map::Interaction) on the fixture tests/data/c13b (the C++
# program tests/c13b_tests.cpp: field::update()'s A press against the compiled fixture; the compiler's checks are in
# tests/test_map_compiler.py). It reuses the D-30 fixture's tileset and sprites (cmake/parts/d30_tests.cmake compiles
# those assets). Included from CMakeLists.txt's test section, after the D-30 part.

set(CAPSULE_C13B_DATA ${CAPSULE_ROOT}/tests/data/c13b)
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/c13b_world.cpp ${CAPSULE_GENERATED_DIR}/c13b_world_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/map_compiler.py
         ${CAPSULE_C13B_DATA}/world.json ${CAPSULE_GENERATED_DIR}/c13b_world.cpp
         --depfile ${CAPSULE_GENERATED_DIR}/c13b_world.d --world-ids ${CAPSULE_GENERATED_DIR}/c13b_world_ids.h
         --scripts ${CAPSULE_C13B_DATA}/scripts/scripts.json
   DEPENDS ${CAPSULE_TEXT_DEPENDS}
         ${CAPSULE_ROOT}/tools/map_compiler.py ${CAPSULE_ROOT}/tools/encounter_compiler.py
         ${CAPSULE_ROOT}/tools/event_compiler.py ${CAPSULE_ROOT}/tools/object_compiler.py
         ${CAPSULE_ROOT}/tools/route_compiler.py ${CAPSULE_ROOT}/tools/location_compiler.py
         ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/asset_compiler.py
         ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_C13B_DATA}/world.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/c13b_world.d
   COMMENT "Compiling the C13b fixture's world")
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/c13b_scripts.cpp ${CAPSULE_GENERATED_DIR}/c13b_script_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_compiler.py
         ${CAPSULE_C13B_DATA}/scripts/scripts.json ${CAPSULE_GENERATED_DIR}/c13b_scripts.cpp
         --ids ${CAPSULE_GENERATED_DIR}/c13b_script_ids.h --depfile ${CAPSULE_GENERATED_DIR}/c13b_scripts.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/route_compiler.py
         ${CAPSULE_ROOT}/tools/asset_compiler.py ${CAPSULE_ROOT}/tools/sequence_compiler.py
         ${CAPSULE_C13B_DATA}/scripts/scripts.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/c13b_scripts.d
   COMMENT "Compiling the C13b fixture's scripts")
add_executable(capsule_c13b_tests
   tests/c13b_tests.cpp
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
   ${CAPSULE_GENERATED_DIR}/d30_assets.cpp
   ${CAPSULE_GENERATED_DIR}/c13b_world.cpp
   ${CAPSULE_GENERATED_DIR}/default_font.cpp
   ${CAPSULE_GENERATED_DIR}/c13b_world_ids.h
   ${CAPSULE_GENERATED_DIR}/c13b_scripts.cpp
   ${CAPSULE_GENERATED_DIR}/c13b_script_ids.h)
target_include_directories(capsule_c13b_tests PRIVATE include ${CAPSULE_GENERATED_DIR})
target_link_libraries(capsule_c13b_tests PRIVATE capsule_options)
add_test(NAME c13b_tests COMMAND capsule_c13b_tests)
