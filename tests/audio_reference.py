#!/usr/bin/env python3
"""!An independent Python model of the engine's audio core (D-34 M4 B1a, format document section 11.2).

Written from the audio inventory ($S/battle/audio_inventory.md: the reference handheld driver's behaviour) and the
format document, not from the C++ (src/audio/): it keeps the same state so a per-frame dump can be compared line by
line, but its code follows the original driver's own structure - the mixer's separate paths (fixed-rate PCM without
interpolation, resampled PCM, compressed and reverse playback), the tone channels' envelope as the original's flow of
jumps, allocation as its scan. tests/test_audio_reference.py runs both on the scripts of tests/data/audio/scripts.json.

Usage (a dump of one case): audio_reference.py CASE OUT
"""
from __future__ import annotations

import json
import sys
from collections.abc import Callable
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools" / "audio"))
sys.path.insert(0, str(HERE.parent / "tools"))

import format as audio_format  # noqa: E402
from sequence_compiler import asset_key  # noqa: E402

DATA = HERE / "data" / "audio"

LENGTHS = list(range(25)) + [n for n in range(25, 97) if n % 4 == 0 or n % 6 == 0]
OCTAVE = [2147483648, 2275179671, 2410468894, 2553802834, 2705659852, 2866546760, 3037000500, 3217589947,
          3408917802, 3611622603, 3826380858, 4053909305]   # round(2^31 * 2^(i/12))
