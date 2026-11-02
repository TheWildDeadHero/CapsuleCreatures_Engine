# M5 BF (D-38): the scene kit's animation-script player (include/capsule/anim/: banks and the player shell,
# docs/ANIM_SCRIPT.md), its tests (tests/anim_tests.cpp on the software video model, with the affine animation,
# scanline and blend parts of the screen framework; tests/test_anim_bank.py: tools/anim_bank.py), the scene /
# animation viewer (capsule_anim_view) and the sandbox (apps/sandbox_anim). Included from CMakeLists.txt after
# screen.cmake (the screen sources and the test world's generated files).

set(CAPSULE_ANIM_SOURCES
   ${CAPSULE_ROOT}/src/anim/alias.cpp
   ${CAPSULE_ROOT}/src/anim/bank.cpp
   ${CAPSULE_ROOT}/src/anim/player.cpp)
target_sources(capsule_engine PRIVATE ${CAPSULE_ANIM_SOURCES})

# capsule_anim_bank(<canim> <symbol> <out.cpp> [INDEXED]): a .canim text assembled into a C++ source defining
# <symbol> (the bank's bytes) and <symbol>_size (tools/anim_bank.py; a game's banks, the tests' and the sandbox's).
function(capsule_anim_bank canim symbol out)
   set(flags)
   if("INDEXED" IN_LIST ARGN)
      set(flags --indexed)
   endif()
   add_custom_command(
      OUTPUT  ${out}
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/anim_bank.py assemble ${canim} ${out} ${flags} --cpp ${symbol}
      DEPENDS ${CAPSULE_ROOT}/tools/anim_bank.py ${canim}
      COMMENT "Assembling animation bank ${symbol}"
      VERBATIM)
endfunction()

# The engine sources the anim tests and the viewer run on the software video model (no HAL beyond it).
set(CAPSULE_ANIM_SOFT_SOURCES
   ${CAPSULE_ANIM_SOURCES}
   ${CAPSULE_ROOT}/src/screen/sprites.cpp
   ${CAPSULE_ROOT}/src/screen/tasks.cpp
   ${CAPSULE_ROOT}/src/screen/layers.cpp
   ${CAPSULE_ROOT}/src/screen/fade.cpp
   ${CAPSULE_ROOT}/src/screen/affine.cpp
   ${CAPSULE_ROOT}/src/screen/scanline.cpp
   ${CAPSULE_ROOT}/src/screen/blend.cpp
   ${CAPSULE_ROOT}/src/asset/asset.cpp
   ${CAPSULE_ROOT}/src/asset/asset_table_empty.cpp
   ${CAPSULE_ROOT}/src/map/map.cpp
   ${CAPSULE_ROOT}/src/map/world_empty.cpp
   ${CAPSULE_ROOT}/src/gfx/background.cpp
   ${CAPSULE_ROOT}/src/gfx/palette.cpp
   ${CAPSULE_ROOT}/src/hal/soft/soft_video.cpp
   ${CAPSULE_ROOT}/src/hal/common/vram_queue.cpp)

