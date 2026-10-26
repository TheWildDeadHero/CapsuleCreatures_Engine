#!/usr/bin/env python3
"""!DPCM4 sample coding (audio format §2.1, §8.2): the engine's compressed sample format, encoder and decoder.

Format: blocks of 64 samples in 33 bytes. Byte 0 of a block is sample 0 as a signed byte; byte 1's low nibble is
the delta to sample 1 (its high nibble is unused); every later byte holds two deltas, the high nibble first. A
delta nibble indexes DELTAS; the decoder adds it to the previous sample in 8 bits (wrapping).

Encoder (the reference behaviour the format follows):

- input samples are levels -128..127 (quantize(): clamp(floor(x * 128), -128, 127) of a value in [-1, 1));
- each block starts a new chain from its first sample;
- each delta is chosen by a depth-first search over the next `lookahead` samples of the same block (1-8; the block
  is filled with silence past the last input sample): the candidate whose squared error at this sample plus the
  best squared error of the rest of the window is smallest wins, the lowest index winning ties; a candidate whose
  level leaves -128..127 is never chosen (the deeper levels follow the same rule); a candidate whose own squared
  error is already not below the best total found so far is skipped;
- `pad` True encodes whole blocks (the final block's missing samples encode silence); `pad` False stops after the
  last input sample: the final block keeps only the bytes of the samples it has, and a byte whose low nibble would
  hold the first sample past the end is not written (its high nibble's sample is dropped from the data).

Lookahead 1 (the common case) uses a table of every (previous level, sample) pair built from the same search.
"""
from __future__ import annotations

import math

## The delta of each nibble value.
DELTAS = (0, 1, 4, 9, 16, 25, 36, 49, -64, -49, -36, -25, -16, -9, -4, -1)
## Samples per block.
BLOCK_SAMPLES = 64
## Bytes per whole block (the first sample, then 32 bytes of deltas).
BLOCK_BYTES = 33
## Lowest and highest level.
LEVEL_MIN, LEVEL_MAX = -128, 127
## Levels in all.
LEVELS = LEVEL_MAX - LEVEL_MIN + 1
## Lookahead range (the encoder clamps to it) and the default.
LOOKAHEAD_MIN, LOOKAHEAD_MAX, LOOKAHEAD_DEFAULT = 1, 8, 3
## Input scale: a value in [-1, 1) times this is the level before flooring.
SCALE = 128.0
## Bits of a nibble, its mask, a byte's mask.
NIBBLE_BITS, NIBBLE_MASK, BYTE_MASK = 4, 0xF, 0xFF
## A byte's sign bit (8-bit wrap of the decoder's accumulator).
SIGN_BIT = 0x80
## "No error found yet" (larger than any squared error sum of a window).
NO_ERROR = 1 << 62
## Number of nibbles a delta byte holds.
NIBBLES_PER_BYTE = 2


def quantize(x: float) -> int:
   """!A sample value as a level: clamp(floor(x * 128), -128, 127).
   @param x  the value (nominally in [-1, 1))
   @return the level
   """
   return max(LEVEL_MIN, min(LEVEL_MAX, math.floor(x * SCALE)))


def to_signed(byte: int) -> int:
   """!A byte as a signed 8-bit value.
   @param byte  0..255 (higher bits ignored)
   @return -128..127
   """
   byte &= BYTE_MASK
   return byte - (SIGN_BIT << 1) if byte & SIGN_BIT else byte


def search(window: list[int], previous: int) -> tuple[int, int]:
   """!The depth-first delta search over a window (the module notes' rule).
   @param window    the next samples' levels (1-8 of them)
   @param previous  the level before the first one
   @return (best total squared error, chosen nibble; len(DELTAS) when none is in range)
   """
   if not window:
      return 0, 0
   best, chosen = NO_ERROR, len(DELTAS)
   target = window[0]
   for index, delta in enumerate(DELTAS):
      level = previous + delta
      error = (target - level) * (target - level)
      if error >= best:
         continue   # cannot beat the best total (the rest of the window adds a non-negative error)
      rest, _ = search(window[1:], level)
      if error + rest < best and LEVEL_MIN <= level <= LEVEL_MAX:
         best, chosen = error + rest, index
   return best, chosen


