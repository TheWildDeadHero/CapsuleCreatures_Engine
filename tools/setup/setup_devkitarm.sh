#!/usr/bin/env bash
# Installs devkitARM + libgba + gbafix (devkitPro's gba-dev group) for GBA builds.
#
# Follows devkitPro's official instructions: https://devkitpro.org/wiki/devkitPro_pacman
#   - Arch-based (Arch, CachyOS, EndeavourOS, Manjaro, ...): adds devkitPro's own
#     pacman repositories (not the AUR) and installs gba-dev with the system pacman.
#     It never upgrades the rest of the system: only devkitPro's repositories are
#     synced (your other package databases are left as they are). If pacman then
#     can't fetch a package or satisfy a dependency, the script stops and suggests
#     a full system upgrade (sudo pacman -Syu) for you to run yourself.
#   - Debian-based (Debian, Ubuntu, Mint, ...): runs devkitPro's official
#     install-devkitpro-pacman script, then installs gba-dev with dkp-pacman.
#
# Run as your normal user; it calls sudo only for the steps that need it.
# Safe to re-run: steps already done are skipped.
#
# Usage: tools/setup/setup_devkitarm.sh [--dry-run] [--yes]
#   --dry-run   print what would run, change nothing
#   --yes       don't ask for confirmation (pacman's own prompts are answered too)
set -euo pipefail

readonly DKP_KEY="BC26F752D25B92CE272E0F44F7FD5492264BB9D0"
readonly DKP_KEYSERVER="keyserver.ubuntu.com"
readonly DKP_KEYRING_URL="https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst"
readonly DKP_DEBIAN_SCRIPT_URL="https://apt.devkitpro.org/install-devkitpro-pacman"
readonly DEVKITPRO_DIR="${DEVKITPRO_DIR:-/opt/devkitpro}"   # overridable for testing
readonly PACMAN_CONF="${PACMAN_CONF:-/etc/pacman.conf}"          # overridable for testing
readonly OS_RELEASE_FILE="${OS_RELEASE_FILE:-/etc/os-release}"  # overridable for testing

DRY_RUN=0
ASSUME_YES=0
for arg in "$@"; do
   case "$arg" in
      --dry-run) DRY_RUN=1 ;;
      --yes) ASSUME_YES=1 ;;
      -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
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

# glibc systems use dkp-linux; musl systems use dkp-linux-musl (per devkitPro).
linux_repo() {
   # shellcheck disable=SC2016  # $arch is for pacman to expand, not the shell
   if ldd --version 2>&1 | grep -qi musl; then
      printf '[dkp-linux-musl]\nServer = https://pkg.devkitpro.org/packages/linux-musl/$arch/\n'
   else
      printf '[dkp-linux]\nServer = https://pkg.devkitpro.org/packages/linux/$arch/\n'
   fi
}

setup_arch() {
   command -v pacman >/dev/null || die "pacman not found"
   echo "    This will: import devkitPro's signing key, install devkitpro-keyring,"
   echo "    add devkitPro's [dkp-libs]/[dkp-linux] repos to $PACMAN_CONF (backed up first),"
   echo "    sync only devkitPro's package databases, and run 'sudo pacman -S --needed gba-dev'."
   echo "    Nothing else on your system is upgraded."
   confirm "Continue?" || die "cancelled, nothing was changed"

   say "devkitPro package signing key"
   if sudo pacman-key --list-keys "$DKP_KEY" >/dev/null 2>&1; then
      skip "key $DKP_KEY present"
   else
      run sudo pacman-key --recv "$DKP_KEY" --keyserver "$DKP_KEYSERVER"
      run sudo pacman-key --lsign "$DKP_KEY"
   fi

   say "devkitPro keyring package"
   if pacman -Q devkitpro-keyring >/dev/null 2>&1; then
      skip "devkitpro-keyring installed"
   else
      run sudo pacman -U --noconfirm "$DKP_KEYRING_URL"
      # devkitPro's documented fix if the keyring hook could not run.
      run sudo pacman-key --populate devkitpro
   fi

   say "devkitPro repositories in $PACMAN_CONF"
   if grep -q '^\[dkp-libs\]' "$PACMAN_CONF"; then
      skip "[dkp-libs] present"
   else
      local backup
      backup="$PACMAN_CONF.bak-$(date +%Y%m%d-%H%M%S)"
      local block
      block="$(printf '\n# devkitPro (added by setup_devkitarm.sh, see https://devkitpro.org/wiki/devkitPro_pacman)\n[dkp-libs]\nServer = https://pkg.devkitpro.org/packages\n\n%s' "$(linux_repo)")"
      echo "    backing up to $backup and appending:"
      printf '%s\n' "$block" | sed 's/^/      /'
      run sudo cp "$PACMAN_CONF" "$backup"
      if [ "$DRY_RUN" -eq 0 ]; then
         printf '%s\n' "$block" | sudo tee -a "$PACMAN_CONF" >/dev/null
      fi
   fi

   say "Sync devkitPro's package databases only"
   # A temporary config with your [options] and only the devkitPro repos, so
   # 'pacman -Sy' refreshes those databases and leaves every other one (and so
   # every other package) exactly as it is: no system upgrade, no partial one.
   local dkp_conf="/tmp/setup_devkitarm-pacman.conf"
   [ "$DRY_RUN" -eq 1 ] || dkp_conf="$(mktemp)"
   if [ "$DRY_RUN" -eq 0 ]; then
      {
         awk '/^\[/ { in_options = ($0 == "[options]") } in_options' "$PACMAN_CONF"
         printf '\n[dkp-libs]\nServer = https://pkg.devkitpro.org/packages\n\n'
         linux_repo
      } > "$dkp_conf"
      chmod 644 "$dkp_conf"
   fi
   if ! run sudo pacman --config "$dkp_conf" -Sy; then
      rm -f "$dkp_conf"
      pacman_failed "could not sync devkitPro's package databases"
   fi
   [ "$DRY_RUN" -eq 1 ] || rm -f "$dkp_conf"

   say "Install gba-dev"
   local flags=(-S --needed)
   [ "$ASSUME_YES" -eq 1 ] && flags+=(--noconfirm)
   run sudo pacman "${flags[@]}" gba-dev || pacman_failed "could not install gba-dev"
}