if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM MATCHES "^(desktop|headless)$")
   capsule_anim_bank(${CAPSULE_ROOT}/tests/data/anim/test_bank.canim kTestAnimBank
                     ${CAPSULE_GENERATED_DIR}/test_anim_bank.cpp INDEXED)
   add_executable(capsule_anim_tests
      ${CAPSULE_ROOT}/tests/anim_tests.cpp
      ${CAPSULE_ANIM_SOFT_SOURCES}
      ${CAPSULE_GENERATED_DIR}/test_anim_bank.cpp)
   target_include_directories(capsule_anim_tests PRIVATE ${CAPSULE_ROOT}/include ${CAPSULE_ROOT}/src)
   target_link_libraries(capsule_anim_tests PRIVATE capsule_options)
   target_compile_definitions(capsule_anim_tests PRIVATE ${CAPSULE_BUILD_SETTING_TEST_DEFS})
   add_test(NAME anim_tests COMMAND capsule_anim_tests)
   add_test(NAME anim_bank_tests COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_anim_bank.py)

   # D-39: the player's aliases against natives of the same layouts, with the generated command table
   # (capsule_vm_dispatch: cmake/parts/vm_ext.cmake, included before this file)
   set(anim_alias_data ${CAPSULE_ROOT}/tests/data/vm_ext)
   add_custom_command(
      OUTPUT  ${CAPSULE_GENERATED_DIR}/anim_alias_bank.cpp
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/anim_bank.py assemble ${anim_alias_data}/anim_alias_bank.canim
              ${CAPSULE_GENERATED_DIR}/anim_alias_bank.cpp --cpp kAnimAliasBank
      DEPENDS ${CAPSULE_ROOT}/tools/anim_bank.py ${CAPSULE_ROOT}/tools/vm_ext.py ${anim_alias_data}/anim_alias_bank.canim
              ${anim_alias_data}/anim_alias_vm_ext.json
      COMMENT "Assembling animation bank kAnimAliasBank"
      VERBATIM)
   capsule_vm_dispatch(OUT ${CAPSULE_GENERATED_DIR}/anim_alias_dispatch.inc
                       MANIFESTS ${anim_alias_data}/anim_alias_vm_ext.json)
   add_executable(capsule_anim_alias_tests
      ${CAPSULE_ROOT}/tests/anim_alias_tests.cpp
      ${CAPSULE_ANIM_SOFT_SOURCES}
      ${CAPSULE_GENERATED_DIR}/anim_alias_bank.cpp
      ${CAPSULE_GENERATED_DIR}/anim_alias_dispatch.inc)
   target_include_directories(capsule_anim_alias_tests PRIVATE ${CAPSULE_ROOT}/include ${CAPSULE_ROOT}/src
                              ${CAPSULE_GENERATED_DIR})
   target_link_libraries(capsule_anim_alias_tests PRIVATE capsule_options)
   target_compile_definitions(capsule_anim_alias_tests PRIVATE ${CAPSULE_BUILD_SETTING_TEST_DEFS})
   add_test(NAME anim_alias_tests COMMAND capsule_anim_alias_tests)
   capsule_vm_ext_check(anim_alias_manifest_check MANIFESTS ${anim_alias_data}/anim_alias_vm_ext.json
                        SOURCES ${CAPSULE_ROOT}/tests/anim_alias_tests.cpp)

   # The scene / animation viewer: disassembles a bank, plays the sandbox's scenes on the software video model and
   # writes frames (apps/sandbox_anim/src/anim_view.cpp).
   capsule_anim_bank(${CAPSULE_ROOT}/apps/sandbox_anim/data/shapes.canim kShapesBank
                     ${CAPSULE_GENERATED_DIR}/anim_view_shapes_bank.cpp INDEXED)
   add_executable(capsule_anim_view
      ${CAPSULE_ROOT}/apps/sandbox_anim/src/anim_view.cpp
      ${CAPSULE_ROOT}/apps/sandbox_anim/src/scenes.cpp
      ${CAPSULE_ANIM_SOFT_SOURCES}
      ${CAPSULE_GENERATED_DIR}/anim_view_shapes_bank.cpp)
   target_include_directories(capsule_anim_view PRIVATE ${CAPSULE_ROOT}/include ${CAPSULE_ROOT}/src)
   target_link_libraries(capsule_anim_view PRIVATE capsule_options)
   target_compile_definitions(capsule_anim_view PRIVATE ${CAPSULE_BUILD_SETTING_TEST_DEFS})
   add_test(NAME anim_view_scenes COMMAND capsule_anim_view --check)
endif()

if(NOT CAPSULE_PLATFORM STREQUAL "headless" AND CAPSULE_ENGINE_TESTS AND CAPSULE_TEST_APPS)
   # The scene kit's sandbox (simple shapes): affine animation, scanline effects, palette blends, a script.
   capsule_game(${CAPSULE_ROOT}/apps/sandbox_anim)
   capsule_anim_bank(${CAPSULE_ROOT}/apps/sandbox_anim/data/shapes.canim kShapesBank
                     ${CMAKE_CURRENT_BINARY_DIR}/generated/sandbox_anim/shapes_bank.cpp INDEXED)
   target_sources(capsule_sandbox_anim PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/sandbox_anim/shapes_bank.cpp)
endif()
