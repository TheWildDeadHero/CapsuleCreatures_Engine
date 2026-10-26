#!/usr/bin/env python3
"""!Audio manifest -> the build's audio table (D-34, M4 B1a; format document section 4).

Reads a capsule.audio/1 manifest (tools/audio/format.py checks it) and writes C++: the rom::Table kTable with its
song table, players, clip voices and Asset Keys, every instrument set as a window into its voice bank, keymaps, wave
patterns, samples (a 16-byte header, then the bytes) and songs (header, track offsets, command bytes). Everything is
`const`: ROM on the GBA. Large byte arrays are string literals; with --parts N the samples and songs spread over N
more files (OUT with _1 .. _N before the suffix), each kept under --part-bytes, so big tables compile fast.

Layout rules (format section 2.4): a bank's voices are padded with `none` voices so every window holds 128 voices and
any keymap byte or drum key indexes inside the array (the bank ends 256 voices past its last window's start).
A song table entry's Asset Key is its "name" when given, else its song's key (0 for an empty slot): the run time's
audio::find_sound() looks them up. A null song plays the empty song (no tracks).

Usage: audio_compiler.py MANIFEST OUT.cpp [--parts N] [--part-bytes B] [--depfile D]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent / "audio"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import format as audio_format  # noqa: E402
from sequence_compiler import asset_key  # noqa: E402

## Voices past a window's start that stay readable (keymap bytes and drum keys index up to 255).
WINDOW_REACH = 256
## Voices of an instrument set window.
SET_VOICES = audio_format.SET_VOICES
## SampleHeader::format values.
SAMPLE_FORMAT = {"pcm8": 0, "dpcm4": 1}
## SampleHeader::flags: loops.
SAMPLE_LOOP = 0x40
## SongHeader::reverb: set it.
REVERB_SET = 0x80
## Bytes of a part file before the next one starts.
DEFAULT_PART_BYTES = 1 << 20
## Printable bytes written as themselves in string literals (others as 3-digit octal escapes).
PLAIN = set(range(0x20, 0x7F)) - {ord('"'), ord("\\"), ord("?")}
## Characters per string literal line.
LITERAL_LINE = 100
## Octal escape width.
OCTAL_DIGITS = 3
## Duty / period / sweep defaults.
DEFAULT_ZERO = 0
## A voice's default key.
DEFAULT_KEY = audio_format.DEFAULT_KEY
## Hex digits per wave byte.
HEX_PAIR = 2
## Base of hex text.
HEX_BASE = 16
## The empty song's symbol.
EMPTY_SONG = "kEmptySong"


def literal(data: bytes, indent: str = "   ") -> str:
   """!Bytes as a C++ string literal (split over lines).
   @param data    the bytes
   @param indent  each line's indent
   @return the literal (its implicit terminating 0 is one byte past the data)
   """
   lines = []
   cur = ""
   for b in data:
      cur += chr(b) if b in PLAIN else f"\\{b:0{OCTAL_DIGITS}o}"
      if len(cur) >= LITERAL_LINE:
         lines.append(f'{indent}"{cur}"')
         cur = ""
   if cur or not lines:
      lines.append(f'{indent}"{cur}"')
   return "\n".join(lines)


def ident(prefix: str, index: int) -> str:
   """!A generated symbol.
   @param prefix  its kind
   @param index   its number
   @return the name
   """
   return f"k{prefix}{index}"


class Compiler:
   """!Lays out one manifest's table."""

   def __init__(self, aset: audio_format.AudioSet) -> None:
      """!Indexes the manifest.
      @param aset  the checked manifest
      """
      ## The manifest.
      self.aset = aset
      ## Its document.
      self.doc = aset.doc
      ## sample key -> symbol.
      self.samples = {e["key"]: ident("Sample", i) for i, e in enumerate(self.doc.get("samples", []))}
      ## wave key -> symbol.
      self.waves = {e["key"]: ident("Wave", i) for i, e in enumerate(self.doc.get("waves", []))}
      ## keymap key -> symbol.
      self.keymaps = {e["key"]: ident("Keymap", i) for i, e in enumerate(self.doc.get("keymaps", []))}
      banks: list[tuple[str, list[dict[str, Any]]]] = []
      sets: dict[str, tuple[int, int]] = {}
      bank_index = {}
      for e in self.doc.get("voice_banks", []):
         bank_index[e["key"]] = len(banks)
         banks.append((ident("Bank", len(banks)), list(e["voices"])))
      for e in self.doc.get("instrument_sets", []):
         if "voices" in e:
            sets[e["key"]] = (len(banks), 0)
            banks.append((ident("Bank", len(banks)), list(e["voices"])))
         else:
            sets[e["key"]] = (bank_index[e["bank"]], e.get("offset", 0))
      ends = [len(v) for _, v in banks]
      for b, off in sets.values():
         ends[b] = max(ends[b], off + WINDOW_REACH)
      for i, (sym, voices) in enumerate(banks):
         voices.extend({"kind": "none"} for _ in range(max(ends[i], SET_VOICES) - len(voices)))
      ## Banks: (symbol, voices); the manifest's, then one per set with its own voices.
      self.banks = banks
      ## instruments key -> (bank index, offset).
      self.sets = sets
      ## song key -> symbol.
      self.songs = {e["key"]: ident("Song", i) for i, e in enumerate(self.doc.get("songs", []))}

   def set_ref(self, key: str) -> str:
      """!The address of an instrument set's first voice.
      @param key  the set
      @return the C++ expression
      """
      b, off = self.sets[key]
      return f"&{self.banks[b][0]}[{off}]"

   def voice(self, v: dict[str, Any], reverse: bool = False) -> str:
      """!A voice initialiser.
      @param v        the voice
      @param reverse  force the reverse flag (a reversed clip twin)
      @return the C++ braces
      """
      kind = v["kind"]
      code = audio_format.VOICE_KINDS[kind]
      key = v.get("key", DEFAULT_KEY)
      env = "{.adsr = {%d, %d, %d, %d}}" % (v.get("attack", 0), v.get("decay", 0), v.get("sustain", 0),
                                              v.get("release", 0))
      length = v.get("length", 0)
      pan_sweep = v.get("pan_sweep", 0)
      if kind == "none":
         return "{0, 0, 0, 0, {.value = 0}, {.adsr = {0, 0, 0, 0}}}"
      if kind == "sample":
         if v.get("fixed_rate"):
            code |= audio_format.FLAG_FIXED_RATE
         if v.get("reverse") or reverse:
            code |= audio_format.FLAG_REVERSE
         ref = f"{{.sample = &{self.samples[v['sample']]}.header}}"
         return f"{{{code}, {key}, 0, {pan_sweep}, {ref}, {env}}}"
      if kind in ("split", "drums"):
         ref = f"{{.set = {self.set_ref(v['instruments'])}}}"
         env = f"{{.keymap = {self.keymaps[v['keymap']]}}}" if kind == "split" else "{.adsr = {0, 0, 0, 0}}"
         return f"{{{code}, {v.get('key', 0)}, 0, {pan_sweep}, {ref}, {env}}}"
      if v.get("even_pitch"):
         code |= audio_format.FLAG_EVEN_PITCH
      if kind == "wave":
         ref = f"{{.wave = {self.waves[v['wave']]}}}"
      elif kind == "noise":
         ref = f"{{.value = {v.get('period', DEFAULT_ZERO)}}}"
      else:
         ref = f"{{.value = {v.get('duty', DEFAULT_ZERO)}}}"
      if kind == "square_sweep":
         pan_sweep = v.get("sweep", pan_sweep)
      return f"{{{code}, {key}, {length}, {pan_sweep}, {ref}, {env}}}"

   def sample_def(self, e: dict[str, Any]) -> str:
      """!A sample's definition.
      @param e  the sample entry
      @return the C++
      """
      data = self.aset.sample_bytes[e["key"]]
      flags = SAMPLE_LOOP if e.get("loop") else 0
      head = (f"{SAMPLE_FORMAT[e.get('format', 'pcm8')]}, {{0, 0}}, {flags}, {e['pitch']}u, "
              f"{e.get('loop_start', 0)}u, {e['length']}u")
      return (f"/// {e['key']}\nextern const SampleBlob<{len(data) + 1}> {self.samples[e['key']]};\n"
              f"const SampleBlob<{len(data) + 1}> {self.samples[e['key']]} = {{{{{head}}},\n{literal(data)}}};\n")

   def song_def(self, e: dict[str, Any]) -> str:
      """!A song's definition (header, track offsets, command bytes).
      @param e  the song entry
      @return the C++
      """
      sym = self.songs[e["key"]]
      song = self.aset.songs[e["key"]]
      reverb = 0 if e.get("reverb") is None else REVERB_SET | e["reverb"]
      tracks = ", ".join(map(str, song.tracks)) or "0"
      return (f"/// {e['key']}\nextern const uint8_t {sym}Data[{len(song.data) + 1}];\n"
              f"const uint8_t {sym}Data[{len(song.data) + 1}] =\n{literal(song.data)};\n"
              f"const uint16_t {sym}Tracks[] = {{{tracks}}};\n"
              f"extern const SongHeader {sym};\n"
              f"const SongHeader {sym} = {{{len(song.tracks)}, {e.get('priority', 0)}, {reverb}, 0, "
              f"{self.set_ref(e['instruments'])}, {sym}Data, {sym}Tracks}};\n")

   def bank_decls(self) -> str:
      """!Declarations of the banks (songs in any file point into them).
      @return the C++
      """
      banks = self.banks
      return "".join(f"extern const Voice {sym}[{len(voices)}];\n" for sym, voices in banks)

   def blobs(self) -> list[tuple[str, str, int]]:
      """!Every sample and song definition with its declaration and size (they may go to part files).
      @return (definition, declaration, bytes) per object
      """
      out = []
      for e in self.doc.get("samples", []):
         n = len(self.aset.sample_bytes[e["key"]]) + 1
         out.append((self.sample_def(e), f"extern const SampleBlob<{n}> {self.samples[e['key']]};", n))
      for e in self.doc.get("songs", []):
         sym = self.songs[e["key"]]
         n = len(self.aset.songs[e["key"]].data) + 1
         out.append((self.song_def(e), f"extern const SongHeader {sym};", n))
      return out

   def main_body(self) -> str:
      """!Waves, keymaps, banks, clips, the song table and kTable.
      @return the C++
      """
      parts = []
      for e in self.doc.get("waves", []):
         d = e["data"]
         vals = ", ".join(str(int(d[i:i + HEX_PAIR], HEX_BASE)) for i in range(0, len(d), HEX_PAIR))
         parts.append(f"/// {e['key']}\nconst uint8_t {self.waves[e['key']]}[kWaveBytes] = {{{vals}}};\n")
      for e in self.doc.get("keymaps", []):
         parts.append(f"/// {e['key']}\nconst uint8_t {self.keymaps[e['key']]}[{len(e['entries'])}] = "
                      f"{{{', '.join(map(str, e['entries']))}}};\n")
      banks = self.banks   # a local alias (Doxygen reads "for ... in self.x" as a new member)
      for sym, voices in banks:
         body = ",\n".join(f"   {self.voice(v)}" for v in voices)
         parts.append(f"const Voice {sym}[{len(voices)}] = {{\n{body}}};\n")
      parts.append(f"const SongHeader {EMPTY_SONG} = {{0, 0, 0, 0, nullptr, nullptr, nullptr}};\n")
      players = self.doc.get("players", [])
      player_index = {p["name"]: i for i, p in enumerate(players)}
      cfg = ", ".join(f"{{{p['tracks']}, {1 if p.get('start_check') else 0}, {{0, 0}}}}" for p in players)
      parts.append(f"const PlayerConfig kPlayers[{max(len(players), 1)}] = {{{cfg}}};\n")
      table = self.doc.get("song_table", [])
      entries = []
      keys = []
      for e in table:
         song = f"&{self.songs[e['song']]}" if e.get("song") else f"&{EMPTY_SONG}"
         entries.append(f"   {{{song}, {player_index[e['player']]}, {{0, 0, 0}}}}")
         name = e.get("name") or e.get("song")
         keys.append(f"0x{asset_key(name, 'song_table'):08X}u" if name else "0u")
      parts.append(f"const SongEntry kSongs[{max(len(table), 1)}] = {{\n" + ",\n".join(entries) + "};\n")
      parts.append(f"const uint32_t kKeys[{max(len(keys), 1)}] = {{{', '.join(keys)}}};\n")
      clips = self.doc.get("clips", [])
      by_key = {c["key"]: c for c in clips}
      cv = []
      for c in clips:
         if "reverse_of" in c:
            cv.append(f"   {self.voice(by_key[c['reverse_of']]['voice'], reverse=True)}")
         else:
            cv.append(f"   {self.voice(c['voice'])}")
      if not cv:
         cv.append("   {0, 0, 0, 0, {.value = 0}, {.adsr = {0, 0, 0, 0}}}")
      parts.append(f"const Voice kClips[{len(cv)}] = {{\n" + ",\n".join(cv) + "};\n")
      return "\n".join(parts)

   def table_def(self) -> str:
      """!kTable.
      @return the C++
      """
      mixer = self.doc.get("mixer", {})
      rate = audio_format.RATES.index(mixer.get("rate", audio_format.DEFAULT_RATE))
      stereo = 1 if mixer.get("output", "stereo") == "stereo" else 0
      players = len(self.doc.get("players", []))
      sounds = len(self.doc.get("song_table", []))
      clips = len(self.doc.get("clips", []))
      return (f"const Table kTable = {{data::kSongs, {sounds}, data::kPlayers, {players}, {rate}, "
              f"{mixer.get('channels', audio_format.DEFAULT_CHANNELS)}, "
              f"{mixer.get('master_volume', audio_format.DEFAULT_MASTER_VOLUME)}, "
              f"{mixer.get('dac_bits', audio_format.DEFAULT_DAC_BITS)}, {stereo}, data::kClips, {clips}, "
              f"data::kKeys}};\n")


