#!/usr/bin/env python3
"""Tests for tools/text_encoder.py: code pages, encoding, markup, and the
compilers that encode text with a game's code page (C-2, D-20)."""
from __future__ import annotations

import copy
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import asset_compiler as ac  # noqa: E402
import battle_compiler as bc  # noqa: E402
import script_compiler as sc  # noqa: E402
import sequence_compiler as seqc  # noqa: E402
import text_encoder as te  # noqa: E402

## The test page: ASCII, é at 0x80, x with a combining acute at 0x81, aliases for ’ and the ellipsis.
ACCENTS = ROOT / "tests" / "data" / "text" / "accents.codepage.json"
## Every printable ASCII character.
ASCII = "".join(chr(c) for c in range(0x20, 0x7F))


def page(**changes: Any) -> dict[str, Any]:
   """A copy of the accents page's manifest with some keys replaced.
   @param changes  keys to replace
   @return the manifest
   """
   data = json.loads(ACCENTS.read_text(encoding="utf-8"))
   data.update(copy.deepcopy(changes))
   return data


class DefaultPage(unittest.TestCase):
   """The engine's ASCII page encodes exactly as the compilers did before code pages."""

   def test_ascii_is_identity(self) -> None:
      """Every printable ASCII character is its own code; newline FE, end FF."""
      self.assertEqual(te.DEFAULT.encode(ASCII, "t", markup=False), [ord(c) for c in ASCII] + [0xFF])
      self.assertEqual(te.encode("a\nb", "t"), [0x61, 0xFE, 0x62, 0xFF])
      self.assertEqual(te.DEFAULT.font_span(), (0x20, 95))
      self.assertEqual(te.DEFAULT.glyph_codes(), list(range(0x20, 0x7F)))

   def test_outside_the_page(self) -> None:
      """A character the page does not have is an error naming it and the page."""
      with self.assertRaisesRegex(te.TextError, r"t: character 'é' is not in the code page 'en'"):
         te.encode("café", "t")
      self.assertEqual(te.DEFAULT.missing("café ’x’"), ["é", "’"])

   def test_wrappers_keep_their_messages(self) -> None:
      """asset_compiler.encode_text and battle_compiler.encode raise their own errors."""
      with self.assertRaisesRegex(ac.ManifestError, "not in the code page"):
         ac.encode_text("café", "t")
      with self.assertRaisesRegex(ac.ManifestError, "text must be a string"):
         ac.encode_text(1, "t")
      with self.assertRaisesRegex(bc.ManifestError, "not in the code page"):
         bc.encode("café", "t")


class Markup(unittest.TestCase):
   """Placeholders (FD nn) and terms ({@concept})."""

   def test_placeholders(self) -> None:
      """{name} becomes FD and the argument byte, where the caller allows it."""
      self.assertEqual(te.encode("{user}!", "t", {"user"}), [0xFD, 0x00, 0x21, 0xFF])
      self.assertEqual(te.encode("{status}", "t", {"status"}), [0xFD, 0x08, 0xFF])
      with self.assertRaisesRegex(te.TextError, r"placeholder \{move\} is not available here \(available: \{user\}\)"):
         te.encode("{move}", "t", {"user"})
      with self.assertRaisesRegex(te.TextError, "unknown placeholder"):
         te.encode("{nope}", "t", {"user"})
      with self.assertRaisesRegex(te.TextError, "no placeholders here"):
         te.encode("{user}", "t")

   def test_battle_names(self) -> None:
      """The battle manifests' {party} and {passive} are the Party and Passive arguments."""
      self.assertEqual(bc.encode("{party} {passive}", "t", {"party", "passive"}),
                       [0xFD, 0x05, 0x20, 0xFD, 0x06, 0xFF])
      self.assertEqual(bc.encode("{user}", "t", {"user"}), [0xFD, 0x00, 0xFF])

   def test_plain_braces(self) -> None:
      """Without markup (asset and script texts) braces are characters."""
      self.assertEqual(ac.encode_text("{x}", "t"), [0x7B, 0x78, 0x7D, 0xFF])

   def test_terms(self) -> None:
      """{@concept} and {@concept:many} expand first; unknown terms and forms are errors."""
      terms = {"monster": {"one": "Capsule", "many": "Capsules"}, "rival": "Tamer"}
      self.assertEqual(te.DEFAULT.decode(te.encode("{@monster:many} of {@rival}", "t", terms=terms)),
                       "Capsules of Tamer")
      self.assertEqual(te.DEFAULT.decode(te.encode("{@monster}: {user}", "t", {"user"}, terms)), "Capsule: {user}")
      with self.assertRaisesRegex(te.TextError, r"unknown term \{@orb\}"):
         te.encode("{@orb}", "t", terms=terms)
      with self.assertRaisesRegex(te.TextError, "no terms here"):
         te.encode("{@orb}", "t")
      with self.assertRaisesRegex(te.TextError, "has no 'many' form"):
         te.encode("{@rival:many}", "t", terms=terms)
      with self.assertRaisesRegex(te.TextError, "the form is one of"):
         te.encode("{@monster:few}", "t", terms=terms)

   def test_decode(self) -> None:
      """decode() shows placeholders by name and drops controls."""
      self.assertEqual(te.DEFAULT.decode([0x48, 0xFD, 0x02, 0xFC, 0x07, 0xFE, 0x69, 0xFF, 0x41]), "H{move}\ni")


