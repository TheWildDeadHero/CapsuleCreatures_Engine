#!/usr/bin/env python3
"""!WAV file -> engine sample (audio format §2.1, §8.2): a library and a command line.

   wav_sample.py IN.wav OUT.bin [--dpcm] [--lookahead N] [--no-pad] [--loop-start N] [--loop-end N]
                 [--key K] [--tune CENTS] [--rate HZ] [--asset sample:NAME] [--json OUT.json]

writes the sample's data bytes (OUT.bin: the bytes from the first data byte, without the header) and prints (or
writes) its manifest entry (`capsule.audio/1` "samples" item: format, pitch, loop, loop_start, length, data).

Reading (read_wav()):

- a RIFF/WAVE file whose RIFF length matches the file; chunks in order, an odd-length chunk followed by a pad byte;
  a chunk running past the end is an error; `fmt `: mono only, integer PCM of 8 (unsigned), 16, 24 or 32 bits
  (signed, little-endian) or IEEE float of 32 or 64 bits; the last `data` chunk is the samples (whole frames);
- `smpl`: the unity key (capped at 127), the pitch fraction as cents = fraction / (2^32 x 100) (the reference
  converter's scale), at most one loop, of type 0: loop start, and loop end = the chunk's end + 1 (the chunk names
  the last sample played; 32-bit wrap);
- `agbp` (u32): the exact pitch value; `agbl` (u32): the exact length (the sample's end); 0 means absent;
- every sample as a value: 8-bit (b - 128) / 128, 16/24/32-bit s / 2^(bits - 1), float as stored; reads past the
  data are 0; the loop end is capped to the sample count (no `smpl` loop: the sample count).

Converting (convert()):

- overrides: loop start = min(value, loop end) and the loop on; loop end = min(value, loop end); tune, key, rate;
- pitch = the `agbp` value when present, else trunc(p x 1024) with p = rate when key 60 and tune 0, else
  rate x 2^((60 - key) / 12 + tune / 1200) (double precision, as the reference converter);
- length = the `agbl` value when present, else the loop end;
- PCM8 data: quantize() of each sample below the loop end (clamp(floor(x x 128), -128, 127), two's complement),
  then zeros to a multiple of 4 bytes; DPCM4 data: dpcm.encode() of the same levels (lookahead 1-8, pad / no pad).
  The data covers the samples below the loop end; `length` may be shorter (the extra bytes stay readable).
"""
from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dpcm  # noqa: E402

## Manifest formats.
FORMAT_PCM8, FORMAT_DPCM4 = "pcm8", "dpcm4"
## The header's format word: 0 PCM, 1 DPCM; bit 30 marks a loop.
FORMAT_WORD = {FORMAT_PCM8: 0, FORMAT_DPCM4: 1}
## The loop bit of the format word.
LOOP_BIT = 1 << 30
## Header bytes (format word, pitch, loop start, length: four little-endian u32).
HEADER_BYTES = 16
## PCM data is padded to this many bytes.
PCM_ALIGN = 4
## RIFF ids.
RIFF, WAVE, FMT, DATA, SMPL, AGBP, AGBL = b"RIFF", b"WAVE", b"fmt ", b"data", b"smpl", b"agbp", b"agbl"
## Bytes of a chunk's id and length.
CHUNK_HEAD = 8
## Bytes of an id.
ID_BYTES = 4
## fmt tags: integer PCM, IEEE float.
TAG_PCM, TAG_FLOAT = 1, 3
## Supported (tag, block align, bits) -> sample kind.
KINDS = {(TAG_PCM, 1, 8): "u8", (TAG_PCM, 2, 16): "s16", (TAG_PCM, 3, 24): "s24", (TAG_PCM, 4, 32): "s32",
         (TAG_FLOAT, 4, 32): "f32", (TAG_FLOAT, 8, 64): "f64"}