# Stops with advice. A full system upgrade is only suggested here, after pacman
# has actually failed, never run by this script.
pacman_failed() {
   cat >&2 <<MSG
error: $1.

If the output above shows downloads failing (404 / "failed retrieving file"),
or a dependency that cannot be satisfied, your package databases are probably
out of date relative to the mirrors, or gba-dev needs newer system packages.
The usual fix on Arch-based systems is a full system upgrade, which you run
yourself:

    sudo pacman -Syu

then re-run this script. If the error mentions keys or signatures instead, run
'sudo pacman-key --populate devkitpro' and re-run.
MSG
   exit 1
}

setup_debian() {
   say "devkitPro pacman (dkp-pacman)"
   if command -v dkp-pacman >/dev/null; then
      skip "dkp-pacman installed"
   else
      command -v wget >/dev/null || die "wget is needed: sudo apt install wget"
      local script="/tmp/install-devkitpro-pacman"
      [ "$DRY_RUN" -eq 1 ] || script="$(mktemp)"
      run wget -O "$script" "$DKP_DEBIAN_SCRIPT_URL"
      run chmod +x "$script"
      echo "    devkitPro asks you to inspect this script before running it: $script"
      confirm "Run devkitPro's install-devkitpro-pacman with sudo?" || die "cancelled"
      run sudo "$script"
      run rm -f "$script"
   fi

   say "Install gba-dev"
   local flags=(-S --needed)
   [ "$ASSUME_YES" -eq 1 ] && flags+=(--noconfirm)
   run sudo dkp-pacman "${flags[@]}" gba-dev
}

verify() {
   say "Verify"
   local cxx="$DEVKITPRO_DIR/devkitARM/bin/arm-none-eabi-g++"
   local ok=1
   if [ -x "$cxx" ]; then
      echo "    $("$cxx" --version | head -n1)"
      local tmp
      tmp="$(mktemp -d)"
      printf '#include <concepts>\nconsteval int f() { return 1; }\nint main() { return f() - 1; }\n' > "$tmp/t.cpp"
      if "$cxx" -std=c++20 -mthumb -mcpu=arm7tdmi -c "$tmp/t.cpp" -o "$tmp/t.o" 2>/dev/null; then
         echo "    C++20 for arm7tdmi: ok"
      else
         echo "    C++20 test compile FAILED"; ok=0
      fi
      rm -rf "$tmp"
   else
      echo "    missing: $cxx"; ok=0
   fi
   for f in "$DEVKITPRO_DIR/libgba/lib/libgba.a" "$DEVKITPRO_DIR/tools/bin/gbafix"; do
      if [ -e "$f" ]; then echo "    found: $f"; else echo "    missing: $f"; ok=0; fi
   done
   if [ -r /etc/profile.d/devkit-env.sh ]; then
      echo "    DEVKITPRO/DEVKITARM are set by /etc/profile.d/devkit-env.sh (new logins)"
   fi
   echo "    The engine's GBA toolchain file (cmake/toolchains/gba-devkitarm.cmake) defaults to $DEVKITPRO_DIR,"
   echo "    so no environment setup is required."
   [ "$ok" -eq 1 ] || die "devkitARM is not fully installed"
   say "devkitARM is ready"
}

main() {
   [ "$(id -u)" -ne 0 ] || die "run as your normal user; the script uses sudo where needed"
   local family
   family="$(detect_family)"
   say "Detected: ${family}-based system"
   case "$family" in
      arch) setup_arch ;;
      debian) setup_debian ;;
      *) die "unsupported distribution; follow https://devkitpro.org/wiki/devkitPro_pacman" ;;
   esac
   if [ "$DRY_RUN" -eq 1 ]; then
      say "Dry run finished; nothing was changed"
   else
      verify
   fi
}

main
