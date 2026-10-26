# M4 B1b (D-34): the audio backends (include/capsule/hal/audio.h), the output model, their tests, apps/audio_render,
# apps/test_audio and apps/sandbox_audio. Included from CMakeLists.txt after cmake/parts/audio.cmake (B1a's core:
# CAPSULE_AUDIO_SOURCES, capsule_audio_table()).
#
# The engine library gets this platform's backend; nothing of it (or of the core) is linked into a program that never
# calls audio::init(): the system HALs reach the audio step only through src/hal/common/audio_hook.h's pointer.
#   GBA        src/hal/gba/audio_gba.cpp: DirectSound FIFOs by DMA1/2 from the core's ring, timer 0, VCOUNT 150, the tone
#              registers; the VBlank step and timer-1 budget (hal_gba.cpp runs it on every blank)
#   desktop    src/hal/sdl2/audio_sdl2.cpp + the output model: an SDL audio device (CAPSULE_AUDIO=0: none)
#   headless   src/hal/headless/audio_headless.cpp + the output model: every frame captured (audio_headless.h)
#   dreamcast  src/hal/dc/audio_dc.cpp: silent (the core still steps)

set(CAPSULE_AUDIO_HOOK_SOURCES ${CAPSULE_ROOT}/src/hal/common/audio_hook.cpp)
set(CAPSULE_AUDIO_MODEL_SOURCES ${CAPSULE_ROOT}/src/hal/common/apu_model.cpp)
# The headless backend on its own (tests and apps/audio_render link it without the system HAL).
set(CAPSULE_AUDIO_HEADLESS_SOURCES ${CAPSULE_ROOT}/src/hal/headless/audio_headless.cpp ${CAPSULE_AUDIO_MODEL_SOURCES}
                                   ${CAPSULE_AUDIO_HOOK_SOURCES})

target_sources(capsule_engine PRIVATE ${CAPSULE_AUDIO_HOOK_SOURCES})
if(CAPSULE_PLATFORM STREQUAL "gba")
   target_sources(capsule_engine PRIVATE ${CAPSULE_ROOT}/src/hal/gba/audio_gba.cpp)
elseif(CAPSULE_PLATFORM STREQUAL "desktop")
   target_sources(capsule_engine PRIVATE ${CAPSULE_ROOT}/src/hal/sdl2/audio_sdl2.cpp ${CAPSULE_AUDIO_MODEL_SOURCES})
elseif(CAPSULE_PLATFORM STREQUAL "dreamcast")
   target_sources(capsule_engine PRIVATE ${CAPSULE_ROOT}/src/hal/dc/audio_dc.cpp)
else()
   target_sources(capsule_engine PRIVATE ${CAPSULE_ROOT}/src/hal/headless/audio_headless.cpp
                                         ${CAPSULE_AUDIO_MODEL_SOURCES})
endif()

# A game's audio table: project.json's "audio" key names its capsule.audio/1 manifest (relative to the game's folder);
# B1a's compiler turns it into the table that replaces the library's empty default. Call after capsule_game(<dir>).
function(capsule_audio_game dir)
   file(READ ${dir}/reference/project.json json)
   string(JSON name GET "${json}" name)
   string(JSON manifest ERROR_VARIABLE err GET "${json}" audio)
   if(err OR NOT manifest OR NOT COMMAND capsule_audio_table)
      return()
   endif()
   set(gen ${CMAKE_CURRENT_BINARY_DIR}/generated/${name})
   capsule_audio_table(${dir}/${manifest} ${gen}/audio_table.cpp)
   target_sources(capsule_${name} PRIVATE ${gen}/audio_table.cpp)
endfunction()

