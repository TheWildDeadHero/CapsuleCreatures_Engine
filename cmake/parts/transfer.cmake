# Record transfer v2 (C-07a, D-23 Q9; include/capsule/monster/transfer.h): its
# source, the transfer table generator (capsule_transfer_table(), which
# capsule_game() calls for a game with battle data) and its tests
# (capsule_transfer_tests()). Included from CMakeLists.txt once the engine
# library exists and before the games; CMakeLists.txt calls
# capsule_transfer_tests() after every test target exists.

# Export and import are part of the engine library on every platform (GBA
# included). Nothing in the engine calls them, so a static link pulls them in
# only for a program that transfers records; that program links a transfer
# table (capsule_transfer_table()).
target_sources(capsule_engine PRIVATE src/monster/transfer.cpp)

# What a transfer table compile depends on: the battle compile and the mirror.
set(CAPSULE_TRANSFER_DEPENDS
   ${CAPSULE_ROOT}/tools/record_transfer.py
   ${CAPSULE_TEXT_DEPENDS}
   ${CAPSULE_BATTLE_COMPILER_DEPENDS})

# Compiles the transfer table of a battle manifest (capsule::monster::kTransfer
# for the kLayout the same manifest's battle data defines), or with NAMESPACE
# a transfer fixture's stand-alone table and layout (and HEADER, its header).
# @param manifest   battle.json or a capsule.transfer_fixture/1 file
# @param out        generated C++ source
# @param NAMESPACE  (optional) the fixture's namespace
# @param HEADER     (optional) generated header declaring the fixture's tables
function(capsule_transfer_table manifest out)
   cmake_parse_arguments(arg "" "NAMESPACE;HEADER" "" ${ARGN})
   set(extra "")
   set(outputs ${out})
   if(arg_NAMESPACE)
      list(APPEND extra --namespace ${arg_NAMESPACE})
   endif()
   if(arg_HEADER)
      list(APPEND extra --header ${arg_HEADER})
      list(APPEND outputs ${arg_HEADER})
   endif()
   add_custom_command(
      OUTPUT  ${outputs}
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/record_transfer.py emit ${manifest} ${out}
            ${extra} --platform ${CAPSULE_PLATFORM} --depfile ${out}.d
      DEPENDS ${CAPSULE_TRANSFER_DEPENDS} ${manifest}
      DEPFILE ${out}.d
      COMMENT "Compiling the transfer table of ${manifest}")
endfunction()

# Adds the transfer tests (host builds of the engine's own tests, with the Starter sample's battle data).
function(capsule_transfer_tests)
   if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM MATCHES "^(desktop|headless)$" AND TARGET capsule_starter_data)
      # The fixtures (tests/data/transfer), the Starter sample's own table, and
      # the Python mirror's test vectors over them.
      set(dir ${CAPSULE_ROOT}/tests/data/transfer)
      set(gen ${CAPSULE_GENERATED_DIR}/transfer)
      set(fixture_sources "")
      foreach(name six four shuffled twin)
         capsule_transfer_table(${dir}/${name}.json ${gen}/transfer_${name}.cpp
                                NAMESPACE ${name} HEADER ${gen}/transfer_${name}.h)
         list(APPEND fixture_sources ${gen}/transfer_${name}.cpp ${gen}/transfer_${name}.h)
      endforeach()
      capsule_transfer_table(${CAPSULE_ROOT}/samples/starter/battle.json ${gen}/test_transfer.cpp)
      add_custom_command(
         OUTPUT  ${gen}/transfer_cases.h
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/record_transfer.py cases ${dir}/cases.json
               ${gen}/transfer_cases.h --platform ${CAPSULE_PLATFORM} --depfile ${gen}/transfer_cases.d
         DEPENDS ${CAPSULE_TRANSFER_DEPENDS} ${dir}/cases.json
         DEPFILE ${gen}/transfer_cases.d
         COMMENT "Computing the transfer test vectors")

      # Linked with the Starter sample's battle data: its kLayout is the build's layout.
      add_executable(capsule_transfer_tests
         tests/transfer_tests.cpp
         src/monster/transfer.cpp
         ${fixture_sources}
         ${gen}/test_transfer.cpp
         ${gen}/transfer_cases.h)
      target_include_directories(capsule_transfer_tests PRIVATE include ${gen})
      target_link_libraries(capsule_transfer_tests PRIVATE capsule_starter_data)
      add_test(NAME transfer_tests COMMAND capsule_transfer_tests)
      add_test(NAME record_transfer_tests
               COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_record_transfer.py)
   endif()
endfunction()
