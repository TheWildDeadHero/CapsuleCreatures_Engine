#!/usr/bin/env python3
"""Tests for tools/script_compiler.py: the language, the checks (B-1, B-2,
P-18) and the bank it emits."""
from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import script_compiler as sc  # noqa: E402


def compile_text(text: str, **manifest: Any) -> sc.Compiler:
   """Compiles one in-memory source.
   @param text      the source
   @param manifest  extra manifest keys
   @return the Compiler
   """
   m = {"schema": "capsule.scripts/1", "sources": ["main.cscript"]}
   m.update(manifest)
   return sc.Compiler(m, sources={"main.cscript": text}).run()


def compile_with_vehicles(text: str, vehicles: list[str]) -> sc.Compiler:
   """Compiles one in-memory source as if its manifest's world had these vehicle classes.
   @param text      the source
   @param vehicles  vehicle class names, in class order
   @return the Compiler
   """
   m = {"schema": "capsule.scripts/1", "sources": ["main.cscript"]}
   comp = sc.Compiler(m, sources={"main.cscript": text})
   comp.vehicle_names = vehicles
   return comp.run()


def errors(comp: sc.Compiler) -> list[str]:
   """Error messages.
   @param comp  a Compiler
   @return list of strings
   """
   return [d.message for d in comp.errors()]


def warnings(comp: sc.Compiler) -> list[str]:
   """Warning messages.
   @param comp  a Compiler
   @return list of strings
   """
   return [d.message for d in comp.diags if d.severity == "warning"]


class Fixture(unittest.TestCase):
   """The engine's test fixture compiles."""

   def test_fixture(self) -> None:
      """tests/data/scripts compiles with only the expected result."""
      comp = sc.compile_file(ROOT / "tests" / "data" / "scripts" / "scripts.json")
      self.assertEqual(errors(comp), [])
      r = comp.result
      # F2: + the world's two flight point flags; C10: the session bank (empty) last; C1: two more permanent
      # flags (the encounter test's beaten flags)
      self.assertEqual(r["flag_first"], [0, 5, 6, 7, 8, 8])
      self.assertIn("field.on_frame.1", r["names"])
      # the fixture's name slots: {hero} (a length byte and 5 codes) and C4's str:1-str:5 (8 codes each)
      self.assertEqual(r["name_bytes"], 6 + 5 * (8 + 1))
      self.assertEqual(comp.save_bytes(), sum(r["flag_bytes"]) + sum(r["var_bytes"]) + r["name_bytes"])
      self.assertIn("SCRIPT_FLOW", comp.emit_ids("x"))
      self.assertIn("const Bank kBank", comp.emit_cpp("x"))