class FieldPlaceholders(unittest.TestCase):
   """Field placeholders (D-22 decision 6): FD 0x80 + index, controls, and the range split from battle ones."""

   def test_ids_follow_declaration_order(self) -> None:
      """Field placeholders take 0x80, 0x81, ... in order; battle arguments stay below 0x80."""
      field = te.field_placeholders(["player", "rival", "coins"])
      self.assertEqual(field, {"player": 0x80, "rival": 0x81, "coins": 0x82})
      self.assertTrue(max(te.PLACEHOLDERS.values()) <= te.LAST_BATTLE_ARG < te.FIRST_FIELD_ARG)
      self.assertEqual(te.MAX_FIELD_PLACEHOLDERS, 127)
      many = te.field_placeholders(f"p{i}" for i in range(te.MAX_FIELD_PLACEHOLDERS))
      self.assertEqual(max(many.values()), te.LAST_FIELD_ARG)   # never 0xFF (the end code)
      with self.assertRaisesRegex(te.TextError, "at most 127"):
         te.field_placeholders(f"p{i}" for i in range(te.MAX_FIELD_PLACEHOLDERS + 1))

   def test_names(self) -> None:
      """Names are lower-case identifiers, declared once, never a control's name."""
      for bad, message in (("Player", "lower-case identifier"), ("a b", "lower-case identifier"),
                           ("page", "is a control"), ("prompt", "is a control")):
         with self.assertRaisesRegex(te.TextError, message):
            te.field_placeholders([bad])
      with self.assertRaisesRegex(te.TextError, "declared twice"):
         te.field_placeholders(["a", "a"])

   def test_encode_field(self) -> None:
      """Field text: its placeholders as FD nn, {prompt} as FA, {page} as FB, terms expanded first."""
      field = te.field_placeholders(["player", "coins"])
      codes = te.encode_field("Hi {player}!{prompt}\n{page}{coins} {@town}", "t", field, {"town": "Elm"})
      self.assertEqual(codes, [0x48, 0x69, 0x20, te.CODE_PLACEHOLDER, 0x80, 0x21, te.CODE_PROMPT, te.CODE_NEWLINE,
                               te.CODE_PAGE_BREAK, te.CODE_PLACEHOLDER, 0x81, 0x20, 0x45, 0x6C, 0x6D, te.CODE_END])
      self.assertEqual(te.DEFAULT.decode(codes, field), "Hi {player}!{prompt}\n{page}{coins} Elm")
      self.assertEqual(te.DEFAULT.decode(codes), "Hi {80}!{prompt}\n{page}{81} Elm")
      with self.assertRaisesRegex(te.TextError, r"unknown placeholder \{rival\}.*available: \{coins\}, \{player\}"):
         te.encode_field("{rival}", "t", field)
      with self.assertRaisesRegex(te.TextError, r"unknown placeholder \{user\}"):
         te.encode_field("{user}", "t", field)   # battle names are not field names

   def test_battle_texts_have_no_controls(self) -> None:
      """Battle texts keep their bytes: {page} and {prompt} are not controls there."""
      with self.assertRaisesRegex(te.TextError, r"\{page\}"):
         te.encode("{page}", "t", ["user"])
      self.assertEqual(te.encode("{user}", "t", ["user"]), [te.CODE_PLACEHOLDER, 0, te.CODE_END])


