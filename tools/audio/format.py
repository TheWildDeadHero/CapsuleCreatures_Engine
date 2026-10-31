#!/usr/bin/env python3
"""!The engine's audio format (D-34, M4 B1a; format document sections 2 and 3): the capsule.audio/1 manifest schema
and its validator, and the song command stream (.seq) with a one-mnemonic-per-line text form (assembler and
disassembler).

Manifest (`audio.json`; paths relative to it): mixer, players, song_table, samples, waves, keymaps, voice_banks,
instrument_sets, songs, clips (format section 2). load() reads and checks one and returns an AudioSet the compiler
(tools/audio_compiler.py) packs; validate() returns the problems without raising.

Command stream (format section 3): bytes below 0x80 are arguments; at a command position an argument byte repeats
the track's running command (NOTE, PROGRAM .. NOTE_OFF). The text form, one command per line:

    TRACK                      a new track starts here
    @loop:                     a label (jump / call target)
    NOTE 24 c4 v100 +2         NOTE <ticks> [key [vVELOCITY [+EXTRA]]] (ticks 0: held until NOTE_OFF)
    RUN 62 v90                 the running command again with these arguments (no command byte)
    WAIT 24                    END, JUMP @l, CALL @l, RETURN, REPEAT n @l, VAR op addr data [@l]
    PRIORITY v, TEMPO v, TRANSPOSE s, PROGRAM n, VOLUME v, PAN v, BEND v, BEND_RANGE v, LFO_SPEED v, LFO_DELAY v,
    MOD_DEPTH v, MOD_TYPE v, FINE_TUNE v, NOTE_OFF [key]
    ECHO_VOLUME v, ECHO_LENGTH v, RELEASE v, HOLD n, START_OFFSET n, ATTACK v, DECAY v, SUSTAIN v, TONE_LENGTH v,
    SWEEP v                    the EXT sub-commands
    BYTES 0x80 60              raw bytes (tests of edge cases)

Keys are numbers or names (c4 = 60, cs4 / c#4 = 61, c-1 = 0). Values are raw bytes (PAN 64 is the centre).
Comments start with `#` or `;`.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

## Manifest schema id.
SCHEMA = "capsule.audio/1"
## The mixing rates (index = rate step).
RATES = (5734, 7884, 10512, 13379, 15768, 18157, 21024, 26758, 31536, 36314, 40137, 42048)
## Sampled channels.
MIN_CHANNELS, MAX_CHANNELS = 1, 12
## Master volume.
MAX_MASTER_VOLUME = 15
## DAC bits.
MIN_DAC_BITS, MAX_DAC_BITS = 6, 9
## Players in a table (the engine's kMaxPlayers).
MAX_PLAYERS = 8
## Tracks of one player and of a song.
MAX_TRACKS = 16
## Tracks of every player together, the two clip players' 4 included (the engine's kMaxTracks).
MAX_ALL_TRACKS = 40
## Clip players' tracks.
CLIP_TRACKS = 4
## Sound numbers (u16).
MAX_SOUNDS = 0xFFFF
## Voices in an instrument set window.
SET_VOICES = 128
## Keymap entries.
KEYMAP_ENTRIES = 128
## Wave pattern nibbles.
WAVE_NIBBLES = 32
## Highest byte value.
BYTE_MAX = 0xFF
## Highest argument byte.
ARG_MAX = 0x7F
## Highest u16 / u32.
U16_MAX, U32_MAX = 0xFFFF, 0xFFFFFFFF
## Envelope maxima of tone voices: attack / decay / release 3 bits, sustain 4 bits.
TONE_ENV_MAX, TONE_SUSTAIN_MAX = 7, 15
## Square duty values.
MAX_DUTY = 3
## Tone length.
MAX_TONE_LENGTH = 63
## Noise periods: 0 15-bit, 1 7-bit.
MAX_NOISE_PERIOD = 1
## Song priority / reverb.
MAX_PRIORITY, MAX_REVERB = 255, 127
## Sample formats.
SAMPLE_FORMATS = ("pcm8", "dpcm4")
## The entry of check_voice()'s key sets that lists the DPCM samples (not a key kind of the manifest).
DPCM_KEYS = "dpcm4 samples"
## DPCM block: samples and bytes.
DPCM_BLOCK_SAMPLES, DPCM_BLOCK_BYTES = 64, 33
## Defaults: a voice's key, the mixer's rate, channels, master volume and DAC bits.
DEFAULT_KEY, DEFAULT_RATE, DEFAULT_CHANNELS, DEFAULT_MASTER_VOLUME, DEFAULT_DAC_BITS = 60, 13379, 5, 12, 8
## VAR's argument counts: without and with a target.
VAR_ARGS, VAR_JUMP_ARGS = 3, 4
## Asset key kinds of the manifest.
KINDS = ("song", "instruments", "keymap", "sample", "wave", "clip", "bank")

## Voice kinds and their engine values (rom::VoiceKind).
VOICE_KINDS = {"none": 0, "sample": 1, "square_sweep": 2, "square": 3, "wave": 4, "noise": 5, "split": 6, "drums": 7}
## Voice::kind flags.
FLAG_FIXED_RATE, FLAG_REVERSE, FLAG_EVEN_PITCH = 0x10, 0x20, 0x40
## Tone kinds.
TONE_KINDS = ("square_sweep", "square", "wave", "noise")

# -- the command stream ------------------------------------------------------------------------------------------------

## NOTE / WAIT lengths (ticks), index = the command's offset.
LENGTHS = tuple(list(range(25)) + [n for n in range(25, 97) if n % 4 == 0 or n % 6 == 0])
## NOTE 0x80 + i, WAIT 0xB1 + i.
NOTE_BASE, WAIT_BASE = 0x80, 0xB1
## The last NOTE / WAIT bytes.
NOTE_LAST, WAIT_LAST = NOTE_BASE + len(LENGTHS) - 1, WAIT_BASE + len(LENGTHS) - 1
## Single-byte commands with fixed arguments: mnemonic -> (byte, argument kinds). Kinds: b byte, s signed byte,
## a u16 target, r REPEAT count, n u16 value, w u32 value.
COMMANDS: dict[str, tuple[int, str]] = {
   "END": (0xE2, ""), "JUMP": (0xE3, "a"), "CALL": (0xE4, "a"), "RETURN": (0xE5, ""), "REPEAT": (0xE6, "ba"),
   "VAR": (0xE7, ""), "PRIORITY": (0xE8, "b"), "TEMPO": (0xE9, "b"), "TRANSPOSE": (0xEA, "s"),
   "PROGRAM": (0xEB, "b"), "VOLUME": (0xEC, "b"), "PAN": (0xED, "b"), "BEND": (0xEE, "b"),
   "BEND_RANGE": (0xEF, "b"), "LFO_SPEED": (0xF0, "b"), "LFO_DELAY": (0xF1, "b"), "MOD_DEPTH": (0xF2, "b"),
   "MOD_TYPE": (0xF3, "b"), "FINE_TUNE": (0xF4, "b"),
}
## EXT.
EXT_BYTE = 0xF5
## NOTE_OFF.
NOTE_OFF_BYTE = 0xF6
## EXT sub-commands: mnemonic -> (sub, argument kind).
EXT_COMMANDS: dict[str, tuple[int, str]] = {
   "ECHO_VOLUME": (0, "b"), "ECHO_LENGTH": (1, "b"), "RELEASE": (2, "b"), "HOLD": (3, "n"), "START_OFFSET": (4, "w"),
   "ATTACK": (5, "b"), "DECAY": (6, "b"), "SUSTAIN": (7, "b"), "TONE_LENGTH": (8, "b"), "SWEEP": (9, "b"),
}
## The running set starts at PROGRAM (NOTE bytes are in it too).
RUNNING_FIRST = 0xEB
## The first reserved byte.
RESERVED_FIRST = 0xF7
## Bytes at or above this start a command.
COMMAND_FIRST = 0x80
## VAR operations; the first that jumps.
VAR_OPS, VAR_FIRST_JUMP = 18, 6
## VAR area bytes.
VAR_BYTES = 16
## NOTE optional arguments.
NOTE_ARGS = 3
## Bytes of a u16 / u32.
U16_BYTES, U32_BYTES = 2, 4
## Bits per byte.
BYTE_BITS = 8
## Note names.
NOTE_NAMES = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}
## Octave of key 0 ("c-1").
KEY_OCTAVE_BASE = 1
## Keys.
MAX_KEY = 127
## Notes per octave.
OCTAVE = 12

## Byte -> mnemonic.
BYTE_NAMES = {b: name for name, (b, _) in COMMANDS.items()}
## EXT sub -> mnemonic.
EXT_NAMES = {sub: name for name, (sub, _) in EXT_COMMANDS.items()}

## A key name's groups: sharp, octave.
KEY_SHARP_GROUP, KEY_OCTAVE_GROUP = 2, 3
## NOTE argument positions: velocity, gate extra.
VELOCITY_ARG, EXTRA_ARG = 1, 2
## A key name.
KEY_RE = re.compile(r"^([a-gA-G])([s#]?)(-?\d+)$")


class FormatError(Exception):
   """!A malformed manifest or song text."""


def is_running(b: int) -> bool:
   """!Whether a command byte becomes the running command.
   @param b  the command byte (>= 0x80)
   @return True for NOTE and PROGRAM .. NOTE_OFF
   """
   return NOTE_BASE <= b <= NOTE_LAST or RUNNING_FIRST <= b < RESERVED_FIRST


def parse_key(text: str, where: str) -> int:
   """!A key: a number or a name (c4 = 60).
   @param text   the token
   @param where  location for errors
   @return 0-127
   @throws FormatError when invalid
   """
   m = KEY_RE.match(text)
   if m:
      octave, name, sharp = int(m.group(KEY_OCTAVE_GROUP)), m.group(1).lower(), 1 if m.group(KEY_SHARP_GROUP) else 0
      key = (octave + KEY_OCTAVE_BASE) * OCTAVE + NOTE_NAMES[name] + sharp
   else:
      key = parse_int(text, where)
   if not 0 <= key <= MAX_KEY:
      raise FormatError(f"{where}: key {text!r} out of 0-{MAX_KEY}")
   return key


def parse_int(text: str, where: str) -> int:
   """!An integer (decimal, 0x hex, negative allowed).
   @param text   the token
   @param where  location for errors
   @return the value
   @throws FormatError when invalid
   """
   try:
      return int(text, 0)
   except ValueError as e:
      raise FormatError(f"{where}: {text!r} is not a number") from e


def key_name(key: int) -> str:
   """!A key's name.
   @param key  0-127
   @return e.g. "c4", "cs4"
   """
   names = ["c", "cs", "d", "ds", "e", "f", "fs", "g", "gs", "a", "as", "b"]
   return f"{names[key % OCTAVE]}{key // OCTAVE - KEY_OCTAVE_BASE}"


@dataclass
class Assembled:
   """!An assembled song: its bytes and its tracks' start offsets."""

   ## The command bytes.
   data: bytes
   ## Track start offsets.
   tracks: list[int]


