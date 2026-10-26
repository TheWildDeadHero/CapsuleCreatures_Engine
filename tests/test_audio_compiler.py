#!/usr/bin/env python3
"""!M4 B1a (D-34): tests of the audio format tools: tools/audio/format.py (the manifest validator, the song text
assembler and disassembler), tools/audio_compiler.py (the generated table), tools/audio/gen_tables.py (the committed
tables match their formulas) and the test content's generator (tests/data/audio/make_content.py)."""
from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "audio"))
sys.path.insert(0, str(ROOT / "tools"))
import audio_compiler as compiler  # noqa: E402
import format as fmt  # noqa: E402
import gen_tables  # noqa: E402

## The test content.
DATA = ROOT / "tests" / "data" / "audio"
## A parsed JSON object.
JsonDict = dict[str, Any]


def manifest() -> JsonDict:
   """!The test manifest.
   @return a fresh copy
   """
   return json.loads((DATA / "audio.json").read_text())


class AssemblerTests(unittest.TestCase):
   """!The song text form."""

   def test_every_test_song_round_trips(self) -> None:
      """!assemble(disassemble(x)) == x for every test song, and every song checks clean.
      @return None
      """
      for path in sorted((DATA / "songs").glob("*.cseq")):
         song = fmt.assemble(path.read_text(), path.name)
         self.assertEqual(fmt.check_song(song.data, song.tracks, path.name), [], path.name)
         again = fmt.assemble(fmt.disassemble(song.data, song.tracks), path.name)
         self.assertEqual(again.data, song.data, path.name)
         self.assertEqual(again.tracks, song.tracks, path.name)

   def test_encoding(self) -> None:
      """!The bytes of format section 3.
      @return None
      """
      song = fmt.assemble("TRACK\nPROGRAM 3\nNOTE 0 c4 v100\nRUN 62\nWAIT 96\nNOTE 24 c4 v80 +2\n@l:\n"
                          "NOTE_OFF\nHOLD 300\nSTART_OFFSET 70000\nVAR 6 1 2 @l\nTRANSPOSE -2\nEND\n")
      self.assertEqual(song.data[:2], bytes([0xEB, 3]))
      self.assertEqual(song.data[2:5], bytes([0x80, 60, 100]))
      self.assertEqual(song.data[5], 62)
      self.assertEqual(song.data[6], 0xB1 + 48)
      self.assertEqual(song.data[7:11], bytes([0x80 + 24, 60, 80, 2]))
      label = 11
      self.assertEqual(song.data[11], 0xF6)
      self.assertEqual(song.data[12:16], bytes([0xF5, 3]) + (300).to_bytes(2, "little"))
      self.assertEqual(song.data[16:22], bytes([0xF5, 4]) + (70000).to_bytes(4, "little"))
      self.assertEqual(song.data[22:28], bytes([0xE7, 6, 1, 2]) + label.to_bytes(2, "little"))
      self.assertEqual(song.data[28:30], bytes([0xEA, 0xFE]))
      self.assertEqual(song.data[30], 0xE2)
      self.assertEqual(fmt.parse_key("cs4", "k"), 61)
      self.assertEqual(fmt.parse_key("c-1", "k"), 0)
      self.assertEqual(fmt.LENGTHS[-1], 96)
      self.assertEqual(len(fmt.LENGTHS), 49)

   def test_refusals(self) -> None:
      """!Malformed text is refused with its line.
      @return None
      """
      for text, what in [("WAIT 25", "not a length"), ("RUN 60", "no running"), ("JUMP @x", "unknown label"),
                         ("FOO 1", "unknown command"), ("VAR 6 0 1", "take a target"), ("VAR 1 0 1 @l\n@l:", "take"),
                         ("NOTE 12 200", "out of"), ("PROGRAM 200", "out of"), ("@a:\n@a:", "twice"),
                         ("VAR 18 0 0", "out of")]:
         with self.assertRaises(fmt.FormatError, msg=text) as cm:
            fmt.assemble(text, "t")
         self.assertIn(what, str(cm.exception), text)

   def test_check_song(self) -> None:
      """!Reserved bytes, unknown EXT subs, bad targets and tracks that start with an argument.
      @return None
      """
      self.assertTrue(fmt.check_song(bytes([0xF8]), [0], "s"))
      self.assertTrue(fmt.check_song(bytes([0xF5, 12]), [0], "s"))
      self.assertTrue(fmt.check_song(bytes([0xE3, 50, 0]), [0], "s"))
      self.assertTrue(fmt.check_song(bytes([60, 0xE2]), [0], "s"))
      self.assertEqual(fmt.check_song(b"", [], "s"), [])


