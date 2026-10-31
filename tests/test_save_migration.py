#!/usr/bin/env python3
"""!Save images of older manifest shapes (C6, review4 findings 1 and 2): the Python writer (tools/save_layout.py)
makes images from changed copies of the recruit fixture (tests/data/recruit) and the save fixture's script banks
(tests/data/save: scripts.json, save.cscript, save_history.json), and tests/save_tests.cpp loads them into the
current build: items reordered, one removed and one added since, a pocket added, a lowered quantity_max, a box
geometry changed (and one that no longer fits), the script banks of recorded and unrecorded shapes. C7 (the final
review's repros): parties of another party size (grown, shrunk, one with more members than the build holds), a
variable whose range was narrowed, two copies of different sizes and sequences either side of the wrap.

The images are kept in tests/data/save/*.bin; this test rewrites them with --record and otherwise checks that the
writer still makes the same bytes. It also checks tools/save_shape.py (shapes, history, remaps) on its own.
"""
from __future__ import annotations

import copy
import json
import re
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler as bc  # noqa: E402
import save_layout as sl  # noqa: E402
import save_shape  # noqa: E402
import script_compiler as sc  # noqa: E402
from battlec import inventory as battle_inventory  # noqa: E402

## The recruit fixture: the current build's battle data.
RECRUIT = ROOT / "tests/data/recruit"
## The save fixture (scripts, history, the images).
SAVE = ROOT / "tests/data/save"
## The platform the fixture's images are made for (the test build's).
PLATFORM = "headless"
## --record: rewrite the images.
RECORD = "--record" in sys.argv
if RECORD:
   sys.argv.remove("--record")

## A parsed JSON object.
JsonDict = dict[str, Any]


def variant(edit_battle: Callable[[JsonDict], None] | None = None,
            edit_structure: Callable[[JsonDict], None] | None = None,
            edit_logic: Callable[[str], str] | None = None) -> JsonDict:
   """!The recruit fixture compiled after edits (an older shape of it).
   @param edit_battle     callable(battle.json) or None
   @param edit_structure  callable(structure.json) or None
   @param edit_logic      callable(logic text) -> logic text, or None
   @return bc.compile_battle() result
   """
   with tempfile.TemporaryDirectory() as tmp:
      folder = Path(tmp)
      battle = json.loads((RECRUIT / "battle.json").read_text(encoding="utf-8"))
      structure = json.loads((RECRUIT / "structure.json").read_text(encoding="utf-8"))
      logic = (RECRUIT / "logic.ceffect").read_text(encoding="utf-8")
      if edit_battle is not None:
         edit_battle(battle)
      if edit_structure is not None:
         edit_structure(structure)
      if edit_logic is not None:
         logic = edit_logic(logic)
      (folder / "battle.json").write_text(json.dumps(battle), encoding="utf-8")
      (folder / "structure.json").write_text(json.dumps(structure), encoding="utf-8")
      (folder / "logic.ceffect").write_text(logic, encoding="utf-8")
      (folder / "ruleset.json").write_bytes((RECRUIT / "ruleset.json").read_bytes())
      return bc.compile_battle(folder / "battle.json", platform=PLATFORM)


def item_id(c: JsonDict, name: str) -> int:
   """!An item's id in a compiled battle.
   @param c     compiled battle
   @param name  item name
   @return its id
   """
   return c["names"]["items"].index(name) + 1


def pocket_of(c: JsonDict, name: str) -> int:
   """!A pocket's index in a compiled battle.
   @param c     compiled battle
   @param name  pocket name
   @return its index
   """
   return [p["name"] for p in c["inventory"]["pockets"]].index(name)


def bag_of(c: JsonDict, slots: dict[str, list[tuple[str, int]]]) -> dict[int, list[tuple[int, int]]]:
   """!A bag by names.
   @param c      compiled battle
   @param slots  {pocket name: [(item name, count)]}
   @return {pocket index: [(item id, count)]}
   """
   return {pocket_of(c, p): [(item_id(c, i), n) for i, n in items] for p, items in slots.items()}


