#!/usr/bin/env python3
"""!Tests for tools/audio/wav_sample.py and tools/audio/dpcm.py (run by CTest, or directly): sine, square and noise
WAV files written by code, in every sample format, with loop / key / exact-value chunks; DPCM block edges, the
padding rule, and the encoder against an independent exhaustive search."""
from __future__ import annotations

import json
import math
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools" / "audio"
sys.path.insert(0, str(TOOLS))
import dpcm  # noqa: E402
import wav_sample as W  # noqa: E402

## The test files' rate.
RATE = 10512


def chunk(cid: bytes, body: bytes) -> bytes:
   """!A RIFF chunk (an odd body gets its pad byte).
   @param cid   the id
   @param body  the body
   @return the chunk
   """
   return cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) % 2 else b"")


def wav(samples: bytes, bits: int = 8, tag: int = 1, rate: int = RATE, extra: bytes = b"", channels: int = 1) -> bytes:
   """!A WAV file.
   @param samples   the data chunk's bytes
   @param bits      bits per sample
   @param tag       1 integer, 3 float
   @param rate      sample rate
   @param extra     chunks before the data
   @param channels  channels
   @return the file
   """
   align = bits // 8 * channels
   fmt = struct.pack("<HHIIHH", tag, channels, rate, rate * align, align, bits)
   body = b"WAVE" + chunk(b"fmt ", fmt) + extra + chunk(b"data", samples)
   return b"RIFF" + struct.pack("<I", len(body)) + body


def smpl(key: int, start: int | None = None, end: int | None = None, fraction: int = 0) -> bytes:
   """!A sampler chunk.
   @param key       unity key
   @param start     loop start (None: no loop)
   @param end       loop end (the last sample played)
   @param fraction  pitch fraction
   @return the chunk
   """
   loops = 0 if start is None else 1
   body = struct.pack("<9I", 0, 0, 0, key, fraction, 0, 0, loops, 0)
   if loops:
      body += struct.pack("<6I", 0, 0, start, end, 0, 0)
   return chunk(b"smpl", body)


def sine(n: int, period: int = 37, amplitude: float = 0.9) -> list[float]:
   """!A sine.
   @param n          samples
   @param period     samples per cycle
   @param amplitude  peak
   @return values
   """
   return [amplitude * math.sin(2 * math.pi * i / period) for i in range(n)]


def u8(values: list[float]) -> bytes:
   """!Values as unsigned 8-bit samples.
   @param values  values in [-1, 1)
   @return bytes
   """
   return bytes(max(0, min(255, int(round(v * 127)) + 128)) for v in values)


def reference_choice(window: list[int], previous: int) -> tuple[int, int]:
   """!An exhaustive search over every in-range path (no pruning): (best total error, first best index).
   @param window    target levels
   @param previous  the previous level
   @return (error, index)
   """
   best = None
   for i, d in enumerate(dpcm.DELTAS):
      level = previous + d
      if not dpcm.LEVEL_MIN <= level <= dpcm.LEVEL_MAX:
         continue
      error = (window[0] - level) ** 2 + (reference_choice(window[1:], level)[0] if len(window) > 1 else 0)
      if best is None or error < best[0]:
         best = (error, i)
   assert best is not None
   return best


def reference_encode(levels: list[int], lookahead: int) -> list[int]:
   """!The decoded output an encoder following reference_choice() gives (whole blocks of silence-padded input).
   @param levels     input levels
   @param lookahead  depth
   @return decoded levels
   """
   out = []
   for start in range(0, len(levels), dpcm.BLOCK_SAMPLES):
      block = levels[start:start + dpcm.BLOCK_SAMPLES]
      block += [0] * (dpcm.BLOCK_SAMPLES - len(block))
      level = block[0]
      out.append(level)
      for j in range(1, dpcm.BLOCK_SAMPLES):
         window = block[j:j + min(lookahead, dpcm.BLOCK_SAMPLES - j)]
         level += dpcm.DELTAS[reference_choice(window, level)[1]]
         out.append(level)
   return out[:len(levels)]