class Language(unittest.TestCase):
   """Statements compile to the documented bytecode."""

   def code(self, body: str, decls: str = "var v : u8; flag f;") -> bytes:
      """Bytecode of one script.
      @param body   statements
      @param decls  declarations
      @return bytes
      """
      comp = compile_text(f"{decls}\nscript s {{ {body} }}")
      self.assertEqual(errors(comp), [])
      o, n, _ = comp.result["scripts"][0]
      return comp.result["code"][o:o + n]

   def test_operands(self) -> None:
      """Constants pick the smallest encoding; var(...) is its own operand type."""
      op = sc.OP
      self.assertEqual(self.code("var(v) = 5;"), bytes([op["SetVar"], 0, 0, sc.OPND_CONST8, 5, op["End"]]))
      c = self.code("var(v) = var(v);", "var v : u8;")
      self.assertEqual(c[3], sc.OPND_VAR)
      c = self.code("native n(-1, 70000);", "native n(a, b);")
      self.assertEqual(c[4], sc.OPND_CONST16)
      self.assertEqual(c[7], sc.OPND_CONST32)

   def test_branch_offsets(self) -> None:
      """if/else jumps land inside the script (16-bit offsets from its start)."""
      c = self.code("if flag(f) { var(v) = 1; } else { var(v) = 2; }")
      self.assertEqual(c[0], sc.OP["IfFlagClear"])
      target = struct.unpack_from("<H", c, 3)[0]
      self.assertEqual(c[target], sc.OP["SetVar"])
      self.assertEqual(c[target + 4], 2)

   def test_constant_comparison(self) -> None:
      """Comparisons of two constants are decided by the compiler."""
      c = self.code("if 1 == 2 { var(v) = 1; }")
      self.assertEqual(c[0], sc.OP["Goto"])

   def test_text(self) -> None:
      """Inline texts are pooled once; text:"..." keys use Asset Key v1."""
      comp = compile_text('script s { msg "Hi"; msg "Hi"; msg text:"Two words"; }')
      self.assertEqual(errors(comp), [])
      self.assertEqual(comp.result["text"], [ord("H"), ord("i"), 0xFF])
      o, n, w = comp.result["scripts"][0]
      code = comp.result["code"][o:o + n]
      # The third msg's asset key: a little-endian u32 at byte 14.
      key = struct.unpack_from("<I", code, 14)[0]
      self.assertEqual(key, sc.asset_key("text:TWO WORDS"))
      self.assertEqual(w, 1)   # waits

   def test_dedupe(self) -> None:
      """Identical scripts are stored once."""
      comp = compile_text("var v : u8; script a { var(v) = 1; } script b { var(v) = 1; }")
      a, b = comp.result["scripts"][:2]
      self.assertEqual(a[0], b[0])

   def test_when_game(self) -> None:
      """when game(...) picks a build variant; unknown games are errors."""
      src = "var v : u8; script s { when game(b) { var(v) = 1; } }"
      self.assertEqual(errors(compile_text(src, games=["a", "b"], game="a")), [])
      self.assertTrue(any("unknown game" in e for e in errors(compile_text(src, games=["a"], game="a"))))
      comp = compile_text("when game(b) { var v : u8; } script s { var(v) = 1; }", games=["a", "b"], game="a")
      self.assertTrue(any("unknown variable" in e for e in errors(comp)))

   def test_layout(self) -> None:
      """Flags are numbered bank by bank; variables are aligned by width."""
      comp = compile_text("flag a : map; flag b; flag c : daily; var x : u8; var y : u32; var z : u16;"
                          "script s { set flag(a); set flag(b); set flag(c); var(x) = 1; var(y) = 1; var(z) = 1; }")
      self.assertEqual(comp.flag_ids, {"b": 0, "c": 1, "a": 2})
      offsets = {name: v[2] for name, v in zip(comp.result["var_names"], comp.result["vars"])}
      self.assertEqual(offsets, {"y": 0, "z": 4, "x": 6})
      self.assertEqual(comp.result["var_bytes"][0], 7)

   def test_objects(self) -> None:
      """Map object commands (B-21): self / player / operands, directions, default steps."""
      op = sc.OP
      self.assertEqual(self.code("move self left;"),
                       bytes([op["ObjectMove"], sc.OPND_CONST8, sc.OBJECT_SELF, 2, sc.OPND_CONST8, 1, op["End"]]))
      self.assertEqual(self.code("move player up var(v);")[3:5], bytes([1, sc.OPND_VAR]))
      self.assertEqual(self.code("face 3 player;"), bytes([op["ObjectFace"], sc.OPND_CONST8, 3, sc.FACE_PLAYER,
                                                           op["End"]]))
      self.assertEqual(self.code("face player self;")[3], sc.FACE_SELF)
      self.assertEqual(self.code("hide self; show 2;"),
                       bytes([op["ObjectHide"], sc.OPND_CONST8, sc.OBJECT_SELF, op["ObjectShow"], sc.OPND_CONST8, 2,
                              op["End"]]))

   def test_eventing(self) -> None:
      """RPG Maker XP-style commands (D-17) encode as documented."""
      op = sc.OP
      c = self.code("route self repeat skip { up 2; wait 30; face toward; through on; }")
      self.assertEqual(c[0:3], bytes([op["Route"], sc.OPND_CONST8, sc.OBJECT_SELF]))
      self.assertEqual(c[3], sc.ROUTE_NOWAIT)   # repeating routes never wait
      n = c[4] | (c[5] << 8)   # the route's length, a little-endian u16
      self.assertEqual(c[6:6 + n], bytes([3, 1, 1, 2, 8, 30, 0, 14, 19, 8, 1, 0]))
      self.assertEqual(self.code("route player { left; } wait_moves;")[3], 0)
      self.assertIn(op["WaitMoves"], self.code("wait_moves;"))
      self.assertEqual(self.code("set self(B);"), bytes([op["SelfSwitch"], 1, 1, op["End"]]))
      c = self.code("if self(C) { var(v) = 1; }")
      self.assertEqual(c[0:2], bytes([op["IfSelfClear"], 2]))
      self.assertEqual(self.code("var(v) *= 3;")[0:4], bytes([op["VarOp"], 0, 0, 0]))
      c = self.code("var(v) = pos_x(player);")
      self.assertEqual(c[3:6], bytes([sc.OPND_QUERY, 0, sc.OBJECT_PLAYER]))
      c = self.code("var(v) = timer;")
      self.assertEqual(c[3:6], bytes([sc.OPND_QUERY, list(sc.QUERIES).index("timer"), 0]))
      self.assertEqual(self.code("input_number 3 -> var(v);", "var v : u16;")[0:4],
                       bytes([op["InputNumber"], 0, 0, 3]))
      self.assertEqual(self.code("wait_key -> var(v);")[0], op["WaitKey"])
      self.assertEqual(self.code("msg_pos top;"), bytes([op["MessagePos"], 2, op["End"]]))
      c = self.code("warp somewhere 3 4 left mosaic;")
      self.assertEqual(c[0], op["Warp"])
      self.assertEqual(c[-3:-1], bytes([2, sc.TRANSITIONS.index("mosaic")]))
      self.assertEqual(self.code("warp somewhere 1 1;")[-2], sc.TRANSITION_DEFAULT)
      self.assertEqual(self.code("place 2 5 6 up;")[-2], 1)
      self.assertEqual(self.code("scroll right 4 speed 3;"), bytes([op["Scroll"], 3, sc.OPND_CONST8, 4, 3,
                                                                     op["End"]]))
      self.assertEqual(self.code("scroll back;")[1], 4)
      self.assertEqual(self.code("tint 31 0 0 8 30;"), bytes([op["Tint"], 31, 0, 0, 8, 30, 0, op["End"]]))
      self.assertEqual(self.code("tint off;")[4], 0)
      self.assertEqual(self.code("flash 31 31 31 10;"), bytes([op["Flash"], 31, 31, 31, 10, 0, op["End"]]))
      self.assertEqual(self.code("shake 2 20;"), bytes([op["Shake"], 2, 20, 0, op["End"]]))
      self.assertEqual(self.code("timer start 90; timer stop;")[0], op["TimerStart"])
      self.assertEqual(self.code("encounters off;"), bytes([op["Encounters"], 0, op["End"]]))

   def test_vehicle(self) -> None:
      """`vehicle N;` takes a class number; `vehicle NAME;` a class of the manifest's world (A2)."""
      op = sc.OP
      self.assertEqual(self.code("vehicle 1;"), bytes([op["Vehicle"], 1, op["End"]]))
      comp = compile_with_vehicles("script s { vehicle swim; vehicle 0; }", ["foot", "swim"])
      self.assertEqual(errors(comp), [])
      o, n, _ = comp.result["scripts"][0]
      self.assertEqual(comp.result["code"][o:o + n], bytes([op["Vehicle"], 1, op["Vehicle"], 0, op["End"]]))
      # The map test world's scripts name its classes.
      comp = sc.compile_file(ROOT / "tests" / "data" / "maps" / "scripts" / "scripts.json")
      self.assertEqual(comp.vehicle_names, ["foot", "swim", "paddle", "trek"])
      self.assertEqual(errors(comp), [])

   def test_encounter(self) -> None:
      """D-36 package 2 C1: encounter statements and battles compile to Encounter / Battle (capsule/script/ops.h)."""
      op, k = sc.OP, sc.ENCOUNTER_KINDS
      comp = compile_text('flag f; script s { encounter set b 300 self beaten flag(f) intro "Hi" then t rematch; }'
                          " script t { }")
      self.assertEqual(errors(comp), [])
      o, n, _ = comp.result["scripts"][0]
      c = comp.result["code"][o:o + n]
      parts = sc.ENCOUNTER_TEXTS["intro"] | sc.ENCOUNTER_HAS_THEN
      text_bytes = 1 + 4   # a text reference: its tag and 32-bit value
      self.assertEqual(c[:10], bytes([op["Encounter"], k.index("set"), sc.ENCOUNTER_SIDES["b"], sc.OPND_CONST16,
                                      300 & 0xFF, 300 >> 8, sc.OPND_CONST8, sc.OBJECT_SELF, 0, 0]))
      self.assertEqual(c[10], parts)
      self.assertEqual(c[11], sc.TEXT_INLINE)
      at = 11 + text_bytes
      self.assertEqual(struct.unpack_from("<H", c, at)[0], 1)   # script t's ID (s is 0)
      self.assertEqual(c[at + 2:], bytes([sc.ENCOUNTER_BITS["rematch"], 0, op["End"]]))   # bits, mode 0
      self.assertEqual(self.code("encounter get beaten -> var(v); encounter text defeat; encounter continue;"),
                       bytes([op["Encounter"], k.index("get"), sc.ENCOUNTER_FIELDS.index("beaten"), 0, 0,
                              op["Encounter"], k.index("text"), sc.ENCOUNTER_TEXTS["defeat"], 0,
                              op["Encounter"], k.index("continue")]))   # continue leaves: no End after it
      # msg modifiers: the flags byte (MessageEx's bits)
      self.assertEqual(self.code("encounter text intro nowait instant;"),
                       bytes([op["Encounter"], k.index("text"), sc.ENCOUNTER_TEXTS["intro"],
                              sc.MSG_FLAGS["nowait"] | sc.MSG_FLAGS["instant"], op["End"]]))
      self.assertEqual(self.code("face player encounter; encounter select b;"),
                       bytes([op["ObjectFace"], sc.OPND_CONST8, sc.OBJECT_PLAYER, sc.FACE_WORDS["encounter"],
                              op["Encounter"], k.index("select"), sc.OPND_CONST8, sc.ENCOUNTER_SIDES["b"],
                              op["End"]]))
      # C1b: the spotter group's statements; side C (its code after kEncounterSideCurrent)
      self.assertEqual(self.code("encounter load; encounter approach; encounter next -> var(v); encounter next; "
                                 "encounter select c;"),
                       bytes([op["Encounter"], k.index("load"), op["Encounter"], k.index("approach"),
                              op["Encounter"], k.index("next"), 0, 0, op["Encounter"], k.index("next"), 0xFF, 0xFF,
                              op["Encounter"], k.index("select"), sc.OPND_CONST8, sc.ENCOUNTER_SIDES["c"],
                              op["End"]]))
      self.assertEqual(sc.ENCOUNTER_SIDES["c"], sc.VM.constants["kEncounterSideC"])
      b = sc.BATTLE_KINDS
      self.assertEqual(self.code("battle unowned 25 5 -> var(v); battle party;"),
                       bytes([op["Battle"], b.index("unowned"), sc.OPND_CONST8, 25, sc.OPND_CONST8, 5, sc.OPND_CONST8,
                              sc.BATTLE_NO_ITEM, 0, 0, op["Battle"], b.index("party"), 0xFF, 0xFF, op["End"]]))


