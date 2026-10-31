#!/usr/bin/env python3
"""!Tests for tools/audio/midi_song.py (run by CTest, or directly): tiny MIDI files written by code.

Each test builds a MIDI file from (delta, event bytes) lists and checks the song bytes, the ticks a walk gives, or
the converter's intermediate events. Expected bytes are spelled with the module's command constants and the
lengths table.
"""
from __future__ import annotations

import struct
import sys
import unittest
from fractions import Fraction
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "audio"))
import midi_song as M  # noqa: E402

## Ticks per quarter note of the test files (the song's own resolution: no scaling).
DIVISION = 24


def vlq(value: int) -> bytes:
   """!A MIDI variable-length number.
   @param value  the number
   @return its bytes
   """
   out = [value & 0x7F]
   value >>= 7
   while value:
      out.append(0x80 | (value & 0x7F))
      value >>= 7
   return bytes(reversed(out))


def track(events: list[tuple[int, bytes]], end_delta: int = 0) -> bytes:
   """!A track chunk.
   @param events     (delta, event bytes) pairs
   @param end_delta  delta of the end of track
   @return the chunk
   """
   body = b"".join(vlq(d) + e for d, e in events) + vlq(end_delta) + b"\xff\x2f\x00"
   return b"MTrk" + struct.pack(">I", len(body)) + body


def midi(*tracks: bytes, division: int = DIVISION) -> bytes:
   """!A format 1 file.
   @param tracks    track chunks
   @param division  ticks per quarter note
   @return the file
   """
   return b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), division) + b"".join(tracks)


def on(key: int, velocity: int = 100, channel: int = 0) -> bytes:
   """!A note on.
   @param key       key
   @param velocity  velocity
   @param channel   channel
   @return the event
   """
   return bytes([0x90 | channel, key, velocity])


def off(key: int, channel: int = 0) -> bytes:
   """!A note off.
   @param key      key
   @param channel  channel
   @return the event
   """
   return bytes([0x80 | channel, key, 0])


def cc(number: int, value: int, channel: int = 0) -> bytes:
   """!A controller.
   @param number   controller
   @param value    value
   @param channel  channel
   @return the event
   """
   return bytes([0xB0 | channel, number, value])


def tempo(us: int) -> bytes:
   """!A tempo meta event.
   @param us  microseconds per quarter
   @return the event
   """
   return b"\xff\x51\x03" + us.to_bytes(3, "big")


def text(t: str) -> bytes:
   """!A marker text event.
   @param t  the text
   @return the event
   """
   return b"\xff\x06" + vlq(len(t)) + t.encode()


def wait(ticks: int) -> int:
   """!A WAIT command byte.
   @param ticks  a length of the table
   @return the byte
   """
   return M.WAIT + M.LENGTHS.index(ticks)


def note(ticks: int) -> int:
   """!A NOTE command byte.
   @param ticks  a length of the table (0: held)
   @return the byte
   """
   return M.NOTE + M.LENGTHS.index(ticks)


def notes(song: M.Song) -> list[list[tuple[int, int]]]:
   """!Each track's (tick, key) of every note it plays (a walk).
   @param song  the song
   @return per track
   """
   out = []
   for run in M.run_song(song):
      key = 0
      played = []
      for s in run.steps:
         if M.NOTE <= s.command < M.WAIT:
            key = s.args[0] if s.args else key
            played.append((s.tick, key))
      out.append(played)
   return out


