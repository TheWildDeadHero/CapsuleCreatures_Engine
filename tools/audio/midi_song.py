#!/usr/bin/env python3
"""!Standard MIDI file -> engine song (audio format §2.5, §3, §8.1): a library and a command line.

   midi_song.py IN.mid OUT.json [options]        writes OUT.json (the compiled song entry) and OUT.seq beside it
   midi_song.py IN.mid --listing [options]       prints each track's commands with their ticks

Options (one line of a conversion config, as the reference converter takes them; a letter's value may follow it
directly or as the next word): -VN master volume (127), -GSET instrument set (`instruments:SET`, one leading
`_` dropped; default `instruments:dummy`), -PN priority (0), -RN reverb (none), -X 48 clocks per beat (24),
-E exact gate times, -N no compression, -LNAME the song's name (`song:NAME`; default the file name).

The conversion (the reference converter's documented behaviour, restated):

Reading. A format 0 or 1 file with ticks-per-quarter timing. The first track is read once for the *sequence
events*: text events of 1-2 characters `[` (loop start), `][` (loop end and start), `]` (loop end), `:` (label),
each numbered in file order; tempo; time signatures (a whole note = 96 x clocks-factor x numerator / 2^exponent
ticks); its end of track. Then every track (the first included) is read once per MIDI channel 0-15; a channel
with at least one note of positive length makes one output track, in (track, channel) order. A track's events:
notes (a note on with a velocity; its length runs to the first note off, or note on with velocity 0, of the same
key on the channel; a note without an end before the end of track is an error), controllers, program changes,
pitch bends (the high byte), and every note on with velocity 0 as a *silent mark* (no command; it ends a pattern
block, see Compression); the end of track. Running status follows the MIDI rules (meta and system events clear
it). Text events longer than 2 characters, other meta events and system events are skipped.

Merging and times. Each output track merges its events with the sequence events (at equal time and kind, the
sequence event first; tempo events only into the first output track); the merged end is the later end of track.
Times become ticks: 24 x factor x time / division (integer division); a note's length likewise, at least 1, and
without -E lengths under 96 round down to the lengths table L (§3). Velocities round up to a multiple of 4
(at most 127). Whole-note marks are inserted every whole note from tick 0 (a time signature restarts the count
from itself; in the first output track a time signature that changes the length also marks the original one, a
command-free boundary). Notes longer than 96 ticks become held notes (NOTE 0) plus a note-off event at their end.
Events are then ordered stably by (time, kind) with a note's kind raised by its key and note-offs by key; the
kinds in order: note-off, label, loop end, loop end+start, loop start, original time signature, whole-note mark,
pattern, time signature, tempo, program, controller, bend, note. Gaps over 96 ticks split into 96-tick waits and
a gap not in L into its rounded-down L value plus the rest; each event's wait is the gap to the next event; events
after the end of track are dropped.

Commands, per event (each followed by its wait, waits of 0 omitted). The track starts with VOLUME (master volume)
when no volume controller comes before its first note, the first event's wait, TRANSPOSE 0. Note: NOTE L-index of
its length (NOTE 0 for a held one), key, velocity, gate extra (with -E, the length minus its L value, 1-3).
Note-off event: NOTE_OFF key. Labels and loop starts: a jump target; loop end: JUMP to the latest loop start.
Tempo: TEMPO round(60000000 / microseconds per quarter) x factor / 2 (integers), where the division is done in
IEEE single precision (rounded to nearest, ties to even) and the rounding of that single-precision value is half
away from zero. Program: PROGRAM n. Bend: BEND high byte. Controllers: 1 MOD_DEPTH, 7 VOLUME v x master / 127
(integer), 10 PAN, 20 BEND_RANGE, 21 LFO_SPEED, 22 MOD_TYPE, 24 FINE_TUNE, 26 LFO_DELAY, 33 / 39 PRIORITY;
13 / 14 / 15 store a variable operation, its address and its jump label number, 12 / 16 write VAR (operation,
address, value; the jump ops 6-17 jump to the label 17 defined: controller 17 v marks label v); 30 selects the
extended command (8 echo volume, 9 echo length) and 29 / 31 write it (EXT 0 v / EXT 1 v; another selection writes
nothing): **controller 30 writes no wait** (the reference behaviour: its wait is lost). Other controllers and the
silent marks write only their wait. The track ends with END.

Compression (on unless -N): (a) a whole-note block (a mark and the events up to the next boundary: a note-off
event, label, loop marker, original time signature, mark, pattern, silent mark or the end) whose score is at
least 6 becomes a pattern: every later block identical to it (same first wait, same events) becomes CALL to the
first copy, which gets a label and a RETURN at its boundary; the score counts a non-zero first wait, per note
1 for each of key / velocity / L-length / exact length that changed (at least 1), per other event 1 when it
repeats the previous kind (2 for a controller with another number), 2 otherwise, and 1 per non-zero wait (a held
note's L-length compares as a value distinct from every L entry); (b) running status: a command equal to the
previous written one (not after a wait, label or pattern for notes; labels, patterns, waits and non-running
commands reset the memory as the reference does) omits its command byte, and a note whose key (and velocity)
did not change omits them (EXT is always written in full: the engine's assembler never omits it; the running
command is EXT either way). Running status does not change what is played.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path

## The lengths table L (ticks; §3).
LENGTHS = tuple(range(25)) + (28, 30, 32, 36, 40, 42, 44, 48, 52, 54, 56, 60, 64, 66, 68, 72, 76, 78, 80, 84, 88,
                              90, 92, 96)
## A whole note's ticks (the longest length; also the split size of gaps and the held-note threshold).
WHOLE = 96

## Command bytes (§3).
NOTE, WAIT, END, JUMP, CALL, RETURN, REPEAT, VAR = 0x80, 0xB1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7
## Command bytes (§3), continued.
PRIORITY, TEMPO, TRANSPOSE, PROGRAM, VOLUME, PAN, BEND, BEND_RANGE = 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF
## Command bytes (§3), continued.
LFO_SPEED, LFO_DELAY, MOD_DEPTH, MOD_TYPE, FINE_TUNE, EXT, NOTE_OFF = 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6
## The first reserved byte.
RESERVED = 0xF7
## EXT sub-commands written: echo volume, echo length.
EXT_ECHO_VOLUME, EXT_ECHO_LENGTH = 0, 1
## Argument bytes are below this.
ARG_LIMIT = 0x80
## Commands in the running set (§3, R).
RUNNING = frozenset({PROGRAM, VOLUME, PAN, BEND, BEND_RANGE, LFO_SPEED, LFO_DELAY, MOD_DEPTH, MOD_TYPE, FINE_TUNE,
                     EXT, NOTE_OFF})
## VAR operations: the first jump operation and the last one.
VAR_FIRST_JUMP, VAR_LAST = 6, 17
## The variable area's size.
VAR_AREA = 16
## The first VAR operation that takes its operand from the area.
VAR_FIRST_FROM_AREA = 3
## Comparisons of the jump operations (equal, not equal, above, at least, at most, below), immediate then area.
VAR_JUMP_KINDS = 6

## Event kinds (their order decides events at the same time; notes add their key).
EV_SILENT = 0x00
## Event kinds, continued.
EV_END_OF_TIE, EV_LABEL, EV_LOOP_END, EV_LOOP_END_BEGIN, EV_LOOP_BEGIN = 0x01, 0x11, 0x12, 0x13, 0x14
## Event kinds, continued.
EV_ORIGINAL_TIME_SIGNATURE, EV_WHOLE_NOTE_MARK, EV_PATTERN, EV_TIME_SIGNATURE = 0x15, 0x16, 0x17, 0x18
## Event kinds, continued.
EV_TEMPO, EV_PROGRAM, EV_CONTROLLER, EV_BEND, EV_NOTE, EV_TIME_SPLIT, EV_END = 0x19, 0x21, 0x22, 0x23, 0x40, 0xFE, 0xFF
## The highest kind that ends a pattern block (EV_END ends one too).
EV_LAST_BOUNDARY = EV_PATTERN
## An unknown kind the score compares (never produced; kept so the score's rule reads as written).
EV_UNUSED_KIND = 0x25

## MIDI: header and track ids, the header's length.
MTHD, MTRK, MTHD_LENGTH = b"MThd", b"MTrk", 6
## MIDI: where the first track header starts.
FIRST_TRACK = 14
## MIDI: status bit, meta, the first system byte, channel mask.
STATUS, META, SYSTEM, CHANNEL_MASK = 0x80, 0xFF, 0xF0, 0x0F
## MIDI: channel event kinds (the high nibble).
NOTE_OFF_EVENT, NOTE_ON_EVENT, CONTROL_EVENT, PROGRAM_EVENT, PRESSURE_EVENT, BEND_EVENT = 0x8, 0x9, 0xB, 0xC, 0xD, 0xE
## MIDI: channels.
CHANNELS = 16
## MIDI: meta events: the text range, end of track, tempo, time signature.
META_TEXT_FIRST, META_TEXT_LAST, META_END, META_TEMPO, META_TIME_SIGNATURE = 1, 7, 0x2F, 0x51, 0x58
## MIDI: tempo and time signature lengths.
TEMPO_BYTES, TIME_SIGNATURE_BYTES = 3, 4
## MIDI: longest text read (longer ones are skipped).
TEXT_MAX = 2
## MIDI: the largest time signature exponent and whole-note length.
EXPONENT_LIMIT, WHOLE_LIMIT = 16, 0x10000
## MIDI: variable-length numbers: 7 bits per byte.
VLQ_BITS, VLQ_MASK = 7, 0x7F
## Bits of a byte, a byte's largest value.
BYTE_BITS, BYTE_MAX = 8, 0xFF
## Bits of a nibble (a channel event's kind is the status byte's high nibble).
NIBBLE_BITS = 4
## Data bytes of channel events (program change and channel pressure have one).
ONE_DATA, TWO_DATA = 1, 2
## Loop markers.
MARKERS = {"[": EV_LOOP_BEGIN, "][": EV_LOOP_END_BEGIN, "]": EV_LOOP_END, ":": EV_LABEL}

## Ticks per beat at factor 1.
CLOCKS_PER_BEAT = 24
## Microseconds per minute.
US_PER_MINUTE = 60000000
## The tempo byte is BPM / 2.
TEMPO_DIVISOR = 2
## Defaults: master volume, priority, the largest volume.
DEFAULT_VOLUME, DEFAULT_PRIORITY, VOLUME_MAX = 127, 0, 127
## The largest velocity.
VELOCITY_MAX = 127
## Velocities round up to a multiple of this.
VELOCITY_STEP = 4
## Pan / bend / tune centre (argument = centre + signed value).
CENTRE = 64
## Controllers.
CC_MOD, CC_VOLUME, CC_PAN, CC_VAR_WRITE, CC_VAR_OP, CC_VAR_ADDRESS, CC_VAR_LABEL = 1, 7, 10, 12, 13, 14, 15
## Controllers, continued.
CC_VAR_WRITE_2, CC_LABEL, CC_BEND_RANGE, CC_LFO_SPEED, CC_MOD_TYPE, CC_TUNE, CC_LFO_DELAY = 16, 17, 20, 21, 22, 24, 26
## Controllers, continued.
CC_EXT_VALUE, CC_EXT_SELECT, CC_EXT_VALUE_2, CC_PRIORITY, CC_PRIORITY_2 = 29, 30, 31, 33, 39
## Extended command selections.
SELECT_ECHO_VOLUME, SELECT_ECHO_LENGTH = 8, 9
## A controller -> its plain command.
PLAIN_CONTROLLERS = {CC_MOD: MOD_DEPTH, CC_PAN: PAN, CC_BEND_RANGE: BEND_RANGE, CC_LFO_SPEED: LFO_SPEED,
                     CC_MOD_TYPE: MOD_TYPE, CC_TUNE: FINE_TUNE, CC_LFO_DELAY: LFO_DELAY}
## Compression: the lowest score made a pattern.
SCORE_MIN = 6
## The pattern flag of a whole-note mark's number.
PATTERN_FLAG = 0x80000000
## The score's start values: velocity and key "previous", duration "previous" (none).
SCORE_VELOCITY, SCORE_KEY, SCORE_NO_DURATION = 0x80, 0x40, -(1 << 31)
## Clocks factor with -X.
FACTOR_X = 2
## Song data offsets are u16.
U16_LIMIT = 0x10000
## Bytes of a u16 argument.
U16_BYTES = 2
## The most tracks a song holds (§2.5).
MAX_TRACKS = 16
## Reverb argument range.
REVERB_MAX = 127
## Exact one half and two (rounding).
HALF, TWO = Fraction(1, 2), Fraction(2)
## Score of an event repeating the previous kind, of another one.
SCORE_SAME, SCORE_OTHER = 1, 2
## Single precision: mantissa bits (with the hidden bit).
FLOAT32_MANTISSA = 24
## Walker: ticks after which a song that has not ended is reported as not ending.
WALK_TICK_LIMIT = 1 << 20
## The sequencer's tempo threshold (one tick per 150 of its counter) and the start tempo.
TICK_THRESHOLD, START_TEMPO = 150, 150
## The deepest call stack (a CALL at this depth ends the track).
CALL_DEPTH = 3
## Lengths of the variable operations' arguments (operation, address, value).
VAR_ARGS = 3
## Echo / extended arguments: sub-commands with a u16 / u32 argument; the others take one byte.
EXT_HOLD, EXT_START = 3, 4
## Bytes of a u32 argument.
U32_BYTES = 4


class MidiError(Exception):
   """!A file (or options) the converter cannot convert."""


## The held-note length's L value in the score (distinct from every L entry and from SCORE_NO_DURATION).
HELD_LOOKUP = object()


def l_index(ticks: int) -> int:
   """!A length's index in L.
   @param ticks  a value of L
   @return its index
   @throws MidiError when it is not in L
   """
   try:
      return LENGTHS.index(ticks)
   except ValueError:
      raise MidiError(f"{ticks} ticks is not a length of the table") from None


def round_down_length(ticks: int) -> int:
   """!The largest L value not above a length (0..96).
   @param ticks  the length
   @return its L value
   """
   return max(v for v in LENGTHS if v <= ticks)


def round_velocity(velocity: int) -> int:
   """!A velocity rounded up to a multiple of 4, at most 127.
   @param velocity  0..127
   @return the rounded velocity
   """
   return min(VELOCITY_MAX, -(-velocity // VELOCITY_STEP) * VELOCITY_STEP)


def float32(q: Fraction) -> Fraction:
   """!A positive rational rounded to IEEE single precision (to nearest, ties to even), exactly.
   @param q  the value (> 0, in the normal range)
   @return the single-precision value
   """
   exponent = q.numerator.bit_length() - q.denominator.bit_length()
   if TWO ** exponent > q:
      exponent -= 1
   unit = TWO ** (exponent - (FLOAT32_MANTISSA - 1))   # the spacing of floats in [2^e, 2^(e+1))
   scaled = q / unit
   whole = scaled.numerator // scaled.denominator
   rest = scaled - whole
   if rest > HALF or (rest == HALF and whole & 1):
      whole += 1
   return whole * unit


def round_half_away(q: Fraction) -> int:
   """!Rounds a non-negative rational to an integer, halves away from zero.
   @param q  the value
   @return the integer
   """
   return int((q + HALF).__floor__())


def tempo_bpm(microseconds: int) -> int:
   """!The reference converter's beats per minute of a tempo event (single-precision division, rounded).
   @param microseconds  microseconds per quarter note (> 0)
   @return BPM
   """
   return round_half_away(float32(Fraction(US_PER_MINUTE, microseconds)))


def tempo_bpm_struct(microseconds: int) -> int:
   """!tempo_bpm() through the platform's single-precision rounding of the double quotient (a cross-check: it can
   differ only when the double lands exactly on a single-precision midpoint).
   @param microseconds  microseconds per quarter note (> 0)
   @return BPM
   """
   single = struct.unpack("<f", struct.pack("<f", US_PER_MINUTE / microseconds))[0]
   return round_half_away(Fraction(single))


@dataclass
class Options:
   """!Conversion options (module notes)."""

   ## Master volume.
   volume: int = DEFAULT_VOLUME
   ## The instrument set's asset key.
   instruments: str = "instruments:dummy"
   ## Song priority.
   priority: int = DEFAULT_PRIORITY
   ## Reverb, None: leave it.
   reverb: int | None = None
   ## Clocks-per-beat factor (1, or 2 with -X).
   factor: int = 1
   ## -E.
   exact_gate: bool = False
   ## Not -N.
   compress: bool = True
   ## -L (the song's name).
   name: str | None = None


def parse_int(text: str, option: str) -> int:
   """!A leading integer (leading spaces and a sign allowed, trailing text ignored), as the reference reads it.
   @param text    the value
   @param option  the option letter (for errors)
   @return the number
   """
   m = re.match(r"\s*([+-]?\d+)", text)
   if not m:
      raise MidiError(f"-{option}: not a number: {text!r}")
   return int(m.group(1))


def parse_options(text: str | list[str]) -> Options:
   """!Options from a config line's text (module notes).
   @param text  the options ("-E -R50 -G_set -V080") or their words
   @return the options
   @throws MidiError for an unknown or incomplete option
   """
   words = text.split() if isinstance(text, str) else list(text)
   out = Options()
   i = 0
   while i < len(words):
      word = words[i]
      i += 1
      if not (word.startswith("-") and len(word) > 1):
         continue   # a file name
      letter = word[1].upper()
      if letter in "EXN":
         out.exact_gate |= letter == "E"
         out.factor = FACTOR_X if letter == "X" else out.factor
         out.compress &= letter != "N"
         continue
      if letter not in "GLPRV":
         raise MidiError(f"unknown option {word!r}")
      if len(word) > U16_BYTES:
         value = word[U16_BYTES:]
      elif i < len(words):
         value, i = words[i], i + 1
      else:
         raise MidiError(f"{word}: missing value")
      if letter == "G":
         out.instruments = "instruments:" + (value[1:] if value.startswith("_") else value)
      elif letter == "L":
         out.name = value
      elif letter == "P":
         out.priority = parse_int(value, letter)
      elif letter == "R":
         out.reverb = parse_int(value, letter)
      else:
         out.volume = parse_int(value, letter)
   return out


## Clocks per beat of the manifest's author form (24, or 48 for the factor 2).
CLOCKS_PER_BEAT_X = CLOCKS_PER_BEAT * FACTOR_X


def options_from_entry(entry: dict[str, object]) -> Options:
   """!The options of a manifest song in the author form (§2.5): its "options" text, then the fields "volume",
   "instruments", "priority", "reverb", "clocks_per_beat" (24 | 48), "exact_gate", "compress" over it.
   @param entry  the song entry
   @return the options
   @throws MidiError for a field of the wrong kind
   """
   opts = parse_options(str(entry.get("options", "")))
   for name in ("volume", "priority"):
      if name in entry:
         if not isinstance(entry[name], int):
            raise MidiError(f"'{name}' must be an integer")
         setattr(opts, name, entry[name])
   if "reverb" in entry:
      if entry["reverb"] is not None and not isinstance(entry["reverb"], int):
         raise MidiError("'reverb' must be an integer or null")
      opts.reverb = entry["reverb"]   # type: ignore[assignment]
   if "instruments" in entry:
      opts.instruments = str(entry["instruments"])
   if "clocks_per_beat" in entry:
      if entry["clocks_per_beat"] not in (CLOCKS_PER_BEAT, CLOCKS_PER_BEAT_X):
         raise MidiError(f"'clocks_per_beat' must be {CLOCKS_PER_BEAT} or {CLOCKS_PER_BEAT_X}")
      opts.factor = FACTOR_X if entry["clocks_per_beat"] == CLOCKS_PER_BEAT_X else 1
   for name in ("exact_gate", "compress"):
      if name in entry:
         setattr(opts, name, bool(entry[name]))
   return opts


@dataclass
class Event:
   """!One event of a track (time is absolute until waits are computed, then the wait after the event)."""

   ## Ticks (or the wait after it).
   time: int
   ## EV_*.
   kind: int
   ## Key (notes, note-offs).
   note: int = 0
   ## Velocity, controller number, program, block number, bend low byte.
   param1: int = 0
   ## Length, controller value, tempo, mark number, bend high byte.
   param2: int = 0

   def key(self) -> tuple[int, int, int]:
      """!The ordering key: (time, kind raised by a note's key, a note-off's key).
      @return the key
      """
      e = self   # a local alias (Doxygen reads some uses of self.x as new members)
      kind = e.kind + (e.note if e.kind == EV_NOTE else 0)
      return e.time, kind, e.note if e.kind == EV_END_OF_TIE else 0


def before(a: Event, b: Event) -> bool:
   """!Whether a orders strictly before b.
   @param a  an event
   @param b  another
   @return a < b
   """
   return a.key() < b.key()


def boundary(kind: int) -> bool:
   """!Whether an event kind ends a pattern block.
   @param kind  EV_*
   @return True for EV_END and the kinds up to EV_PATTERN
   """
   return kind == EV_END or kind <= EV_LAST_BOUNDARY


class Reader:
   """!The file as a cursor with running status (the reference reads the whole file, unbounded by chunks)."""

   def __init__(self, data: bytes) -> None:
      """!A reader at the start.
      @param data  the file
      """
      ## The file.
      self.data = data
      ## Position.
      self.pos = 0
      ## Running status.
      self.status = 0
      ## Absolute time of the track being read.
      self.time = 0

   def u8(self) -> int:
      """!One byte.
      @return it
      """
      if self.pos >= len(self.data):
         raise MidiError("unexpected end of file")
      self.pos += 1
      return self.data[self.pos - 1]

   def big(self, count: int) -> int:
      """!A big-endian number.
      @param count  bytes
      @return its value
      """
      v = 0
      for _ in range(count):
         v = (v << BYTE_BITS) | self.u8()
      return v

   def vlq(self) -> int:
      """!A variable-length number.
      @return its value
      """
      v = 0
      while True:
         b = self.u8()
         v = (v << VLQ_BITS) | (b & VLQ_MASK)
         if not b & STATUS:
            return v

   def skip(self, count: int) -> None:
      """!Skips bytes.
      @param count  how many
      @return None
      """
      self.pos += count

   def category(self) -> tuple[str, int, int]:
      """!An event's category with running status.
      @return ("control" | "system" | "meta", status byte, data bytes of a control event)
      """
      status = self.u8()
      if status < STATUS:
         self.pos -= 1
         status = self.status
      if status == META:
         self.status = 0
         return "meta", status, 0
      if status >= SYSTEM:
         self.status = 0
         return "system", status, 0
      if status >= STATUS:
         self.status = status
         return "control", status, ONE_DATA if status >> NIBBLE_BITS in (PROGRAM_EVENT, PRESSURE_EVENT) else TWO_DATA
      raise MidiError("invalid event (a data byte without running status)")


@dataclass
class File:
   """!A read file: its header, the sequence events and where each track's data starts."""

   ## Ticks per quarter note.
   division: int
   ## Each track's data start.
   tracks: list[int] = field(default_factory=list)
   ## The sequence events (first track).
   sequence: list[Event] = field(default_factory=list)