class CodePages(unittest.TestCase):
   """Loading and checking code pages."""

   def test_accents(self) -> None:
      """NFC input, longest match first, multi-code-point characters and aliases."""
      p = te.CodePage.load(ACCENTS)
      self.assertEqual(p.id, "xx")
      self.assertEqual(p.encode("café", "t"), [0x63, 0x61, 0x66, 0x80, 0xFF])
      self.assertEqual(p.encode("cafe\u0301", "t"), [0x63, 0x61, 0x66, 0x80, 0xFF])   # decomposed: NFC first
      self.assertEqual(p.encode("x\u0301x", "t"), [0x81, 0x78, 0xFF])                 # the longer match wins
      self.assertEqual(p.encode("it’s…", "t"), [0x69, 0x74, 0x27, 0x73, 0x2E, 0x2E, 0x2E, 0xFF])
      self.assertEqual(p.font_span(), (0x20, 0x81 - 0x20 + 1))
      self.assertEqual(p.decode(p.encode("é x\u0301", "t")), "é x\u0301")
      self.assertEqual(p.missing("y\u0301 \u00fc"), ["\u00fd", "\u00fc"])   # NFC: y with acute is precomposed

   def test_rules(self) -> None:
      """Codes up to 0xF9, each code and character once, space and digits fixed, aliases explicit."""
      bad = [
         (page(map={"0xFA": "é"}), "must be 0x00-0xF9"),
         (page(map={"0x41": "é"}), "code 0x41 is given twice"),
         (page(map={"0x80": "A"}), "character 'A' is given twice"),
         (page(ranges=[{"first": "0x21", "chars": ASCII}]), "the space must be code 0x20"),
         (page(ranges=[{"first": "0x20", "chars": " "}, {"first": "0x40", "chars": "0123456789"}], aliases={}),
          "the digit '0' must be code 0x30"),
         (page(aliases={"a": "b"}), "the character is mapped itself"),
         (page(aliases={"ü": "ö"}), "is not in the code page"),
         (page(map={"0x80": "e\u0301"}), "not NFC-normalised"),
         (page(map={"0x80": "\n"}), "the newline is not a character"),
         (page(schema="capsule.codepage/2"), "schema must be"),
         (page(extra=1), "unknown keys"),
         (page(id=""), "needs an id"),
         (page(ranges=[{"first": "0xF9", "chars": "ab"}]), "must be 0x00-0xF9"),
      ]
      for data, message in bad:
         with self.subTest(message=message):
            with self.assertRaisesRegex(te.TextError, message):
               te.CodePage(data, "p")

   def test_option(self) -> None:
      """--code-page PATH is taken out of a command line; without it, the default page."""
      args = ["a", "--code-page", str(ACCENTS), "b"]
      self.assertEqual(te.option(args).id, "xx")
      self.assertEqual(args, ["a", "b"])
      self.assertIs(te.option(["a"]), te.DEFAULT)
      with self.assertRaisesRegex(te.TextError, "needs a path"):
         te.option(["a", "--code-page"])


class Compilers(unittest.TestCase):
   """The compilers take the code page explicitly."""

   def test_asset_compiler(self) -> None:
      """Text assets use the page passed in; the command line takes --code-page."""
      p = te.CodePage.load(ACCENTS)
      manifest = {"schema": "capsule.assets/1", "assets": [{"key": "text:X", "kind": "text", "text": "é"}]}
      self.assertEqual(ac.compile_manifest(manifest, codepage=p)[3], [[0x80, 0xFF]])
      with self.assertRaises(ac.ManifestError):
         ac.compile_manifest(manifest)
      with tempfile.TemporaryDirectory() as tmp:
         src = Path(tmp) / "assets.json"
         src.write_text(json.dumps(manifest), encoding="utf-8")
         cmd = [sys.executable, str(ROOT / "tools" / "asset_compiler.py"), str(src), str(Path(tmp) / "a.cpp")]
         self.assertNotEqual(subprocess.run(cmd, capture_output=True).returncode, 0)
         self.assertEqual(subprocess.run(cmd + ["--code-page", str(ACCENTS)], capture_output=True).returncode, 0)
         self.assertIn("0x80, 0xFF", (Path(tmp) / "a.cpp").read_text(encoding="utf-8"))

   def test_script_compiler(self) -> None:
      """Inline script texts use the compiler's page."""
      with tempfile.TemporaryDirectory() as tmp:
         base = Path(tmp)
         (base / "s.cscript").write_text('script hello { msg "é"; end; }\n', encoding="utf-8")
         (base / "scripts.json").write_text(json.dumps({"schema": "capsule.scripts/1", "sources": ["s.cscript"]}),
                                            encoding="utf-8")
         self.assertTrue(sc.compile_file(base / "scripts.json").errors())
         comp = sc.compile_file(base / "scripts.json", codepage=te.CodePage.load(ACCENTS))
         self.assertEqual(comp.errors(), [])
         self.assertEqual(comp.result["text"], [0x80, 0xFF])

   def test_bg3_text_needs_a_small_font(self) -> None:
      """Text on bg3 is refused when the page's font has more than 96 glyphs; bg0-bg2 take any."""
      m = json.loads((ROOT / "tests" / "data" / "sequences" / "credits_map.json").read_text(encoding="utf-8"))
      wide = te.CodePage.load(ACCENTS)
      seqc.Compiler(copy.deepcopy(m), codepage=wide).run()

      def to_bg3(node: Any) -> None:
         """Moves every text command (in stages and macros) to bg3."""
         if isinstance(node, dict):
            if node.get("op") == "text":
               node["target"] = "bg3"
            for v in node.values():
               to_bg3(v)
         elif isinstance(node, list):
            for v in node:
               to_bg3(v)
      to_bg3(m)
      seqc.Compiler(copy.deepcopy(m)).run()
      with self.assertRaisesRegex(seqc.ManifestError, "text on bg3 needs a font of at most 96 glyphs"):
         seqc.Compiler(m, codepage=wide).run()


