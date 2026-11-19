#!/usr/bin/env python3
"""!The exclusion manifest (D-36 §5, package 3; docs/USAGE.md "Exclusion manifest"): the engine modules and script
ops a game does not build, generated from its usage report and its own object files, and checked against them.

A game names its manifest in `capsule.project/1` (`"exclude": "reference/exclude.json"`):

    {"schema": "capsule.exclude/1", "game": "starter_battle", "generated_from": "usage/starter_battle.json",
     "modules": [{"name": "seq", "reason": "..."}], "script_ops": [{"name": "Nursery", "reason": "..."}]}

`generate()` lists what can be excluded: an excludable module (EXCLUDABLE_MODULES) that no usage record names and
that none of the game's object files references (their undefined symbols, by mangled namespace, as the link
comparison reads them: this finds a game's own C++ that calls an engine module, which no compiler sees), and a
generic script op the game's scripts do not emit. `check()` compares a manifest with the report and the objects:

- error: an excluded item the game uses, a module that cannot be excluded, an item listed twice;
- warning: an item this engine does not have (a stale exclusion);
- suggestion: an item nothing uses that the manifest does not exclude.

Run by tools/usage.py (`exclude`, `exclude-check`) and by the script compiler (`--exclude`: an excluded op a script
emits is an error at that statement). A parent build that sets CAPSULE_EXCLUDE to the manifest compiles the excluded
modules out of the engine library (step 3B, cmake/exclude.cmake).
"""
from __future__ import annotations

import json
import re
import subprocess
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import usage

## The manifest's schema.
SCHEMA = "capsule.exclude/1"
## The manifest's keys.
KEYS = frozenset({"schema", "game", "generated_from", "about", "modules", "script_ops"})
## The item lists of a manifest.
ITEM_LISTS = ("modules", "script_ops")
## An item's keys.
ITEM_KEYS = frozenset({"name", "reason"})
## The engine modules a game can exclude (package 3: the modules the link comparison found linked but unused). Step 3B
## leaves them out of the library (cmake/exclude.cmake CAPSULE_EXCLUDABLE_MODULES, the same list) and cuts the
## always-compiled code that reaches them (capsule/exclude_config.h).
EXCLUDABLE_MODULES = ("combine", "growth", "monster", "seq", "storage")
## The script VM's manifest (its generic ops).
SCRIPT_VM = usage.ENGINE_SOURCES / "script" / "script_vm.json"
## The op kind the script VM declares for its own ops.
GENERIC_KIND = "generic"
## The usage category of the opcodes the scripts emit.
SCRIPT_OP_CATEGORY = "script.op"
## An excludable module's reason.
MODULE_REASON = "no compiler records it; the game's code does not call it"
## A script op's reason.
OP_REASON = "no script emits it"
## The nm kind of an undefined symbol (the references that pull an archive member into a link).
UNDEFINED = "U"
## A line of `nm -A -P -u`: the file (an object, or `archive[member]`), the symbol and its kind.
NM_LINE = re.compile(r"^(?P<file>.+?):\s+(?P<name>\S+)\s+(?P<kind>\S)\b")
## Object files per nm run (keeps the command line short).
NM_BATCH = 200
## The folder CMake writes a target's objects under; an object is shown from after it.
OBJECT_FOLDER = ".dir/"
## The manifest file's JSON indent.
JSON_INDENT = 1


class ExcludeError(Exception):
   """!A manifest that does not read, or an nm that does not run."""


@dataclass
class Findings:
   """!What check() found."""

   ## Excluded items the game uses, items that cannot be excluded, duplicates.
   errors: list[str] = field(default_factory=list)
   ## Items this engine does not have.
   warnings: list[str] = field(default_factory=list)
   ## Items nothing uses that the manifest does not exclude.
   suggestions: list[str] = field(default_factory=list)


def script_ops(path: Path = SCRIPT_VM) -> list[str]:
   """!The script VM's generic ops, in id order.
   @param path  script_vm.json
   @return their names
   @throws ExcludeError when it does not read
   """
   try:
      vm = json.loads(path.read_text(encoding="utf-8"))
   except (OSError, json.JSONDecodeError) as e:
      raise ExcludeError(f"{path}: {e}") from None
   ops = sorted((o for o in vm.get("ops", []) if o.get("kind") == GENERIC_KIND), key=lambda o: o["id"])
   return [o["name"] for o in ops]


def object_label(path: str) -> str:
   """!An object file as findings name it: its path below the target's CMake folder.
   @param path  the object (or `archive[member]`)
   @return the label
   """
   posix = path.replace("\\", "/")
   at = posix.find(OBJECT_FOLDER)
   return posix[at + len(OBJECT_FOLDER):] if at >= 0 else posix


