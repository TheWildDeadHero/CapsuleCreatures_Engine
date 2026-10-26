#!/usr/bin/env python3
"""!The audio core against its Python reference model (D-34 M4 B1a, format document section 11.2-11.3).

For every script of tests/data/audio/scripts.json: the C++ core (capsule_audio_tests --run) and the model
(tests/audio_reference.py) dump each frame's state (L1), both sides of the segment mixed (L2) and the tone register
writes (L3); the dumps must be equal line for line (the first difference is printed). Each level's hash is checked
against tests/data/audio/golden.json (regressions); the high-quality build (--hq-exe) must give the same L1-L3 and a
non-silent high-quality stream.

Usage: test_audio_reference.py --exe EXE [--hq-exe EXE] [--record] [--case NAME]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import audio_reference  # noqa: E402

DATA = HERE / "data" / "audio"
GOLDEN = DATA / "golden.json"
LEVELS = {"L1": "FPTCD", "L2": "RL", "L3": "W"}


def script_text(case: dict) -> str:
   """!A case as the C++ runner's script."""
   lines = [f"frames {case['frames']}"]
   for ev in case["events"]:
      lines.append(" ".join(str(x) for x in ev))
   return "\n".join(lines) + "\n"


def run_exe(exe: str, case: dict, tmp: Path) -> list[str]:
   """!The C++ core's dump of a case."""
   script = tmp / f"{case['name']}.txt"
   out = tmp / f"{case['name']}.out"
   script.write_text(script_text(case))
   subprocess.run([exe, "--run", str(script), str(out)], check=True)
   return out.read_text().splitlines()


def hashes(lines: list[str]) -> dict[str, str]:
   """!Each level's SHA-256 (over its lines, with the frame lines)."""
   out = {}
   for level, tags in LEVELS.items():
      h = hashlib.sha256()
      for line in lines:
         if line[:1] in tags or line.startswith("F "):
            h.update(line.encode() + b"\n")
      out[level] = h.hexdigest()
   return out


def first_difference(a: list[str], b: list[str]) -> str | None:
   """!The first differing line of two dumps."""
   frame = "?"
   for i, (x, y) in enumerate(zip(a, b)):
      if x.startswith("F "):
         frame = x[2:]
      if x != y:
         return f"frame {frame}, line {i + 1}:\n  core:  {x[:300]}\n  model: {y[:300]}"
   if len(a) != len(b):
      return f"lengths differ: core {len(a)} lines, model {len(b)}"
   return None


def main(argv: list[str]) -> int:
   ap = argparse.ArgumentParser()
   ap.add_argument("--exe", required=True)
   ap.add_argument("--hq-exe")
   ap.add_argument("--record", action="store_true")
   ap.add_argument("--case")
   args = ap.parse_args(argv)
   cases = json.loads((DATA / "scripts.json").read_text())["cases"]
   golden = json.loads(GOLDEN.read_text()) if GOLDEN.exists() else {}
   table = audio_reference.Table(DATA / "audio.json")
   failures = 0
   frames = 0
   with tempfile.TemporaryDirectory() as d:
      tmp = Path(d)
      for case in cases:
         if args.case and case["name"] != args.case:
            continue
         core = run_exe(args.exe, case, tmp)
         model = audio_reference.run_case(case, table)
         frames += case["frames"]
         diff = first_difference(core, model)
         if diff:
            print(f"{case['name']}: the core and the model differ at {diff}")
            failures += 1
         h = hashes(core)
         if args.record:
            golden[case["name"]] = h
         elif golden.get(case["name"]) != h:
            print(f"{case['name']}: golden hashes differ ({golden.get(case['name'])} vs {h})")
            failures += 1
         if args.hq_exe:
            hq = run_exe(args.hq_exe, case, tmp)
            plain = [x for x in hq if not x.startswith("H ")]
            if plain != core:
               print(f"{case['name']}: the high-quality build changes L1-L3: {first_difference(plain, core)}")
               failures += 1
            sums = [int(x[2:]) for x in hq if x.startswith("H ")]
            if len(sums) != case["frames"] or (any(x.startswith("R ") and x.strip("R 0") for x in core) and
                                               not any(sums)):
               print(f"{case['name']}: the high-quality stream is missing or silent")
               failures += 1
   if args.record:
      GOLDEN.write_text(json.dumps(golden, indent=1, sort_keys=True) + "\n")
   print(f"{len(cases) if not args.case else 1} case(s), {frames} frames: {failures} failure(s)")
   return 1 if failures else 0


if __name__ == "__main__":
   sys.exit(main(sys.argv[1:]))
