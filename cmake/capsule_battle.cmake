# Battle data and battle test programs for the engine's own tests and for any
# repository that adds the engine as a subproject (the reference-ruleset repo,
# D-23 section 4; a game's test tree). Included from CMakeLists.txt after
# cmake/parts/rng.cmake and cmake/parts/battle.cmake; the functions run in the
# caller's directory and use Python3::Interpreter, which the engine's
# CMakeLists.txt finds as a global imported target (the caller needs no
# find_package(Python3 ...) of its own).
#
# - capsule_battle_data(<dir> <manifest> [DEPENDS ...] [COMMENT ...]): compiles
#   a battle manifest (capsule.battle/1 with its structure manifest and ruleset)
#   into <dir>/test_battle.cpp + <dir>/battle_ids.h for this build's platform.
# - capsule_battle_library(<target> <manifest> [DIR <dir>] [RNG_SOURCE <file>]
#   [DEPENDS ...]): an OBJECT library with that data and a generator (default:
#   the engine's), for test programs that share it; a program linking it also
#   gets the engine's battle modules, the creature records and the random
#   streams, compiled once per build for every data set (capsule_battle_modules:
#   no module includes a data set's battle_ids.h, kData is extern, and a build
#   has one platform).
# - capsule_battle_test(<name> <manifest> SOURCES ... [DIR <dir>]
#   [RNG_SOURCE <file>] [ARGS ...] [DEPENDS ...]): one test program
#   capsule_<name> built against the data (kData is per program) and the ctest
#   <name> running it with ARGS.
# The data goes to <current binary dir>/generated/<dir> (default: the name).
# The manifest's folder (its JSON and .ceffect files, also under effects/) is a
# dependency; DEPENDS adds files outside it (a ruleset in another folder).

# Compiles one battle manifest for the test programs.
# @param dir       output folder
# @param manifest  battle.json (capsule.battle/1)
# @param DEPENDS   (optional) more inputs outside the manifest's folder
# @param COMMENT   (optional) the build message
function(capsule_battle_data dir manifest)
   cmake_parse_arguments(arg "" "COMMENT" "DEPENDS" ${ARGN})
   get_filename_component(manifest ${manifest} ABSOLUTE)
   get_filename_component(folder ${manifest} DIRECTORY)
   # The manifest's own files, so a first build and a new source rebuild the data (the depfile names them too).
   file(GLOB inputs CONFIGURE_DEPENDS ${folder}/*.json ${folder}/*.ceffect ${folder}/effects/*.ceffect)
   if(NOT arg_COMMENT)
      set(arg_COMMENT "Compiling the battle data ${manifest}")
   endif()
   add_custom_command(
      OUTPUT  ${dir}/test_battle.cpp ${dir}/battle_ids.h
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/battle_compiler.py
            ${manifest} ${dir}/test_battle.cpp
            --ids ${dir}/battle_ids.h --depfile ${dir}/test_battle.d
            --platform ${CAPSULE_PLATFORM}
      DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_BATTLE_COMPILER_DEPENDS} ${manifest} ${inputs} ${arg_DEPENDS}
      DEPFILE ${dir}/test_battle.d
      COMMENT "${arg_COMMENT}")
endfunction()

# The engine's battle modules, the creature records and the random streams (no generator), compiled once per build
# as the OBJECT library capsule_battle_modules that every data library hands to its programs.
function(capsule_battle_modules)
   if(TARGET capsule_battle_modules)
      return()
   endif()
   set(modules ${CAPSULE_BATTLE_SOURCES} src/monster/monster.cpp ${CAPSULE_RNG_STREAM_SOURCES})
   list(TRANSFORM modules PREPEND ${CAPSULE_ROOT}/)
   add_library(capsule_battle_modules OBJECT ${modules})
   target_include_directories(capsule_battle_modules PUBLIC ${CAPSULE_ROOT}/include)
   target_link_libraries(capsule_battle_modules PUBLIC capsule_options)
endfunction()

# One data set and a generator as an OBJECT library; its programs also get the shared battle modules
# (capsule_battle_modules' objects, as interface sources of the library: a program linking it builds them in).
# @param target      the library
# @param manifest    battle.json
# @param DIR         (optional) output folder name under generated/ (default: the target)
# @param RNG_SOURCE  (optional) a generator source (default: the engine's default generator)
# @param DEPENDS     (optional) more data inputs (capsule_battle_data)
function(capsule_battle_library target manifest)
   cmake_parse_arguments(arg "" "DIR;RNG_SOURCE" "DEPENDS" ${ARGN})
   if(NOT arg_DIR)
      set(arg_DIR ${target})
   endif()
   set(dir ${CMAKE_CURRENT_BINARY_DIR}/generated/${arg_DIR})
   capsule_battle_data(${dir} ${manifest} DEPENDS ${arg_DEPENDS})
   capsule_battle_modules()
   if(arg_RNG_SOURCE)
      get_filename_component(generator ${arg_RNG_SOURCE} ABSOLUTE)
   else()
      set(generator ${CAPSULE_ROOT}/${CAPSULE_RNG_DEFAULT_SOURCE})
   endif()
   add_library(${target} OBJECT ${generator} ${dir}/test_battle.cpp ${dir}/battle_ids.h)
   # The data folder first: its battle_ids.h is the one the programs include.
   target_include_directories(${target} BEFORE PUBLIC ${dir} ${CAPSULE_ROOT}/include)
   target_link_libraries(${target} PUBLIC capsule_options)
   # the shared modules' objects go into every program that links this library (object files of an OBJECT
   # library reach only its direct consumers, so they are interface sources here)
   target_sources(${target} INTERFACE $<TARGET_OBJECTS:capsule_battle_modules>)
   add_dependencies(${target} capsule_battle_modules)
   # G4: the growth context's defaults (the game's context values and clock), a static library: a program's own win
   capsule_growth_defaults()
   target_link_libraries(${target} INTERFACE capsule_growth_defaults)
endfunction()

# One battle test program against its own data set, and its ctest.
# @param name        the ctest (the program is capsule_<name>)
# @param manifest    battle.json
# @param SOURCES     the test's sources
# @param DIR         (optional) output folder name under generated/ (default: the name)
# @param RNG_SOURCE  (optional) a generator source (default: the engine's default generator)
# @param ARGS        (optional) the program's command-line arguments
# @param DEPENDS     (optional) more data inputs (capsule_battle_data)
function(capsule_battle_test name manifest)
   cmake_parse_arguments(arg "" "DIR;RNG_SOURCE" "SOURCES;ARGS;DEPENDS" ${ARGN})
   if(NOT arg_DIR)
      set(arg_DIR ${name})
   endif()
   set(library_args DIR ${arg_DIR} DEPENDS ${arg_DEPENDS})
   if(arg_RNG_SOURCE)
      list(APPEND library_args RNG_SOURCE ${arg_RNG_SOURCE})
   endif()
   capsule_battle_library(capsule_${name}_data ${manifest} ${library_args})
   add_executable(capsule_${name} ${arg_SOURCES})
   target_link_libraries(capsule_${name} PRIVATE capsule_${name}_data)
   add_test(NAME ${name} COMMAND capsule_${name} ${arg_ARGS})
endfunction()