def parse_references(text: str, modules: Iterable[str]) -> dict[str, dict[str, str]]:
   """!The engine modules an `nm -A -P -u` listing's objects reference.
   @param text     the listing
   @param modules  the engine's modules
   @return module -> object label -> the first symbol it references there
   """
   known = list(modules)
   refs: dict[str, dict[str, str]] = {}
   for line in text.splitlines():
      m = NM_LINE.match(line.strip())
      if m is None or m["kind"] != UNDEFINED:
         continue
      module = usage.symbol_module(m["name"], known)
      if module is not None:
         refs.setdefault(module, {}).setdefault(object_label(m["file"]), m["name"])
   return refs


def run_nm(nm: str, files: Sequence[str]) -> str | None:
   """!One nm run.
   @param nm     the nm
   @param files  the files it lists
   @return its listing, or None when it cannot read one of them
   @throws ExcludeError when nm does not start
   """
   try:
      done = subprocess.run([nm, "-A", "-P", "-u", *files], capture_output=True, text=True, check=False)
   except OSError as e:
      raise ExcludeError(f"{nm}: {e}") from None
   return done.stdout if done.returncode == 0 else None


def read_references(objects: Sequence[Path], nm: str, modules: Iterable[str],
                    unreadable: list[str] | None = None) -> dict[str, dict[str, str]]:
   """!Runs nm over the game's object files (or archives) and reads the engine modules they reference.
   @param objects     the files
   @param nm          the nm to run (a gcc-nm for link-time optimized objects)
   @param modules     the engine's modules
   @param unreadable  receives the files nm cannot read (such as an LLVM bitcode object in a macOS universal file,
                      which Xcode's nm refuses): their references are not seen, which the check warns about
   @return parse_references()'s result for the files it read
   @throws ExcludeError when nm does not start
   """
   text = []
   files = [str(p) for p in objects]
   for start in range(0, len(files), NM_BATCH):
      batch = files[start:start + NM_BATCH]
      listing = run_nm(nm, batch)
      if listing is not None:
         text.append(listing)
         continue
      # one file it cannot read fails the run: list the batch's files one at a time
      for one in batch:
         listing = run_nm(nm, [one])
         if listing is not None:
            text.append(listing)
         elif unreadable is not None:
            unreadable.append(object_label(one))
   return parse_references("".join(text), modules)


def unreadable_warning(unreadable: Sequence[str]) -> str:
   """!The warning about object files nm could not read.
   @param unreadable  read_references()'s list
   @return it
   """
   return (f"nm could not read {len(unreadable)} object file(s) (first: {unreadable[0]}): the engine modules they "
           f"reference are not seen, so an excluded module they use is not found and their modules may be suggested")


def used_ops(report: Mapping[str, Any]) -> dict[str, int] | None:
   """!The script ops a report's scripts emit.
   @param report  the usage report
   @return op -> times emitted, or None when the game compiles no scripts
   """
   counts = report.get("used", {}).get(SCRIPT_OP_CATEGORY)
   return None if counts is None else {k: v for k, v in counts.items() if v > 0}


def generate(report: Mapping[str, Any], refs: Mapping[str, Mapping[str, str]], ops: Sequence[str],
             generated_from: str = "") -> dict[str, Any]:
   """!The manifest of everything the game can exclude.
   @param report          its usage report
   @param refs            parse_references() of its objects
   @param ops             the script VM's generic ops
   @param generated_from  the report's path, as the manifest names it
   @return the manifest
   """
   reported = report.get("modules", {})
   modules = [{"name": m, "reason": MODULE_REASON} for m in EXCLUDABLE_MODULES
              if m in reported and not reported[m].get("used") and m not in refs]
   emitted = used_ops(report)
   excluded_ops = [] if emitted is None else [{"name": o, "reason": OP_REASON} for o in ops if o not in emitted]
   return {"schema": SCHEMA, "game": report.get("game", ""), "generated_from": generated_from,
           "modules": modules, "script_ops": excluded_ops}


