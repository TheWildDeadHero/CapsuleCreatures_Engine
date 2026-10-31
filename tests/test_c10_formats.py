#!/usr/bin/env python3
"""!Tests of the C10 data formats in the compilers (the R7 review's findings): the scripts manifest's "vm" options,
the session lifetime (never saved), the face and object words (tools/script_compiler.py), immediate triggers
(tools/event_compiler.py), text box styles and the text tables' language (tools/asset_compiler.py). Fixture:
tests/data/c10 (copied to a temporary folder when a test changes it)."""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import asset_compiler as ac  # noqa: E402
import map_compiler as mc  # noqa: E402
import save_shape  # noqa: E402
import script_compiler as sc  # noqa: E402

## The fixture.
FIXTURE = ROOT / "tests/data/c10"
## A parsed JSON object.
JsonDict = dict[str, Any]


def copy_fixture(tmp: str) -> Path:
   """!A copy of the fixture (its tileset is the D-30 fixture's, copied beside it).
   @param tmp  an empty temporary folder
   @return the copy's folder
   """
   shutil.copytree(FIXTURE, Path(tmp) / "c10")
   shutil.copytree(ROOT / "tests/data/d30", Path(tmp) / "d30")
   return Path(tmp) / "c10"


def compile_scripts(folder: Path) -> sc.Compiler:
   """!Compiles a copy's scripts.
   @param folder  the copy
   @return the compiler (its result set)
   """
   return sc.compile_file(folder / "scripts/scripts.json")


class VmOptions(unittest.TestCase):
   """!The scripts manifest's "vm" object (R7 findings 2, 4, 8)."""

   def test_flags(self) -> None:
      """!Each option's bit; the fixture asks for all three; the bank's initializer ends with them."""
      self.assertEqual(sc.vm_flags({}), 0)
      self.assertEqual(sc.vm_flags({"resume": "next_frame"}), 1)
      self.assertEqual(sc.vm_flags({"random": "modulo", "vars": "wrap"}), 6)
      self.assertEqual(sc.vm_flags({"resume": "same_frame", "random": "scaled", "vars": "saturate"}), 0)
      comp = sc.compile_file(FIXTURE / "scripts/scripts.json")
      self.assertEqual([d for d in comp.diags if d.severity == "error"], [])
      self.assertEqual(comp.vm_flags, 7)
      self.assertTrue(comp.emit_cpp("x").rstrip().endswith("    0,\n    7,\n};\n\n} // namespace capsule::script"))

   def test_errors(self) -> None:
      """!Unknown keys and values are refused."""
      for bad in ({"speed": "fast"}, {"resume": "later"}, ["resume"]):
         with self.assertRaises(sc.ScriptError):
            sc.vm_flags(bad)


class Lifetimes(unittest.TestCase):
   """!The session lifetime (R7 finding 13): last, in RAM, never in a save."""

   def test_session_is_not_saved(self) -> None:
      """!The fixture's session flag and variable take bank storage but no save bytes; the save shape has the four
      saved lifetimes' sub-banks only."""
      comp = sc.compile_file(FIXTURE / "scripts/scripts.json")
      r = comp.result
      session = sc.LIFETIMES.index("session")
      self.assertEqual(session, len(sc.LIFETIMES) - 1)
      self.assertEqual((r["flag_bytes"][session], r["var_bytes"][session]), (1, 2))
      self.assertEqual(comp.save_bytes(), sum(r["flag_bytes"][:session]) + sum(r["var_bytes"][:session]) +
                       r["name_bytes"])
      self.assertEqual(len(save_shape.shapes(comp)), 2 * session + 1)
      self.assertNotIn("passing", [n for s in save_shape.shapes(comp) for n in s["entries"]])


class Statements(unittest.TestCase):
   """!`face OBJ opposite_player` and the `selected` object (R7 finding 10)."""

   def test_bytes(self) -> None:
      """!ObjectFace with kObjectSelected (250) and kFaceOppositePlayer (6)."""
      comp = sc.compile_file(FIXTURE / "scripts/scripts.json")
      code = bytes(comp.result["code"])
      const8 = 0   # the operand tag of a one-byte constant (capsule::script::kOpConst8)
      selected = bytes([sc.OP["ObjectFace"], const8, sc.OBJECT_SELECTED, sc.FACE_OPPOSITE_PLAYER])
      own = bytes([sc.OP["ObjectFace"], const8, sc.OBJECT_SELF, sc.FACE_OPPOSITE_PLAYER])
      self.assertIn(selected, code)
      self.assertIn(own, code)