class EngineAgreement(unittest.TestCase):
   """The encoder's constants are the engine headers'."""

   def test_codes(self) -> None:
      """capsule/text/encode.h has the same codes and Arg order."""
      header = (ROOT / "include" / "capsule" / "text" / "encode.h").read_text(encoding="utf-8")

      def code(name: str) -> int:
         """A Code constant of the header."""
         return int(re.search(r"\b" + name + r"\s*=\s*(0x[0-9A-Fa-f]+)", header).group(1), 16)
      self.assertEqual(code("kMaxGlyphCode"), te.MAX_GLYPH_CODE)
      self.assertEqual(code("kCodePrompt"), te.CODE_PROMPT)
      self.assertEqual(code("kCodePageBreak"), te.CODE_PAGE_BREAK)
      self.assertEqual(code("kCodeControl"), te.CODE_CONTROL)
      self.assertEqual(code("kCodePlaceholder"), te.CODE_PLACEHOLDER)
      self.assertEqual(code("kCodeNewline"), te.CODE_NEWLINE)
      self.assertEqual(code("kCodeEnd"), te.CODE_END)
      self.assertEqual(code("kCodeSpace"), te.CODE_SPACE)
      self.assertEqual(code("kCodeDigitZero"), te.CODE_DIGIT_ZERO)
      body = re.search(r"enum class Arg : uint8_t\s*\{(.*?)\};", header, re.S).group(1)
      args = [n for n in re.findall(r"^\s*(\w+),", body, re.M) if n != "NUM_ARGS"]
      self.assertEqual([re.sub(r"(?<!^)([A-Z])", r"_\1", a).lower() for a in args], list(te.PLACEHOLDERS))
      self.assertEqual(list(te.PLACEHOLDERS.values()), list(range(len(args))))
      self.assertEqual(code("kLastBattleArg"), te.LAST_BATTLE_ARG)
      self.assertEqual(code("kFirstFieldArg"), te.FIRST_FIELD_ARG)
      self.assertEqual(code("kLastFieldArg"), te.LAST_FIELD_ARG)
      self.assertEqual(int(re.search(r"kMaxPlaceholderCodes = (\d+);", header).group(1)), te.MAX_PLACEHOLDER_CODES)
      self.assertEqual(sorted(te.CONTROLS.values()), [te.CODE_PROMPT, te.CODE_PAGE_BREAK])

   def test_bg3_limit(self) -> None:
      """src/text/text_layer.cpp locks the same BG3 font limit."""
      source = (ROOT / "src" / "text" / "text_layer.cpp").read_text(encoding="utf-8")
      self.assertIn(f"static_assert(kLastBlockMaxFontGlyphs == {te.BG3_MAX_FONT_GLYPHS},", source)

   def test_role_registry(self) -> None:
      """battle_compiler.ROLES is capsule/monster/monster.h's registry, text_language appended last."""
      header = (ROOT / "include" / "capsule" / "monster" / "monster.h").read_text(encoding="utf-8")
      body = re.search(r"enum class Role : uint8_t\s*\{(.*?)\};", header, re.S).group(1)
      roles = [re.sub(r"(?<!^)([A-Z])", r"_\1", n).lower() for n in re.findall(r"^\s*(\w+),", body, re.M)]
      self.assertEqual(roles, [r[0] for r in bc.ROLES])
      self.assertEqual(roles[-1], "text_language")
      shapes = re.search(r"kRoleShapes\[[^]]*\] = \{(.*?)\};", header, re.S).group(1)
      pairs = [(int(w), int(n)) for w, n in re.findall(r"\{(\d+),\s*(\d+)\}", shapes)]
      self.assertEqual(pairs, [(r[1], r[2]) for r in bc.ROLES])
      lay = bc.compile_layout(bc.REQUIRED_ROLES + ["text_language"], "l")
      self.assertNotEqual(lay["offsets"][-1], bc.ROLE_ABSENT)


if __name__ == "__main__":
   unittest.main()
