# Doors and overrides tests: door animations, cell override sets, direction-grid sprite sheets, the object draw order and tile
# animation targets on the neutral fixture tests/data/doors_overrides (the C++ program tests/doors_overrides_tests.cpp: the map, field, door
# player, sprite order and overworld warp sequence against the compiled fixture) and the compilers' Python tests
# (tests/test_doors_overrides_formats.py). Included from CMakeLists.txt's test section.

set(CAPSULE_DOORS_OVERRIDES_DATA ${CAPSULE_ROOT}/tests/data/doors_overrides)
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/doors_overrides_world.cpp ${CAPSULE_GENERATED_DIR}/doors_overrides_world_ids.h
           ${CAPSULE_GENERATED_DIR}/doors_overrides_encounter_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/map_compiler.py
         ${CAPSULE_DOORS_OVERRIDES_DATA}/world.json ${CAPSULE_GENERATED_DIR}/doors_overrides_world.cpp
         --depfile ${CAPSULE_GENERATED_DIR}/doors_overrides_world.d --ids ${CAPSULE_GENERATED_DIR}/doors_overrides_encounter_ids.h
         --world-ids ${CAPSULE_GENERATED_DIR}/doors_overrides_world_ids.h
         --scripts ${CAPSULE_DOORS_OVERRIDES_DATA}/scripts/scripts.json
   DEPENDS ${CAPSULE_TEXT_DEPENDS}
         ${CAPSULE_ROOT}/tools/map_compiler.py ${CAPSULE_ROOT}/tools/encounter_compiler.py
         ${CAPSULE_ROOT}/tools/event_compiler.py ${CAPSULE_ROOT}/tools/object_compiler.py
         ${CAPSULE_ROOT}/tools/route_compiler.py ${CAPSULE_ROOT}/tools/location_compiler.py
         ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/asset_compiler.py
         ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_DOORS_OVERRIDES_DATA}/world.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/doors_overrides_world.d
   COMMENT "Compiling the doors and overrides fixture's world")
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/doors_overrides_scripts.cpp ${CAPSULE_GENERATED_DIR}/doors_overrides_script_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_compiler.py
         ${CAPSULE_DOORS_OVERRIDES_DATA}/scripts/scripts.json ${CAPSULE_GENERATED_DIR}/doors_overrides_scripts.cpp
         --ids ${CAPSULE_GENERATED_DIR}/doors_overrides_script_ids.h --depfile ${CAPSULE_GENERATED_DIR}/doors_overrides_scripts.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/route_compiler.py
         ${CAPSULE_ROOT}/tools/asset_compiler.py ${CAPSULE_ROOT}/tools/sequence_compiler.py
         ${CAPSULE_DOORS_OVERRIDES_DATA}/scripts/scripts.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/doors_overrides_scripts.d
   COMMENT "Compiling the doors and overrides fixture's scripts")
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/doors_overrides_assets.cpp
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/asset_compiler.py
         ${CAPSULE_DOORS_OVERRIDES_DATA}/assets.json ${CAPSULE_GENERATED_DIR}/doors_overrides_assets.cpp
         --depfile ${CAPSULE_GENERATED_DIR}/doors_overrides_assets.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/asset_compiler.py
         ${CAPSULE_ROOT}/tools/sequence_compiler.py ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_DOORS_OVERRIDES_DATA}/assets.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/doors_overrides_assets.d
   COMMENT "Compiling the doors and overrides fixture's assets")
add_executable(capsule_doors_overrides_tests
   tests/doors_overrides_tests.cpp
   src/asset/asset.cpp
   ${CAPSULE_TEST_RNG_SOURCES}
   src/encounter/encounter.cpp
   src/field/doors.cpp
   src/field/field.cpp
   src/field/field_route_default.cpp
   src/field/field_spot_default.cpp
   src/field/field_props.cpp
   src/gfx/background.cpp
   src/gfx/palette.cpp
   src/gfx/sprites.cpp
   src/map/map.cpp
   src/overworld/overworld.cpp
   src/script/script.cpp
   src/script/script_dispatch_all.cpp
   src/script/script_battle_none.cpp
   src/script/field_actions.cpp
   src/script/script_port_defaults.cpp
   ${CAPSULE_SCRIPT_ITEMS_SOURCES}
   src/seq/sequence.cpp
   src/seq/sequence_port_hal.cpp
   src/seq/sequence_game_default.cpp
   src/text/font.cpp
   src/text/print.cpp
   src/text/text_layer.cpp
   src/hal/soft/soft_video.cpp
   src/hal/common/vram_queue.cpp
   ${CAPSULE_GENERATED_DIR}/doors_overrides_assets.cpp
   ${CAPSULE_GENERATED_DIR}/doors_overrides_world.cpp
   ${CAPSULE_GENERATED_DIR}/default_font.cpp
   ${CAPSULE_GENERATED_DIR}/doors_overrides_world_ids.h
   ${CAPSULE_GENERATED_DIR}/doors_overrides_scripts.cpp
   ${CAPSULE_GENERATED_DIR}/doors_overrides_script_ids.h)
target_include_directories(capsule_doors_overrides_tests PRIVATE include ${CAPSULE_GENERATED_DIR})
target_link_libraries(capsule_doors_overrides_tests PRIVATE capsule_options)
add_test(NAME doors_overrides_tests COMMAND capsule_doors_overrides_tests)
add_test(NAME doors_overrides_formats_tests COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_doors_overrides_formats.py)