class Props(unittest.TestCase):
   """C2: props and object draw order."""

   def code(self, body: str) -> bytes:
      """Bytecode of one script.
      @param body  statements
      @return bytes
      """
      comp = compile_text(f"var v : u8;\nscript s {{ {body} }}")
      self.assertEqual(errors(comp), [])
      o, n, _ = comp.result["scripts"][0]
      return comp.result["code"][o:o + n]

   def test_bytes(self) -> None:
      """Place (its sprite's key, x and y operands, level, facing), face, remove; order fixed and reset."""
      op = sc.OP
      k = sc.PROP_KINDS
      key = sc.asset_key("sprite:AUDIENCE", "test").to_bytes(4, "little")
      self.assertEqual(self.code("prop place 3 sprite:AUDIENCE 12 var(v) 3 left;")[:8],
                       bytes([op["Prop"], k.index("place"), 3]) + key + bytes([sc.OPND_CONST8]))
      code = self.code("prop place 3 sprite:AUDIENCE 12 var(v) 3 left;")
      self.assertEqual(code[-3:], bytes([3, sc.DIRECTIONS.index("left"), op["End"]]))
      self.assertEqual(self.code("prop face 3 down; prop remove 3;"),
                       bytes([op["Prop"], k.index("face"), 3, sc.DIRECTIONS.index("down"),
                              op["Prop"], k.index("remove"), 3, op["End"]]))
      self.assertEqual(self.code("order player 83; order 2 reset;"),
                       bytes([op["ObjectOrder"], sc.OPND_CONST8, sc.VM.constants["kObjectPlayer"], 1, 83,
                              op["ObjectOrder"], sc.OPND_CONST8, 2, 0, 0, op["End"]]))

   def test_refusals(self) -> None:
      """A number or level past a byte, a reference that is no sprite, an order past a byte."""
      for body, text in (("prop face 256 down;", "0-255"), ("prop place 1 text:a.b 1 2 3 down;", "sprite:NAME"),
                         ("prop place 1 sprite:A 1 2 300 down;", "level"), ("order 1 256;", "order must be"),
                         ("prop spin 1;", "place")):
         errs = errors(compile_text(f"script s {{ {body} }}"))
         self.assertTrue(any(text in e for e in errs), (body, errs))


class Menus(unittest.TestCase):
   """C3: positioned menus of a list asset."""

   def code(self, body: str) -> bytes:
      """Bytecode of one script.
      @param body  statements
      @return bytes
      """
      comp = compile_text(f"var v : u8;\nscript s {{ {body} }}")
      self.assertEqual(errors(comp), [])
      o, n, _ = comp.result["scripts"][0]
      return comp.result["code"][o:o + n]

   def test_bytes(self) -> None:
      """Every field in order; the defaults (the port's place and delay, a column, no cancel)."""
      op = sc.OP
      c = sc.VM.constants
      lst = sc.asset_key("list:MULTI_X", "test").to_bytes(4, "little")
      style = sc.asset_key("text_box:LIST", "test").to_bytes(4, "little")
      flags = c["kMenuBack"] | c["kMenuAt"] | c["kMenuFadeWait"]
      self.assertEqual(self.code("menu list:MULTI_X at 17 6 default 1 columns 2 rows 3 delay 12 cancel 127 fadewait "
                                 "style text_box:LIST -> var(v);"),
                       bytes([op["Menu"], 0, 0, flags, 17, 6, 1, 2, 3, 12, 127]) + lst + style + bytes([op["End"]]))
      self.assertEqual(self.code("menu list:MULTI_X -> var(v);"),
                       bytes([op["Menu"], 0, 0, 0, 0, 0, 0, 1, 0, c["kMenuStyleDelay"], c["kNoCancel"]]) + lst +
                       bytes(4) + bytes([op["End"]]))
      # noback: the cancel value stays (a menu that cannot open), Back does nothing
      self.assertEqual(self.code("menu list:MULTI_X cancel 127 noback -> var(v);")[3], 0)

   def test_refusals(self) -> None:
      """No list, an unknown field, a value past a byte, a delay of the port's own value, no columns, no style."""
      for body, text in (("menu text:a.b -> var(v);", "list:NAME"), ("menu list:A spin 3 -> var(v);", "unknown field"),
                         ("menu list:A cancel 256 -> var(v);", "0-255"), ("menu list:A delay 255 -> var(v);", "0-254"),
                         ("menu list:A columns 0 -> var(v);", "columns"),
                         ("menu list:A style 3 -> var(v);", "text_box"),
                         ("menu list:A at 1 -> var(v);", "number")):
         errs = errors(compile_text(f"var v : u8;\nscript s {{ {body} }}"))
         self.assertTrue(any(text in e for e in errs), (body, errs))


class Names(unittest.TestCase):
   """C4: name slots from texts, list entries, numbers and kinds."""

   ## The manifest's placeholders (a name slot first, a variable second) and its own name kind.
   MANIFEST = {"placeholders": {"str:1": {"name": 8}, "count": {"var": "v"}}, "name_kinds": ["trophy"]}

   def code(self, body: str) -> bytes:
      """Bytecode of one script.
      @param body  statements
      @return bytes
      """
      comp = compile_text(f"var v : u8;\nscript s {{ {body} }}", **self.MANIFEST)
      self.assertEqual(errors(comp), [])
      o, n, _ = comp.result["scripts"][0]
      return comp.result["code"][o:o + n]

   def test_bytes(self) -> None:
      """Each source: its byte, then its fields; a kind's count only when given; a game kind after the engine's."""
      op, src = sc.OP["Name"], sc.NAME_SOURCES
      lst = sc.asset_key("list:STD", "test").to_bytes(4, "little")
      text = sc.asset_key("text:A", "test").to_bytes(4, "little")
      self.assertEqual(self.code("name str:1 = text:A;"),
                       bytes([op, 0, src.index("text"), sc.TEXT_ASSET]) + text + bytes([sc.OP["End"]]))
      self.assertEqual(self.code("name str:1 = list:STD 3;"),
                       bytes([op, 0, src.index("list")]) + lst + bytes([sc.OPND_CONST8, 3, sc.OP["End"]]))
      self.assertEqual(self.code("name str:1 = number 9;"),
                       bytes([op, 0, src.index("number"), sc.OPND_CONST8, 9, sc.OP["End"]]))
      self.assertEqual(self.code("name str:1 = move 2;")[3], sc.NAME_KINDS.index("move"))   # a keyword as a kind
      self.assertEqual(self.code("name str:1 = item 4;"),
                       bytes([op, 0, src.index("kind"), sc.NAME_KINDS.index("item"), sc.OPND_CONST8, 4, 0,
                              sc.OP["End"]]))
      self.assertEqual(self.code("name str:1 = trophy 4 count 2;"),
                       bytes([op, 0, src.index("kind"), len(sc.NAME_KINDS), sc.OPND_CONST8, 4, 1, sc.OPND_CONST8, 2,
                              sc.OP["End"]]))

   def test_refusals(self) -> None:
      """A placeholder that is no name slot (or none), an unknown kind, a malformed name_kinds."""
      for body, text in (("name count = number 1;", "no name placeholder"),
                         ("name str:9 = number 1;", "no name placeholder"),
                         ("name str:1 = medal 1;", "unknown name kind")):
         errs = errors(compile_text(f"var v : u8;\nscript s {{ {body} }}", **self.MANIFEST))
         self.assertTrue(any(text in e for e in errs), (body, errs))
      for kinds in (["item"], ["Trophy"], ["a", "a"], "trophy"):
         with self.assertRaises(sc.ScriptError):
            sc.game_name_kinds(kinds)


