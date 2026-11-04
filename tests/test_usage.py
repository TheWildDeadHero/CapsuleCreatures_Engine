#!/usr/bin/env python3
"""!Tests for tools/usage.py (D-36 usage reports): records and the VM banks' records merged into a report, the
engine modules a report marks, a GNU ld map and an nm listing compared against a report (small invented files), the
command line, and the compilers' --usage records on the engine's own test and sample content (script, asset, map,
sequence, audio, battle; their generated sources are the same with and without it)."""
from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from collections.abc import Callable
from contextlib import redirect_stdout
from pathlib import Path

## The engine's root.
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import asset_compiler  # noqa: E402
import audio_compiler  # noqa: E402
import battle_compiler  # noqa: E402
import map_compiler  # noqa: E402
import script_compiler  # noqa: E402
import sequence_compiler  # noqa: E402
import usage  # noqa: E402
import vm_ext  # noqa: E402

## The sandbox map's world and scripts (events, objects with a route, encounters, doors).
SANDBOX_MAP = ROOT / "apps" / "sandbox_map" / "data"
## The script test app's scripts (natives called).
TEST_SCRIPTS = ROOT / "apps" / "test_script" / "data" / "scripts.json"
## The Starter sample (assets and battle data).
STARTER = ROOT / "samples" / "starter"
## The audio test content.
AUDIO = ROOT / "tests" / "data" / "audio" / "audio.json"
## A test sequence.
SEQUENCE = ROOT / "tests" / "data" / "sequences" / "credits_map.json"
## A small GNU ld map: an engine member with two symbols, a section named after a local engine function, the game's
## own object, a library member, fill and an assignment (skipped), a non-code section (skipped).
MAP = """\
Archive member included to satisfy reference by file (symbol)

Linker script and memory map

 .text          0x08000000       0x40 lib/libcapsule_engine.a(vm.cpp.obj)
                0x08000000                _ZN7capsule6script3runEv
                0x08000030                _ZN7capsule3seq4tickEv
 .text._ZN7capsule5audio12_GLOBAL__N_14mixEv
                0x08000040       0x10 lib/libcapsule_engine.a(mixer.cpp.obj)
 .text          0x08000050       0x20 CMakeFiles/capsule_game.dir/src/game.cpp.obj
                0x08000050                main
 .text          0x08000070        0x8 /opt/gba/lib/libc.a(memcpy.o)
 *fill*         0x08000078        0x8
                0x08000080                __end = .
 .debug_info    0x00000000      0x100 lib/libcapsule_engine.a(vm.cpp.obj)
"""
## An nm listing: two engine symbols, a game symbol, an unsized and an absolute one (skipped).
NM = """\
08000000 00000030 T _ZN7capsule6script3runEv
08000030 00000010 t _ZN7capsule3seq4tickEv
08000040 00000020 T main
08000060 W _ZN7capsule5audio4stopEv
00000004 00000004 A __some_absolute
"""


def write(folder: Path, name: str, obj: object) -> Path:
   """!Writes a JSON file.
   @param folder  where
   @param name    its name
   @param obj     the content
   @return its path
   """
   path = folder / name
   path.parent.mkdir(parents=True, exist_ok=True)
   path.write_text(json.dumps(obj), encoding="utf-8")
   return path


