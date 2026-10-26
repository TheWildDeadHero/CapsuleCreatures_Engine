# One build for every platform (P-8):
#   desktop    SDL2 HAL on Linux, macOS and Windows (MinGW-w64), plus the tests
#              (presets desktop-* on Linux/macOS, windows-* on Windows)
#   headless   engine core + tests on the software video model, no SDL and no
#              apps; for CI and cross-compiled big-endian checks (P-19)
#              (preset headless, big-endian-check)
#   gba        devkitARM + libgba ROMs, selected by cmake/toolchains/gba-devkitarm.cmake
#              (presets gba-debug / gba-release)
#   dreamcast  KallistiOS ELF + selfboot CDI, selected by cmake/toolchains/dreamcast-kos.cmake
#              (presets dc-debug / dc-release)
cmake_minimum_required(VERSION 3.24)

# macOS: universal binaries (Apple silicon + Intel) by default, runnable back to
# macOS 10.15. Must be set before project(); a -D on the command line wins.
# Ignored for the GBA toolchain (its CMAKE_SYSTEM_NAME is Generic).
if(CMAKE_HOST_APPLE)
   set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures to build")
   set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
endif()

project(capsule_engine LANGUAGES C CXX)

# The engine checkout (this file's folder). Every engine path below is built from it, so the engine also works as a
# subproject (add_subdirectory) of another repository: the reference-ruleset repo (D-23 section 4) or a game's tree.
set(CAPSULE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
# The engine's own tests, test apps and sample apps; off when the engine is a subproject (D-23 section 4).
option(CAPSULE_ENGINE_TESTS "Build the engine's own tests and apps" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED CAPSULE_PLATFORM)
   set(CAPSULE_PLATFORM desktop)
endif()
set(CAPSULE_PLATFORMS desktop headless gba dreamcast)
if(NOT CAPSULE_PLATFORM IN_LIST CAPSULE_PLATFORMS)
   message(FATAL_ERROR "CAPSULE_PLATFORM must be one of ${CAPSULE_PLATFORMS}, got '${CAPSULE_PLATFORM}'")
endif()
message(STATUS "Capsule platform: ${CAPSULE_PLATFORM}")
option(CAPSULE_TEST_APPS "Build the per-system test apps (apps/test_*)" ON)
# C6: the GBA cartridge's save medium: 64 KB flash (default) or 32 KB SRAM (the save image's budget follows it:
# tools/save_layout.py --platform gba_sram, capsule::save::kSaveBudget, src/hal/gba/save_gba.cpp).
option(CAPSULE_GBA_SAVE_SRAM "GBA: save to the cartridge's 32 KB SRAM instead of its 64 KB flash" OFF)
# UM (D-33): every GBA link fails when EWRAM (.ewram + .sbss) passes 256 KB or IWRAM leaves less stack than this
# below __sp_usr (cmake/gba_memory.ld), and prints both regions' use (cmake/gba_memory_report.cmake). The stack holds
# the deepest call chain plus what the interrupt handlers push (they switch to their own 160 B stack first).
set(CAPSULE_GBA_STACK_RESERVE 2048 CACHE STRING "GBA: bytes of IWRAM the link keeps free for the user stack")
# OFF only to measure a program that does not fit (the report then says by how much); never for a ROM that runs.
option(CAPSULE_GBA_MEMORY_CHECKS "GBA: fail the link past EWRAM's end or below the stack reserve" ON)

# API reference: `cmake --build --preset <any> --target docs` writes
# build/docs/html/index.html. Doxyfile fails on any undocumented item, which
# is the project's documentation rule (README, "Conventions").
find_package(Doxygen 1.9.8 QUIET)
if(DOXYGEN_FOUND)
   add_custom_target(docs
      COMMAND Doxygen::doxygen ${CAPSULE_ROOT}/Doxyfile
      WORKING_DIRECTORY ${CAPSULE_ROOT}
      COMMENT "Building the API reference (build/docs/html)"
      VERBATIM)
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)   # the tools use 3.10 type syntax at import
if(CAPSULE_PLATFORM STREQUAL "desktop")
   if(WIN32 AND NOT MINGW)
      message(FATAL_ERROR "Windows builds use MinGW-w64 (GCC). Use the windows-* presets from an MSYS2 MINGW64 shell.")
   endif()
   include(cmake/sdl2.cmake)   # pinned SDL2, built static with the project
endif()

# P-8 / P-10: no RTTI, no exceptions, dead code stripped. The engine is
# single-threaded (the GBA has no threads; the host HALs run one loop), so
# function-local statics need no guard: -fno-threadsafe-statics drops the
# __cxa_guard calls (the GBA toolchain file sets it too).
add_library(capsule_options INTERFACE)
target_compile_options(capsule_options INTERFACE
   -Wall -Wextra -Wpedantic -Wshadow
   -fno-rtti -fno-exceptions
   $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
   -ffunction-sections -fdata-sections
   # Assert messages show repo-relative paths, not the build machine's.
   -fmacro-prefix-map=${CAPSULE_ROOT}/=)
if(CAPSULE_PLATFORM STREQUAL "gba" AND CAPSULE_GBA_SAVE_SRAM)
   target_compile_definitions(capsule_options INTERFACE CAPSULE_SAVE_SRAM)
endif()
# Dead-code stripping: Apple's linker spells it -dead_strip.
target_link_options(capsule_options INTERFACE
   $<IF:$<PLATFORM_ID:Darwin>,LINKER:-dead_strip,LINKER:--gc-sections>)
# MinGW: link the C++ runtime and pthreads statically, so executables run
# without MinGW DLLs next to them.
if(MINGW)
   target_link_options(capsule_options INTERFACE -static)
endif()
if(CAPSULE_PLATFORM STREQUAL "gba")
   set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)   # P-10: LTO on every GBA build
endif()

# Runtime build settings (A-2, A-5). Each game sets them in the "build" object
# of its reference/project.json (tools/build_settings.py checks them; the
# studio edits them). A non-empty cache variable overrides every game's value.
# Pairs: project.json key, cache variable.
set(CAPSULE_BUILD_SETTINGS
   vram_must_land_slots CAPSULE_VRAM_MUST_LAND_SLOTS
   vram_streaming_slots CAPSULE_VRAM_STREAMING_SLOTS
   vram_budget_cycles   CAPSULE_VRAM_BUDGET_CYCLES
   gba_waitcnt          CAPSULE_GBA_WAITCNT
   screen_arena_bytes   CAPSULE_SCREEN_ARENA_BYTES)
set(CAPSULE_VRAM_MUST_LAND_SLOTS "" CACHE STRING "VRAM queue: must-land slots for every game (power of two; default 32)")
set(CAPSULE_VRAM_STREAMING_SLOTS "" CACHE STRING "VRAM queue: streaming slots for every game (power of two; default 64)")
set(CAPSULE_VRAM_BUDGET_CYCLES "" CACHE STRING "VRAM queue: cycles per VBlank for every game (default 83776 - 16384)")
set(CAPSULE_GBA_WAITCNT "" CACHE STRING "GBA: WAITCNT for every game (default 0x4014: prefetch, ROM 3/1)")
set(CAPSULE_SCREEN_ARENA_BYTES "" CACHE STRING "Screens: bytes of the screen arena for every game (default 32768)")
# Cache overrides as tools/build_settings.py --set arguments, and as compile
# definitions for the tests that build the queue themselves.
set(CAPSULE_BUILD_SETTING_ARGS)
set(CAPSULE_BUILD_SETTING_TEST_DEFS)
set(_pairs ${CAPSULE_BUILD_SETTINGS})
while(_pairs)
   list(POP_FRONT _pairs _key _var)
   if(NOT "${${_var}}" STREQUAL "")
      list(APPEND CAPSULE_BUILD_SETTING_ARGS --set ${_key}=${${_var}})
      list(APPEND CAPSULE_BUILD_SETTING_TEST_DEFS ${_var}=${${_var}})
   endif()
