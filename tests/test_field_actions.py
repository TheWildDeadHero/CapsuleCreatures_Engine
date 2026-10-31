#!/usr/bin/env python3
"""!Tests of the field actions and their mechanisms in the compilers (D-27 section 3, package F2): the structure's
field_actions (tools/battlec/field.py: records, requirements as growth rules with knows.MOVE, passive words and the
game references, the Python mirror), the world's object kinds, map tags, flight points, a map's darkness, tags and
escape point and a vehicle class that dismounts (tools/map_compiler.py, tools/object_compiler.py), and the script
compiler's side (tools/script_compiler.py: targets resolved against the world, the implicit member variable, the
asking wrappers, the flight flags and escape variables, the statements darkness / flight / escape / field_action),
on the neutral fixtures tests/data/field and tests/data/maps (run by CTest, or directly)."""
from __future__ import annotations

import copy
import json
import sys
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler as bc  # noqa: E402
import map_compiler as mc  # noqa: E402
import script_compiler as sc  # noqa: E402

## A parsed JSON object.
JsonDict = dict[str, Any]

## The neutral field fixture.
FIXTURE = ROOT / "tests/data/field/battle.json"
## Its scripts manifest (names the fixture's world).
FIXTURE_SCRIPTS = ROOT / "tests/data/field/scripts.json"
## The map test world (escape points, flight points, darkness).
MAP_WORLD = ROOT / "tests/data/maps/world.json"
## The map test world's scripts manifest.
MAP_SCRIPTS = ROOT / "tests/data/maps/scripts/scripts.json"
## The Starter sample.
STARTER = ROOT / "samples/starter/battle.json"
## The members' level in the mirror's parties.
LEVEL = 5


def compile_with(edit: Callable[[JsonDict], None]) -> JsonDict:
   """!Compiles the fixture after an edit of its merged view, from memory.
   @param edit  callable(merged view)
   @return compiled
   """
   data = bc.view_of(FIXTURE)
   edit(data)
   return bc.compile_battle(FIXTURE, None, bc.sources_of(FIXTURE, data))


def with_actions(actions: list[JsonDict]) -> Callable[[JsonDict], None]:
   """!An edit that replaces the fixture's field actions.
   @param actions  the new list
   @return the edit
   """
   def edit(data: JsonDict) -> None:
      """!Replaces them.
      @param data  the merged view
      @return None
      """
      data["field_actions"] = actions
   return edit


def scripts(text: str, actions: list[JsonDict] | None = None) -> sc.Compiler:
   """!Compiles the fixture's scripts manifest with another source text (its world's F2 data loaded).
   @param text     field.cscript's text
   @param actions  replacement field action records (compile_field_action_records() form), or None
   @return the compiler (run)
   """
   manifest = json.loads(FIXTURE_SCRIPTS.read_text(encoding="utf-8"))
   comp = sc.Compiler(manifest, FIXTURE_SCRIPTS.parent, sources={"field.cscript": text})
   comp.world_field = sc.load_world_field(FIXTURE_SCRIPTS.parent / manifest["world"])
   if actions is not None:
      real_run = comp.read_manifest

      def read_manifest() -> list[str]:
         """!The manifest, then the test's field actions.
         @return the source paths
         """
         paths = real_run()
         comp.field_actions = actions
         return paths
      comp.read_manifest = read_manifest   # type: ignore[method-assign]
   return comp.run()


## The fixture's own declarations its field actions need (the scripts they run, the flag and variable they read).
NEEDS = ("flag badge_one;\nvar story : u8 0..9;\nscript do_paddle { }\nscript do_shove { }\nscript do_lamp { }\n"
         "script do_rope { }\n")


