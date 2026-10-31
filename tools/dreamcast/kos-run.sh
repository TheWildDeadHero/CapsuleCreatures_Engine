#!/usr/bin/env bash
# Runs a command inside the KallistiOS environment: sources environ.sh, then
# executes the arguments. CMake uses it as the compiler and linker launcher for
# Dreamcast builds, so builds work from any shell, VS Code task or CMake GUI
# without sourcing environ.sh first.
#
#   KOS_ENVIRON  path to environ.sh (default: /opt/toolchains/dc/kos/environ.sh)
set -e
environ="${KOS_ENVIRON:-${KOS_BASE:-/opt/toolchains/dc/kos}/environ.sh}"
if [ ! -r "$environ" ]; then
   echo "kos-run: KallistiOS environment not found at $environ" >&2
   echo "kos-run: run tools/setup/setup_kallistios.sh, or set KOS_ENVIRON" >&2
   exit 1
fi
# shellcheck disable=SC1090
. "$environ" >/dev/null
exec "$@"