endwhile()
if(CAPSULE_PLATFORM STREQUAL "gba")
   target_compile_definitions(capsule_options INTERFACE CAPSULE_PLATFORM_GBA=1)
endif()

# Text (C-2, D-20): every compiler that writes text encodes it with
# tools/text_encoder.py and a code page; without one, the engine's ASCII page.
set(CAPSULE_TEXT_DEPENDS ${CAPSULE_ROOT}/tools/text_encoder.py ${CAPSULE_ROOT}/assets/text/ascii.codepage.json)

# P-1: the default font is compiled from its asset at build time.
set(CAPSULE_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
   OUTPUT  ${CAPSULE_GENERATED_DIR}/default_font.cpp
   COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/font_compiler.py
         ${CAPSULE_ROOT}/assets/fonts/default_8x8.txt
         ${CAPSULE_GENERATED_DIR}/default_font.cpp kDefaultFont
   DEPENDS ${CAPSULE_ROOT}/tools/font_compiler.py ${CAPSULE_TEXT_DEPENDS}
         ${CAPSULE_ROOT}/assets/fonts/default_8x8.txt
   COMMENT "Compiling default font")

# B-11: each build compiles its own input target.
function(capsule_input_map target symbol out)
   add_custom_command(
      OUTPUT  ${out}
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/input_compiler.py
            ${CAPSULE_ROOT}/assets/input/default_input.json ${target} ${out} ${symbol}
      DEPENDS ${CAPSULE_ROOT}/tools/input_compiler.py
            ${CAPSULE_ROOT}/assets/input/default_input.json
      COMMENT "Compiling input map (${target}) as ${symbol}")
endfunction()
if(CAPSULE_PLATFORM STREQUAL "gba")
   capsule_input_map(gba kInputMap ${CAPSULE_GENERATED_DIR}/input_map.cpp)
elseif(CAPSULE_PLATFORM STREQUAL "dreamcast")
   capsule_input_map(dreamcast kInputMap ${CAPSULE_GENERATED_DIR}/input_map.cpp)
else()
   capsule_input_map(snes_layout kInputMap ${CAPSULE_GENERATED_DIR}/input_map.cpp)
endif()

# Sequence manifests (title flows, cutscenes, credits) compile to ROM tables.
# An optional fourth argument names the game's code page (checked: text on bg3).
function(capsule_sequence manifest symbol out)
   set(page_args)
   set(page_depends)
   if(ARGC GREATER 3)
      set(page_args --code-page ${ARGV3})
      set(page_depends ${ARGV3})
   endif()
   add_custom_command(
      OUTPUT  ${out}
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/sequence_compiler.py
            ${manifest} ${out} --symbol ${symbol} ${page_args}
      DEPENDS ${CAPSULE_ROOT}/tools/sequence_compiler.py ${CAPSULE_TEXT_DEPENDS} ${manifest} ${page_depends}
      COMMENT "Compiling sequence ${manifest} as ${symbol}")
endfunction()

# Test manifests (tests/data/sequences): used by the sequence tests and the
# sequence test app. <name>.json -> generated/seq_<name>.inc, symbol kSeq<Name>.
set(CAPSULE_TEST_SEQUENCES title_timeout title_song_end credits_map credits_showcase cutscene)
set(CAPSULE_TEST_SEQUENCE_INCS)
foreach(name ${CAPSULE_TEST_SEQUENCES})
   string(REPLACE "_" ";" parts ${name})
   set(symbol kSeq)
   foreach(part ${parts})
      string(SUBSTRING ${part} 0 1 first)
      string(SUBSTRING ${part} 1 -1 rest)
      string(TOUPPER ${first} first)
      string(APPEND symbol ${first}${rest})
   endforeach()
   capsule_sequence(${CAPSULE_ROOT}/tests/data/sequences/${name}.json ${symbol}
                    ${CAPSULE_GENERATED_DIR}/seq_${name}.inc)
   list(APPEND CAPSULE_TEST_SEQUENCE_INCS ${CAPSULE_GENERATED_DIR}/seq_${name}.inc)
endforeach()

# Per-subsystem fragments (cmake/parts): the source lists a package owns, so
# that package edits its fragment instead of this file.
include(cmake/parts/rng.cmake)      # CAPSULE_RNG_SOURCES, CAPSULE_TEST_RNG_SOURCES
include(cmake/parts/battle.cmake)   # CAPSULE_BATTLE_SOURCES, CAPSULE_BATTLE_COMPILER_DEPENDS
include(cmake/parts/inventory.cmake)   # CAPSULE_INVENTORY_SOURCES (G2: bag, item use, save image)
include(cmake/parts/growth.cmake)   # CAPSULE_GROWTH_SOURCES, capsule_growth_defaults() (G4)
include(cmake/capsule_battle.cmake)  # capsule_battle_data(), capsule_battle_library(), capsule_battle_test()

# Engine core + this platform's HAL.
add_library(capsule_engine STATIC
   src/asset/asset.cpp
   src/asset/asset_table_empty.cpp
   ${CAPSULE_BATTLE_SOURCES}
   ${CAPSULE_INVENTORY_SOURCES}
   ${CAPSULE_GROWTH_SOURCES}
   src/battle/battle_empty.cpp
   src/battle/encounter_lead.cpp
   src/battle/field_actions.cpp
   src/battle/step_effects.cpp
   src/battle/presenter_view.cpp
   ${CAPSULE_RNG_SOURCES}
   src/encounter/encounter.cpp
   src/field/doors.cpp
   src/field/field.cpp
   src/field/field_game_default.cpp
   src/field/field_route_default.cpp
   src/gfx/background.cpp
   src/gfx/palette.cpp
   src/gfx/sprites.cpp
   src/hal/common/seed.cpp
   src/input/input.cpp
   src/map/map.cpp
   src/map/world_empty.cpp
   src/monster/monster.cpp
   src/overworld/overworld.cpp
   src/overworld/locations_empty.cpp
   src/script/script.cpp
   src/script/field_effects.cpp
   src/script/field_actions.cpp
   src/script/step_effects.cpp
   src/script/script_empty.cpp
   src/script/script_game_default.cpp
   src/script/script_i4_default.cpp
   src/script/script_port_hal.cpp
   src/text/font.cpp
   src/text/print.cpp
   src/text/text_layer.cpp
   src/text/window.cpp
   src/seq/sequence.cpp
   src/seq/sequence_port_hal.cpp
   src/seq/sequence_game_default.cpp
   ${CAPSULE_GENERATED_DIR}/default_font.cpp
   ${CAPSULE_GENERATED_DIR}/input_map.cpp)
target_include_directories(capsule_engine PUBLIC include)
target_link_libraries(capsule_engine PUBLIC capsule_options)

if(CAPSULE_PLATFORM STREQUAL "gba")
   add_library(libgba STATIC IMPORTED GLOBAL)   # global: games of a parent repository link it too
   set_target_properties(libgba PROPERTIES IMPORTED_LOCATION ${LIBGBA}/lib/libgba.a)
   target_include_directories(libgba SYSTEM INTERFACE ${LIBGBA}/include)
   target_link_libraries(capsule_engine PUBLIC libgba)
   target_sources(capsule_engine PRIVATE src/hal/gba/save_gba.cpp)   # G2: the cartridge's flash (or SRAM)