class Counters(unittest.TestCase):
   """C5: counter boxes."""

   ## A manifest with two counters (one with a label).
   MANIFEST = {"counters": {"gold": {"var": "v", "label": "Gold"}, "coins": {"var": "w"}}}

   def code(self, body: str) -> bytes:
      """Bytecode of one script.
      @param body  statements
      @return bytes
      """
      comp = compile_text(f"var v : u16;\nvar w : u8;\nscript s {{ {body} }}", **self.MANIFEST)
      self.assertEqual(errors(comp), [])
      o, n, _ = comp.result["scripts"][0]
      return comp.result["code"][o:o + n]

   def test_bytes(self) -> None:
      """Show: the kind, the box, its variable, the cells, the label inline; update and hide: no cells or label."""
      op, k = sc.OP["Counter"], sc.COUNTER_KINDS
      show = self.code("counter show coins at 20 1;")
      self.assertEqual(show[:7], bytes([op, k.index("show"), 1, 1, 0, 20, 1]))
      self.assertEqual(show[7], sc.TEXT_INLINE)
      self.assertEqual(self.code("counter update gold; counter hide gold;"),
                       bytes([op, k.index("update"), 0, 0, 0, op, k.index("hide"), 0, 0, 0, sc.OP["End"]]))

   def test_refusals(self) -> None:
      """An undeclared counter, a show without cells, a malformed counters key."""
      for body, text in (("counter show silver at 0 0;", "no counter"), ("counter show gold;", "expected 'at'"),
                         ("counter spin gold;", "show")):
         errs = errors(compile_text(f"var v : u16;\nvar w : u8;\nscript s {{ {body} }}", **self.MANIFEST))
         self.assertTrue(any(text in e for e in errs), (body, errs))
      for bad in ({"Gold": {"var": "v"}}, {"gold": {"label": "x"}}, {"gold": {"var": "v", "font": "x"}}, ["gold"]):
         with self.assertRaises(sc.ScriptError):
            sc.game_counters(bad)


## C6: the bytes of a variable id after an op (u16).
VAR_ID_BYTES = 2


class Stats(unittest.TestCase):
   """C6: game statistics."""

   ## A manifest with a stat bound to a variable and one the game's port holds.
   MANIFEST = {"stats": {"steps": {"var": "v"}, "visits": {}}, "stats_port": True}

   def code(self, body: str, **manifest: Any) -> bytes:
      """Bytecode of one script.
      @param body      statements
      @param manifest  manifest keys (default MANIFEST)
      @return bytes
      """
      comp = compile_text(f"var v : u16;\nscript s {{ {body} }}", **(manifest or self.MANIFEST))
      self.assertEqual(errors(comp), [])
      o, n, _ = comp.result["scripts"][0]
      return comp.result["code"][o:o + n]

   def test_bytes(self) -> None:
      """A bound stat is the variable's add (1 by default); a port's stat is Stat with its index and the amount."""
      self.assertEqual(self.code("stat add steps;"), self.code("var(v) += 1;"))
      self.assertEqual(self.code("stat add steps var(v);"), self.code("var(v) += var(v);"))
      add_one = self.code("var(v) += 1;")
      amount = add_one[1 + VAR_ID_BYTES:-1]   # the operand after AddVar's variable, before End
      self.assertEqual(self.code("stat add visits;"), bytes([sc.OP["Stat"], 1]) + amount + bytes([sc.OP["End"]]))

   def test_refusals(self) -> None:
      """An undeclared stat, a missing add, a port's stat without stats_port, malformed stats."""
      for body, text in (("stat add jumps;", "no statistic"), ("stat steps;", "expected 'add'")):
         errs = errors(compile_text(f"var v : u16;\nscript s {{ {body} }}", **self.MANIFEST))
         self.assertTrue(any(text in e for e in errs), (body, errs))
      with self.assertRaises(sc.ScriptError):
         sc.game_stats({"visits": {}}, False)
      for bad in ({"Steps": {"var": "v"}}, {"steps": {"var": "v", "max": 9}}, ["steps"]):
         with self.assertRaises(sc.ScriptError):
            sc.game_stats(bad, True)
      with self.assertRaises(sc.ScriptError):
         sc.game_stats({}, "yes")


class PartyStatements(unittest.TestCase):
   """C7: the party statements and the item queries."""

   def code(self, body: str) -> bytes:
      """Bytecode of one script.
      @param body  statements
      @return bytes
      """
      comp = compile_text(f"var v : u16;\nvar w : u8;\nscript s {{ {body} }}")
      self.assertEqual(errors(comp), [])
      o, n, _ = comp.result["scripts"][0]
      return comp.result["code"][o:o + n]

   def test_party_bytes(self) -> None:
      """Heal / stash / restore: the kind alone; count: hatched and the variable; find: the slot and species
      variables (kNoVar without species) and the move."""
      op, k, end = sc.OP["Party"], sc.PARTY_KINDS, sc.OP["End"]
      self.assertEqual(self.code("party heal; party stash; party restore;"),
                       bytes([op, k.index("heal"), op, k.index("stash"), op, k.index("restore"), end]))
      count = self.code("party count hatched -> var(v);")
      self.assertEqual(count[:3], bytes([op, k.index("count"), 1]))
      self.assertEqual(self.code("party count -> var(v);")[2], 0)
      move = self.code("var(v) += 33;")[1 + VAR_ID_BYTES:-1]   # the operand 33
      find = self.code("party find move 33 -> var(v) species var(w);")
      self.assertEqual(find[:2], bytes([op, k.index("find")]))
      self.assertEqual(find[2 + 2 * VAR_ID_BYTES:-1], move)
      bare = self.code("party find move 33 -> var(v);")
      no_var = sc.NO_VAR.to_bytes(VAR_ID_BYTES, "little")
      self.assertEqual(bare[2 + VAR_ID_BYTES:2 + 2 * VAR_ID_BYTES], no_var)

   def test_give_bytes(self) -> None:
      """C8: give: the result variable, the species, the level and the item (0 without one); give egg: the result
      and the species."""
      op, k = sc.OP["Party"], sc.PARTY_KINDS
      give = self.code("party give 4 5 item 7 -> var(v);")
      self.assertEqual(give[:2], bytes([op, k.index("give")]))
      self.assertEqual(self.code("party give 4 5 -> var(v);"), self.code("party give 4 5 item 0 -> var(v);"))
      self.assertEqual(self.code("party give egg 4 -> var(v);")[:2], bytes([op, k.index(sc.PARTY_GIVE_EGG)]))
      for body in ("party give 4 -> var(v);", "party give egg 4;"):   # no level; no result
         errs = errors(compile_text(f"var v : u16;\nscript s {{ {body} }}"))
         self.assertTrue(errs, (body, errs))

   def test_item_queries(self) -> None:
      """ItemQuery: its kind, the variable, the item and, for room, the count (1 by default)."""
      op, k = sc.OP["ItemQuery"], sc.ITEM_QUERY_KINDS
      self.assertEqual(self.code("item_room(3, 5) -> var(v);")[:2], bytes([op, k.index("room")]))
      self.assertEqual(self.code("item_room(3) -> var(v);"), self.code("item_room(3, 1) -> var(v);"))
      self.code("item_room(3, 0) -> var(v);")   # room for none more: allowed (an imported game asks it)
      self.assertEqual(self.code("item_pocket(3) -> var(v);")[:2], bytes([op, k.index("pocket")]))

   def test_refusals(self) -> None:
      """A count or find without its variable, a find without `move`, a query without its result."""
      for body, text in (("party count;", "expected '->'"), ("party find 33 -> var(v);", "expected 'move'"),
                         ("item_pocket(3);", "needs -> var(v)")):
         errs = errors(compile_text(f"var v : u16;\nscript s {{ {body} }}"))
         self.assertTrue(any(text in e for e in errs), (body, errs))