class WavReadTests(unittest.TestCase):
   """!Reading: formats, chunks, errors."""

   def test_formats(self) -> None:
      """!8-bit unsigned, 16/24/32-bit signed and 32/64-bit float values."""
      self.assertEqual(W.read_wav(wav(bytes([0, 128, 255]))).values, [-1.0, 0.0, 127 / 128])
      self.assertEqual(W.read_wav(wav(struct.pack("<3h", -32768, -1, 256), bits=16)).values,
                       [-1.0, -1 / 32768, 256 / 32768])
      s24 = b"".join(v.to_bytes(3, "little", signed=True) for v in (-8388608, 4096))
      self.assertEqual(W.read_wav(wav(s24, bits=24)).values, [-1.0, 4096 / 8388608])
      self.assertEqual(W.read_wav(wav(struct.pack("<i", 1 << 30), bits=32)).values, [0.5])
      self.assertEqual(W.read_wav(wav(struct.pack("<2f", 0.25, -0.75), bits=32, tag=3)).values, [0.25, -0.75])
      self.assertEqual(W.read_wav(wav(struct.pack("<d", 0.125), bits=64, tag=3)).values, [0.125])

   def test_chunks(self) -> None:
      """!smpl (key, loop end + 1, the fraction's scale), agbp, agbl, an odd chunk's pad byte."""
      extra = chunk(b"note", b"odd") + smpl(72, 10, 19, 1 << 31) + chunk(b"agbp", struct.pack("<I", 999)) + \
         chunk(b"agbl", struct.pack("<I", 15))
      info = W.read_wav(wav(u8(sine(40)), extra=extra))
      self.assertEqual((info.key, info.loop, info.loop_start, info.loop_end), (72, True, 10, 20))
      self.assertEqual(info.tune, (1 << 31) / (4294967296.0 * 100.0))
      self.assertEqual((info.agb_pitch, info.agb_length), (999, 15))
      self.assertEqual(W.read_wav(wav(u8(sine(8)), extra=smpl(60, 2, 99))).loop_end, 8)   # capped to the count

   def test_errors(self) -> None:
      """!Stereo, a wrong RIFF length, a chunk past the end, no data."""
      with self.assertRaises(W.WavError):
         W.read_wav(wav(bytes(4), bits=8, channels=2))
      bad = bytearray(wav(bytes(4)))
      bad[4] += 1
      with self.assertRaises(W.WavError):
         W.read_wav(bytes(bad))
      cut = wav(bytes(4))[:-2]
      with self.assertRaises(W.WavError):
         W.read_wav(b"RIFF" + struct.pack("<I", len(cut) - 8) + cut[8:])
      no_data = b"WAVE" + chunk(b"fmt ", struct.pack("<HHIIHH", 1, 1, RATE, RATE, 1, 8))
      with self.assertRaises(W.WavError):
         W.read_wav(b"RIFF" + struct.pack("<I", len(no_data)) + no_data)


class ConvertTests(unittest.TestCase):
   """!Headers, PCM8 data, overrides."""

   def test_pcm8(self) -> None:
      """!floor(x x 128) clamped, padded to 4 bytes; pitch = rate x 1024; length = the loop end."""
      values = [-1.0, -0.004, 0.0, 0.999, 1.0]
      s = W.convert(W.read_wav(wav(struct.pack("<5f", *values), bits=32, tag=3)))
      self.assertEqual(list(s.data), [0x80, 0xFF, 0, 127, 127, 0, 0, 0])
      self.assertEqual((s.format, s.pitch, s.length, s.loop), ("pcm8", RATE * 1024, 5, False))
      self.assertEqual(s.header(), struct.pack("<4I", 0, RATE * 1024, 0, 5))

   def test_key_and_tune(self) -> None:
      """!Key 72: half the rate; a loop sets bit 30; agbp / agbl override pitch and length."""
      s = W.convert(W.read_wav(wav(u8(sine(30)), extra=smpl(72, 4, 9))))
      self.assertEqual(s.pitch, int(RATE * math.pow(2.0, (60 - 72) / 12.0) * 1024.0))
      self.assertEqual((s.loop, s.loop_start, s.length, len(s.data)), (True, 4, 10, 12))
      self.assertEqual(struct.unpack("<I", s.header()[:4])[0], 1 << 30)
      exact = W.convert(W.read_wav(wav(u8(sine(30)), extra=chunk(b"agbp", struct.pack("<I", 12345)) +
                                       chunk(b"agbl", struct.pack("<I", 29)))))
      self.assertEqual((exact.pitch, exact.length, exact.samples), (12345, 29, 30))

   def test_overrides(self) -> None:
      """!Command-line style overrides: loop start (turns the loop on), loop end, key, tune, rate."""
      info = W.read_wav(wav(u8(sine(50))))
      s = W.convert(info, loop_start=60, loop_end=40, key=48, tune=50.0, rate=8000)
      self.assertEqual((s.loop, s.loop_start, s.length), (True, 50, 40))
      self.assertEqual(s.pitch, int(8000 * math.pow(2.0, 1.0 + 50.0 / 1200.0) * 1024.0))

   def test_author_form(self) -> None:
      """!A manifest's author-form entry: the WAV beside it, DPCM with its options, a loop end override."""
      with tempfile.TemporaryDirectory() as d:
         (Path(d) / "x.wav").write_bytes(wav(u8(sine(90))))
         s = W.convert_entry({"wav": "x.wav", "format": "dpcm4", "lookahead": 1, "pad": False, "length": 80},
                             Path(d))
         self.assertEqual((s.format, s.length, len(s.data)), ("dpcm4", 80, dpcm.encoded_size(80, False)))
         with self.assertRaises(W.WavError):
            W.convert_entry({"wav": "missing.wav"}, Path(d))

   def test_cli(self) -> None:
      """!The command line writes the data and the manifest entry."""
      with tempfile.TemporaryDirectory() as d:
         src = Path(d) / "tone.wav"
         src.write_bytes(wav(u8(sine(70))))
         out = Path(d) / "tone.bin"
         entry = Path(d) / "tone.json"
         done = subprocess.run([sys.executable, str(TOOLS / "wav_sample.py"), str(src), str(out), "--dpcm",
                                "--lookahead", "1", "--no-pad", "--json", str(entry)], capture_output=True, text=True)
         self.assertEqual(done.returncode, 0, done.stderr)
         self.assertEqual(len(out.read_bytes()), dpcm.encoded_size(70, False))
         self.assertEqual(json.loads(entry.read_text())["format"], "dpcm4")


