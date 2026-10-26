#!/usr/bin/env python3
"""!Tests for the inventory section of the battle compiler (D-26 section 2, package G2: tools/battlec/inventory.py):
pockets, the items' bag and use fields against their use_field / use_battle logic, shops, the emitted table and
ids, the effect language's use hooks and `teach`, the script compiler's bag statements, and the save image's
sizes (tools/save_layout.py), on the neutral fixture tests/data/inventory and the Starter sample (run by CTest, or
directly)."""
from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler as bc  # noqa: E402
import build_settings  # noqa: E402
import effect_compiler as ec  # noqa: E402
import save_layout  # noqa: E402
import script_compiler as sc  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## The neutral inventory fixture.
FIXTURE = ROOT / "tests/data/inventory/battle.json"
## Its effect logic.
FIXTURE_LOGIC = ROOT / "tests/data/inventory/logic.ceffect"
## The Starter sample.
STARTER = ROOT / "samples/starter/battle.json"
## --record: rewrite tests/data/inventory/save_image.bin from the Python image (C6: format version 2).
RECORD = "--record" in sys.argv
if RECORD:
   sys.argv.remove("--record")


def compile_with(edit: Callable[[JsonDict], None], logic: str | None = None, platform: str | None = None) -> JsonDict:
   """!Compiles the fixture after an edit of its merged view (and, optionally, other logic), from memory.
   @param edit      callable(merged view)
   @param logic     replacement text of logic.ceffect, or None
   @param platform  target platform, or None (the GBA)
   @return compiled
   """
   data = bc.view_of(FIXTURE)
   edit(data)
   sources = bc.sources_of(FIXTURE, data)
   if logic is not None:
      sources[FIXTURE_LOGIC.resolve()] = logic
   return bc.compile_battle(FIXTURE, None, sources, platform=platform)


def item(data: JsonDict, name: str) -> JsonDict:
   """!An item of a merged view.
   @param data  the view
   @param name  its name
   @return the entry
   """
   return next(i for i in data["items"] if i["name"] == name)


class PocketTests(unittest.TestCase):
   """!The structure's pockets."""

   def test_fixture(self) -> None:
      """!Three pockets laid out one after another; their slots are the bag's."""
      c = bc.compile_battle(FIXTURE)
      inv = c["inventory"]
      self.assertEqual([(p["name"], p["first"], p["capacity"], p["quantity_max"], p["sort"]) for p in inv["pockets"]],
                       [("goods", 0, 4, 5, "added"), ("discs", 4, 3, 1, "index"), ("key", 7, 2, 1, "added")])
      self.assertEqual(inv["slot_count"], 9)
      ids = bc.emit_ids("battle.json", c)
      self.assertIn("inline constexpr uint8_t POCKET_DISCS = 1;", ids)
      self.assertIn("inline constexpr uint8_t SHOP_STALL = 0;", ids)
      self.assertIn("const InventoryData kInventory = {kPockets, kItemInfo, kShops, kShopItems, 10, 9, 3, 1, {0, 0}, "
                    "kItemKeys};",
                    bc.emit("battle.json", c))

   def test_starter(self) -> None:
      """!The Starter's four pockets (D-26 decision 2): Supplies, Orbs, Charms, Key."""
      c = bc.compile_battle(STARTER)
      self.assertEqual([p["label_text"] for p in c["inventory"]["pockets"]], ["Supplies", "Orbs", "Charms", "Key"])
      self.assertEqual([s["name"] for s in c["inventory"]["shops"]], ["Corner Shop", "Charm Stall"])

   def test_refusals(self) -> None:
      """!Names, sort rules, quantities and the platform's caps."""
      def pockets(value: Any) -> Callable[[JsonDict], None]:
         """!An edit setting the pockets.
         @param value  the new pockets
         @return the edit
         """
         def edit(d: JsonDict) -> None:
            """!@param d  the view
            @return None"""
            d["pockets"] = value
            for i in d["items"]:
               i.pop("pocket", None)
            d.pop("shops", None)
         return edit
      cases = [([{"name": "Goods", "capacity": 2}], "lower-case identifier"),
               ([{"name": "a", "capacity": 1}, {"name": "a", "capacity": 1}], "declared twice"),
               ([{"name": "a", "capacity": 1, "sort": "price"}], "sort"),
               ([{"name": "a", "capacity": 1, "quantity_max": 0}], "quantity_max"),
               ([{"name": "a", "capacity": 200}, {"name": "b", "capacity": 100}], "max_bag_slots"),
               ([{"name": f"p{i}", "capacity": 1} for i in range(9)], "max_pockets"),
               ([{"name": "a"}], "capacity")]
      for value, text in cases:
         with self.subTest(text=text), self.assertRaisesRegex(bc.ManifestError, text):
            compile_with(pockets(value))
      # the desktop holds more
      c = compile_with(pockets([{"name": "a", "capacity": 200}, {"name": "b", "capacity": 100}]), platform="desktop")
      self.assertEqual(c["inventory"]["slot_count"], 300)
      self.assertEqual(build_settings.platform_limits("gba")["max_bag_slots"], 256)