class BattleSideTests(unittest.TestCase):
   """!structure.json field_actions in the battle compiler."""

   def test_fixture(self) -> None:
      """The four actions: their rules follow the lead rule, the game references, the ids and kFieldRules."""
      c = compile_with(lambda d: None)
      acts = c["field"]["actions"]
      self.assertEqual([a["name"] for a in acts["actions"]], ["paddle", "shove", "lamp", "rope"])
      self.assertEqual([(a["target"], a["target_name"], a["ask"]) for a in acts["actions"]],
                       [("behavior", "water", True), ("object", "boulder", False), ("tag", "dark", True),
                        ("none", None, False)])
      self.assertEqual(acts["refs"], [("flag", "badge_one"), ("var", "story")])
      self.assertEqual([a["rule"] for a in acts["actions"]], list(range(acts["first_rule"], acts["first_rule"] + 4)))
      rules = c["transform"]["rules"]
      self.assertEqual(rules[acts["first_rule"]]["owner"], "field_actions:paddle")
      self.assertEqual(sorted(rules[acts["first_rule"]]["used"]), ["flag.badge_one", "knows.spark"])
      ids = bc.emit_ids("battle.json", c)
      self.assertIn("FIELD_ACTION_ROPE = 3", ids)
      self.assertIn("NUM_FIELD_ACTIONS = 4", ids)
      cpp = bc.emit("battle.json", c)
      self.assertIn(f"kFieldRules = {{0x0000, 3, 4, 0x{acts['first_rule']:04X}, 0, 0, "
                    f"0x{c['field']['hook_lead_rule']:04X}, 0}}", cpp)
      self.assertIn("rules::field_state(0)", cpp)
      self.assertIn("rules::member_knows(in, 0)", cpp)
      self.assertIn("rules::member_passive(in)", cpp)

   def test_member_passive_form(self) -> None:
      """C8 (D-27 review): the Python mirror reads a member's passive from its form, as rules::member_passive() does
      (growth::species_view()): the Starter's Glimmet given a form with another passive."""
      from battlec.field import member_passive
      c = bc.compile_battle(STARTER)
      si = [sp["name"] for sp in c["species"]].index("Glimmet")   # key order
      base = c["species"][si]["passives"]
      other = next(k for k in range(len(c["names"]["passives"])) if k not in base)
      form = c["transform"]["species"][si]["forms"][0]
      form["passives"] = [other] + list(form["passives"][1:])
      role = c["transform"]["transformation"]["form_role_name"]
      record = {"species": "Glimmet", "passive_selector": 0}
      self.assertEqual(member_passive(c, {**record, role: 0}), base[0] + 1)
      self.assertEqual(member_passive(c, {**record, role: 1}), other + 1)

   def test_mirror(self) -> None:
      """The Python mirror picks the member the engine picks (tests/field_actions_tests.cpp)."""
      c = compile_with(lambda d: None)
      party = [{"species": "Cube", "level": LEVEL, "moves": ["Tap"]},
               {"species": "Pebble", "level": LEVEL, "moves": ["Spark", "Rush"]},
               {"species": "LeadKeen", "level": LEVEL, "moves": ["Spark"]}]
      self.assertIsNone(bc.action_member(c, party, "paddle"))
      self.assertEqual(bc.action_member(c, party, "paddle", {"flag.badge_one": 1}), 1)
      self.assertEqual(bc.action_member(c, party, "shove"), 1)
      self.assertEqual(bc.action_member(c, party, "lamp"), 2)
      self.assertEqual(bc.action_member(c, party, "lamp", {"var.story": 2}), 0)
      self.assertEqual(bc.action_member(c, party, "rope"), 0)
      with self.assertRaises(bc.ManifestError):
         bc.action_member(c, party, "nope")

   def test_refused(self) -> None:
      """Records and requirements the compiler refuses."""
      base = {"name": "a", "script": "s"}
      bad = [([{**base, "name": "Bad"}], "name of its own"),
             ([base, base], "name of its own"),
             ([{**base, "label": "x" * 17}], "label"),
             ([{**base, "target": {"behavior": "water", "tag": "dark"}}], "target"),
             ([{**base, "target": {"cell": "x"}}], "unknown key"),
             ([{**base, "ask": "yes"}], "ask"),
             ([{**base, "prompt": ""}], "prompt"),
             ([{"name": "a"}], "script"),
             ([{**base, "requires": "knows.nothing"}], "knows.nothing"),
             ([{**base, "requires": "level > 300"}], "never hold"),
             ([{**base, "requires": " "}], "requires"),
             ([{**base, "name": f"a{k}"} for k in range(bc.MAX_FIELD_ACTIONS + 1)], "at most")]
      for actions, words in bad:
         with self.subTest(words=words):
            with self.assertRaisesRegex(bc.ManifestError, words):
               compile_with(with_actions(actions))

   def test_words(self) -> None:
      """Passive words, species fields and roles read together; a battle without actions is unchanged."""
      c = compile_with(with_actions([{"name": "a", "script": "s",
                                      "requires": "passive == passive.keen && species.size < 2 && level >= 3"}]))
      self.assertEqual(c["field"]["actions"]["refs"], [])
      party = [{"species": "Pebble", "level": LEVEL}, {"species": "LeadKeen", "level": LEVEL}]
      self.assertEqual(bc.action_member(c, party, "a"), 1)
      none = compile_with(with_actions([]))
      self.assertEqual(none["field"]["actions"], {"actions": [], "first_rule": bc.NO_ACTION_RULE, "refs": []})
      self.assertNotIn("NUM_FIELD_ACTIONS", bc.emit_ids("battle.json", none))

   def test_starter(self) -> None:
      """The Starter's three actions (D-27 decision 3): wade, clear, light, each asking."""
      c = bc.compile_battle(STARTER)
      acts = c["field"]["actions"]["actions"]
      self.assertEqual([(a["name"], a["target"], a["target_name"], a["ask"]) for a in acts],
                       [("wade", "behavior", "water", True), ("clear", "object", "bush", True),
                        ("light", "tag", "dark", True)])
      party = [{"species": "Breezle", "level": LEVEL, "moves": ["Gale Kick"]},
               {"species": "Sproutle", "level": 3, "moves": ["Vine Lash"]},
               {"species": "Tidepup", "level": LEVEL, "moves": ["Tide Slam"]},
               {"species": "Glimmet", "level": LEVEL, "moves": ["Mend Light"]}]
      self.assertEqual([bc.action_member(c, party, a["name"]) for a in acts], [2, 1, 3])


