# Dreamcast toolchain: KallistiOS (SH-4, newlib, C++20). Used by the dc-*
# presets; selects the Dreamcast platform in CMakeLists.txt.
#
# Wraps KallistiOS's own toolchain file. The KOS environment (environ.sh) is
# loaded here for configuration and by tools/dreamcast/kos-run.sh for every
# compile and link, so nothing has to be sourced by hand.
#
#   KOS_ENVIRON  path to environ.sh (default: /opt/toolchains/dc/kos/environ.sh,
#                as installed by tools/setup/setup_kallistios.sh)

set(CAPSULE_PLATFORM dreamcast)

if(NOT DEFINED KOS_ENVIRON AND DEFINED ENV{KOS_ENVIRON})
   set(KOS_ENVIRON "$ENV{KOS_ENVIRON}")
endif()
if(NOT DEFINED KOS_ENVIRON)
   set(KOS_ENVIRON /opt/toolchains/dc/kos/environ.sh)
endif()
if(NOT EXISTS ${KOS_ENVIRON})
   message(FATAL_ERROR "KallistiOS not found (no ${KOS_ENVIRON}). Run tools/setup/setup_kallistios.sh "
                  "or set KOS_ENVIRON to your environ.sh.")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES KOS_ENVIRON)

# Import the KOS_* / DC_* variables and PATH that environ.sh exports, so
# KallistiOS's toolchain file and CMake's compiler checks see them.
execute_process(
   COMMAND bash -c ". \"$1\" >/dev/null && for v in \${!KOS_*} \${!DC_*} PATH; do printf '%s=%s\\n' \"$v\" \"\${!v}\"; done"
         kos-env ${KOS_ENVIRON}
   OUTPUT_VARIABLE _kos_env
   RESULT_VARIABLE _kos_env_result)
if(NOT _kos_env_result EQUAL 0)
   message(FATAL_ERROR "Sourcing ${KOS_ENVIRON} failed")
endif()
string(REPLACE "\n" ";" _kos_env "${_kos_env}")
foreach(_line IN LISTS _kos_env)
   if(_line MATCHES "^([A-Za-z0-9_]+)=(.*)$")
      set(ENV{${CMAKE_MATCH_1}} "${CMAKE_MATCH_2}")
   endif()
endforeach()

include($ENV{KOS_BASE}/utils/cmake/kallistios.toolchain.cmake)

# Every compile and link runs inside the KOS environment. Run through bash so
# the script works even where its executable bit was lost (zip downloads etc.).
get_filename_component(_capsule_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
foreach(lang C CXX)
   set(CMAKE_${lang}_COMPILER_LAUNCHER bash ${_capsule_root}/tools/dreamcast/kos-run.sh)
   set(CMAKE_${lang}_LINKER_LAUNCHER   bash ${_capsule_root}/tools/dreamcast/kos-run.sh)
endforeach()

set(CMAKE_EXECUTABLE_SUFFIX .elf)
find_program(CAPSULE_MKDCDISC mkdcdisc HINTS $ENV{DC_TOOLS_BASE})