class ItemFieldTests(unittest.TestCase):
   """!The items' bag and use fields."""

   def test_fields(self) -> None:
      """!Defaults (no pocket, price 0, use none, consumed and holdable unless key) and the flag bits."""
      c = bc.compile_battle(FIXTURE)
      f = {n: c["inventory"]["items"][i + 1] for i, n in enumerate(c["names"]["items"])}
      self.assertEqual((f["Loose Stone"]["pocket"], f["Loose Stone"]["price"], f["Loose Stone"]["use"]),
                       (bc.NO_POCKET, 0, "none"))
      self.assertEqual((f["Badge"]["key"], f["Badge"]["consumed"], f["Badge"]["holdable"], f["Badge"]["flags"]),
                       (True, False, False, bc.ITEM_FLAG_BITS["key"]))
      self.assertEqual((f["Ether"]["target"], f["Disc Tap"]["consumed"]), ("move_slot", False))
      self.assertIsNone(c["inventory"]["items"][0])

   def test_refusals(self) -> None:
      """!Unknown pocket, use or target, a battler target used out of battle, a consumed key item, a price."""
      cases = [("pocket", "pouch", "unknown pocket"), ("use", "anywhere", "use"), ("target", "self", "target"),
               ("price", -1, "price"), ("consumed", 1, "true or false")]
      for key, value, text in cases:
         with self.subTest(key=key), self.assertRaisesRegex(bc.ManifestError, text):
            compile_with(lambda d, k=key, v=value: item(d, "Charm").update({k: v}))
      with self.assertRaisesRegex(bc.ManifestError, "battle's target"):
         compile_with(lambda d: item(d, "Salve").update({"target": "battler"}))
      with self.assertRaisesRegex(bc.ManifestError, "never consumed"):
         compile_with(lambda d: item(d, "Badge").update({"consumed": True}))
      with self.assertRaisesRegex(bc.ManifestError, "unknown key"):
         compile_with(lambda d: item(d, "Charm").update({"weight": 3}))

   def test_logic_agrees(self) -> None:
      """!A use needs its hook; a hook of a use the item does not have is refused."""
      with self.assertRaisesRegex(bc.ManifestError, "needs an `on use_field`"):
         compile_with(lambda d: item(d, "Charm").update({"use": "field", "target": "member"}))
      with self.assertRaisesRegex(bc.ManifestError, "needs an `on use_battle`"):
         compile_with(lambda d: item(d, "Ether").update({"use": "both"}))
      with self.assertRaisesRegex(bc.ManifestError, "would never run"):
         compile_with(lambda d: item(d, "Salve").update({"use": "field"}))

   def test_held(self) -> None:
      """!A party member holds only holdable items."""
      def hold(d: JsonDict) -> None:
         """!@param d  the view
         @return None"""
         d["parties"][0]["members"][0]["item"] = "Badge"
      with self.assertRaisesRegex(bc.ManifestError, "not holdable"):
         compile_with(hold)

   def test_shops(self) -> None:
      """!Shop items exist, have a price and a pocket, once each."""
      cases = [(["Nothing"], "unknown item"), (["Salve", "Salve"], "twice"), (["Badge"], "no price"),
               (["Loose Stone"], "no price")]
      for items, text in cases:
         with self.subTest(text=text), self.assertRaisesRegex(bc.ManifestError, text):
            compile_with(lambda d, it=items: d.update({"shops": [{"name": "S", "items": it}]}))
      with self.assertRaisesRegex(bc.ManifestError, "no pocket"):
         compile_with(lambda d: (item(d, "Loose Stone").update({"price": 5}),
                                 d.update({"shops": [{"name": "S", "items": ["Loose Stone"]}]})))