def old_items(battle: JsonDict) -> None:
   """!The older item list: Ether before Salve, no Beacon yet, a Relic since removed.
   @param battle  battle.json
   @return None
   """
   items = {i["name"]: i for i in battle["items"]}
   relic = {"name": "Relic", "pocket": "goods", "price": 1}
   battle["items"] = [items["Ether"], items["Salve"], items["Lure"], items["Snare"], items["Badge"], relic]
   for p in battle["parties"]:
      if "items" in p:
         p["items"] = [i for i in p["items"] if i != "Beacon"]
   for prof in battle.get("ai_profiles", []):
      if isinstance(prof, dict) and "items" in prof:
         prof["items"] = [r for r in prof["items"] if not (isinstance(r, dict) and r.get("item") == "Beacon")]


def without_beacon(logic: str) -> str:
   """!The logic before Beacon existed.
   @param logic  logic.ceffect text
   @return the text without Beacon's owner
   """
   return re.sub(r'(#[^\n]*\n)?item "Beacon" \{.*?\n\}\n', "", logic, flags=re.S)


def script_banks() -> list[JsonDict]:
   """!The save fixture's current sub-banks.
   @return save_shape.shapes() of tests/data/save/scripts.json
   """
   comp = sc.compile_file(SAVE / "scripts.json")
   assert not comp.errors(), comp.errors()
   return save_shape.shapes(comp)


def old_script_payload() -> bytes:
   """!A Script section in the recorded older shapes (permanent flags, permanent variables, the name slot), the
   daily flags in today's shape and the map variables in a shape nobody recorded.
   @return the section's bytes
   """
   history: dict[str, JsonDict] = {}
   for s in save_shape.load_history(SAVE / "save_history.json"):
      history.setdefault(s["bank"], s)   # the first recorded shape of each bank (the oldest)
   labels = save_shape.sub_bank_names()
   current = {s["bank"]: s for s in script_banks()}

   def sub(label: str, h: int, data: bytes) -> bytes:
      """!One sub-bank."""
      return bytes([labels.index(label)]) + sl.u32(h) + sl.u16(len(data)) + data

   flags = bytes([0b111])                                    # met (bit 0), gone (bit 1, dropped) and lit (bit 2)
   variables = sl.u16(700) + sl.u16(900) + bytes([7, 200])    # coins 700, walked 900 (over 500), mood 7, retired
   names = bytes([6]) + b"ABCDEF"                            # six codes; the slot holds four now
   parts = [sub("flags.permanent", history["flags.permanent"]["hash"], flags),
            sub("flags.daily", current["flags.daily"]["hash"], bytes([1])),
            sub("vars.permanent", history["vars.permanent"]["hash"], variables),
            sub("vars.map", save_shape.shape_hash(save_shape.VARS, [["door", 0, 2, 0]]), sl.u16(9)),
            sub("names", history["names"]["hash"], names)]
   return bytes([len(parts)]) + b"".join(parts)


## The older vars.permanent shape whose walked had the range 0..900 (today 0..500; save_history.json records it).
WIDE_WALKED = [["coins", 0, 4, 0], ["walked", 4, 2, 0, 0, 900], ["mood", 6, 1, 0, 0, 9]]
## The sequence number before the wrap.
LAST_SEQUENCE = 0xFFFFFFFF


def party_of(size: int) -> Callable[[JsonDict], None]:
   """!An edit that gives the party another size (the record shape stays).
   @param size  members the party holds
   @return the structure edit
   """
   def edit(structure: JsonDict) -> None:
      """!The party size (and the formats' party_max within it)."""
      structure["monster_layout"]["party_size"] = size
      for f in structure.get("formats", []):
         f["party_max"] = min(f.get("party_max", size), size)
   return edit