def _byte(v: int, where: str, lo: int = 0, hi: int = BYTE_MAX) -> int:
   """!Checks a byte value.
   @param v      the value
   @param where  location for errors
   @param lo     minimum
   @param hi     maximum
   @return v
   @throws FormatError out of range
   """
   if not lo <= v <= hi:
      raise FormatError(f"{where}: {v} out of {lo}-{hi}")
   return v


def _length_index(ticks: int, where: str) -> int:
   """!A NOTE / WAIT length's index.
   @param ticks  the length
   @param where  location for errors
   @return its index in LENGTHS
   @throws FormatError for a length not in the table
   """
   if ticks not in LENGTHS:
      raise FormatError(f"{where}: {ticks} ticks is not a length ({', '.join(map(str, LENGTHS))})")
   return LENGTHS.index(ticks)


def _note_args(tokens: list[str], where: str) -> list[int]:
   """!NOTE / RUN arguments: [key [vVEL [+EXTRA]]].
   @param tokens  the tokens after the length (or after RUN)
   @param where   location for errors
   @return the argument bytes
   @throws FormatError when malformed
   """
   out: list[int] = []
   for i, tok in enumerate(tokens):
      if i >= NOTE_ARGS:
         raise FormatError(f"{where}: at most key, velocity and gate extra")
      if i == 0:
         out.append(parse_key(tok, where))
      elif i == 1:
         out.append(_byte(parse_int(tok[1:] if tok.startswith("v") else tok, where), where, 0, ARG_MAX))
      else:
         out.append(_byte(parse_int(tok[1:] if tok.startswith("+") else tok, where), where, 0, ARG_MAX))
   return out


