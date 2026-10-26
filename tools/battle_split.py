#!/usr/bin/env python3
"""!Splits a single-file battle manifest (the form before E5s) into the content
manifest and the structure manifest (D-23 section 2; tools/battlec/manifest.py).

    battle_split.py <battle.json>... [--structure <name>] [--check]

Each battle.json keeps its content (types, moves, species, passives, items,
traits, parties, presenters, messages, effects, texts, cues, editor data) and
names the new structure manifest under "structure" (right after "ruleset");
the structure keys (battlec.manifest.STRUCTURE_KEYS: monster_layout, stats,
stages, rules, chart, categories, the condition records, move_flags, tags,
growth, formats, turn, learn, exp_policy, generation, terms) move, in their
order, to that file (capsule.structure/1) next to it: structure.json for a
battle.json, STEM.structure.json for any other name (--structure chooses
the name, with one manifest only). Values are moved unchanged, so the
compiled data is the same.

Files are written as the studio writes them (JSON, one-space indents, UTF-8,
a final newline), all or nothing (write_all(): temporary files first, then
renamed over their targets). Nothing is written when a manifest is not
capsule.battle/1, already names a structure manifest, or its structure file
exists already. --check only reports what would move.

Exit status: 0 when every manifest was split (or would be), 1 when one was
refused, 2 for a command line that is not understood.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from battlec.common import Json, ManifestError  # noqa: E402
from battlec.manifest import STRUCTURE_KEYS, split_manifest, structure_file_for  # noqa: E402

## Indent of the written JSON (as the studio's documents are saved).
JSON_INDENT = 1
## Exit status: every manifest split.
EXIT_OK = 0
## Exit status: a manifest was refused.
EXIT_REFUSED = 1
## Exit status: a command line that is not understood (argparse's own).
EXIT_USAGE = 2
## Suffix of the temporary file write_all() writes beside each target (a hidden name: a leading dot).
TEMP_SUFFIX = ".migrating"


def write_all(files: Sequence[tuple[Path, str]]) -> None:
   """!Writes several text files all or nothing: each to a temporary file beside its target first; when every one
   is written, each is renamed over its target (os.replace(), atomic per file on one file system). A failure while
   the temporary files are written removes them and leaves every target as it was.
   @param files  (target path, text) pairs, renamed in this order
   @return None
   @throws ManifestError when a file cannot be written or renamed
   """
   temps: list[tuple[Path, Path]] = []
   try:
      for path, text in files:
         tmp = path.with_name(f".{path.name}{TEMP_SUFFIX}")
         temps.append((tmp, path))
         tmp.write_text(text, encoding="utf-8")
   except OSError as e:
      for tmp, _path in temps:
         tmp.unlink(missing_ok=True)
      raise ManifestError(f"{temps[-1][1]}: cannot write ({e.strerror}); nothing written") from None
   done: list[Path] = []
   for tmp, path in temps:
      try:
         os.replace(tmp, path)
      except OSError as e:
         for left, _path in temps[len(done):]:
            left.unlink(missing_ok=True)
         raise ManifestError(f"{path}: cannot write ({e.strerror}); written before it: "
                             f"{', '.join(str(d) for d in done) or 'nothing'}") from None
      done.append(path)


def dump(data: Json) -> str:
   """!JSON text as the data files are written.
   @param data  a manifest
   @return the text, with a final newline
   """
   return json.dumps(data, indent=JSON_INDENT, ensure_ascii=False) + "\n"


def split_file(path: str | Path, structure_name: str | None = None,
               write: bool = True) -> tuple[Json, Json, Path]:
   """!Splits one single-file manifest on disk.
   @param path            the battle.json to split (rewritten in place)
   @param structure_name  the structure manifest's file name (relative to it), or None for structure_file_for()
   @param write           false: only compute the result
   @return (content manifest, structure manifest, structure path)
   @throws ManifestError when the file cannot be read, is refused, or its structure file exists
   """
   path = Path(path)
   try:
      merged = json.loads(path.read_text(encoding="utf-8"))
   except OSError as e:
      raise ManifestError(f"{path}: cannot read ({e.strerror})") from None
   except json.JSONDecodeError as e:
      raise ManifestError(f"{path}: invalid JSON ({e})") from None
   name = structure_name or structure_file_for(path.name)
   try:
      battle, structure = split_manifest(merged, name)
   except ManifestError as e:
      raise ManifestError(f"{path}: {e}") from None
   st_path = path.parent / name
   if st_path.exists():
      raise ManifestError(f"{path}: {st_path} exists already; nothing written")
   if write:
      # both or neither (the structure first: a failed rename of the content leaves the old manifest intact)
      write_all([(st_path, dump(structure)), (path, dump(battle))])
   return battle, structure, st_path


def main(argv: Sequence[str]) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return process exit status
   """
   parser = argparse.ArgumentParser(prog="battle_split.py", description=__doc__.splitlines()[0])
   parser.add_argument("manifests", nargs="+", type=Path, help="single-file battle manifests to split")
   parser.add_argument("--structure", help="the structure manifest's file name (one manifest only)")
   parser.add_argument("--check", action="store_true", help="report only, write nothing")
   args = parser.parse_args(argv[1:])
   if args.structure is not None and len(args.manifests) != 1:
      parser.error("--structure names the structure of one manifest only")
   status = EXIT_OK
   for path in args.manifests:
      try:
         _battle, structure, st_path = split_file(path, args.structure, write=not args.check)
      except ManifestError as e:
         print(f"error: {e}", file=sys.stderr)
         status = EXIT_REFUSED
         continue
      moved = [k for k in structure if k in STRUCTURE_KEYS]
      verb = "would move" if args.check else "moved"
      print(f"{path}: {verb} {len(moved)} structure key(s) to {st_path.name} ({', '.join(moved) or 'none'})")
   return status


if __name__ == "__main__":
   sys.exit(main(sys.argv))