class ImmediateTriggers(unittest.TestCase):
   """!Triggers with "run": "immediate" (R7 finding 3)."""

   def test_encoding(self) -> None:
      """!The immediate trigger's Event::direction carries kTriggerImmediate; the plain one does not."""
      with tempfile.TemporaryDirectory() as tmp:
         out = Path(tmp) / "w.cpp"
         rc = mc.main(["map_compiler.py", str(FIXTURE / "world.json"), str(out), "--scripts",
                       str(FIXTURE / "scripts/scripts.json")])
         self.assertEqual(rc, 0)
         text = out.read_text(encoding="utf-8")
      self.assertIn("EventKind::Trigger, 0, 68,", text)   # kAnyDirection | kTriggerImmediate
      self.assertIn("EventKind::Trigger, 0, 4,", text)

   def test_refusals(self) -> None:
      """!A bad run value, and an immediate trigger whose script may wait, are errors."""
      with tempfile.TemporaryDirectory() as tmp:
         folder = copy_fixture(tmp)
         plain = folder / "maps/plain.json"
         m = json.loads(plain.read_text(encoding="utf-8"))
         m["events"]["triggers"][0]["run"] = "soon"
         plain.write_text(json.dumps(m), encoding="utf-8")
         with self.assertRaisesRegex(Exception, "run must be one of"):
            mc.compile_world(folder / "world.json", scripts_path=folder / "scripts/scripts.json")
         m["events"]["triggers"][0]["run"] = "immediate"
         m["events"]["triggers"][0]["script"] = "pauses"
         plain.write_text(json.dumps(m), encoding="utf-8")
         with self.assertRaisesRegex(Exception, "pauses may wait"):
            mc.compile_world(folder / "world.json", scripts_path=folder / "scripts/scripts.json")


class TextBoxes(unittest.TestCase):
   """!Text box styles (kind "text_box")."""

   def manifest(self) -> JsonDict:
      """!The fixture's assets.
      @return the parsed manifest
      """
      return json.loads((FIXTURE / "assets.json").read_text(encoding="utf-8"))

   def test_emitted(self) -> None:
      """!The style's settings in the TextBox initializer, its fonts resolved, the table naming kTextBoxes."""
      ac.compile_manifest(self.manifest(), FIXTURE)
      boxes = ac.compile_manifest.text_boxes
      self.assertEqual(len(boxes), 1)
      self.assertEqual(boxes[0]["window"]["typing"], 1)
      self.assertEqual(boxes[0]["window"]["speedup"], 1)
      entries, bgs, sprites, texts, lists = ac.compile_manifest(self.manifest(), FIXTURE)
      text = ac.emit("assets.json", entries, bgs, sprites, texts, lists, ac.compile_manifest.fonts,
                     ac.compile_manifest.doors, ac.compile_manifest.text_boxes)
      self.assertIn("const text::PropFont* const kBoxFonts0[] = {nullptr, nullptr, nullptr, &kFonts[0]};", text)
      self.assertIn(".char_block = 255, .first_tile = 200", text)
      self.assertIn(".typing = 1, .char_delay = 3, .speedup = 1, .lead_frames = 2, .font_count = 4", text)
      self.assertIn("kFonts, nullptr, kTextBoxes};", text)

   def test_refusals(self) -> None:
      """!Unknown settings, values out of range, unknown words and fonts that are no font asset are errors."""
      for change, message in ((lambda b: b["window"].__setitem__("speed", 1), "unknown"),
                              (lambda b: b["window"].__setitem__("x", 99), "x must be"),
                              (lambda b: b["window"].__setitem__("typing", "fast"), "typing is one of"),
                              (lambda b: b.__setitem__("font", "font:MISSING"), "no font asset"),
                              (lambda b: b["fonts"].__setitem__("x", "font:C10_BOX"), "Font control number")):
         m = self.manifest()
         change(next(a for a in m["assets"] if a["kind"] == "text_box"))
         with self.assertRaisesRegex(ac.ManifestError, message):
            ac.compile_manifest(m, FIXTURE)


class Languages(unittest.TestCase):
   """!Text tables of several languages in one manifest: the build takes one (S2's finding)."""

   def texts(self, m: JsonDict, language: str | None = None) -> dict[str, list[int]]:
      """!The compiled texts by key.
      @param m         the manifest
      @param language  the build's language, or None
      @return key name -> codes
      """
      entries, _, _, texts, _ = ac.compile_manifest(m, FIXTURE, language=language)
      return {name: list(texts[i]) for _key, kind, i, name in entries if kind == ac.KIND_TEXT}

   def test_selection(self) -> None:
      """!The manifest's language, --language, the fallback for a key the build's language lacks."""
      m = json.loads((FIXTURE / "assets.json").read_text(encoding="utf-8"))
      en = self.texts(m)
      self.assertEqual(en["text:C10.HELLO"][:2], [ord("A"), ord("B")])
      fr = self.texts(m, "fr")
      self.assertEqual(fr["text:C10.HELLO"][:2], [ord("B"), ord("A")])
      self.assertIn("text:C10.ONLY_EN", fr)   # the fallback language's
      del m["fallback_language"]
      self.assertNotIn("text:C10.ONLY_EN", self.texts(m, "fr"))

   def test_refusals(self) -> None:
      """!Several languages without a choice, a language no table has, a fallback no table has."""
      m = json.loads((FIXTURE / "assets.json").read_text(encoding="utf-8"))
      del m["language"]
      with self.assertRaisesRegex(ac.ManifestError, "several languages"):
         self.texts(m)
      with self.assertRaisesRegex(ac.ManifestError, "no text table is in language"):
         self.texts(m, "de")
      m["language"] = "en"
      m["fallback_language"] = "de"
      with self.assertRaisesRegex(ac.ManifestError, "fallback_language"):
         self.texts(m, "fr")


if __name__ == "__main__":
   unittest.main()
