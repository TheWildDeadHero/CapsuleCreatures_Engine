#!/usr/bin/env python3
"""!Tests for record transfer v2 in Python: tools/record_transfer.py (the mirror of src/monster/transfer.cpp) and
the compiler's keys and tables (tools/battlec/layout.py, tools/battlec/transfer.py). Run by CTest, or directly;
`--record` rewrites tests/data/transfer/golden.json (the canonical bytes of every case, which pin the format).

tests/transfer_tests.cpp checks the C++ export and import against this mirror's results on the same cases, byte
for byte.
"""
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
sys.path.insert(0, str(ROOT / "tools"))
import record_transfer as rt  # noqa: E402
from battlec import transfer as bt  # noqa: E402
from battlec.common import ManifestError  # noqa: E402
from battlec.layout import compile_layout, name_hash, role_key, stat_key  # noqa: E402

## The fixtures and cases.
DATA = ROOT / "tests" / "data" / "transfer"
## The recorded canonical bytes per case.
GOLDEN = DATA / "golden.json"
## Platform the tests compile for (the build's headless limits).
PLATFORM = "headless"
## Two custom role names whose transfer keys collide (FNV-1a 32 of "role:" + name).
ROLE_COLLISION = ("oujabrbq", "knxmcezt")
## Two stat transfer keys that collide (FNV-1a 32 of "stat:" + key).
STAT_COLLISION = ("ycxlrixs", "jbddtevk")


def load_cases() -> tuple[dict[str, Any], dict[str, Any]]:
   """!The case file and its tables.
   @return (case file, {table name: transfer table})
   """
   spec = json.loads((DATA / "cases.json").read_text(encoding="utf-8"))
   tables = {n: rt.load(DATA / t["source"], PLATFORM) for n, t in spec["tables"].items()}
   return spec, tables


def case_bytes(spec: dict[str, Any], tables: dict[str, Any]) -> dict[str, str]:
   """!The canonical bytes of every case (hex).
   @param spec    case file
   @param tables  its tables
   @return {case name: hex}
   """
   return {c["name"]: rt.export_record(tables[c["from"]], spec["records"][c["record"]]).hex() for c in spec["cases"]}


def fixture(name: str) -> dict[str, Any]:
   """!A fixture's JSON.
   @param name  file stem
   @return the fixture
   """
   return json.loads((DATA / f"{name}.json").read_text(encoding="utf-8"))


class Keys(unittest.TestCase):
   """Transfer keys and their collision checks."""

   def test_keys_are_prefixed_fnv(self) -> None:
      """A role key is FNV-1a of "role:" + name, a stat key of "stat:" + key (capsule::monster::transfer_key)."""
      self.assertEqual(role_key("friendship"), name_hash("role:friendship"))
      self.assertEqual(stat_key("hp"), name_hash("stat:hp"))
      self.assertNotEqual(role_key("hp"), stat_key("hp"))
      h = 0x811C9DC5
      for b in b"stat:atk":
         h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
      self.assertEqual(stat_key("atk"), h)

   def test_collisions_are_real(self) -> None:
      """The test pairs collide (so the checks below test something)."""
      self.assertEqual(role_key(ROLE_COLLISION[0]), role_key(ROLE_COLLISION[1]))
      self.assertEqual(stat_key(STAT_COLLISION[0]), stat_key(STAT_COLLISION[1]))

   def test_custom_role_collision_is_an_error(self) -> None:
      """compile_layout rejects custom roles whose keys collide (every battle compile)."""
      roles = ["species_key", "level", "experience", "moves", "pp", "current_hp", "primary_status"]
      roles += [{"custom": ROLE_COLLISION[0], "width": 1}, {"custom": ROLE_COLLISION[1], "width": 1}]
      with self.assertRaisesRegex(ManifestError, "same transfer key"):
         compile_layout(roles, "test")
      compile_layout(roles[:-1], "test")

   def test_stat_collision_is_an_error(self) -> None:
      """compile_transfer rejects stats whose transfer keys collide."""
      data = fixture("four")
      data["stats"][0]["transfer"] = STAT_COLLISION[0]
      data["stats"][1]["transfer"] = STAT_COLLISION[1]
      with self.assertRaisesRegex(ManifestError, "same transfer key"):
         bt.load_fixture(data, "four.json")