def range_script_payload() -> bytes:
   """!A Script section whose permanent variables are in today's offsets and widths but the older range of walked
   (WIDE_WALKED): walked 900 must clamp to 500 when it loads.
   @return the section's bytes
   """
   labels = save_shape.sub_bank_names()
   variables = sl.u32(3) + sl.u16(900) + bytes([4])           # coins 3, walked 900, mood 4
   h = save_shape.shape_hash(save_shape.VARS, WIDE_WALKED)
   return bytes([1, labels.index("vars.permanent")]) + sl.u32(h) + sl.u16(len(variables)) + variables


def images() -> dict[str, bytes]:
   """!Every image the C++ test loads (see its test_migrations()).
   @return {file name: bytes}
   """
   current = bc.compile_battle(RECRUIT / "battle.json", platform=PLATFORM)
   size = current["layout"]["bytes"]
   out = {}
   # the current shapes: every section, the Python bytes are the engine's
   rec = bytes(range(1, size + 1))
   out["current.bin"] = sl.assemble({
      "bag": sl.bag_payload(current, bag_of(current, {"goods": [("Salve", 2), ("Ether", 1)], "key": [("Badge", 1)]})),
      "party": sl.party_payload(current, [rec]),
      "boxes": sl.boxes_payload(current, {4: rec}, 1),
      "script": sl.script_payload(script_banks(), {"flags.permanent": bytes([0b011]), "vars.permanent": sl.u32(5)})})
   # items reordered, Beacon added since, a Relic removed since
   old = variant(old_items, edit_logic=without_beacon)
   out["items.bin"] = sl.assemble({"bag": sl.bag_payload(old, bag_of(old, {
      "goods": [("Ether", 3), ("Salve", 2), ("Relic", 1)], "key": [("Badge", 1)]}))})
   # the orbs pocket added since: Lure and Snare were goods
   def no_orbs(structure: JsonDict) -> None:
      """!The pockets before orbs."""
      structure["pockets"] = [p for p in structure["pockets"] if p["name"] != "orbs"]

   def orbs_in_goods(battle: JsonDict) -> None:
      """!Lure and Snare in goods."""
      for i in battle["items"]:
         if i.get("pocket") == "orbs":
            i["pocket"] = "goods"
   old = variant(orbs_in_goods, no_orbs)
   out["pockets.bin"] = sl.assemble({"bag": sl.bag_payload(old, bag_of(old, {
      "goods": [("Salve", 1), ("Lure", 2), ("Snare", 1)]}))})
   # goods held up to 20 before: 12 Salves clamp to today's 5 (review4 finding 2)
   def more_goods(structure: JsonDict) -> None:
      """!goods quantity_max 20."""
      for p in structure["pockets"]:
         if p["name"] == "goods":
            p["quantity_max"] = 20
   old = variant(edit_structure=more_goods)
   out["quantity.bin"] = sl.assemble({"bag": sl.bag_payload(old, bag_of(old, {"goods": [("Salve", 12)]}))})
   # three boxes of two before (today two of three): slot 0 stays, slot 3 (box 1, place 1) stays in its box and
   # place, slot 5 (box 2) moves to the first free slot; the current box 2 is box 1 now
   def boxes_3x2(structure: JsonDict) -> None:
      """!Three boxes of two."""
      structure["storage"] = {"box_count": 3, "box_size": 2, "names": "Old {n}"}
   old = variant(edit_structure=boxes_3x2)
   out["boxes.bin"] = sl.assemble({"boxes": sl.boxes_payload(old, {0: bytes([1] * size), 3: bytes([3] * size),
                                                                  5: bytes([5] * size)}, 2)})
   # four boxes of three holding seven records: today's six slots cannot take them (refused, nothing changes)
   def boxes_4x3(structure: JsonDict) -> None:
      """!Four boxes of three."""
      structure["storage"] = {"box_count": 4, "box_size": 3, "names": "Old {n}"}
   old = variant(edit_structure=boxes_4x3)
   out["boxes_full.bin"] = sl.assemble({
      "bag": sl.bag_payload(old, bag_of(old, {"goods": [("Salve", 4)]})),
      "boxes": sl.boxes_payload(old, {k: bytes([k] * size) for k in range(7)}, 0)})
   # the script banks of older shapes (recorded remaps, a transient bank without one)
   out["script.bin"] = sl.assemble({"script": old_script_payload()})
   # a permanent bank of a shape nobody recorded: refused
   labels = save_shape.sub_bank_names()
   unknown = bytes([1, labels.index("flags.permanent")]) + sl.u32(save_shape.shape_hash(save_shape.FLAGS, ["x"]))
   out["script_refused.bin"] = sl.assemble({"script": unknown + sl.u16(1) + bytes([1])})
   # a party of another record layout: refused
   party = bytearray(sl.party_payload(current, [rec]))
   party[0] ^= 1
   out["party_layout.bin"] = sl.assemble({"party": bytes(party)})
   # C7: the party size is not the record's shape: a party of two grows into today's three, one of five holding
   # three members shrinks into it (with its boxes), one of five holding four is refused
   grow = variant(edit_structure=party_of(2))
   out["party_grow.bin"] = sl.assemble({"party": sl.party_payload(grow, [bytes([0x21] * size), bytes([0x22] * size)])})
   five = variant(edit_structure=party_of(5))
   out["party_shrink.bin"] = sl.assemble({
      "party": sl.party_payload(five, [bytes([0x51 + k] * size) for k in range(3)]),
      "boxes": sl.boxes_payload(five, {2: bytes([0x5B] * size)}, 0)})
   out["party_over.bin"] = sl.assemble({"party": sl.party_payload(five, [bytes([0x51] * size)] * 4)})
   # C7: a narrowed variable range is another shape (walked 900 of 0..900 clamps to 500)
   out["range.bin"] = sl.assemble({"script": range_script_payload()})
   # C7: a newer copy of another size than current.bin (more members, records and items), for the other slot
   out["newer.bin"] = sl.assemble({
      "bag": sl.bag_payload(current, bag_of(current, {"goods": [("Salve", 4), ("Ether", 2)], "key": [("Badge", 1)]})),
      "party": sl.party_payload(current, [bytes([0x91] * size), bytes([0x92] * size)]),
      "boxes": sl.boxes_payload(current, {0: rec, 1: rec, 5: rec}, 0),
      "script": sl.script_payload(script_banks())}, 2)
   # C7: the sequence wrap: 0 follows 0xFFFFFFFF
   out["wrap_old.bin"] = sl.assemble({"party": sl.party_payload(current, [bytes([0x61] * size)])}, LAST_SEQUENCE)
   out["wrap_new.bin"] = sl.assemble({"party": sl.party_payload(current, [bytes([0x62] * size)])}, 0)
   return out


