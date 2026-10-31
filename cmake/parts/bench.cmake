# Battle test bench (D-23 section 3.3: apps/battle_bench, capsule.bench/1
# scenarios). Included from CMakeLists.txt after every engine and test target
# exists. Desktop and headless only: the bench is a host program, never built
# for the GBA.
#
# capsule_battle_bench(<target> <manifest> [RNG <name> RNG_SOURCE <file>])
# builds the bench for one game's battle manifest:
# - the battle data, generated like the battle tests' (tools/battle_compiler.py
#   into <binary dir>/bench/<target>: battle.cpp, battle_ids.h);
# - the bench tables (tools/bench_scenario.py emit-data: the manifest's names,
#   effect owner and hook names and the debug pc -> source line table, which
#   only bench builds have);
# - the engine's battle modules, with two link-time seams for the traces
#   (apps/battle_bench/trace.h): the generator's next16() and the effect VM
#   host functions of src/battle/effect_host.cpp are compiled under other
#   names, and the bench defines the originals, records and forwards;
# - RNG: the generator's name (a scenario's "rng" must name it) and, for any
#   name but "default" (the engine's neutral generator), RNG_SOURCE: the source
#   that defines next16() (a game's own, or the reference repo's Method-1 one,
#   which replays the WP-2 goldens there).
#
# The studio builds a game's bench by configuring the engine once into a cache
# folder with -DCAPSULE_BENCH_MANIFEST=<battle.json> (and optionally
# -DCAPSULE_BENCH_RNG=<name> -DCAPSULE_BENCH_RNG_SOURCE=<file>) and building the
# target capsule_bench_game (tools/bench_scenario.py build).

set(CAPSULE_BENCH_MANIFEST "" CACHE FILEPATH "A game's battle manifest: builds its bench as capsule_bench_game")
set(CAPSULE_BENCH_RNG "default" CACHE STRING "Generator name of capsule_bench_game (default: the engine's)")
set(CAPSULE_BENCH_RNG_SOURCE "" CACHE FILEPATH "Generator source of capsule_bench_game when CAPSULE_BENCH_RNG is not default")

# The bench's own sources.
set(CAPSULE_BENCH_SOURCES
   apps/battle_bench/json.cpp
   apps/battle_bench/main.cpp
   apps/battle_bench/output.cpp
   apps/battle_bench/scenario.cpp
   apps/battle_bench/trace.cpp)
# What the bench tables depend on besides the battle compiler's files.
set(CAPSULE_BENCH_TOOL ${CAPSULE_ROOT}/tools/bench_scenario.py)
# The effect VM host functions the bench traces (renamed traced_<name> in its copy of effect_host.cpp).
set(CAPSULE_BENCH_HOST_FUNCTIONS
   host_get host_set host_has host_role_get host_role_set host_battlers host_below host_between host_chance
   host_odds host_action)

