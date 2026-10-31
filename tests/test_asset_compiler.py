#!/usr/bin/env python3
"""!Tests for tools/asset_compiler.py (run by CTest, or directly)."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import asset_compiler as ac  # noqa: E402
import manifest_paths  # noqa: E402
import sequence_compiler as sc  # noqa: E402

## A parsed JSON object (a manifest or an asset).
JsonDict = dict[str, Any]

## Directory of the test manifest (its image frames are relative to it).
FIXTURE_DIR = ROOT / "tests/data/assets"
## The test manifest.
FIXTURE = json.loads((FIXTURE_DIR / "test_assets.json").read_text(encoding="utf-8"))


def compile_fixture() -> tuple[list[Any], ...]:
   """!Compiles the test manifest.
   @return compile_manifest() results
   """
   return ac.compile_manifest(FIXTURE, FIXTURE_DIR)


def manifest(*assets: JsonDict) -> JsonDict:
   """!A minimal manifest around some assets.
   @param assets  asset objects
   @return the manifest
   """
   return {"schema": "capsule.assets/1", "assets": list(assets)}


def bg(**kw: Any) -> JsonDict:
   """!A background asset with defaults.
   @param kw  overrides
   @return the asset object
   """
   a = {"key": "image:T", "kind": "background", "palette": ["#ffffff"], "shapes": []}
   a.update(kw)
   return a


class AssetCompilerTests(unittest.TestCase):
   """!Compiler rules and output."""

   def errors(self, m: JsonDict, fragment: str, field: Any = None) -> None:
      """!Asserts that compiling @p m fails with @p fragment in the message.
      @param m         manifest
      @param fragment  expected text
      @param field     field placeholders for the compile, or None
      """
      with self.assertRaises(ac.ManifestError) as ctx:
         ac.compile_manifest(m, field=field)
      self.assertIn(fragment, str(ctx.exception))

   def test_fixture_compiles_sorted(self) -> None:
      """!The fixture compiles and the index is sorted by key."""
      entries, bgs, sprites, texts, lists = compile_fixture()
      keys = [e[0] for e in entries]
      self.assertEqual(keys, sorted(keys))
      # I3: a third sprite (STEPPER: streamed, a step table, flips)
      self.assertEqual((len(bgs), len(sprites), len(texts), len(lists)), (3, 3, 1, 1))
      self.assertEqual(entries[[e[3] for e in entries].index("image:DISC")][0], sc.asset_key("image:DISC"))

   def test_halves_dedupe_to_three_tiles(self) -> None:
      """!Two solid halves: blank tile 0 plus one tile per color, map follows."""
      halves = bg(palette=["#ff0000", "#00ff00"], shapes=FIXTURE["assets"][0]["shapes"])
      _, bgs, _, _, _ = ac.compile_manifest(manifest(halves))
      b = bgs[0]
      self.assertEqual(b["count"], 3)
      self.assertEqual(b["map"][0], 1)
      self.assertEqual(b["map"][31], 2)
      self.assertEqual(b["tiles"][8], 0x11111111)   # tile 1, row 0

   def test_flips_are_reused(self) -> None:
      """!A shape and its mirror image share one tile with flip bits."""
      def rect(x: int, y: int, w: int, h: int) -> JsonDict:
         return {"shape": "rect", "x": x, "y": y, "w": w, "h": h, "color": 1}
      # An L in tile 0 and its mirror image in tile 1.
      shapes = [rect(0, 0, 3, 8), rect(0, 0, 8, 2), rect(13, 0, 3, 8), rect(8, 0, 8, 2)]
      _, bgs, _, _, _ = ac.compile_manifest(manifest(bg(shapes=shapes)))
      self.assertEqual(bgs[0]["count"], 2)
      # Map entries: low 10 bits are the tile index, bit 10 is the horizontal flip.
      self.assertEqual(bgs[0]["map"][0] & 0x3FF, bgs[0]["map"][1] & 0x3FF)
      self.assertEqual(bgs[0]["map"][1] & 0x400, 0x400)

   def test_both_forms_point_at_each_other(self) -> None:
      """!"affine": "both" emits a regular and an 8bpp form linked by index."""
      _, bgs, _, _, _ = compile_fixture()
      disc = [b for b in bgs if b.get("other") is not None]
      self.assertEqual(len(disc), 2)
      self.assertEqual({b["affine"] for b in disc}, {0, 1})
      affine = [b for b in disc if b["affine"]][0]
      self.assertEqual((affine["width"], affine["priority"]), (16, 2))
      self.assertEqual(len(affine["tiles"]) % 16, 0)

   def test_sprite_frames_and_animations(self) -> None:
      """!Frames are 1D tiles; animations keep period and loop."""
      _, _, sprites, _, _ = compile_fixture()
      s = sprites[0]
      self.assertEqual((s["shape"], s["size"], s["frames"], s["tiles_total"]), (0, 1, 2, 8))
      self.assertEqual(s["animations"], [([0, 1], 4, 1, None, None), ([1], 1, 0, None, None)])

   def test_image_frames_auto_palette_and_durations(self) -> None:
      """!Image frames get a palette in order of appearance; durations pass through; deps listed."""
      deps: list[Path] = []
      _, _, sprites, _, _ = ac.compile_manifest(FIXTURE, FIXTURE_DIR, deps)
      s = sprites[1]
      self.assertEqual(s["palette"][:4], [0, 31, 31 << 10, 0x7FFF])   # red, blue, white
      self.assertEqual(s["animations"], [([0, 1], 0, 1, [2, 5], None)])
      self.assertEqual([p.name for p in deps], ["flip_0.png", "flip_1.png"])
      self.assertEqual(s["tiles"][0], 0x00001111)          # frame 0 row 0: red left half
      self.assertEqual(s["tiles"][8 + 3], 0x22233222)      # frame 1 row 3: white centre

   def test_image_rules(self) -> None:
      """!Image frames: size must match, auto palette needs all images, durations checked."""
      def spr(frames: list[JsonDict], palette: str | list[str] = "auto", anim: JsonDict | None = None) -> JsonDict:
         a = {"key": "sprite:I", "kind": "sprite", "width": 16, "height": 16, "palette": palette,
              "frames": frames}
         if anim:
            a["animations"] = [anim]
         return manifest(a)
      img = {"image": "images/flip_0.png"}
      with self.assertRaises(ac.ManifestError) as ctx:
         ac.compile_manifest(spr([img]), FIXTURE_DIR)
      self.assertIn("is 8x8, the sprite is 16x16", str(ctx.exception))
      self.errors(spr([img]), "need the manifest's directory")
      with self.assertRaises(ac.ManifestError) as ctx:
         ac.compile_manifest(spr([{"shapes": []}]), FIXTURE_DIR)
      self.assertIn("needs every frame to be an image", str(ctx.exception))
      self.errors(spr([{"image": "/abs.png"}]), "relative")
      self.errors(spr([{"shapes": []}], ["#ffffff"], {"frames": [0], "durations": [0]}), "durations")
      self.errors(spr([{"shapes": []}], ["#ffffff"], {"frames": [0], "durations": [3], "period": 2}), "not both")

   def test_absolute_on_every_system(self) -> None:
      """!D-40 RW3: an image path absolute on any system is refused on every system (POSIX root, drive, UNC)."""
      for text in ("/abs.png", "C:/abs.png", "c:abs.png", "C:\\abs.png", "//server/share/a.png",
                   "\\\\server\\share\\a.png"):
         with self.subTest(path=text):
            self.assertTrue(manifest_paths.is_absolute_text(text))
            frame = {"key": "sprite:I", "kind": "sprite", "width": 16, "height": 16, "palette": "auto",
                     "frames": [{"image": text}]}
            self.errors(manifest(frame), "relative")
      for text in ("a.png", "images/a.png", "../shared/a.png", "./a.png"):
         with self.subTest(path=text):
            self.assertFalse(manifest_paths.is_absolute_text(text))

   def test_text_encoding(self) -> None:
      """!Text uses the engine code page with newline and end codes."""
      _, _, _, texts, _ = compile_fixture()
      self.assertEqual(texts[0], [ord("H"), ord("i"), 0xFE] + [ord(c) for c in "there"] + [0xFF])
      self.errors(manifest({"key": "text:X", "kind": "text", "text": "café"}), "not in the code page")

   def test_field_text(self) -> None:
      """!With the game's field placeholders (--scripts) a text asset is field text; without, braces are characters."""
      import tempfile
      import script_compiler as scc
      text = manifest({"key": "text:X", "kind": "text", "text": "Hi {player}{prompt}"})
      _, _, _, plain, _ = ac.compile_manifest(text)
      self.assertEqual(plain[0][:4], [ord("H"), ord("i"), ord(" "), ord("{")])
      with tempfile.TemporaryDirectory() as tmp:
         scripts = Path(tmp) / "scripts.json"
         scripts.write_text(json.dumps({"schema": "capsule.scripts/1", "sources": [],
                                        "placeholders": {"player": {"name": 7}}}), encoding="utf-8")
         field = scc.load_placeholders(scripts)
         _, _, _, texts, _ = ac.compile_manifest(text, field=field)
         self.assertEqual(texts[0], [ord("H"), ord("i"), ord(" "), 0xFD, 0x80, 0xFA, 0xFF])
         out = Path(tmp) / "assets.cpp"
         src = Path(tmp) / "assets.json"
         src.write_text(json.dumps(text), encoding="utf-8")
         self.assertEqual(ac.main(["asset_compiler.py", str(src), str(out), "--scripts", str(scripts)]), 0)
         self.assertIn("0xFD, 0x80", out.read_text(encoding="utf-8"))
         self.errors(manifest({"key": "text:X", "kind": "text", "text": "{nobody}"}), "nobody", field=field)

   def test_rules(self) -> None:
      """!Unknown keys, bad sizes, colors and duplicates are errors."""
      self.errors(manifest(bg(colour=1)), "unknown key")
      self.errors(manifest(bg(size=128)), "256x256")
      self.errors(manifest(bg(shapes=[{"shape": "rect", "x": 0, "y": 0, "w": 8, "h": 8, "color": 16}])), "color")
      self.errors(manifest(bg(), bg()), "collides")
      self.errors(manifest({"key": "sprite:S", "kind": "sprite", "width": 24, "height": 24,
                            "palette": ["#ffffff"], "frames": [{"shapes": []}]}), "not a sprite size")
      self.errors(manifest({"key": "list:L", "kind": "list", "items": []}), "list needs")
      self.errors({"schema": "capsule.assets/2", "assets": []}, "schema")

   def test_emit_mentions_every_table(self) -> None:
      """!The generated source defines the table and the VRAM-plan checks."""
      src = ac.emit("x.json", *compile_fixture())
      for needle in ("const Table kTable", "gfx::background::kMaxTiles", "gfx::sprites::kTilesPerSlot",
                     "kListItems0", "&kBackgrounds["):
         self.assertIn(needle, src)


if __name__ == "__main__":
   unittest.main()