def assemble(text: str, where: str = "song") -> Assembled:
   """!Assembles the text form.
   @param text   the song text
   @param where  location for errors
   @return its bytes and track offsets
   @throws FormatError when malformed
   """
   out = bytearray()
   tracks: list[int] = []
   labels: dict[str, int] = {}
   fixups: list[tuple[int, str, str]] = []
   running: int | None = None
   for n, raw in enumerate(text.splitlines(), 1):
      w = f"{where}:{n}"
      line = re.split(r"[#;]", raw, maxsplit=1)[0].strip()
      if not line:
         continue
      if line.startswith("@") and line.endswith(":"):
         name = line[1:-1]
         if name in labels:
            raise FormatError(f"{w}: label @{name} defined twice")
         labels[name] = len(out)
         continue
      tok = line.split()
      op = tok[0].upper()
      args = tok[1:]

      def target(t: str) -> None:
         """!A u16 jump target (a label, fixed up at the end).
         @param t  the token
         """
         if not t.startswith("@"):
            raise FormatError(f"{w}: a target is @label, got {t!r}")
         fixups.append((len(out), t[1:], w))
         out.extend(b"\0\0")

      if op == "TRACK":
         tracks.append(len(out))
         running = None
      elif op == "NOTE":
         if not args:
            raise FormatError(f"{w}: NOTE <ticks> [key [vVEL [+EXTRA]]]")
         b = NOTE_BASE + _length_index(parse_int(args[0], w), w)
         out.append(b)
         out.extend(_note_args(args[1:], w))
         running = b
      elif op == "WAIT":
         if len(args) != 1:
            raise FormatError(f"{w}: WAIT <ticks>")
         out.append(WAIT_BASE + _length_index(parse_int(args[0], w), w))
      elif op == "RUN":
         if running is None:
            raise FormatError(f"{w}: RUN with no running command before it")
         if not args:
            raise FormatError(f"{w}: RUN needs arguments")
         if NOTE_BASE <= running <= NOTE_LAST:
            out.extend(_note_args(args, w))
         elif running == NOTE_OFF_BYTE:
            out.append(parse_key(args[0], w))
         else:
            out.extend(_byte(parse_int(a, w), w, 0, ARG_MAX) for a in args)
      elif op == "BYTES":
         out.extend(_byte(parse_int(a, w), w) for a in args)
      elif op == "NOTE_OFF":
         out.append(NOTE_OFF_BYTE)
         if args:
            out.append(parse_key(args[0], w))
         running = NOTE_OFF_BYTE
      elif op == "VAR":
         if len(args) not in (VAR_ARGS, VAR_JUMP_ARGS):
            raise FormatError(f"{w}: VAR op addr data [@target]")
         opv = _byte(parse_int(args[0], w), w, 0, VAR_OPS - 1)
         out.extend([COMMANDS["VAR"][0], opv, _byte(parse_int(args[1], w), w, 0, VAR_BYTES - 1),
                     _byte(parse_int(args[VAR_ARGS - 1], w), w)])
         if (opv >= VAR_FIRST_JUMP) != (len(args) == VAR_JUMP_ARGS):
            raise FormatError(f"{w}: VAR ops {VAR_FIRST_JUMP}-{VAR_OPS - 1} (and only they) take a target")
         if len(args) == VAR_JUMP_ARGS:
            target(args[VAR_ARGS])
      elif op in EXT_COMMANDS:
         sub, kind = EXT_COMMANDS[op]
         if len(args) != 1:
            raise FormatError(f"{w}: {op} <value>")
         out.extend([EXT_BYTE, sub])
         v = parse_int(args[0], w)
         if kind == "b":
            out.append(_byte(v, w))
         else:
            size = U16_BYTES if kind == "n" else U32_BYTES
            _byte(v, w, 0, U16_MAX if kind == "n" else U32_MAX)
            out.extend(v.to_bytes(size, "little"))
         running = EXT_BYTE
      elif op in COMMANDS:
         b, kinds = COMMANDS[op]
         if len(args) != len(kinds):
            raise FormatError(f"{w}: {op} takes {len(kinds)} argument(s)")
         out.append(b)
         for k, a in zip(kinds, args):
            if k == "a":
               target(a)
            elif k == "s":
               out.append(_byte(parse_int(a, w), w, -(ARG_MAX + 1), ARG_MAX) & BYTE_MAX)
            else:
               out.append(_byte(parse_int(a, w), w, 0, ARG_MAX if op == "PROGRAM" else BYTE_MAX))
         if is_running(b):
            running = b
      else:
         raise FormatError(f"{w}: unknown command {tok[0]!r}")
   for at, name, w in fixups:
      if name not in labels:
         raise FormatError(f"{w}: unknown label @{name}")
      out[at:at + U16_BYTES] = labels[name].to_bytes(U16_BYTES, "little")
   if not tracks and out:
      tracks = [0]
   if len(tracks) > MAX_TRACKS:
      raise FormatError(f"{where}: {len(tracks)} tracks (at most {MAX_TRACKS})")
   if len(out) > U16_MAX:
      raise FormatError(f"{where}: {len(out)} bytes (targets are u16)")
   return Assembled(bytes(out), tracks)