class SceneStatements(unittest.TestCase):
   """C9: the field scene statements."""

   def code(self, body: str) -> bytes:
      """Bytecode of one script.
      @param body  statements
      @return bytes
      """
      comp = compile_text(f"var v : u16;\nscript s {{ {body} }}")
      self.assertEqual(errors(comp), [])
      o, n, _ = comp.result["scripts"][0]
      return comp.result["code"][o:o + n]

   def operand(self, n: int) -> bytes:
      """The bytes of a constant operand.
      @param n  the value
      @return them
      """
      return self.code(f"var(v) += {n};")[1 + VAR_ID_BYTES:-1]

   def test_bytes(self) -> None:
      """Each statement: its op, its kind and its operands in order; `darkness <n> fade` is DarknessFade."""
      end = bytes([sc.OP["End"]])
      self.assertEqual(self.code("map redraw;"), bytes([sc.OP["MapView"], sc.MAP_VIEW_KINDS.index("redraw")]) + end)
      self.assertEqual(self.code("map layout 2;"),
                       bytes([sc.OP["MapView"], sc.MAP_VIEW_KINDS.index("layout")]) + self.operand(2) + end)
      self.assertEqual(self.code("animation arg 1 9;"), bytes([sc.OP["Animation"], sc.ANIMATION_KINDS.index("arg")])
                       + self.operand(1) + self.operand(9) + end)
      self.assertEqual(self.code("animation wait 5;"),
                       bytes([sc.OP["Animation"], sc.ANIMATION_KINDS.index("wait")]) + self.operand(5) + end)
      self.assertEqual(self.code("camera detach;"),
                       bytes([sc.OP["Camera"], sc.CAMERA_KINDS.index("detach")]) + end)
      shake = bytes([sc.OP["Camera"], sc.CAMERA_KINDS.index("shake")])
      operands = b"".join(self.operand(n) for n in (2, 3, 4, 5))
      self.assertEqual(self.code("camera shake 2 3 4 5;"), shake + operands + end)
      self.assertEqual(self.code("darkness 1 fade;")[0], sc.OP["DarknessFade"])
      self.assertEqual(self.code("darkness 1;")[0], sc.OP["Darkness"])

   def test_refusals(self) -> None:
      """A shake short of its four operands, a layout without its map, an animation without its id."""
      for body in ("camera shake 2 3;", "map layout;", "animation start;"):
         self.assertTrue(errors(compile_text(f"var v : u16;\nscript s {{ {body} }}")), body)


class Checks(unittest.TestCase):
   """Build errors and warnings."""

   def assert_error(self, src: str, text: str, **manifest: Any) -> None:
      """Asserts an error containing @p text.
      @param src       source
      @param text      expected fragment
      @param manifest  extra manifest keys
      """
      errs = errors(compile_text(src, **manifest))
      self.assertTrue(any(text in e for e in errs), errs)

   def test_recursion(self) -> None:
      """Direct and indirect recursion name the cycle."""
      self.assert_error("script a { call a; }", "recursion: a -> a")
      self.assert_error("script a { call b; } script b { call c; } script c { call a; }",
                       "recursion: a -> b -> c -> a")
      self.assert_error("var v : u8; script a { call var(v) of [b]; } script b { call a; }", "recursion")

   def test_encounter_checks(self) -> None:
      """An encounter's after / then script must exist, and a wrong word names what may come."""
      self.assert_error("script s { encounter set a 1 self after nowhere; }", "unknown script 'nowhere'")
      self.assert_error("script s { encounter set a 1 self loud; }", "expected beaten")
      self.assert_error("var v : u8; script s { encounter get party; }", "needs -> var(v)")
      self.assert_error("script a { encounter approach; } map_scripts m { on_load -> a; }", "on_load script a waits")

   def test_depth(self) -> None:
      """A chain deeper than stack_depth is an error."""
      src = "script a { call b; } script b { call c; } script c { }"
      self.assertEqual(errors(compile_text(src, stack_depth=3)), [])
      self.assert_error(src, "calls 3 deep", stack_depth=2)

   def test_names(self) -> None:
      """Unknown and duplicate names, labels and arities."""
      self.assert_error("script a { call nope; }", "unknown script")
      self.assert_error("script a { set flag(x); }", "unknown flag")
      self.assert_error("flag x; var x : u8;", "already declared")
      self.assert_error("script a { goto nowhere; }", "unknown label")
      self.assert_error("script a { l: l: end; }", "appears twice")
      self.assert_error("native n(a); script s { native n(); }", "takes 1 argument")
      self.assert_error("native n(a); var v : u8; script s { native n(1) -> var(v); }", "returns no value")
      self.assert_error("script s { native m(); }", "unknown native")

   def test_ranges(self) -> None:
      """Constants must fit a variable's declared range (P-18)."""
      self.assert_error("var v : u8 0..9; script s { var(v) = 10; }", "outside v's range")
      self.assert_error("var v : u8 0..300;", "does not fit u8")
      self.assert_error("var v : u8 0..1; script s { choice [\"a\", \"b\", \"c\"] -> var(v); }", "outside")

   def test_object_checks(self) -> None:
      """Bad objects, directions and step counts; moves wait (not in on_load)."""
      self.assert_error("script s { move 64 up; }", "is not a local ID")
      self.assert_error("script s { move self north; }", "expected down, up, left, right")
      self.assert_error("script s { move self up 70000; }", "0-65535 steps")
      self.assert_error("script s { face self self; }", "cannot face itself")
      self.assert_error("script s { hide player; }", "cannot be hidden")
      self.assert_error("flag move;", "expected")
      self.assert_error("script a { move self up; } map_scripts m { on_load -> a; }", "on_load script a waits")
      self.assertEqual(errors(compile_text("script a { face 1 up; hide 2; } map_scripts m { on_load -> a; }")), [])

   def test_vehicle_checks(self) -> None:
      """Unknown class names, names without a world, numbers out of range."""
      self.assert_error("script s { vehicle swim; }", "class names need the manifest's world")
      self.assert_error("script s { vehicle 8; }", "class 8 does not exist (0-7)")
      self.assert_error("script s { vehicle; }", "expected a vehicle class name or number")
      comp = compile_with_vehicles("script s { vehicle bike; vehicle 2; }", ["foot", "swim"])
      errs = errors(comp)
      self.assertTrue(any("'bike' is not a vehicle class of the world (foot, swim)" in e for e in errs), errs)
      self.assertTrue(any("class 2 does not exist (0-1)" in e for e in errs), errs)

   def test_world_uses(self) -> None:
      """Flags the world's map objects and events use are not 'never used' (and the maps are deps)."""
      comp = sc.compile_file(Path(__file__).resolve().parent / "data" / "maps" / "scripts" / "scripts.json")
      self.assertEqual(errors(comp), [])
      self.assertFalse(any("never used" in w for w in warnings(comp)), warnings(comp))
      self.assertTrue(any(d.endswith("town.json") for d in comp.deps))

   def test_eventing_checks(self) -> None:
      """Bad routes, ranges and on_frame conditions that cannot be tracked."""
      self.assert_error("script s { route self { fly; } }", "unknown step 'fly'")
      self.assert_error("script s { route player repeat { up; } }", "repeating route")
      self.assert_error("script s { route self { set nope; } }", "unknown flag")
      self.assert_error("script s { input_number 12 -> var(v); } var v : u32;", "1-9 digits")
      self.assert_error("var v : u8; script s { input_number 3 -> var(v); }", "outside")
      self.assert_error("script s { tint 40 0 0 8; }", "colors are 0-31")
      self.assert_error("script s { shake 9 10; }", "power of 1-8")
      self.assert_error("script s { timer start 6000; }", "1-5999 seconds")
      self.assert_error("var v : u8; script s { var(v) /= 0; }", "divides by zero")
      self.assert_error("script s { set self(E); }", "self switches are A, B, C and D")
      self.assert_error("var v : u8; script s { } map_scripts m { on_frame pos_x(player) == 3 -> s; }",
                       "cannot read pos_x")
      self.assert_error("script s { } map_scripts m { on_frame self(A) -> s; }", "cannot read self switches")
      self.assert_error("script a { wait_moves; } map_scripts m { on_load -> a; }", "waits")

   def test_uses_self(self) -> None:
      """Scripts using self switches (directly or through calls) are reported for the object compiler."""
      comp = compile_text("script a { set self(A); } script b { call a; } script c { }")
      self.assertEqual(comp.result["uses_self"], {"a", "b"})

   def test_waits_in_load(self) -> None:
      """on_load / on_transition scripts must not wait, also through calls."""
      self.assert_error('script a { call b; } script b { msg "x"; } map_scripts m { on_load -> a; }',
                       "on_load script a waits")
      self.assert_error("native n() waits; script a { native n(); } map_scripts m { on_transition -> a; }",
                       "waits")

   def test_triggers(self) -> None:
      """Trigger limits and maps of the world."""
      many = "flag f; script s { }\nmap_scripts m {" + " on_frame flag(f) -> s;" * 17 + " }"
      self.assert_error(many, "17 on_frame triggers")
      deps = "".join(f"flag f{i};" for i in range(9)) + "script s { }\nmap_scripts m { on_frame " + \
          " && ".join(f"flag(f{i})" for i in range(9)) + " -> s; }"
      self.assert_error(deps, "at most 8")
      comp = sc.Compiler({"schema": "capsule.scripts/1", "sources": ["a"]},
                         sources={"a": "script s { } map_scripts nowhere { on_load -> s; }"},
                         map_names={"field"}).run()
      self.assertTrue(any("not in the world" in e for e in errors(comp)))

   def test_text_assets(self) -> None:
      """text:key must name a text asset when the assets manifest is given."""
      comp = sc.Compiler({"schema": "capsule.scripts/1", "sources": ["a"]},
                         sources={"a": "script s { msg text:missing; }"}, asset_keys={}).run()
      self.assertTrue(any("not a text asset" in e for e in errors(comp)))

   def test_code_page(self) -> None:
      """Inline text outside the code page is an error at its position."""
      comp = compile_text('script s {\n  msg "café";\n}')
      d = comp.errors()[0]
      self.assertIn("not in the code page", d.message)
      self.assertEqual((d.line, d.col), (2, 7))

   def test_syntax_position(self) -> None:
      """Syntax errors carry the file, line and column."""
      comp = compile_text("script s {\n  var(x) = ;\n}")
      d = comp.errors()[0]
      self.assertEqual((d.file, d.line), ("main.cscript", 2))

   def test_warnings(self) -> None:
      """Unreachable statements and unused declarations warn."""
      comp = compile_text("flag f; var v : u8; script s { end; var(v) = 1; }")
      w = warnings(comp)
      self.assertIn("unreachable statement", w)
      self.assertIn("flag f is never used", w)

   def test_review_fixes(self) -> None:
      """Spacing, leading zeros, both ends of stored ranges, map-name order, case clashes."""
      comp = compile_text("var x:u8 0..9; flag f:map; script s { top:var(x) += 1; if var(x) < 09 { goto top; } }")
      self.assertEqual(errors(comp), [])
      self.assertEqual(compile_text("const C = 010; var v : u8 0..9; script s { var(v) = C; }").errors()[0].message,
                       "10 is outside v's range 0..9")
      self.assert_error('var v : u8 1..5; script s { yesno -> var(v); }', "yesno stores 0..1")
      self.assert_error('var v : u8 1..2; script s { choice ["a", "b"] -> var(v); }', "stores 0..1")
      self.assert_error("var v : u8 1..5; script s { var(v) = random(5); }", "random(5) stores 0..4")
      self.assertEqual(errors(compile_text("script room { } map_scripts room { on_load -> room; }")), [])
      self.assertEqual(errors(compile_text("map_scripts room { on_load -> room; } script room { }")), [])
      self.assert_error("flag seen; flag Seen;", "only in case")
      w = warnings(compile_text("var v : u8; script s { if var(v) == 1 { return; } else { end; } var(v) = 2; }"))
      self.assertIn("unreachable statement", w)

   def test_not_utf8(self) -> None:
      """A source that is not UTF-8 is a diagnostic on that source."""
      import tempfile
      with tempfile.TemporaryDirectory() as d:
         (Path(d) / "a.cscript").write_bytes(b"// caf\xe9\n")
         comp = sc.Compiler({"schema": "capsule.scripts/1", "sources": ["a.cscript"]}, Path(d)).run()
         e = comp.errors()[0]
         self.assertEqual(e.file, "a.cscript")
         self.assertIn("not UTF-8", e.message)

   def test_manifest(self) -> None:
      """Manifest keys and settings are checked."""
      comp = sc.Compiler({"schema": "capsule.scripts/1", "sources": [], "bogus": 1}).run()
      self.assertTrue(any("unknown manifest key" in e for e in errors(comp)))
      comp = sc.Compiler({"schema": "capsule.scripts/1", "sources": [], "stack_depth": 40}).run()
      self.assertTrue(any("stack_depth" in e for e in errors(comp)))



