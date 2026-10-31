# D-30 tests (DS1): door animations, cell override sets, direction-grid sprite sheets, the object draw order and tile
# animation targets on the neutral fixture tests/data/d30 (the C++ program tests/d30_tests.cpp: the map, field, door
# player, sprite order and overworld warp sequence against the compiled fixture) and the compilers' Python tests
# (tests/test_d30_formats.py). Included from CMakeLists.txt's test section.

set(CAPSULE_D30_DATA ${CAPSULE_ROOT}/tests/data/d30)
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/d30_world.cpp ${CAPSULE_GENERATED_DIR}/d30_world_ids.h
           ${CAPSULE_GENERATED_DIR}/d30_encounter_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/map_compiler.py
         ${CAPSULE_D30_DATA}/world.json ${CAPSULE_GENERATED_DIR}/d30_world.cpp
         --depfile ${CAPSULE_GENERATED_DIR}/d30_world.d --ids ${CAPSULE_GENERATED_DIR}/d30_encounter_ids.h
         --world-ids ${CAPSULE_GENERATED_DIR}/d30_world_ids.h
         --scripts ${CAPSULE_D30_DATA}/scripts/scripts.json
   DEPENDS ${CAPSULE_TEXT_DEPENDS}
         ${CAPSULE_ROOT}/tools/map_compiler.py ${CAPSULE_ROOT}/tools/encounter_compiler.py
         ${CAPSULE_ROOT}/tools/event_compiler.py ${CAPSULE_ROOT}/tools/object_compiler.py
         ${CAPSULE_ROOT}/tools/route_compiler.py ${CAPSULE_ROOT}/tools/location_compiler.py
         ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/asset_compiler.py
         ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_D30_DATA}/world.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/d30_world.d
   COMMENT "Compiling the D-30 fixture's world")
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/d30_scripts.cpp ${CAPSULE_GENERATED_DIR}/d30_script_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_compiler.py
         ${CAPSULE_D30_DATA}/scripts/scripts.json ${CAPSULE_GENERATED_DIR}/d30_scripts.cpp
         --ids ${CAPSULE_GENERATED_DIR}/d30_script_ids.h --depfile ${CAPSULE_GENERATED_DIR}/d30_scripts.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/route_compiler.py
         ${CAPSULE_ROOT}/tools/asset_compiler.py ${CAPSULE_ROOT}/tools/sequence_compiler.py
         ${CAPSULE_D30_DATA}/scripts/scripts.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/d30_scripts.d
   COMMENT "Compiling the D-30 fixture's scripts")
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/d30_assets.cpp
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/asset_compiler.py
         ${CAPSULE_D30_DATA}/assets.json ${CAPSULE_GENERATED_DIR}/d30_assets.cpp
         --depfile ${CAPSULE_GENERATED_DIR}/d30_assets.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/asset_compiler.py
         ${CAPSULE_ROOT}/tools/sequence_compiler.py ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_D30_DATA}/assets.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/d30_assets.d
   COMMENT "Compiling the D-30 fixture's assets")
add_executable(capsule_d30_tests
   tests/d30_tests.cpp
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
   ${CAPSULE_GENERATED_DIR}/d30_world.cpp
   ${CAPSULE_GENERATED_DIR}/default_font.cpp
   ${CAPSULE_GENERATED_DIR}/d30_world_ids.h
   ${CAPSULE_GENERATED_DIR}/d30_scripts.cpp
   ${CAPSULE_GENERATED_DIR}/d30_script_ids.h)
target_include_directories(capsule_d30_tests PRIVATE include ${CAPSULE_GENERATED_DIR})
target_link_libraries(capsule_d30_tests PRIVATE capsule_options)
add_test(NAME d30_tests COMMAND capsule_d30_tests)
add_test(NAME d30_formats_tests COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_d30_formats.py)