class WorldTests(unittest.TestCase):
   """!The world's and the maps' F2 keys in the map compiler."""

   def setUp(self) -> None:
      """No edits yet (compile() reads the map test world's files)."""
      ## In-memory manifests replacing files (resolved path -> parsed JSON).
      self.src: dict[Path, JsonDict] = {}

   def edit(self, rel: str, change: Callable[[JsonDict], None]) -> None:
      """!Edits one manifest of the world, in memory.
      @param rel     path in the world folder
      @param change  callable(manifest)
      @return None
      """
      p = (MAP_WORLD.parent / rel).resolve()
      m = self.src.get(p) or json.loads(p.read_text(encoding="utf-8"))
      change(m)
      self.src[p] = m

   def compile(self) -> JsonDict:
      """!Compiles the world with the edits.
      @return compile_world() result
      """
      return mc.compile_world(MAP_WORLD, sources=self.src, scripts_path=MAP_SCRIPTS)

   def test_fixture(self) -> None:
      """Darkness, tags, the escape point, the object kind, the dismount flag and their emitted forms."""
      w = self.compile()
      maps = {mp["name"]: mp for mp in w["maps"]}
      self.assertEqual((maps["small"]["darkness"], maps["small"]["tags"]), (2, 1))
      self.assertEqual(maps["field"]["flags"], mc.MAP_ESCAPE_POINT)
      self.assertEqual([o["kind"] for o in w["events"]["objects"]["town"]["objects"]], [0, 0, 0, 0, 1])
      cpp = mc.emit("world.json", w)
      self.assertIn("kMapNoRun, 2, 0x0001},", cpp)
      self.assertIn("{1, 2, 0, kVehicleDismount},   // paddle", cpp)
      self.assertEqual([fp["name"] for fp in mc.compile_flight_points(json.loads(MAP_WORLD.read_text()),
                                                                       list(maps))], ["home", "town_gate"])

   def test_refused(self) -> None:
      """Bad darkness, tags, escape points, kinds, flight points, dismounts."""
      cases = [("maps/small.json", lambda m: m.update(darkness=5), "darkness"),
               ("maps/small.json", lambda m: m.update(tags=["bright"]), "map_tags"),
               ("maps/field.json", lambda m: m.update(escape_point=1), "escape_point"),
               ("maps/town.json", lambda m: m["objects"][4].update(kind="crate"), "object_kinds"),
               ("world.json", lambda w: w.update(map_tags=["a"] * 2), "distinct"),
               ("world.json", lambda w: w.update(flight_points=[{"name": "x", "map": "nowhere", "x": 0, "y": 0}]),
                "unknown map"),
               ("world.json", lambda w: w["flight_points"].append(copy.deepcopy(w["flight_points"][0])),
                "name of its own"),
               ("world.json", lambda w: w["vehicle_classes"][0].update(dismount=True), "dismount")]
      for rel, change, words in cases:
         with self.subTest(words=words):
            self.setUp()
            self.edit(rel, change)
            with self.assertRaisesRegex(mc.ManifestError, words):
               self.compile()