class RecordTests(unittest.TestCase):
   """!Records, the merge and the modules."""

   def test_record_and_read(self) -> None:
      """!A record keeps its non-empty categories sorted; both schemas read; another schema is refused."""
      rec = usage.record("g", "script", {"script.op": {"b": 1, "a": 2}, "script.native": {}})
      self.assertEqual(rec["used"], {"script.op": {"a": 2, "b": 1}})
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp) / "usage" / "g"
         usage.write_record(folder / "scripts.json", usage.game_of(folder / "scripts.json"), "script",
                            {"script.op": {"End": 3}})
         self.assertEqual(json.loads((folder / "scripts.json").read_text())["game"], "g")
         bank = write(folder, "bank.json", vm_ext.usage_record("battle", "b", {"goto": 2}, table="ai"))
         self.assertEqual(usage.read_record(bank), ("bank:b", {"vm.battle.ai": {"goto": 2}}))
         bad = write(folder, "bad.json", {"schema": "other/1"})
         with self.assertRaises(usage.UsageError):
            usage.read_record(bad)

   def test_merge(self) -> None:
      """!Counts sum across records; the modules the categories (and single entries) name are used, the base modules
      always, the others not; libraries and HAL sources are listed."""
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         a = write(folder, "a.json", usage.record("g", "script", {"script.op": {"End": 2, "ItemGive": 1}}))
         b = write(folder, "b.json", usage.record("g", "seq", {"seq.op": {"end": 1}, "script.op": {"End": 1}}))
         c = write(folder, "c.json", vm_ext.usage_record("anim", "x", {"wait": 4}))
         report = usage.merge([a, b, c], "g", "gba", ["test_kit"], ["src/hal/gba/hal_gba.cpp"])
      self.assertEqual(report["schema"], usage.REPORT_SCHEMA)
      self.assertEqual(report["used"]["script.op"], {"End": 3, "ItemGive": 1})
      self.assertEqual(report["used"]["vm.anim"], {"wait": 4})
      modules = report["modules"]
      for module in ("script", "seq", "anim", "inventory", "core", "hal"):
         self.assertTrue(modules[module]["used"], module)
      self.assertEqual(modules["inventory"]["by"], ["script.op"])
      self.assertFalse(modules["battle"]["used"])
      self.assertEqual((report["libraries"], report["hal"]), (["test_kit"], ["src/hal/gba/hal_gba.cpp"]))
      self.assertIn("Engine modules nothing uses:", usage.summary(report))

   def test_module_of(self) -> None:
      """!The longest category prefix wins; single entries override; unknown categories have none."""
      self.assertEqual(usage.module_of("vm.battle.ai", "goto"), "battle")
      self.assertEqual(usage.module_of("map.event", "warp"), "field")
      self.assertEqual(usage.module_of("asset.kind", "font"), "text")
      self.assertEqual(usage.module_of("asset.kind", "sprite"), "asset")
      self.assertIsNone(usage.module_of("nothing.known", "x"))

   def test_collecting(self) -> None:
      """!note() counts only inside collecting(), into the innermost collector."""
      usage.note("route.op", "Move")
      with usage.collecting() as outer:
         usage.note("route.op", "Move")
         with usage.collecting() as inner:
            usage.note("route.op", "End", 2)
      self.assertEqual(outer.used, {"route.op": {"Move": 1}})
      self.assertEqual(inner.used, {"route.op": {"End": 2}})


class LinkTests(unittest.TestCase):
   """!The map and the nm listing against a report."""

   def report(self) -> dict:
      """!A report that uses the script module only.
      @return it
      """
      return {"schema": usage.REPORT_SCHEMA, "game": "g",
              "modules": {m: {"used": m == "script"} for m in usage.engine_modules()}}

   def test_map(self) -> None:
      """!Symbols go to their namespace's module, a section to its name's, the rest to the input's; fill,
      assignments and debugging sections are left out."""
      linked = usage.link_usage(MAP)
      self.assertEqual(linked["script"]["bytes"], 0x30)
      self.assertEqual(linked["seq"]["bytes"], 0x10)
      self.assertEqual(linked["audio"]["bytes"], 0x10)
      self.assertEqual(linked[usage.GAME_MODULE]["bytes"], 0x20)
      self.assertEqual(linked[usage.TOOLCHAIN_MODULE]["bytes"], 0x8)
      result = usage.compare(self.report(), linked)
      self.assertEqual(result["unused_linked"], ["audio", "seq"])
      self.assertEqual(result["unused_linked_bytes"], 0x20)
      self.assertEqual(result["linked_bytes"], 0x78)

   def test_nm(self) -> None:
      """!Sized symbols by namespace; others in OTHER_MODULE; unsized and absolute symbols left out."""
      linked = usage.read_linked(NM)
      self.assertEqual({m: e["bytes"] for m, e in linked.items()},
                       {"script": 0x30, "seq": 0x10, usage.OTHER_MODULE: 0x20})
      self.assertEqual(usage.compare(self.report(), linked)["unused_linked"], ["seq"])

   def test_own_namespace(self) -> None:
      """!A symbol counts for its own namespace, not for an engine namespace named only in its parameters; section
      names, local entities and veneers too."""
      names = usage.engine_modules()
      # mygame::bag::adjust(short&, capsule::ui::Keys const&): the game's, although it names capsule::ui
      self.assertIsNone(usage.symbol_module("_ZN6mygame3bag6adjustERsRKN7capsule2ui4KeysE", names))
      self.assertEqual(usage.symbol_module(".text._ZN7capsule6script4stepEv", names), "script")
      self.assertEqual(usage.symbol_module("_ZZN7capsule3seq4stepEvE5cache", names), "seq")
      self.assertEqual(usage.symbol_module("_ZN7capsule3rng4nextEv", names), "core")
      self.assertEqual(usage.symbol_module("___ZN7capsule3seq4stepEv_from_arm", names), "seq")
      self.assertIsNone(usage.symbol_module("memcpy", names))

   def test_inputs(self) -> None:
      """!Archive members and runtime objects of src/ find their modules; object suffixes are stripped."""
      index = usage.source_modules()
      names = usage.engine_modules()
      self.assertEqual(usage.source_stem("vm.cpp.obj"), "vm.cpp")
      self.assertEqual(usage.source_stem("vm.o"), "vm")
      self.assertEqual(usage.module_of_input("libcapsule_engine.a(vm.cpp.obj)", index, names), "battle")
      self.assertEqual(usage.module_of_input("libc.a(memcpy.o)", index, names), usage.TOOLCHAIN_MODULE)
      self.assertEqual(usage.module_of_input("x/src/hal/gba/runtime.cpp.obj", index, names), "hal")
      self.assertEqual(usage.module_of_input("x/generated/g/world.cpp.obj", index, names), usage.GAME_MODULE)