## Every generated file's head (the blob type and the namespace).
PROLOGUE = """// Generated by tools/audio_compiler.py from {src}. Do not edit.
#include "capsule/audio/rom.h"

namespace capsule::audio::rom
{{
namespace data
{{

/// A sample: its header, then its bytes (one more: the literal's terminating 0).
template <unsigned N>
struct SampleBlob
{{
   SampleHeader header;   ///< The header.
   uint8_t bytes[N];      ///< The data.
}};
"""
## The end of the data namespace.
DATA_END = "\n}   // namespace data\n"
## The end of the file.
EPILOGUE = "\n}   // namespace capsule::audio::rom\n"


def part_path(out: Path, n: int) -> Path:
   """!The n-th part file of an output.
   @param out  the main output
   @param n    1-based part
   @return its path
   """
   return out.with_name(f"{out.stem}_{n}{out.suffix}")


def compile_manifest(manifest: Path, out: Path, parts: int = 0, part_bytes: int = DEFAULT_PART_BYTES) -> list[Path]:
   """!Writes the table.
   @param manifest    audio.json
   @param out         the main C++ file
   @param parts       part files for samples and songs (0: everything in out)
   @param part_bytes  bytes per part before the next
   @return every file the manifest read (for a depfile)
   @throws audio_format.FormatError for a bad manifest, or more data than the parts hold
   """
   aset = audio_format.load(manifest)
   for note in aset.notes:
      print(f"{manifest}: {note}", file=sys.stderr)
   c = Compiler(aset)
   src = manifest.name
   blobs = c.blobs()
   head = PROLOGUE.format(src=src) + "\n" + c.bank_decls()
   main = [head]
   if parts == 0:
      main.extend(d for d, _, _ in blobs)
   else:
      files: list[list[str]] = [[] for _ in range(parts)]
      sizes = [0] * parts
      at = 0
      for d, _, n in blobs:
         while at < parts - 1 and sizes[at] and sizes[at] + n > part_bytes:
            at += 1
         files[at].append(d)
         sizes[at] += n
      for i, defs in enumerate(files, 1):
         part_path(out, i).write_text(head + "\n".join(defs) + DATA_END + EPILOGUE, encoding="utf-8")
      main.extend(decl + "\n" for _, decl, _ in blobs)
   main.append(c.main_body())
   main.append(DATA_END)
   main.append("\n" + c.table_def())
   main.append(EPILOGUE)
   out.write_text("".join(main), encoding="utf-8")
   read = [manifest]
   base = manifest.parent
   for e in aset.doc.get("samples", []):
      read.append(base / e["data"])
   for e in aset.doc.get("songs", []):
      for k in ("data", "asm", "midi"):
         if k in e:
            read.append(base / e[k])
   return read


def main(argv: list[str]) -> int:
   """!Command line.
   @param argv  arguments
   @return exit status
   """
   ap = argparse.ArgumentParser(description="Compiles a capsule.audio/1 manifest into the build's audio table.")
   ap.add_argument("manifest", type=Path)
   ap.add_argument("out", type=Path)
   ap.add_argument("--parts", type=int, default=0, help="part files for samples and songs")
   ap.add_argument("--part-bytes", type=int, default=DEFAULT_PART_BYTES)
   ap.add_argument("--depfile", type=Path)
   args = ap.parse_args(argv)
   try:
      read = compile_manifest(args.manifest, args.out, args.parts, args.part_bytes)
   except audio_format.FormatError as e:
      print(f"error: {e}", file=sys.stderr)
      return 1
   if args.depfile:
      deps = " ".join(str(p.resolve()).replace(" ", "\\ ") for p in read)
      args.depfile.write_text(f"{args.out}: {deps}\n", encoding="utf-8")
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv[1:]))