else()
   # Every other platform runs the software GBA video model (src/hal/soft).
   target_sources(capsule_engine PRIVATE src/hal/soft/soft_video.cpp)
   # G2: the save medium in RAM, the default a desktop game's file backend (CAPSULE_RUNTIME_SOURCES) replaces
   target_sources(capsule_engine PRIVATE src/hal/common/save_memory.cpp)
   if(CAPSULE_PLATFORM STREQUAL "desktop")
      target_sources(capsule_engine PRIVATE src/hal/sdl2/hal_sdl2.cpp)
      target_link_libraries(capsule_engine PRIVATE SDL2::SDL2-static)
   elseif(CAPSULE_PLATFORM STREQUAL "dreamcast")
      target_sources(capsule_engine PRIVATE src/hal/dc/hal_dc.cpp)
   else()
      target_sources(capsule_engine PRIVATE src/hal/headless/hal_headless.cpp)
   endif()
endif()
# The sources that read build settings are compiled per game, with that game's
# settings (capsule_game()). Headless builds no games: the library has them.
# Absolute, so a parent repository's games compile them too (capsule_game() runs in the caller's directory).
set(CAPSULE_RUNTIME_SOURCES ${CAPSULE_ROOT}/src/hal/common/vram_queue.cpp
                            ${CAPSULE_ROOT}/src/screen/arena_storage.cpp)   # UM: screen_arena_bytes
if(CAPSULE_PLATFORM STREQUAL "gba")
   list(APPEND CAPSULE_RUNTIME_SOURCES ${CAPSULE_ROOT}/src/hal/gba/hal_gba.cpp)
elseif(CAPSULE_PLATFORM STREQUAL "desktop")
   # G2: the save file, named after the game (capsule_game() passes CAPSULE_GAME_NAME)
   list(APPEND CAPSULE_RUNTIME_SOURCES ${CAPSULE_ROOT}/src/hal/sdl2/save_file.cpp)
endif()
if(CAPSULE_PLATFORM STREQUAL "headless")
   target_sources(capsule_engine PRIVATE ${CAPSULE_RUNTIME_SOURCES})
   target_compile_definitions(capsule_engine PRIVATE ${CAPSULE_BUILD_SETTING_TEST_DEFS})
endif()

# Record transfer v2 (include/capsule/monster/transfer.h): its source and capsule_transfer_table(), which
# capsule_game() uses for every game with battle data; its tests are added at the end (capsule_transfer_tests()).
include(cmake/parts/transfer.cmake)

# Apps are laid out like games (P-13 layers): the engine (this library), a
# reference manifest and data.
#   apps/<app>/reference/project.json  capsule.project/1: name, ROM header,
#                                      sources, helper libraries, asset
#                                      manifest, sequences, text
#   apps/<app>/data/                   the app's assets (capsule.assets/1)
#   apps/<app>/src/                    its scene code
# capsule_game(<dir>) builds capsule_<name>: a desktop executable; on GBA
# capsule_<name>.elf + header-fixed .gba + linker map; on Dreamcast
# capsule_<name>.elf + selfboot .cdi when mkdcdisc is installed. Generated
# files go to generated/<name>/ (asset table as assets.cpp, each sequence as
# seq_<file name>.inc). Not built headless.
# Text: "text": {"language": "en", "code_pages": {"en": "<page>.codepage.json"},
# "fonts": {"en": "<font>.txt" or [base font, extra glyph files...]}} (paths
# relative to the app). With a language, every compiler gets its code page and
# the game compiles its own kDefaultFont (font.cpp, which replaces the engine's
# at link time; default font: the engine's); without one, the ASCII page.
function(capsule_project_get out json)
   string(JSON value ERROR_VARIABLE err GET "${json}" ${ARGN})
   if(err)
      set(value "")
   endif()
   set(${out} "${value}" PARENT_SCOPE)
endfunction()

# B1 / B2: whether a game's battle structure declares combination methods and eggs or a nursery (capsule_game()
# links the empty script ports without them). <dir>: the game's folder; <battle>: its battle manifest (relative to
# dir; empty: none); <out_combine> / <out_eggs>: TRUE or FALSE.
function(capsule_battle_parts dir battle out_combine out_eggs)
   set(combine FALSE)
   set(eggs FALSE)
   if(battle)
      file(READ ${dir}/${battle} battle_json)
      string(JSON structure ERROR_VARIABLE err GET "${battle_json}" structure)
      if(err)
         set(structure structure.json)
      endif()
      get_filename_component(battle_dir ${dir}/${battle} DIRECTORY)
      if(EXISTS ${battle_dir}/${structure})
         # once per file (several games share the Starter's structure; Ninja refuses a file listed twice)
         get_filename_component(structure_path ${battle_dir}/${structure} ABSOLUTE)
         get_property(watched DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
         if(NOT structure_path IN_LIST watched)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${structure_path})
         endif()
         file(READ ${structure_path} structure_json)
         string(JSON n ERROR_VARIABLE err LENGTH "${structure_json}" combine_methods)
         if(NOT err AND n GREATER 0)
            set(combine TRUE)
         endif()
         foreach(key eggs nursery)
            string(JSON kind ERROR_VARIABLE err TYPE "${structure_json}" ${key})
            if(NOT err AND kind STREQUAL "OBJECT")
               set(eggs TRUE)
            endif()
         endforeach()
      endif()
   endif()
   set(${out_combine} ${combine} PARENT_SCOPE)
   set(${out_eggs} ${eggs} PARENT_SCOPE)
endfunction()