## Bytes per sample of each kind.
KIND_BYTES = {"u8": 1, "s16": 2, "s24": 3, "s32": 4, "f32": 4, "f64": 8}
## The unsigned 8-bit zero.
U8_ZERO = 128.0
## Bits of a byte.
BITS_PER_BYTE = 8
## Offsets in the fmt chunk: tag, channels, rate, block align, bits.
FMT_TAG, FMT_CHANNELS, FMT_RATE, FMT_ALIGN, FMT_BITS = 0, 2, 4, 12, 14
## Offsets in the smpl chunk: unity key, pitch fraction, loop count; the first loop's type, start, end.
SMPL_KEY, SMPL_FRACTION, SMPL_LOOPS, SMPL_LOOP_TYPE, SMPL_LOOP_START, SMPL_LOOP_END = 12, 16, 28, 40, 44, 48
## The pitch fraction's scale (2^32 x 100, the reference converter's).
FRACTION_SCALE = 4294967296.0 * 100.0
## The highest key.
KEY_MAX = 127
## An octave's frequency ratio.
OCTAVE = 2.0
## The base key and its frequency maths.
BASE_KEY, SEMITONES, CENTS_PER_OCTAVE = 60, 12.0, 1200.0
## Pitch values are Hz x 1024.
PITCH_SCALE = 1024.0
## 32-bit wrap.
U32_MASK = 0xFFFFFFFF
## No loop end given (the reference's default before capping to the sample count).
NO_LOOP_END = U32_MASK


class WavError(Exception):
   """!A WAV file this converter does not read."""


@dataclass
class WavInfo:
   """!What read_wav() takes from a file."""

   ## Sample rate (Hz).
   rate: int
   ## Sample kind (u8, s16, s24, s32, f32, f64).
   kind: str
   ## Every sample's value.
   values: list[float] = field(default_factory=list)
   ## Loop start (samples).
   loop_start: int = 0
   ## Loop end (exclusive), capped to the sample count.
   loop_end: int = NO_LOOP_END
   ## A loop is set.
   loop: bool = False
   ## Unity key.
   key: int = BASE_KEY
   ## Tuning (cents).
   tune: float = 0.0
   ## Exact pitch value (0: absent).
   agb_pitch: int = 0
   ## Exact length (0: absent).
   agb_length: int = 0


@dataclass
class Sample:
   """!A converted sample: the header fields and the data bytes."""

   ## FORMAT_PCM8 or FORMAT_DPCM4.
   format: str
   ## Hz x 1024 at the base key.
   pitch: int
   ## Looping.
   loop: bool
   ## Loop start (samples).
   loop_start: int
   ## Samples played (the end).
   length: int
   ## The data bytes (from the first data byte).
   data: bytes
   ## Samples the data encodes (the loop end).
   samples: int

   def header(self) -> bytes:
      """!The 16-byte header the reference converter writes before the data.
      @return format word, pitch, loop start, length (little-endian u32)
      """
      word = FORMAT_WORD[self.format] | (LOOP_BIT if self.loop else 0)
      return struct.pack("<IIII", word, self.pitch & U32_MASK, self.loop_start & U32_MASK, self.length & U32_MASK)

   def entry(self, key: str, data_path: str) -> dict[str, object]:
      """!The manifest entry (`samples` item).
      @param key        its asset key ("sample:NAME")
      @param data_path  the data file, relative to the manifest
      @return the entry
      """
      s = self   # a local alias (Doxygen reads some uses of self.x as new members)
      return {"key": key, "format": s.format, "pitch": s.pitch, "loop": s.loop, "loop_start": s.loop_start,
              "length": s.length, "data": data_path}


def u16(data: bytes, at: int) -> int:
   """!A little-endian u16 (WavError when the chunk is too short).
   @param data  the chunk
   @param at    its offset
   @return the value
   """
   if at + struct.calcsize("<H") > len(data):
      raise WavError("a chunk is too short")
   return struct.unpack_from("<H", data, at)[0]


def u32(data: bytes, at: int) -> int:
   """!A little-endian u32 (WavError when the chunk is too short).
   @param data  the chunk
   @param at    its offset
   @return the value
   """
   if at + struct.calcsize("<I") > len(data):
      raise WavError("a chunk is too short")
   return struct.unpack_from("<I", data, at)[0]


