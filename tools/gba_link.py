#!/usr/bin/env python3
"""!The GBA link's launcher (D-42 §4.6; docs/USAGE.md): capsule_game() runs every GBA game's link through it.

    gba_link.py --nm NM [--demangler C++FILT] [--project project.json] [--stack-reserve BYTES] [--game NAME]
                [--out REPORT.json] [--check] [--memory-script gba_memory.ld] -- LINK COMMAND...

It runs the link command unchanged. After a link that succeeds it lists the program's symbols and prints its memory
by owner (tools/usage_memory.py), writing the report to --out; with --check, a passed budget fails the build and the
program is removed, so the next build links it again. When the link fails because the program does not fit (the
memory checks' assertions, a region that overflows), it links a second time beside the program with the checks left
out and the linker told to write its output anyway, prints that program's memory by owner, naming the largest
contributors, and exits with the first link's status. The program itself is never written by that second link.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

## The tools' folder.
TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import build_settings  # noqa: E402
import usage  # noqa: E402
import usage_memory  # noqa: E402

## What the linker prints when a program does not fit: the memory checks' assertions (cmake/gba_memory.ld), a region
## that overflows, a section past its region, and a section placed past its region's end (gba_cart.ld's IWRAM
## sections: "address 0x... of <program> section `.data' is not within region `iwram'", which GNU ld may print
## without the others, stopping before the assertions are checked).
NO_FIT = ("GBA memory:", "overflowed by", "will not fit in region", "is not within region")
## The linker option that writes the output despite errors.
KEEP_OUTPUT = "-Wl,--noinhibit-exec"
## A linker map option's path, as one argument (`-Wl,-Map,path` or `-Wl,-Map=path`).
MAP_OPTION = re.compile(r"(-Map[,=])([^,]+)")
## The output option.
OUTPUT_OPTION = "-o"
## The suffix of the second link's files (beside the program).
MEASURE_SUFFIX = ".does_not_fit"
## Exit status for a bad command line.
EXIT_USAGE = 2


def run(command: Sequence[str]) -> tuple[int, str]:
   """!Runs a command, its output and errors together.
   @param command  the command
   @return (exit status, output)
   """
   try:
      done = subprocess.run(list(command), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            errors="replace")
   except OSError as e:
      return 1, f"gba_link.py: cannot run {command[0]}: {e}\n"
   return done.returncode, done.stdout


def output_of(command: Sequence[str]) -> Path | None:
   """!The program a link command writes.
   @param command  the link command
   @return the path after -o, or None
   """
   args = list(command)
   for i, arg in enumerate(args[:-1]):
      if arg == OUTPUT_OPTION:
         return Path(args[i + 1])
   return None


def measure_command(command: Sequence[str], memory_script: Path | None, side: Path) -> list[str]:
   """!The second link's command: the checks left out, the output kept despite errors, every file beside the program.
   @param command        the link command
   @param memory_script  the checks' linker script (cmake/gba_memory.ld), or None
   @param side           the second link's program
   @return the command
   """
   out: list[str] = []
   skip_next = False
   for i, arg in enumerate(command):
      if skip_next:
         skip_next = False
         continue
      if arg == OUTPUT_OPTION and i + 1 < len(command):
         out += [OUTPUT_OPTION, str(side)]
         skip_next = True
         continue
      if memory_script is not None and Path(arg).name == memory_script.name:
         continue
      out.append(MAP_OPTION.sub(lambda m: m.group(1) + str(side.with_name(side.name + ".map")), arg))
   return out + [KEEP_OUTPUT]


def listing(nm: str, program: Path) -> str | None:
   """!A program's sized symbols (`nm -S --defined-only`).
   @param nm       the nm program
   @param program  the linked program
   @return the listing, or None when nm fails
   """
   try:
      done = subprocess.run([nm, "-S", "--defined-only", str(program)], capture_output=True, text=True,
                            errors="replace")
   except OSError:
      return None
   return done.stdout if done.returncode == 0 else None


def memory(args: argparse.Namespace, program: Path, out: Path | None) -> dict | None:
   """!A linked program's memory by owner, printed.
   @param args     the launcher's options
   @param program  the program
   @param out      where to write the report, or None
   @return usage_memory.report()'s object, or None when the program cannot be listed
   """
   text = listing(args.nm, program)
   if text is None:
      print(f"gba_link.py: {args.nm} could not list {program}; no memory report", file=sys.stderr)
      return None
   budgets = None
   if args.project is not None:
      try:
         budgets = build_settings.load_memory_budgets(args.project)
      except build_settings.SettingsError as e:
         print(f"{args.project}: build settings: {e}", file=sys.stderr)
   result = usage_memory.report(text, budgets, args.stack_reserve, args.game, args.demangler)
   if out is not None:
      out.parent.mkdir(parents=True, exist_ok=True)
      usage.write_if_changed(out, usage.as_json(result))
   sys.stdout.write(usage_memory.report_text(result))
   return result


def main(argv: Sequence[str]) -> int:
   """!Command line; see the module docstring.
   @param argv  the arguments (without the program)
   @return the link's exit status, or 1 for a passed budget
   """
   argv = list(argv)
   if "--" not in argv:
      print("gba_link.py: give the link command after --", file=sys.stderr)
      return EXIT_USAGE
   split = argv.index("--")
   command = argv[split + 1:]
   ap = argparse.ArgumentParser(prog="gba_link.py", description="Runs a GBA link and reports its memory by owner.")
   ap.add_argument("--nm", required=True)
   ap.add_argument("--demangler")
   ap.add_argument("--project", type=Path)
   ap.add_argument("--stack-reserve", type=int, default=usage_memory.DEFAULT_STACK_RESERVE)
   ap.add_argument("--game", default="")
   ap.add_argument("--out", type=Path)
   ap.add_argument("--check", action="store_true")
   ap.add_argument("--memory-script", type=Path)
   try:
      args = ap.parse_args(argv[:split])
   except SystemExit as e:
      return int(e.code) if isinstance(e.code, int) else EXIT_USAGE
   if not command:
      print("gba_link.py: the link command is empty", file=sys.stderr)
      return EXIT_USAGE

   status, text = run(command)
   sys.stdout.write(text)
   program = output_of(command)
   if program is None:
      return status
   if status == 0:
      result = memory(args, program, args.out)
      if args.check and result is not None and result["over"]:
         # A passed budget fails the build; without the program the next build links (and checks) again.
         program.unlink(missing_ok=True)
         print(f"gba_link.py: {program.name} is over its memory budget (above); removed", file=sys.stderr)
         return 1
      return 0
   if not any(mark in text for mark in NO_FIT):
      return status

   # The program does not fit: link it once more beside it, without the checks, and say what holds the memory.
   side = program.with_name(program.stem + MEASURE_SUFFIX + program.suffix)
   side.unlink(missing_ok=True)
   _, measured = run(measure_command(command, args.memory_script, side))
   if not side.exists():
      print("gba_link.py: the measuring link wrote no program; no memory report", file=sys.stderr)
      sys.stdout.write(measured)
      return status
   print(f"The program does not fit. Measured by a second link without the checks ({side.name}, never a ROM):")
   memory(args, side, None)
   return status


if __name__ == "__main__":
   sys.exit(main(sys.argv[1:]))