def validate(manifest: Any, where: str = "") -> dict[str, Any]:
   """!Checks a manifest's shape.
   @param manifest  the parsed file
   @param where     its path, for the messages
   @return it
   @throws ExcludeError when it is not a capsule.exclude/1 manifest
   """
   prefix = f"{where}: " if where else ""
   if not isinstance(manifest, dict) or manifest.get("schema") != SCHEMA:
      raise ExcludeError(f"{prefix}schema must be {SCHEMA}")
   unknown = sorted(set(manifest) - KEYS)
   if unknown:
      raise ExcludeError(f"{prefix}unknown key(s) {', '.join(unknown)}")
   for key in ITEM_LISTS:
      items = manifest.get(key, [])
      if not isinstance(items, list):
         raise ExcludeError(f"{prefix}{key} must be a list")
      for item in items:
         if not isinstance(item, dict) or not isinstance(item.get("name"), str) or set(item) - ITEM_KEYS:
            raise ExcludeError(f"{prefix}{key}: each item is {{\"name\": ..., \"reason\": ...}}")
   return manifest


def load(path: Path) -> dict[str, Any]:
   """!Reads a manifest.
   @param path  the file
   @return it
   @throws ExcludeError when it does not read or is not one
   """
   try:
      raw = json.loads(Path(path).read_text(encoding="utf-8"))
   except (OSError, json.JSONDecodeError) as e:
      raise ExcludeError(f"{path}: {e}") from None
   return validate(raw, str(path))


def names(manifest: Mapping[str, Any], key: str) -> list[str]:
   """!The names a manifest's list holds.
   @param manifest  the manifest
   @param key       "modules" or "script_ops"
   @return them, in order
   """
   return [item["name"] for item in manifest.get(key, [])]


def duplicates(items: Sequence[str]) -> list[str]:
   """!Names listed more than once.
   @param items  the names
   @return the repeated ones, sorted
   """
   return sorted({n for n in items if items.count(n) > 1})


def check(manifest: Mapping[str, Any], report: Mapping[str, Any], refs: Mapping[str, Mapping[str, str]],
          ops: Sequence[str], unreadable: Sequence[str] = ()) -> Findings:
   """!A manifest against a game's usage report and object files.
   @param manifest    the manifest
   @param report      the usage report
   @param refs        parse_references() of the game's objects
   @param ops         the script VM's generic ops
   @param unreadable  the objects nm could not read (a warning)
   @return the findings
   """
   found = Findings()
   if unreadable:
      found.warnings.append(unreadable_warning(unreadable))
   reported = report.get("modules", {})
   modules = names(manifest, "modules")
   excluded_ops = names(manifest, "script_ops")
   for key, items in (("modules", modules), ("script_ops", excluded_ops)):
      found.errors += [f"{key}: {n} is listed more than once" for n in duplicates(items)]

   for module in dict.fromkeys(modules):
      if module not in reported:
         found.warnings.append(f"module {module}: this engine has no such module (a stale exclusion)")
      elif module not in EXCLUDABLE_MODULES:
         found.errors.append(f"module {module}: cannot be excluded (excludable: {', '.join(EXCLUDABLE_MODULES)})")
      else:
         if reported[module].get("used"):
            found.errors.append(f"module {module} is excluded but used: {', '.join(reported[module]['by'])} "
                                f"(the usage report)")
         for obj, symbol in sorted(refs.get(module, {}).items()):
            found.errors.append(f"module {module} is excluded but {obj} references it ({symbol})")
   for module in EXCLUDABLE_MODULES:
      if module in reported and not reported[module].get("used") and module not in refs and module not in modules:
         found.suggestions.append(f"module {module}: nothing uses it; it could be excluded")

   emitted = used_ops(report) or {}
   for op in dict.fromkeys(excluded_ops):
      if op not in ops:
         found.warnings.append(f"script op {op}: the script VM has no such op (a stale exclusion)")
      elif op in emitted:
         found.errors.append(f"script op {op} is excluded but the scripts emit it {emitted[op]} times")
   if used_ops(report) is not None:
      found.suggestions += [f"script op {op}: no script emits it; it could be excluded" for op in ops
                            if op not in emitted and op not in excluded_ops]
   return found


def findings_text(found: Findings) -> str:
   """!The findings as the build prints them.
   @param found  check()'s result
   @return the lines, with a summary line last
   """
   lines = [f"error: {e}" for e in found.errors] + [f"warning: {w}" for w in found.warnings]
   lines += [f"note: {s}" for s in found.suggestions]
   lines.append(f"exclude_check: {len(found.errors)} errors, {len(found.warnings)} warnings, "
                f"{len(found.suggestions)} suggestions")
   return "\n".join(lines) + "\n"


def manifest_text(manifest: Mapping[str, Any]) -> str:
   """!A manifest as written.
   @param manifest  the manifest
   @return its JSON text
   """
   return json.dumps(manifest, indent=JSON_INDENT, ensure_ascii=False) + "\n"