def read_sequence(r: Reader, factor: int, blocks: list[int]) -> list[Event]:
   """!The first track's sequence events (module notes).
   @param r       the reader at the track's data
   @param factor  clocks factor
   @param blocks  the block counter (one element, shared)
   @return the events up to and with the end of track
   """
   out: list[Event] = []
   while True:
      r.time += r.vlq()
      kind, _, size = r.category()
      if kind == "control":
         r.skip(size)
         continue
      if kind == "system":
         r.skip(r.vlq())
         continue
      meta = r.u8()
      e = Event(r.time, 0)
      if META_TEXT_FIRST <= meta <= META_TEXT_LAST:
         length = r.vlq()
         if length == 0:
            raise MidiError("failed to read event text")   # the reference cannot read an empty text
         text = ""
         if length <= TEXT_MAX:
            text = r.data[r.pos:r.pos + length].decode("latin-1")
         r.skip(length)
         if text not in MARKERS:
            continue
         e.kind, e.param1 = MARKERS[text], blocks[0] & BYTE_MAX
         blocks[0] += 1
      elif meta == META_END:
         r.skip(r.vlq())
         e.kind = EV_END
      elif meta == META_TEMPO:
         if r.vlq() != TEMPO_BYTES:
            raise MidiError("invalid tempo size")
         e.kind, e.param2 = EV_TEMPO, r.big(TEMPO_BYTES)
      elif meta == META_TIME_SIGNATURE:
         if r.vlq() != TIME_SIGNATURE_BYTES:
            raise MidiError("invalid time signature size")
         numerator, exponent = r.u8(), r.u8()
         if exponent >= EXPONENT_LIMIT:
            raise MidiError("invalid time signature denominator")
         r.skip(TIME_SIGNATURE_BYTES - U16_BYTES)
         whole = WHOLE * numerator * factor // (1 << exponent)
         if not 0 < whole < WHOLE_LIMIT:
            raise MidiError("invalid time signature")
         e.kind, e.param2 = EV_TIME_SIGNATURE, whole
      else:
         r.skip(r.vlq())
         continue
      out.append(e)
      if e.kind == EV_END:
         return out


