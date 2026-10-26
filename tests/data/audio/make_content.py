#!/usr/bin/env python3
"""!Writes the audio tests' samples (tests/data/audio/samples/*.bin): simple waveforms made by code (D-34 M4, tier 0).

Each file is the sample's bytes plus a known guard byte (or the DPCM blocks past the end) so the tests see the
mixer's reads past a sample's end. DPCM samples are encoded here with a greedy one-step encoder (the first index
wins ties, the level wraps in 8 bits) - test content only, independent of the importer's encoder.

Usage: make_content.py [--check]  (--check: exit 1 when a committed file differs)
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DELTAS = [i * i if i < 8 else -(16 - i) * (16 - i) for i in range(16)]


def s8(v: int) -> int:
   """!A signed byte's two's complement.
   @param v  -128..127
   @return 0..255
   """
   return v & 0xFF


def pcm(values: list[int], guard: int) -> bytes:
   """!PCM8 bytes plus a guard byte.
   @param values  samples -128..127
   @param guard   the byte after them
   @return the bytes
   """
   return bytes(s8(v) for v in values) + bytes([s8(guard)])


def dpcm(values: list[int], blocks: int) -> bytes:
   """!DPCM blocks (33 bytes per 64 samples), greedy deltas.
   @param values  samples (padded with the last one to whole blocks)
   @param blocks  blocks to write
   @return the bytes
   """
   vals = values + [values[-1]] * (blocks * 64 - len(values))
   out = bytearray()
   for b in range(blocks):
      blk = vals[b * 64:(b + 1) * 64]
      level = s8(blk[0])
      out.append(level)
      nibbles = []
      for target in blk[1:]:
         best = min(range(16), key=lambda i: (abs(((level + DELTAS[i]) & 0xFF) - s8(target)), i))
         level = (level + DELTAS[best]) & 0xFF
         nibbles.append(best)
      out.append(nibbles[0])
      for i in range(1, 63, 2):
         out.append((nibbles[i] << 4) | nibbles[i + 1])
   return bytes(out)


def isin(i: int, period: int, amp: int) -> int:
   """!An integer sine (Bhaskara's approximation on whole degrees: no floating point).
   @param i       sample index
   @param period  samples per period
   @param amp     amplitude
   @return the sample
   """
   deg = (i * 360 // period) % 360
   x = deg % 180
   v = amp * 4 * x * (180 - x) // (40500 - x * (180 - x))
   return v if deg < 180 else -v


def noise(n: int, seed: int) -> list[int]:
   """!Noise from a 32-bit xorshift generator.
   @param n     samples
   @param seed  start
   @return samples -100..99
   """
   out = []
   x = seed
   for _ in range(n):
      x ^= (x << 13) & 0xFFFFFFFF
      x ^= x >> 17
      x ^= (x << 5) & 0xFFFFFFFF
      out.append((x >> 16) % 200 - 100)
   return out


def content() -> dict[str, bytes]:
   """!Every sample file.
   @return name -> bytes
   """
   sine = [isin(i, 128, 100) for i in range(128)]
   square = [80 if (i // 16) % 2 == 0 else -80 for i in range(1003)]
   saw = [(i * 4) % 256 - 128 for i in range(1101)]
   short = [10, 20, 30, 40, 50, 60, 70]
   tri = [abs((i % 100) - 50) * 4 - 100 for i in range(300)]
   dsine = [isin(i, 80, 110) for i in range(640 + 64)]
   walk = []
   v = 0
   for r in noise(1027 + 64, 7):
      v = max(-120, min(120, v + r // 8))
      walk.append(v)
   return {
      "sine_loop.bin": pcm(sine, 0x55),
      "square_once.bin": pcm(square, 0x40),
      "saw_loop.bin": pcm(saw, 0x7F),
      "noise_once.bin": pcm(noise(1002, 3), -1),
      "short.bin": pcm(short, 0x22),
      "fixed.bin": pcm(tri, -5),
      "dpcm_sine_loop.bin": dpcm(dsine, 640 // 64 + 1),
      "dpcm_walk_once.bin": dpcm(walk, 1027 // 64 + 1),
   }


def main(argv: list[str]) -> int:
   """!Command line.
   @param argv  arguments
   @return exit status
   """
   ap = argparse.ArgumentParser()
   ap.add_argument("--check", action="store_true")
   args = ap.parse_args(argv)
   bad = 0
   for name, data in content().items():
      path = HERE / "samples" / name
      if args.check:
         if not path.exists() or path.read_bytes() != data:
            print(f"{path}: differs from make_content.py", file=sys.stderr)
            bad = 1
      else:
         path.parent.mkdir(exist_ok=True)
         path.write_bytes(data)
   return bad


if __name__ == "__main__":
   sys.exit(main(sys.argv[1:]))