class Tables(unittest.TestCase):
   """compile_transfer / load_fixture / emit_cpp."""

   def test_orders_and_defaults(self) -> None:
      """Stats and custom roles are ordered by key; transfer_defaults expand per per-stat role."""
      t = bt.load_fixture(fixture("six"), "six.json")
      self.assertEqual([t["stat_keys"][i] for i in t["stat_order"]], sorted(t["stat_keys"]))
      self.assertEqual([t["custom_keys"][i] for i in t["custom_order"]], sorted(t["custom_keys"]))
      spa, spd = t["stat_names"].index("spa"), t["stat_names"].index("spd")
      self.assertEqual(t["stat_defaults"][spa], [7, 0, 0])
      self.assertEqual(t["stat_defaults"][spd], [3, 3, 0])   # an int: every held per-stat role (no awakening)
      self.assertEqual(t["layout"]["hash"], 0x62409274)       # six's own layout hash: defaults are not folded

   def test_sample_and_its_twin_share_the_hash(self) -> None:
      """The Starter sample's battle data and the twin fixture have one transfer hash (the raw form works between
      them)."""
      _spec, tables = load_cases()
      self.assertEqual(tables["sample"]["hash"], tables["twin"]["hash"])
      self.assertNotEqual(tables["sample"]["hash"], tables["six"]["hash"])
      self.assertNotEqual(tables["six"]["hash"], tables["shuffled"]["hash"])
      self.assertNotEqual(tables["six"]["hash"], tables["four"]["hash"])

   def test_bad_transfer_defaults(self) -> None:
      """transfer_defaults: an object, per-stat roles the layout holds, values that fit, stats that exist."""
      def bad(change: Any, pattern: str) -> None:
         data = fixture("six")
         data["monster_layout"]["transfer_defaults"] = change
         with self.assertRaisesRegex(ManifestError, pattern):
            bt.load_fixture(data, "six.json")
      bad([1], "an object")
      bad({"spa": {"stat_awaken": 1}}, "no per-stat role")
      bad({"spa": {"level": 1}}, "no per-stat role")
      bad({"spa": 32}, "stat_genes")
      bad({"luck": 1}, "names no stat: luck")

   def test_emit(self) -> None:
      """The build's table names kLayout; a fixture's has its own layout and a header."""
      _spec, tables = load_cases()
      build = bt.emit_cpp("battle.json", tables["sample"])
      self.assertIn("const TransferInfo kTransfer = {&kLayout,", build)
      self.assertIn("namespace capsule::monster", build)
      self.assertNotIn("const Layout kLayout", build)
      self.assertIn("nullptr, 0x", build)   # all-0 stat defaults
      four = bt.emit_cpp("four.json", tables["four"], "four")
      self.assertIn("const Layout kLayout = {kOffsets, kWidths, kCounts, kCustom, 0xAC286074u, 32,", four)
      six = bt.emit_cpp("six.json", tables["six"], "six")
      self.assertIn("const uint8_t kStatDefaults[] = {", six)
      self.assertIn("extern const capsule::monster::TransferInfo kTransfer;", bt.emit_header("six"))

   def test_emit_unstored(self) -> None:
      """C5: a fixture's unstored custom role (E13a "stored": false) is a RoleSlot without bits after the stored ones
      and counts in Layout::unstored_count; the layout hash and the transfer table do not change."""
      data = fixture("six")
      data["monster_layout"]["roles"].append({"custom": "lucky", "width": 1, "stored": False})
      t = bt.load_fixture(data, "six.json")
      six = bt.load_fixture(fixture("six"), "six.json")
      self.assertEqual((t["hash"], t["custom_keys"]), (six["hash"], six["custom_keys"]))
      out = bt.emit_cpp("six.json", t, "six")
      self.assertIn(f"{bt.ABSENT}, 1, 1}},   // lucky (unstored)", out)
      stored = len(t["layout"]["custom"])
      self.assertIn(f", {stored}, 1, {{0}}, 0x{t['layout']['record_hash']:08X}u}};", out)
      plain = bt.emit_cpp("six.json", six, "six")
      self.assertIn(f", {stored}, 0, {{0}}, 0x{six['layout']['record_hash']:08X}u}};", plain)