class ManifestTests(unittest.TestCase):
   """!The capsule.audio/1 validator."""

   def test_good(self) -> None:
      """!The test manifest checks clean.
      @return None
      """
      self.assertEqual(fmt.validate(manifest(), DATA), [])

   def test_refusals(self) -> None:
      """!Each broken field is named.
      @return None
      """
      def broken(change: Any) -> list[str]:
         doc = manifest()
         change(doc)
         return fmt.validate(doc, DATA)

      cases = [
         (lambda d: d.update(schema="x"), "schema"),
         (lambda d: d["mixer"].update(rate=12345), "rate"),
         (lambda d: d["mixer"].update(channels=13), "channels"),
         (lambda d: d["players"].append({"name": "x", "tracks": 17}), "tracks"),
         (lambda d: d["song_table"].append({"song": "song:nope", "player": "music"}), "song_table"),
         (lambda d: d["song_table"].append({"song": None, "player": "nobody"}), "player"),
         (lambda d: d["samples"][0].update(loop_start=500), "loop_start"),
         (lambda d: d["waves"][0].update(data="12"), "hex"),
         (lambda d: d["keymaps"][0]["entries"].pop(), "128"),
         (lambda d: d["voice_banks"][0]["voices"].append({"kind": "square", "duty": 4}), "duty"),
         (lambda d: d["voice_banks"][0]["voices"].append({"kind": "wave", "wave": "wave:nope"}), "wave"),
         (lambda d: d["voice_banks"][0]["voices"].append({"kind": "sample", "sample": "sample:sine_loop",
                                                          "attack": 300}), "attack"),
         (lambda d: d["songs"][0].update(instruments="instruments:nope"), "instruments"),
         (lambda d: d["clips"].append({"key": "clip:x", "reverse_of": "clip:nope"}), "reverse_of"),
         (lambda d: d["clips"].append({"key": "clip:y", "voice": {"kind": "square"}}), "sample voice"),
         (lambda d: d["songs"].append(copy.deepcopy(d["songs"][0])), "twice"),
         (lambda d: d["song_table"][0].update(name="BEEP"), "Asset Key"),
         (lambda d: d["players"].extend([{"name": f"p{i}", "tracks": 1} for i in range(6)]), "at most"),
         (lambda d: d["voice_banks"][0]["voices"].append({"kind": "sample", "sample": "sample:dpcm_sine_loop",
                                                          "compressed": False}), "as PCM"),
         (lambda d: d["voice_banks"][0]["voices"].append({"kind": "sample", "sample": "sample:sine_loop",
                                                          "compressed": True}), "silent"),
         (lambda d: d["voice_banks"][0]["voices"].append({"kind": "sample", "sample": "sample:sine_loop",
                                                          "compressed": 1}), "true or false"),
      ]
      for change, what in cases:
         errors = broken(change)
         self.assertTrue(any(what in e for e in errors), f"{what}: {errors}")

   def test_compressed_flag(self) -> None:
      """!A sample voice's "compressed" flag is accepted where the reference driver and the engine agree (C15a, R10
      L5).
      @return None
      """
      doc = manifest()
      doc["voice_banks"][0]["voices"].extend([
         {"kind": "sample", "sample": "sample:dpcm_sine_loop", "compressed": True},
         {"kind": "sample", "sample": "sample:sine_loop", "compressed": False},
         {"kind": "sample", "sample": "sample:sine_loop", "compressed": True, "reverse": True},
      ])
      self.assertEqual(fmt.validate(doc, DATA), [])

   def test_midi_songs(self) -> None:
      """!C14a (S7, A6's ask): a MIDI song is converted with the author form's fields over its "options" text
      (midi_song.options_from_entry), and the converter's own errors come back as problems, not exceptions.
      @return None
      """
      # one quarter-note C4 on channel 0 (24 ticks per quarter): note on, note off, end of track
      body = bytes([0x00, 0x90, 60, 100, 24, 0x80, 60, 0, 0x00, 0xFF, 0x2F, 0x00])
      smf = (b"MThd" + (6).to_bytes(4, "big") + (1).to_bytes(2, "big") + (1).to_bytes(2, "big") +
             (24).to_bytes(2, "big") + b"MTrk" + len(body).to_bytes(4, "big") + body)
      with tempfile.TemporaryDirectory() as tmp:
         path = Path(tmp) / "tune.mid"
         path.write_bytes(smf)

         def with_song(**fields: Any) -> list[str]:
            doc = manifest()
            entry = copy.deepcopy(doc["songs"][0])
            entry.pop("asm")
            entry.update(midi=str(path), **fields)
            doc["songs"][0] = entry
            return fmt.validate(doc, DATA)

         self.assertEqual(with_song(options="-E -R50 -V080"), [])
         self.assertEqual(with_song(volume=64), [])   # an author field alone
         bad_option = with_song(options="-Z")
         self.assertTrue(any("tune.mid" in e for e in bad_option), bad_option)
         bad_field = with_song(volume="loud")   # options_from_entry() refuses it (parse_options() never saw it)
         self.assertTrue(any("volume" in e for e in bad_field), bad_field)

   def test_padding_note(self) -> None:
      """!A sample file shorter than playback reads is padded (and noted).
      @return None
      """
      doc = manifest()
      doc["samples"][0]["length"] = 200
      errors, aset = fmt.check(doc, DATA)
      self.assertEqual(errors, [])
      self.assertEqual(len(aset.sample_bytes["sample:sine_loop"]), 201)
      self.assertTrue(any("padded" in n for n in aset.notes))
      self.assertEqual(fmt.dpcm_bytes_needed(64), 2 * 33)