## Lookahead 1's choices, built on first use: index (previous - LEVEL_MIN) * LEVELS + (sample - LEVEL_MIN).
_SINGLE: list[int] = []


def single_choice(previous: int, sample: int) -> int:
   """!The nibble lookahead 1 chooses (search() over a one-sample window, from a table).
   @param previous  the previous level
   @param sample    the target level
   @return the nibble
   """
   if not _SINGLE:
      table = [search([s], p)[1] for p in range(LEVEL_MIN, LEVEL_MAX + 1) for s in range(LEVEL_MIN, LEVEL_MAX + 1)]
      _SINGLE.extend(table)
   return _SINGLE[(previous - LEVEL_MIN) * LEVELS + (sample - LEVEL_MIN)]


def encoded_size(count: int, pad: bool) -> int:
   """!Bytes encode() writes for a number of samples.
   @param count  samples
   @param pad    whole blocks
   @return bytes
   """
   whole, rest = divmod(count, BLOCK_SAMPLES)
   if pad:
      return (whole + (1 if rest else 0)) * BLOCK_BYTES
   # a partial block: its first sample, then a byte per delta pair ending on an odd sample (1, 3, 5, ...)
   return whole * BLOCK_BYTES + ((1 + rest // NIBBLES_PER_BYTE) if rest else 0)


def encode(levels: list[int], lookahead: int = LOOKAHEAD_DEFAULT, pad: bool = True) -> bytes:
   """!Encodes levels (module notes).
   @param levels     samples, -128..127
   @param lookahead  search depth (clamped to 1-8)
   @param pad        whole blocks (True) or stop after the last sample (False)
   @return the encoded bytes
   @throws ValueError when a level is out of range
   """
   if any(not LEVEL_MIN <= v <= LEVEL_MAX for v in levels):
      raise ValueError("a DPCM input level outside -128..127")
   depth = max(LOOKAHEAD_MIN, min(LOOKAHEAD_MAX, lookahead))
   out = bytearray()
   for start in range(0, len(levels), BLOCK_SAMPLES):
      block = levels[start:start + BLOCK_SAMPLES]
      in_block = len(block)
      block = block + [0] * (BLOCK_SAMPLES - in_block)   # silence past the input (also read by the search)
      level = block[0]
      out.append(level & BYTE_MASK)
      limit = BLOCK_SAMPLES if pad else in_block
      position = 1
      high = 0
      while position < limit:
         low = choose(block, position, level, depth)   # a low nibble completes the byte
         level += DELTAS[low]
         position += 1
         out.append(high | low)
         if position >= BLOCK_SAMPLES or position >= limit:
            break
         nibble = choose(block, position, level, depth)   # a high nibble waits for its pair (dropped at the end
         level += DELTAS[nibble]                          # without pad)
         position += 1
         high = nibble << NIBBLE_BITS
   return bytes(out)


def choose(block: list[int], position: int, level: int, depth: int) -> int:
   """!The nibble of one sample: the search over the block's next `depth` samples (never past the block).
   @param block     the block's 64 levels
   @param position  the sample
   @param level     the previous level
   @param depth     the lookahead
   @return the nibble
   """
   if depth == 1:
      return single_choice(level, block[position])
   return search(block[position:position + min(depth, BLOCK_SAMPLES - position)], level)[1]


def decode(data: bytes, count: int) -> list[int]:
   """!Decodes samples (8-bit wrapping accumulator).
   @param data   encoded bytes from a block start
   @param count  samples to decode
   @return the levels
   @throws ValueError when data holds fewer than count samples
   """
   out: list[int] = []
   for i in range(count):
      block, j = divmod(i, BLOCK_SAMPLES)
      base = block * BLOCK_BYTES
      at = base + (0 if j == 0 else 1 + j // NIBBLES_PER_BYTE)
      if at >= len(data):
         raise ValueError(f"DPCM data ends before sample {i}")
      if j == 0:
         level = to_signed(data[at])
      else:
         nibble = data[at] >> NIBBLE_BITS if j % NIBBLES_PER_BYTE == 0 else data[at] & NIBBLE_MASK
         level = to_signed(level + DELTAS[nibble])
      out.append(level)
   return out
