#!/usr/bin/env python3
"""!Tests of apps/audio_render (D-34 M4 B1b): the CLI over the engine's test audio table writes four consistent
streams per frame (format §11: L1 state.jsonl, L2 segments.bin, L3 tones.txt, L4 out.wav) and a summary whose
hashes are those of the files, and two runs of the same request are identical. With the high-quality build
(capsule_audio_render_hq) as well: its L1-L3 equal the plain build's, its WAV has 4x the rate.

   test_audio_render.py PATH_TO_capsule_audio_render [PATH_TO_capsule_audio_render_hq]
"""
from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

## The renderer, from the command line.
RENDERER: Path | None = None
## The high-quality renderer (optional).
RENDERER_HQ: Path | None = None
## The HQ mix's samples per output sample of the plain one.
HQ_FACTOR = 4
## Bytes of a WAV header (RIFF + fmt + data).
WAV_HEADER_BYTES = 44
## Bytes per stereo 16-bit sample frame.
WAV_FRAME_BYTES = 4
## Sides per ring segment.
SIDES = 2
## FNV-1a 64 offset basis and prime.
FNV64_BASIS = 0xCBF29CE484222325
## FNV-1a 64 prime.
FNV64_PRIME = 0x100000001B3
## 64-bit mask.
MASK64 = (1 << 64) - 1
## Frames of the fixed-length request.
FIXED_FRAMES = 40
## C14a: TrackState::flags' "exists" bit (capsule/audio/state.h kTrackExists).
TRACK_EXISTS = 0x80
## C14a: the frame whose calls start --then's sound in test_then.
THEN_FRAME = 5
## The renderer's exit status for a usage error.
USAGE_STATUS = 2


def fnv64(data: bytes) -> str:
   """!FNV-1a 64 of bytes, as audio_render prints it.
   @param data  the bytes
   @return 16 hex digits
   """
   h = FNV64_BASIS
   for b in data:
      h = ((h ^ b) * FNV64_PRIME) & MASK64
   return f"{h:016x}"


def render(out: Path, *args: str, exe: Path | None = None) -> dict[str, object]:
   """!Runs the renderer.
   @param out   output folder
   @param args  request
   @param exe   the renderer (default RENDERER)
   @return its summary.json
   """
   exe = exe or RENDERER
   assert exe is not None
   out.mkdir(parents=True, exist_ok=True)
   done = subprocess.run([str(exe), *args, "--out", str(out), "--quiet"], capture_output=True, text=True,
                         check=False)
   if done.returncode != 0:
      raise AssertionError(f"audio_render {' '.join(args)}: status {done.returncode}: {done.stderr}")
   return json.loads((out / "summary.json").read_text(encoding="utf-8"))