@dataclass
class Decoded:
   """!One decoded command of a linear walk."""

   ## Its offset.
   at: int
   ## Its length in bytes.
   size: int
   ## The text line (without labels).
   text: str
   ## Jump target, if any.
   target: int | None = None
   ## Problem found, if any.
   error: str | None = None


def decode(data: bytes) -> list[Decoded]:
   """!Walks the bytes linearly as the sequencer reads them (running status from the previous command byte).
   @param data  the command bytes
   @return every command
   """
   out: list[Decoded] = []
   i = 0
   running: int | None = None

   def u16(at: int) -> int:
      """!A little-endian u16 (0 past the end).
      @param at  its offset
      @return the value
      """
      return int.from_bytes(data[at:at + U16_BYTES].ljust(U16_BYTES, b"\0"), "little")

   while i < len(data):
      start = i
      b = data[i]
      cmd = b
      run = False
      if b < COMMAND_FIRST:
         if running is None:
            out.append(Decoded(i, 1, f"BYTES {b}", error="argument byte with no running command"))
            i += 1
            continue
         cmd = running
         run = True
      else:
         i += 1
         if is_running(b):
            running = b
      name = ""
      tgt = None
      err = None
      if NOTE_BASE <= cmd <= NOTE_LAST:
         args = []
         while len(args) < NOTE_ARGS and i < len(data) and data[i] < COMMAND_FIRST:
            args.append(data[i])
            i += 1
         parts = [key_name(args[0])] if args else []
         if len(args) > 1:
            parts.append(f"v{args[1]}")
         if len(args) > NOTE_ARGS - 1:
            parts.append(f"+{args[EXTRA_ARG]}")
         name = ("RUN " if run else f"NOTE {LENGTHS[cmd - NOTE_BASE]} ") + " ".join(parts)
      elif WAIT_BASE <= cmd <= WAIT_LAST:
         name = f"WAIT {LENGTHS[cmd - WAIT_BASE]}"
      elif cmd == NOTE_OFF_BYTE:
         key = None
         if i < len(data) and data[i] < COMMAND_FIRST:
            key = data[i]
            i += 1
         name = ("RUN " if run else "NOTE_OFF ") + (key_name(key) if key is not None else "")
      elif cmd == EXT_BYTE:
         sub = data[i] if i < len(data) else None
         i += 1
         if sub not in EXT_NAMES:
            err = f"unknown EXT sub-command {sub}"
            name = f"BYTES {EXT_BYTE} {sub}"
         else:
            kind = EXT_COMMANDS[EXT_NAMES[sub]][1]
            size = 1 if kind == "b" else (U16_BYTES if kind == "n" else U32_BYTES)
            v = int.from_bytes(data[i:i + size], "little")
            i += size
            name = f"{EXT_NAMES[sub]} {v}"
            if run:
               err = "EXT by running status is not written by the assembler"
      elif cmd == COMMANDS["VAR"][0]:
         op, addr, val = (list(data[i:i + NOTE_ARGS]) + [0, 0, 0])[:NOTE_ARGS]
         i += NOTE_ARGS
         name = f"VAR {op} {addr} {val}"
         if op >= VAR_OPS:
            err = f"VAR operation {op} (0-{VAR_OPS - 1})"
         elif addr >= VAR_BYTES:
            err = f"VAR address {addr} (0-{VAR_BYTES - 1})"
         elif op >= VAR_FIRST_JUMP:
            tgt = u16(i)
            i += U16_BYTES
      elif cmd in BYTE_NAMES:
         mnemonic = BYTE_NAMES[cmd]
         kinds = COMMANDS[mnemonic][1]
         vals = []
         for k in kinds:
            if k == "a":
               tgt = u16(i)
               i += U16_BYTES
            else:
               v = data[i] if i < len(data) else 0
               i += 1
               vals.append(v - (BYTE_MAX + 1) if k == "s" and v > ARG_MAX else v)
         if mnemonic == "PROGRAM" and vals and vals[0] > ARG_MAX:
            err = f"PROGRAM {vals[0]} (0-{ARG_MAX})"
         name = ("RUN " if run else f"{mnemonic} ") + " ".join(map(str, vals))
      else:
         name = f"BYTES {cmd}"
         err = f"reserved command byte 0x{cmd:02X}"
      if i > len(data):
         err = err or "the command runs past the data's end"
      out.append(Decoded(start, min(i, len(data)) - start, name.strip(), tgt, err))
   return out