class LogicTests(unittest.TestCase):
   """!The effect language's use hooks and `teach`."""

   def test_hooks_and_teach(self) -> None:
      """!use_field / use_battle are item hooks; teach compiles to Op::Teach with the move as its operand."""
      self.assertEqual(ec.HOOKS[ec.HOOK["use_field"]]["owners"], {"item"})
      self.assertEqual(ec.HOOKS[ec.HOOK["use_field"] + 1]["name"], "use_battle")
      c = bc.compile_battle(FIXTURE)
      ops = [name for _at, name, _v in ec.instructions(c["effects"]["bank"].code)]
      self.assertIn("Teach", ops)
      self.assertEqual(ec.OPS[ec.OP["Teach"]], ("Teach", ["subj"], 1, 1))
      # a move cannot have the item hook
      with self.assertRaisesRegex(bc.ManifestError, "not `on use_field`"):
         compile_with(lambda d: None,
                      FIXTURE_LOGIC.read_text() + '\nmove "Tap" { on use_field { restore user hp 1; } }\n')


class TeachableTests(unittest.TestCase):
   """!C6 (review4 finding 23): species' teachable moves."""

   def test_fixture(self) -> None:
      """!Blob lists Burst and Tap (sorted ids); Cube lists none: its range is empty."""
      c = bc.compile_battle(FIXTURE)
      blob = next(sp for sp in c["species"] if sp["name"] == "Blob")
      cube = next(sp for sp in c["species"] if sp["name"] == "Cube")
      moves = c["names"]["moves"]
      self.assertEqual(blob["teachable"], sorted([moves.index("Burst"), moves.index("Tap")]))
      self.assertIsNone(cube["teachable"])
      cpp = bc.emit("battle.json", c)
      self.assertIn("const uint16_t kTeachFirst[] = {", cpp)
      self.assertIn(", kTeachFirst, kTeachMoves,", cpp)

   def test_refusals(self) -> None:
      """!An unknown move or one listed twice is refused; without any list nothing is emitted."""
      def species(data: JsonDict, name: str) -> JsonDict:
         """!A species of the view."""
         return next(sp for sp in data["species"] if sp["name"] == name)
      with self.assertRaisesRegex(bc.ManifestError, "teachable"):
         compile_with(lambda d: species(d, "Cube").update(teachable=["Tap", "Tap"]))
      with self.assertRaises(bc.ManifestError):
         compile_with(lambda d: species(d, "Cube").update(teachable=["Nothing"]))
      c = compile_with(lambda d: species(d, "Blob").pop("teachable"))
      self.assertIn(", nullptr, nullptr,", bc.emit("battle.json", c))


class ScriptTests(unittest.TestCase):
   """!The script compiler's bag statements against a battle manifest."""

   def compile_script(self, text: str) -> sc.Compiler:
      """!Compiles one script source with the fixture as its battle.
      @param text  .cscript text
      @return the compiler
      """
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         (folder / "s.cscript").write_text(text, encoding="utf-8")
         manifest = {"schema": "capsule.scripts/1", "sources": ["s.cscript"], "battle": str(FIXTURE)}
         (folder / "scripts.json").write_text(json.dumps(manifest), encoding="utf-8")
         return sc.compile_file(folder / "scripts.json")

   def test_statements(self) -> None:
      """!Item names resolve to ids from 1, shops by name; open_bag and shop wait; unknown names are errors."""
      comp = self.compile_script('var coins : u16;\nvar n : u8;\nscript s {\n'
                                 '    give_item("Ether", 2) -> var(n);\n    take_item("Ether");\n'
                                 '    count_item(3) -> var(n);\n    open_bag -> var(n);\n'
                                 '    shop "Stall" money var(coins);\n    time_of_day -> var(n);\n}\n')
      self.assertEqual([d.message for d in comp.errors()], [])
      code = comp.result["code"]
      self.assertIn(sc.OP["ItemGive"], code)
      self.assertIn(sc.OP["Shop"], code)
      bad = self.compile_script('var n : u8;\nscript s {\n    give_item("Nothing") -> var(n);\n'
                                '    shop "Market" money var(n);\n}\n')
      self.assertEqual(len(bad.errors()), 2)
      waits = self.compile_script('var n : u8;\nscript s { open_bag -> var(n); }\n'
                                  'map_scripts room { on_load -> s; }\n')
      self.assertTrue(any("wait" in d.message for d in waits.errors()))

   def test_names_stay_free(self) -> None:
      """!The statements are not keywords: an older script named shop still compiles."""
      comp = self.compile_script("script shop { end; }\n")
      self.assertEqual([d.message for d in comp.errors()], [])


