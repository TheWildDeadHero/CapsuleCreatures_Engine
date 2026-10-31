# Building and targets

How to set up the toolchains and build the engine for each target. A new user
starts with the umbrella repository's
[getting-started guide](https://github.com/TheWildDeadHero/CapsuleCreatures/blob/main/docs/GETTING_STARTED.md), which walks through
these steps for the Starter sample.

## First-time setup

Clone the umbrella repository with `--recurse-submodules` (or run
`git submodule update --init --recursive` in an existing clone); nothing else
needs fetching. The engine's own `submodules/libgba` is pinned for reference
only: the build links the libgba that devkitPro installs.

GBA toolchain: devkitPro's `gba-dev` package group (devkitARM, libgba,
gbafix). The toolkit's installer sets it up together with the desktop tools
and the studio: run `./setup` (Linux, macOS) or `setup.cmd` (Windows) from the
umbrella repository (`--dry-run` first to see every step;
`toolkit/install/SUPPORT.md` lists what it does on each operating system).

It follows devkitPro's official instructions
(https://devkitpro.org/wiki/devkitPro_pacman). On Arch-based systems such as
CachyOS it adds devkitPro's own pacman repositories (not the AUR); on
Debian-based systems it runs devkitPro's official installer script. It never
upgrades the rest of your system: on Arch it syncs only devkitPro's package
databases. If pacman then can't fetch a package or satisfy a dependency, it
stops and suggests `sudo pacman -Syu` for you to run. It asks before changing
anything, backs up `/etc/pacman.conf`, can be re-run safely, and finishes by
test-compiling C++20 for the GBA's CPU. The GBA toolchain file expects the
standard `/opt/devkitpro` location; set the `DEVKITPRO` environment variable
(or `-DDEVKITPRO=...`) if yours differs. Optional: `mgba-qt` to run the ROM.

Desktop needs CMake 3.24+, Python 3.10+ and a C/C++ compiler. SDL2 is not
installed separately: the build downloads a pinned release (version and
SHA-256 in `cmake/sdl2.cmake`) on first configure and links it statically.
Offline, pass `-DFETCHCONTENT_SOURCE_DIR_SDL2=/path/to/SDL2-<version>`.

| OS | Compiler | Notes |
|---|---|---|
| Linux | GCC or Clang | SDL builds its X11/Wayland backends only if their headers are present (Arch-based systems ship them with the libraries; Debian-based need `libx11-dev`, `libwayland-dev`, `libxkbcommon-dev` and friends, see the apt lists in `.github/workflows/build.yml`) |
| macOS | Xcode Command Line Tools (Apple Clang) | universal binaries (`arm64` + `x86_64`) for macOS 10.15+ by default; override with `-DCMAKE_OSX_ARCHITECTURES=...` / `-DCMAKE_OSX_DEPLOYMENT_TARGET=...` |
| Windows | MinGW-w64 GCC via MSYS2 | from an MSYS2 **UCRT64** shell (the toolkit's `setup.cmd` installs these): `pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-python`; executables are linked statically and need no DLLs next to them |

Dreamcast toolchain: KallistiOS. Install it with

```
tools/setup/setup_kallistios.sh           # --dry-run first to see every step
```

It follows KallistiOS's own instructions: build prerequisites from your
distribution's official repositories (`pacman -S --needed` on Arch-based
systems, no AUR, no system upgrade; `apt-get` on Debian-based), then
KallistiOS and kos-ports cloned at pinned commits into `/opt/toolchains/dc`,
the SH-4 toolchain built from source with KallistiOS's `stable` profile
(GCC 15.2), KallistiOS itself, and mkdcdisc for bootable `.cdi` images.
Building the toolchain takes from about 20 minutes to over an hour. Nothing
needs sourcing afterwards: the `dc-*` presets load `environ.sh` themselves
(set `KOS_ENVIRON` if yours lives elsewhere).

## Building

In VS Code, **Terminal → Run Task** lists everything:

| Task | What it does |
|---|---|
| GBA: build ROM | CMake preset `gba-debug`: every app's ROM in `build/gba-debug/` (`capsule_hello.gba`, ...) |
| GBA: build ROM (release) | preset `gba-release`: ROMs in `build/gba-release/`, `ENGINE_ASSERT` compiled out |
| GBA: run in mGBA | builds, then opens `build/gba-debug/capsule_hello.gba` in mGBA (`mgba-qt`) |
| GBA: run input viewer in mGBA | builds `capsule_input_viewer.gba` and opens it |
| Desktop: build (debug) | CMake preset `desktop-debug` |
| Desktop: run hello world | opens the hello world in a 240×160 window (×3) |
| Desktop: run input viewer | opens the input viewer |
| GBA: clean | removes the `gba-debug` build's outputs |
| Check: all tests + principles | builds `desktop-debug`, runs all its tests (`ctest`), then the principle scan |

CMake builds every platform from one `CMakeLists.txt`. The presets also appear
in the CMake Tools extension; each OS lists only the ones it can build:

| Preset | Host | Builds |
|---|---|---|
| `desktop-debug`, `desktop-release` | Linux, macOS | desktop apps (+ tests on debug) |
| `windows-debug`, `windows-release` | Windows (MSYS2 UCRT64) | desktop apps with MinGW-w64 and Ninja (+ tests on debug) |
| `gba-debug`, `gba-release` | any host with devkitARM | GBA ROMs through `cmake/toolchains/gba-devkitarm.cmake` |
| `dc-debug`, `dc-release` | Linux, macOS with KallistiOS | Dreamcast `.elf` (dc-tool, emulators) and selfboot `.cdi`, through `cmake/toolchains/dreamcast-kos.cmake` |
| `headless` | any | engine core and tests on the software video model; no SDL, no apps |
| `big-endian-check` | Linux with `g++-s390x-linux-gnu`, `qemu-user` | headless build for big-endian s390x, tests run under QEMU (P-19) |

From a terminal:

```
cmake --preset gba-debug && cmake --build --preset gba-debug
cmake --preset desktop-debug && cmake --build --preset desktop-debug && ctest --preset desktop-debug
```

From the umbrella repository, `./build PRESET [--test]` (`build.cmd` on
Windows) runs the same three commands in `engine/`; the build folder is
`engine/build/<preset>/`.

GitHub Actions (`.github/workflows/build.yml`) builds Linux, macOS and Windows
(debug with tests, and release), the big-endian check, both GBA ROM builds in
devkitPro's devkitARM container, and both Dreamcast builds (KallistiOS
installed by the setup script and cached by its pinned commit) on every push,
and keeps the ROMs, ELFs and disc images as build artifacts. See
[Tools and CI](TOOLS_AND_CI.md#checks-and-ci) for every job.

Each app in `apps/<name>/` builds as `capsule_<name>`; on GBA that is the
ELF file `capsule_<name>` (no file extension), a header-fixed
`capsule_<name>.gba` and a linker `capsule_<name>.map` side by side; on Dreamcast an
`.elf` and, when mkdcdisc is installed, a `.cdi`. Both GBA builds use `-O2 -g`
with LTO; release only adds `NDEBUG`. Tests build and run in the desktop and
headless configurations. Release builds define `NDEBUG`, which compiles
`ENGINE_ASSERT` out. A failed assert aborts with a message on desktop and halts
with the expression, file and line on screen on GBA and Dreamcast.

The Dreamcast stretches the 240×160 frame to fill the whole 640×480 screen,
with no border. The 3:2 image is shown at 4:3, so about 11% narrower than on a
GBA. Scaling is "sharp bilinear": the CPU doubles the frame to 480×320, and the
PowerVR draws it as a full-screen textured quad with bilinear filtering for the
remaining 4/3 × 3/2. Pixels stay crisp and evenly sized, with only their edges
blended. The PowerVR's vertex coordinates are written as precomputed IEEE-754
bit patterns, so the HAL stays integer-only (P-6). It runs at the display's
refresh (60 Hz NTSC/VGA, 50 Hz PAL) rather than the GBA's 59.73 Hz.

## Building a game

A game is a folder laid out like the apps: `reference/project.json`
(`capsule.project/1`: its name, ROM header, `"sources"` and data files),
`data/` and `src/`. The toolkit's **New game** makes one from any template,
ready to build: a world (map `start`), the player's sprite in
`data/assets.json` and a first scene, `src/game.cpp` (the player walks the
start map with the D-pad; a text box names the game). The engine builds a
game outside its own tree with the cache list `CAPSULE_GAMES` (game folders;
a relative one is taken from the engine's folder), on any preset but
`headless` and `big-endian-check`, which build no games:

```
cmake --preset gba-release -B /path/to/MyGame/build/gba-release \
      -DCAPSULE_GAMES=/path/to/MyGame -DCAPSULE_ENGINE_TESTS=OFF
cmake --build /path/to/MyGame/build/gba-release
```

`CAPSULE_ENGINE_TESTS=OFF` leaves out the engine's own tests and apps, so
only the engine library and the game build. From the umbrella repository,
`./build PRESET --game FOLDER` (`build.cmd` on Windows) runs those two
commands, with the game's build folder `FOLDER/build/<preset>/`; a desktop
build there reuses the SDL2 source of the engine's own build of the preset,
if it has one. The game builds as `capsule_<name>` (and
`capsule_<name>.gba` on the GBA), like an app. A parent repository with the
engine as a subproject calls `capsule_game(<folder>)` itself instead.

## Byte order and alignment (P-19)

Data that crosses a boundary (ROM blobs, saves, link and trade records, the
emulated GBA VRAM) has a fixed little-endian layout and is read and written
only through `capsule/core/endian.h` (`load_le16/32`, `store_le16/32`), never
by casting a byte pointer to a multi-byte type. That keeps big-endian hosts
(PowerPC) on the GBA's bytes, and avoids misaligned loads, which fault on the
Dreamcast's SH-4 and silently return rotated data on the GBA's ARM7TDMI. The
`big-endian-check` preset and CI job run the tests on a big-endian target.

## Planned targets

Planned: a PowerPC Mac OS X target. SDL 1.2 still supports it but is LGPL
without a static-linking exception, so that target would link it as a dynamic
library (which satisfies the LGPL without extra obligations) or use a native
HAL; it also needs a community GCC 11+ toolchain for PowerPC Darwin.