# Builds a battle bench.
# @param target    the executable
# @param manifest  the game's battle manifest (capsule.battle/1)
# @param RNG         (optional) the generator's name: default (the engine's generator) or a game's
# @param RNG_SOURCE  with a name other than default: the generator's source (defines capsule::rng::next16())
function(capsule_battle_bench target manifest)
   cmake_parse_arguments(BENCH "" "RNG;RNG_SOURCE" "" ${ARGN})
   if(NOT CAPSULE_PLATFORM MATCHES "^(desktop|headless)$")
      message(FATAL_ERROR "capsule_battle_bench(${target}): the bench is a desktop / headless host program")
   endif()
   if(NOT BENCH_RNG)
      set(BENCH_RNG default)
   endif()
   if(BENCH_RNG STREQUAL "default")
      set(generator ${CAPSULE_ROOT}/${CAPSULE_RNG_DEFAULT_SOURCE})
   elseif(BENCH_RNG_SOURCE)
      get_filename_component(generator ${BENCH_RNG_SOURCE} ABSOLUTE)
   else()
      message(FATAL_ERROR "capsule_battle_bench(${target}): RNG '${BENCH_RNG}' needs RNG_SOURCE (its generator source)")
   endif()
   get_filename_component(manifest ${manifest} ABSOLUTE)
   set(dir ${CMAKE_CURRENT_BINARY_DIR}/bench/${target})

   # The battle data (as the battle tests generate theirs) and the bench tables, from the same manifest.
   add_custom_command(
      OUTPUT  ${dir}/battle.cpp ${dir}/battle_ids.h
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/battle_compiler.py
            ${manifest} ${dir}/battle.cpp --ids ${dir}/battle_ids.h --depfile ${dir}/battle.d
            --platform ${CAPSULE_PLATFORM}
      DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_BATTLE_COMPILER_DEPENDS} ${manifest}
      DEPFILE ${dir}/battle.d
      COMMENT "Compiling the battle data of ${target}")
   add_custom_command(
      OUTPUT  ${dir}/bench_data.cpp
      COMMAND Python3::Interpreter ${CAPSULE_BENCH_TOOL} emit-data
            ${manifest} ${dir}/bench_data.cpp --depfile ${dir}/bench_data.d --platform ${CAPSULE_PLATFORM}
      DEPENDS ${CAPSULE_TEXT_DEPENDS} ${CAPSULE_BATTLE_COMPILER_DEPENDS} ${CAPSULE_BENCH_TOOL} ${manifest}
      DEPFILE ${dir}/bench_data.d
      COMMENT "Compiling the bench tables of ${target}")

   # The seams: the generator and the VM host compiled under other names (the bench defines the originals). Each
   # is a generated source that renames the functions and includes the engine's file, so the other targets that
   # build those files stay unchanged.
   set(renames "")
   foreach(fn ${CAPSULE_BENCH_HOST_FUNCTIONS})
      string(APPEND renames "#define ${fn} traced_${fn}\n")
   endforeach()
   file(CONFIGURE OUTPUT ${dir}/seam_host.cpp CONTENT
        "// Generated by cmake/parts/bench.cmake. Do not edit.\n${renames}#include \"${CAPSULE_ROOT}/src/battle/effect_host.cpp\"\n")
   file(CONFIGURE OUTPUT ${dir}/seam_rng.cpp CONTENT
        "// Generated by cmake/parts/bench.cmake. Do not edit.\n#define next16 bench_generator_next16\n#include \"${generator}\"\n")

   # The battle modules without the host (seam_host.cpp builds it), the records and the global streams.
   set(battle ${CAPSULE_BATTLE_SOURCES})
   list(REMOVE_ITEM battle src/battle/effect_host.cpp)
   list(TRANSFORM battle PREPEND ${CAPSULE_ROOT}/)
   list(TRANSFORM CAPSULE_BENCH_SOURCES PREPEND ${CAPSULE_ROOT}/ OUTPUT_VARIABLE bench)
   add_executable(${target}
      ${bench}
      ${battle}
      ${CAPSULE_ROOT}/src/monster/monster.cpp
      ${CAPSULE_ROOT}/${CAPSULE_RNG_STREAM_SOURCES}
      ${dir}/battle.cpp
      ${dir}/battle_ids.h
      ${dir}/bench_data.cpp
      ${dir}/seam_host.cpp
      ${dir}/seam_rng.cpp)
   target_include_directories(${target} PRIVATE
      ${CAPSULE_ROOT}/include ${CAPSULE_ROOT}/apps/battle_bench ${CAPSULE_ROOT}/tests ${dir})
   target_compile_definitions(${target} PRIVATE CAPSULE_BENCH_RNG="${BENCH_RNG}")
   # G4: the growth context's defaults (the game's context values and clock; growth.cmake)
   capsule_growth_defaults()
   target_link_libraries(${target} PRIVATE capsule_options capsule_growth_defaults)
endfunction()

if(CAPSULE_PLATFORM MATCHES "^(desktop|headless)$")
   # A game's bench (the studio's cache build).
   if(CAPSULE_BENCH_MANIFEST)
      capsule_battle_bench(capsule_bench_game ${CAPSULE_BENCH_MANIFEST}
                           RNG ${CAPSULE_BENCH_RNG} RNG_SOURCE "${CAPSULE_BENCH_RNG_SOURCE}")
   endif()
endif()

if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM MATCHES "^(desktop|headless)$")
   # The engine's bench: the Starter sample (E9) with the engine's default generator, so its scenarios replay the
   # neutral goldens. The classic sample's bench and its WP-2 scenarios live in the reference-ruleset repo (E11).
   capsule_battle_bench(capsule_starter_bench ${CAPSULE_ROOT}/samples/starter/battle.json RNG default)

   # The Starter sample's scenarios: each tests/bench/starter/*.json reproduces its neutral golden transcript.
   file(GLOB CAPSULE_STARTER_BENCH_SCENARIOS CONFIGURE_DEPENDS ${CAPSULE_ROOT}/tests/bench/starter/*.json)
   foreach(scenario ${CAPSULE_STARTER_BENCH_SCENARIOS})
      get_filename_component(name ${scenario} NAME_WE)
      add_test(NAME bench_starter_${name}
               COMMAND Python3::Interpreter ${CAPSULE_BENCH_TOOL} check ${scenario}
                       --bench $<TARGET_FILE:capsule_starter_bench>)
   endforeach()
   # The scenario tool: validation, parsing, pokes, the VM trace and error handling against the bench.
   add_test(NAME bench_scenario_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_bench_scenario.py)
   set_tests_properties(bench_scenario_tests PROPERTIES
                        ENVIRONMENT "CAPSULE_BENCH=$<TARGET_FILE:capsule_starter_bench>")
endif()
