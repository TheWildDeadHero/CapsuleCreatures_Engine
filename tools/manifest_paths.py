#!/usr/bin/env python3
"""!Paths as manifests write them, judged the same on Linux, Windows and macOS (D-40 RW3).

Manifests store relative paths with "/" between parts, whatever system wrote them, and a manifest written on one
system is compiled on the others. pathlib's own is_absolute() answers only for the running system: on Windows it
calls "/x" relative (it has no drive), on Linux it calls "C:/x" relative (a drive means nothing there). So a tool that
refuses absolute manifest paths would refuse a different set on each system. is_absolute_text() counts a path
absolute when any of them would: "/x" (POSIX; on Windows the current drive's root), "C:..." (a drive) and
"//server/share" (UNC, also written with backslashes). It is the rule of the toolkit's
editor/capsule_studio/core/paths.py, so the studio and the build refuse the same paths.

relative_to_folder() gives the path from a folder to a file the tools hold as an absolute path (an included
manifest's images, I4), written with "/" whatever the system's separator is.
"""
from __future__ import annotations

import os
import re
from os import PathLike
from pathlib import Path, PurePosixPath

## A Windows drive at the start of a path text: one letter and a colon ("C:", "c:/x", "C:x"; backslashes too).
DRIVE = re.compile(r"^[A-Za-z]:")
## How a UNC path (//server/share, or the same with backslashes) starts once each backslash is read as "/".
UNC_START = "//"


class PathError(ValueError):
   """!A path that cannot be written as a manifest path (a file on another drive than the manifest, Windows)."""


def is_absolute_text(text: str) -> bool:
   """!Whether a path text, as a manifest gives it, is absolute on any system the tools run on.
   @param text  the path as written ("/" or a backslash between parts)
   @return True for a POSIX absolute path, a Windows drive path (also drive-relative "C:x") or a UNC path
   """
   unified = text.replace("\\", "/")
   return PurePosixPath(unified).is_absolute() or bool(DRIVE.match(unified)) or unified.startswith(UNC_START)


def relative_to_folder(target: str | PathLike[str], folder: str | PathLike[str]) -> str:
   """!The path from a folder to a file, written with "/" (".." where it climbs).
   The file is a path the tools resolved on this system, so the running system's rules compare the two (drive
   letters and case on Windows); the folder is resolved first, as the file was.
   @param target  the file (absolute)
   @param folder  the folder
   @return the relative path, e.g. "images/x.png" or "../c/x.png"
   @throws PathError when the file is on another drive than the folder (Windows: no relative path exists)
   """
   real_folder = Path(folder).resolve()
   try:
      return Path(os.path.relpath(target, real_folder)).as_posix()
   except ValueError:   # Windows: another drive
      raise PathError(f"{target} is on another drive than {real_folder.as_posix()}: manifest paths are relative, so "
                      "keep the file on the manifest's drive") from None
