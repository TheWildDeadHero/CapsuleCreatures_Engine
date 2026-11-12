#!/usr/bin/env python3
"""!Tests for tools/connectors.py (D-10 section 4; run by CTest, or directly), on a copy of the map sandbox and the
Starter's battle data with the sample connector (samples/plugins/party_moods): the attach points, the schemas, the
references against the compilers' own ids, the hooks, the outputs and their C++, the pure-Python rule, and the
compilers taking the blocks off only at the attach points."""
from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler  # noqa: E402
import connectors  # noqa: E402
import map_compiler  # noqa: E402

## The sample connector.
SAMPLE = ROOT / "samples/plugins/party_moods"
## Its id.
SAMPLE_ID = "sample.party_moods"
## A constant line of an ids header: name, number.
ID_LINE = re.compile(r"constexpr \w+ ([A-Z]+)_\w+ = (\d+);")


def write_json(path: Path, data: Any) -> None:
   """!Writes JSON.
   @param path  the file
   @param data  the data
   @return None
   """
   path.write_text(json.dumps(data, indent=1), encoding="utf-8")


def read_json(path: Path) -> Any:
   """!Reads JSON.
   @param path  the file
   @return the data
   """
   return json.loads(path.read_text(encoding="utf-8"))


class ConnectorTests(unittest.TestCase):
   """!The map sandbox with the sample plugin."""

   def setUp(self) -> None:
      """!Copies the game (its battle data stays at the same relative place) and lists the sample plugin.
      @return None
      """
      ## Temporary root.
      self.root = Path(tempfile.mkdtemp(prefix="connectors_"))
      shutil.copytree(ROOT / "apps/sandbox_map", self.root / "apps/sandbox_map")
      shutil.copytree(ROOT / "samples/starter", self.root / "samples/starter")
      ## The game folder.
      self.game = self.root / "apps/sandbox_map"
      shutil.copytree(SAMPLE, self.game / "plugins/party_moods")
      ## The project manifest.
      self.project = self.game / "reference/project.json"
      p = read_json(self.project)
      p["plugins"] = ["plugins/party_moods"]
      write_json(self.project, p)
      ## The battle manifest.
      self.battle = self.root / "samples/starter/battle.json"
      ## The cave map.
      self.cave = self.game / "data/maps/cave.json"

   def tearDown(self) -> None:
      """!Removes the copy and the plugins' private modules.
      @return None
      """
      shutil.rmtree(self.root, ignore_errors=True)
      for name in [n for n in sys.modules if n.startswith("capsule_connector_")]:
         del sys.modules[name]

   def edit(self, path: Path, change: Any) -> None:
      """!Changes a JSON file in place.
      @param path    the file
      @param change  called with its data
      @return None
      """
      data = read_json(path)
      change(data)
      write_json(path, data)

   def give_blocks(self) -> None:
      """!Blocks on two parties, the cave and one of its cells.
      @return None
      """
      def battle(b: Any) -> None:
         """!The parties' blocks."""
         b["parties"][0]["ext"] = {SAMPLE_ID: {"mood": "angry", "bonus": 5, "rival": "Scholar Wen"}}
         b["parties"][1]["ext"] = {SAMPLE_ID: {"bonus": 3}}

      def cave(m: Any) -> None:
         """!The cave's blocks."""
         m["ext"] = {SAMPLE_ID: {"music_mood": "cheerful"}}
         m["cell_ext"] = {SAMPLE_ID: [{"at": [-3, -4], "mood": "angry"}]}

      self.edit(self.battle, battle)
      self.edit(self.cave, cave)

   def errors(self, result: connectors.Result) -> list[str]:
      """!A result's error messages.
      @param result  the result
      @return messages
      """
      return [p.message for p in result.errors()]

   # -- the build -------------------------------------------------------------------------------------------------

   def test_build_writes_tables(self) -> None:
      """!Records resolve to the compilers' numbers, the hooks run, outputs and the depfile are written.
      @return None
      """
      self.give_blocks()
      result = connectors.run(self.project)
      self.assertEqual(self.errors(result), [])
      self.assertEqual([p.message.split(": ")[-1] for p in result.problems],
                       ["a calm opponent's mood bonus does nothing"])
      parties = {r.name: r for r in result.records[SAMPLE_ID] if r.attach == "party"}
      self.assertEqual(parties["Novice Kit"].values, {"mood": 1, "bonus": 5, "rival": 1})
      self.assertEqual(parties["Scholar Wen"].values, {"mood": 0, "bonus": 3, "rival": None})
      cell = next(r for r in result.records[SAMPLE_ID] if r.attach == "cell")
      self.assertEqual((cell.name, cell.number), ("cave@-3,-4", 1))
      out = self.root / "out/ext"
      connectors.write(result, out)
      connectors.write_depfile(self.root / "out/ext.d", out / connectors.AGGREGATE_NAME, result.deps)
      self.assertIn(f'#include "{SAMPLE_ID}/party_moods.cpp"', (out / connectors.AGGREGATE_NAME).read_text())
      source = (out / SAMPLE_ID / "party_moods.cpp").read_text()
      self.assertIn("{0, Mood(1), 5, 1},", source)
      self.assertIn("{1, -3, -4, Mood(1)},", source)
      self.assertIn("case 1: return Mood(2);", source)
      deps = (self.root / "out/ext.d").read_text()
      for needed in (self.project, self.battle, self.cave, self.game / "plugins/party_moods/party_moods/connector.py"):
         # The depfile writes "/" on every host (Ninja and make read it on Windows too).
         self.assertIn(needed.resolve().as_posix(), deps)
      # Unchanged outputs keep their time stamps; a file no longer written goes.
      stamp = (out / SAMPLE_ID / "party_moods.cpp").stat().st_mtime_ns
      (out / SAMPLE_ID / "stale.h").write_text("old")
      connectors.write(connectors.run(self.project), out)
      self.assertEqual((out / SAMPLE_ID / "party_moods.cpp").stat().st_mtime_ns, stamp)
      self.assertFalse((out / SAMPLE_ID / "stale.h").exists())

   def test_generated_cpp_compiles(self) -> None:
      """!The sample's outputs compile (when a C++ compiler is installed).
      @return None
      """
      compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
      if compiler is None:
         self.skipTest("no C++ compiler")
      self.give_blocks()
      out = self.root / "gen/ext"
      connectors.write(connectors.run(self.project), out)
      done = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                             "-I", str(self.root / "gen"), str(out / connectors.AGGREGATE_NAME)],
                            capture_output=True, text=True)
      self.assertEqual(done.returncode, 0, done.stderr)

   def test_numbers_match_the_compilers(self) -> None:
      """!References resolve to the numbers battle_ids.h and world_ids.h give.
      @return None
      """
      game = connectors.Game(self.project)
      compiled = battle_compiler.compile_battle(self.battle)
      from battlec import emit as battle_emit
      header = battle_emit.emit_ids(self.battle.name, compiled)
      by_prefix: dict[str, list[int]] = {}
      for prefix, number in ID_LINE.findall(header):
         by_prefix.setdefault(prefix, []).append(int(number))
      for kind, prefix in (("species", "SPECIES"), ("move", "MOVE"), ("item", "ITEM"), ("presenter", "PRESENTER")):
         self.assertEqual(list(game.numbers(kind).values()), by_prefix[prefix], kind)
      world = map_compiler.compile_world(self.game / "data/world.json")
      self.assertEqual(game.numbers("map"), {name: n for n, name in enumerate(world["map_names"])})
      self.assertEqual(game.resolve("asset", "sprite:WALKER", "test"),
                       __import__("sequence_compiler").asset_key("sprite:WALKER"))

   # -- refusals --------------------------------------------------------------------------------------------------

   def test_blocks_that_do_not_fit(self) -> None:
      """!Schema errors stop the build before any hook runs, each naming the file, the record and the plugin.
      @return None
      """
      def battle(b: Any) -> None:
         """!Bad blocks."""
         b["parties"][0]["ext"] = {SAMPLE_ID: {"mood": "sleepy", "bonus": 99, "colour": 1}}
         b["parties"][1]["ext"] = {SAMPLE_ID: {"rival": "Nobody"}}
         b["species"][0]["ext"] = {SAMPLE_ID: {}}
         b["moves"][0]["ext"] = {"acme.missing": {}}

      self.edit(self.battle, battle)
      self.edit(self.cave, lambda m: m.update(cell_ext={SAMPLE_ID: [{"at": [99, 99], "mood": "calm"},
                                                                    {"at": [1], "mood": "calm"}]}))
      messages = "\n".join(self.errors(connectors.run(self.project)))
      self.assertIn("parties[0]: ext.sample.party_moods: unknown key(s) colour", messages)
      self.assertIn("does not attach at species", messages)
      self.assertIn("the game loads no plugin 'acme.missing'", messages)
      self.assertIn("cell 99,99 is outside the map's chunks", messages)
      self.assertIn("\"at\" is [x, y]", messages)
      # Schema errors stop the build before references resolve: with only these left, both are found.
      def second(b: Any) -> None:
         """!One bad choice and one bad reference."""
         b["parties"][0]["ext"] = {SAMPLE_ID: {"mood": "sleepy"}}
         b["species"][0].pop("ext")
         b["moves"][0].pop("ext")

      self.edit(self.battle, second)
      self.edit(self.cave, lambda m: m.pop("cell_ext"))
      self.assertIn("one of calm, angry, cheerful is needed, not 'sleepy'",
                    "\n".join(self.errors(connectors.run(self.project))))
      self.edit(self.battle, lambda b: b["parties"][0].pop("ext"))
      self.assertIn("there is no party 'Nobody'", "\n".join(self.errors(connectors.run(self.project))))

   def test_hook_errors(self) -> None:
      """!A hook's error fails the build, naming the plugin; nothing is emitted for it.
      @return None
      """
      self.edit(self.battle, lambda b: b["parties"][1].update(ext={SAMPLE_ID: {"rival": "Scholar Wen"}}))
      result = connectors.run(self.project)
      self.assertIn("an opponent cannot be its own rival", "\n".join(self.errors(result)))
      self.assertEqual(result.outputs, {})
      self.assertEqual(connectors.main(["check", str(self.project)]), 1)

   def plugin(self, connector: str, api: int = 3, extra: dict[str, Any] | None = None) -> Path:
      """!Writes a one-file plugin into the game and lists it alone.
      @param connector  its connector module's source
      @param api        its plugin API
      @param extra      more plugin.json keys
      @return its folder
      """
      folder = self.game / "plugins/probe"
      (folder / "probe").mkdir(parents=True)
      (folder / "probe/__init__.py").write_text("")
      (folder / "probe/connector.py").write_text(connector)
      manifest = {"schema": "capsule.plugin/1", "id": "acme.probe", "version": "2026.11.12.0000", "api": api,
                  "connector": {"module": "probe.connector"}}
      manifest.update(extra or {})
      write_json(folder / "plugin.json", manifest)
      self.edit(self.project, lambda p: p.update(plugins=["plugins/probe"]))
      return folder

   def test_plugin_refusals(self) -> None:
      """!Native code, ctypes, Qt, another API, no attach point, a transform hook, and bad outputs are refused.
      @return None
      """
      cases = [
         ("import ctypes\ndef register(c):\n   pass\n", 3, "imports ctypes"),
         ("from PySide6 import QtCore\ndef register(c):\n   pass\n", 3, "imports PySide6"),
         ("def register(c):\n   c.attach('map', c.block())\n", 2, "for plugin API 2"),
         ("def register(c):\n   pass\n", 3, "attaches nowhere"),
         ("def register(c):\n   c.attach('galaxy', c.block())\n", 3, "'galaxy' is no attach point"),
         ("def register(c):\n   c.attach('map', c.block())\n   c.on_transform(print)\n", 3, "transform hooks"),
         ("raise RuntimeError('boom')\n", 3, "importing probe.connector failed: RuntimeError: boom"),
      ]
      for source, api, message in cases:
         folder = self.plugin(source, api)
         with self.assertRaises(connectors.ConnectorError) as caught:
            connectors.run(self.project)
         self.assertIn(message, str(caught.exception))
         shutil.rmtree(folder)
         for name in [n for n in sys.modules if n.startswith("capsule_connector_")]:
            del sys.modules[name]
      folder = self.plugin("def register(c):\n   c.attach('map', c.block())\n")
      (folder / "native.so").write_bytes(b"")
      with self.assertRaises(connectors.ConnectorError) as caught:
         connectors.run(self.project)
      self.assertIn("native.so is native code", str(caught.exception))

   def test_outputs_and_hook_crashes(self) -> None:
      """!write() takes relative names with known suffixes, once each; a crashing hook is reported, not raised.
      @return None
      """
      self.plugin("def register(c):\n"
                  "   c.attach('world', c.block(n=c.integer(0, 9, default=1)))\n"
                  "   c.on_emit(lambda ctx: [ctx.write(n, 'x') for n in ('a.h', 'a.h')])\n")
      self.edit(self.game / "data/world.json", lambda w: w.update(ext={"acme.probe": {}}))
      self.assertIn("writes a.h twice", "\n".join(self.errors(connectors.run(self.project))))
      for bad in ("../a.h", "/abs.h", "a.exe"):
         shutil.rmtree(self.game / "plugins/probe")
         for name in [n for n in sys.modules if n.startswith("capsule_connector_")]:
            del sys.modules[name]
         self.plugin("def register(c):\n"
                     "   c.attach('world', c.block())\n"
                     f"   c.on_emit(lambda ctx: ctx.write({bad!r}, 'x'))\n")
         self.assertIn("cannot write", "\n".join(self.errors(connectors.run(self.project))), bad)
      shutil.rmtree(self.game / "plugins/probe")
      for name in [n for n in sys.modules if n.startswith("capsule_connector_")]:
         del sys.modules[name]
      self.plugin("def register(c):\n"
                  "   c.attach('world', c.block())\n"
                  "   c.on_validate(lambda r, ctx: 1 / 0)\n")
      self.assertIn("its validate hook failed: ZeroDivisionError", "\n".join(self.errors(connectors.run(self.project))))

   # -- the compilers ---------------------------------------------------------------------------------------------

   def test_compilers_take_blocks_off_only_at_attach_points(self) -> None:
      """!Blocks at attach points compile; an "ext" anywhere else is still an unknown key.
      @return None
      """
      self.give_blocks()
      self.edit(self.cave, lambda m: m["layers"][0].update(ext={SAMPLE_ID: {}}))
      world = map_compiler.compile_world(self.game / "data/world.json")
      self.assertEqual(len(world["map_names"]), len(read_json(self.game / "data/world.json")["maps"]))
      battle_compiler.compile_battle(self.battle)
      structure = self.root / "samples/starter/structure.json"
      self.edit(structure, lambda s: s.update(ext={SAMPLE_ID: {}}))
      with self.assertRaises(battle_compiler.ManifestError) as caught:
         battle_compiler.compile_battle(self.battle)
      self.assertIn("ext", str(caught.exception))
      self.assertEqual(connectors.without_ext({"ext": 1, "a": 2}, "world"), {"a": 2})
      self.assertEqual(connectors.without_ext({"ext": 1, "cell_ext": 2, "layers": [{"ext": 3, "bg": 1}]}, "map"),
                       {"layers": [{"bg": 1}]})

   def test_command_line(self) -> None:
      """!build writes the files and the depfile; attach-points lists them.
      @return None
      """
      self.give_blocks()
      out = self.root / "cli/ext"
      self.assertEqual(connectors.main(["build", str(self.project), "--out-dir", str(out), "--depfile",
                                        str(self.root / "cli/ext.d")]), 0)
      self.assertTrue((out / SAMPLE_ID / "party_moods.h").is_file())
      self.assertTrue((self.root / "cli/ext.d").is_file())
      self.assertEqual(connectors.main(["check", str(self.project)]), 0)
      self.assertIn("cell: a map file", connectors.describe_attach_points())

   def test_game_without_plugins(self) -> None:
      """!A game without plugins builds an empty connectors.cpp; a stray block fails it.
      @return None
      """
      self.edit(self.project, lambda p: p.pop("plugins"))
      result = connectors.run(self.project)
      self.assertEqual(result.problems, [])
      out = self.root / "none/ext"
      connectors.write(result, out)
      self.assertNotIn("#include", (out / connectors.AGGREGATE_NAME).read_text())
      self.give_blocks()
      self.assertTrue(self.errors(connectors.run(self.project)))

   def test_describe_for_the_studio(self) -> None:
      """!A plugin's schemas as plain JSON (what the Studio's inspector builds its forms from).
      @return None
      """
      plugin = connectors.load_plugin(SAMPLE)
      party = plugin.spec.describe()["party"]["fields"]
      self.assertEqual(party["mood"], {"type": "choice", "default": "calm", "title": "Mood",
                                       "help": "How the opponent behaves", "choices": ["calm", "angry", "cheerful"]})
      self.assertEqual(party["bonus"]["maximum"], 20)
      self.assertEqual(party["rival"]["kind"], "party")

   def test_targets_and_block_edits(self) -> None:
      """!The Studio's edits: every record a block can go on, and setting and removing blocks in place (the build
      then reads them back; a file left without blocks has no "ext" or "cell_ext").
      @return None
      """
      game = connectors.Game(self.project)
      parties = [t.name for t in connectors.targets(game, "party")]
      self.assertEqual(parties, [p["name"] for p in read_json(self.battle)["parties"]])
      self.assertIn("cave", [t.name for t in connectors.targets(game, "map")])
      self.assertIn("cave/2", [t.name for t in connectors.targets(game, "layer")])
      self.assertEqual([t.name for t in connectors.targets(game, "project")], ["project"])
      with self.assertRaises(connectors.ConnectorError):
         connectors.targets(game, "nowhere")
      with self.assertRaises(connectors.ConnectorError):
         connectors.find_target(game, "party", "Nobody")
      with self.assertRaises(connectors.ConnectorError):
         connectors.find_target(game, "cell", "cave@3")

      party = connectors.find_target(game, "party", parties[0])
      battle = read_json(self.battle)
      connectors.set_block(battle, party, SAMPLE_ID, {"mood": "angry", "bonus": 2})
      self.assertEqual(connectors.get_block(battle, party, SAMPLE_ID), {"mood": "angry", "bonus": 2})
      write_json(self.battle, battle)

      cell = connectors.find_target(game, "cell", "cave@-3,-4")
      cave = read_json(self.cave)
      connectors.set_block(cave, cell, SAMPLE_ID, {"mood": "calm"})
      connectors.set_block(cave, connectors.find_target(game, "cell", "cave@-5,-4"), SAMPLE_ID, {"mood": "angry"})
      self.assertEqual(connectors.cells_of(cave, SAMPLE_ID), [(-5, -4), (-3, -4)])
      self.assertEqual(connectors.get_block(cave, cell, SAMPLE_ID), {"mood": "calm"})
      write_json(self.cave, cave)

      result = connectors.run(self.project, emit=False)
      self.assertEqual(self.errors(result), [])
      self.assertEqual(sorted(r.name for r in result.records[SAMPLE_ID]),
                       sorted([parties[0], "cave@-3,-4", "cave@-5,-4"]))

      # Removing every block leaves the files as they were.
      connectors.set_block(battle, party, SAMPLE_ID, None)
      self.assertNotIn("ext", battle["parties"][0])
      for at in ("cave@-3,-4", "cave@-5,-4"):
         connectors.set_block(cave, connectors.find_target(game, "cell", at), SAMPLE_ID, None)
      self.assertNotIn("cell_ext", cave)


if __name__ == "__main__":
   unittest.main()
