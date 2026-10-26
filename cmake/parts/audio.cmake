# M4 B1a (D-34): the audio core (include/capsule/audio/: songs on players, notes and channels, envelopes, the mixer,
# the tone channels, clips; src/audio/), its compiler (tools/audio_compiler.py with tools/audio/format.py and
# gen_tables.py), its tests and their content (tests/data/audio). Included from CMakeLists.txt before
# cmake/parts/audio_hal.cmake (B1b's backends use CAPSULE_AUDIO_SOURCES and capsule_audio_table()).
#
# Nothing of the core is linked into a program that never starts it (audio::init()): the engine library holds it,
# and src/audio/audio_default.cpp (the empty table) only when no generated table replaces it.
#   CAPSULE_AUDIO_SOURCES          the core without a table (programs built from sources: tests, apps/audio_render)
#   CAPSULE_AUDIO_DEFAULT_SOURCES  the empty table
#   capsule_audio_table(<manifest> <out.cpp> [PARTS n])  compiles a capsule.audio/1 manifest into a table source
#                                  (with PARTS, also <out>_1.cpp .. <out>_n.cpp: the samples and songs, <= 1 MB each)
#   CAPSULE_AUDIO_HQ               option (desktop only, off): the high-quality desktop mix (never in parity runs)

set(CAPSULE_AUDIO_SOURCES
   ${CAPSULE_ROOT}/src/audio/audio.cpp
   ${CAPSULE_ROOT}/src/audio/sequencer.cpp
   ${CAPSULE_ROOT}/src/audio/notes.cpp
   ${CAPSULE_ROOT}/src/audio/envelope.cpp
   ${CAPSULE_ROOT}/src/audio/mixer.cpp
   ${CAPSULE_ROOT}/src/audio/tone.cpp
   ${CAPSULE_ROOT}/src/audio/clip.cpp
   ${CAPSULE_ROOT}/src/audio/tables.cpp)
set(CAPSULE_AUDIO_DEFAULT_SOURCES ${CAPSULE_ROOT}/src/audio/audio_default.cpp)
set(CAPSULE_AUDIO_COMPILER_DEPENDS ${CAPSULE_ROOT}/tools/audio_compiler.py ${CAPSULE_ROOT}/tools/audio/format.py
                                   ${CAPSULE_ROOT}/tools/sequence_compiler.py)
target_sources(capsule_engine PRIVATE ${CAPSULE_AUDIO_SOURCES} ${CAPSULE_AUDIO_DEFAULT_SOURCES})

option(CAPSULE_AUDIO_HQ "Desktop: the high-quality audio mix (4x the samples, 16-bit sums)" OFF)
if(CAPSULE_AUDIO_HQ AND CAPSULE_PLATFORM STREQUAL "desktop")
   target_compile_definitions(capsule_engine PUBLIC CAPSULE_AUDIO_HQ)
endif()

function(capsule_audio_table manifest out)
   cmake_parse_arguments(arg "" "PARTS" "" ${ARGN})
   set(outputs ${out})
   set(part_args)
   if(arg_PARTS)
      get_filename_component(dir ${out} DIRECTORY)
      get_filename_component(stem ${out} NAME_WE)
      foreach(i RANGE 1 ${arg_PARTS})
         list(APPEND outputs ${dir}/${stem}_${i}.cpp)
      endforeach()
      set(part_args --parts ${arg_PARTS})
   endif()
   # C14a (S7): the output folder exists before the compiler runs (Ninja creates it; the Makefile generators do not)
   get_filename_component(out_dir ${out} DIRECTORY)
   file(MAKE_DIRECTORY ${out_dir})
   add_custom_command(
      OUTPUT  ${outputs}
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/audio_compiler.py ${manifest} ${out} ${part_args}
            --depfile ${out}.d
      DEPENDS ${CAPSULE_AUDIO_COMPILER_DEPENDS} ${manifest}
      DEPFILE ${out}.d   # the samples and songs the manifest names
      COMMENT "Compiling audio ${manifest}")
endfunction()

if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM MATCHES "^(desktop|headless)$")
   # The core against the test content and the recording backend (tests/support/audio_test_hal.h); the second
   # program is the same with the high-quality mix compiled in (its 8-bit output and states must not change).
   capsule_audio_table(${CAPSULE_ROOT}/tests/data/audio/audio.json ${CAPSULE_GENERATED_DIR}/test_audio_table.cpp)
   foreach(t capsule_audio_tests capsule_audio_hq_tests)
      add_executable(${t}
         ${CAPSULE_ROOT}/tests/audio_tests.cpp
         ${CAPSULE_ROOT}/tests/support/audio_test_hal.cpp
         ${CAPSULE_AUDIO_SOURCES}
         ${CAPSULE_GENERATED_DIR}/test_audio_table.cpp)
      target_include_directories(${t} PRIVATE ${CAPSULE_ROOT}/include ${CAPSULE_ROOT}/src ${CAPSULE_ROOT}/tests)
      target_link_libraries(${t} PRIVATE capsule_options)
   endforeach()
   target_compile_definitions(capsule_audio_hq_tests PRIVATE CAPSULE_AUDIO_HQ)
   add_test(NAME audio_tests COMMAND capsule_audio_tests)
   add_test(NAME audio_hq_tests COMMAND capsule_audio_hq_tests)
   # L1-L3 of every test script against the Python reference model, and the golden hashes.
   add_test(NAME audio_reference_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_audio_reference.py
                    --exe $<TARGET_FILE:capsule_audio_tests> --hq-exe $<TARGET_FILE:capsule_audio_hq_tests>)
   add_test(NAME audio_compiler_tests COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_audio_compiler.py)
endif()