def note_end(r: Reader, channel: int, key: int) -> int:
   """!A note's length: the time to the first note off (or note on with velocity 0) of its key and channel.
   @param r        the reader just after the note on (restored afterwards)
   @param channel  its channel
   @param key      its key
   @return the length in file ticks
   """
   pos, status = r.pos, r.status
   length = 0
   while True:
      length += r.vlq()
      kind, byte, size = r.category()
      if kind == "control":
         if byte & CHANNEL_MASK != channel:
            r.skip(size)
            continue
         high = byte >> NIBBLE_BITS
         if high in (NOTE_OFF_EVENT, NOTE_ON_EVENT):
            k, velocity = r.u8(), r.u8()
            if k == key and (high == NOTE_OFF_EVENT or velocity == 0):
               break
         else:
            r.skip(size)
      elif kind == "system":
         r.skip(r.vlq())
      else:
         meta = r.u8()
         r.skip(r.vlq())
         if meta == META_END:
            raise MidiError("a note doesn't end")
   r.pos, r.status = pos, status
   return length


def read_track(r: Reader, start: int, channel: int) -> tuple[list[Event], bool]:
   """!One channel's events of one track (module notes).
   @param r        the reader
   @param start    the track's data start
   @param channel  the channel
   @return (events up to and with the end of track, whether a note of positive length exists)
   """
   r.pos, r.time, r.status = start, 0, 0
   out: list[Event] = []
   notes = False
   while True:
      r.time += r.vlq()
      kind, byte, size = r.category()
      e = Event(r.time, EV_SILENT)
      if kind == "control":
         if byte & CHANNEL_MASK != channel:
            r.skip(size)
            continue
         high = byte >> NIBBLE_BITS
         if high == NOTE_ON_EVENT:
            key, velocity = r.u8(), r.u8()
            if velocity:
               e.kind, e.note, e.param1 = EV_NOTE, key, velocity
               e.param2 = note_end(r, channel, key)
               notes |= e.param2 > 0
         elif high == CONTROL_EVENT:
            e.kind, e.param1, e.param2 = EV_CONTROLLER, r.u8(), r.u8()
         elif high == PROGRAM_EVENT:
            e.kind, e.param1 = EV_PROGRAM, r.u8()
         elif high == BEND_EVENT:
            e.kind, e.param1, e.param2 = EV_BEND, r.u8(), r.u8()
         else:
            r.skip(size)
            continue
         out.append(e)   # a note on with velocity 0 stays as a silent mark
         continue
      if kind == "system":
         r.skip(r.vlq())
         continue
      meta = r.u8()
      r.skip(r.vlq())
      if meta == META_END:
         e.kind = EV_END
         out.append(e)
         return out, notes