function(capsule_game dir)
   set(manifest ${dir}/reference/project.json)
   file(READ ${manifest} json)
   set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${manifest})
   capsule_project_get(schema "${json}" schema)
   if(NOT schema STREQUAL "capsule.project/1")
      message(FATAL_ERROR "${manifest}: schema must be capsule.project/1")
   endif()
   capsule_project_get(name "${json}" name)
   capsule_project_get(rom_title "${json}" rom title)
   capsule_project_get(rom_code "${json}" rom code)
   capsule_project_get(rom_maker "${json}" rom maker)
   set(target capsule_${name})
   # Generated files of the game, in the binary folder of the directory that builds it.
   set(gen ${CMAKE_CURRENT_BINARY_DIR}/generated/${name})

   set(sources)
   string(JSON n LENGTH "${json}" sources)
   math(EXPR last "${n} - 1")
   foreach(i RANGE ${last})
      string(JSON src GET "${json}" sources ${i})
      list(APPEND sources ${dir}/${src})
   endforeach()
   add_executable(${target} ${sources})
   target_link_libraries(${target} PRIVATE capsule_engine)
   target_include_directories(${target} PRIVATE ${gen})

   # Build settings (A-2, A-5): the game's "build" object, checked by
   # tools/build_settings.py; cache variables override it.
   set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CAPSULE_ROOT}/tools/build_settings.py)
   execute_process(
      COMMAND ${Python3_EXECUTABLE} ${CAPSULE_ROOT}/tools/build_settings.py ${manifest} --cmake
            ${CAPSULE_BUILD_SETTING_ARGS}
      OUTPUT_VARIABLE settings_defs ERROR_VARIABLE settings_err RESULT_VARIABLE settings_rc)
   if(NOT settings_rc EQUAL 0)
      message(FATAL_ERROR "${settings_err}")
   endif()
   add_library(${target}_runtime OBJECT ${CAPSULE_RUNTIME_SOURCES})
   target_link_libraries(${target}_runtime PRIVATE capsule_engine)
   target_compile_definitions(${target}_runtime PRIVATE ${settings_defs} CAPSULE_GAME_NAME="${name}")
   target_compile_definitions(${target} PRIVATE ${settings_defs})
   target_link_libraries(${target} PRIVATE ${target}_runtime)

   # Text (C-2, D-20): the game's language selects its code page and font.
   set(page)
   set(page_args)
   capsule_project_get(language "${json}" text language)
   if(language)
      capsule_project_get(page "${json}" text code_pages ${language})
      if(NOT page)
         message(FATAL_ERROR "${manifest}: text.code_pages names no code page for the language '${language}'")
      endif()
      set(page ${dir}/${page})
      set(page_args --code-page ${page})
      set(fonts)
      string(JSON font_type ERROR_VARIABLE err TYPE "${json}" text fonts ${language})
      if(err)
         set(fonts ${CAPSULE_ROOT}/assets/fonts/default_8x8.txt)
      elseif(font_type STREQUAL "ARRAY")
         string(JSON n LENGTH "${json}" text fonts ${language})
         math(EXPR last "${n} - 1")
         foreach(i RANGE ${last})
            string(JSON font GET "${json}" text fonts ${language} ${i})
            list(APPEND fonts ${dir}/${font})
         endforeach()
      else()
         string(JSON font GET "${json}" text fonts ${language})
         set(fonts ${dir}/${font})
      endif()
      # The first file is the font; the others add glyphs (font_compiler.py --glyphs).
      list(GET fonts 0 font_args)
      set(more_fonts)
      list(LENGTH fonts font_count)
      if(font_count GREATER 1)   # I4: list(SUBLIST) refuses a begin past the end (a game with the default font)
         list(SUBLIST fonts 1 -1 more_fonts)
      endif()
      foreach(more ${more_fonts})
         list(APPEND font_args --glyphs ${more})
      endforeach()
      add_custom_command(
         OUTPUT  ${gen}/font.cpp
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/font_compiler.py ${font_args}
               ${gen}/font.cpp kDefaultFont ${page_args}
         DEPENDS ${CAPSULE_ROOT}/tools/font_compiler.py ${CAPSULE_TEXT_DEPENDS} ${fonts} ${page}
         COMMENT "Compiling the font of ${name} (${language})")
      target_sources(${target} PRIVATE ${gen}/font.cpp)
   endif()

   string(JSON n ERROR_VARIABLE err LENGTH "${json}" libraries)
   if(NOT err AND n GREATER 0)
      math(EXPR last "${n} - 1")
      foreach(i RANGE ${last})
         string(JSON lib GET "${json}" libraries ${i})
         target_link_libraries(${target} PRIVATE capsule_${lib})
      endforeach()
   endif()

   # Field placeholders (D-22 decision 6): the game's scripts manifest declares them; text assets and battle
   # texts may then use them ({player}: FD 80-FE, resolved when the text prints).
   capsule_project_get(game_scripts "${json}" scripts)
   set(field_args)
   set(field_depends)
   if(game_scripts)
      set(field_args --scripts ${dir}/${game_scripts})
      set(field_depends ${dir}/${game_scripts} ${CAPSULE_ROOT}/tools/script_compiler.py)
   endif()

   capsule_project_get(assets "${json}" assets)
   if(assets)
      add_custom_command(
         OUTPUT  ${gen}/assets.cpp
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/asset_compiler.py ${dir}/${assets} ${gen}/assets.cpp
               --depfile ${gen}/assets.d ${page_args} ${field_args}
         DEPENDS ${CAPSULE_ROOT}/tools/asset_compiler.py ${CAPSULE_ROOT}/tools/sequence_compiler.py
               ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_TEXT_DEPENDS} ${dir}/${assets} ${page} ${field_depends}
         DEPFILE ${gen}/assets.d   # image frames the manifest reads
         COMMENT "Compiling assets of ${name}")
      target_sources(${target} PRIVATE ${gen}/assets.cpp)
   endif()

   # Maps: a capsule.world/1 manifest with its tilesets, maps and encounter tables (C-03, D-12).
   # encounter_ids.h names the world's encounter methods and conditions for the app's code.
   capsule_project_get(world "${json}" maps)
   if(world)
      set(world_args)
      capsule_project_get(debug_names "${json}" debug_names)
      if(debug_names)
         list(APPEND world_args --debug-names)
      endif()
      capsule_project_get(world_scripts "${json}" scripts)
      if(world_scripts)
         list(APPEND world_args --scripts ${dir}/${world_scripts})
      endif()
      add_custom_command(
         OUTPUT  ${gen}/world.cpp ${gen}/encounter_ids.h ${gen}/world_ids.h
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/map_compiler.py ${dir}/${world} ${gen}/world.cpp
               --depfile ${gen}/world.d --ids ${gen}/encounter_ids.h --world-ids ${gen}/world_ids.h ${world_args}
               ${page_args}
         DEPENDS ${CAPSULE_ROOT}/tools/map_compiler.py ${CAPSULE_ROOT}/tools/encounter_compiler.py
               ${CAPSULE_ROOT}/tools/event_compiler.py ${CAPSULE_ROOT}/tools/object_compiler.py
               ${CAPSULE_ROOT}/tools/route_compiler.py ${CAPSULE_ROOT}/tools/location_compiler.py
               ${CAPSULE_ROOT}/tools/script_compiler.py
               ${CAPSULE_ROOT}/tools/asset_compiler.py
               ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_ROOT}/tools/sequence_compiler.py
               ${CAPSULE_TEXT_DEPENDS} ${dir}/${world} ${page}
         DEPFILE ${gen}/world.d   # tilesets, maps, sheet images and encounter files the world reads
         COMMENT "Compiling maps of ${name}")
      target_sources(${target} PRIVATE ${gen}/world.cpp ${gen}/encounter_ids.h ${gen}/world_ids.h)
   endif()

   # Battle data: a capsule.battle/1 manifest with its structure manifest and ruleset (C-08, P-13, E5s),
   # checked against this platform's engine limits (stats, record bytes), and its record transfer table
   # (kTransfer, E2c; nothing is linked from it unless the game calls monster::export_record() / import_record()).
   capsule_project_get(battle "${json}" battle)
   if(battle)
      add_custom_command(
         OUTPUT  ${gen}/battle.cpp ${gen}/battle_ids.h
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/battle_compiler.py ${dir}/${battle} ${gen}/battle.cpp
               --ids ${gen}/battle_ids.h --depfile ${gen}/battle.d --platform ${CAPSULE_PLATFORM} ${page_args}
               ${field_args}
         DEPENDS ${CAPSULE_BATTLE_COMPILER_DEPENDS} ${CAPSULE_TEXT_DEPENDS} ${dir}/${battle} ${page} ${field_depends}
         DEPFILE ${gen}/battle.d   # the structure manifest and the ruleset the manifest names
         COMMENT "Compiling battle data of ${name}")
      capsule_transfer_table(${dir}/${battle} ${gen}/transfer.cpp)
      target_sources(${target} PRIVATE ${gen}/battle.cpp ${gen}/battle_ids.h ${gen}/transfer.cpp)
      # G2: the engine save image's sizes (bag, party, boxes, script banks) against this platform's budget
      # (tools/save_layout.py; the build fails when the image is over it, or when the medium keeps fewer than two
      # copies unless project.json opts in with "save": {"single_copy": true}; C6: the GBA's SRAM is its own medium)
      set(save_args)
      set(save_platform ${CAPSULE_PLATFORM})
      if(CAPSULE_PLATFORM STREQUAL "gba" AND CAPSULE_GBA_SAVE_SRAM)
         set(save_platform gba_sram)
      endif()
      capsule_project_get(single_copy "${json}" save single_copy)
      if(single_copy)
         list(APPEND save_args --single-copy)
      endif()
      set(save_depends ${gen}/battle.cpp)   # its depfile names the battle's inputs
      if(game_scripts)
         list(APPEND save_args --scripts ${dir}/${game_scripts})
         list(APPEND save_depends ${gen}/scripts.cpp)   # likewise the scripts' sources
      endif()
      add_custom_command(
         OUTPUT  ${gen}/save_layout.txt
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/save_layout.py ${dir}/${battle} ${save_args}
               --platform ${save_platform} --out ${gen}/save_layout.txt
         DEPENDS ${CAPSULE_ROOT}/tools/save_layout.py ${save_depends} ${field_depends}
         COMMENT "Sizing the save image of ${name}")
      target_sources(${target} PRIVATE ${gen}/save_layout.txt)
   endif()

   # B1 / B2: a game whose structure has no combination methods (or no eggs and nursery), or no battle data at all,
   # links the statements' empty ports (src/script/script_combine_none.cpp, script_nursery_none.cpp) instead of the
   # engine library's defaults, which keep their state in EWRAM (the configure step reads the structure manifest
   # and runs again when it changes). C9: their definitions are weak (CAPSULE_WEAK), so a game that defines its own
   # combine or nursery port (replacing the functions, capsule/script/combine_port.h) keeps it: no duplicate symbol
   capsule_battle_parts(${dir} "${battle}" has_combine has_eggs)
   if(NOT has_combine)
      target_sources(${target} PRIVATE ${CAPSULE_ROOT}/src/script/script_combine_none.cpp)
   endif()
   if(NOT has_eggs)
      target_sources(${target} PRIVATE ${CAPSULE_ROOT}/src/script/script_nursery_none.cpp)
   endif()

   # Scripts: a capsule.scripts/1 manifest with its .cscript sources (B-1, B-2, B-4).
   # script_ids.h names the scripts, flags, variables and natives for the app's code.
   capsule_project_get(scripts "${json}" scripts)
   if(scripts)
      set(script_args)
      capsule_project_get(debug_names "${json}" debug_names)
      if(debug_names)
         list(APPEND script_args --debug-names)
      endif()
      add_custom_command(
         OUTPUT  ${gen}/scripts.cpp ${gen}/script_ids.h
         COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_compiler.py ${dir}/${scripts}
               ${gen}/scripts.cpp --ids ${gen}/script_ids.h --depfile ${gen}/scripts.d ${script_args} ${page_args}
         DEPENDS ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/route_compiler.py
               ${CAPSULE_ROOT}/tools/asset_compiler.py
               ${CAPSULE_ROOT}/tools/sequence_compiler.py ${CAPSULE_TEXT_DEPENDS} ${dir}/${scripts} ${page}
         DEPFILE ${gen}/scripts.d   # the sources the manifest names
         COMMENT "Compiling scripts of ${name}")
      target_sources(${target} PRIVATE ${gen}/scripts.cpp ${gen}/script_ids.h)
   endif()

   string(JSON n ERROR_VARIABLE err LENGTH "${json}" sequences)
   if(NOT err AND n GREATER 0)
      math(EXPR last "${n} - 1")
      foreach(i RANGE ${last})
         string(JSON symbol MEMBER "${json}" sequences ${i})
         string(JSON seq GET "${json}" sequences ${symbol})
         get_filename_component(seq_path ${dir}/${seq} ABSOLUTE)
         get_filename_component(seq_name ${seq_path} NAME_WE)
         capsule_sequence(${seq_path} ${symbol} ${gen}/seq_${seq_name}.inc ${page})
         target_sources(${target} PRIVATE ${gen}/seq_${seq_name}.inc)
      endforeach()
   endif()

   if(CAPSULE_PLATFORM STREQUAL "gba")
      target_link_options(${target} PRIVATE -Wl,-Map,$<TARGET_FILE_DIR:${target}>/${target}.map)
      # UM (D-33): the memory checks (an input script after gba_cart.ld: the link fails past EWRAM's end or below the
      # stack reserve) and the memory report after the link
      if(CAPSULE_GBA_MEMORY_CHECKS)
         target_link_options(${target} PRIVATE -Wl,--defsym=__capsule_stack_reserve=${CAPSULE_GBA_STACK_RESERVE}
                             ${CAPSULE_ROOT}/cmake/gba_memory.ld)
         set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${CAPSULE_ROOT}/cmake/gba_memory.ld)
      endif()
      add_custom_command(TARGET ${target} POST_BUILD
         COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DELF=$<TARGET_FILE:${target}>
                 -DRESERVE=${CAPSULE_GBA_STACK_RESERVE} -P ${CAPSULE_ROOT}/cmake/gba_memory_report.cmake
         VERBATIM)
      set(rom $<TARGET_FILE_DIR:${target}>/${target}.gba)
      add_custom_command(TARGET ${target} POST_BUILD
         COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${target}> ${rom}
         COMMAND ${CAPSULE_GBAFIX} ${rom} "-t${rom_title}" -c${rom_code} -m${rom_maker}
         COMMENT "Building ROM ${target}.gba"
         VERBATIM)
   elseif(CAPSULE_PLATFORM STREQUAL "dreamcast" AND CAPSULE_MKDCDISC)
      # Selfboot disc image (IP.BIN and 1ST_READ.BIN scrambling done by mkdcdisc).
      add_custom_command(TARGET ${target} POST_BUILD
         COMMAND ${CAPSULE_MKDCDISC} -e $<TARGET_FILE:${target}>
               -o $<TARGET_FILE_DIR:${target}>/${target}.cdi
         COMMENT "Building disc image ${target}.cdi"
         VERBATIM)
   endif()
