# Field options tests (a review's engine findings) on the neutral fixture tests/data/field_options: the script VM's options, the
# selected object, session flags, immediate triggers, on_resume before spawning, object levels, removal and return,
# a stopped spotter, a text box style and two-language text tables (the C++ program tests/field_options_tests.cpp) and the
# compilers' Python tests (tests/test_field_options_formats.py). Included from CMakeLists.txt's test section.

set(CAPSULE_FIELD_OPTIONS_DATA ${CAPSULE_ROOT}/tests/data/field_options)
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/field_options_world.cpp ${CAPSULE_GENERATED_DIR}/field_options_world_ids.h
           ${CAPSULE_GENERATED_DIR}/field_options_encounter_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/map_compiler.py
         ${CAPSULE_FIELD_OPTIONS_DATA}/world.json ${CAPSULE_GENERATED_DIR}/field_options_world.cpp
         --depfile ${CAPSULE_GENERATED_DIR}/field_options_world.d --ids ${CAPSULE_GENERATED_DIR}/field_options_encounter_ids.h
         --world-ids ${CAPSULE_GENERATED_DIR}/field_options_world_ids.h
         --scripts ${CAPSULE_FIELD_OPTIONS_DATA}/scripts/scripts.json
   DEPENDS ${CAPSULE_TEXT_DEPENDS}
         ${CAPSULE_ROOT}/tools/map_compiler.py ${CAPSULE_ROOT}/tools/encounter_compiler.py
         ${CAPSULE_ROOT}/tools/event_compiler.py ${CAPSULE_ROOT}/tools/object_compiler.py
         ${CAPSULE_ROOT}/tools/route_compiler.py ${CAPSULE_ROOT}/tools/location_compiler.py
         ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/asset_compiler.py
         ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_FIELD_OPTIONS_DATA}/world.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/field_options_world.d
   COMMENT "Compiling the field options fixture's world")
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/field_options_scripts.cpp ${CAPSULE_GENERATED_DIR}/field_options_script_ids.h
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_compiler.py
         ${CAPSULE_FIELD_OPTIONS_DATA}/scripts/scripts.json ${CAPSULE_GENERATED_DIR}/field_options_scripts.cpp
         --ids ${CAPSULE_GENERATED_DIR}/field_options_script_ids.h --depfile ${CAPSULE_GENERATED_DIR}/field_options_scripts.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/route_compiler.py
         ${CAPSULE_ROOT}/tools/asset_compiler.py ${CAPSULE_ROOT}/tools/sequence_compiler.py
         ${CAPSULE_FIELD_OPTIONS_DATA}/scripts/scripts.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/field_options_scripts.d
   COMMENT "Compiling the field options fixture's scripts")
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/field_options_assets.cpp
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/asset_compiler.py
         ${CAPSULE_FIELD_OPTIONS_DATA}/assets.json ${CAPSULE_GENERATED_DIR}/field_options_assets.cpp
         --depfile ${CAPSULE_GENERATED_DIR}/field_options_assets.d
   DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/asset_compiler.py ${CAPSULE_ROOT}/tools/font_compiler.py
         ${CAPSULE_ROOT}/tools/sequence_compiler.py ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_FIELD_OPTIONS_DATA}/assets.json
   DEPFILE ${CAPSULE_GENERATED_DIR}/field_options_assets.d
   COMMENT "Compiling the field options fixture's assets")
add_executable(capsule_field_options_tests
   tests/field_options_tests.cpp
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
   ${CAPSULE_GENERATED_DIR}/field_options_assets.cpp
   ${CAPSULE_GENERATED_DIR}/field_options_world.cpp
   ${CAPSULE_GENERATED_DIR}/default_font.cpp
   ${CAPSULE_GENERATED_DIR}/field_options_world_ids.h
   ${CAPSULE_GENERATED_DIR}/field_options_scripts.cpp
   ${CAPSULE_GENERATED_DIR}/field_options_script_ids.h)
target_include_directories(capsule_field_options_tests PRIVATE include ${CAPSULE_GENERATED_DIR})
target_link_libraries(capsule_field_options_tests PRIVATE capsule_options)
# A review finding: the test's port definitions are qualified; -Werror=missing-declarations keeps them so
set_source_files_properties(tests/field_options_tests.cpp PROPERTIES COMPILE_OPTIONS -Werror=missing-declarations)
add_test(NAME field_options_tests COMMAND capsule_field_options_tests)
add_test(NAME field_options_formats_tests COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_field_options_formats.py)
