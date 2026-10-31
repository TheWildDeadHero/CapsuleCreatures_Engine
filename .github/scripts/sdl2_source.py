#!/usr/bin/env python3
"""!Prepares SDL2's source for CI, so FetchContent never downloads it.

Reads the pinned version and SHA-256 from cmake/sdl2.cmake (the one place they
are set), downloads the release archive into the cache folder unless a copy
with the right hash is already there (the workflow keeps that folder in
actions/cache), unpacks it beside the archive and prints the unpacked folder,
which the workflow passes to CMake as -DFETCHCONTENT_SOURCE_DIR_SDL2.

Usage: sdl2_source.py CACHE_DIR [--engine ENGINE_ROOT]

With GITHUB_OUTPUT set, also writes `dir=<folder>` there. Standard library
only; runs on Linux, macOS and MSYS2's Python.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
import tarfile
import urllib.request
from pathlib import Path

## The pinned SDL2 version in cmake/sdl2.cmake.
VERSION_PATTERN = re.compile(r"set\(CAPSULE_SDL2_VERSION\s+([0-9.]+)\)")
## The pinned archive hash in cmake/sdl2.cmake.
SHA256_PATTERN = re.compile(r"set\(CAPSULE_SDL2_SHA256\s+([0-9a-f]{64})\)")
## Where SDL publishes its release archives (the same URL cmake/sdl2.cmake uses).
URL_TEMPLATE = "https://github.com/libsdl-org/SDL/releases/download/release-{v}/SDL2-{v}.tar.gz"
## Bytes read at a time while hashing or downloading.
CHUNK_SIZE = 1 << 20
## Seconds before a stalled download gives up.
DOWNLOAD_TIMEOUT = 120
## First Python whose tarfile takes extraction filters (the "data" filter refuses links and paths outside the folder).
TAR_FILTER_PYTHON = (3, 12)


def read_pin(engine: Path) -> tuple[str, str]:
   """!The SDL2 version and archive hash the engine pins.
   @param engine  the engine checkout's root
   @return (version, sha256)
   """
   text = (engine / "cmake" / "sdl2.cmake").read_text(encoding="utf-8")
   version = VERSION_PATTERN.search(text)
   digest = SHA256_PATTERN.search(text)
   if not version or not digest:
      sys.exit("error: cmake/sdl2.cmake does not set CAPSULE_SDL2_VERSION and CAPSULE_SDL2_SHA256")
   return version.group(1), digest.group(1)


def file_sha256(path: Path) -> str:
   """!The SHA-256 of a file, as lowercase hex.
   @param path  the file
   @return hex digest
   """
   h = hashlib.sha256()
   with path.open("rb") as f:
      for chunk in iter(lambda: f.read(CHUNK_SIZE), b""):
         h.update(chunk)
   return h.hexdigest()


def download(url: str, dest: Path) -> None:
   """!Downloads url to dest (through a temporary name, so a broken transfer never looks cached).
   @param url   the archive's address
   @param dest  where it is saved
   """
   partial = dest.with_suffix(dest.suffix + ".part")
   with urllib.request.urlopen(url, timeout=DOWNLOAD_TIMEOUT) as response, partial.open("wb") as out:
      for chunk in iter(lambda: response.read(CHUNK_SIZE), b""):
         out.write(chunk)
   partial.replace(dest)


def main(argv: list[str] | None = None) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  arguments without the program name, or None for sys.argv
   @return exit status
   """
   p = argparse.ArgumentParser(description="Prepare SDL2's pinned source for CI.")
   p.add_argument("cache_dir", help="folder kept by actions/cache")
   p.add_argument("--engine", default=".", help="the engine checkout (default: the current folder)")
   args = p.parse_args(argv)

   engine = Path(args.engine).resolve()
   cache = Path(args.cache_dir).resolve()
   cache.mkdir(parents=True, exist_ok=True)
   version, digest = read_pin(engine)
   archive = cache / f"SDL2-{version}.tar.gz"

   # A cached archive is reused only when its hash still matches the pin.
   if archive.is_file() and file_sha256(archive) == digest:
      print(f"SDL2 {version}: cached archive", file=sys.stderr)
   else:
      print(f"SDL2 {version}: downloading", file=sys.stderr)
      download(URL_TEMPLATE.format(v=version), archive)
      if file_sha256(archive) != digest:
         archive.unlink()
         sys.exit(f"error: SDL2-{version}.tar.gz does not match the SHA-256 pinned in cmake/sdl2.cmake")

   # Unpacked fresh every run (the cache holds only the archive); the archive's top folder is SDL2-<version>.
   source = cache / f"SDL2-{version}"
   if not (source / "CMakeLists.txt").is_file():
      with tarfile.open(archive, "r:gz") as tar:
         if sys.version_info >= TAR_FILTER_PYTHON:
            tar.extractall(cache, filter="data")
         else:
            tar.extractall(cache)
   # Forward slashes: CMake on Windows (MinGW) reads them as it reads native paths.
   folder = source.as_posix()
   print(folder)
   output = os.environ.get("GITHUB_OUTPUT")
   if output:
      with open(output, "a", encoding="utf-8") as f:
         f.write(f"dir={folder}\n")
   return 0


if __name__ == "__main__":
   sys.exit(main())
