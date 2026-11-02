# M5 BX: the battle script machine (include/capsule/battle/vm.h, src/battle/vm.cpp) and the controller channel
# (include/capsule/battle/controller.h, src/battle/controller.cpp), the raw draws (include/capsule/core/random_raw.h,
# header only), the bank tool (tools/script_bank.py), their tests and the sandbox (apps/sandbox_battle_vm). Included
# from CMakeLists.txt after the engine library and the test targets exist.
#
# Both sources are members of the engine library on every platform: a program that never calls them links none of
# their code (a static library's members are pulled in only when referenced).

set(CAPSULE_BATTLE_VM_SOURCES
   ${CAPSULE_ROOT}/src/battle/vm.cpp
   ${CAPSULE_ROOT}/src/battle/vm_alias.cpp
   ${CAPSULE_ROOT}/src/battle/controller.cpp)
target_sources(capsule_engine PRIVATE ${CAPSULE_BATTLE_VM_SOURCES})

# Assembles a bank source (capsule.script-bank/1) into a C++ source and its label ids header.
# @param source     the bank source (JSON)
# @param namespace  the C++ namespace of kBank, kBankSize and the kLabel... ids
# @param out_cpp    the generated source
# @param out_ids    the generated header
# @param ARGN       further files the source names (its VM extension manifests, D-39)
function(capsule_script_bank source namespace out_cpp out_ids)
   add_custom_command(
      OUTPUT  ${out_cpp} ${out_ids}
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_bank.py asm ${source}
            --cpp ${out_cpp} --ids ${out_ids} --symbol kBank --namespace ${namespace}
      DEPENDS ${CAPSULE_ROOT}/tools/script_bank.py ${CAPSULE_ROOT}/tools/vm_ext.py ${source} ${ARGN}
      COMMENT "Assembling script bank ${namespace}")
endfunction()

if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM MATCHES "^(desktop|headless)$")
   capsule_script_bank(${CAPSULE_ROOT}/tests/data/battle_vm/test_bank.json test_bank
                       ${CAPSULE_GENERATED_DIR}/test_bank.cpp ${CAPSULE_GENERATED_DIR}/test_bank_ids.h)
   add_executable(capsule_battle_script_vm_tests
      ${CAPSULE_ROOT}/tests/battle_script_vm_tests.cpp
      ${CAPSULE_BATTLE_VM_SOURCES}
      ${CAPSULE_GENERATED_DIR}/test_bank.cpp
      ${CAPSULE_ROOT}/${CAPSULE_RNG_STREAM_SOURCES})
   capsule_rng(capsule_battle_script_vm_tests default)
   target_include_directories(capsule_battle_script_vm_tests PRIVATE ${CAPSULE_ROOT}/include
                              ${CAPSULE_GENERATED_DIR})
   target_link_libraries(capsule_battle_script_vm_tests PRIVATE capsule_options)
   add_test(NAME battle_script_vm_tests COMMAND capsule_battle_script_vm_tests)
   # D-39: the aliases (one per generic step) against natives of the same layouts, with the generated dispatch
   set(alias_data ${CAPSULE_ROOT}/tests/data/vm_ext)
   capsule_script_bank(${alias_data}/battle_alias_bank.json battle_alias_bank
                       ${CAPSULE_GENERATED_DIR}/battle_alias_bank.cpp ${CAPSULE_GENERATED_DIR}/battle_alias_bank_ids.h
                       ${alias_data}/battle_alias_vm_ext.json)
   capsule_vm_dispatch(OUT ${CAPSULE_GENERATED_DIR}/battle_alias_dispatch.inc
                       MANIFESTS ${alias_data}/battle_alias_vm_ext.json)
   add_executable(capsule_battle_vm_alias_tests
      ${CAPSULE_ROOT}/tests/battle_vm_alias_tests.cpp
      ${CAPSULE_BATTLE_VM_SOURCES}
      ${CAPSULE_GENERATED_DIR}/battle_alias_bank.cpp
      ${CAPSULE_GENERATED_DIR}/battle_alias_dispatch.inc)
   target_include_directories(capsule_battle_vm_alias_tests PRIVATE ${CAPSULE_ROOT}/include ${CAPSULE_GENERATED_DIR})
   target_link_libraries(capsule_battle_vm_alias_tests PRIVATE capsule_options)
   add_test(NAME battle_vm_alias_tests COMMAND capsule_battle_vm_alias_tests)
   capsule_vm_ext_check(battle_vm_alias_manifest_check MANIFESTS ${alias_data}/battle_alias_vm_ext.json
                        SOURCES ${CAPSULE_ROOT}/tests/battle_vm_alias_tests.cpp)
   add_test(NAME script_bank_tests COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_script_bank.py)
endif()

if(NOT CAPSULE_PLATFORM STREQUAL "headless" AND CAPSULE_ENGINE_TESTS AND CAPSULE_TEST_APPS)
   # The sandbox (simple shapes): a script slides two squares through the channel.
   capsule_game(${CAPSULE_ROOT}/apps/sandbox_battle_vm)
   set(sandbox_vm_gen ${CMAKE_CURRENT_BINARY_DIR}/generated/sandbox_battle_vm)   # capsule_game()'s gen folder
   capsule_script_bank(${CAPSULE_ROOT}/apps/sandbox_battle_vm/data/sandbox_bank.json sandbox_bank
                       ${sandbox_vm_gen}/sandbox_bank.cpp ${sandbox_vm_gen}/sandbox_bank_ids.h
                       ${CAPSULE_ROOT}/apps/sandbox_battle_vm/data/sandbox_vm_ext.json)
   # D-39: the command table generated from the sandbox's manifest (aliases + its two natives)
   capsule_vm_dispatch(OUT ${sandbox_vm_gen}/sandbox_dispatch.inc
                       MANIFESTS ${CAPSULE_ROOT}/apps/sandbox_battle_vm/data/sandbox_vm_ext.json)
   target_sources(capsule_sandbox_battle_vm PRIVATE ${sandbox_vm_gen}/sandbox_bank.cpp
                  ${sandbox_vm_gen}/sandbox_bank_ids.h ${sandbox_vm_gen}/sandbox_dispatch.inc)
endif()