class ScriptSideTests(unittest.TestCase):
   """!The script compiler's side of F2."""

   def test_fixture(self) -> None:
      """Targets resolved against the world, the asking wrappers, the references and the flight point."""
      comp = scripts(NEEDS)
      self.assertEqual([str(d) for d in comp.errors()], [])
      r = comp.result
      ids = comp.script_ids
      self.assertEqual([(s, tk, tv, fl) for _lb, s, tk, tv, fl, _n in r["field_actions"]],
                       [(ids["field_action_ask_paddle"], 1, 1, sc.ACTION_ASKS), (ids["do_shove"], 2, 1, 0),
                        (ids["field_action_ask_lamp"], 3, 0, sc.ACTION_ASKS), (ids["do_rope"], 0, 0, 0)])
      self.assertEqual([(kd, n) for _i, kd, n in r["field_action_refs"]], [(0, "flag.badge_one"), (1, "var.story")])
      self.assertEqual(r["f2_vars"]["member"], comp.var_ids["field_action_member"])
      self.assertEqual(r["f2_vars"]["escape"], [sc.NO_VAR] * len(sc.ESCAPE_VARS))   # no escape point here
      self.assertEqual([fp[-1] for fp in r["flight_points"]], ["plain"])
      cpp = comp.emit_cpp("scripts.json")
      self.assertIn("kFieldActions, kFieldActionRefs, kFlightPoints,", cpp)
      self.assertIn("FLIGHT_POINT_PLAIN = 0", comp.emit_ids("scripts.json"))
      self.assertFalse([d for d in comp.diags if "never used" in str(d) and "badge_one" in str(d)])

   def test_refused(self) -> None:
      """Missing scripts, flags and targets, a world without the target."""
      comp = scripts("script do_paddle { }")
      text = " ".join(str(d) for d in comp.errors())
      self.assertIn("do_shove", text)
      self.assertIn("flag.badge_one", text)
      base = {"name": "a", "label": "A", "requires": None, "ask": False, "prompt": None, "script": "do_rope"}
      for target, name, words in (("behavior", "lava", "lava"), ("object", "crate", "crate"),
                                  ("tag", "bright", "bright")):
         with self.subTest(words=words):
            comp = scripts(NEEDS, [{**base, "target": target, "target_name": name}])
            self.assertTrue(any(words in str(d) for d in comp.errors()), [str(d) for d in comp.errors()])
      manifest = json.loads(FIXTURE_SCRIPTS.read_text(encoding="utf-8"))
      comp = sc.Compiler(manifest, FIXTURE_SCRIPTS.parent, sources={"field.cscript": NEEDS}).run()
      self.assertTrue(any("needs the manifest's world" in str(d) for d in comp.errors()))

   def test_statements(self) -> None:
      """darkness, flight, escape and field_action encode as documented; their checks."""
      comp = sc.compile_file(MAP_SCRIPTS)
      self.assertEqual([str(d) for d in comp.errors()], [])
      r = comp.result
      op = sc.OP
      code = {n: r["code"][o:o + ln] for n, (o, ln, _w) in zip(r["names"], r["scripts"])}
      self.assertEqual(code["go_dark"], bytes([op["Darkness"], sc.OPND_CONST8, 3, op["End"]]))
      self.assertEqual(code["fly_home"], bytes([op["Flight"], 0, 0, op["Flight"], 1, 0, sc.TRANSITION_DEFAULT,
                                                op["End"]]))
      esc = comp.var_ids["escaped"]
      self.assertEqual(code["run_away"], bytes([op["Escape"], 0, esc & 0xFF, esc >> 8, op["End"]]))
      self.assertEqual(r["f2_vars"]["escape"], [comp.var_ids[n] for n in sc.ESCAPE_VARS])
      self.assertEqual([fp[-1] for fp in r["flight_points"]], ["home", "town_gate"])
      # field_action available on the field fixture
      comp = scripts(NEEDS + "var v : u8;\nscript s { field_action available lamp -> var(v); }\n")
      self.assertEqual([str(d) for d in comp.errors()], [])
      o, ln, _w = comp.result["scripts"][comp.script_ids["s"]]
      v = comp.var_ids["v"]
      self.assertEqual(comp.result["code"][o:o + ln], bytes([op["FieldAction"], 0, 2, v & 0xFF, v >> 8, op["End"]]))
      bad = [("script s { darkness 9; }", "0 lit to"),
             ("script s { flight visit nowhere; }", "nowhere"),
             ("script s { flight menu; }", "needs -> var"),
             ("script s { escape; }", "escape_point"),
             ("var v : u8;\nscript s { field_action available nope -> var(v); }", "nope"),
             ("script s { field_action available lamp; }", "needs -> var")]
      for text, words in bad:
         with self.subTest(text=text):
            comp = scripts(NEEDS + text)
            self.assertTrue(any(words in str(d) for d in comp.errors()), [str(d) for d in comp.errors()])

   def test_names_stay_free(self) -> None:
      """The statement words are no keywords: a variable may still be called darkness or escape."""
      comp = scripts(NEEDS + "var darkness : u8;\nvar escape : u8;\nscript s { var(darkness) = 1; "
                     "var(escape) = 2; }\n")
      self.assertEqual([str(d) for d in comp.errors()], [])

   def test_prompt_text(self) -> None:
      """An asking action's wrapper shows its prompt (default "Use LABEL?"), quotes and line breaks kept."""
      src = sc.field_action_source([{"name": "a", "label": "Go", "ask": True, "prompt": None, "script": "s"},
                                    {"name": "b", "label": "B", "ask": True, "prompt": 'Say "hi"\nnow?',
                                     "script": "s"}], None)
      self.assertIn('msg "Use Go?";', src)
      self.assertIn('msg "Say \\"hi\\"\\nnow?";', src)
      self.assertIn("call s;", src)


if __name__ == "__main__":
   unittest.main()
