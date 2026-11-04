# Map view tests: the shown map's cell edits and layouts, ledges with collision, the camera mover, fixed draw orders,
# sprite offsets, effect sprite slots, a route's walk in place and window 0's edges per scanline, on the fixture
# tests/data/map_view (it reuses the doors and overrides fixture's tileset and sprites: cmake/parts/doors_overrides_tests.cmake compiles those assets).
# Included from CMakeLists.txt's test section, after the doors and overrides part.

set(CAPSULE_MAP_VIEW_DATA ${CAPSULE_ROOT}/tests/data/map_view)
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/map_view_world.cpp ${CAPSULE_GENERATED_DIR}/map_view_world_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/map_compiler.py
         ${CAPSULE_MAP_VIEW_DATA}/world.json ${CAPSULE_GENERATED_DIR}/map_view_world.cpp
         --depfile ${CAPSULE_GENERATED_DIR}/map_view_world.d --world-ids ${CAPSULE_GENERATED_DIR}/map_view_world_ids.h
   DEPENDS ${CAPSULE_ROOT}/tools/map_compiler.py ${CAPSULE_ROOT}/tools/event_compiler.py
         ${CAPSULE_ROOT}/tools/object_compiler.py ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_MAP_VIEW_DATA}/world.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/map_view_world.d
   COMMENT "Compiling the map view fixture's world")
add_executable(capsule_map_view_tests
   tests/map_view_tests.cpp
   src/asset/asset.cpp
   ${CAPSULE_TEST_RNG_SOURCES}
   src/encounter/encounter.cpp
   src/field/doors.cpp
   src/field/field.cpp
   src/field/field_game_default.cpp
   src/field/field_route_default.cpp
   src/gfx/background.cpp
   src/gfx/palette.cpp
   src/gfx/sprites.cpp
   src/map/map.cpp
   src/overworld/overworld.cpp
   src/script/script.cpp
   src/script/script_dispatch_all.cpp
   src/script/script_battle_none.cpp
   src/script/script_empty.cpp
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
   ${CAPSULE_GENERATED_DIR}/map_view_world.cpp
   ${CAPSULE_GENERATED_DIR}/default_font.cpp
   ${CAPSULE_GENERATED_DIR}/map_view_world_ids.h)
target_include_directories(capsule_map_view_tests PRIVATE include ${CAPSULE_GENERATED_DIR})
target_link_libraries(capsule_map_view_tests PRIVATE capsule_options)
add_test(NAME map_view_tests COMMAND capsule_map_view_tests)