## Placeholders of the field placeholder tests: one of each binding kind.
PLACEHOLDERS = {"hero": {"name": 5}, "count": {"var": "coins"}, "town": {"term": "town"},
                "towns": {"term": "town", "form": "many"}, "lead": {"record": "nickname", "slot": 2}}
## Their terms.
TERMS = {"town": {"one": "Elm", "many": "Elms"}}


class Placeholders(unittest.TestCase):
   """Field placeholders (D-22 decision 6): the manifest's bindings, markup texts and the bank's table."""

   def compile(self, text: str, **manifest: Any) -> sc.Compiler:
      """Compiles with the test placeholders.
      @param text      the source
      @param manifest  manifest keys replacing the test ones
      @return the Compiler
      """
      keys = {"placeholders": PLACEHOLDERS, "terms": TERMS}
      keys.update(manifest)
      return compile_text("var coins : u16;\n" + text, **keys)

   def test_table_and_texts(self) -> None:
      """Each binding gets its entry; texts encode FD 0x80 + index, {prompt}, {page} and terms."""
      comp = self.compile('script s { msgbox "Hi {hero}: {count} {towns}{prompt}{page}{lead} {@town:many}"; }')
      self.assertEqual(errors(comp), [])
      r = comp.result
      self.assertEqual(r["placeholders"], [(0, 5, 0, 0), (1, 0, 0, 0), (2, 0, 0, 0), (2, 0, 0, 4),
                                           (3, 2, sc.record_roles(None)[0]["nickname"][0], 12)])
      # The term texts come first in the pool ("Elm", "Elms"), then the message.
      self.assertEqual(r["text"][:9], [0x45, 0x6C, 0x6D, sc.CODE_END, 0x45, 0x6C, 0x6D, 0x73, sc.CODE_END])
      msg = r["text"][9:]
      self.assertEqual(msg[:4], [0x48, 0x69, 0x20, 0xFD])
      self.assertEqual(msg[4], 0x80)
      self.assertIn(0xFA, msg)
      self.assertIn(0xFB, msg)
      self.assertEqual(msg[-5:], [0x45, 0x6C, 0x6D, 0x73, sc.CODE_END])
      self.assertEqual(r["name_bytes"], 6)   # a length byte and five codes
      self.assertEqual(comp.save_bytes(), 2 + 6)
      cpp = comp.emit_cpp("x")
      self.assertIn("const Placeholder kPlaceholders[] = {", cpp)
      self.assertIn("{PlaceholderKind{0}, 5, 0, 0u}", cpp)
      self.assertIn("uint8_t g_names[6];", cpp)
      self.assertIn("kPlaceholders, g_names, 6,", cpp)
      ids = comp.emit_ids("x")
      self.assertIn("inline constexpr uint8_t PLACEHOLDER_HERO = 0;", ids)
      self.assertIn("inline constexpr uint8_t PLACEHOLDER_LEAD = 4;", ids)
      self.assertIn("Flags + variables + name slots.", ids)

   def test_without_placeholders_braces_are_characters(self) -> None:
      """No "placeholders" key: texts keep braces as characters (bytes as before) and the bank has no table."""
      comp = compile_text('script s { msgbox "{hero} {page}"; }')
      self.assertEqual(errors(comp), [])
      self.assertEqual(comp.result["text"], [ord(c) for c in "{hero} {page}"] + [sc.CODE_END])
      cpp = comp.emit_cpp("x")
      self.assertIn("    nullptr, nullptr, 0,\n    0,\n", cpp)
      self.assertNotIn("PLACEHOLDER_", comp.emit_ids("x"))
      # An empty "placeholders" turns markup on (for {prompt} / {page}) without a table.
      comp = compile_text('script s { msgbox "a{page}b"; }', placeholders={})
      self.assertEqual(comp.result["text"], [0x61, 0xFB, 0x62, sc.CODE_END])

   def test_checks(self) -> None:
      """Bindings are checked: kinds, ranges, variables, terms, roles, slots; texts name declared placeholders."""
      cases = [
         ({"p": {"name": 0}}, "1-24 characters"),
         ({"p": {"name": 25}}, "1-24 characters"),
         ({"p": {"var": "nope"}}, "unknown variable 'nope'"),
         ({"p": {"term": "nope"}}, "names a concept"),
         ({"p": {"term": "town", "form": "few"}}, "form is one or many"),
         ({"p": {"record": "nope"}}, "record names a record role"),
         ({"p": {"record": "level", "slot": 6}}, "party slot, 0-5"),
         ({"p": {"name": 3, "var": "coins"}}, "a binding with one of"),
         ({"p": {"name": 3, "slot": 1}}, "unknown keys ['slot']"),
         ({"Page": {"name": 3}}, "lower-case identifier"),
         ({"page": {"name": 3}}, "is a control"),
      ]
      for holders, message in cases:
         with self.subTest(message=message):
            comp = self.compile("", placeholders=holders)
            self.assertTrue(any(message in e for e in errors(comp)), errors(comp))
      comp = self.compile('script s { msg "{rival}"; }')
      self.assertTrue(any("unknown placeholder {rival}" in e for e in errors(comp)), errors(comp))
      comp = self.compile('script s { msg "{@nope}"; }')
      self.assertTrue(any("unknown term {@nope}" in e for e in errors(comp)), errors(comp))

   def test_bound_variable_is_used(self) -> None:
      """A variable bound to a placeholder is not reported as never used."""
      comp = self.compile("")
      self.assertFalse(any("coins" in w for w in warnings(comp)), warnings(comp))

   def test_load_placeholders(self) -> None:
      """Other compilers read a manifest's placeholders (and their widest text) without compiling it."""
      field = sc.FieldPlaceholders({"placeholders": PLACEHOLDERS, "terms": TERMS})
      self.assertEqual(field.codes["lead"], 0x84)
      self.assertEqual(field.widths, {"hero": 5, "count": sc.MAX_VAR_DIGITS, "town": 3, "towns": 4, "lead": 12})
      self.assertIsNone(sc.load_placeholders(None))
      loaded = sc.load_placeholders(ROOT / "tests" / "data" / "maps" / "scripts" / "scripts.json")
      self.assertEqual((loaded.codes, loaded.widths), ({"hero": 0x80}, {"hero": 6}))
      self.assertIsNone(sc.load_placeholders(ROOT / "apps" / "test_script" / "data" / "scripts.json"))