if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM MATCHES "^(desktop|headless)$")
   # The headless backend and the output model against the core stub (tests/support/audio_core_stub.h).
   add_executable(capsule_audio_hal_tests
      ${CAPSULE_ROOT}/tests/audio_hal_tests.cpp
      ${CAPSULE_ROOT}/tests/support/audio_core_stub.cpp
      ${CAPSULE_AUDIO_HEADLESS_SOURCES})
   target_include_directories(capsule_audio_hal_tests PRIVATE ${CAPSULE_ROOT}/include ${CAPSULE_ROOT}/tests)
   target_link_libraries(capsule_audio_hal_tests PRIVATE capsule_options)
   add_test(NAME audio_hal_tests COMMAND capsule_audio_hal_tests)
endif()

if(CAPSULE_PLATFORM MATCHES "^(desktop|headless)$" AND DEFINED CAPSULE_AUDIO_SOURCES)
   # apps/audio_render: the core, the headless backend and a table, without the system HAL. The default build renders
   # the engine's test content (tests/data/audio); capsule_audio_render(<target> <manifest>) builds one for another
   # manifest (the studio's player, a game's own tools).
   # HQ (optional third argument TRUE): the core and the backend with CAPSULE_AUDIO_HQ, the high-quality desktop mix.
   function(capsule_audio_render target manifest)
      set(hq ${ARGV2})
      set(table)
      if(manifest AND COMMAND capsule_audio_table)
         set(table ${CMAKE_CURRENT_BINARY_DIR}/generated/${target}/audio_table.cpp)
         capsule_audio_table(${manifest} ${table})
      elseif(DEFINED CAPSULE_AUDIO_DEFAULT_SOURCES)
         set(table ${CAPSULE_AUDIO_DEFAULT_SOURCES})   # the empty table
      endif()
      add_executable(${target}
         ${CAPSULE_ROOT}/apps/audio_render/src/audio_render.cpp
         ${CAPSULE_ROOT}/apps/common/audio_tour.cpp
         ${CAPSULE_AUDIO_SOURCES}
         ${CAPSULE_AUDIO_HEADLESS_SOURCES}
         ${table})
      target_include_directories(${target} PRIVATE ${CAPSULE_ROOT}/include ${CAPSULE_ROOT}/src ${CAPSULE_ROOT}/apps/common)
      target_link_libraries(${target} PRIVATE capsule_options)
      if(hq)
         target_compile_definitions(${target} PRIVATE CAPSULE_AUDIO_HQ)
      endif()
   endfunction()
   set(CAPSULE_AUDIO_TEST_MANIFEST ${CAPSULE_ROOT}/tests/data/audio/audio.json)
   if(NOT EXISTS ${CAPSULE_AUDIO_TEST_MANIFEST})
      set(CAPSULE_AUDIO_TEST_MANIFEST "")
   endif()
   capsule_audio_render(capsule_audio_render "${CAPSULE_AUDIO_TEST_MANIFEST}")
   capsule_audio_render(capsule_audio_render_hq "${CAPSULE_AUDIO_TEST_MANIFEST}" TRUE)
   if(CAPSULE_ENGINE_TESTS)
      # The tour over the test content: every stream written, consistent (tests/test_audio_render.py); the HQ build
      # keeps the same 8-bit ring and states (its L1-L3 hashes equal the plain build's), only the WAV differs.
      add_test(NAME audio_render_tests
               COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_audio_render.py $<TARGET_FILE:capsule_audio_render>
                       $<TARGET_FILE:capsule_audio_render_hq>)
   endif()
endif()

if(NOT CAPSULE_PLATFORM STREQUAL "headless" AND CAPSULE_ENGINE_TESTS AND CAPSULE_TEST_APPS AND DEFINED CAPSULE_AUDIO_SOURCES)
   # The audio test app (the tour, with the GBA's budget) and the audio sandbox (sounds and clips by hand).
   foreach(app test_audio sandbox_audio)
      capsule_game(${CAPSULE_ROOT}/apps/${app})
      capsule_audio_game(${CAPSULE_ROOT}/apps/${app})
   endforeach()
endif()