class CommandLineTests(unittest.TestCase):
   """!merge, show and linkmap."""

   def test_commands(self) -> None:
      """!merge writes the report (missing records are skipped), show prints it, linkmap compares and writes."""
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         rec = write(folder, "usage/g/scripts.json", usage.record("g", "script", {"script.op": {"End": 1}}))
         out = folder / "usage" / "g.json"
         self.assertEqual(usage.main(["merge", str(rec), str(folder / "missing.json"), "--game", "g", "--out",
                                      str(out), "--platform", "gba", "--libraries", "--hal"]), 0)
         self.assertEqual(json.loads(out.read_text())["used"], {"script.op": {"End": 1}})
         text = io.StringIO()
         with redirect_stdout(text):
            self.assertEqual(usage.main(["show", str(out)]), 0)
         self.assertIn("script.op: 1 used, 1 uses", text.getvalue())
         listing = folder / "g.nm"
         listing.write_text(NM, encoding="utf-8")
         with redirect_stdout(io.StringIO()):
            self.assertEqual(usage.main(["linkmap", str(out), str(listing), "--out", str(folder / "cmp.json")]), 0)
         self.assertEqual(json.loads((folder / "cmp.json").read_text())["unused_linked"], ["seq"])
         self.assertEqual(usage.main(["show", str(rec)]), 1)   # a record is no report


class CompilerTests(unittest.TestCase):
   """!Each compiler's --usage record, and the same generated source with and without it."""

   def run_both(self, compile_fn: Callable[[Path, list[str]], int], out_name: str) -> dict:
      """!Runs a compiler's main() without and with --usage.
      @param compile_fn  callable(out path, extra args) -> exit status
      @param out_name    its output's file name
      @return the record's "used"
      """
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         for sub in ("plain", "with"):   # the build makes the output folders (the audio compiler needs them)
            (folder / sub).mkdir()
         with redirect_stdout(io.StringIO()):
            self.assertEqual(compile_fn(folder / "plain" / out_name, []), 0)
            rec = folder / "usage" / "game" / "rec.json"
            self.assertEqual(compile_fn(folder / "with" / out_name, ["--usage", str(rec)]), 0)
         self.assertEqual((folder / "plain" / out_name).read_bytes(), (folder / "with" / out_name).read_bytes())
         raw = json.loads(rec.read_text(encoding="utf-8"))
         self.assertEqual((raw["schema"], raw["game"]), (usage.RECORD_SCHEMA, "game"))
         return raw["used"]

   def test_scripts(self) -> None:
      """!Opcodes, natives called."""
      used = self.run_both(lambda out, extra: script_compiler.main(
         ["sc", str(TEST_SCRIPTS), str(out), *extra]), "scripts.cpp")
      self.assertGreater(used["script.op"]["End"], 0)
      self.assertTrue(used["script.native"])

   def test_world(self) -> None:
      """!Event kinds, objects, encounter methods, features, the objects' route steps."""
      used = self.run_both(lambda out, extra: map_compiler.main(
         ["mc", str(SANDBOX_MAP / "world.json"), str(out), "--scripts", str(SANDBOX_MAP / "scripts" / "scripts.json"),
          *extra]), "world.cpp")
      self.assertEqual(set(used["map.event"]), {"warp", "trigger", "sign"})
      self.assertIn("route", used["map.object"])
      self.assertIn("Move", used["route.op"])
      self.assertIn("walk", used["map.encounter"])

   def test_assets_battle(self) -> None:
      """!Asset kinds; battle parts and the effect programs' instructions."""
      used = self.run_both(lambda out, extra: asset_compiler.main(
         ["ac", str(STARTER / "assets.json"), str(out), *extra]), "assets.cpp")
      self.assertIn("sprite", used["asset.kind"])
      used = self.run_both(lambda out, extra: battle_compiler.main(
         ["bc", str(STARTER / "battle.json"), str(out), *extra]), "battle.cpp")
      self.assertIn("moves", used["battle.feature"])
      self.assertTrue(used["effect.op"])

   def test_sequence_audio(self) -> None:
      """!Sequence ops; audio sections, voice kinds and song commands."""
      used = self.run_both(lambda out, extra: sequence_compiler.main(
         ["seq", str(SEQUENCE), str(out), *extra]), "seq.inc")
      self.assertIn("end", used["seq.op"])
      used = self.run_both(lambda out, extra: audio_compiler.main([str(AUDIO), str(out), *extra]), "audio.cpp")
      self.assertIn("songs", used["audio.kind"])
      self.assertIn("NOTE", used["audio.command"])
      self.assertIn("sample", used["audio.voice"])


if __name__ == "__main__":
   unittest.main()