def disassemble(data: bytes, tracks: list[int]) -> str:
   """!The text form of a song (assemble() gives the bytes back).
   @param data    the command bytes
   @param tracks  track start offsets
   @return the text
   """
   cmds = decode(data)
   targets = sorted({c.target for c in cmds if c.target is not None})
   label = {t: f"@l{t}" for t in targets}
   starts = set(tracks)
   lines: list[str] = []
   for c in cmds:
      if c.at in starts:
         lines.append("TRACK")
      if c.at in label:
         lines.append(f"{label[c.at]}:")
      text = c.text
      if c.target is not None:
         text = f"{text} {label[c.target]}".replace("  ", " ")
      lines.append(text)
   return "\n".join(lines) + "\n"


def check_song(data: bytes, tracks: list[int], where: str) -> list[str]:
   """!Problems of a song's bytes: reserved bytes, bad sub-commands and targets, tracks that start with an argument.
   @param data    the command bytes
   @param tracks  track start offsets
   @param where   location for messages
   @return the problems
   """
   errors: list[str] = []
   if len(tracks) > MAX_TRACKS:
      errors.append(f"{where}: {len(tracks)} tracks (0-{MAX_TRACKS})")
   for t in tracks:
      if not 0 <= t < len(data):
         errors.append(f"{where}: track offset {t} outside the {len(data)} bytes")
      elif data[t] < COMMAND_FIRST:
         errors.append(f"{where}: track at {t} starts with an argument byte")
   starts = set()
   for c in decode(data):
      starts.add(c.at)
      if c.error:
         errors.append(f"{where}: at {c.at}: {c.error}")
      if c.target is not None and not 0 <= c.target < len(data):
         errors.append(f"{where}: at {c.at}: target {c.target} outside the data")
   return errors


# -- the manifest ------------------------------------------------------------------------------------------------------


@dataclass
class AudioSet:
   """!A checked manifest, its data loaded."""

   ## The manifest's folder.
   base: Path
   ## The manifest.
   doc: dict[str, Any]
   ## sample key -> bytes (the data, extended to every byte playback reads).
   sample_bytes: dict[str, bytes] = field(default_factory=dict)
   ## song key -> its assembled form.
   songs: dict[str, Assembled] = field(default_factory=dict)
   ## Notes for the compiler's report.
   notes: list[str] = field(default_factory=list)


def _keyed(doc: dict[str, Any], section: str, kind: str, errors: list[str]) -> dict[str, dict[str, Any]]:
   """!A section's entries by key.
   @param doc      the manifest
   @param section  its name
   @param kind     the key kind
   @param errors   problems (appended)
   @return key -> entry
   """
   out: dict[str, dict[str, Any]] = {}
   for i, e in enumerate(doc.get(section, [])):
      w = f"{section}[{i}]"
      if not isinstance(e, dict) or not isinstance(e.get("key"), str):
         errors.append(f"{w}: an object with a 'key' is expected")
         continue
      if not e["key"].startswith(f"{kind}:"):
         errors.append(f"{w}: key {e['key']!r} must start with '{kind}:'")
      if e["key"] in out:
         errors.append(f"{w}: key {e['key']!r} given twice")
      out[e["key"]] = e
   return out