def sample_values(raw: bytes, kind: str) -> list[float]:
   """!The data chunk's samples as values (whole samples only).
   @param raw   the data chunk
   @param kind  the sample kind
   @return values
   """
   width = KIND_BYTES[kind]
   count = len(raw) // width
   raw = raw[:count * width]
   if kind == "u8":
      return [(b - U8_ZERO) / U8_ZERO for b in raw]
   if kind == "f32":
      return list(struct.unpack(f"<{count}f", raw))
   if kind == "f64":
      return list(struct.unpack(f"<{count}d", raw))
   scale = float(1 << (width * BITS_PER_BYTE - 1))
   return [int.from_bytes(raw[i * width:(i + 1) * width], "little", signed=True) / scale for i in range(count)]


def read_wav(data: bytes) -> WavInfo:
   """!Reads a WAV file (module notes).
   @param data  the file
   @return what it holds
   @throws WavError when it is not a file this converter reads
   """
   if data[:ID_BYTES] != RIFF or len(data) < CHUNK_HEAD + ID_BYTES:
      raise WavError("not a RIFF file")
   if u32(data, ID_BYTES) + CHUNK_HEAD != len(data):
      raise WavError("the RIFF length does not match the file")
   if data[CHUNK_HEAD:CHUNK_HEAD + ID_BYTES] != WAVE:
      raise WavError("not a WAVE file")
   pos = CHUNK_HEAD + ID_BYTES
   info: WavInfo | None = None
   raw: bytes | None = None
   smpl: bytes | None = None
   pitch = length = 0
   while pos + CHUNK_HEAD <= len(data):
      cid, size = data[pos:pos + ID_BYTES], u32(data, pos + ID_BYTES)
      if pos + CHUNK_HEAD + size > len(data):
         raise WavError(f"a chunk runs past the end of the file (offset {pos})")
      body = data[pos + CHUNK_HEAD:pos + CHUNK_HEAD + size]
      if cid == FMT:
         if u16(body, FMT_CHANNELS) != 1:
            raise WavError("not mono")
         spec = (u16(body, FMT_TAG), u16(body, FMT_ALIGN), u16(body, FMT_BITS))
         if spec not in KINDS:
            raise WavError(f"unsupported sample format (tag, align, bits) {spec}")
         info = WavInfo(u32(body, FMT_RATE), KINDS[spec])
      elif cid == DATA:
         raw = body
      elif cid == SMPL:
         smpl = body
      elif cid == AGBP and size >= struct.calcsize("<I"):
         pitch = u32(body, 0)
      elif cid == AGBL and size >= struct.calcsize("<I"):
         length = u32(body, 0)
      pos += CHUNK_HEAD + size + (size & 1)
   if info is None:
      raise WavError("no fmt chunk")
   if raw is None:
      raise WavError("no data chunk")
   info.values = sample_values(raw, info.kind)
   info.agb_pitch, info.agb_length = pitch, length
   if smpl is not None:
      info.key = min(u32(smpl, SMPL_KEY), KEY_MAX)
      info.tune = u32(smpl, SMPL_FRACTION) / FRACTION_SCALE
      loops = u32(smpl, SMPL_LOOPS)
      if loops > 1:
         raise WavError("more than one loop in the smpl chunk")
      if loops == 1:
         if u32(smpl, SMPL_LOOP_TYPE) != 0:
            raise WavError("an unsupported loop type")
         info.loop_start = u32(smpl, SMPL_LOOP_START)
         info.loop_end = (u32(smpl, SMPL_LOOP_END) + 1) & U32_MASK
         info.loop = True
   info.loop_end = min(info.loop_end, len(info.values))
   return info


def value_at(info: WavInfo, i: int) -> float:
   """!One sample's value (0 past the data).
   @param info  the file
   @param i     the sample
   @return its value
   """
   return info.values[i] if 0 <= i < len(info.values) else 0.0