endfunction()
if(CAPSULE_PLATFORM STREQUAL "gba" AND NOT CAPSULE_GBAFIX)
   message(FATAL_ERROR "gbafix not found under ${DEVKITPRO}/tools/bin. Install devkitPro's gba-dev group.")
endif()
if(CAPSULE_PLATFORM STREQUAL "dreamcast" AND NOT CAPSULE_MKDCDISC)
   message(WARNING "mkdcdisc not found: Dreamcast builds produce .elf only (no .cdi). "
               "tools/setup/setup_kallistios.sh installs it.")
endif()
if(NOT CAPSULE_PLATFORM STREQUAL "headless")
   # Helper library shared by the test and sandbox apps (on-screen console).
   add_library(capsule_test_kit STATIC apps/common/test_kit.cpp)
   target_include_directories(capsule_test_kit PUBLIC apps/common)
   target_link_libraries(capsule_test_kit PUBLIC capsule_engine)
   # The battle sandbox kit (apps/common/battle_kit.h): the Starter's sandbox and a parent repository's (the
   # reference repo's sandbox_battle) are it with their own configuration; it reads the game's kData and assets.
   add_library(capsule_battle_kit STATIC apps/common/battle_kit.cpp)
   target_include_directories(capsule_battle_kit PUBLIC apps/common)
   target_link_libraries(capsule_battle_kit PUBLIC capsule_test_kit capsule_engine)

   if(CAPSULE_ENGINE_TESTS)
      capsule_game(${CAPSULE_ROOT}/apps/hello)
      capsule_game(${CAPSULE_ROOT}/apps/input_viewer)
   endif()
   # Test apps (one per engine system) and module sandboxes (one per gameplay
   # module); see README "Test apps" and "Conventions". The battle module's
   # sandbox is apps/starter_battle (the Starter sample played); the Gen-3
   # sample's sandbox_battle lives in the reference-ruleset repo (E11).
   if(CAPSULE_ENGINE_TESTS AND CAPSULE_TEST_APPS)
      foreach(app test_video test_vram test_script test_text test_sequence sandbox_sequence sandbox_map test_battle)
         capsule_game(${CAPSULE_ROOT}/apps/${app})
      endforeach()
      # The Starter sample played (E9): samples/starter's battle data and art.
      capsule_game(${CAPSULE_ROOT}/apps/starter_battle)
      # The inventory's sandbox (G2): the Starter's party and bag, item use, shops, the save image.
      capsule_game(${CAPSULE_ROOT}/apps/sandbox_party)
   endif()