class Mirror(unittest.TestCase):
   """The export / import rule (the same as the C++)."""

   @classmethod
   def setUpClass(cls) -> None:
      """Loads the cases once."""
      cls.spec, cls.tables = load_cases()

   def test_golden_bytes(self) -> None:
      """Every case's canonical bytes are the recorded ones (the format is pinned)."""
      recorded = json.loads(GOLDEN.read_text(encoding="utf-8"))["cases"]
      self.assertEqual(case_bytes(self.spec, self.tables), recorded)

   def test_layout_independent(self) -> None:
      """Two layouts holding the same values in another order write the same bytes."""
      record = self.spec["records"]["six"]
      self.assertEqual(rt.export_record(self.tables["six"], record), rt.export_record(self.tables["shuffled"], record))
      starter = self.spec["records"]["starter"]
      self.assertEqual(rt.export_record(self.tables["twin"], starter), rt.export_record(self.tables["sample"], starter))

   def test_same_layout_round_trip(self) -> None:
      """Same layout: the record comes back unchanged, no flags; raw and canonical agree."""
      for name in ("six", "four"):
         t = self.tables[name]
         record = self.spec["records"][name]
         packed = rt.pack(t, record)
         for raw in (False, True):
            back, flags, read = rt.import_bytes(t, rt.export_bytes(t, packed, raw))
            self.assertEqual(back, packed)
            self.assertEqual(flags, rt.RAW if raw else 0)
            self.assertEqual(read, rt.transfer_size(t, raw))

   def test_six_to_four(self) -> None:
      """Stats by key (spa, spd dropped), narrowed widths, two of four slots, one-sided custom roles."""
      sent = rt.export_record(self.tables["six"], self.spec["records"]["six"])
      rec, flags = rt.import_record(self.tables["four"], sent)
      self.assertEqual(rt.flag_names(flags), ["narrowed", "dropped", "defaulted"])
      self.assertEqual(rec["stat_genes"], {"vigor": 15, "power": 15, "def": 3, "pace": 15})
      self.assertEqual(rec["stat_training"], {"vigor": 252, "power": 6, "def": 0, "pace": 255})
      self.assertEqual(rec["moves"], [1, 300])
      self.assertEqual(rec["pp"], [10, 20])
      self.assertEqual(rec["held_item"], 255)
      self.assertEqual((rec["friendship"], rec["met_level"], rec["gender"]), (63, 5, 0))
      self.assertEqual(rec["nickname"], [10, 11, 12, 13, 14])

   def test_four_to_six(self) -> None:
      """Missing stats take transfer_defaults; missing slots are "no move"; the nickname is padded with end codes."""
      sent = rt.export_record(self.tables["four"], self.spec["records"]["four"])
      rec, flags = rt.import_record(self.tables["six"], sent)
      self.assertEqual(rt.flag_names(flags), ["dropped", "defaulted"])
      self.assertEqual(rec["stat_genes"], {"hp": 15, "atk": 9, "def": 0, "spa": 7, "spd": 3, "spe": 7})
      self.assertEqual(rec["stat_training"], {"hp": 1, "atk": 2, "def": 3, "spa": 0, "spd": 3, "spe": 4})
      self.assertEqual(rec["moves"], [17, 0xFFFF, 0xFFFF, 0xFFFF])   # 10-bit "no move" widened stays "no move"
      self.assertEqual(rec["nickname"], [20, 21, 22, 23, 24] + [0xFF] * 7)
      self.assertEqual((rec["friendship"], rec["nicknamed"], rec["seed"]), (63, 0, 0))

   def test_empty_elements_are_no_loss(self) -> None:
      """Empty move slots and end codes past the receiver's count are not "dropped"; a real move is."""
      t4 = self.tables["four"]
      data = fixture("four")
      data["monster_layout"]["move_slots"] = 4
      data["monster_layout"]["roles"][-1] = "nickname"   # 12 codes
      wide = bt.load_fixture(data, "four_wide.json")
      short = {"moves": [3, 1023, 1023, 1023], "pp": [9, 0, 0, 0], "nickname": [1] + [255] * 11}
      rec, flags = rt.import_record(t4, rt.export_record(wide, short))
      self.assertEqual(flags, 0)
      self.assertEqual((rec["moves"], rec["nickname"]), ([3, 1023], [1, 255, 255, 255, 255]))
      _rec, flags = rt.import_record(t4, rt.export_record(wide, {**short, "moves": [3, 1023, 9, 1023]}))
      self.assertEqual(rt.flag_names(flags), ["dropped"])
      _rec, flags = rt.import_record(t4, rt.export_record(wide, {**short, "pp": [9, 0, 0, 1]}))
      self.assertEqual(rt.flag_names(flags), ["dropped"])

   def test_errors(self) -> None:
      """Truncated input, unknown versions, malformed shapes, raw records of another layout."""
      t6, t4 = self.tables["six"], self.tables["four"]
      data = rt.export_record(t6, self.spec["records"]["six"])
      for n in range(len(data)):
         with self.assertRaisesRegex(rt.TransferError, "Truncated"):
            rt.import_bytes(t4, data[:n])
      self.assertEqual(rt.import_bytes(t4, data + b"\0\0")[2], len(data))
      with self.assertRaisesRegex(rt.TransferError, "BadVersion"):
         rt.import_bytes(t4, b"\x03" + data[1:])
      bad = bytearray(data)
      bad[4] |= 0x80   # a padding bit of the mask
      with self.assertRaisesRegex(rt.TransferError, "BadShape"):
         rt.import_bytes(t4, bytes(bad))
      bad = bytearray(data)
      bad[6:10], bad[10:14] = data[10:14], data[6:10]   # custom keys out of order
      with self.assertRaisesRegex(rt.TransferError, "BadShape"):
         rt.import_bytes(t4, bytes(bad))
      raw = rt.export_record(t6, self.spec["records"]["six"], raw=True)
      with self.assertRaisesRegex(rt.TransferError, "OtherLayout"):
         rt.import_bytes(t4, raw)
      twin_raw = rt.export_record(self.tables["twin"], self.spec["records"]["starter"], raw=True)
      self.assertEqual(rt.import_bytes(self.tables["sample"], twin_raw)[1], rt.RAW)

   def test_pack_checks(self) -> None:
      """pack rejects roles and stats the layout does not hold and values that do not fit."""
      t4 = self.tables["four"]
      with self.assertRaisesRegex(ValueError, "no role 'seed'"):
         rt.pack(t4, {"seed": 1})
      with self.assertRaisesRegex(ValueError, "no stat hp"):
         rt.pack(t4, {"stat_genes": {"hp": 1}})
      with self.assertRaisesRegex(ValueError, "does not fit"):
         rt.pack(t4, {"stat_genes": {"vigor": 16}})
      with self.assertRaisesRegex(ValueError, "at most 2"):
         rt.pack(t4, {"moves": [1, 2, 3]})
      self.assertEqual(rt.unpack(t4, rt.new_record(t4))["met_level"], 5)