## The three-stat battle data: a layout v2 with custom roles (gender 2 bits, ribbons 32 x 1, met_level 7) and
## parties of three.
STATS3 = ROOT / "tests" / "data" / "battle_stats3" / "battle.json"


class Roles(unittest.TestCase):
   """role_get / role_set and record bindings against the game's layout (D-22 decision 2)."""

   def code(self, text: str, **manifest: Any) -> bytes:
      """The bytes of the only script.
      @param text      its body
      @param manifest  extra manifest keys
      @return bytecode
      """
      comp = compile_text("var v : u8;\nscript s { " + text + " }", **manifest)
      self.assertEqual(errors(comp), [])
      o, n, _ = comp.result["scripts"][0]
      return comp.result["code"][o:o + n]

   def errs(self, text: str, **manifest: Any) -> list[str]:
      """The errors of one script.
      @param text      its body
      @param manifest  extra manifest keys
      @return messages
      """
      return errors(compile_text("var v : u8;\nvar wide : u32;\nscript s { " + text + " }", **manifest))

   def test_bytecode(self) -> None:
      """RoleGet: var16, slot operand, role8, element operand; RoleSet adds the value operand."""
      op = sc.OP
      self.assertEqual((op["RoleGet"], op["RoleSet"]), (45, 46))
      battle = {"battle": str(STATS3)}
      self.assertEqual(self.code("role_get(1, ribbons, 31) -> var(v);", **battle),
                       bytes([op["RoleGet"], 0, 0, sc.OPND_CONST8, 1, 0x81, sc.OPND_CONST8, 31, op["End"]]))
      self.assertEqual(self.code("role_set(var(v), gender, 2);", **battle),
                       bytes([op["RoleSet"], sc.OPND_VAR, 0, 0, 0x80, sc.OPND_CONST8, 0, sc.OPND_CONST8, 2,
                              op["End"]]))
      # engine roles by name without the battle; custom roles by ID only
      self.assertEqual(self.code("role_get(5, level) -> var(v);")[5], 5)
      self.assertEqual(self.code("role_set(0, 130, 0, 1);")[3], 130)

   def test_ids_and_bindings(self) -> None:
      """The fixture's scripts name the stats3 layout: ROLE_ ids, record bindings of custom roles."""
      comp = sc.compile_file(ROOT / "tests" / "data" / "scripts" / "scripts.json")
      self.assertEqual(errors(comp), [])
      self.assertIn(str(STATS3), [str(Path(d).resolve()) for d in comp.deps])
      ids = comp.emit_ids("scripts.json")
      self.assertIn("inline constexpr uint8_t ROLE_RIBBONS = 0x81;", ids)
      self.assertIn("inline constexpr uint8_t ROLE_LEVEL = 0x05;", ids)
      self.assertNotIn("ROLE_STAT_AWAKEN", ids)
      self.assertEqual(comp.result["placeholders"][3:5], [(3, 2, 20, 8), (3, 1, 0x82, 0)])   # then C4's str slots
      # without a battle no ROLE_ ids
      self.assertNotIn("ROLE_", compile_text("script s { end; }").emit_ids("x"))

   def test_checks(self) -> None:
      """Unknown roles, slots past the party, elements past the role, values over its width, narrow variables."""
      battle = {"battle": str(STATS3)}
      cases = {"role_get(0, friendship) -> var(v);": "'friendship' is not a record role",
               "role_get(3, gender) -> var(v);": "party slot 3 does not exist (0-2)",
               "role_get(0, ribbons, 32) -> var(v);": "element 32 does not exist (0-31)",
               "role_set(0, gender, 4);": "4 does not fit the role (0-3)",
               "role_get(0, held_item) -> var(v);": "outside v's range",
               "role_get(0, 131) -> var(v);": "role 131 is not a record role of the layout",
               "role_get(0, 11) -> var(v);": "role 11 is not a record role of the layout"}
      for src, text in cases.items():
         with self.subTest(src):
            errs = self.errs(src, **battle)
            self.assertTrue(any(text in e for e in errs), errs)
      self.assertEqual(self.errs("role_get(0, seed) -> var(wide);", **battle), [])   # 32 bits clamp to u32's
      # without the battle: registry engine roles, retired ones refused, unknown custom IDs allowed
      self.assertTrue(any("'friendship' is not a record role" in e
                          for e in self.errs("role_get(0, friendship) -> var(v);")))
      self.assertEqual(self.errs("role_get(5, nickname, 11) -> var(v); role_set(0, 0x9F, 200, 7);"), [])
      self.assertTrue(any("element 12 does not exist" in e for e in self.errs("role_get(0, nickname, 12) -> var(v);")))
      self.assertTrue(any("party slot 6 does not exist (0-5)" in e
                          for e in self.errs("role_get(6, level) -> var(v);")))
      with self.assertRaisesRegex(sc.ScriptError, "role_set takes"):
         sc.Parser("script s { role_set(0, gender); }", "x").file_items()
      with self.assertRaisesRegex(sc.ScriptError, "role_get takes"):
         sc.Parser("script s { role_get(0, gender, 1, 2) -> var(v); }", "x").file_items()
      # a record binding of a role the layout does not hold
      with self.assertRaisesRegex(sc.ScriptError, "record names a record role"):
         sc.Compiler({"schema": "capsule.scripts/1", "sources": [], "battle": str(STATS3),
                      "placeholders": {"x": {"record": "friendship"}}}).read_manifest()
      with self.assertRaisesRegex(sc.ScriptError, "slot is a party slot, 0-2"):
         sc.Compiler({"schema": "capsule.scripts/1", "sources": [], "battle": str(STATS3),
                      "placeholders": {"x": {"record": "gender", "slot": 3}}}).read_manifest()
      with self.assertRaisesRegex(sc.ScriptError, "cannot read battle manifest"):
         sc.Compiler({"schema": "capsule.scripts/1", "sources": [], "battle": "nope.json"}).read_manifest()


