# SDL2 for the desktop HAL, pinned and built with the project as a static
# library, so Linux, macOS and Windows all use the same version and no system
# install is needed. Provides SDL2::SDL2-static.
#
# Offline or local copy: configure with
#   -DFETCHCONTENT_SOURCE_DIR_SDL2=/path/to/SDL2-<version>
# to use an unpacked source tree instead of downloading.

include(FetchContent)

# 2.32.10 (2025-09-01): builds against PipeWire 1.4+ headers with GCC 14+, where 2.30.x stops at an
# incompatible-pointer error in its PipeWire audio backend.
set(CAPSULE_SDL2_VERSION 2.32.10)
set(CAPSULE_SDL2_SHA256  5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165)

# Static library only; no tests, no install rules, no SDL2main (the HAL
# defines SDL_MAIN_HANDLED so apps keep a plain main() on every OS).
set(SDL_SHARED           OFF CACHE BOOL "" FORCE)
set(SDL_STATIC           ON  CACHE BOOL "" FORCE)
set(SDL_TEST             OFF CACHE BOOL "" FORCE)
set(SDL_TESTS            OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY     OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON  CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL_STATIC_PIC       ON  CACHE BOOL "" FORCE)

FetchContent_Declare(SDL2
   URL      https://github.com/libsdl-org/SDL/releases/download/release-${CAPSULE_SDL2_VERSION}/SDL2-${CAPSULE_SDL2_VERSION}.tar.gz
   URL_HASH SHA256=${CAPSULE_SDL2_SHA256}
   DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(SDL2)

if(NOT TARGET SDL2::SDL2-static)
   message(FATAL_ERROR "SDL2 ${CAPSULE_SDL2_VERSION} did not provide SDL2::SDL2-static")
endif()
