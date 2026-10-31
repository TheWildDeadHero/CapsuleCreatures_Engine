# Random number generator (A-4, D-22 decision 4) sources. Included from
# CMakeLists.txt before the engine library.
#
# include/capsule/core/random.h declares rng::next16(); one source file per
# program defines it (the generator). src/core/random.cpp holds the global
# streams and seed(), and works with any generator.
# - CAPSULE_RNG_SOURCES: built into the engine library, so every game gets the
#   streams and the neutral default generator (src/core/random_default.cpp).
# - CAPSULE_TEST_RNG_SOURCES: the test programs that build the generator
#   themselves; the engine's tests use the default generator too.
# - capsule_rng(<target> default) or capsule_rng(<target> SOURCE <file>): links
#   a generator into one program. A generator object in the program itself
#   replaces the engine library's default at link time (the library member
#   that defines next16() is then never pulled in), the same way default_font
#   and battle_empty are replaced. A game or a ruleset repository with its own
#   generator (the reference repo's compat module, D-23 section 4) passes its
#   source file.
#
# Paths are relative to the engine checkout (CAPSULE_ROOT).
set(CAPSULE_RNG_STREAM_SOURCES src/core/random.cpp)
set(CAPSULE_RNG_DEFAULT_SOURCE src/core/random_default.cpp)
set(CAPSULE_RNG_SOURCES ${CAPSULE_RNG_STREAM_SOURCES} ${CAPSULE_RNG_DEFAULT_SOURCE})
set(CAPSULE_TEST_RNG_SOURCES ${CAPSULE_RNG_SOURCES})

# Links a generator into one program: default (the engine's neutral one) or
# SOURCE <file> (a source that defines capsule::rng::next16()). A program that
# does not link the engine library also needs CAPSULE_RNG_STREAM_SOURCES for
# the global streams.
function(capsule_rng target generator)
   if(generator STREQUAL "default")
      target_sources(${target} PRIVATE ${CAPSULE_ROOT}/${CAPSULE_RNG_DEFAULT_SOURCE})
   elseif(generator STREQUAL "SOURCE" AND ARGC EQUAL 3)
      get_filename_component(source ${ARGV2} ABSOLUTE)
      target_sources(${target} PRIVATE ${source})
   else()
      message(FATAL_ERROR "capsule_rng(${target}): default, or SOURCE <file> (a generator source)")
   endif()
endfunction()

# The generator tests (tests/random_tests.cpp): determinism (known first
# draws), distribution, period and ranges of one generator, as the ctest
# <name>. The engine's own (random_default_tests: the default generator) is
# added by the test section of CMakeLists.txt; a ruleset repository tests its
# own generator the same way.
# @param name    the ctest (the program is capsule_<name>)
# @param SOURCE  (optional) the generator's source; without it, the default
# @param SPEC    with SOURCE: the generator's description for the tests (kName,
#                reference(), kKnown; see tests/support/random_default_spec.h)
function(capsule_rng_test name)
   cmake_parse_arguments(arg "" "SOURCE;SPEC" "" ${ARGN})
   set(t capsule_${name})
   add_executable(${t} ${CAPSULE_ROOT}/tests/random_tests.cpp ${CAPSULE_ROOT}/${CAPSULE_RNG_STREAM_SOURCES})
   if(NOT arg_SOURCE)
      capsule_rng(${t} default)
   elseif(arg_SPEC)
      get_filename_component(spec ${arg_SPEC} ABSOLUTE)
      capsule_rng(${t} SOURCE ${arg_SOURCE})
      target_compile_definitions(${t} PRIVATE CAPSULE_RNG_TEST_SPEC="${spec}")
   else()
      message(FATAL_ERROR "capsule_rng_test(${name}): SOURCE <file> needs SPEC <header> (the generator's description)")
   endif()
   target_include_directories(${t} PRIVATE ${CAPSULE_ROOT}/include)
   target_link_libraries(${t} PRIVATE capsule_options)
   add_test(NAME ${name} COMMAND ${t})
endfunction()
