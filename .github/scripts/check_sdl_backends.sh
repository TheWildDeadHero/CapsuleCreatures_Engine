#!/usr/bin/env bash
# Fails unless the SDL2 that a Linux build configured has every audio and video backend a shipped Linux executable
# needs. SDL turns each backend on only when its headers are present at configure time; a missing package does not
# fail the build, it silently leaves the backend out (D-40: a container build had X11 only, so no sound at all).
# SDL loads the backends' libraries at run time, so the executable still starts where one is absent.
# $1: the CMake build folder (build/desktop-release, ...)
set -euo pipefail

build=${1:?usage: check_sdl_backends.sh BUILD_DIR}
# SDL writes one SDL_config.h per configuration under its build folder (include-config-<config>/SDL2/).
config=$(find "$build/_deps" -path '*include-config-*' -name SDL_config.h -print -quit 2>/dev/null || true)
if [ -z "$config" ]; then
   echo "::error::no SDL_config.h under $build/_deps; is this a desktop build?"
   exit 1
fi

required=(
   SDL_AUDIO_DRIVER_PIPEWIRE
   SDL_AUDIO_DRIVER_PULSEAUDIO
   SDL_AUDIO_DRIVER_ALSA
   SDL_VIDEO_DRIVER_WAYLAND
   SDL_VIDEO_DRIVER_X11
)
status=0
for name in "${required[@]}"; do
   if grep -Eq "^#define $name 1" "$config"; then
      echo "$name: on"
   else
      echo "::error::$name is off in $config (install its -dev package before configuring)"
      status=1
   fi
done
exit "$status"