def _int(e: dict[str, Any], name: str, lo: int, hi: int, where: str, errors: list[str],
         default: int | None = None) -> int:
   """!An integer field.
   @param e        the object
   @param name     the field
   @param lo       minimum
   @param hi       maximum
   @param where    location for messages
   @param errors   problems (appended)
   @param default  its value when absent (None: required)
   @return the value (lo when bad)
   """
   v = e.get(name, default)
   if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
      errors.append(f"{where}: '{name}' must be an integer {lo}-{hi}, got {v!r}")
      return lo
   return v


def dpcm_bytes_needed(length: int) -> int:
   """!Bytes a DPCM sample's playback can read: whole blocks covering samples 0..length (the interpolation's next).
   @param length  samples
   @return bytes
   """
   return (length // DPCM_BLOCK_SAMPLES + 1) * DPCM_BLOCK_BYTES


def check_voice(v: Any, where: str, keys: dict[str, set[str]], errors: list[str]) -> None:
   """!One voice object.
   @param v       the voice
   @param where   location for messages
   @param keys    known keys by kind
   @param errors  problems (appended)
   @return None
   """
   if not isinstance(v, dict) or v.get("kind") not in VOICE_KINDS:
      errors.append(f"{where}: a voice needs a 'kind' of {', '.join(VOICE_KINDS)}")
      return
   kind = v["kind"]

   def ref(name: str, kinds: str) -> None:
      """!A key reference.
      @param name   the field
      @param kinds  the key kind
      """
      if v.get(name) not in keys[kinds]:
         errors.append(f"{where}: '{name}' must name a {kinds}: key of the manifest, got {v.get(name)!r}")

   if kind == "none":
      return
   if kind in ("split", "drums"):
      ref("instruments", "instruments")
      if kind == "split":
         ref("keymap", "keymap")
      return
   _int(v, "key", 0, MAX_KEY, where, errors, DEFAULT_KEY)
   if kind == "sample":
      ref("sample", "sample")
      for name in ("attack", "decay", "sustain", "release", "pan_sweep"):
         _int(v, name, 0, BYTE_MAX, where, errors, 0)
      _check_compressed(v, where, keys, errors)
      return
   for name in ("attack", "decay", "release"):
      _int(v, name, 0, TONE_ENV_MAX, where, errors, 0)
   _int(v, "sustain", 0, TONE_SUSTAIN_MAX, where, errors, 0)
   _int(v, "length", 0, MAX_TONE_LENGTH, where, errors, 0)
   _int(v, "pan_sweep", 0, BYTE_MAX, where, errors, 0)
   if kind in ("square_sweep", "square"):
      _int(v, "duty", 0, MAX_DUTY, where, errors, 0)
   if kind == "square_sweep":
      _int(v, "sweep", 0, BYTE_MAX, where, errors, 0)
   if kind == "wave":
      ref("wave", "wave")
   if kind == "noise":
      _int(v, "period", 0, MAX_NOISE_PERIOD, where, errors, 0)


def _check_compressed(v: dict[str, Any], where: str, keys: dict[str, set[str]], errors: list[str]) -> None:
   """!A sample voice's optional "compressed" flag (C15a, R10 L5): the reference driver decodes a sample only when
   the voice says it is compressed AND the sample's header says so, while the engine goes by the sample's format
   alone. The two agree when the flag matches the format; the combinations where they would not are refused: a
   compressed voice over a PCM sample (the reference plays it only reversed, else it is silent), and an
   uncompressed voice over a DPCM sample (the reference plays its bytes as PCM). Without the flag the voice is
   taken to match its sample.
   @param v       the sample voice
   @param where   location for messages
   @param keys    known keys by kind; DPCM_KEYS lists the DPCM samples (check() adds it; without it only the flag's
                  type is checked)
   @param errors  problems (appended)
   @return None
   """
   if "compressed" not in v:
      return
   compressed = v["compressed"]
   if not isinstance(compressed, bool):
      errors.append(f"{where}: 'compressed' must be true or false")
      return
   if DPCM_KEYS not in keys:
      return   # a caller without the samples' formats (a voice checked on its own)
   dpcm = v.get("sample") in keys[DPCM_KEYS]
   if dpcm and not compressed:
      errors.append(f"{where}: 'compressed' false over the DPCM sample {v.get('sample')!r}: the reference driver "
                    "would play its bytes as PCM, which the engine does not reproduce")
   elif compressed and not dpcm and v.get("reverse") is not True:
      errors.append(f"{where}: 'compressed' true over the PCM sample {v.get('sample')!r}: the reference driver "
                    "would leave it silent (it mixes such a voice only reversed), which the engine does not reproduce")


def _load_song(s: dict[str, Any], base: Path, where: str, errors: list[str]) -> Assembled | None:
   """!A song's command bytes from its data file, its text form or its MIDI source.
   @param s       the song entry
   @param base    the manifest's folder
   @param where   location for messages
   @param errors  problems (appended)
   @return the song, or None
   """
   try:
      if "asm" in s:
         return assemble((base / s["asm"]).read_text(encoding="utf-8"), f"{where} {s['asm']}")
      if "data" in s:
         data = (base / s["data"]).read_bytes()
         tracks = s.get("tracks")
         if not isinstance(tracks, list) or not all(isinstance(t, int) for t in tracks):
            errors.append(f"{where}: 'tracks' must list each track's start offset")
            return None
         return Assembled(data, list(tracks))
      if "midi" in s:
         import midi_song   # A6's converter (tools/audio/midi_song.py, beside this module)
         # C14a (S7, A6's ask): the author form's fields (volume, instruments, priority, reverb, clocks_per_beat,
         # exact_gate, compress) over the "options" text, and the converter's own errors reported as problems
         try:
            song = midi_song.convert((base / s["midi"]).read_bytes(), midi_song.options_from_entry(s))
         except midi_song.MidiError as e:
            errors.append(f"{where} {s['midi']}: {e}")
            return None
         return Assembled(bytes(song.data), list(song.tracks))
   except FormatError as e:
      errors.append(str(e))
      return None
   except OSError as e:
      errors.append(f"{where}: {e}")
      return None
   except ImportError:
      errors.append(f"{where}: 'midi' songs need tools/audio/midi_song.py")
      return None
   errors.append(f"{where}: a song needs 'data' + 'tracks', 'asm' or 'midi'")
   return None


def _load_sample(e: dict[str, Any], base: Path, where: str, errors: list[str], notes: list[str]) -> bytes:
   """!A sample's bytes, zero-padded to every byte playback can read.
   @param e       the sample entry
   @param base    the manifest's folder
   @param where   location for messages
   @param errors  problems (appended)
   @param notes   notes (appended)
   @return the bytes
   """
   if e.get("format", "pcm8") not in SAMPLE_FORMATS:
      errors.append(f"{where}: 'format' must be one of {SAMPLE_FORMATS}")
   _int(e, "pitch", 0, U32_MAX, where, errors)
   length = _int(e, "length", 1, U32_MAX, where, errors)
   loop = e.get("loop", False)
   if not isinstance(loop, bool):
      errors.append(f"{where}: 'loop' must be true or false")
   start = _int(e, "loop_start", 0, U32_MAX, where, errors, 0)
   if loop and start >= length:
      errors.append(f"{where}: loop_start {start} must be below length {length}")
   if "data" not in e:
      errors.append(f"{where}: 'data' names the sample's bytes ('wav' sources go through tools/audio/wav_sample.py)")
      return b""
   try:
      data = (base / e["data"]).read_bytes()
   except OSError as ex:
      errors.append(f"{where}: {ex}")
      return b""
   need = dpcm_bytes_needed(length) if e.get("format") == "dpcm4" else length + 1
   if len(data) < need:
      notes.append(f"{where}: {len(data)} bytes, padded with zeros to {need} (playback reads them)")
      data = data + bytes(need - len(data))
   return data


def check(doc: Any, base: Path) -> tuple[list[str], AudioSet]:
   """!Checks a manifest and loads its data.
   @param doc   the parsed JSON
   @param base  its folder
   @return problems (empty when it is good) and the loaded set
   """
   errors: list[str] = []
   aset = AudioSet(base, doc if isinstance(doc, dict) else {})
   if not isinstance(doc, dict) or doc.get("schema") != SCHEMA:
      return [f"schema must be {SCHEMA!r}"], aset
   mixer = doc.get("mixer", {})
   if mixer.get("rate", DEFAULT_RATE) not in RATES:
      errors.append(f"mixer: 'rate' must be one of {RATES}")
   _int(mixer, "channels", MIN_CHANNELS, MAX_CHANNELS, "mixer", errors, DEFAULT_CHANNELS)
   _int(mixer, "master_volume", 0, MAX_MASTER_VOLUME, "mixer", errors, DEFAULT_MASTER_VOLUME)
   _int(mixer, "dac_bits", MIN_DAC_BITS, MAX_DAC_BITS, "mixer", errors, DEFAULT_DAC_BITS)
   if mixer.get("output", "stereo") not in ("stereo", "mono"):
      errors.append("mixer: 'output' must be stereo or mono")
   players = doc.get("players", [])
   if not isinstance(players, list) or len(players) > MAX_PLAYERS:
      errors.append(f"players: at most {MAX_PLAYERS}")
      players = []
   names = []
   total = CLIP_TRACKS
   for i, p in enumerate(players):
      w = f"players[{i}]"
      if not isinstance(p, dict) or not isinstance(p.get("name"), str):
         errors.append(f"{w}: needs a 'name'")
         continue
      names.append(p["name"])
      total += _int(p, "tracks", 1, MAX_TRACKS, w, errors)
      if not isinstance(p.get("start_check", False), bool):
         errors.append(f"{w}: 'start_check' must be true or false")
   if total > MAX_ALL_TRACKS:
      errors.append(f"players: {total} tracks with the clip players' {CLIP_TRACKS} (at most {MAX_ALL_TRACKS})")
   keyed = {kind: _keyed(doc, section, kind, errors) for kind, section in
            (("sample", "samples"), ("wave", "waves"), ("keymap", "keymaps"), ("bank", "voice_banks"),
             ("instruments", "instrument_sets"), ("song", "songs"), ("clip", "clips"))}
   keys = {kind: set(entries) for kind, entries in keyed.items()}
   keys[DPCM_KEYS] = {k for k, e in keyed["sample"].items() if e.get("format") == "dpcm4"}
   for k, e in keyed["sample"].items():
      aset.sample_bytes[k] = _load_sample(e, base, f"samples {k}", errors, aset.notes)
   for k, e in keyed["wave"].items():
      d = e.get("data")
      if not isinstance(d, str) or not re.fullmatch(r"[0-9A-Fa-f]{%d}" % WAVE_NIBBLES, d):
         errors.append(f"waves {k}: 'data' must be {WAVE_NIBBLES} hex nibbles")
   for k, e in keyed["keymap"].items():
      ent = e.get("entries")
      if not isinstance(ent, list) or len(ent) != KEYMAP_ENTRIES or not all(
            isinstance(x, int) and 0 <= x <= BYTE_MAX for x in ent):
         errors.append(f"keymaps {k}: 'entries' must be {KEYMAP_ENTRIES} integers 0-{BYTE_MAX}")
   for k, e in keyed["bank"].items():
      vs = e.get("voices")
      if not isinstance(vs, list):
         errors.append(f"voice_banks {k}: 'voices' must be a list")
         continue
      for i, v in enumerate(vs):
         check_voice(v, f"voice_banks {k} voices[{i}]", keys, errors)
   for k, e in keyed["instruments"].items():
      if "voices" in e:
         vs = e["voices"]
         if not isinstance(vs, list) or len(vs) > SET_VOICES:
            errors.append(f"instrument_sets {k}: 'voices' must be a list of at most {SET_VOICES}")
            continue
         for i, v in enumerate(vs):
            check_voice(v, f"instrument_sets {k} voices[{i}]", keys, errors)
      elif e.get("bank") in keys["bank"]:
         _int(e, "offset", 0, U16_MAX, f"instrument_sets {k}", errors, 0)
      else:
         errors.append(f"instrument_sets {k}: needs 'voices', or a 'bank' and an 'offset'")
   for k, e in keyed["song"].items():
      w = f"songs {k}"
      if e.get("instruments") not in keys["instruments"]:
         errors.append(f"{w}: 'instruments' must name an instruments: key")
      _int(e, "priority", 0, MAX_PRIORITY, w, errors, 0)
      if e.get("reverb") is not None:
         _int(e, "reverb", 0, MAX_REVERB, w, errors)
      song = _load_song(e, base, w, errors)
      if song is not None:
         errors.extend(check_song(song.data, song.tracks, w))
         aset.songs[k] = song
   for k, e in keyed["clip"].items():
      if "reverse_of" in e:
         target = keyed["clip"].get(e["reverse_of"])
         if target is None or "voice" not in target:
            errors.append(f"clips {k}: 'reverse_of' must name a clip with a 'voice'")
      else:
         v = e.get("voice")
         check_voice(v, f"clips {k} voice", keys, errors)
         if isinstance(v, dict) and v.get("kind") != "sample":
            errors.append(f"clips {k}: a clip's voice is a sample voice")
   table = doc.get("song_table", [])
   if not isinstance(table, list) or len(table) > MAX_SOUNDS:
      errors.append("song_table: a list of at most 65535 entries")
      table = []
   for i, e in enumerate(table):
      w = f"song_table[{i}]"
      if not isinstance(e, dict):
         errors.append(f"{w}: an object is expected")
         continue
      if e.get("song") is not None and e.get("song") not in keys["song"]:
         errors.append(f"{w}: 'song' must name a song: key or be null")
      if e.get("player") not in names:
         errors.append(f"{w}: 'player' must name a player")
      if "name" in e and (not isinstance(e["name"], str) or ":" not in e["name"]):
         errors.append(f"{w}: 'name' is an Asset Key 'kind:NAME'")
   return errors, aset


def validate(doc: Any, base: Path) -> list[str]:
   """!The problems of a manifest.
   @param doc   the parsed JSON
   @param base  its folder
   @return the problems (empty when it is good)
   """
   return check(doc, base)[0]


def load(path: Path) -> AudioSet:
   """!Reads and checks a manifest.
   @param path  audio.json
   @return the loaded set
   @throws FormatError listing every problem
   """
   doc = json.loads(Path(path).read_text(encoding="utf-8"))
   errors, aset = check(doc, Path(path).parent)
   if errors:
      raise FormatError(f"{path}:\n  " + "\n  ".join(errors))
   return aset