class I4Statements(unittest.TestCase):
   """I4 (D-29): the statements imported event scripts need, their bytes and their checks."""

   SRC = """
var v : u8;
flag f;
movement walk_up { up; }
mart shop { 1, 2, 3 }
step spin;
script other { end; }
script s {
   lock; lock all; release; release all;
   msg "Hi" nowait instant;
   msgbox npc "Hello";
   msgbox yesno "Sure?" -> var(v);
   wait_msg;
   route 1 walk_up nowait;
   wait_moves last;
   remove 1; visible 1 off; visible player on;
   set_template 1 pos 3 4; set_template 1 movement 2; set_template 1 here;
   door open 1 2; door wait;
   weather set 3; weather apply;
   fade to_black speed 2; fade from_black;
   set_metatile 1 2 0x200 1;
   respawn 5;
   sound se 5; wait_sound se;
   mart shop;
   wait_state;
   when not glitch("g.x") { set flag(f); } else { clear flag(f); }
   call other loose;
   route 1 { step spin 3; in_place left 3; face original; }
   warp Somewhere 1 2 nowait;
   goto other;
}
map_scripts Here { on_resume -> other; on_warp_into var(v) == 1 -> other; }
"""

   def compile(self, glitch: bool) -> tuple[sc.Compiler, bytes]:
      """Compiles SRC with object_base 1 and the glitch switch.
      @param glitch  the glitch's switch
      @return (the Compiler, script s's bytes)
      """
      comp = compile_text(self.SRC, object_base=1, glitches={"g.x": glitch})
      self.assertEqual(errors(comp), [])
      o, n, _ = comp.result["scripts"][comp.script_ids["s"]]
      return comp, bytes(comp.result["code"][o:o + n])

   def test_bytes(self) -> None:
      """Locks, message modes and boxes, object waits, templates, doors, fades, sound, marts, jumps, loose calls."""
      comp, code = self.compile(False)
      op = sc.OP
      self.assertTrue(code.startswith(bytes([op["Lock"], sc.LOCK_SELF, op["Lock"], sc.LOCK_ALL, op["Release"],
                                             sc.LOCK_SELF, op["Release"], sc.LOCK_ALL])))
      flags = sc.MSG_FLAGS["nowait"] | sc.MSG_FLAGS["instant"]
      self.assertIn(bytes([op["MessageEx"], flags]), code)
      # msgbox npc: lock, the object turns to the player, the message, a button, release
      self.assertIn(bytes([op["Lock"], sc.LOCK_SELF, op["ObjectFace"], sc.OPND_CONST8, sc.OBJECT_SELF,
                           sc.FACE_PLAYER, op["Message"]]), code)
      self.assertIn(bytes([op["WaitObject"], sc.OPND_CONST8, sc.OBJECT_LAST]), code)
      self.assertIn(bytes([op["ObjectVisible"], sc.OPND_CONST8, sc.OBJECT_PLAYER, 1]), code)
      self.assertIn(bytes([op["ObjectTemplate"], sc.OPND_CONST8, 1, sc.TEMPLATE_KINDS.index("pos"), sc.OPND_CONST8, 3,
                           sc.OPND_CONST8, 4]), code)
      self.assertIn(bytes([op["Door"], sc.DOOR_KINDS.index("wait")]), code)
      self.assertIn(bytes([op["Fade"], sc.FADE_KINDS.index("to_black"), 2, 0]), code)
      self.assertIn(bytes([op["Mart"], 0, 3, 0, 1, 0, 2, 0, 3, 0]), code)
      self.assertIn(bytes([op["WaitState"], op["SetFlag"], 0, 0]), code)   # the glitch off: the fix (flag f is 0)
      other = comp.script_ids["other"]
      self.assertIn(bytes([op["CallLoose"], other, 0]), code)
      self.assertTrue(code.endswith(bytes([op["Jump"], other, 0])))   # a goto naming a script
      warp_facing = sc.KEEP_FACING | sc.WARP_NOWAIT
      self.assertIn(bytes([warp_facing, sc.TRANSITION_DEFAULT]), code)
      kinds = [t[4] for t in comp.result["triggers"]]
      self.assertEqual(kinds, [sc.TRIGGER_KINDS.index("on_resume"), sc.TRIGGER_KINDS.index("on_warp_into")])
      self.assertEqual(comp.object_base, 1)
      self.assertIn("    1,\n};", comp.emit_cpp("main"))   # Bank::object_base, the initializer's last member

   def test_glitch_on(self) -> None:
      """The glitch on: the original branch."""
      _, code = self.compile(True)
      self.assertIn(bytes([sc.OP["WaitState"], sc.OP["ClearFlag"], 0, 0]), code)

   def test_checks(self) -> None:
      """Object numbering from object_base, the camera's limits, unknown movements, marts and steps."""
      base = {"object_base": 1}
      cases = {"script s { remove 0; }": "is not a local ID",
               "script s { remove camera; }": "remove takes a map object",
               "script s { route 1 nothing; }": "unknown movement",
               "script s { mart nothing; }": "unknown mart",
               "script s { route 1 { step nothing; } }": "unknown game route step"}
      for src, text in cases.items():
         with self.subTest(src):
            errs = errors(compile_text(src, **base))
            self.assertTrue(any(text in e for e in errs), errs)
      self.assertEqual(errors(compile_text("script s { remove 64; }", **base)), [])
      self.assertTrue(any("is not a local ID" in e for e in errors(compile_text("script s { remove 65; }", **base))))

   def test_placeholder_ids(self) -> None:
      """A placeholder's ':' part becomes '_' in the IDs header."""
      comp = compile_text("script s { }", placeholders={"str:1": {"name": 8}})
      self.assertEqual(errors(comp), [])
      self.assertIn("PLACEHOLDER_STR_1 = 0;", comp.emit_ids("main"))


if __name__ == "__main__":
   unittest.main()