TONE = [-2004, -1891, -1785, -1685, -1591, -1501, -1417, -1337, -1262, -1192, -1125, -1062]   # round(-131072/f(36+i))
NOISE = [((13 - i // 4) << 4) | (7 - i % 4) for i in range(56)] + [3, 2, 1, 0]
WAVE_VOL = [0, 0, 0x60, 0x60, 0x60, 0x60, 0x40, 0x40, 0x40, 0x40, 0x80, 0x80, 0x80, 0x80, 0x20, 0x20]
DELTAS = [0, 1, 4, 9, 16, 25, 36, 49, -64, -49, -36, -25, -16, -9, -4, -1]
RATE_SAMPLES = [96, 132, 176, 224, 264, 304, 352, 448, 528, 608, 672, 704]

EXIST, START = 0x80, 0x40
VOLSET, VOLCHG, PITSET, PITCHG = 0x01, 0x03, 0x04, 0x0C
SF_START, SF_STOP, SF_SPECIAL, SF_LOOP, SF_IEC, SF_ENV = 0x80, 0x40, 0x20, 0x10, 0x04, 0x03
SF_ON = SF_START | SF_STOP | SF_IEC | SF_ENV
PAUSE = 0x80000000
NONE = 0xFF

# voice kinds and flags (engine values)
K_NONE, K_SAMPLE, K_SQ1, K_SQ2, K_WAVE, K_NOISE, K_SPLIT, K_DRUMS = range(8)
F_FIX, F_REV, F_EVEN = 0x10, 0x20, 0x40

# tone register offsets from 0x04000060
NR = {"10": 0x00, "11": 0x02, "12": 0x03, "13": 0x04, "14": 0x05, "21": 0x08, "22": 0x09, "23": 0x0C, "24": 0x0D,
      "30": 0x10, "31": 0x12, "32": 0x13, "33": 0x14, "34": 0x15, "41": 0x18, "42": 0x19, "43": 0x1C, "44": 0x1D,
      "50": 0x20, "51": 0x21, "52": 0x24}
SOUNDBIAS_H = 0x29
CH_REGS = {1: ("10", "11", "12", "13", "14"), 2: (None, "21", "22", "23", "24"), 3: ("30", "31", "32", "33", "34"),
           4: (None, "41", "42", "43", "44")}


def s8(v: int) -> int:
   """!A byte as signed."""
   v &= 0xFF
   return v - 256 if v >= 128 else v


def u32(v: int) -> int:
   """!32-bit wrap."""
   return v & 0xFFFFFFFF


def s32(v: int) -> int:
   """!32-bit signed."""
   v &= 0xFFFFFFFF
   return v - (1 << 32) if v >= 1 << 31 else v


# --- the compiled table (as the compiler lays it out) ----------------------------------------------------------------


class Voice:
   """!A voice, resolved."""

   def __init__(self, kind: int = K_NONE, flags: int = 0, key: int = 0, length: int = 0, pan_sweep: int = 0,
                ref: object = 0, adsr: tuple[int, int, int, int] = (0, 0, 0, 0),
                keymap: list[int] | None = None) -> None:
      self.kind, self.flags, self.key, self.length, self.pan_sweep = kind, flags, key, length, pan_sweep
      self.ref, self.adsr, self.keymap = ref, adsr, keymap

   def copy(self) -> "Voice":
      return Voice(self.kind, self.flags, self.key, self.length, self.pan_sweep, self.ref, tuple(self.adsr),
                   self.keymap)

   def code(self) -> int:
      return self.kind | self.flags


class Sample:
   """!A sample: header fields and its bytes."""

   def __init__(self, e: dict, data: bytes) -> None:
      self.dpcm = e.get("format", "pcm8") == "dpcm4"
      self.loop = bool(e.get("loop"))
      self.pitch = e["pitch"]
      self.loop_start = e.get("loop_start", 0)
      self.length = e["length"]
      self.data = data

   def at(self, i: int) -> int:
      """!Sample i (the byte before the data is the length's top byte)."""
      if i < 0:
         return s8(self.length >> 24)
      if not self.dpcm:
         return s8(self.data[i])
      block = i >> 6
      raw = self.data[block * 33:block * 33 + 33]
      level = raw[0]
      vals = [level]
      nibbles = [raw[1] & 0xF]
      for b in raw[2:]:
         nibbles += [b >> 4, b & 0xF]
      for n in nibbles[:63]:
         level = (level + DELTAS[n]) & 0xFF
         vals.append(level)
      return s8(vals[i & 63])


class Table:
   """!The test content as a table."""

   def __init__(self, manifest: Path) -> None:
      aset = audio_format.load(manifest)
      doc = aset.doc
      self.samples = {e["key"]: Sample(e, aset.sample_bytes[e["key"]]) for e in doc.get("samples", [])}
      self.waves = {e["key"]: bytes(int(e["data"][i:i + 2], 16) for i in range(0, 32, 2)) for e in doc["waves"]}
      self.keymaps = {e["key"]: list(e["entries"]) for e in doc.get("keymaps", [])}
      banks = {e["key"]: e["voices"] for e in doc.get("voice_banks", [])}
      self.sets: dict[str, tuple[list, int]] = {}
      raw_sets = {}
      for e in doc.get("instrument_sets", []):
         if "voices" in e:
            raw_sets[e["key"]] = (e["voices"], 0)
         else:
            raw_sets[e["key"]] = (banks[e["bank"]], e.get("offset", 0))
      self.raw_sets = raw_sets
      self.cache: dict[int, Voice] = {}
      self.mixer = doc.get("mixer", {})
      self.players = doc.get("players", [])
      pidx = {p["name"]: i for i, p in enumerate(self.players)}
      self.songs = {}
      for e in doc.get("songs", []):
         s = aset.songs[e["key"]]
         reverb = None if e.get("reverb") is None else e["reverb"]
         self.songs[e["key"]] = {"data": s.data, "tracks": s.tracks, "priority": e.get("priority", 0),
                                 "reverb": reverb, "set": e["instruments"]}
      self.empty = {"data": b"", "tracks": [], "priority": 0, "reverb": None, "set": None}
      self.table = []
      self.keys = []
      for e in doc.get("song_table", []):
         self.table.append((self.songs[e["song"]] if e.get("song") else self.empty, pidx[e["player"]]))
         name = e.get("name") or e.get("song")
         self.keys.append(asset_key(name) if name else 0)
      clips = doc.get("clips", [])
      by = {c["key"]: c for c in clips}
      self.clips = []
      for c in clips:
         if "reverse_of" in c:
            v = self.voice(by[c["reverse_of"]]["voice"])
            v.flags |= F_REV
         else:
            v = self.voice(c["voice"])
         self.clips.append(v)

   def voice(self, v: dict) -> Voice:
      """!A manifest voice as a Voice."""
      kind = audio_format.VOICE_KINDS[v["kind"]]
      if kind == K_NONE:
         return Voice()
      adsr = (v.get("attack", 0), v.get("decay", 0), v.get("sustain", 0), v.get("release", 0))
      if kind == K_SAMPLE:
         flags = (F_FIX if v.get("fixed_rate") else 0) | (F_REV if v.get("reverse") else 0)
         return Voice(kind, flags, v.get("key", 60), 0, v.get("pan_sweep", 0), self.samples[v["sample"]], adsr)
      if kind in (K_SPLIT, K_DRUMS):
         return Voice(kind, 0, v.get("key", 0), 0, v.get("pan_sweep", 0), v["instruments"], (0, 0, 0, 0),
                      self.keymaps[v["keymap"]] if kind == K_SPLIT else None)
      flags = F_EVEN if v.get("even_pitch") else 0
      ps = v.get("sweep", v.get("pan_sweep", 0)) if kind == K_SQ1 else v.get("pan_sweep", 0)
      if kind == K_WAVE:
         ref: object = self.waves[v["wave"]]
      elif kind == K_NOISE:
         ref = v.get("period", 0)
      else:
         ref = v.get("duty", 0)
      return Voice(kind, flags, v.get("key", 60), v.get("length", 0), ps, ref, adsr)

   def set_voice(self, set_key: str, index: int) -> Voice:
      """!Entry `index` of an instrument set (past its bank's end: a none voice, as the compiler's padding)."""
      voices, off = self.raw_sets[set_key]
      i = off + index
      return self.voice(voices[i]) if i < len(voices) else Voice()


# --- the model --------------------------------------------------------------------------------------------------


class Track:
   """!A track's state."""

   def __init__(self) -> None:
      self.offset = 0
      self.stack = [0, 0, 0]
      self.cleared = 0
      self.clear64()
      self.flags = 0

   def clear64(self) -> None:
      """!The first 64 bytes of the original's track: everything but the position and the call stack."""
      self.flags = self.wait = self.depth = self.repeat = self.gate = self.key = self.velocity = 0
      self.running = 0
      self.key_m = self.pit_m = self.key_shift = self.key_shift_x = self.tune = self.pit_x = self.bend = 0
      self.bend_range = self.vol_mr = self.vol_ml = self.vol = self.vol_x = self.pan = self.pan_x = self.mod_m = 0
      self.mod = self.mod_type = self.lfo_speed = self.lfo_phase = self.lfo_delay = self.lfo_delay_count = 0
      self.priority = self.echo_volume = self.echo_length = 0
      self.chan = NONE
      self.voice = Voice()
      self.program = 0
      self.hold = 0
      self.start_offset = 0
      self.events = 0


class Channel:
   """!A channel (sampled or tone)."""

   def __init__(self) -> None:
      self.status = self.kind = self.right = self.left = 0
      self.attack = self.decay = self.sustain = self.release = 0
      self.key = self.env = self.env_r = self.env_l = self.goal = self.counter = 0
      self.echo_volume = self.echo_length = self.gate = self.midi_key = self.velocity = self.priority = 0
      self.rhythm_pan = 0
      self.sustain_goal = self.n4 = self.pan = self.pan_mask = self.modify = self.length = self.sweep = 0
      self.track = 0
      self.prev = self.next = NONE
      self.frequency = 0
      self.position = self.count = self.fraction = 0
      self.sample: Sample | None = None
      self.ref: object = 0
      self.loaded_wave: object = None


class Player:
   """!A player."""

   def __init__(self, first: int, count: int, check: int) -> None:
      self.first, self.count, self.check = first, count, check
      self.song = None
      self.status = PAUSE
      self.clock = 0
      self.sound = 0xFFFF
      self.tempo_d = self.tempo_u = self.tempo_i = self.tempo_c = 0
      self.fade_oi = self.fade_oc = self.fade_ov = 0
      self.priority = 0
      self.busy = 0
      self.voices: object = None
      self.data = b""


class Model:
   """!The whole core."""

   def __init__(self, table: Table) -> None:
      self.t = table
      self.spf = RATE_SAMPLES[audio_format.RATES.index(table.mixer.get("rate", 13379))]
      self.period = 1584 // self.spf
      pcm = (597275 * self.spf + 5000) // 10000
      self.div = (16777216 // pcm + 1) >> 1
      self.channels = table.mixer.get("channels", 5)
      self.master = table.mixer.get("master_volume", 12)
      self.mono = 0 if table.mixer.get("output", "stereo") == "stereo" else 1
      self.reverb = 0
      self.counter = 0
      self.segment = 0
      self.c15 = 0
      self.var = [0] * 16
      self.off = False
      self.busy = 0
      self.right = bytearray(1584)
      self.left = bytearray(1584)
      self.chans = [Channel() for _ in range(16)]
      for i in range(4):
         self.chans[12 + i].kind = K_SQ1 + i
         self.chans[12 + i].pan_mask = 0x11 << i
      self.tracks: list[Track] = []
      self.players: list[Player] = []
      first = 0
      for p in table.players:
         self.players.append(Player(first, p["tracks"], 1 if p.get("start_check") else 0))
         first += p["tracks"]
      self.clip_players = []
      for _ in range(2):
         self.clip_players.append(Player(first, 2, 0))
         first += 2
      self.tracks = [Track() for _ in range(first)]
      self.regs = bytearray(0x40)
      self.regs[SOUNDBIAS_H] = 0x42
      self.regs[NR["50"]] = 0x77
      self.writes: list[tuple[int, int]] = []
      self.clip_songs: list[dict | None] = [None, None]

   # -- registers --
   def w(self, reg: str | int, v: int) -> None:
      r = NR[reg] if isinstance(reg, str) else reg
      self.regs[r] = v & 0xFF
      self.writes.append((r, v & 0xFF))

   def r(self, reg: str | int) -> int:
      return self.regs[NR[reg] if isinstance(reg, str) else reg]

   # -- links --
   def rank(self, t: Track) -> int:
      return self.tracks.index(t) + 1

   def clear_chain(self, ci: int) -> None:
      c = self.chans[ci]
      if c.track == 0:
         return
      t = self.tracks[c.track - 1]
      if c.prev != NONE:
         self.chans[c.prev].next = c.next
      else:
         t.chan = c.next
      if c.next != NONE:
         self.chans[c.next].prev = c.prev
      c.track = 0

   def osc_off(self, n: int) -> None:
      if n == 1:
         self.w("12", 8)
         self.w("14", 0x80)
      elif n == 2:
         self.w("22", 8)
         self.w("24", 0x80)
      elif n == 3:
         self.w("30", 0)
      else:
         self.w("42", 8)
         self.w("44", 0x80)

   def track_stop(self, t: Track) -> None:
      if not t.flags & EXIST:
         return
      ci = t.chan
      while ci != NONE:
         c = self.chans[ci]
         if c.status:
            if K_SQ1 <= c.kind & 0xF <= K_NOISE:
               self.osc_off((c.kind & 0xF) - 1)
            c.status = 0
         c.track = 0
         ci = c.next
      t.chan = NONE

   # -- frequency --
   @staticmethod
   def midi_key_to_freq(sample: Sample, key: int, fine: int) -> int:
      fs = fine << 24
      if key > 178:
         key, fs = 178, 255 << 24
      v1 = OCTAVE[key % 12] >> (14 - key // 12)
      v2 = OCTAVE[(key + 1) % 12] >> (14 - (key + 1) // 12)
      return (sample.pitch * (v1 + (((v2 - v1) * fs) >> 32))) >> 32

   @staticmethod
   def cgb_freq(n: int, key: int, fine: int) -> int:
      if n == 4:
         key = 0 if key <= 20 else min(key - 21, 59)
         return NOISE[key]
      if key <= 35:
         key, fine = 0, 0
      else:
         key -= 36
         if key > 130:
            key, fine = 130, 255
      v1 = TONE[key % 12] >> (key // 12)
      v2 = TONE[(key + 1) % 12] >> ((key + 1) // 12)
      return v1 + ((fine * (v2 - v1)) >> 8) + 2048

   # -- track arithmetic --
   def vol_pit_set(self, t: Track) -> None:
      if t.flags & VOLSET:
         x = (t.vol * t.vol_x) >> 5
         if t.mod_type == 1:
            x = (x * (t.mod_m + 128)) >> 7
         y = 2 * t.pan + t.pan_x
         if t.mod_type == 2:
            y += t.mod_m
         y = max(-128, min(127, y))
         t.vol_mr = (((y + 128) * x) >> 8) & 0xFF
         t.vol_ml = (((127 - y) * x) >> 8) & 0xFF
      if t.flags & PITSET:
         x = (t.tune + t.bend * t.bend_range) * 4 + (t.key_shift << 8) + (t.key_shift_x << 8) + t.pit_x
         if t.mod_type == 0:
            x += 16 * t.mod_m
         t.key_m = s8(x >> 8)
         t.pit_m = x & 0xFF
      t.flags &= ~(PITSET | VOLSET) & 0xFF

   @staticmethod
   def chn_vol(c: Channel, t: Track) -> None:
      r = (c.velocity * (128 + c.rhythm_pan) * t.vol_mr) >> 14
      l = (c.velocity * (127 - c.rhythm_pan) * t.vol_ml) >> 14
      c.right = min(r, 255)
      c.left = min(l, 255)

   def clear_mod(self, t: Track) -> None:
      t.mod_m = 0
      t.lfo_phase = 0
      t.flags |= PITCHG if t.mod_type == 0 else VOLCHG

   # -- commands --
   def note(self, p: Player, t: Track, idx: int) -> None:
      d = p.data
      t.gate = LENGTHS[idx]
      if d[t.offset] < 0x80:
         t.key = d[t.offset]
         t.offset += 1
         if d[t.offset] < 0x80:
            t.velocity = d[t.offset]
            t.offset += 1
            if d[t.offset] < 0x80:
               t.gate = (t.gate + d[t.offset]) & 0xFF
               t.offset += 1
      rp = 0
      v = t.voice
      key = t.key
      if v.kind in (K_SPLIT, K_DRUMS):
         sub = self.t.set_voice(v.ref, v.keymap[t.key] if v.kind == K_SPLIT else t.key)
         if sub.kind in (K_SPLIT, K_DRUMS):
            return
         if v.kind == K_DRUMS:
            if sub.pan_sweep & 0x80:
               rp = s8((sub.pan_sweep - 0xC0) * 2)
            key = sub.key
         v = sub
      if v.kind == K_NONE:
         return
      prio = min(255, p.priority + t.priority)
      rank = self.rank(t)
      if K_SQ1 <= v.kind <= K_NOISE:
         ci = 12 + v.kind - K_SQ1
         c = self.chans[ci]
         if c.status & SF_ON and not c.status & SF_STOP:
            if c.priority < prio:
               pass
            elif c.priority == prio and c.track >= rank:
               pass
            else:
               return
      else:
         # the original's scan (inventory 4.3)
         best_p, best_t, found, releasing = prio, rank, None, False
         ci = None
         for i in range(self.channels):
            c = self.chans[i]
            if not c.status & SF_ON:
               ci = i
               break
            if c.status & SF_STOP:
               if not releasing:
                  releasing = True
                  best_p, best_t, found = c.priority, c.track, i
                  continue
            elif releasing:
               continue
            if c.priority < best_p:
               best_p, best_t, found = c.priority, c.track, i
            elif c.priority > best_p:
               continue
            elif c.track > best_t:
               best_t, found = c.track, i
            elif c.track == best_t:
               found = i
         if ci is None:
            if found is None:
               return
            ci = found
      c = self.chans[ci]
      self.clear_chain(ci)
      c.prev = NONE
      c.next = t.chan
      if t.chan != NONE:
         self.chans[t.chan].prev = ci
      t.chan = ci
      c.track = rank
      t.lfo_delay_count = t.lfo_delay
      if t.lfo_delay:
         self.clear_mod(t)
      self.vol_pit_set(t)
      c.gate, c.midi_key, c.velocity = t.gate, t.key, t.velocity
      c.priority = prio
      c.key = key
      c.rhythm_pan = rp
      c.kind = v.code()
      c.ref = v.ref
      c.attack, c.decay, c.sustain, c.release = v.adsr
      c.echo_volume, c.echo_length = t.echo_volume, t.echo_length
      self.chn_vol(c, t)
      k = max(0, c.key + t.key_m)
      if K_SQ1 <= v.kind <= K_NOISE:
         c.length = v.length
         c.sweep = 8 if v.pan_sweep & 0x80 or not v.pan_sweep & 0x70 else v.pan_sweep
         c.frequency = self.cgb_freq(v.kind - K_SQ1 + 1, k, t.pit_m)
      else:
         c.sample = v.ref
         c.count = t.start_offset
         c.frequency = self.midi_key_to_freq(c.sample, k, t.pit_m)
      c.status = SF_START
      t.flags &= 0xF0

   def fine(self, t: Track) -> None:
      ci = t.chan
      while ci != NONE:
         c = self.chans[ci]
         if c.status & SF_ON:
            c.status |= SF_STOP
         self.clear_chain(ci)
         ci = c.next
      t.flags = 0

   def u16(self, d: bytes, at: int) -> int:
      return d[at] | d[at + 1] << 8

   def run_cmd(self, p: Player, t: Track, cmd: int) -> None:
      d = p.data

      def arg() -> int:
         v = d[t.offset]
         t.offset += 1
         return v

      if cmd == 0xE2:
         self.fine(t)
      elif cmd == 0xE3:
         t.offset = self.u16(d, t.offset)
      elif cmd == 0xE4:
         if t.depth >= 3:
            self.fine(t)
         else:
            t.stack[t.depth] = t.offset + 2
            t.depth += 1
            t.offset = self.u16(d, t.offset)
      elif cmd == 0xE5:
         if t.depth:
            t.depth -= 1
            t.offset = t.stack[t.depth]
      elif cmd == 0xE6:
         n = d[t.offset]
         if n == 0:
            t.offset = self.u16(d, t.offset + 1)
         else:
            t.repeat = (t.repeat + 1) & 0xFF
            t.offset += 1
            if t.repeat < n:
               t.offset = self.u16(d, t.offset)
            else:
               t.repeat = 0
               t.offset += 2
      elif cmd == 0xE7:
         op, addr, data = arg(), arg() & 15, arg()
         a = self.var[addr]
         other = self.var[data & 15]
         if op <= 5:
            self.var[addr] = [data, a + data, a - data, other, a + other, a - other][op] & 0xFF
         elif op < 18:
            rhs = data if op < 12 else other
            cond = [a == rhs, a != rhs, a > rhs, a >= rhs, a <= rhs, a < rhs][(op - 6) % 6]
            if cond:
               t.offset = self.u16(d, t.offset)
            else:
               t.offset += 2
      elif cmd == 0xE8:
         t.priority = arg()
      elif cmd == 0xE9:
         p.tempo_d = (arg() * 2) & 0xFFFF
         p.tempo_i = ((p.tempo_d * p.tempo_u) >> 8) & 0xFFFF
      elif cmd == 0xEA:
         t.key_shift = s8(arg())
         t.flags |= PITCHG
      elif cmd == 0xEB:
         t.program = arg()
         t.voice = self.t.set_voice(p.voices, t.program) if isinstance(p.voices, str) else p.voices.copy()
      elif cmd == 0xEC:
         t.vol = arg()
         t.flags |= VOLCHG
      elif cmd == 0xED:
         t.pan = s8(arg() - 64)
         t.flags |= VOLCHG
      elif cmd == 0xEE:
         t.bend = s8(arg() - 64)
         t.flags |= PITCHG
      elif cmd == 0xEF:
         t.bend_range = arg()
         t.flags |= PITCHG
      elif cmd == 0xF0:
         t.lfo_speed = arg()
         if not t.lfo_speed:
            self.clear_mod(t)
      elif cmd == 0xF1:
         t.lfo_delay = arg()
      elif cmd == 0xF2:
         t.mod = arg()
         if not t.mod:
            self.clear_mod(t)
      elif cmd == 0xF3:
         v = arg()
         if v != t.mod_type:
            t.mod_type = v
            t.flags |= VOLCHG | PITCHG
      elif cmd == 0xF4:
         t.tune = s8(arg() - 64)
         t.flags |= PITCHG
      elif cmd == 0xF5:
         sub = arg()
         if sub == 0:
            t.echo_volume = arg()
         elif sub == 1:
            t.echo_length = arg()
         elif sub in (2, 5, 6, 7):
            v = arg()
            if t.voice.kind != K_SPLIT:
               adsr = list(t.voice.adsr)
               adsr[{5: 0, 6: 1, 7: 2, 2: 3}[sub]] = v
               t.voice = t.voice.copy()
               t.voice.adsr = tuple(adsr)
         elif sub == 3:
            n = self.u16(d, t.offset)
            if t.hold < n:
               t.hold += 1
               t.offset -= 2
               t.wait = 1
            else:
               t.hold = 0
               t.offset += 2
         elif sub == 4:
            t.start_offset = int.from_bytes(d[t.offset:t.offset + 4], "little")
            t.offset += 4
         elif sub == 8:
            t.voice = t.voice.copy()
            t.voice.length = arg()
         elif sub == 9:
            t.voice = t.voice.copy()
            t.voice.pan_sweep = arg()
         else:
            self.fine(t)
      elif cmd == 0xF6:
         key = t.key
         if d[t.offset] < 0x80:
            key = d[t.offset]
            t.key = key
            t.offset += 1
         ci = t.chan
         while ci != NONE:
            c = self.chans[ci]
            if c.status & (SF_START | SF_ENV) and not c.status & SF_STOP and c.midi_key == key:
               c.status |= SF_STOP
               break
            ci = c.next
      else:
         self.fine(t)

   # -- a player's frame (MPlayMain) --
   def main(self, p: Player) -> None:
      if p.status & PAUSE:
         return
      self.fade(p)
      if p.status & PAUSE:
         return
      acc = p.tempo_c + p.tempo_i
      while True:
         p.tempo_c = acc & 0xFFFF
         if acc < 150:
            break
         mask = 0
         for i in range(p.count):
            t = self.tracks[p.first + i]
            if not t.flags & EXIST:
               continue
            mask |= 1 << i
            ci = t.chan
            while ci != NONE:
               c = self.chans[ci]
               nxt = c.next
               if c.status & SF_ON:
                  if c.gate:
                     c.gate -= 1
                     if c.gate == 0:
                        c.status |= SF_STOP
               else:
                  self.clear_chain(ci)
               ci = nxt
            if t.flags & START:
               t.clear64()
               t.flags = EXIST
               t.bend_range = 2
               t.vol_x = 64
               t.lfo_speed = 22
               t.voice = Voice(K_SQ1)
            alive = True
            while t.wait == 0:
               if t.cleared:
                  self.fine(t)
                  alive = False
                  break
               b = p.data[t.offset]
               if b < 0x80:
                  cmd = t.running
               else:
                  t.offset += 1
                  cmd = b
                  if b <= 0xB0 or b >= 0xEB:
                     t.running = b
               t.events += 1
               if cmd < 0x80:
                  self.fine(t)
                  alive = False
                  break
               if cmd <= 0xB0:
                  self.note(p, t, cmd - 0x80)
               elif cmd <= 0xE1:
                  t.wait = LENGTHS[cmd - 0xB1]
               else:
                  self.run_cmd(p, t, cmd)
                  if t.flags == 0:
                     alive = False
                     break
            if alive:
               t.wait -= 1
               if t.lfo_speed and t.mod:
                  if t.lfo_delay_count:
                     t.lfo_delay_count -= 1
                  else:
                     t.lfo_phase = (t.lfo_phase + t.lfo_speed) & 0xFF
                     ph = t.lfo_phase
                     tri = s8(ph) if s8(ph - 64) < 0 else 128 - ph
                     m = (t.mod * tri) >> 6
                     if s8(m) != t.mod_m:
                        t.mod_m = s8(m)
                        t.flags |= PITCHG if t.mod_type == 0 else VOLCHG
         p.clock += 1
         if mask == 0:
            p.status = PAUSE
            return
         p.status = mask
         acc = u32(p.tempo_c - 150)
      for i in range(p.count):
         t = self.tracks[p.first + i]
         if not t.flags & EXIST or not t.flags & 0x0F:
            continue
         self.vol_pit_set(t)
         ci = t.chan
         while ci != NONE:
            c = self.chans[ci]
            nxt = c.next
            if not c.status & SF_ON:
               self.clear_chain(ci)
               ci = nxt
               continue
            tone = K_SQ1 <= c.kind & 0xF <= K_NOISE
            if t.flags & VOLCHG:
               self.chn_vol(c, t)
               if tone:
                  c.modify |= 1
            if t.flags & PITCHG:
               k = max(0, c.key + t.key_m)
               if tone:
                  c.frequency = self.cgb_freq((c.kind & 0xF) - K_SQ1 + 1, k & 0xFF, t.pit_m)
                  c.modify |= 2
               else:
                  c.frequency = self.midi_key_to_freq(c.sample, k & 0xFF, t.pit_m)
            ci = nxt
         t.flags &= 0xF0

   def fade(self, p: Player) -> None:
      if p.fade_oi == 0:
         return
      p.fade_oc = (p.fade_oc - 1) & 0xFFFF
      if p.fade_oc:
         return
      p.fade_oc = p.fade_oi
      if p.fade_ov & 2:
         p.fade_ov = (p.fade_ov + 16) & 0xFFFF
         if p.fade_ov >= 256:
            p.fade_ov = 256
            p.fade_oi = 0
      else:
         p.fade_ov = (p.fade_ov - 16) & 0xFFFF
         if (p.fade_ov if p.fade_ov < 0x8000 else p.fade_ov - 0x10000) <= 0:
            for i in range(p.count):
               t = self.tracks[p.first + i]
               self.track_stop(t)
               if not p.fade_ov & 1:
                  t.flags = 0
            p.status = p.status | PAUSE if p.fade_ov & 1 else PAUSE
            p.fade_oi = 0
            return
      for i in range(p.count):
         t = self.tracks[p.first + i]
         if t.flags & EXIST:
            t.vol_x = (p.fade_ov >> 2) & 0xFF
            t.flags |= VOLCHG

   # -- the tone channels (CgbSound) --
   def mod_vol(self, c: Channel) -> None:
      hard = False
      if not self.mono:
         if c.right >= c.left:
            if c.right // 2 >= c.left:
               c.pan, hard = 0x0F, True
         elif c.left // 2 >= c.right:
            c.pan, hard = 0xF0, True
      c.goal = ((c.left + c.right) & 0xFF) // 16   # envelopeGoal is a u8: the sum is truncated first
      if hard:
         c.goal = min(c.goal, 15)
      else:
         c.pan = 0xFF
      c.sustain_goal = ((c.goal * c.sustain + 15) >> 4) & 0xFF
      c.pan &= c.pan_mask

   def cgb(self) -> None:
      self.c15 = self.c15 - 1 if self.c15 else 14
      for n in range(1, 5):
         c = self.chans[11 + n]
         if c.status & SF_ON:
            self.cgb_channel(n, c)

   def cgb_channel(self, n: int, c: Channel) -> None:
      x0, x1, x2, x3, x4 = CH_REGS[n]
      prev = self.c15
      st = {"dir": self.r(x2)}

      def step_complete() -> str:
         c.counter = (c.counter - 1) & 0xFF
         return "repeat_once" if prev_ok() else "env_complete"

      state = {"prev": prev}

      def prev_ok() -> bool:
         if state["prev"] == 0:
            state["prev"] -= 1
            return True
         return False

      def off() -> None:
         self.osc_off(n)
         c.status = 0
         c.modify = 0

      def echo_start() -> str:
         c.env = ((c.goal * c.echo_volume) + 0xFF) >> 8 & 0xFF
         if c.env:
            c.status |= SF_IEC
            c.modify |= 1
            if n != 3:
               st["dir"] = 8
            return "env_complete"
         return "off"

      def sustain_start() -> str:
         if c.sustain == 0:
            c.status &= ~SF_ENV & 0xFF
            return echo_start()
         c.status -= 1
         c.modify |= 1
         if n != 3:
            st["dir"] = 8
         c.env = c.sustain_goal
         c.counter = 7
         return step_complete()

      def decay_start() -> str:
         c.status -= 1
         c.counter = c.decay
         if c.counter:
            c.modify |= 1
            c.env = c.goal
            if n != 3:
               st["dir"] = c.decay
            return step_complete()
         return sustain_start()

      def repeat() -> str:
         if c.counter == 0:
            if n == 3:
               c.modify |= 1
            self.mod_vol(c)
            phase = c.status & SF_ENV
            if phase == 0:
               c.env = (c.env - 1) & 0xFF
               if s8(c.env) <= 0:
                  return echo_start()
               c.counter = c.release
            elif phase == 1:
               c.env = c.sustain_goal
               c.counter = 7
            elif phase == 2:
               c.env = (c.env - 1) & 0xFF
               if s8(c.env) <= s8(c.sustain_goal):
                  return sustain_start()
               c.counter = c.decay
            else:
               c.env = (c.env + 1) & 0xFF
               if c.env >= c.goal:
                  return decay_start()
               c.counter = c.attack
         return step_complete()

      if c.status & SF_START:
         if c.status & SF_STOP:
            nxt = "off"
         else:
            c.status = 3
            c.modify = 3
            self.mod_vol(c)
            if n in (1, 2):
               if n == 1:
                  self.w(x0, c.sweep)
               self.w(x1, (c.ref << 6) + c.length)
               st["dir"] = c.attack + 8
               c.n4 = 0x40 if c.length else 0
            elif n == 3:
               if c.ref is not c.loaded_wave:
                  self.w(x0, 0x40)
                  for i, b in enumerate(c.ref):
                     self.w(0x30 + i, b)
                  c.loaded_wave = c.ref
               self.w(x0, 0)
               self.w(x1, c.length)
               c.n4 = 0xC0 if c.length else 0x80
            else:
               self.w(x1, c.length)
               self.w(x3, c.ref << 3)
               st["dir"] = c.attack + 8
               c.n4 = 0x40 if c.length else 0
            c.counter = c.attack
            if c.attack:
               c.env = 0
               nxt = step_complete()
            else:
               nxt = decay_start()
      elif c.status & SF_IEC:
         c.echo_length = (c.echo_length - 1) & 0xFF
         nxt = "off" if s8(c.echo_length) <= 0 else "env_complete"
      elif c.status & SF_STOP and c.status & SF_ENV:
         c.status &= ~SF_ENV & 0xFF
         c.counter = c.release
         if c.release:
            c.modify |= 1
            if n != 3:
               st["dir"] = c.release
            nxt = step_complete()
         else:
            nxt = echo_start()
      else:
         nxt = repeat()
      while nxt == "repeat_once":
         nxt = repeat()
      if nxt == "off":
         off()
         return
      # registers
      if c.modify & 2:
         if n < 4 and c.kind & F_EVEN:
            bias = self.r(SOUNDBIAS_H)
            if bias < 0x40:
               c.frequency = (c.frequency + 2) & 0x7FC
            elif bias < 0x80:
               c.frequency = (c.frequency + 1) & 0x7FE
         if n != 4:
            self.w(x3, c.frequency)
         else:
            self.w(x3, (self.r(x3) & 0x08) | c.frequency)
         c.n4 = ((c.n4 & 0xC0) + ((c.frequency >> 8) & 0xFF)) & 0xFF
         self.w(x4, c.n4)
      if c.modify & 1:
         self.w("51", (self.r("51") & ~c.pan_mask) | c.pan)
         if n == 3:
            self.w(x2, WAVE_VOL[c.env])
            if c.n4 & 0x80:
               self.w(x0, 0x80)
               self.w(x4, c.n4)
               c.n4 &= 0x7F
         else:
            self.w(x2, (st["dir"] & 0xF) + (c.env << 4))
            self.w(x4, c.n4 | 0x80)
            if n == 1 and not self.r(x0) & 0x08:
               self.w(x4, c.n4 | 0x80)
      c.modify = 0

   # -- the mixer (SoundMainRAM) --
   def envelope(self, c: Channel) -> bool:
      st = c.status
      if st & SF_START:
         if st & SF_STOP:
            c.status = 0
            return False
         s = c.sample
         off = c.count
         if off >= s.length:
            c.status = 0
            return False
         st = 3
         c.position = s.length - off - 1 if c.kind & F_REV else off
         c.count = s.length - off
         env = 0
         c.fraction = 0
         if s.loop:
            st |= SF_LOOP
         c.status = st
         env += c.attack
         if env >= 255:
            env = 255
            c.status = st = st - 1
      else:
         env = c.env
         if st & SF_IEC:
            old = c.echo_length
            c.echo_length = (old - 1) & 0xFF
            if old <= 1:
               c.status = 0
               return False
         elif st & SF_STOP:
            env = (env * c.release) >> 8
            if env <= c.echo_volume:
               env = c.echo_volume
               if env == 0:
                  c.status = 0
                  return False
               c.status = st | SF_IEC
         elif st & SF_ENV == 2:
            env = (env * c.decay) >> 8
            if env <= c.sustain:
               env = c.sustain
               if env == 0:
                  env = c.echo_volume
                  if env == 0:
                     c.status = 0
                     return False
                  c.status = st | SF_IEC
               else:
                  c.status = st - 1
         elif st & SF_ENV == 3:
            env += c.attack
            if env >= 255:
               env = 255
               c.status = st - 1
      c.env = env
      v = ((self.master + 1) * env) >> 4
      c.env_r = (c.right * v) >> 8
      c.env_l = (c.left * v) >> 8
      return True

   def add(self, side: bytearray, at: int, vol: int, v: int) -> None:
      side[at] = (side[at] + ((vol * v) >> 8)) & 0xFF

   def mix_fixed(self, c: Channel, base: int) -> None:
      """!The original's fixed-rate path: one byte per output sample, no interpolation."""
      s = c.sample
      loop_len = s.length - s.loop_start if c.status & SF_LOOP else 0
      pos, count = c.position, c.count
      for i in range(self.spf):
         v = s.at(pos)
         self.add(self.right, base + i, c.env_r, v)
         self.add(self.left, base + i, c.env_l, v)
         pos += 1
         count -= 1
         if count == 0:
            if loop_len:
               pos, count = s.loop_start, loop_len
            else:
               c.status = 0
               return
      c.position, c.count = pos, count

   def mix_resampled(self, c: Channel, base: int, step: int, back: bool) -> None:
      """!Interpolated stepping (forward, compressed or not; reverse never loops)."""
      s = c.sample
      d = -1 if back else 1
      loop_len = s.length - s.loop_start if c.status & SF_LOOP and not back else 0
      pos, count, fw = c.position, c.count, c.fraction
      s0, s1 = s.at(pos), s.at(pos + d)
      for i in range(self.spf):
         v = s0 + (s32(fw * u32(s1 - s0)) >> 23)
         self.add(self.right, base + i, c.env_r, v)
         self.add(self.left, base + i, c.env_l, v)
         fw = u32(fw + step)
         adv = fw >> 23
         if adv == 0:
            continue
         fw &= ~0x3F800000 & 0xFFFFFFFF
         count -= adv
         if count <= 0:
            if not loop_len:
               c.status = 0
               return
            lr = -count
            while True:
               count += loop_len
               if count > 0:
                  break
               lr -= loop_len
            pos = s.loop_start + lr
         else:
            pos += d * adv
         s0, s1 = s.at(pos), s.at(pos + d)
      c.position, c.count, c.fraction = pos, count, fw

   def mix(self) -> None:
      cnt = self.counter
      seg = 0 if cnt <= 1 else self.period - (cnt - 1)
      base = seg * self.spf
      if self.reverb:
         nxt = 0 if cnt == 2 else base + self.spf
         for i in range(self.spf):
            s = s8(self.right[base + i]) + s8(self.left[base + i]) + s8(self.right[nxt + i]) + s8(self.left[nxt + i])
            v = (s * self.reverb) >> 9
            if v & 0x80:
               v += 1
            self.right[base + i] = self.left[base + i] = v & 0xFF
      else:
         for i in range(self.spf):
            self.right[base + i] = self.left[base + i] = 0
      for ci in range(self.channels):
         c = self.chans[ci]
         if not c.status & SF_ON or not self.envelope(c):
            continue
         rev = bool(c.kind & F_REV)
         fixed = bool(c.kind & F_FIX)
         if rev or c.sample.dpcm:
            if not c.status & SF_SPECIAL:
               c.status |= SF_SPECIAL
            step = 0x800000 if fixed else u32(self.div * c.frequency)
            self.mix_resampled(c, base, step, rev)
         elif fixed:
            self.mix_fixed(c, base)
         else:
            self.mix_resampled(c, base, u32(self.div * c.frequency), False)
      self.segment = seg

   # -- the API --
   def start(self, p: Player, song: dict, sound: int, voices: object = None) -> None:
      if p.busy:
         return
      t0 = self.tracks[p.first]
      idle = (p.song is None or not t0.flags & START) and (not p.status & 0xFFFF or p.status & PAUSE)
      if p.check and not idle and p.priority > song["priority"]:
         return
      p.status = 0
      p.song = song
      p.voices = voices if voices is not None else song["set"]
      p.data = song["data"]
      p.sound = sound
      p.priority = song["priority"]
      p.clock = 0
      p.tempo_d = p.tempo_i = 150
      p.tempo_u = 256
      p.tempo_c = 0
      p.fade_oi = 0
      for i in range(p.count):
         t = self.tracks[p.first + i]
         self.track_stop(t)
         t.cleared = 0
         if i < len(song["tracks"]):
            t.flags = EXIST | START
            t.chan = NONE
            t.offset = song["tracks"][i]
         else:
            t.flags = 0
      if song["reverb"] is not None:
         self.reverb = song["reverb"] & 0x7F

   def pause(self, p: Player) -> None:
      p.status |= PAUSE
      for i in range(p.count):
         self.track_stop(self.tracks[p.first + i])

   def each(self, pi: int, mask: int, f: Callable[[Track], None]) -> None:
      if pi >= len(self.players):
         return
      p = self.players[pi]
      for i in range(p.count):
         t = self.tracks[p.first + i]
         if mask >> i & 1 and t.flags & EXIST:
            f(t)

   def imm_init(self, p: Player) -> None:
      for i in range(p.count):
         t = self.tracks[p.first + i]
         if t.flags & EXIST and t.flags & START:
            t.clear64()
            t.flags = EXIST
            t.bend_range, t.vol_x, t.lfo_speed = 2, 64, 22
            t.voice = Voice(K_SQ1)

   def clip(self, a: list[int]) -> None:
      clip, vol, pan, pitch, hold, release, chorus, prio, offset = a
      if clip >= len(self.t.clips):
         return
      h, mx = 0, 0
      for i, p in enumerate(self.clip_players):
         t = self.tracks[p.first]
         if t.flags == 0 and not (t.chan != NONE and self.chans[t.chan].track == p.first + 1):
            h = i
            break
         if mx < p.clock:
            mx, h = p.clock, i
      b = ((pitch + 0x80) + 0x8000) % 0x10000 - 0x8000
      key, tune = (b >> 8) & 0x7F, (b >> 1) & 0x7F
      tune2 = (chorus + tune) & 0x7F
      body = [0xEB, 0, 0xEC, vol & 0x7F, 0xF5, 4] + list(offset.to_bytes(4, "little")) + \
             [0xF5, 2, release & 0xFF, 0xED, (pan + 64) & 0x7F, 0x80, key, 127, 0xF5, 3] + \
             list(hold.to_bytes(2, "little")) + [0xF6, 0xE2]
      data = bytes([0xF4, tune, 0xE3, 7, 0, 0xF4, tune2] + body)
      song = {"data": data, "tracks": [0, 5] if chorus else [0], "priority": prio, "reverb": None, "set": None}
      self.start(self.clip_players[h], song, clip, voices=self.t.clips[clip])

   def apply(self, ev: list) -> None:
      op, a = ev[1], ev[2:]
      P = self.players
      if op == "play":
         if a[0] < len(self.t.table):
            song, pi = self.t.table[a[0]]
            self.start(P[pi], song, a[0])
      elif op == "play_or_keep":
         song, pi = self.t.table[a[0]]
         p = P[pi]
         if p.song is not song or not p.status & 0xFFFF or p.status & PAUSE:
            self.start(p, song, a[0])
      elif op == "stop_song":
         song, pi = self.t.table[a[0]]
         if P[pi].song is song:
            self.pause(P[pi])
      elif op == "stop_all":
         for p in P + self.clip_players:
            self.pause(p)
      elif op == "resume_all":
         for p in P + self.clip_players:
            p.status &= ~PAUSE
      elif op == "pause":
         self.pause(P[a[0]])
      elif op == "resume":
         P[a[0]].status &= ~PAUSE
      elif op in ("fade_out", "fade_out_paused", "fade_in"):
         p = P[a[0]]
         p.fade_oc = p.fade_oi = a[1]
         p.fade_ov = {"fade_out": 256, "fade_out_paused": 257, "fade_in": 2}[op]
         if op == "fade_in":
            p.status &= ~PAUSE
      elif op == "apply_start":
         self.imm_init(P[a[0]])
      elif op == "set_volume":
         def f(t: Track) -> None:
            t.vol_x = (a[2] // 4) & 0xFF
            t.flags |= VOLCHG
         self.each(a[0], a[1], f)
      elif op == "set_pitch":
         def g(t: Track) -> None:
            t.key_shift_x = s8(a[2] >> 8)
            t.pit_x = a[2] & 0xFF
            t.flags |= PITCHG
         self.each(a[0], a[1], g)
      elif op == "set_pan":
         def h(t: Track) -> None:
            t.pan_x = s8(a[2])
            t.flags |= VOLCHG
         self.each(a[0], a[1], h)
      elif op == "set_tempo_scale":
         p = P[a[0]]
         p.tempo_u = a[1]
         p.tempo_i = (p.tempo_d * p.tempo_u >> 8) & 0xFFFF
      elif op in ("set_mod_depth", "set_lfo_speed"):
         def m(t: Track) -> None:
            if op == "set_mod_depth":
               t.mod = a[2]
               zero = t.mod == 0
            else:
               t.lfo_speed = a[2]
               zero = t.lfo_speed == 0
            if zero:
               self.clear_mod(t)
         self.each(a[0], a[1], m)
      elif op == "clip":
         self.clip(a)
      elif op == "stop_clip":
         self.pause(self.clip_players[a[0]])
      elif op == "clear_clips":
         for p in self.clip_players:
            for i in range(p.count):
               self.tracks[p.first + i].cleared = 1
      elif op == "output":
         self.mono = 0 if a[0] else 1
      elif op == "set_var":
         self.var[a[0] & 15] = a[1] & 0xFF
      elif op == "vsync_off":
         if not self.off:
            self.off = True
            self.right = bytearray(1584)
            self.left = bytearray(1584)
      elif op == "vsync_on":
         if self.off:
            self.off = False
            self.counter = 0
      elif op == "lock":
         (P + [None] * (8 - len(P)) + self.clip_players)[a[0]].busy = 1
      elif op == "lock_driver":
         self.busy = 1
      else:
         raise ValueError(op)

   def vsync(self) -> None:
      if self.off:
         return
      left = self.counter - 1
      self.counter = left & 0xFF
      if left <= 0:
         self.counter = self.period

   def vblank(self) -> None:
      if self.off or self.busy:
         return
      order = self.players + self.clip_players
      start = 0
      for i, p in enumerate(order):
         if p.busy:
            start = i + 1
      for p in order[start:]:
         self.main(p)
      self.cgb()
      self.mix()

   # -- the dump (tests/audio_tests.cpp --run writes the same lines) --
   def dump(self, f: int) -> list[str]:
      out = [f"F {f}"]
      for i, p in enumerate(self.players + self.clip_players):
         out.append(f"P {i} {p.status} {p.clock} {p.sound} {p.tempo_d} {p.tempo_u} {p.tempo_i} {p.tempo_c} "
                    f"{p.fade_oi} {p.fade_oc} {p.fade_ov} {p.priority} {p.count} {p.first} {p.check}")
      for i, t in enumerate(self.tracks):
         v = t.voice
         out.append(" ".join(map(str, ["T", i, t.events, t.start_offset, t.offset, t.hold, *t.stack, t.flags, t.wait,
                                       t.depth, t.repeat, t.gate, t.key, t.velocity, t.running, t.program, v.code(),
                                       v.key, t.key_m, t.pit_m, t.key_shift, t.key_shift_x, t.tune, t.pit_x, t.bend,
                                       t.bend_range, t.vol_mr, t.vol_ml, t.vol, t.vol_x, t.pan, t.pan_x, t.mod_m,
                                       t.mod, t.mod_type, t.lfo_speed, t.lfo_phase, t.lfo_delay, t.lfo_delay_count,
                                       t.priority, t.echo_volume, t.echo_length, t.chan])))
      for i, c in enumerate(self.chans):
         tone = i >= 12
         out.append(" ".join(map(str, ["C", i, c.frequency, c.position, c.count, c.fraction, c.status, c.kind,
                                       c.track, c.key, c.midi_key, c.velocity, c.priority, c.gate, c.rhythm_pan,
                                       c.right, c.left, c.env, c.goal if tone else c.env_r,
                                       c.counter if tone else c.env_l, c.attack, c.decay, c.sustain, c.release,
                                       c.echo_volume, c.echo_length, c.sustain_goal, c.pan, c.n4, c.length,
                                       c.sweep, c.next])))
      out.append(" ".join(map(str, ["D", self.reverb, self.counter, self.segment, self.master, self.mono, self.c15,
                                    *self.var])))
      base = self.segment * self.spf
      out.append("R " + self.right[base:base + self.spf].hex())
      out.append("L " + self.left[base:base + self.spf].hex())
      out.append("W" + "".join(f" {r:02x}={v:02x}" for r, v in self.writes))
      return out


def run_case(case: dict, table: Table | None = None) -> list[str]:
   """!Runs one script case.
   @param case   {"name", "frames", "events"}
   @param table  the content (loaded when None)
   @return the dump lines
   """
   m = Model(table or Table(DATA / "audio.json"))
   events = list(case["events"])
   lines = []
   k = 0
   for f in range(case["frames"]):
      while k < len(events) and events[k][0] == f:
         m.apply(events[k])
         k += 1
      m.vsync()
      m.vblank()
      for p in m.players + m.clip_players:
         p.busy = 0
      m.busy = 0
      lines += m.dump(f)
      m.writes = []
   return lines


def main(argv: list[str]) -> int:
   cases = {c["name"]: c for c in json.loads((DATA / "scripts.json").read_text())["cases"]}
   Path(argv[1]).write_text("\n".join(run_case(cases[argv[0]])) + "\n")
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv[1:]))