class DpcmTests(unittest.TestCase):
   """!The DPCM encoder and decoder."""

   def test_sizes(self) -> None:
      """!Block edges with and without padding; a final high nibble is not written without padding."""
      for n in (1, 2, 3, 4, 5, 63, 64, 65, 66, 127, 128, 129, 130):
         levels = [int(127 * math.sin(i / 5)) for i in range(n)]
         self.assertEqual(len(dpcm.encode(levels, 1, False)), dpcm.encoded_size(n, False), n)
         self.assertEqual(len(dpcm.encode(levels, 1, True)), dpcm.encoded_size(n, True), n)
         self.assertEqual(dpcm.encoded_size(n, True) % dpcm.BLOCK_BYTES, 0)
      self.assertEqual(dpcm.encoded_size(3, False), 2)
      self.assertEqual(dpcm.encoded_size(64, False), dpcm.BLOCK_BYTES)

   def test_against_exhaustive_search(self) -> None:
      """!Square, sine and noise inputs decode as an exhaustive in-range search chooses (lookahead 1-3)."""
      rng = random.Random(7)
      inputs = {"square": [96 if (i // 9) % 2 else -96 for i in range(150)],
                "sine": [dpcm.quantize(v) for v in sine(150, 23, 0.99)],
                "noise": [rng.randint(-128, 127) for _ in range(130)]}
      for name, levels in inputs.items():
         for lookahead in (1, 2, 3):
            data = dpcm.encode(levels, lookahead, True)
            self.assertEqual(dpcm.decode(data, len(levels)), reference_encode(levels, lookahead), (name, lookahead))

   def test_rules(self) -> None:
      """!Levels stay in range (a nearer level past 127 is not taken); the lowest index wins a tie."""
      self.assertEqual(dpcm.DELTAS[dpcm.single_choice(120, 127)], 4)    # 120 + 9 would leave the range
      for previous in range(-128, 128, 5):
         for target in range(-128, 128):
            errors = [(target - previous - d) ** 2 if -128 <= previous + d <= 127 else None for d in dpcm.DELTAS]
            best = min(e for e in errors if e is not None)
            self.assertEqual(dpcm.single_choice(previous, target), errors.index(best))

   def test_decode_wraps(self) -> None:
      """!The decoder's accumulator wraps in 8 bits; data that ends early is an error."""
      self.assertEqual(dpcm.decode(bytes([0x7F, 0x01]), 2), [127, -128])
      with self.assertRaises(ValueError):
         dpcm.decode(bytes([0]), 2)
      with self.assertRaises(ValueError):
         dpcm.encode([200])


if __name__ == "__main__":
   unittest.main()
