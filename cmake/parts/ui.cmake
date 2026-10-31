# N3 (D-32, engine B2 core): the ui module (include/capsule/ui/: menus, lists, grids, the pad's key repeat, a list's
# scroll arrows) joins the engine library; its tests (tests/ui_tests.cpp on the software video model, the text box
# menu and arrow image formats in tests/test_ui_formats.py, the save image's Game section in tests/save_game_tests.cpp)
# and the menu sandbox (apps/sandbox_menu). Included from CMakeLists.txt after the engine library and the tests.

set(CAPSULE_UI_SOURCES
   ${CAPSULE_ROOT}/src/ui/keys.cpp
   ${CAPSULE_ROOT}/src/ui/menu.cpp
   ${CAPSULE_ROOT}/src/ui/arrows.cpp)
target_sources(capsule_engine PRIVATE ${CAPSULE_UI_SOURCES})

if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM MATCHES "^(desktop|headless)$")
   # the fixture's assets: a font, a sprite of two arrows, text box styles with a menu and an arrow image
   set(CAPSULE_UI_DATA ${CAPSULE_ROOT}/tests/data/ui)
   add_custom_command(
      OUTPUT  ${CAPSULE_GENERATED_DIR}/ui_assets.cpp
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/asset_compiler.py
            ${CAPSULE_UI_DATA}/assets.json ${CAPSULE_GENERATED_DIR}/ui_assets.cpp
            --depfile ${CAPSULE_GENERATED_DIR}/ui_assets.d
      DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_ROOT}/tools/asset_compiler.py ${CAPSULE_ROOT}/tools/font_compiler.py
            ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_UI_DATA}/assets.json
      DEPFILE ${CAPSULE_GENERATED_DIR}/ui_assets.d
      COMMENT "Compiling the ui fixture's assets")
   add_executable(capsule_ui_tests
      ${CAPSULE_ROOT}/tests/ui_tests.cpp
      ${CAPSULE_UI_SOURCES}
      ${CAPSULE_ROOT}/src/asset/asset.cpp
      ${CAPSULE_GENERATED_DIR}/ui_assets.cpp
      ${CAPSULE_ROOT}/src/gfx/palette.cpp
      ${CAPSULE_ROOT}/src/gfx/sprites.cpp
      ${CAPSULE_ROOT}/src/text/window.cpp
      ${CAPSULE_ROOT}/src/text/print.cpp
      ${CAPSULE_ROOT}/src/hal/soft/soft_video.cpp
      ${CAPSULE_ROOT}/src/hal/common/vram_queue.cpp)
   target_include_directories(capsule_ui_tests PRIVATE ${CAPSULE_ROOT}/include ${CAPSULE_ROOT}/src)
   target_link_libraries(capsule_ui_tests PRIVATE capsule_options)
   target_compile_definitions(capsule_ui_tests PRIVATE ${CAPSULE_BUILD_SETTING_TEST_DEFS})
   add_test(NAME ui_tests COMMAND capsule_ui_tests)
   add_test(NAME ui_formats_tests COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_ui_formats.py)
endif()
if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM STREQUAL "headless")
   # the save image's Game section (a game's own block) on the engine library and its RAM save medium
   add_executable(capsule_save_game_tests ${CAPSULE_ROOT}/tests/save_game_tests.cpp)
   target_link_libraries(capsule_save_game_tests PRIVATE capsule_engine)
   add_test(NAME save_game_tests COMMAND capsule_save_game_tests)
endif()

if(NOT CAPSULE_PLATFORM STREQUAL "headless" AND CAPSULE_ENGINE_TESTS AND CAPSULE_TEST_APPS)
   # The menu sandbox (simple shapes): a column, a grid, a scrolling list with its arrows, yes / no, a value box
   # and a message box with the arrow image.
   capsule_game(${CAPSULE_ROOT}/apps/sandbox_menu)
endif()
