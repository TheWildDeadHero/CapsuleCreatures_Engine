#!/usr/bin/env python3
"""!Tests for the E13a expression and record features: the ruleset language's bit operators (tools/ruleset_compiler.py
parse, evaluate, interval analysis, C++), the generation list's wide draw order and species fields
(tools/battlec/progression.py, battlec/content.py species_fields), unstored custom roles (battlec/layout.py), the
Python mirrors of battle::record_value() / rederive() and the re-deriving transfer import
(tools/record_transfer.py import_rederived()), on the neutral fixture tests/data/battle_progress/records.json (run
by CTest, or directly)."""
from __future__ import annotations

import random
import sys
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler as bc  # noqa: E402
import record_transfer as rt  # noqa: E402
import ruleset_compiler as rc  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## The E13a records fixture.
FIXTURE = ROOT / "tests/data/battle_progress/records.json"
## The progression fixture (no E13a feature: its layout hash must not move).
PROGRESS = ROOT / "tests/data/battle_progress/battle.json"
## int32 limits.
I32_MIN, I32_MAX = -(1 << 31), (1 << 31) - 1
## Random samples per interval soundness check.
SAMPLES = 400
## A sending game's wider glyph role (C5: the receiver's 8 bits narrow it).
WIDE_GLYPH_BITS = 16
## A glyph only the wider role holds.
WIDE_GLYPH = 300


def compile_with(path: Path, edit: Callable[[JsonDict], None]) -> JsonDict:
   """!Compiles a battle after an edit of its merged view, from memory.
   @param path  battle.json
   @param edit  callable(merged view)
   @return compiled
   """
   data = bc.view_of(path)
   edit(data)
   return bc.compile_battle(path, None, bc.sources_of(path, data))


def entry(data: JsonDict, role: str) -> JsonDict:
   """!A generation entry of a merged view.
   @param data  the view
   @param role  its role
   @return the entry
   """
   return next(e for e in data["generation"] if e["role"] == role)


