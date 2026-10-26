# GBA toolchain: devkitARM + libgba, C++20 (P-8, P-10). Used by the gba-*
# presets; selects the GBA platform in CMakeLists.txt.
#
# Paths default to devkitPro's standard install. Override with the DEVKITPRO
# environment variable or -DDEVKITPRO=/path (DEVKITARM and LIBGBA likewise).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CAPSULE_PLATFORM gba)

foreach(var DEVKITPRO DEVKITARM LIBGBA)
   if(NOT DEFINED ${var} AND DEFINED ENV{${var}})
      set(${var} "$ENV{${var}}")
   endif()
endforeach()
if(NOT DEFINED DEVKITPRO)
   set(DEVKITPRO /opt/devkitpro)
endif()
if(NOT DEFINED DEVKITARM)
   set(DEVKITARM ${DEVKITPRO}/devkitARM)
endif()
if(NOT DEFINED LIBGBA)
   set(LIBGBA ${DEVKITPRO}/libgba)
endif()
# Try-compile projects re-read this file; pass the resolved paths along.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES DEVKITPRO DEVKITARM LIBGBA)

set(_prefix ${DEVKITARM}/bin/arm-none-eabi-)
if(NOT EXISTS ${_prefix}g++)
   message(FATAL_ERROR "devkitARM not found at ${DEVKITARM}. Install devkitPro's gba-dev group "
                  "(tools/setup/setup_devkitarm.sh) or set DEVKITPRO.")
endif()
if(NOT EXISTS ${LIBGBA}/lib/libgba.a)
   message(FATAL_ERROR "libgba not found at ${LIBGBA}. Install devkitPro's gba-dev group or set LIBGBA.")
endif()

set(CMAKE_C_COMPILER   ${_prefix}gcc)
set(CMAKE_CXX_COMPILER ${_prefix}g++)
set(CMAKE_ASM_COMPILER ${_prefix}gcc)
set(CMAKE_OBJCOPY      ${_prefix}objcopy CACHE FILEPATH "objcopy for the GBA target")
find_program(CAPSULE_GBAFIX gbafix PATHS ${DEVKITPRO}/tools/bin NO_DEFAULT_PATH)

# Bare metal: no OS to run test executables, so CMake's compiler checks build a
# static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXECUTABLE_SUFFIX .elf)

set(CMAKE_FIND_ROOT_PATH ${DEVKITARM} ${LIBGBA})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Thumb code for ROM; ARM for hot IWRAM routines is chosen per function later.
set(_arch "-mthumb -mthumb-interwork -mcpu=arm7tdmi -mtune=arm7tdmi")
set(CMAKE_C_FLAGS_INIT   "${_arch}")
set(CMAKE_CXX_FLAGS_INIT "${_arch} -fno-threadsafe-statics")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_arch} -specs=gba.specs")
# Both builds are optimized (cartridge ROM and CPU are small); release only
# compiles ENGINE_ASSERT out. Set as plain variables so CMake's own defaults
# (-O0 debug, -O3 release) are not appended.
foreach(lang C CXX)
   set(CMAKE_${lang}_FLAGS_DEBUG   "-O2 -g")
   set(CMAKE_${lang}_FLAGS_RELEASE "-O2 -g -DNDEBUG")
endforeach()
