# UC (D-33): the screen framework (include/capsule/screen/: full-screen scenes on a stack, their background layers,
# sprites, tasks and data-driven fades; the field suspended and restored), its tests (tests/screen_tests.cpp on the
# software video model with the test world; tests/test_screen_formats.py: the tiles / tilemap / palette asset kinds)
# and its sandbox (apps/sandbox_screen). Included from CMakeLists.txt after the engine library and the tests.

set(CAPSULE_SCREEN_SOURCES
   ${CAPSULE_ROOT}/src/screen/screen.cpp
   ${CAPSULE_ROOT}/src/screen/layers.cpp
   ${CAPSULE_ROOT}/src/screen/sprites.cpp
   ${CAPSULE_ROOT}/src/screen/fade.cpp
   ${CAPSULE_ROOT}/src/screen/tasks.cpp
   ${CAPSULE_ROOT}/src/screen/arena.cpp)
target_sources(capsule_engine PRIVATE ${CAPSULE_SCREEN_SOURCES})

if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM MATCHES "^(desktop|headless)$")
   set(CAPSULE_SCREEN_DATA ${CAPSULE_ROOT}/tests/data/screen)
   add_custom_command(
      OUTPUT  ${CAPSULE_GENERATED_DIR}/screen_assets.cpp
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/asset_compiler.py
            ${CAPSULE_SCREEN_DATA}/assets.json ${CAPSULE_GENERATED_DIR}/screen_assets.cpp
            --depfile ${CAPSULE_GENERATED_DIR}/screen_assets.d
      DEPENDS ${CAPSULE_ROOT}/tools/asset_compiler.py ${CAPSULE_ROOT}/tools/screen_assets.py
            ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_SCREEN_DATA}/assets.json
      DEPFILE ${CAPSULE_GENERATED_DIR}/screen_assets.d
      COMMENT "Compiling the screen tests' assets")
   # the test world (CMakeLists.txt's map tests) for the return to the field
   add_executable(capsule_screen_tests
      ${CAPSULE_ROOT}/tests/screen_tests.cpp
      ${CAPSULE_SCREEN_SOURCES}
      ${CAPSULE_ROOT}/src/screen/arena_storage.cpp
      ${CAPSULE_ROOT}/src/asset/asset.cpp
      ${CAPSULE_GENERATED_DIR}/screen_assets.cpp
      ${CAPSULE_GENERATED_DIR}/test_world.cpp
      ${CAPSULE_ROOT}/src/map/map.cpp
      ${CAPSULE_ROOT}/src/gfx/background.cpp
      ${CAPSULE_ROOT}/src/gfx/palette.cpp
      ${CAPSULE_ROOT}/src/gfx/sprites.cpp
      ${CAPSULE_ROOT}/src/hal/soft/soft_video.cpp
      ${CAPSULE_ROOT}/src/hal/common/vram_queue.cpp)
   target_include_directories(capsule_screen_tests PRIVATE ${CAPSULE_ROOT}/include ${CAPSULE_ROOT}/src
                              ${CAPSULE_GENERATED_DIR})
   target_link_libraries(capsule_screen_tests PRIVATE capsule_options)
   target_compile_definitions(capsule_screen_tests PRIVATE ${CAPSULE_BUILD_SETTING_TEST_DEFS})
   add_test(NAME screen_tests COMMAND capsule_screen_tests)
   add_test(NAME screen_formats_tests COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_screen_formats.py)
endif()

if(NOT CAPSULE_PLATFORM STREQUAL "headless" AND CAPSULE_ENGINE_TESTS AND CAPSULE_TEST_APPS)
   # The screen sandbox (simple shapes): a map as the field, two screens on the stack, fades and the return.
   capsule_game(${CAPSULE_ROOT}/apps/sandbox_screen)
endif()