class EggTransfer(unittest.TestCase):
   """C9: an egg (the role keyed by the eggs' transfer name set) sent to a layout without an egg role."""

   def test_egg_to_a_layout_without_eggs(self) -> None:
      """The import flags it (EGG); import_rederived() refuses it unless the receiver's transfer_eggs is "hatch"."""
      starter = rt.load(ROOT / "samples" / "starter" / "battle.json", PLATFORM)
      records = ROOT / "tests" / "data" / "battle_progress" / "records.json"
      receiver = rt.load(records, PLATFORM)
      self.assertIn(role_key("egg"), starter["custom_keys"])
      self.assertNotIn(role_key("egg"), receiver["custom_keys"])
      record = {"species_key": 7, "seed": 11, "level": 3, "egg": 1, "hatch_counter": 40}
      sent = rt.export_record(starter, record)
      _rec, flags = rt.import_record(receiver, sent)
      self.assertTrue(flags & rt.EGG and flags & rt.DROPPED)
      _rec, plain = rt.import_record(receiver, rt.export_record(starter, {**record, "egg": 0}))
      self.assertFalse(plain & rt.EGG)
      # the Starter receives its own eggs: no flag
      _rec, own = rt.import_record(starter, sent)
      self.assertFalse(own & rt.EGG)
      battle = rt.battle_compiler.compile_battle(records, None, platform=PLATFORM)
      with self.assertRaisesRegex(rt.TransferError, "EggRefused"):
         rt.import_rederived(receiver, sent, battle)
      hatch = {**battle, "combine": {**battle["combine"], "transfer_flags": rt.EGG_FLAG_TRANSFER_HATCH}}
      _rec, flags = rt.import_rederived(receiver, sent, hatch)
      self.assertTrue(flags & rt.EGG)