class ImageTests(unittest.TestCase):
   """!The images the C++ test loads."""

   def test_images(self) -> None:
      """!The writer makes the recorded bytes (--record rewrites them)."""
      made = images()
      for name, data in made.items():
         path = SAVE / name
         if RECORD:
            path.write_bytes(data)
         with self.subTest(name=name):
            self.assertEqual(path.read_bytes(), data)

   def test_current_sizes(self) -> None:
      """!The current image is within the build's most bytes (the engine's image_size())."""
      current = bc.compile_battle(RECRUIT / "battle.json", platform=PLATFORM)
      sections = sl.sections_of(current, script_banks())
      self.assertLessEqual(len(images()["current.bin"]), sl.image_bytes(sections))
      self.assertEqual(sections["script"], 1 + 5 * 7 + 1 + 1 + 7 + 1 + 5)


class ShapeTests(unittest.TestCase):
   """!tools/save_shape.py on the save fixture."""

   def test_shapes(self) -> None:
      """!Sub-banks in capsule::script order, empty ones hash 0, variables by offset with width and sign."""
      banks = {s["bank"]: s for s in script_banks()}
      self.assertEqual(list(banks), save_shape.sub_bank_names())
      self.assertEqual(banks["flags.permanent"]["entries"], ["lit", "met", "fresh"])
      self.assertEqual(banks["vars.permanent"]["entries"],
                       [["coins", 0, 4, 0], ["walked", 4, 2, 0, 0, 500], ["mood", 6, 1, 0, 0, 9]])
      self.assertEqual(banks["names"]["entries"], [["hero", 0, 4]])
      self.assertEqual(banks["flags.area"]["hash"], 0)

   def test_remaps(self) -> None:
      """!The recorded shapes move by name; a removed entry is dropped."""
      comp = sc.compile_file(SAVE / "scripts.json")
      rm = {m["bank"]: m for m in save_shape.remaps(comp, comp.save_history)}
      self.assertEqual(set(rm), {"flags.permanent", "vars.permanent", "names"})
      self.assertEqual(rm["flags.permanent"]["moves"], [(0, comp.flag_ids["met"], 1, 0),
                                                        (1, save_shape.DROPPED, 1, 0),
                                                        (2, comp.flag_ids["lit"], 1, 0)])
      self.assertEqual(rm["names"]["moves"], [(0, 0, 6, 4)])
      self.assertIn("kSaveRemaps", comp.emit_cpp("scripts.json"))

   def test_record(self) -> None:
      """!record() adds the shapes the history lacks once; a bad history is refused."""
      comp = sc.compile_file(SAVE / "scripts.json")
      with tempfile.TemporaryDirectory() as tmp:
         path = Path(tmp) / "h.json"
         self.assertEqual(save_shape.record(comp, path), 5)   # every non-empty sub-bank
         self.assertEqual(save_shape.record(comp, path), 0)
         self.assertEqual(save_shape.remaps(comp, save_shape.load_history(path)), [])
         data = json.loads(path.read_text(encoding="utf-8"))
         bad = copy.deepcopy(data)
         bad["shapes"][0]["bank"] = "flags.weekly"
         path.write_text(json.dumps(bad), encoding="utf-8")
         with self.assertRaises(save_shape.ShapeError):
            save_shape.load_history(path)