class BitOperatorTests(unittest.TestCase):
   """!The ruleset language's & | ^ ~ << >>."""

   def ev(self, text: str, **values: int) -> int:
      """!Evaluates an expression.
      @param text    expression
      @param values  inputs
      @return value
      """
      return rc.evaluate(rc.parse(text, list(values), {}, "t"), values, {})

   def test_values(self) -> None:
      """!C++ int32 semantics, signed values included."""
      self.assertEqual(self.ev("a & b", a=0b1100, b=0b1010), 0b1000)
      self.assertEqual(self.ev("a | b", a=0b1100, b=0b1010), 0b1110)
      self.assertEqual(self.ev("a ^ b", a=0b1100, b=0b1010), 0b0110)
      self.assertEqual(self.ev("~a", a=5), -6)
      self.assertEqual(self.ev("a & b", a=-1, b=-8), -8)
      self.assertEqual(self.ev("a ^ b", a=-2, b=3), -3)
      self.assertEqual(self.ev("a << 3", a=-3), -24)
      self.assertEqual(self.ev("a >> 1", a=-9), -5)   # arithmetic: toward minus infinity (C++20)
      self.assertEqual(self.ev("a >> 31", a=I32_MIN), -1)
      with self.assertRaises(rc.RulesetError):
         self.ev("a << b", a=1, b=32)
      with self.assertRaises(rc.RulesetError):
         self.ev("a >> b", a=1, b=-1)

   def test_precedence(self) -> None:
      """!Bit operators bind tighter than comparisons (Rust order): | < ^ < & < shifts < + < *."""
      self.assertEqual(self.ev("a & 1 == 0", a=2), 1)          # (a & 1) == 0
      self.assertEqual(self.ev("1 | 2 ^ 3 & 6", a=0), 1 | (2 ^ (3 & 6)))
      self.assertEqual(self.ev("1 << 2 + 1", a=0), 1 << 3)     # shifts below sums
      self.assertEqual(self.ev("a ^ b < 8", a=5, b=6), 1)      # (a ^ b) < 8
      self.assertEqual(self.ev("~a & 1", a=4), 1)              # unary first
      self.assertEqual(self.ev("8 >> 1 >> 1", a=0), 2)         # left-associative

   def test_interval_sound(self) -> None:
      """!The interval of every operator holds every value it can take, for signed and non-negative ranges."""
      rnd = random.Random(13)
      ranges = [(0, 0), (0, 1), (0, 255), (3, 9), (-1, 0), (-8, 7), (-300, -2), (-65536, 65535), (5, 65535),
                (-(1 << 20), 1 << 20)]
      for op in ("&", "|", "^"):
         for a in ranges:
            for b in ranges:
               tree = rc.parse(f"x {op} y", ["x", "y"], {}, "t")
               lo, hi = rc.interval(tree, {"x": a, "y": b}, {}, "t", [])
               for _ in range(SAMPLES // 10):
                  x, y = rnd.randint(*a), rnd.randint(*b)
                  for xv, yv in ((x, y), (a[0], b[0]), (a[1], b[1]), (a[0], b[1]), (a[1], b[0])):
                     v = rc.evaluate(tree, {"x": xv, "y": yv}, {})
                     self.assertTrue(lo <= v <= hi, f"{xv} {op} {yv} = {v} outside [{lo}, {hi}]")
      for op in ("<<", ">>"):
         for a in ranges:
            for b in [(0, 0), (0, 3), (2, 7), (0, 15)]:
               tree = rc.parse(f"x {op} y", ["x", "y"], {}, "t")
               try:
                  lo, hi = rc.interval(tree, {"x": a, "y": b}, {}, "t", [])
               except rc.RulesetError:
                  self.assertEqual(op, "<<")   # only a left shift can leave int32
                  continue
               for _ in range(SAMPLES // 10):
                  x, s = rnd.randint(*a), rnd.randint(*b)
                  v = rc.evaluate(tree, {"x": x, "y": s}, {})
                  self.assertTrue(lo <= v <= hi and I32_MIN <= v <= I32_MAX)
      tree = rc.parse("~x", ["x"], {}, "t")
      self.assertEqual(rc.interval(tree, {"x": (-3, 10)}, {}, "t", []), (-11, 2))

   def test_refusals(self) -> None:
      """!Shift amounts outside [0, 31] and left shifts out of int32 are build errors."""
      for text, rng in (("x << y", (0, 32)), ("x >> y", (-1, 3)), ("x << y", (0, 31))):
         tree = rc.parse(text, ["x", "y"], {}, "t")
         with self.assertRaises(rc.RulesetError):
            rc.interval(tree, {"x": (0, 255), "y": rng}, {}, "t", [])
      with self.assertRaises(rc.RulesetError):
         rc.parse("x @ y", ["x", "y"], {}, "t")
      with self.assertRaises(rc.RulesetError):
         rc.load({"schema": rc.SCHEMA, "inputs": {"a.b": [0, 1]}})

   def test_cpp(self) -> None:
      """!C++ parenthesizes every operation (the tree's order, not C's precedence)."""
      tree = rc.parse("a & 1 == 0 | ~b >> 2", ["a", "b"], {}, "t")
      self.assertEqual(rc.to_cpp(tree, {}),
                       "int32_t((a & int32_t{1}) == (int32_t{0} | ((~b) >> int32_t{2})))")

   def test_formulas(self) -> None:
      """!A ruleset formula may use them (they are the whole language's)."""
      f = rc.compile_formula("low", {"expr": "v & MASK | v >> 6", "range": [0, 15]}, ["v"], {"MASK": 15},
                             {"v": (0, 255)}, "t")
      self.assertEqual(f["bounds"], (0, 15))
      self.assertFalse(f["clamp_active"])
      self.assertEqual(rc.evaluate(f["tree"], {"v": 0b11000101}, f["constants"]), 0b0111)


class RecordFeatureTests(unittest.TestCase):
   """!Generation order, species fields, unstored roles, rederive and the transfer import on the records fixture."""

   @classmethod
   def setUpClass(cls) -> None:
      """!Compiles the fixture once.
      @return None
      """
      cls.c = bc.compile_battle(FIXTURE)

   def test_generation_kinds(self) -> None:
      """!The seed is a low-first wide draw; GEN_KINDS appends wide_low_first (GenKind::WideLowFirst)."""
      gen = {e["role"]: e for e in self.c["rules"]["progress"]["generation"]}
      self.assertEqual(bc.GEN_KINDS[gen["seed"]["kind"]], "wide_low_first")
      self.assertEqual(bc.GEN_KINDS.index("wide_low_first"), 5)
      self.assertEqual(bc.DRAWS_PER_KIND["wide_low_first"], 2)
      out = bc.emit("records.json", self.c)
      self.assertIn("{0u, 4294967295u, 1, 5, 1, 0},   // seed: wide_low_first", out)
      high = compile_with(FIXTURE, lambda d: entry(d, "seed").pop("order"))
      self.assertEqual(bc.GEN_KINDS[high["rules"]["progress"]["generation"][0]["kind"]], "wide")

   def test_order_refusals(self) -> None:
      """!order belongs to wide random ranges only and names a known order."""
      def bad(edit: Callable[[JsonDict], None], text: str) -> None:
         with self.assertRaisesRegex(bc.ManifestError, text):
            compile_with(FIXTURE, edit)
      bad(lambda d: entry(d, "seed").update(order="sideways"), "order: one of high_first, low_first")
      bad(lambda d: entry(d, "owner_id").update(order="low_first"), "only a wide random_range")
      bad(lambda d: entry(d, "stat_genes").update(order="low_first"), "only a wide random_range")

   def test_unstored_layout(self) -> None:
      """!An unstored role has an ID after the stored custom roles, no bits, and leaves the hash and bytes alone."""
      lay = self.c["layout"]
      self.assertEqual([c["name"] for c in lay["custom"]], ["secret", "lean", "glyph"])
      self.assertEqual(lay["unstored"][0]["name"], "marked")
      self.assertEqual(lay["ids"]["marked"], bc.CUSTOM_ROLE_BASE + 3)
      self.assertIsNone(bc.role_info(lay, "marked"))
      self.assertNotIn("marked", lay["roles"])

      def drop(d: JsonDict) -> None:
         d["monster_layout"]["roles"] = [r for r in d["monster_layout"]["roles"]
                                         if not (isinstance(r, dict) and r.get("custom") == "marked")]
         d["generation"] = [e for e in d["generation"] if e["role"] != "marked"]
      without = compile_with(FIXTURE, drop)
      self.assertEqual(without["layout"]["hash"], lay["hash"])
      self.assertEqual(without["layout"]["bytes"], lay["bytes"])
      out = bc.emit("records.json", self.c)
      self.assertIn("{0xC5F847A1u, 0u, 0xFFFF, 1, 1},   // marked (role 0x83, unstored)", out)
      self.assertIn(f"0x{lay['hash']:08X}u, {lay['bytes']}, 3, 2, 3, 3, 1, {{0}}, 0x{lay['record_hash']:08X}u}};", out)

   def test_unstored_refusals(self) -> None:
      """!An unstored role needs exactly one derived or constant entry; stored must be a bool."""
      def bad(edit: Callable[[JsonDict], None], text: str) -> None:
         with self.assertRaisesRegex(bc.ManifestError, text):
            compile_with(FIXTURE, edit)
      bad(lambda d: d.update(generation=[e for e in d["generation"] if e["role"] != "marked"]),
          "unstored role 'marked' is computed on read")
      bad(lambda d: entry(d, "marked").update(random_range=[0, 1]) or entry(d, "marked").pop("derived"),
          "unstored role 'marked'")
      bad(lambda d: next(r for r in d["monster_layout"]["roles"]
                         if isinstance(r, dict) and r.get("custom") == "marked").update(stored="no"),
          "stored: true")

   def test_species_fields(self) -> None:
      """!species.NAME reads the species' field; refusals for unknown fields and out-of-range values."""
      species = {sp["name"]: sp for sp in self.c["species"]}
      self.assertEqual(species["Blob"]["fields"], {"balance": 200})
      self.assertEqual(self.c["species_fields"], [{"name": "balance", "width": 8, "max": 255, "values": [],
                                                   "default": 0}])
      out = bc.emit("records.json", self.c)
      self.assertIn("constexpr uint8_t kSpeciesFields[][kSpeciesFieldCount] = {", out)
      self.assertIn("species_field(rec, 0)", out)

      def bad(edit: Callable[[JsonDict], None], text: str) -> None:
         with self.assertRaisesRegex(bc.ManifestError, text):
            compile_with(FIXTURE, edit)
      bad(lambda d: d["species"][0]["fields"].update(weight=3), "not a species field")
      bad(lambda d: d["species"][0]["fields"].update(balance=256), "balance")
      bad(lambda d: entry(d, "lean").update(derived="species.weight > 0"), "unknown name 'species.weight'")
      bad(lambda d: d["species_fields"].append({"name": "balance", "width": 4}), "declared twice")
      bad(lambda d: d["species_fields"].append({"name": "huge", "width": 32}), "width")

      # without a reader, no table is emitted (no unused function)
      def no_reader(d: JsonDict) -> None:
         entry(d, "lean").update(derived="seed_lo & 1")
      self.assertNotIn("kSpeciesFields", bc.emit("records.json", compile_with(FIXTURE, no_reader)))

   def test_record_values(self) -> None:
      """!derived_values(): the fixture's rules on records of both species."""
      rnd = random.Random(7)
      by_name = {sp["name"]: sp for sp in self.c["species"]}
      for _ in range(SAMPLES):
         sp = by_name[rnd.choice(["Blob", "Cube"])]
         seed, owner, secret = rnd.getrandbits(32), rnd.getrandbits(16), rnd.getrandbits(16)
         rec = {"species_key": sp["key"], "seed": seed, "owner_id": owner, "secret": secret}
         got = bc.derived_values(self.c, rec)
         hi, lo = seed >> 16, seed & 0xFFFF
         self.assertEqual(got["lean"], int((lo & 255) < sp["fields"]["balance"]))
         self.assertEqual(got["marked"], int((owner ^ secret ^ hi ^ lo) < 8))
         glyph = ((hi >> 8 & 3) << 6 | (hi & 3) << 4 | (lo >> 8 & 3) << 2 | lo & 3) % 28
         self.assertEqual(got["glyph"], glyph)
         self.assertEqual(got["trait"], ~lo & 1)
      # a species the data does not have reads the field's default (0: never lean)
      self.assertEqual(bc.derived_values(self.c, {"species_key": 1, "seed": 0})["lean"], 0)

   def test_rederive_and_import(self) -> None:
      """!rederive() fixes stale stored derived roles; import_rederived() keeps what the sender gave and derives
      what it lacks."""
      sp = self.c["species"][0]
      rec = {"species_key": sp["key"], "level": 5, "seed": 0x12345678, "owner_id": 9, "secret": 4, "glyph": 200,
             "lean": 0}
      fixed = bc.rederive(self.c, rec)
      self.assertEqual(fixed["glyph"], bc.derived_values(self.c, rec)["glyph"])
      self.assertEqual(fixed["lean"], 1)            # 0x78 < 200
      self.assertNotIn("marked", fixed)             # unstored: nothing to write
      self.assertEqual(bc.rederive(self.c, rec, ["glyph"])["glyph"], 200)

      table = rt.load(FIXTURE)
      data = rt.export_record(table, rec)
      received: set[str] = set()
      got, _flags = rt.import_record(table, data, received)
      self.assertIn("glyph", received)
      self.assertNotIn("marked", received)
      kept, _flags = rt.import_rederived(table, data, self.c)
      self.assertEqual(kept["glyph"], 200)          # the sender had it

      # a sender without glyph and lean (another game's layout): both re-derived
      def strip(d: JsonDict) -> None:
         d["monster_layout"]["roles"] = [r for r in d["monster_layout"]["roles"]
                                         if not (isinstance(r, dict) and r.get("custom") in ("glyph", "lean"))]
         d["generation"] = [e for e in d["generation"] if e["role"] not in ("glyph", "lean")]
      other = compile_with(FIXTURE, strip)
      sender = rt.compile_transfer(other["layout"], other["stats"], "sender")
      data = rt.export_record(sender, {k: v for k, v in rec.items() if k not in ("glyph", "lean")})
      moved, flags = rt.import_rederived(table, data, self.c)
      self.assertTrue(flags & rt.DEFAULTED)
      self.assertEqual(moved["glyph"], fixed["glyph"])
      self.assertEqual(moved["lean"], 1)

      # C5: a sender whose glyph is wider holds a value the receiver narrows: not received whole, re-derived
      def widen(d: JsonDict) -> None:
         for r in d["monster_layout"]["roles"]:
            if isinstance(r, dict) and r.get("custom") == "glyph":
               r["width"] = WIDE_GLYPH_BITS
      wide = compile_with(FIXTURE, widen)
      sender = rt.compile_transfer(wide["layout"], wide["stats"], "sender")
      data = rt.export_record(sender, {**rec, "glyph": WIDE_GLYPH})
      narrowed: set[str] = set()
      _got, flags = rt.import_record(table, data, narrowed)
      self.assertTrue(flags & rt.NARROWED)
      self.assertNotIn("glyph", narrowed)
      self.assertIn("secret", narrowed)
      moved, _flags = rt.import_rederived(table, data, self.c)
      self.assertEqual(moved["glyph"], fixed["glyph"])

      # the raw form brings every held role
      raw_received: set[str] = set()
      rt.import_record(table, rt.export_record(table, rec, raw=True), raw_received)
      self.assertEqual(raw_received, set(table["layout"]["roles"]))

   def test_unstored_ids(self) -> None:
      """!C5: the ids header names an unstored role UNSTORED_ROLE_<NAME> (record_value() only), never ROLE_<NAME>
      (monster::get() and field scripts' role_get read 0 for it); effect logic may read it, not write it."""
      ids = bc.emit_ids("records.json", self.c)
      marked = self.c["layout"]["unstored_ids"]["marked"]
      self.assertIn(f"inline constexpr uint8_t UNSTORED_ROLE_MARKED = 0x{marked:02X};", ids)
      self.assertNotIn("uint8_t ROLE_MARKED", ids)
      self.assertIn("uint8_t ROLE_GLYPH", ids)
      cat = self.c["effect_catalog"]
      self.assertEqual(cat.unstored_roles, {"marked"})
      self.assertIn("glyph", cat.roles)

   def test_progress_hash_kept(self) -> None:
      """!A layout without unstored roles keeps its hash (E13a changes no existing layout)."""
      plain = bc.compile_battle(PROGRESS)
      self.assertEqual(plain["layout"]["unstored"], [])
      self.assertNotIn("kSpeciesFields", bc.emit("battle.json", plain))


if __name__ == "__main__":
   unittest.main()