def read_file(data: bytes, factor: int) -> File:
   """!The header, the track starts and the sequence events.
   @param data    the file
   @param factor  clocks factor
   @return the file
   """
   r = Reader(data)
   if data[:len(MTHD)] != MTHD:
      raise MidiError("not a MIDI file")
   r.pos = len(MTHD)
   if r.big(U32_BYTES) != MTHD_LENGTH:
      raise MidiError("the MIDI header's length isn't 6")
   fmt, count, division = r.big(U16_BYTES), r.big(U16_BYTES), r.big(U16_BYTES)
   if fmt >= U16_BYTES:
      raise MidiError(f"unsupported MIDI format ({fmt})")
   if division & (U16_LIMIT >> 1) or division == 0:
      raise MidiError(f"unsupported MIDI time division ({division})")
   out = File(division)
   at = FIRST_TRACK
   for _ in range(count):
      r.pos = at
      if data[at:at + len(MTRK)] != MTRK:
         raise MidiError("a track header doesn't start with MTrk")
      r.pos += len(MTRK)
      size = r.big(U32_BYTES)
      out.tracks.append(r.pos)
      at = r.pos + size
   if out.tracks:
      r.pos, r.time, r.status = out.tracks[0], 0, 0
      out.sequence = read_sequence(r, factor, [0])
   else:
      raise MidiError("no tracks")
   return out


def merge(track: list[Event], sequence: list[Event]) -> list[Event]:
   """!Merges a track's events with the sequence events (the sequence's first at equal keys; the later end).
   @param track     the track's events (ending with EV_END)
   @param sequence  the sequence events (ending with EV_END)
   @return merged events
   """
   out: list[Event] = []
   i = j = 0
   while track[i].kind != EV_END and sequence[j].kind != EV_END:
      if before(track[i], sequence[j]):
         out.append(Event(**vars(track[i])))
         i += 1
      else:
         out.append(Event(**vars(sequence[j])))
         j += 1
   while track[i].kind != EV_END:
      out.append(Event(**vars(track[i])))
      i += 1
   while sequence[j].kind != EV_END:
      out.append(Event(**vars(sequence[j])))
      j += 1
   out.append(Event(**vars(sequence[j] if before(track[i], sequence[j]) else track[i])))
   return out