class CommandLine(unittest.TestCase):
   """record_transfer.py emit / export / import."""

   def run_tool(self, *args: str) -> subprocess.CompletedProcess[str]:
      """Runs the tool.
      @param args  arguments
      @return the finished process
      """
      return subprocess.run([sys.executable, str(ROOT / "tools" / "record_transfer.py"), *args], capture_output=True,
                            text=True)

   def test_export_import(self) -> None:
      """export prints hex, import prints the record and flags."""
      spec, _tables = load_cases()
      with tempfile.TemporaryDirectory() as tmp:
         record = Path(tmp) / "record.json"
         record.write_text(json.dumps(spec["records"]["six"]), encoding="utf-8")
         out = self.run_tool("export", str(DATA / "six.json"), str(record), "--platform", PLATFORM)
         self.assertEqual(out.returncode, 0, out.stderr)
         self.assertEqual(out.stdout.strip(), json.loads(GOLDEN.read_text(encoding="utf-8"))["cases"]["six_same"])
         back = self.run_tool("import", str(DATA / "four.json"), out.stdout.strip(), "--platform", PLATFORM)
         self.assertEqual(back.returncode, 0, back.stderr)
         self.assertEqual(json.loads(back.stdout)["flags"], ["narrowed", "dropped", "defaulted"])
         bad = self.run_tool("import", str(DATA / "four.json"), "03", "--platform", PLATFORM)
         self.assertEqual(bad.returncode, 1)
         self.assertIn("BadVersion", bad.stderr)

   def test_emit_writes_a_depfile(self) -> None:
      """emit of a battle manifest writes the table and a depfile naming the manifest."""
      with tempfile.TemporaryDirectory() as tmp:
         out, dep = Path(tmp) / "t.cpp", Path(tmp) / "t.d"
         r = self.run_tool("emit", str(ROOT / "samples" / "starter" / "battle.json"), str(out),
                           "--platform", PLATFORM, "--depfile", str(dep))
         self.assertEqual(r.returncode, 0, r.stderr)
         self.assertIn("kTransfer", out.read_text(encoding="utf-8"))
         self.assertIn("battle.json", dep.read_text(encoding="utf-8"))
      self.assertEqual(self.run_tool("emit").returncode, rt.EXIT_USAGE)


def record() -> None:
   """!Rewrites golden.json from the mirror.
   @return None
   """
   spec, tables = load_cases()
   GOLDEN.write_text(json.dumps({"schema": "capsule.transfer_golden/1", "cases": case_bytes(spec, tables)},
                                indent=1) + "\n", encoding="utf-8")
   print(f"{GOLDEN}: {len(spec['cases'])} cases recorded")


if __name__ == "__main__":
   if "--record" in sys.argv:
      record()
   else:
      unittest.main()