endif()

# Tests run on the build machine (or under CMAKE_CROSSCOMPILING_EMULATOR, e.g.
# QEMU for the big-endian check): desktop and headless configurations.
if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM MATCHES "^(desktop|headless)$")
   enable_testing()
   add_executable(capsule_static_checks tests/static_checks.cpp)
   target_include_directories(capsule_static_checks PRIVATE include)
   target_link_libraries(capsule_static_checks PRIVATE capsule_options)
   add_test(NAME static_checks COMMAND capsule_static_checks)
   # The default random generator (tests/random_tests.cpp, cmake/parts/rng.cmake).
   capsule_rng_test(random_default_tests)

   # Input logic against every target's map; no HAL linked.
   capsule_input_map(gba         kTestInputMapGba  ${CAPSULE_GENERATED_DIR}/test_input_map_gba.cpp)
   capsule_input_map(snes_layout kTestInputMapSnes ${CAPSULE_GENERATED_DIR}/test_input_map_snes.cpp)
   capsule_input_map(dreamcast   kTestInputMapDreamcast ${CAPSULE_GENERATED_DIR}/test_input_map_dreamcast.cpp)
   add_executable(capsule_input_tests
      tests/input_tests.cpp
      src/input/input.cpp
      ${CAPSULE_GENERATED_DIR}/test_input_map_gba.cpp
      ${CAPSULE_GENERATED_DIR}/test_input_map_snes.cpp
      ${CAPSULE_GENERATED_DIR}/test_input_map_dreamcast.cpp)
   target_include_directories(capsule_input_tests PRIVATE include)
   target_link_libraries(capsule_input_tests PRIVATE capsule_options)
   add_test(NAME input_tests COMMAND capsule_input_tests)
   add_test(NAME input_compiler_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_input_compiler.py)

   # Software GBA video model (shared by desktop, Dreamcast, headless).
   add_executable(capsule_soft_video_tests tests/soft_video_tests.cpp src/hal/soft/soft_video.cpp
      src/hal/common/vram_queue.cpp)
   target_include_directories(capsule_soft_video_tests PRIVATE include)
   target_link_libraries(capsule_soft_video_tests PRIVATE capsule_options)
   add_test(NAME soft_video_tests COMMAND capsule_soft_video_tests)

   # VRAM upload queue (A-5) and random seeding (A-4) on the software video model.
   add_executable(capsule_vram_tests
      tests/vram_tests.cpp
      ${CAPSULE_TEST_RNG_SOURCES}
      src/gfx/palette.cpp
      src/hal/common/seed.cpp
      src/hal/common/vram_queue.cpp
      src/hal/soft/soft_video.cpp)
   target_include_directories(capsule_vram_tests PRIVATE include)
   target_link_libraries(capsule_vram_tests PRIVATE capsule_options)
   add_test(NAME vram_tests COMMAND capsule_vram_tests)

   # Sequence player against the test manifests, with a mock port; no HAL linked.
   add_executable(capsule_sequence_tests
      tests/sequence_tests.cpp
      src/seq/sequence.cpp
      ${CAPSULE_TEST_SEQUENCE_INCS})
   target_include_directories(capsule_sequence_tests PRIVATE include ${CAPSULE_GENERATED_DIR})
   target_link_libraries(capsule_sequence_tests PRIVATE capsule_options)
   add_test(NAME sequence_tests COMMAND capsule_sequence_tests)
   add_test(NAME sequence_compiler_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_sequence_compiler.py)

   # Asset table and graphics modules against a compiled test manifest, drawn
   # with the software video model.
   add_custom_command(
      OUTPUT  ${CAPSULE_GENERATED_DIR}/test_assets.cpp
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/asset_compiler.py
            ${CAPSULE_ROOT}/tests/data/assets/test_assets.json ${CAPSULE_GENERATED_DIR}/test_assets.cpp
            --depfile ${CAPSULE_GENERATED_DIR}/test_assets.d
      DEPENDS ${CAPSULE_TEXT_DEPENDS}
            ${CAPSULE_ROOT}/tools/asset_compiler.py ${CAPSULE_ROOT}/tools/sequence_compiler.py
            ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_ROOT}/tests/data/assets/test_assets.json
      DEPFILE ${CAPSULE_GENERATED_DIR}/test_assets.d
      COMMENT "Compiling test assets")
   add_executable(capsule_asset_tests
      tests/asset_tests.cpp
      src/asset/asset.cpp
      src/gfx/background.cpp
      src/gfx/palette.cpp
      src/gfx/sprites.cpp
      src/text/font.cpp
      src/text/print.cpp
      src/text/text_layer.cpp
      src/hal/soft/soft_video.cpp
      src/hal/common/vram_queue.cpp
      ${CAPSULE_GENERATED_DIR}/default_font.cpp
      ${CAPSULE_GENERATED_DIR}/test_assets.cpp)
   target_include_directories(capsule_asset_tests PRIVATE include)
   target_link_libraries(capsule_asset_tests PRIVATE capsule_options)
   add_test(NAME asset_tests COMMAND capsule_asset_tests)
   add_test(NAME asset_compiler_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_asset_compiler.py)
   add_test(NAME bitmap_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_bitmap.py)
   # A6 (M4): the audio author tools (MIDI -> song, WAV -> sample, DPCM)
   add_test(NAME midi_song_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_midi_song.py)
   add_test(NAME wav_sample_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_wav_sample.py)

   # Map renderer, field movement and map events against a compiled test world
   # (with the scripts its events run).
   add_custom_command(
      OUTPUT  ${CAPSULE_GENERATED_DIR}/test_world.cpp ${CAPSULE_GENERATED_DIR}/test_encounter_ids.h
            ${CAPSULE_GENERATED_DIR}/test_world_ids.h
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/map_compiler.py
            ${CAPSULE_ROOT}/tests/data/maps/world.json ${CAPSULE_GENERATED_DIR}/test_world.cpp
            --depfile ${CAPSULE_GENERATED_DIR}/test_world.d --ids ${CAPSULE_GENERATED_DIR}/test_encounter_ids.h
            --world-ids ${CAPSULE_GENERATED_DIR}/test_world_ids.h
            --scripts ${CAPSULE_ROOT}/tests/data/maps/scripts/scripts.json --debug-names
      DEPENDS ${CAPSULE_TEXT_DEPENDS}
            ${CAPSULE_ROOT}/tools/map_compiler.py ${CAPSULE_ROOT}/tools/encounter_compiler.py
            ${CAPSULE_ROOT}/tools/event_compiler.py ${CAPSULE_ROOT}/tools/object_compiler.py
               ${CAPSULE_ROOT}/tools/route_compiler.py ${CAPSULE_ROOT}/tools/location_compiler.py
               ${CAPSULE_ROOT}/tools/script_compiler.py
            ${CAPSULE_ROOT}/tools/asset_compiler.py
            ${CAPSULE_ROOT}/tools/bitmap.py ${CAPSULE_ROOT}/tests/data/maps/world.json
      DEPFILE ${CAPSULE_GENERATED_DIR}/test_world.d
      COMMENT "Compiling test world")
   add_custom_command(
      OUTPUT  ${CAPSULE_GENERATED_DIR}/test_map_scripts.cpp ${CAPSULE_GENERATED_DIR}/test_map_script_ids.h
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_compiler.py
            ${CAPSULE_ROOT}/tests/data/maps/scripts/scripts.json ${CAPSULE_GENERATED_DIR}/test_map_scripts.cpp
            --ids ${CAPSULE_GENERATED_DIR}/test_map_script_ids.h
            --depfile ${CAPSULE_GENERATED_DIR}/test_map_scripts.d
      DEPENDS ${CAPSULE_TEXT_DEPENDS}
            ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/route_compiler.py
               ${CAPSULE_ROOT}/tools/asset_compiler.py
            ${CAPSULE_ROOT}/tools/sequence_compiler.py ${CAPSULE_ROOT}/tests/data/maps/scripts/scripts.json
      DEPFILE ${CAPSULE_GENERATED_DIR}/test_map_scripts.d
      COMMENT "Compiling test world scripts")
   add_executable(capsule_map_tests
      tests/map_tests.cpp
      src/asset/asset.cpp
      ${CAPSULE_TEST_RNG_SOURCES}
      src/encounter/encounter.cpp
      src/field/doors.cpp
      src/field/field.cpp
      src/field/field_route_default.cpp
      src/gfx/background.cpp
      src/gfx/palette.cpp
      src/gfx/sprites.cpp
      src/map/map.cpp
      src/overworld/overworld.cpp
      src/script/script.cpp
      src/script/field_actions.cpp
      src/script/script_i4_default.cpp
      ${CAPSULE_SCRIPT_ITEMS_SOURCES}
      src/seq/sequence.cpp
      src/seq/sequence_port_hal.cpp
      src/seq/sequence_game_default.cpp
      src/text/font.cpp
      src/text/print.cpp
      src/text/text_layer.cpp
      src/hal/soft/soft_video.cpp
      src/hal/common/vram_queue.cpp
      ${CAPSULE_GENERATED_DIR}/test_assets.cpp
      ${CAPSULE_GENERATED_DIR}/test_world.cpp
      ${CAPSULE_GENERATED_DIR}/default_font.cpp
      ${CAPSULE_GENERATED_DIR}/test_encounter_ids.h
      ${CAPSULE_GENERATED_DIR}/test_world_ids.h
      ${CAPSULE_GENERATED_DIR}/test_map_scripts.cpp
      ${CAPSULE_GENERATED_DIR}/test_map_script_ids.h)
   target_include_directories(capsule_map_tests PRIVATE include ${CAPSULE_GENERATED_DIR})
   target_link_libraries(capsule_map_tests PRIVATE capsule_options)
   add_test(NAME map_tests COMMAND capsule_map_tests)
   add_test(NAME map_compiler_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_map_compiler.py)
   add_test(NAME encounter_compiler_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_encounter_compiler.py)

   # Battle engine, creature records and goldens (cmake/parts/battle_tests.cmake).
   include(cmake/parts/battle_tests.cmake)
   # G2: the bag, item use out of battle and the save image (cmake/parts/inventory_tests.cmake).
   include(cmake/parts/inventory_tests.cmake)
   # G4: transformation rules, forms and training values (cmake/parts/growth_tests.cmake).
   include(cmake/parts/growth_tests.cmake)
   # G3: the Item action, recruitment and the storage boxes (cmake/parts/recruit_tests.cmake).
   include(cmake/parts/recruit_tests.cmake)
   # C6: the save image's migrations, cut writes and failing reads (cmake/parts/save_tests.cmake).
   include(cmake/parts/save_tests.cmake)
   # F1: the lead rule, the encounter hooks and pipelines, the timed field effects (cmake/parts/field_tests.cmake).
   include(cmake/parts/field_tests.cmake)
   # DS1 (D-30): doors, override sets, direction grids, the draw order, animation targets (cmake/parts/d30_tests.cmake).
   include(cmake/parts/d30_tests.cmake)
   include(cmake/parts/n2_tests.cmake)   # N2 (D-32): cell edits, layouts, ledges, the camera mover, window lines
   include(cmake/parts/c13b_tests.cmake)   # C13b: the world's cell interaction rules
   include(cmake/parts/n1_tests.cmake)   # N1 (D-32): the per-frame gameplay draw
   # C10: the R7 review's engine findings: VM options, triggers, objects, text box styles (cmake/parts/c10_tests.cmake).
   include(cmake/parts/c10_tests.cmake)
   # B1: combination methods, previews, the Combination scope (cmake/parts/combine_tests.cmake).
   include(cmake/parts/combine_tests.cmake)
   # Presenter view on its neutral data set (tests/data/battle_presenters): the shared blend while presenters fade
   # (RA1).
   add_executable(capsule_presenter_view_tests
      tests/presenter_view_tests.cpp
      src/asset/asset.cpp
      src/battle/presenter_view.cpp
      src/gfx/background.cpp
      src/gfx/palette.cpp
      src/gfx/sprites.cpp
      src/seq/sequence.cpp
      src/seq/sequence_port_hal.cpp
      src/seq/sequence_game_default.cpp
      src/text/font.cpp
      src/text/print.cpp
      src/text/text_layer.cpp
      src/hal/soft/soft_video.cpp
      src/hal/common/vram_queue.cpp
      ${CAPSULE_GENERATED_DIR}/default_font.cpp)
   target_link_libraries(capsule_presenter_view_tests PRIVATE capsule_presenter_data)
   add_test(NAME presenter_view_tests COMMAND capsule_presenter_view_tests)

   # Script VM, flags and variables against the compiled test scripts (B-1, B-2).
   add_custom_command(
      OUTPUT  ${CAPSULE_GENERATED_DIR}/test_script.cpp ${CAPSULE_GENERATED_DIR}/test_script_ids.h
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/script_compiler.py
            ${CAPSULE_ROOT}/tests/data/scripts/scripts.json ${CAPSULE_GENERATED_DIR}/test_script.cpp
            --ids ${CAPSULE_GENERATED_DIR}/test_script_ids.h --depfile ${CAPSULE_GENERATED_DIR}/test_script.d
            --debug-names
      DEPENDS ${CAPSULE_TEXT_DEPENDS}
            ${CAPSULE_ROOT}/tools/script_compiler.py ${CAPSULE_ROOT}/tools/route_compiler.py
               ${CAPSULE_ROOT}/tools/asset_compiler.py
            ${CAPSULE_ROOT}/tools/sequence_compiler.py ${CAPSULE_ROOT}/tests/data/scripts/scripts.json
            ${CAPSULE_ROOT}/tests/data/scripts/assets.json
      DEPFILE ${CAPSULE_GENERATED_DIR}/test_script.d
      COMMENT "Compiling test scripts")
   add_custom_command(
      OUTPUT  ${CAPSULE_GENERATED_DIR}/test_script_assets.cpp
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/asset_compiler.py
            ${CAPSULE_ROOT}/tests/data/scripts/assets.json ${CAPSULE_GENERATED_DIR}/test_script_assets.cpp
      DEPENDS ${CAPSULE_TEXT_DEPENDS}
            ${CAPSULE_ROOT}/tools/asset_compiler.py ${CAPSULE_ROOT}/tests/data/scripts/assets.json
      COMMENT "Compiling test script assets")
   add_executable(capsule_script_tests
      tests/script_tests.cpp
      src/script/script_menu.cpp
      # C9: the weak empty ports a game without methods or eggs links: the test's own port stubs replace them
      src/script/script_combine_none.cpp
      src/script/script_nursery_none.cpp
      src/asset/asset.cpp
      ${CAPSULE_TEST_RNG_SOURCES}
      src/script/script.cpp
      src/script/field_effects.cpp
      src/script/field_actions.cpp
      ${CAPSULE_GENERATED_DIR}/test_script.cpp
      ${CAPSULE_GENERATED_DIR}/test_script_ids.h
      ${CAPSULE_GENERATED_DIR}/test_script_assets.cpp)
   target_include_directories(capsule_script_tests PRIVATE include ${CAPSULE_GENERATED_DIR})
   target_link_libraries(capsule_script_tests PRIVATE capsule_options)
   add_test(NAME script_tests COMMAND capsule_script_tests)
   # The engine's script port with the real text layer: the box after a preview or a map plan change (RA1).
   add_executable(capsule_script_port_tests
      tests/script_port_tests.cpp
      src/asset/asset.cpp
      src/asset/asset_table_empty.cpp
      src/battle/battle_empty.cpp
      src/gfx/background.cpp
      src/gfx/palette.cpp
      src/gfx/sprites.cpp
      src/monster/monster.cpp
      src/script/script_port_hal.cpp
      src/text/font.cpp
      src/text/print.cpp
      src/text/text_layer.cpp
      src/hal/soft/soft_video.cpp
      src/hal/common/vram_queue.cpp
      ${CAPSULE_GENERATED_DIR}/default_font.cpp)
   target_include_directories(capsule_script_port_tests PRIVATE include)
   target_link_libraries(capsule_script_port_tests PRIVATE capsule_options)
   add_test(NAME script_port_tests COMMAND capsule_script_port_tests)
   # I4: proportional text windows (capsule/text/window.h) on the software video model.
   add_executable(capsule_text_window_tests
      tests/text_window_tests.cpp
      src/text/window.cpp
      src/text/print.cpp
      src/hal/soft/soft_video.cpp
      src/hal/common/vram_queue.cpp)
   target_include_directories(capsule_text_window_tests PRIVATE include)
   target_link_libraries(capsule_text_window_tests PRIVATE capsule_options)
   add_test(NAME text_window_tests COMMAND capsule_text_window_tests)
   add_test(NAME script_compiler_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_script_compiler.py)
   add_test(NAME event_compiler_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_event_compiler.py)
   add_test(NAME object_compiler_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_object_compiler.py)
   add_test(NAME location_compiler_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_location_compiler.py)
   add_test(NAME build_settings_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_build_settings.py)
   # Text (C-2, D-20): code pages and fonts; the snapshot of every generated source.
   add_test(NAME text_encoder_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_text_encoder.py)
   add_test(NAME font_compiler_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_font_compiler.py)
   add_test(NAME i4_text_tests   # I4: controls, proportional fonts, text tables, route steps
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_i4_text.py)
   add_test(NAME generated_snapshot_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_generated_snapshot.py)

   # Code style (project rules: 3-space indents, braces on their own line, names, annotations, no magic numbers).
   add_test(NAME style_check
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/ci/check_style.py
                    ${CAPSULE_ROOT}/src ${CAPSULE_ROOT}/include ${CAPSULE_ROOT}/apps
                    ${CAPSULE_ROOT}/tests ${CAPSULE_ROOT}/tools ${CAPSULE_ROOT}/cmake
                    ${CAPSULE_ROOT}/CMakeLists.txt)
   add_test(NAME style_check_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_check_style.py)

   # Code map (project instruction: track code structure and locations): CODE_MAP.md must match the code.
   add_test(NAME code_map_check
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/ci/code_map.py check --root ${CAPSULE_ROOT})
   add_test(NAME code_map_tests
            COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_code_map.py)

   # Tests that build the VRAM queue themselves use the cache overrides, if any.
   foreach(t capsule_soft_video_tests capsule_vram_tests capsule_asset_tests capsule_map_tests
             capsule_presenter_view_tests capsule_script_port_tests)
      target_compile_definitions(${t} PRIVATE ${CAPSULE_BUILD_SETTING_TEST_DEFS})
   endforeach()