def convert_times(events: list[Event], division: int, opts: Options) -> None:
   """!File ticks to song ticks, note lengths and velocities (module notes).
   @param events    the merged events (changed in place)
   @param division  ticks per quarter note
   @param opts      options
   @return None
   """
   scale = CLOCKS_PER_BEAT * opts.factor
   for e in events:
      e.time = scale * e.time // division
      if e.kind == EV_NOTE:
         e.param1 = round_velocity(e.param1)
         length = max(1, scale * e.param2 // division)
         if not opts.exact_gate and length < WHOLE:
            length = round_down_length(length)
         e.param2 = length


def insert_marks(events: list[Event], factor: int, first_track: bool) -> list[Event]:
   """!Inserts the whole-note marks (module notes).
   @param events       the events
   @param factor       clocks factor
   @param first_track  this is the first output track
   @return the events with the marks
   """
   out: list[Event] = []
   mark = Event(0, EV_TIME_SIGNATURE, param2=WHOLE * factor)
   for e in events:
      while before(mark, e):
         out.append(Event(**vars(mark)))
         mark.time += mark.param2
      if e.kind == EV_TIME_SIGNATURE:
         if first_track and e.param2 != mark.param2:
            out.append(Event(e.time, EV_ORIGINAL_TIME_SIGNATURE, e.note, e.param1, e.param2))
         mark.param2 = e.param2
         mark.time = e.time + mark.param2
      out.append(e)
   return out


def hold_long_notes(events: list[Event]) -> list[Event]:
   """!Notes longer than a whole note become held notes and note-off events.
   @param events  the events
   @return the events with the note-offs
   """
   out: list[Event] = []
   for e in events:
      if e.kind == EV_NOTE and e.param2 > WHOLE:
         out.append(Event(e.time, EV_NOTE, e.note, e.param1, -1))
         out.append(Event(e.time + e.param2, EV_END_OF_TIE, e.note))
      else:
         out.append(e)
   return out


def split_gaps(events: list[Event]) -> list[Event]:
   """!Splits gaps into lengths of L (module notes).
   @param events  the ordered events
   @return the events with the time splits
   """
   out: list[Event] = []
   time = 0
   for e in events:
      gap = e.time - time
      if gap > WHOLE:
         wholes = (gap - 1) // WHOLE
         gap -= WHOLE * wholes
         for _ in range(wholes):
            time += WHOLE
            out.append(Event(time, EV_TIME_SPLIT))
      value = round_down_length(gap)
      if value != gap:
         out.append(Event(time + value, EV_TIME_SPLIT))
      time = e.time
      out.append(e)
   return out


def to_waits(events: list[Event]) -> int:
   """!Each event's time becomes the wait to the next; time signatures become numbered whole-note marks.
   @param events  the events (changed in place)
   @return the first event's time (the track's initial wait)
   """
   initial = events[0].time
   count = 0
   i = 0
   while i < len(events) and events[i].kind != EV_END:
      events[i].time = events[i + 1].time - events[i].time
      if events[i].kind == EV_TIME_SIGNATURE:
         events[i].kind, events[i].param2 = EV_WHOLE_NOTE_MARK, count
         count += 1
      i += 1
   return initial


def score(events: list[Event], index: int) -> int:
   """!A whole-note block's compression score (module notes, Compression (a)).
   @param events  the events
   @param index   the block's mark
   @return the score
   """
   total = 1 if events[index].time > 0 else 0
   last_param1, last_kind = events[index].param1, events[index].kind
   last_velocity, last_key, last_length = SCORE_VELOCITY, SCORE_KEY, SCORE_NO_DURATION
   i = index + 1
   while not boundary(events[i].kind):
      e = events[i]
      if e.kind == EV_NOTE:
         value = 0
         if e.note != last_key:
            value, last_key = value + 1, e.note
         if e.param1 != last_velocity:
            value, last_velocity = value + 1, e.param1
         lookup = HELD_LOOKUP if e.param2 < 0 else round_down_length(e.param2)
         if lookup != last_length:
            value, last_length = value + 1, lookup
         if e.param2 != last_length:
            value += 1
         total += max(value, 1)
      else:
         last_length = SCORE_NO_DURATION
         if e.kind == last_kind:
            plain = last_kind not in (EV_CONTROLLER, EV_UNUSED_KIND, EV_END_OF_TIE)
            total += SCORE_SAME if plain or e.param1 == last_param1 else SCORE_OTHER
         else:
            total += SCORE_OTHER
      last_param1, last_kind = e.param1, e.kind
      total += 1 if e.time else 0
      i += 1
   return total


def same_block(events: list[Event], first: int, other: int) -> bool:
   """!Whether a later block repeats a block (the first event after each mark is always compared).
   @param events  the events
   @param first   the first block's mark
   @param other   the later block's mark
   @return True when identical
   """
   a, b = events[first], events[other]
   if (a.kind, a.note, a.param1, a.time) != (b.kind, b.note, b.param1, b.time):
      return False
   first += 1
   other += 1
   while True:
      if other >= len(events) or vars(events[first]) != vars(events[other]):
         return False
      first += 1
      other += 1
      if boundary(events[first].kind):
         break
   return other < len(events) and boundary(events[other].kind)


def compress(events: list[Event]) -> None:
   """!Marks repeated blocks as patterns (module notes, Compression (a)).
   @param events  the events (changed in place)
   @return None
   """
   i = 0
   while events[i].kind != EV_END:
      if events[i].kind != EV_WHOLE_NOTE_MARK:
         i += 1
         continue
      if score(events, i) >= SCORE_MIN:
         j = i + 1
         while events[j].kind != EV_END:
            if events[j].kind == EV_WHOLE_NOTE_MARK and same_block(events, i, j):
               events[j].kind = EV_PATTERN
               events[j].param2 = events[i].param2 & (PATTERN_FLAG - 1)
               events[i].param2 |= PATTERN_FLAG
            j += 1
      i += 1


@dataclass
class TrackCode:
   """!One track's bytes with its labels and references (offsets within the track)."""

   ## The bytes.
   code: bytearray = field(default_factory=bytearray)
   ## Label -> offset.
   labels: dict[tuple[str, int], int] = field(default_factory=dict)
   ## (offset of a u16, label).
   refs: list[tuple[int, tuple[str, int]]] = field(default_factory=list)


class TrackWriter:
   """!Writes one track's commands (module notes, Commands and Compression (b))."""

   def __init__(self, opts: Options) -> None:
      """!A writer for one song's tracks (the extended command, the variable operation and the block number carry
      over from track to track, as in the reference).
      @param opts  options
      """
      ## Options.
      self.opts = opts
      ## The output.
      self.out = TrackCode()
      ## The last command written: its op name (a command byte, or a note name with its length).
      self.last_op = None
      ## Keep last_op over the next note.
      self.keep = False
      ## The last key written.
      self.last_key = -1
      ## The last velocity written.
      self.last_velocity = -1
      ## Whether the last note changed its key (the note-off rule).
      self.key_changed = False
      ## Inside a pattern's first copy.
      self.in_pattern = False
      ## The selected extended command and the stored variable operation, address and label.
      self.extended = 0
      ## The stored variable operation.
      self.var_op = 0
      ## The stored variable address.
      self.var_address = 0
      ## The stored variable label.
      self.var_label = 0
      ## The latest block number (a label's) and the latest loop start's.
      self.block = 0

   def reset(self) -> None:
      """!Forgets running status (labels, patterns).
      @return None
      """
      self.last_velocity = -1
      self.last_key = -1
      self.key_changed = False
      self.keep = False
      self.last_op = None
      self.in_pattern = False

   def emit(self, *values: int) -> None:
      """!Appends bytes.
      @param values  bytes (0..255)
      @return None
      """
      for v in values:
         if not 0 <= v <= BYTE_MAX:
            raise MidiError(f"byte value {v} out of range")
         self.out.code.append(v)

   def ref(self, label: tuple[str, int]) -> None:
      """!Appends a u16 reference to a label.
      @param label  (kind, number)
      @return None
      """
      self.out.refs.append((len(self.out.code), label))
      self.out.code.extend(bytes(U16_BYTES))

   def label(self, label: tuple[str, int]) -> None:
      """!Defines a label here.
      @param label  (kind, number)
      @return None
      """
      if label in self.out.labels:
         raise MidiError(f"label {label} defined twice")
      self.out.labels[label] = len(self.out.code)

   def wait(self, ticks: int) -> None:
      """!A wait (none for 0).
      @param ticks  the wait
      @return None
      """
      if ticks > 0:
         self.emit(WAIT + l_index(ticks))
         self.keep = self.key_changed = True

   def plain(self, *values: int) -> None:
      """!A non-running command (or a running one written in full without updating running status).
      @param values  its bytes
      @return None
      """
      self.emit(*values)
      self.keep = self.key_changed = True

   def op(self, wait: int, name: object, command: int, args: list[int] | None, full: bool = False) -> None:
      """!A running-set command: its byte is omitted when it repeats the last one (with compression and args).
      @param wait     the wait after it
      @param name     its op name (running status compares names)
      @param command  its byte
      @param args     its arguments, or None (then the byte is always written)
      @param full     always write the byte (running status is the same either way)
      @return None
      """
      if full or args is None or not self.opts.compress or self.last_op != name:
         self.emit(command)
         self.last_op = name
      self.emit(*(args or []))
      self.wait(wait)

   def note(self, e: Event) -> None:
      """!A note (or a held note).
      @param e  the event
      @return None
      """
      velocity = round_velocity(e.param1)
      length = -1 if e.param2 == -1 else round_down_length(e.param2)
      extra = e.param2 - length if self.opts.exact_gate and length != -1 else 0
      name = f"NOTE{length}"   # op names compare equal only for the same note length
      command = NOTE + (0 if length == -1 else l_index(length))
      key_changed = not self.opts.compress or e.note != self.last_key
      velocity_changed = not self.opts.compress or velocity != self.last_velocity
      if self.keep:
         self.keep = False
      else:
         self.last_op = None
      if key_changed or velocity_changed or extra > 0:
         w = self   # a local alias (Doxygen 1.12 reads this assignment as a new member)
         w.last_key = e.note
         args = [e.note]
         if velocity_changed or extra > 0:
            self.last_velocity = velocity
            args.append(velocity)
         if extra > 0:
            args.append(extra)
         self.op(e.time, name, command, args)
      else:
         self.op(e.time, name, command, None)
      self.key_changed = key_changed

   def note_off(self, e: Event) -> None:
      """!A note-off event.
      @param e  the event
      @return None
      """
      key_changed = e.note != self.last_key
      if not key_changed or not self.key_changed:
         self.last_op = None
      if not key_changed and self.opts.compress:
         self.op(e.time, NOTE_OFF, NOTE_OFF, None)
      else:
         self.last_key = e.note
         self.op(e.time, NOTE_OFF, NOTE_OFF, [e.note])
      self.key_changed = key_changed

   def block_label(self, e: Event) -> None:
      """!A label or loop marker's jump target.
      @param e  the event
      @return None
      """
      self.block = e.param1 + 1
      self.label(("block", self.block))
      self.wait(e.time)
      self.reset()

   def controller(self, e: Event) -> None:
      """!A controller (module notes).
      @param e  the event
      @return None
      """
      number, value = e.param1, e.param2
      if number in PLAIN_CONTROLLERS:
         self.op(e.time, PLAIN_CONTROLLERS[number], PLAIN_CONTROLLERS[number], [value])
      elif number == CC_VOLUME:
         self.op(e.time, VOLUME, VOLUME, [value * self.opts.volume // VOLUME_MAX])
      elif number in (CC_VAR_WRITE, CC_VAR_WRITE_2):
         if self.var_op <= VAR_LAST:
            self.plain(VAR, self.var_op, self.var_address, value)
            if self.var_op >= VAR_FIRST_JUMP:
               self.ref(("var", self.var_label))
         self.wait(e.time)
      elif number == CC_VAR_OP:
         self.var_op = value
         self.wait(e.time)
      elif number == CC_VAR_ADDRESS:
         self.var_address = value
         self.wait(e.time)
      elif number == CC_VAR_LABEL:
         self.var_label = value
         self.wait(e.time)
      elif number == CC_LABEL:
         self.label(("var", value))
         self.wait(e.time)
         self.reset()
      elif number in (CC_EXT_VALUE, CC_EXT_VALUE_2):
         if self.extended in (SELECT_ECHO_VOLUME, SELECT_ECHO_LENGTH):
            sub = EXT_ECHO_VOLUME if self.extended == SELECT_ECHO_VOLUME else EXT_ECHO_LENGTH
            self.op(e.time, EXT, EXT, [sub, value], full=True)   # EXT is always written in full
         else:
            self.wait(e.time)
      elif number == CC_EXT_SELECT:
         self.extended = value   # no wait (module notes)
      elif number in (CC_PRIORITY, CC_PRIORITY_2):
         self.plain(PRIORITY, value)
         self.wait(e.time)
      else:
         self.wait(e.time)

   def track(self, events: list[Event], initial_wait: int) -> TrackCode:
      """!Writes a track.
      @param events        its events (waits computed, compressed)
      @param initial_wait  the first event's time
      @return its code
      """
      self.out = TrackCode()
      self.reset()
      first = next((e for e in events if e.kind == EV_NOTE or (e.kind == EV_CONTROLLER and e.param1 == CC_VOLUME)),
                   None)
      if first is None or first.kind == EV_NOTE:
         self.plain(VOLUME, VOLUME_MAX * self.opts.volume // VOLUME_MAX)   # no volume before the first note
      self.wait(initial_wait)
      self.plain(TRANSPOSE, 0)
      loop_block = 0
      i = 0
      while events[i].kind != EV_END:
         e = events[i]
         if boundary(e.kind):
            if self.in_pattern:
               self.plain(RETURN)
            self.in_pattern = False
         if e.kind == EV_NOTE:
            self.note(e)
         elif e.kind == EV_END_OF_TIE:
            self.note_off(e)
         elif e.kind == EV_LABEL:
            self.block_label(e)
         elif e.kind in (EV_LOOP_END, EV_LOOP_END_BEGIN):
            self.plain(JUMP)
            self.ref(("block", loop_block))
            self.block_label(e)
            if e.kind == EV_LOOP_END_BEGIN:
               loop_block = self.block
         elif e.kind == EV_LOOP_BEGIN:
            self.block_label(e)
            loop_block = self.block
         elif e.kind == EV_WHOLE_NOTE_MARK:
            if e.param2 & PATTERN_FLAG:
               self.label(("pattern", e.param2 & (PATTERN_FLAG - 1)))
               self.reset()
               self.in_pattern = True
            self.wait(e.time)
         elif e.kind == EV_PATTERN:
            self.plain(CALL)
            self.ref(("pattern", e.param2))
            while not boundary(events[i + 1].kind):
               i += 1
            self.reset()
         elif e.kind == EV_TEMPO:
            value = tempo_bpm(e.param2) * self.opts.factor // TEMPO_DIVISOR
            self.plain(TEMPO, value)
            self.wait(e.time)
         elif e.kind == EV_PROGRAM:
            self.op(e.time, PROGRAM, PROGRAM, [e.param1])
         elif e.kind == EV_BEND:
            self.op(e.time, BEND, BEND, [e.param2])
         elif e.kind == EV_CONTROLLER:
            self.controller(e)
         else:
            self.wait(e.time)
         i += 1
      self.plain(END)
      return self.out


@dataclass
class Song:
   """!A converted song (the compiled form of §2.5)."""

   ## Its name (the asset key is song:NAME).
   name: str
   ## The instrument set's key.
   instruments: str
   ## Priority.
   priority: int
   ## Reverb or None.
   reverb: int | None
   ## The command bytes of every track.
   data: bytes
   ## Each track's start offset.
   tracks: list[int]
   ## Each track's (MIDI track, channel).
   sources: list[tuple[int, int]] = field(default_factory=list)

   def entry(self, data_path: str, key: str | None = None) -> dict[str, object]:
      """!The manifest entry ("songs" item).
      @param data_path  the .seq file, relative to the manifest
      @param key        the asset key (default song:NAME)
      @return the entry
      """
      s = self   # a local alias (Doxygen reads some uses of self.x as new members)
      return {"key": key or f"song:{s.name}", "instruments": s.instruments, "priority": s.priority,
              "reverb": s.reverb, "data": data_path, "tracks": list(s.tracks)}


def assemble(codes: list[TrackCode]) -> tuple[bytes, list[int]]:
   """!Lays the tracks out one after the other and resolves their references.
   @param codes  the tracks
   @return (song data, track offsets)
   @throws MidiError for an undefined label or data past 64 KiB
   """
   data = bytearray()
   offsets: list[int] = []
   for code in codes:
      base = len(data)
      offsets.append(base)
      body = bytearray(code.code)
      for at, label in code.refs:
         if label not in code.labels:
            raise MidiError(f"track {len(offsets)}: undefined label {label}")
         body[at:at + U16_BYTES] = (base + code.labels[label]).to_bytes(U16_BYTES, "little")
      data.extend(body)
   if len(data) > U16_LIMIT:
      raise MidiError(f"{len(data)} bytes of song data (at most {U16_LIMIT})")
   return bytes(data), offsets


def convert(data: bytes, opts: Options, name: str = "song") -> Song:
   """!Converts a MIDI file (module notes).
   @param data  the file
   @param opts  options
   @param name  the song's name when the options give none
   @return the song
   @throws MidiError when it cannot be converted
   """
   if not 0 <= opts.priority <= BYTE_MAX or not 0 <= opts.volume <= BYTE_MAX:
      raise MidiError("priority and volume must be 0-255")
   if opts.reverb is not None and not 0 <= opts.reverb <= REVERB_MAX:
      raise MidiError(f"reverb must be 0-{REVERB_MAX}")
   f = read_file(data, opts.factor)
   r = Reader(data)
   sequence = f.sequence
   writer = TrackWriter(opts)
   codes: list[TrackCode] = []
   sources: list[tuple[int, int]] = []
   for number, start in enumerate(f.tracks):
      for channel in range(CHANNELS):
         events, notes = read_track(r, start, channel)
         if not notes:
            continue
         first = not codes
         merged = merge(events, sequence)
         if first:
            sequence = [e for e in sequence if e.kind != EV_TEMPO]   # tempo only in the first output track
         convert_times(merged, f.division, opts)
         merged = insert_marks(merged, opts.factor, first)
         merged = hold_long_notes(merged)
         merged.sort(key=Event.key)   # stable, as the reference's
         merged = split_gaps(merged)
         initial = to_waits(merged)
         if opts.compress:
            compress(merged)
         codes.append(writer.track(merged, initial))
         sources.append((number, channel))
   if len(codes) > MAX_TRACKS:
      raise MidiError(f"{len(codes)} tracks (at most {MAX_TRACKS})")
   song_data, offsets = assemble(codes)
   return Song(opts.name or name, opts.instruments, opts.priority, opts.reverb, song_data, offsets, sources)


## Bytes of arguments after each fixed-size command (NOTE, NOTE_OFF, VAR and EXT are read by the walker).
FIXED_ARGS = {END: 0, JUMP: U16_BYTES, CALL: U16_BYTES, RETURN: 0, REPEAT: 1 + U16_BYTES, PRIORITY: 1, TEMPO: 1,
              TRANSPOSE: 1, PROGRAM: 1, VOLUME: 1, PAN: 1, BEND: 1, BEND_RANGE: 1, LFO_SPEED: 1, LFO_DELAY: 1,
              MOD_DEPTH: 1, MOD_TYPE: 1, FINE_TUNE: 1}
## Command names for listings.
NAMES = {END: "END", JUMP: "JUMP", CALL: "CALL", RETURN: "RETURN", REPEAT: "REPEAT", VAR: "VAR",
         PRIORITY: "PRIORITY", TEMPO: "TEMPO", TRANSPOSE: "TRANSPOSE", PROGRAM: "PROGRAM", VOLUME: "VOLUME",
         PAN: "PAN", BEND: "BEND", BEND_RANGE: "BEND_RANGE", LFO_SPEED: "LFO_SPEED", LFO_DELAY: "LFO_DELAY",
         MOD_DEPTH: "MOD_DEPTH", MOD_TYPE: "MOD_TYPE", FINE_TUNE: "FINE_TUNE", EXT: "EXT", NOTE_OFF: "NOTE_OFF"}


@dataclass
class Step:
   """!One command a track executes."""

   ## When.
   tick: int
   ## Its offset in the song data (the command byte, or its first argument byte).
   offset: int
   ## The command byte (also when running status omitted it).
   command: int
   ## Its argument bytes (u16 arguments as one value).
   args: list[int]


@dataclass
class TrackRun:
   """!What one track does when it plays alone (walk())."""

   ## Commands executed, in order.
   steps: list[Step] = field(default_factory=list)
   ## The tick END ran at (None: it loops or runs on).
   end_tick: int | None = None
   ## The tick of the first backward JUMP (a loop).
   loop_tick: int | None = None
   ## That JUMP's target offset.
   loop_to: int | None = None
   ## Deepest CALL nesting.
   max_depth: int = 0


def walk(data: bytes, start: int, tick_limit: int = WALK_TICK_LIMIT) -> TrackRun:
   """!Runs one track's commands alone (§3 semantics for control flow and waits; the variable area starts at 0 and
   only this track writes it), up to its END, its first backward JUMP or tick_limit.
   @param data        the song data
   @param start       the track's offset
   @param tick_limit  ticks to give up after
   @return the run
   @throws MidiError for bytes that are not commands
   """
   run = TrackRun()
   pos, tick, running = start, 0, 0
   stack: list[int] = []
   area = [0] * VAR_AREA
   repeat = 0

   def arg() -> int:
      """!The next argument byte.
      @return it
      """
      nonlocal pos
      if pos >= len(data):
         raise MidiError("a track runs past the song data")
      pos += 1
      return data[pos - 1]

   def word() -> int:
      """!The next u16 argument.
      @return it
      """
      return arg() | (arg() << BYTE_BITS)

   def optional() -> int | None:
      """!The next byte if it is an argument (below 0x80).
      @return it, or None
      """
      return arg() if pos < len(data) and data[pos] < ARG_LIMIT else None

   while tick <= tick_limit:
      at = pos
      byte = arg()
      if byte < ARG_LIMIT:
         if not running:
            raise MidiError(f"offset {at}: an argument without a running command")
         command, pos = running, at
      else:
         command = byte
         if NOTE <= command < WAIT or command in RUNNING:
            running = command
      if WAIT <= command < END:
         run.steps.append(Step(tick, at, command, []))
         tick += LENGTHS[command - WAIT]
         continue
      args: list[int] = []
      if NOTE <= command < WAIT:
         for _ in range(VAR_ARGS):
            value = optional()
            if value is None:
               break
            args.append(value)
      elif command == NOTE_OFF:
         value = optional()
         args = [] if value is None else [value]
      elif command == VAR:
         args = [arg() for _ in range(VAR_ARGS)]
         if args[0] >= VAR_FIRST_JUMP:
            args.append(word())
      elif command == EXT:
         sub = arg()
         size = U16_BYTES if sub == EXT_HOLD else U32_BYTES if sub == EXT_START else 1
         args = [sub, int.from_bytes(bytes(arg() for _ in range(size)), "little")]
      elif command in (JUMP, CALL):
         args = [word()]
      elif command == REPEAT:
         args = [arg(), word()]
      elif command in FIXED_ARGS:
         args = [arg() for _ in range(FIXED_ARGS[command])]
      run.steps.append(Step(tick, at, command, args))
      if command >= RESERVED or command == END or (command == CALL and len(stack) >= CALL_DEPTH):
         run.end_tick = tick
         return run
      if command == JUMP:
         if args[0] <= at:
            run.loop_tick, run.loop_to = tick, args[0]
            return run
         pos = args[0]
      elif command == CALL:
         stack.append(pos)
         run.max_depth = max(run.max_depth, len(stack))
         pos = args[0]
      elif command == RETURN and stack:
         pos = stack.pop()
      elif command == REPEAT:
         repeat += 1
         if args[0] == 0 or repeat < args[0]:
            pos = args[1]
         else:
            repeat = 0
      elif command == VAR:
         pos = var_step(area, args, pos)
   return run


def var_step(area: list[int], args: list[int], pos: int) -> int:
   """!One VAR command on the variable area (reads outside it give 0, writes outside it are dropped). Operations:
   0-2 set / add / subtract the value, 3-5 the same with the area byte the value names; 6-11 jump when the address's
   byte is equal / not equal / above / at least / at most / below the value, 12-17 the same against the area byte.
   @param area  the 16-byte area
   @param args  operation, address, value [, target]
   @param pos   the position after the command
   @return the next position
   """
   op, address, value = args[:VAR_ARGS]
   if op > VAR_LAST:
      return pos
   from_area = VAR_FIRST_FROM_AREA <= op < VAR_FIRST_JUMP or op >= VAR_FIRST_JUMP + VAR_JUMP_KINDS
   operand = var_get(area, value) if from_area else value
   left = var_get(area, address)
   if op < VAR_FIRST_JUMP:
      kind = op % VAR_FIRST_FROM_AREA
      result = (operand, left + operand, left - operand)[kind]
      if 0 <= address < VAR_AREA:
         area[address] = result & BYTE_MAX
      return pos
   test = (op - VAR_FIRST_JUMP) % VAR_JUMP_KINDS
   taken = (left == operand, left != operand, left > operand, left >= operand, left <= operand, left < operand)[test]
   return args[VAR_ARGS] if taken else pos


def var_get(area: list[int], index: int) -> int:
   """!A variable area byte.
   @param area   the area
   @param index  its index
   @return its value (0 outside the area)
   """
   return area[index] if 0 <= index < VAR_AREA else 0


def run_song(song: Song, tick_limit: int = WALK_TICK_LIMIT) -> list[TrackRun]:
   """!Walks every track (walk()).
   @param song        the song
   @param tick_limit  ticks to give up after
   @return each track's run
   """
   return [walk(song.data, start, tick_limit) for start in song.tracks]


def frames_to_end(runs: list[TrackRun]) -> int | None:
   """!The sequencer's frames from the start until the frame whose ticks run the last END: one frame adds the tempo
   (2 x the TEMPO argument; 150 at the start) to a counter and plays a tick per 150 it holds; a TEMPO command run
   in a tick counts from the next frame (tracks run in order within a tick).
   @param runs  the tracks' runs
   @return frames (1 for a song without tracks: its first frame finds none), or None when a track never ends
   """
   if not runs:
      return 1
   if any(r.end_tick is None for r in runs):
      return None
   end = max(r.end_tick or 0 for r in runs)
   changes = sorted((s.tick, n, k, s.args[0]) for n, r in enumerate(runs) for k, s in enumerate(r.steps)
                    if s.command == TEMPO)
   tempo, counter, tick, frame, at = START_TEMPO, 0, 0, 0, 0
   while True:
      frame += 1
      counter += tempo
      while counter >= TICK_THRESHOLD:
         counter -= TICK_THRESHOLD
         while at < len(changes) and changes[at][0] <= tick:
            tempo = changes[at][-1] * TEMPO_DIVISOR
            at += 1
         if tick >= end:
            return frame
         tick += 1
      if tempo == 0 and counter < TICK_THRESHOLD:
         return None


def listing(song: Song) -> str:
   """!Each track's commands with their ticks (a debugging view).
   @param song  the song
   @return the text
   """
   lines: list[str] = []
   for n, run in enumerate(run_song(song)):
      lines.append(f"track {n} (MIDI track {song.sources[n][0]}, channel {song.sources[n][1]}):")
      for s in run.steps:
         if NOTE <= s.command < WAIT:
            name = f"NOTE {LENGTHS[s.command - NOTE]}"
         elif WAIT <= s.command < END:
            name = f"WAIT {LENGTHS[s.command - WAIT]}"
         else:
            name = NAMES.get(s.command, f"0x{s.command:02X}")
         lines.append(f"   {s.tick:6d} @{s.offset:05d} {name} {' '.join(str(a) for a in s.args)}".rstrip())
      ending = f"end {run.end_tick}" if run.end_tick is not None else f"loops at {run.loop_tick} to @{run.loop_to}"
      lines.append(f"   {ending}")
   return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
   """!Command line (module notes).
   @param argv  arguments (default: sys.argv[1:])
   @return exit status
   """
   words = list(sys.argv[1:] if argv is None else argv)
   files: list[str] = []
   options: list[str] = []
   show = False
   i = 0
   while i < len(words):
      word = words[i]
      i += 1
      if word == "--listing":
         show = True
      elif word in ("-h", "--help"):
         print(__doc__)
         return 0
      elif word.startswith("-") and len(word) > 1:
         options.append(word)
         if len(word) == U16_BYTES and word[1].upper() in "GLPRV" and i < len(words):
            options.append(words[i])
            i += 1
      else:
         files.append(word)
   if not files or (not show and len(files) != U16_BYTES):
      print("usage: midi_song.py IN.mid OUT.json [options] | IN.mid --listing [options]", file=sys.stderr)
      return 1
   source = Path(files[0])
   try:
      song = convert(source.read_bytes(), parse_options(options), source.stem)
   except (OSError, MidiError) as e:
      print(f"midi_song: {e}", file=sys.stderr)
      return 1
   if show:
      sys.stdout.write(listing(song))
      return 0
   out = Path(files[1])
   seq = out.with_suffix(".seq")
   seq.write_bytes(song.data)
   out.write_text(json.dumps(song.entry(seq.name), indent=1) + "\n", encoding="utf-8", newline="\n")
   return 0


if __name__ == "__main__":
   sys.exit(main())