class TempoTests(unittest.TestCase):
   """!TEMPO's single-precision division and half-away rounding."""

   def test_single_precision_changes_the_rounding(self) -> None:
      """!60000000 / 794702 = 75.4999987...: single precision gives 75.5, rounded up to 76."""
      self.assertEqual(M.round_half_away(Fraction(60000000, 794702)), 75)
      self.assertEqual(M.tempo_bpm(794702), 76)
      self.assertEqual(M.tempo_bpm(754717), 80)
      self.assertEqual(M.tempo_bpm(433213), 139)

   def test_ordinary_tempos(self) -> None:
      """!Whole BPMs come out unchanged; the exact method equals the platform's single rounding."""
      for bpm in range(20, 400):
         self.assertEqual(M.tempo_bpm(round(60000000 / bpm)), bpm)
      for us in range(200000, 1000000, 997):
         self.assertEqual(M.tempo_bpm(us), M.tempo_bpm_struct(us))

   def test_float32_rounding(self) -> None:
      """!Ties go to even; exact values stay."""
      self.assertEqual(M.float32(Fraction(3, 2)), Fraction(3, 2))
      one_and_half_ulp = 1 + Fraction(1, 2 ** 24)    # halfway between 1 and the next float: to even (1)
      self.assertEqual(M.float32(one_and_half_ulp), 1)
      above = 1 + Fraction(3, 2 ** 24)               # halfway between 1+2^-23 and 1+2^-22: to even (1+2^-22)
      self.assertEqual(M.float32(above), 1 + Fraction(2, 2 ** 23))

   def test_tempo_command(self) -> None:
      """!The TEMPO argument is BPM / 2 (BPM x 2 / 2 with -X), only in the first output track."""
      data = midi(track([(0, tempo(500000)), (0, on(60)), (24, off(60)), (0, on(62, channel=1)),
                         (24, off(62, channel=1))]))
      song = M.convert(data, M.parse_options("-E"))
      runs = M.run_song(song)
      self.assertEqual(len(runs), 2)
      tempos = [[s.args for s in r.steps if s.command == M.TEMPO] for r in runs]
      self.assertEqual(tempos, [[[60]], []])
      x2 = M.convert(midi(track([(0, tempo(500000)), (0, on(60)), (24, off(60))])), M.parse_options("-E -X"))
      self.assertEqual([s.args for s in M.run_song(x2)[0].steps if s.command == M.TEMPO], [[120]])