class FinalReviewTests(unittest.TestCase):
   """!C7: the final review's repros on the Python side."""

   def test_narrowed_range_is_a_shape(self) -> None:
      """!The probe's repro: mood 0..9 narrowed to 0..5 changes the shape hash (it did not, so the old value was
      copied unclamped); a variable of its width's whole range keeps its C6 entry.
      @return None
      """
      self.assertEqual(save_shape.var_entry("door", 0, 1, 0, 255), ["door", 0, 1, 0])
      self.assertEqual(save_shape.var_entry("lead", 0, 2, -5, 5), ["lead", 0, 2, 1, -5, 5])
      self.assertEqual({w: save_shape.default_range(w) for w, _lo, _hi in sc.TYPES.values()},
                       {w: (lo, hi) for w, lo, hi in sc.TYPES.values()})
      wide = [save_shape.var_entry("mood", 6, 1, 0, 9)]
      narrow = [save_shape.var_entry("mood", 6, 1, 0, 5)]
      self.assertNotEqual(save_shape.shape_hash(save_shape.VARS, wide), save_shape.shape_hash(save_shape.VARS, narrow))
      comp = sc.compile_file(SAVE / "scripts.json")
      wide_hash = save_shape.shape_hash(save_shape.VARS, WIDE_WALKED)
      rm = [m for m in save_shape.remaps(comp, comp.save_history) if m["from_hash"] == wide_hash]
      self.assertEqual(len(rm), 1)
      self.assertEqual(rm[0]["moves"][1], (4, comp.result["var_names"].index("walked"), 2, 0))

   def test_history_entries(self) -> None:
      """!A history's variable entries have four or six fields.
      @return None
      """
      with tempfile.TemporaryDirectory() as tmp:
         path = Path(tmp) / "h.json"
         path.write_text(json.dumps({"format": save_shape.HISTORY_FORMAT, "shapes": [
            {"bank": "vars.permanent", "entries": [["coins", 0, 4]]}]}), encoding="utf-8")
         with self.assertRaises(save_shape.ShapeError):
            save_shape.load_history(path)

   def test_unrecorded(self) -> None:
      """!The build check: the fixture records every shape it builds; a history without them fails save_layout and
      save_shape check; a game without a history is not checked.
      @return None
      """
      comp = sc.compile_file(SAVE / "scripts.json")
      self.assertEqual(save_shape.unrecorded(comp, comp.save_history), [])
      self.assertEqual(save_shape.main(["save_shape.py", "check", str(SAVE / "scripts.json")]), 0)
      self.assertEqual(save_shape.unrecorded(comp, []), [])
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         for name in ("scripts.json", "save.cscript"):
            (folder / name).write_bytes((SAVE / name).read_bytes())
         history = json.loads((SAVE / "save_history.json").read_text(encoding="utf-8"))
         history["shapes"] = [s for s in history["shapes"] if s["entries"] != WIDE_WALKED][:4]
         (folder / "save_history.json").write_text(json.dumps(history), encoding="utf-8")
         old = sc.compile_file(folder / "scripts.json")
         self.assertIn("vars.permanent", save_shape.unrecorded(old, old.save_history))
         with mock.patch("sys.stderr"):
            self.assertEqual(save_shape.main(["save_shape.py", "check", str(folder / "scripts.json")]),
                             save_shape.EXIT_FAILED)
            result = sl.layout(RECRUIT / "battle.json", folder / "scripts.json", PLATFORM)
            self.assertFalse(result["ok"])
            self.assertIn("vars.permanent", result["unrecorded"])
            out = folder / "layout.txt"
            args = ["save_layout.py", str(RECRUIT / "battle.json"), "--scripts", str(folder / "scripts.json"),
                    "--platform", PLATFORM, "--out", str(out)]
            with mock.patch("sys.stdout"):
               self.assertEqual(sl.main(args), sl.EXIT_FAILED)
               self.assertEqual(save_shape.record(old, folder / "save_history.json") > 0, True)
               self.assertEqual(sl.main(args), 0)

   def test_slots(self) -> None:
      """!Copies lie in slots of the medium alone: half the medium in erase units; an image over a slot keeps one.
      @return None
      """
      self.assertEqual(sl.slot_bytes(0x10000, 0x1000), 0x8000)
      self.assertEqual(sl.slot_bytes(0x8000, 1), 0x4000)
      self.assertEqual(sl.slot_bytes(0x11000, 0x1000), 0x8000)
      self.assertEqual(sl.copies_of(0x8000, 0x10000, 0x1000), 2)
      self.assertEqual(sl.copies_of(0x8001, 0x10000, 0x1000), 1)
      self.assertEqual(sl.copies_of(0x10001, 0x10000, 0x1000), 0)

   def test_party_record_hash(self) -> None:
      """!The party and boxes sections hold the record hash, which the party size leaves alone.
      @return None
      """
      current = bc.compile_battle(RECRUIT / "battle.json", platform=PLATFORM)
      five = variant(edit_structure=party_of(5))
      self.assertNotEqual(current["layout"]["hash"], five["layout"]["hash"])
      self.assertEqual(current["layout"]["record_hash"], five["layout"]["record_hash"])
      self.assertEqual(sl.party_payload(current, [])[:sl.U32_BYTES], sl.u32(current["layout"]["record_hash"]))
      self.assertIn(f"0x{current['layout']['record_hash']:08X}u}};", bc.emit("battle.json", current))

   def test_save_key_zero(self) -> None:
      """!An item whose save key is 0 (the engine's no item) is refused at build time.
      @return None
      """
      with mock.patch.object(battle_inventory, "asset_key", return_value=battle_inventory.NO_SAVE_KEY):
         with self.assertRaises(bc.ManifestError) as ctx:
            bc.compile_battle(RECRUIT / "battle.json", platform=PLATFORM)
      self.assertIn("save key 0", str(ctx.exception))


if __name__ == "__main__":
   unittest.main()
