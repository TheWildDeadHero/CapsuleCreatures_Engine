# Effect VM (D-22 decision 1, D-23 section 1): its sources and tests.
# Included from CMakeLists.txt after every engine and test target exists, so
# it can add sources to them (target_sources) and add its own tests (guarded
# by CAPSULE_PLATFORM MATCHES "^(desktop|headless)$").

# The interpreter is one of the battle modules (cmake/parts/battle.cmake:
# CAPSULE_BATTLE_SOURCES, with its host src/battle/effect_host.cpp), so it is
# part of the engine library on every platform (GBA included).

# What an effect compile depends on: the compiler and the compilers it imports.
set(CAPSULE_EFFECT_COMPILER_DEPENDS
   ${CAPSULE_ROOT}/tools/effect_compiler.py
   ${CAPSULE_ROOT}/tools/script_compiler.py
   ${CAPSULE_ROOT}/tools/ruleset_compiler.py
   ${CAPSULE_ROOT}/tools/asset_compiler.py
   ${CAPSULE_ROOT}/tools/route_compiler.py
   ${CAPSULE_ROOT}/tools/sequence_compiler.py
   ${CAPSULE_TEXT_DEPENDS})

# Compiles a capsule.effects/1 manifest to a bank (kBank, kOwnerNames and
# point_handler() in namespace `namespace`).
# @param manifest   the manifest
# @param namespace  C++ namespace of the tables
# @param out        generated C++ source
function(capsule_effect_bank manifest namespace out)
   get_filename_component(dir ${manifest} DIRECTORY)
   file(GLOB sources CONFIGURE_DEPENDS ${dir}/*.ceffect ${dir}/*.json)
   add_custom_command(
      OUTPUT  ${out}
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/effect_compiler.py ${manifest}
            --emit ${out} --namespace ${namespace}
      DEPENDS ${CAPSULE_EFFECT_COMPILER_DEPENDS} ${sources}
      COMMENT "Compiling effect bank ${namespace}")
endfunction()

if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM MATCHES "^(desktop|headless)$")
   # The VM against a mock host (tests/support/effect_mock_host.h): the neutral fixture (every opcode). The D-23
   # section 1.7 classic examples run in the reference-ruleset repo against the same mock host.
   capsule_effect_bank(${CAPSULE_ROOT}/tests/data/effects/vm.json vm_effects
                       ${CAPSULE_GENERATED_DIR}/test_effects_vm.cpp)
   add_executable(capsule_effect_vm_tests
      tests/effect_vm_tests.cpp
      src/battle/effect_vm.cpp
      ${CAPSULE_GENERATED_DIR}/test_effects_vm.cpp)
   target_include_directories(capsule_effect_vm_tests PRIVATE include)
   target_link_libraries(capsule_effect_vm_tests PRIVATE capsule_options)
   # The kEffectOpsPerRun backstop asserts in debug builds; the tests check its fault path.
   target_compile_definitions(capsule_effect_vm_tests PRIVATE CAPSULE_ASSERTS=0)
   add_test(NAME effect_vm_tests COMMAND capsule_effect_vm_tests)
   add_test(NAME effect_compiler_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_effect_compiler.py)
endif()