class CompilerTests(unittest.TestCase):
   """!The generated table."""

   def test_compile(self) -> None:
      """!One file, then parts; the keys, the empty song and the banks' padding.
      @return None
      """
      with tempfile.TemporaryDirectory() as d:
         out = Path(d) / "t.cpp"
         read = compiler.compile_manifest(DATA / "audio.json", out)
         text = out.read_text()
         self.assertIn("const Table kTable = {data::kSongs, 21, data::kPlayers, 4, 3, 5, 12, 8, 1, data::kClips, 4",
                       text)
         self.assertIn("&kEmptySong", text)
         self.assertIn(f"0x{compiler.asset_key('sound:BEEP'):08X}u", text)
         self.assertIn("const Voice kBank0[320]", text)
         self.assertIn(DATA / "samples" / "sine_loop.bin", read)
         parts = compiler.compile_manifest(DATA / "audio.json", out, parts=2, part_bytes=2000)
         self.assertEqual(parts, read)
         self.assertTrue((Path(d) / "t_1.cpp").exists() and (Path(d) / "t_2.cpp").exists())
         self.assertIn("extern const SampleBlob", out.read_text())

   def test_literal(self) -> None:
      """!String literals hold every byte (octal escapes, no trigraphs).
      @return None
      """
      self.assertEqual(compiler.literal(b'a"?\\\x00\xff'), '   "a\\042\\077\\134\\000\\377"')

   def test_command_line(self) -> None:
      """!A bad manifest fails with its problems.
      @return None
      """
      with tempfile.TemporaryDirectory() as d:
         bad = Path(d) / "audio.json"
         bad.write_text(json.dumps({"schema": "nope"}))
         self.assertEqual(compiler.main([str(bad), str(Path(d) / "x.cpp")]), 1)


class GeneratedTests(unittest.TestCase):
   """!Committed generated files."""

   def test_tables(self) -> None:
      """!src/audio/tables.cpp is gen_tables.py's output.
      @return None
      """
      self.assertEqual(gen_tables.main(["--check"]), 0)

   def test_content(self) -> None:
      """!The test samples are make_content.py's output.
      @return None
      """
      r = subprocess.run([sys.executable, str(DATA / "make_content.py"), "--check"], check=False)
      self.assertEqual(r.returncode, 0)


if __name__ == "__main__":
   unittest.main()
