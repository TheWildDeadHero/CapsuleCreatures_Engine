#!/usr/bin/env bash
# Installs KallistiOS (KOS) for Dreamcast builds: the SH-4 toolchain (GCC from
# KOS's "stable" kos-chain profile), KallistiOS itself, kos-ports (source only)
# and mkdcdisc (selfboot .cdi images), under /opt/toolchains/dc as KOS expects.
#
# Follows KallistiOS's own instructions (utils/kos-chain/README.md and
# doc/debian.md in the KOS tree). KOS and kos-ports are pinned to the commits
# below so every machine builds the same thing; bump them deliberately.
#   - Arch-based (Arch, CachyOS, EndeavourOS, Manjaro, ...): build prerequisites
#     from the official repositories with 'pacman -S --needed'. No AUR, and no
#     system upgrade: if pacman can't fetch or resolve a package, the script
#     stops and suggests 'sudo pacman -Syu' for you to run yourself.
#   - Debian-based (Debian, Ubuntu, Mint, ...): prerequisites with apt-get.
#
# Building the toolchain compiles binutils, GCC and newlib from source: expect
# roughly 20 minutes to well over an hour depending on the machine.
#
# Run as your normal user; it calls sudo only to install packages and to create
# /opt/toolchains/dc (which it then hands to you). Safe to re-run: steps
# already done are skipped.
#
# Usage: tools/setup/setup_kallistios.sh [--dry-run] [--yes] [--no-disc-tool]
#   --dry-run       print what would run, change nothing
#   --yes           don't ask for confirmation (package managers too)
#   --no-disc-tool  skip mkdcdisc (builds then produce .elf only, no .cdi)
set -euo pipefail

readonly KOS_REPO="https://github.com/KallistiOS/KallistiOS.git"
readonly KOS_COMMIT="568c3404c292a941bcf550465bd968f4fa249aa2"        # 2026-10-11
readonly PORTS_REPO="https://github.com/KallistiOS/kos-ports.git"
readonly PORTS_COMMIT="f4faacc42faaf552625777b7709e871a827e1055"      # 2026-10
readonly MKDCDISC_REPO="https://gitlab.com/simulant/mkdcdisc.git"     # MIT; main branch
readonly DC_ROOT="${DC_ROOT:-/opt/toolchains/dc}"                     # KOS's fixed layout; override for testing only
readonly OS_RELEASE_FILE="${OS_RELEASE_FILE:-/etc/os-release}"        # overridable for testing

readonly ARCH_PACKAGES=(base-devel git subversion texinfo libjpeg-turbo libpng libelf python meson ninja wget)
readonly DEBIAN_PACKAGES=(build-essential git subversion texinfo libjpeg-dev libpng-dev libelf-dev python3 meson ninja-build wget)

DRY_RUN=0
ASSUME_YES=0
DISC_TOOL=1
for arg in "$@"; do
   case "$arg" in
      --dry-run) DRY_RUN=1 ;;
      --yes) ASSUME_YES=1 ;;
      --no-disc-tool) DISC_TOOL=0 ;;
      -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
      *) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
   esac
done

say()  { printf '\033[1m==> %s\033[0m\n' "$*"; }
skip() { printf '    %s (already done)\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# Runs a command, or prints it in --dry-run mode.
run() {
   if [ "$DRY_RUN" -eq 1 ]; then
      printf '    [dry-run] %s\n' "$*"
   else
      "$@"
   fi
}

confirm() {
   [ "$ASSUME_YES" -eq 1 ] && return 0
   [ "$DRY_RUN" -eq 1 ] && return 0
   read -r -p "$1 [y/N] " reply
   [[ "$reply" =~ ^[Yy]$ ]]
}

detect_family() {
   local id="" like=""
   if [ -r "$OS_RELEASE_FILE" ]; then
      # shellcheck disable=SC1090
      . "$OS_RELEASE_FILE"
      id="${ID:-}"; like="${ID_LIKE:-}"
   fi
   case " $id $like " in
      *" arch "*) echo arch ;;
      *" debian "*|*" ubuntu "*) echo debian ;;
      *) echo unknown ;;
   esac
}

pacman_failed() {
   cat >&2 <<MSG
error: $1.

If the output above shows downloads failing (404 / "failed retrieving file"),
or a dependency that cannot be satisfied, your package databases are probably
out of date relative to the mirrors. The usual fix on Arch-based systems is a
full system upgrade, which you run yourself:

    sudo pacman -Syu

then re-run this script.
MSG
   exit 1
}

install_prerequisites() {
   local family="$1"
   say "Build prerequisites"
   case "$family" in
      arch)
         local flags=(-S --needed)
         [ "$ASSUME_YES" -eq 1 ] && flags+=(--noconfirm)
         run sudo pacman "${flags[@]}" "${ARCH_PACKAGES[@]}" || pacman_failed "could not install the prerequisites"
         ;;
      debian)
         local flags=(-y --no-install-recommends)
         run sudo apt-get update
         run sudo apt-get install "${flags[@]}" "${DEBIAN_PACKAGES[@]}"
         ;;
   esac
}

# Clones (or reuses) a repository and checks out a pinned commit.
checkout() {
   local repo="$1" dir="$2" commit="$3"
   if [ -d "$dir/.git" ]; then
      local have
      have="$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)"
      if [ "$have" = "$commit" ]; then skip "$dir at ${commit:0:12}"; return; fi
      echo "    $dir is at ${have:0:12}, pinned commit is ${commit:0:12}"
      confirm "Check out the pinned commit in $dir?" || die "cancelled; $dir left as it is"
      run git -C "$dir" fetch --quiet origin "$commit"
   else
      run git clone --quiet "$repo" "$dir"
   fi
   run git -C "$dir" -c advice.detachedHead=false checkout --quiet "$commit"
}