endif()

# Fragments that add to the targets above (empty until their packages fill them).
include(cmake/parts/effect.cmake)   # effect VM (D-22 decision 1)
include(cmake/parts/bench.cmake)    # battle test bench (apps/battle_bench)
include(cmake/parts/ui.cmake)       # N3: the ui module (menus, lists, key repeat, scroll arrows), its tests, sandbox
include(cmake/parts/screen.cmake)   # UC: full-screen scenes (layers, sprites, tasks, fades, field return)
include(cmake/parts/audio.cmake)       # M4 B1a: the audio core, its compiler and tests
include(cmake/parts/audio_hal.cmake)   # M4 B1b: audio backends, output model, audio_render, test/sandbox apps (after audio.cmake)
capsule_transfer_tests()            # record transfer v2 tests (cmake/parts/transfer.cmake)

# A parent repository (the engine as a subproject) calls the engine's functions (capsule_game(),
# capsule_battle_test(), capsule_battle_bench(), capsule_rng(), ...) in its own directory: hand it the variables
# they read.
if(NOT PROJECT_IS_TOP_LEVEL)
   foreach(var CAPSULE_ROOT CAPSULE_PLATFORM CAPSULE_GENERATED_DIR CAPSULE_TEXT_DEPENDS
               CAPSULE_BATTLE_SOURCES CAPSULE_BATTLE_COMPILER_DEPENDS
               CAPSULE_RNG_STREAM_SOURCES CAPSULE_RNG_DEFAULT_SOURCE CAPSULE_RNG_SOURCES CAPSULE_TEST_RNG_SOURCES
               CAPSULE_RUNTIME_SOURCES CAPSULE_BUILD_SETTING_ARGS CAPSULE_BUILD_SETTING_TEST_DEFS
               CAPSULE_TRANSFER_DEPENDS CAPSULE_EFFECT_COMPILER_DEPENDS
               CAPSULE_BENCH_SOURCES CAPSULE_BENCH_TOOL CAPSULE_BENCH_HOST_FUNCTIONS
               CAPSULE_INVENTORY_SOURCES CAPSULE_SCRIPT_ITEMS_SOURCES CAPSULE_GROWTH_SOURCES
               CAPSULE_GROWTH_DEFAULT_SOURCES
               CAPSULE_AUDIO_HOOK_SOURCES CAPSULE_AUDIO_MODEL_SOURCES CAPSULE_AUDIO_HEADLESS_SOURCES)
      set(${var} "${${var}}" PARENT_SCOPE)
   endforeach()
   # P-10: the parent's games are LTO builds on the GBA too.
   if(CAPSULE_PLATFORM STREQUAL "gba")
      set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON PARENT_SCOPE)
   endif()
endif()