class SaveLayoutTests(unittest.TestCase):
   """!tools/save_layout.py: the save image's sizes (capsule/save/save.h)."""

   def test_fixture(self) -> None:
      """!Bag 9 slots of 6 bytes after its count, party 3 records of 44 bytes after its head, no boxes, no scripts
      (tests/inventory_tests.cpp checks the same sizes in C++): the sections present take a table entry each."""
      r = save_layout.layout(FIXTURE, platform="headless")
      self.assertEqual(r["sections"], {"bag": 2 + 9 * 6, "party": 7 + 3 * 44, "boxes": 0, "script": 0, "nursery": 0})
      self.assertEqual(r["image"], save_layout.HEADER_BYTES + 2 * save_layout.SECTION_ENTRY_BYTES + 56 + 139)
      self.assertEqual((r["budget"], r["copies"]), (build_settings.platform_limits("headless")["max_save_bytes"], 2))
      self.assertTrue(r["ok"])

   def test_media(self) -> None:
      """!C6 (review4 finding 4): the GBA's flash keeps whole 4 KB sectors per copy, its SRAM is its own medium
      (32 KB, byte units); a medium with room for one copy fails the layout unless the game opts in."""
      flash = save_layout.layout(FIXTURE, platform="gba")
      sram = save_layout.layout(FIXTURE, platform="gba_sram")
      self.assertEqual((flash["budget"], flash["unit"], flash["copies"]), (0x10000, 0x1000, 2))
      self.assertEqual((sram["budget"], sram["unit"], sram["copies"]), (0x8000, 1, 2))
      self.assertEqual(save_layout.medium("headless")[1:], (build_settings.platform_limits("headless")
                                                             ["max_save_bytes"], 1))
      with self.assertRaises(build_settings.SettingsError):
         save_layout.medium("cartridge")

   def test_one_copy_fails_without_opting_in(self) -> None:
      """!An image that fits the medium once: the command fails, --single-copy lets it through."""
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         for name in ("battle.json", "logic.ceffect", "ruleset.json"):
            (folder / name).write_bytes((FIXTURE.parent / name).read_bytes())
         structure = json.loads((FIXTURE.parent / "structure.json").read_text(encoding="utf-8"))
         # 20 KB of boxed records: one copy of the image fits the 32 KB SRAM, two do not
         structure["storage"] = {"box_count": 8, "box_size": 60}
         (folder / "structure.json").write_text(json.dumps(structure), encoding="utf-8")
         r = save_layout.layout(folder / "battle.json", platform="gba_sram")
         self.assertEqual(r["copies"], 1)
         self.assertFalse(r["ok"])
         self.assertTrue(save_layout.layout(folder / "battle.json", platform="gba_sram", single_copy=True)["ok"])
         out = folder / "report.txt"
         args = ["save_layout.py", str(folder / "battle.json"), "--platform", "gba_sram", "--out", str(out)]
         with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(save_layout.main(args), save_layout.EXIT_FAILED)
            self.assertEqual(save_layout.main(args + ["--single-copy"]), 0)
         self.assertIn("single_copy", err.getvalue())
         self.assertIn("1 copies, single copy allowed", out.read_text(encoding="utf-8"))

   def test_image(self) -> None:
      """!The Python image is the engine's byte for byte (tests/inventory_tests.cpp compares the C++ write with the
      same file): three Salves, Disc Tap and the Badge, no party, sequence 1."""
      c = bc.compile_battle(FIXTURE, platform="headless")
      names = c["names"]["items"]
      bag = {0: [(names.index("Salve") + 1, 3)], 1: [(names.index("Disc Tap") + 1, 1)],
             2: [(names.index("Badge") + 1, 1)]}
      data = save_layout.image(c, bag)
      if RECORD:
         (FIXTURE.parent / "save_image.bin").write_bytes(data)
      self.assertEqual(data, (FIXTURE.parent / "save_image.bin").read_bytes())
      # a sorted pocket keeps the engine's order whatever order the slots come in
      self.assertEqual(save_layout.pocket_order(c, 1, [(6, 1), (5, 1)]), [(5, 1), (6, 1)])

   def test_starter_fits_gba(self) -> None:
      """!The Starter's image fits the GBA's 64 KB flash twice."""
      r = save_layout.layout(STARTER)
      self.assertLessEqual(r["image"], r["budget"])
      self.assertEqual((r["platform"], r["copies"]), ("gba", 2))


if __name__ == "__main__":
   unittest.main()