class ConversionTests(unittest.TestCase):
   """!Notes, waits, held notes, velocities, controllers and running status."""

   def test_one_note(self) -> None:
      """!VOLUME (master), TRANSPOSE 0, the note with key and velocity, its wait, END."""
      song = M.convert(midi(track([(0, on(60, 99)), (24, off(60))])), M.parse_options("-V100"))
      self.assertEqual(song.data, bytes([M.VOLUME, 100, M.TRANSPOSE, 0, note(24), 60, 100, wait(24), M.END]))
      self.assertEqual(song.tracks, [0])
      self.assertEqual(M.run_song(song)[0].end_tick, 24)

   def test_exact_gate_and_rounding(self) -> None:
      """!A 27-tick note: NOTE 24 (+3 with -E); gaps split into table lengths; velocities round up to 4."""
      data = midi(track([(0, on(60, 1)), (27, off(60))], end_delta=200))
      plain = M.convert(data, M.parse_options(""))
      exact = M.convert(data, M.parse_options("-E"))
      self.assertEqual(plain.data[2:7], bytes([M.TRANSPOSE, 0, note(24), 60, 4]))
      self.assertEqual(exact.data[2:8], bytes([M.TRANSPOSE, 0, note(24), 60, 4, 3]))
      self.assertEqual(M.run_song(exact)[0].end_tick, 227)
      waits = [M.LENGTHS[s.command - M.WAIT] for s in M.run_song(exact)[0].steps if M.WAIT <= s.command < M.END]
      self.assertEqual(sum(waits), 227)
      self.assertTrue(all(w in M.LENGTHS for w in waits))

   def test_held_note(self) -> None:
      """!A note longer than 96 ticks is NOTE 0 and a NOTE_OFF at its end."""
      song = M.convert(midi(track([(0, on(48)), (120, off(48))])), M.parse_options("-E"))
      steps = M.run_song(song)[0].steps
      kinds = [(s.tick, s.command) for s in steps if s.command in (M.NOTE, M.NOTE_OFF)]
      self.assertEqual(kinds, [(0, M.NOTE), (120, M.NOTE_OFF)])

   def test_controllers(self) -> None:
      """!Volume scaled by the master volume, pan, modulation, bend, program, echo, priority."""
      events = [(0, bytes([0xC0, 5])), (0, cc(7, 100)), (0, cc(10, 70)), (0, cc(1, 3)), (0, bytes([0xE0, 0, 72])),
                (0, cc(30, 8)), (0, cc(29, 5)), (0, cc(33, 4)), (0, on(60)), (24, off(60))]
      song = M.convert(midi(track(events)), M.parse_options("-V064"))
      steps = [(s.command, s.args) for s in M.run_song(song)[0].steps if s.command not in (M.TRANSPOSE,)]
      self.assertIn((M.PROGRAM, [5]), steps)
      self.assertIn((M.VOLUME, [100 * 64 // 127]), steps)
      self.assertIn((M.PAN, [70]), steps)
      self.assertIn((M.MOD_DEPTH, [3]), steps)
      self.assertIn((M.BEND, [72]), steps)
      self.assertIn((M.EXT, [M.EXT_ECHO_VOLUME, 5]), steps)
      self.assertIn((M.PRIORITY, [4]), steps)
      self.assertNotIn(M.VOLUME, [c for c, _ in steps[:1]])   # a volume comes before the note: no master VOLUME

   def test_extended_select_loses_its_wait(self) -> None:
      """!Controller 30 writes no wait: the 12 ticks after it are lost."""
      song = M.convert(midi(track([(0, on(60)), (24, off(60)), (0, cc(30, 8)), (12, on(62)), (24, off(62))])),
                       M.parse_options("-E"))
      self.assertEqual(notes(song), [[(0, 60), (24, 62)]])
      self.assertEqual(M.run_song(song)[0].end_tick, 48)

   def test_running_status(self) -> None:
      """!Two volume changes in a row: the second omits its command byte; an unchanged note omits key and velocity."""
      song = M.convert(midi(track([(0, cc(7, 100)), (0, cc(7, 90)), (0, on(60)), (24, off(60)), (0, on(60)),
                                   (24, off(60))])), M.parse_options("-E"))
      body = song.data[2:]
      self.assertEqual(body[:3], bytes([M.VOLUME, 100, 90]))
      self.assertEqual(body[3:], bytes([note(24), 60, 100, wait(24), note(24), wait(24), M.END]))
      uncompressed = M.convert(midi(track([(0, cc(7, 100)), (0, cc(7, 90)), (0, on(60)), (24, off(60))])),
                               M.parse_options("-E -N"))
      self.assertEqual(uncompressed.data[2:6], bytes([M.VOLUME, 100, M.VOLUME, 90]))

   def test_tracks_by_channel(self) -> None:
      """!One output track per (track, channel) with notes, in order; the end is the later end of track."""
      data = midi(track([(0, on(60, channel=2)), (24, off(60, channel=2)), (0, on(64)), (24, off(64))]),
                  track([(0, on(67, channel=1)), (96, off(67, channel=1))]))
      song = M.convert(data, M.parse_options("-E"))
      self.assertEqual(song.sources, [(0, 0), (0, 2), (1, 1)])
      self.assertEqual([r.end_tick for r in M.run_song(song)], [48, 48, 96])

   def test_options(self) -> None:
      """!The config line's options, with values attached or separate, letters in any case."""
      o = M.parse_options("-E -R50 -G_town -v080 -P 5 -X -N -L name")
      self.assertEqual((o.exact_gate, o.reverb, o.instruments, o.volume, o.priority, o.factor, o.compress, o.name),
                       (True, 50, "instruments:town", 80, 5, 2, False, "name"))
      self.assertEqual(M.parse_options("").instruments, "instruments:dummy")
      with self.assertRaises(M.MidiError):
         M.parse_options("-Q")

   def test_author_fields(self) -> None:
      """!The manifest's author form: fields over the options text."""
      o = M.options_from_entry({"options": "-E -V080", "volume": 90, "clocks_per_beat": 48, "reverb": None,
                                "instruments": "instruments:x", "compress": False})
      self.assertEqual((o.exact_gate, o.volume, o.factor, o.reverb, o.instruments, o.compress),
                       (True, 90, 2, None, "instruments:x", False))
      with self.assertRaises(M.MidiError):
         M.options_from_entry({"clocks_per_beat": 30})

   def test_errors(self) -> None:
      """!A note without an end, a file that is not MIDI."""
      with self.assertRaises(M.MidiError):
         M.convert(midi(track([(0, on(60))])), M.Options())
      with self.assertRaises(M.MidiError):
         M.convert(b"RIFF....", M.Options())


class StructureTests(unittest.TestCase):
   """!Loops, labels, patterns, variables, frames."""

   def test_loop(self) -> None:
      """!`[` and `]` markers: a jump back to the loop start; the walk reports the loop."""
      song = M.convert(midi(track([(0, text("[")), (0, on(60)), (24, off(60)), (0, text("]"))], end_delta=0)),
                       M.parse_options("-E"))
      run = M.run_song(song)[0]
      self.assertIsNone(run.end_tick)
      self.assertEqual(run.loop_tick, 24)
      self.assertIsNone(M.frames_to_end([run]))

   def test_patterns_keep_the_notes(self) -> None:
      """!Repeated whole notes become CALL / RETURN; without compression the same notes play at the same ticks."""
      bar = [(0, on(60)), (12, off(60)), (0, on(64)), (12, off(64)), (0, on(67)), (12, off(67)), (0, on(72)),
             (12, off(72)), (0, cc(10, 40)), (48, cc(10, 41))]
      events = bar * 4
      data = midi(track(events))
      packed = M.convert(data, M.parse_options("-E"))
      flat = M.convert(data, M.parse_options("-E -N"))
      self.assertIn(M.CALL, [s.command for s in M.run_song(packed)[0].steps])
      self.assertEqual(notes(packed), notes(flat))
      self.assertEqual(M.run_song(packed)[0].end_tick, M.run_song(flat)[0].end_tick)
      self.assertLess(len(packed.data), len(flat.data))

   def test_variables(self) -> None:
      """!Controllers 13 / 14 / 12 write VAR (operation, address, value)."""
      song = M.convert(midi(track([(0, cc(13, 0)), (0, cc(14, 3)), (0, cc(12, 117)), (0, on(60)), (24, off(60))])),
                       M.parse_options("-E"))
      self.assertIn([0, 3, 117], [s.args for s in M.run_song(song)[0].steps if s.command == M.VAR])

   def test_frames(self) -> None:
      """!150 per tick: no tempo is a tick a frame; 300 BPM (TEMPO 150) two from the frame after the TEMPO."""
      fast = M.convert(midi(track([(0, tempo(200000)), (0, on(60)), (24, off(60))])), M.parse_options("-E"))
      self.assertEqual(M.frames_to_end(M.run_song(fast)), 13)
      slow = M.convert(midi(track([(0, on(60)), (24, off(60))])), M.parse_options("-E"))
      self.assertEqual(M.frames_to_end(M.run_song(slow)), 25)

   def test_silent_marks_end_blocks(self) -> None:
      """!A note on with velocity 0 ends the note and adds an event (it only waits)."""
      song = M.convert(midi(track([(0, on(60)), (24, on(60, 0)), (0, on(62)), (24, off(62))])),
                       M.parse_options("-E"))
      self.assertEqual(notes(song), [[(0, 60), (24, 62)]])


if __name__ == "__main__":
   unittest.main()