def convert(info: WavInfo, dpcm4: bool = False, lookahead: int = dpcm.LOOKAHEAD_DEFAULT, pad: bool = True,
            loop_start: int | None = None, loop_end: int | None = None, tune: float | None = None,
            key: int | None = None, rate: int | None = None) -> Sample:
   """!Converts a read file (module notes).
   @param info        the file
   @param dpcm4       DPCM4 instead of PCM8
   @param lookahead   DPCM search depth (1-8)
   @param pad         DPCM whole blocks
   @param loop_start  loop start override (turns the loop on)
   @param loop_end    loop end override
   @param tune        tuning override (cents)
   @param key         key override (clamped 0-127)
   @param rate        rate override (Hz)
   @return the sample
   """
   start, end, looping = info.loop_start, info.loop_end, info.loop
   cents, base, hz = info.tune, info.key, info.rate
   if loop_start is not None:
      start, looping = min(loop_start, end), True
   if loop_end is not None:
      end = min(loop_end, end)
   if tune is not None:
      cents = tune
   if key is not None:
      base = max(0, min(KEY_MAX, key))
   if rate is not None:
      hz = rate
   if base == BASE_KEY and cents == 0.0:
      frequency = float(hz)
   else:
      frequency = hz * math.pow(OCTAVE, (BASE_KEY - float(base)) / SEMITONES + cents / CENTS_PER_OCTAVE)
   pitch = info.agb_pitch if info.agb_pitch else int(frequency * PITCH_SCALE) & U32_MASK
   length = info.agb_length if info.agb_length else end
   levels = [dpcm.quantize(value_at(info, i)) for i in range(end)]
   if dpcm4:
      data = dpcm.encode(levels, lookahead, pad)
   else:
      body = bytearray(v & dpcm.BYTE_MASK for v in levels)
      body.extend(bytes(-len(body) % PCM_ALIGN))
      data = bytes(body)
   return Sample(FORMAT_DPCM4 if dpcm4 else FORMAT_PCM8, pitch, looping, start, length, data, end)


def convert_entry(entry: dict[str, object], base: Path) -> Sample:
   """!Converts a manifest sample in the author form (§2.1): "wav" (relative to base), optional "format"
   ("dpcm4"), "lookahead", "pad", "loop_start", "length" (the loop end), "key", "tune_cents".
   @param entry  the sample entry
   @param base   the manifest's folder
   @return the sample
   @throws WavError for a missing or unreadable file
   """
   if "wav" not in entry:
      raise WavError("an author-form sample needs 'wav'")
   try:
      data = (base / str(entry["wav"])).read_bytes()
   except OSError as e:
      raise WavError(str(e)) from None

   def number(name: str) -> int | None:
      """!An optional integer field.
      @param name  the field
      @return its value, or None
      """
      v = entry.get(name)
      return int(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None

   tune = entry.get("tune_cents")
   return convert(read_wav(data), entry.get("format") == FORMAT_DPCM4, number("lookahead") or dpcm.LOOKAHEAD_DEFAULT,
                  bool(entry.get("pad", True)), number("loop_start"), number("length"),
                  float(tune) if isinstance(tune, (int, float)) else None, number("key"))


def main(argv: list[str] | None = None) -> int:
   """!Command line (module notes).
   @param argv  arguments (default: sys.argv[1:])
   @return exit status
   """
   p = argparse.ArgumentParser(description="Converts a WAV file to an engine sample.")
   p.add_argument("wav", type=Path)
   p.add_argument("out", type=Path, help="the data file to write")
   p.add_argument("--dpcm", action="store_true", help="DPCM4 compression")
   p.add_argument("--lookahead", type=int, default=dpcm.LOOKAHEAD_DEFAULT)
   p.add_argument("--no-pad", action="store_true", help="DPCM: stop after the last sample")
   p.add_argument("--loop-start", type=int)
   p.add_argument("--loop-end", type=int)
   p.add_argument("--key", type=int)
   p.add_argument("--tune", type=float, help="cents")
   p.add_argument("--rate", type=int)
   p.add_argument("--asset", help="asset key of the entry (default: sample:<file name>)")
   p.add_argument("--json", type=Path, help="write the manifest entry here (default: print it)")
   args = p.parse_args(argv)
   try:
      sample = convert(read_wav(args.wav.read_bytes()), args.dpcm, args.lookahead, not args.no_pad,
                       args.loop_start, args.loop_end, args.tune, args.key, args.rate)
   except (OSError, WavError) as e:
      print(f"wav_sample: {e}", file=sys.stderr)
      return 1
   args.out.write_bytes(sample.data)
   entry = sample.entry(args.asset or f"sample:{args.wav.stem}", args.out.name)
   text = json.dumps(entry, indent=1) + "\n"
   if args.json:
      args.json.write_text(text, encoding="utf-8")
   else:
      sys.stdout.write(text)
   return 0


if __name__ == "__main__":
   sys.exit(main())