class AudioRenderTests(unittest.TestCase):
   """!The streams and the summary (skipped without the renderer: unittest discovery runs this file too)."""

   def setUp(self) -> None:
      """!Skips when no renderer was given."""
      if RENDERER is None or not RENDERER.is_file():
         self.skipTest("pass the capsule_audio_render binary (ctest audio_render_tests does)")

   def sounds(self) -> int:
      """!The table's sound count.
      @return it
      """
      assert RENDERER is not None
      done = subprocess.run([str(RENDERER), "--list"], capture_output=True, text=True, check=True)
      return len(json.loads(done.stdout)["sounds"])

   def check_streams(self, out: Path, summary: dict[str, object]) -> None:
      """!Every stream has one entry per frame and the summary's hashes.
      @param out      the folder
      @param summary  its summary.json
      """
      frames = int(summary["frames"])
      samples = int(summary["samples_per_frame"])
      segments = (out / "segments.bin").read_bytes()
      self.assertEqual(len(segments), frames * samples * SIDES)
      states = (out / "state.jsonl").read_bytes()
      lines = states.decode("utf-8").splitlines()
      self.assertEqual(len(lines), frames)
      self.assertEqual([json.loads(line)["frame"] for line in lines], list(range(1, frames + 1)))
      wav = (out / "out.wav").read_bytes()
      self.assertEqual(wav[:4], b"RIFF")
      (rate,) = struct.unpack_from("<I", wav, 24)
      self.assertEqual(rate, int(summary["rate"]))
      (data_bytes,) = struct.unpack_from("<I", wav, 40)
      self.assertEqual(len(wav), WAV_HEADER_BYTES + data_bytes)
      self.assertEqual(data_bytes, frames * int(summary["output_samples_per_frame"]) * WAV_FRAME_BYTES)
      self.assertEqual(summary["l1"], fnv64(states))
      self.assertEqual(summary["l2"], fnv64(segments))
      self.assertEqual(summary["l3"], fnv64((out / "tones.txt").read_bytes()))
      self.assertEqual(summary["l4"], fnv64(wav[WAV_HEADER_BYTES:]))
      for line in (out / "tones.txt").read_text(encoding="utf-8").splitlines():
         frame, offset, value = line.split()
         self.assertTrue(1 <= int(frame) <= frames)
         self.assertLess(int(offset, 16), 0x40)
         self.assertLess(int(value, 16), 0x100)

   def test_list(self) -> None:
      """!--list prints the table as JSON."""
      assert RENDERER is not None
      done = subprocess.run([str(RENDERER), "--list"], capture_output=True, text=True, check=True)
      table = json.loads(done.stdout)
      self.assertIn("sounds", table)
      self.assertGreater(table["samples_per_frame"], 0)

   def test_tour_streams(self) -> None:
      """!The tour writes consistent streams and runs to its end."""
      with tempfile.TemporaryDirectory() as tmp:
         out = Path(tmp)
         summary = render(out, "--tour")
         self.assertGreater(int(summary["frames"]), 0)
         self.check_streams(out, summary)

   def test_repeatable(self) -> None:
      """!The same request twice gives the same hashes; a fixed frame count is honoured."""
      if self.sounds() == 0:
         self.skipTest("the table has no sounds")
      with tempfile.TemporaryDirectory() as tmp:
         first = render(Path(tmp) / "a", "--sound", "0", "--frames", str(FIXED_FRAMES))
         second = render(Path(tmp) / "b", "--sound", "0", "--frames", str(FIXED_FRAMES))
         self.assertEqual(first, second)
         self.assertEqual(int(first["frames"]), FIXED_FRAMES)
         self.check_streams(Path(tmp) / "a", first)


   def ending_sounds(self, out: Path) -> list[tuple[int, dict[str, object]]]:
      """!The sounds of the table that end by itself (C14a).
      @param out  a scratch folder
      @return their numbers and summaries
      """
      found: list[tuple[int, dict[str, object]]] = []
      for n in range(self.sounds()):
         summary = render(out / str(n), "--sound", str(n), "--no-wav")
         if int(summary["status_end_frame"]) != 0:
            found.append((n, summary))
      if not found:
         self.skipTest("no sound of the table ends")
      return found

   def test_end_frames(self) -> None:
      """!C14a (S7): end_frame is the frame whose ticks ran the last END (after it no track of the player exists),
      status_end_frame the first frame whose status shows the player ended (the tick after the last END)."""
      if self.sounds() == 0:
         self.skipTest("the table has no sounds")
      with tempfile.TemporaryDirectory() as tmp:
         n, summary = self.ending_sounds(Path(tmp))[0]
         end, status_end = int(summary["end_frame"]), int(summary["status_end_frame"])
         self.assertTrue(0 < end < status_end, summary)
         states = [json.loads(line) for line in (Path(tmp) / str(n) / "state.jsonl").read_text().splitlines()]
         player = next(s["player"] for s in self.listing()["sounds"] if s["sound"] == n)

         def exists(frame: int) -> bool:
            state = states[frame - 1]
            p = next(q for q in state["players"] if q["index"] == player)
            first, count = int(p["first_track"]), int(p["track_count"])
            return any(int(t["flags"]) & TRACK_EXISTS for t in state["tracks"][first:first + count])

         self.assertTrue(exists(end - 1))
         self.assertFalse(exists(end))
         self.assertEqual(int(summary["then_end_frame"]), 0)

   def test_then(self) -> None:
      """!C14a (S7): --then N@F plays a second sound with frame F's calls; the run lasts until both ended."""
      if self.sounds() == 0:
         self.skipTest("the table has no sounds")
      with tempfile.TemporaryDirectory() as tmp:
         ending = self.ending_sounds(Path(tmp))
         players = {int(s["sound"]): int(s["player"]) for s in self.listing()["sounds"]}
         n, alone = ending[0]
         other = [m for m, _ in ending if players[m] != players[n]]
         if not other:
            self.skipTest("no two ending sounds on different players")
         both = render(Path(tmp) / "then", "--sound", str(n), "--then", f"{other[0]}@{THEN_FRAME}", "--no-wav")
         self.assertEqual(both["end_frame"], alone["end_frame"])   # its own tracks' ENDs, on its own player
         self.assertGreater(int(both["then_end_frame"]), THEN_FRAME)
         self.assertGreaterEqual(int(both["frames"]), int(both["then_status_end_frame"]))
         bad = subprocess.run([str(RENDERER), "--sound", str(n), "--then", "1@1", "--out", tmp], capture_output=True,
                              check=False)
         self.assertEqual(bad.returncode, USAGE_STATUS)   # F >= 2: blank 1 comes before any call

   def listing(self) -> dict[str, object]:
      """!The table's --list output.
      @return it
      """
      assert RENDERER is not None
      done = subprocess.run([str(RENDERER), "--list"], capture_output=True, text=True, check=True)
      return dict(json.loads(done.stdout))

   def test_hq_keeps_the_ring_and_state(self) -> None:
      """!The HQ build: the same L1-L3 as the plain one, 4x the output rate and samples."""
      if RENDERER_HQ is None or not RENDERER_HQ.is_file():
         self.skipTest("no HQ renderer given")
      with tempfile.TemporaryDirectory() as tmp:
         plain = render(Path(tmp) / "plain", "--tour")
         hq = render(Path(tmp) / "hq", "--tour", exe=RENDERER_HQ)
         for level in ("l1", "l2", "l3", "frames"):
            self.assertEqual(plain[level], hq[level], level)
         self.assertEqual(int(hq["rate"]), HQ_FACTOR * int(plain["rate"]))
         self.assertEqual(int(hq["output_samples_per_frame"]), HQ_FACTOR * int(plain["output_samples_per_frame"]))
         self.check_streams(Path(tmp) / "hq", hq)


if __name__ == "__main__":
   if len(sys.argv) < 2:
      print("usage: test_audio_render.py PATH_TO_capsule_audio_render", file=sys.stderr)
      sys.exit(2)
   RENDERER = Path(sys.argv.pop(1))
   if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
      RENDERER_HQ = Path(sys.argv.pop(1))
   unittest.main()