build_toolchain() {
   say "SH-4 toolchain (kos-chain, stable profile)"
   if [ -x "$DC_ROOT/sh-elf/bin/sh-elf-g++" ]; then
      skip "$DC_ROOT/sh-elf/bin/sh-elf-g++"
      return
   fi
   echo "    Building binutils, GCC and newlib from source; this takes a while."
   run make -C "$DC_ROOT/kos/utils/kos-chain" build platform=dreamcast toolchain_profile=stable
}

configure_environ() {
   say "KallistiOS environment (environ.sh)"
   if [ -f "$DC_ROOT/kos/environ.sh" ]; then
      skip "$DC_ROOT/kos/environ.sh"
   else
      run cp "$DC_ROOT/kos/doc/environ.sh.sample" "$DC_ROOT/kos/environ.sh"
   fi
}

build_kos() {
   say "KallistiOS"
   if [ -f "$DC_ROOT/kos/lib/dreamcast/libkallisti.a" ]; then
      skip "libkallisti.a"
      return
   fi
   run bash -c ". '$DC_ROOT/kos/environ.sh' && make -C '$DC_ROOT/kos'"
}

build_mkdcdisc() {
   [ "$DISC_TOOL" -eq 1 ] || { say "mkdcdisc skipped (--no-disc-tool)"; return; }
   say "mkdcdisc (selfboot .cdi images)"
   if [ -x "$DC_ROOT/bin/mkdcdisc" ]; then skip "$DC_ROOT/bin/mkdcdisc"; return; fi
   local src="$DC_ROOT/src/mkdcdisc"
   run mkdir -p "$DC_ROOT/src" "$DC_ROOT/bin"
   [ -d "$src/.git" ] || run git clone --quiet "$MKDCDISC_REPO" "$src"
   # Best effort: without mkdcdisc, Dreamcast builds still produce .elf files.
   if run bash -c "cd '$src' && meson setup builddir --buildtype=release >/dev/null && ninja -C builddir"; then
      run cp "$src/builddir/mkdcdisc" "$DC_ROOT/bin/mkdcdisc"
   else
      echo "    warning: mkdcdisc did not build; Dreamcast builds will produce .elf only." >&2
      echo "    See $src/README.md, or re-run with --no-disc-tool to silence this." >&2
   fi
}

verify() {
   say "Verify"
   local ok=1
   local engine_root
   engine_root="$(cd "$(dirname "$0")/../.." && pwd)"
   local tmp
   tmp="$(mktemp -d)"
   printf '#include <concepts>\nconsteval int f() { return 1; }\nint main() { return f() - 1; }\n' > "$tmp/t.cpp"
   if KOS_ENVIRON="$DC_ROOT/kos/environ.sh" bash "$engine_root/tools/dreamcast/kos-run.sh" \
          kos-c++ -std=c++20 -c "$tmp/t.cpp" -o "$tmp/t.o" 2>/dev/null; then
      echo "    C++20 for SH-4 through kos-c++: ok"
   else
      echo "    C++20 test compile FAILED"; ok=0
   fi
   rm -rf "$tmp"
   for f in "$DC_ROOT/kos/environ.sh" "$DC_ROOT/kos/lib/dreamcast/libkallisti.a"; do
      if [ -e "$f" ]; then echo "    found: $f"; else echo "    missing: $f"; ok=0; fi
   done
   if [ -x "$DC_ROOT/bin/mkdcdisc" ]; then
      echo "    found: $DC_ROOT/bin/mkdcdisc"
   else
      echo "    not installed: mkdcdisc (Dreamcast builds produce .elf only)"
   fi
   echo "    The dc-* presets find KallistiOS at $DC_ROOT/kos/environ.sh; nothing needs sourcing."
   [ "$ok" -eq 1 ] || die "KallistiOS is not fully installed"
   say "KallistiOS is ready"
}

main() {
   [ "$(id -u)" -ne 0 ] || die "run as your normal user; the script uses sudo where needed"
   local family
   family="$(detect_family)"
   say "Detected: ${family}-based system"
   case "$family" in
      arch|debian) ;;
      *) die "unsupported distribution; follow utils/kos-chain/README.md in the KallistiOS tree" ;;
   esac
   echo "    This will: install build prerequisites with your package manager,"
   echo "    create $DC_ROOT (owned by you), clone KallistiOS and kos-ports at pinned"
   echo "    commits, build the SH-4 toolchain and KallistiOS there, and build mkdcdisc."
   echo "    Nothing else on your system is upgraded."
   confirm "Continue?" || die "cancelled, nothing was changed"

   install_prerequisites "$family"

   say "Install directory $DC_ROOT"
   if [ -d "$DC_ROOT" ] && [ -w "$DC_ROOT" ]; then
      skip "$DC_ROOT exists and is writable"
   else
      run sudo mkdir -p "$DC_ROOT"
      run sudo chown "$(id -u):$(id -g)" "$DC_ROOT"
   fi

   say "KallistiOS sources"
   checkout "$KOS_REPO" "$DC_ROOT/kos" "$KOS_COMMIT"
   checkout "$PORTS_REPO" "$DC_ROOT/kos-ports" "$PORTS_COMMIT"

   build_toolchain
   configure_environ
   build_kos
   build_mkdcdisc

   if [ "$DRY_RUN" -eq 1 ]; then
      say "Dry run finished; nothing was changed"
   else
      verify
   fi
}

main
