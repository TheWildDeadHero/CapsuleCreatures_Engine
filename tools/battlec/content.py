"""!Content of the battle compiler: moves (data; their logic is effect code,
battlec/logic.py), passives and held items (data and point handlers),
species with learnsets, and parties with their members.
"""
from __future__ import annotations

import re
from collections.abc import Mapping, Sequence
from typing import Any

from sequence_compiler import asset_key

from .ai import FLAG_RULES, compile_ai_profiles, party_profile
from .chart import MAX_TYPES_PER
from .common import (I16_MAX, NONE8, NONE16, PERCENT, PAIR, Q8_ONE, U8_MAX, U16_MAX, Json, Lookups, ManifestError,
                     Texts, check_keys, integer, register_keys)
from .layout import DEFAULT_MOVE_SLOTS, MAX_MOVE_SLOTS, PARTY_SIZE
from .presenters import MAX_PRESENTERS
from .resources import base_fields, compile_cost, move_bases
from .stats import is_damaging

## Move targets, in capsule::battle::Target order.
TARGETS = ["selected", "selected_foe", "self", "ally", "all_foes", "all_others", "random_foe", "field"]
## Owner kind of each holder list (the effect language's owner words).
OWNER_KIND = {"passives": "passive", "items": "item"}
## The former party AI flags (capsule::battle::AiFlag before G1; a party names an AI profile now: battlec/ai.py,
## tools/battle_ai.py migrates a flag list).
AI_FLAGS = list(FLAG_RULES)
## The passive slots of a structure without "passive_slots" (D-25 decision 6: the manifest's count): one rolled
## slot. A game with more (a Gen-3-style hidden one, say) declares them.
DEFAULT_PASSIVE_SLOTS = [{"name": "passive", "rolled": True}]
## A passive slot's name: a lower-case identifier (editors and member data name it).
PASSIVE_SLOT_NAME = re.compile(r"[a-z][a-z0-9_]*\Z")
## Default of a passive slot's "rolled".
DEFAULT_ROLLED = True
## Party flag bit: foes cannot run (1 << capsule::battle::PARTY_NO_ESCAPE).
PARTY_NO_ESCAPE = 2
## Default party rank.
DEFAULT_RANK = 1
## Most items a party may use in battle (capsule::battle::kMaxPartyItems; G3).
MAX_PARTY_ITEMS = 4
## Move slots a party member's data holds (capsule::monster::kMaxMoveSlots; MemberData::moves): a member names
## up to the layout's move_slots, the rest are padded with NONE16.
MAX_MOVES = MAX_MOVE_SLOTS
## Learnset entries per species (an 8-bit count).
MAX_LEARNSET = 255
## Bits of the Q8 shift in a share `max(1, x * q >> 8)` (the fixed-damage summary reads it).
Q8_SHIFT = 8
## Highest move priority (the lowest is its negative).
MAX_PRIORITY = 7
## Species default: experience yield (neutral, D-22 decision 8: none; a species that gives experience states it).
DEFAULT_EXP_YIELD = 0
## Most moves one species learns at one level (one level-up stays one small unit of events).
MAX_LEARN_PER_LEVEL = 4
## Most species fields (structure species_fields, E13a).
MAX_SPECIES_FIELDS = 16
## Widest species field (derived expressions analyse int32 values: 31 bits stay non-negative).
MAX_SPECIES_FIELD_BITS = 31
## A species field's name: a lower-case identifier (derived expressions read it as species.NAME).
SPECIES_FIELD_NAME = re.compile(r"[a-z][a-z0-9_]*\Z")

register_keys({
   "passive_slot": {"name", "rolled"},
})


def compile_passive_slots(src: Json, limit: int, where: str) -> list[dict[str, Any]]:
   """!The structure's passive slots (structure.json "passive_slots"; D-25 decision 6), in slot order:
   [{"name", "rolled"}]. A species names up to one passive per slot (the rest are empty: the passive of the first
   rolled slot it fills applies, never a slot only given; capsule::battle passive_for()); a party member names its
   slot ("passive": index or name); the generation entry {"role": "passive_selector", "random_range": [0, "last"]}
   rolls one of the rolled slots ("rolled": false: a slot only data or logic gives, such as a Gen-3-style hidden
   one). Default: DEFAULT_PASSIVE_SLOTS.
   @param src    the manifest's passive_slots, or None
   @param limit  this platform's most slots (build_settings PLATFORM_LIMITS max_passive_slots)
   @param where  location (the structure manifest)
   @return [{"name", "rolled"}]
   """
   entries = DEFAULT_PASSIVE_SLOTS if src is None else src
   w0 = f"{where}: passive_slots"
   if not isinstance(entries, list) or not 1 <= len(entries) <= limit:
      raise ManifestError(f"{w0}: a list of 1 to {limit} passive slots (this platform's engine holds {limit})")
   out: list[dict[str, Any]] = []
   for i, e in enumerate(entries):
      w = f"{w0}[{i}]"
      check_keys(e, "passive_slot", w)
      name = e.get("name")
      if not isinstance(name, str) or not PASSIVE_SLOT_NAME.match(name):
         raise ManifestError(f"{w}.name: a lower-case identifier, got {name!r}")
      if any(o["name"] == name for o in out):
         raise ManifestError(f"{w}: passive slot {name!r} is declared twice")
      rolled = e.get("rolled", DEFAULT_ROLLED)
      if not isinstance(rolled, bool):
         raise ManifestError(f"{w}.rolled: true or false")
      out.append({"name": name, "rolled": rolled})
   return out


def passive_slot_bits(slots: Sequence[Mapping[str, Any]]) -> int:
   """!The passive selector bits that hold a slot index (capsule::battle::BattleData::passive_selector_mask).
   @param slots  compile_passive_slots() result
   @return bits (0 for one slot)
   """
   return (len(slots) - 1).bit_length()


def passive_rolled_mask(slots: Sequence[Mapping[str, Any]]) -> int:
   """!Bit per rolled passive slot (capsule::battle::BattleData::passive_rolled).
   @param slots  compile_passive_slots() result
   @return the mask
   """
   return sum(1 << k for k, sl in enumerate(slots) if sl["rolled"])


def compile_moves(entries: Sequence[Json], category_entries: Sequence[Json], categories: Sequence[Mapping[str, int]],
                  n: Lookups, where: str, enc: Texts, logic: Mapping[tuple[str, str], Json],
                  move_flags: Sequence[str] = (), crit_max: int = 0, resources: Sequence[Mapping[str, Any]] = (),
                  ruleset: Mapping[str, Any] | None = None, rs_name: str = "",
                  type_categories: Sequence[int] | None = None) -> list[dict[str, Any]]:
   """!Moves (their logic is their `.ceffect` owner, battlec/logic.py; their bases and costs are the resources',
   battlec/resources.py).
   @param entries           the manifest's moves
   @param category_entries  the manifest's categories (the first is the default)
   @param categories        compile_categories() result
   @param n                 lookups
   @param where             location
   @param enc               this compile's text encoders
   @param logic             `.ceffect` owners by (kind, name)
   @param move_flags        the manifest's move flags (a move's "flags" name them; bit n = move_flags[n])
   @param crit_max          highest crit_stage a move may add (the crit stage's highest stage; 0 without one)
   @param resources         compiled resources (each move states every move resource's base field; "cost")
   @param ruleset           compiled ruleset (cost formulas), or None without formula costs
   @param rs_name           ruleset file name (messages)
   @param type_categories   the category per type of the structure's category_by_type rule
                            (stats.compile_category_by_type()), or None; with it a move without a category takes
                            its type's, and a move's damaging category must be its type's
   @return compiled moves ("formula": the move deals formula damage; the AI's summary of its logic; "base": the
           move resources' bases; "cost": [{resource, amount, formula}])
   """
   cat_names = n["categories"]
   bases = base_fields(resources)
   moves = []
   for i, m in enumerate(entries):
      w = f"{where}: moves[{i}] ({m.get('name')})"
      if not isinstance(m, dict):
         raise ManifestError(f"{w}: must be an object")
      if "pp" in m and "pp" not in bases:
         raise ManifestError(f"{w}.pp: a move's pp is the base of a move resource now (D-25): declare "
                             f"{{\"name\": \"pp\", \"scope\": \"move\", \"base\": \"pp\", ...}} in the structure's "
                             f"resources (tools/battle_resources.py migrates a battle)")
      check_keys({k: v for k, v in m.items() if k not in bases}, "move", w)
      if type_categories is None:
         cat = cat_names(m.get("category", category_entries[0]["name"]), f"{w}.category")
      else:
         # category_by_type: the type decides a damaging move's category; a move may only name a non-damaging one
         by_type = type_categories[n["types"](m.get("type"), f"{w}.type")]
         cat = cat_names(m["category"], f"{w}.category") if "category" in m else by_type
         if is_damaging(categories, cat) and cat != by_type:
            raise ManifestError(f"{w}.category: the structure's category_by_type decides a damaging move's category "
                                f"({category_entries[by_type]['name']} for {m.get('type')}); state none, or a "
                                f"non-damaging one")
      hits = is_damaging(categories, cat)
      tgt = m.get("target", "selected")
      if tgt not in TARGETS:
         raise ManifestError(f"{w}: target must be one of {', '.join(TARGETS)}")
      flags = 0
      for f in m.get("flags", []):
         if f not in move_flags:
            raise ManifestError(f"{w}: unknown flag {f!r} (the manifest's move_flags: "
                                f"{', '.join(move_flags) or 'none'})")
         flags |= 1 << list(move_flags).index(f)
      power = integer(m.get("power", 0), 0, U16_MAX, f"{w}.power")
      if not hits and power:
         raise ManifestError(f"{w}: moves of a non-damaging category have no power")
      code = logic.get(("move", m["name"]))
      if code is None:
         if hits and not power:
            raise ManifestError(f"{w}: a move of a damaging category without .ceffect logic hits with its power "
                                f"(give it power, or logic)")
         ops = ["damage"] if hits else []   # the standard hit (battlec/logic.py)
         hints = logic_hints(None)
      else:
         ops = logic_ops(code)
         if hits and power and not any(h.get("phase") == "hit" for h in code.get("handlers", [])):
            ops.append("damage")   # the default hit phase (battlec/logic.py)
         hints = logic_hints(code)
      if "damage" in ops and (not hits or power == 0):
         raise ManifestError(f"{w}: a damage effect needs a move of a damaging category with power")
      acc = m.get("accuracy", PERCENT)
      moves.append({"name": enc.text(m["name"], f"{w}.name"), "formula": "damage" in ops,
                    "type": n["types"](m.get("type"), f"{w}.type"), "category": cat,
                    "power": power, "accuracy": 0 if acc is None else integer(acc, 1, PERCENT, f"{w}.accuracy"),
                    "base": move_bases(m, resources, w),
                    "cost": compile_cost(m, resources, ruleset or {"formulas": {}}, rs_name, w),
                    "priority": integer(m.get("priority", 0), -MAX_PRIORITY, MAX_PRIORITY, f"{w}.priority"),
                    "target": TARGETS.index(tgt), "flags": flags,
                    "crit_stage": integer(m.get("crit_stage", 0), 0, crit_max, f"{w}.crit_stage"),
                    "statuses": min(U8_MAX, ops.count("status")), **hints})
      if hints["fixed_amount"] > I16_MAX or hints["fixed_levels"] > U8_MAX:
         raise ManifestError(f"{w}: too much fixed damage")
   return moves


def walk_statements(body: Sequence[Json]) -> list[Json]:
   """!Every statement of a body, nested blocks included, in source order.
   @param body  statements
   @return the statement nodes
   """
   out: list[Json] = []
   for s in body:
      out.append(s)
      for key in ("then", "else", "body"):
         if isinstance(s.get(key), list):
            out += walk_statements(s[key])
   return out


def owner_statements(owner: Json) -> list[Json]:
   """!Every statement of an owner's handlers.
   @param owner  the owner node
   @return the statement nodes
   """
   return [s for h in owner.get("handlers", []) if isinstance(h.get("body"), list) for s in walk_statements(h["body"])]


def logic_ops(owner: Json) -> list[str]:
   """!What a `.ceffect` move's statements do, for its checks and the AI: damage for `damage`, status for
   `inflict`.
   @param owner  the owner node
   @return op names, in statement order
   """
   return ["damage" if s["stmt"] == "damage" else "status" for s in owner_statements(owner)
           if s.get("stmt") in ("damage", "inflict")]


def logic_hints(owner: Json | None) -> dict[str, int]:
   """!The fixed damage the AI reads of a move (MoveData fixed_amount, fixed_levels, fixed_fraction): the
   `damage_hp target AMOUNT typed;` statements whose amount is a number, `user.level` or a share of the
   target's HP, `max(1, target.hp * Q8 >> 8)`.
   @param owner  the move's owner node, or None (no logic)
   @return the fields
   """
   amount = levels = fraction = 0
   for s in owner_statements(owner) if owner is not None else []:
      if s.get("stmt") != "damage_hp" or s.get("who") != "target" or not s.get("typed"):
         continue
      v = s.get("amount")
      if isinstance(v, int) and not isinstance(v, bool):
         amount += v
      elif v == {"ref": "user.level"}:
         levels += 1
      elif share_of_hp(v) and not fraction:
         fraction = share_of_hp(v)
   return {"fixed_amount": amount, "fixed_levels": levels, "fixed_fraction": fraction}


def share_of_hp(v: Json) -> int:
   """!The Q8 share of `max(1, target.hp * Q8 >> 8)`.
   @param v  an expression node
   @return the share (1-256), or 0 for another expression
   """
   if not isinstance(v, dict) or v.get("call") != "max" or len(v.get("args", [])) != PAIR or v["args"][0] != 1:
      return 0
   shift = v["args"][1]
   if not isinstance(shift, dict) or shift.get("op") != ">>" or shift["args"][1] != Q8_SHIFT:
      return 0
   product = shift["args"][0]
   if not isinstance(product, dict) or product.get("op") != "*" or product["args"][0] != {"ref": "target.hp"}:
      return 0
   q = product["args"][1]
   return q if isinstance(q, int) and not isinstance(q, bool) and 1 <= q <= Q8_ONE else 0


register_keys({
   "move": {"name", "type", "category", "power", "accuracy", "cost", "priority", "target", "flags", "crit_stage"},
   "effect": {"name", "announce", "handlers"},
   # G4: transforms / forms are tools/battlec/growth.py's
   "species": {"name", "types", "base", "passives", "growth", "exp_yield", "front", "back", "learnset", "fields",
               "transforms", "forms", "teachable", "combine_moves"},
   "species_field": {"name", "width", "default", "values"},
   "party": {"name", "presenters", "reward", "rank", "escape", "ai", "members", "items"},
   "member": {"species", "level", "moves", "item", "passive", "genes", "trait"},
})


def compile_holders(entries: Sequence[Json], key: str, w0: str, enc: Texts, handlers: list[dict[str, int]],
                    points: Any, logic_points: Mapping[tuple[str, str], Sequence[Json]]) -> list[dict[str, Any]]:
   """!Passives or items: their point handlers (the manifest's {"point", "code"} handlers, then the point members
   of their `.ceffect` owner) appended to the point handler table; their lifecycle logic is their `.ceffect`
   owner (battlec/logic.py).
   @param entries       the manifest's passives or items
   @param key           "passives" or "items"
   @param w0            location
   @param enc           this compile's text encoders
   @param handlers      point handler table (appended)
   @param points        the battle's point handlers (battlec.pipelines.PointHandlers)
   @param logic_points  `.ceffect` point members by (kind, name)
   @return compiled passives or items ("first" / "count" in the point handler table, "announce")
   """
   kind = OWNER_KIND[key]
   out = []
   for i, a in enumerate(entries):
      w = f"{w0}[{i}] ({a.get('name')})"
      # G2: an item also has its bag and use fields (battlec/inventory.py checks them)
      check_keys(a, "item" if key == "items" else "effect", w)
      first_h = len(handlers)
      hs = a.get("handlers", [])
      if not isinstance(hs, list):
         raise ManifestError(f"{w}.handlers must be a list")
      for j, h in enumerate(hs):
         handlers += points.handler(key, a["name"], h, f"{w}.handlers[{j}]")
      for node in logic_points.get((kind, a["name"]), []):
         handlers.append(points.add_node(key, a["name"], node, f"{w}: .ceffect point {node['point']}"))
      out.append({"name": enc.text(a["name"], f"{w}.name"), "first": first_h, "count": len(handlers) - first_h,
                  "announce": bool(a.get("announce", True)), "source": a["name"]})
   return out


def compile_species_fields(src: Json, where: str) -> list[dict[str, Any]]:
   """!The structure's species fields (E13a, D-23 section 2 species_fields): numeric data columns each species
   gives ("fields": {name: value}), read by the generation list's derived expressions as species.NAME. Each is
   {"name", "width": 1-31 bits, "default": the value of a species that leaves it out (0), "values": optional value
   names (B1, D-28: a group field such as a race or a family; value k is the k-th name, a species may give the
   name, combination tables and expressions read the words FIELD.NAME)}.
   @param src    the structure's species_fields (or None)
   @param where  location
   @return [{"name", "width", "max", "default", "values"}] in manifest order
   """
   src = [] if src is None else src
   w0 = f"{where}: species_fields"
   if not isinstance(src, list) or len(src) > MAX_SPECIES_FIELDS:
      raise ManifestError(f"{w0}: a list of at most {MAX_SPECIES_FIELDS} fields")
   out = []
   for i, f in enumerate(src):
      w = f"{w0}[{i}]"
      check_keys(f, "species_field", w)
      name = f.get("name")
      if not isinstance(name, str) or not SPECIES_FIELD_NAME.match(name):
         raise ManifestError(f"{w}.name: a lower-case identifier, got {name!r}")
      if any(o["name"] == name for o in out):
         raise ManifestError(f"{w}.name: {name!r} declared twice")
      width = integer(f.get("width"), 1, MAX_SPECIES_FIELD_BITS, f"{w}.width")
      top = (1 << width) - 1
      values = f.get("values", [])
      if not isinstance(values, list) or not all(isinstance(v, str) and SPECIES_FIELD_NAME.match(v)
                                                 for v in values):
         raise ManifestError(f"{w}.values: a list of lower-case identifiers (value 0 first)")
      if len(set(values)) != len(values):
         raise ManifestError(f"{w}.values: a name is given twice")
      if len(values) > top + 1:
         raise ManifestError(f"{w}.values: {len(values)} names do not fit {width} bits")
      default = field_value(f.get("default", 0), {"name": name, "values": values}, f"{w}.default")
      out.append({"name": name, "width": width, "max": top, "values": list(values),
                  "default": integer(default, 0, top, f"{w}.default")})
   return out


def field_value(v: Json, field: Mapping[str, Any], where: str) -> Json:
   """!A species field's value as a number (B1: a field with value names takes one of its names).
   @param v      the manifest's value (a number or a value name)
   @param field  the field ({"name", "values"})
   @param where  location
   @return the number (other values unchanged: integer() checks them)
   """
   if isinstance(v, str):
      names = field.get("values") or []
      if v not in names:
         raise ManifestError(f"{where}: {v!r} is not a value of {field['name']!r} "
                             f"({', '.join(names) or 'it names no values'})")
      return names.index(v)
   return v


def compile_species(entries: Sequence[Json], n: Lookups, growth_names: list[str], level_max: int, where: str,
                    enc: Texts, stat_names: Sequence[str], types_per_species: int = 1,
                    passive_slots: Sequence[Mapping[str, Any]] = DEFAULT_PASSIVE_SLOTS,
                    species_fields: Sequence[Mapping[str, Any]] = ()) -> list[dict[str, Any]]:
   """!Species, sorted by key.
   @param entries       the manifest's species
   @param n             lookups
   @param growth_names  growth table names
   @param level_max     highest level
   @param where         location
   @param enc           this compile's text encoders
   @param stat_names    the manifest's stats (one base stat each, in this order)
   @param types_per_species  most types a species has (chart.types_per_species)
   @param passive_slots  compile_passive_slots() result (a species names up to one passive per slot)
   @param species_fields  compile_species_fields() result (a species' "fields" give their values)
   @return compiled species in key order (each with "fields": {field name: value})
   """
   species = []
   for i, sp in enumerate(entries):
      w = f"{where}: species[{i}] ({sp.get('name')})"
      check_keys(sp, "species", w)
      tl = sp.get("types", [])
      if not isinstance(tl, list) or not 1 <= len(tl) <= types_per_species:
         raise ManifestError(f"{w}.types: 1 to {types_per_species} types (chart.types_per_species)")
      ts = [n["types"](t, f"{w}.types") for t in tl] + [NONE8] * (MAX_TYPES_PER - len(tl))
      base = sp.get("base")
      if not isinstance(base, list) or len(base) != len(stat_names):
         raise ManifestError(f"{w}.base: {len(stat_names)} base stats ({' '.join(stat_names)})")
      base = [integer(v, 1, U16_MAX, f"{w}.base") for v in base]
      ab = sp.get("passives", [])
      slot_names = [sl["name"] for sl in passive_slots]
      if not isinstance(ab, list) or len(ab) > len(passive_slots):
         raise ManifestError(f"{w}.passives: up to {len(passive_slots)}, one per passive slot "
                             f"({', '.join(slot_names)}; structure passive_slots)")
      ab = [NONE8 if a is None else n["passives"](a, f"{w}.passives") for a in ab] + \
           [NONE8] * (len(passive_slots) - len(ab))
      g = sp.get("growth")
      if g not in growth_names:
         raise ManifestError(f"{w}.growth: one of {', '.join(growth_names)}")
      learn = []
      for j, le in enumerate(sp.get("learnset", [])):
         if not isinstance(le, list) or len(le) != PAIR:
            raise ManifestError(f"{w}.learnset[{j}]: [level, move]")
         level, move = le
         learn.append((integer(level, 1, level_max, f"{w}.learnset[{j}]"),
                       n["moves"](move, f"{w}.learnset[{j}]")))
      learn.sort(key=lambda x: x[0])
      for lv in {x[0] for x in learn if x[0] > 1}:
         if sum(1 for x in learn if x[0] == lv) > MAX_LEARN_PER_LEVEL:
            raise ManifestError(f"{w}.learnset: at most {MAX_LEARN_PER_LEVEL} moves at one level ({lv})")
      if not learn:
         raise ManifestError(f"{w}: needs at least one learnset move")
      if len(learn) > MAX_LEARNSET:
         raise ManifestError(f"{w}: at most {MAX_LEARNSET} learnset entries")
      given = sp.get("fields", {})
      if not isinstance(given, dict):
         raise ManifestError(f"{w}.fields: an object {{species field: value}}")
      declared = {f["name"]: f for f in species_fields}
      for name in given:
         if name not in declared:
            raise ManifestError(f"{w}.fields.{name}: not a species field of the structure "
                                f"({', '.join(declared) or 'none declared'}; species_fields)")
      # B1: a field with value names takes a name (its index) or a number
      fields = {name: integer(field_value(given.get(name, f["default"]), f, f"{w}.fields.{name}"), 0, f["max"],
                              f"{w}.fields.{name}")
                for name, f in declared.items()}
      # C6 (review4 finding 23): the moves `teach` may give it (a manifest where any species lists them checks
      # every species: one without the key learns none); None: not listed
      teachable = sp.get("teachable")
      if teachable is not None:
         if not isinstance(teachable, list) or len(set(teachable)) != len(teachable):
            raise ManifestError(f"{w}.teachable: a list of move names (each once)")
         teachable = sorted(n["moves"](m, f"{w}.teachable") for m in teachable)
      key = asset_key(f"species:{sp['name']}", w)
      species.append({"name": sp["name"], "text": enc.text(sp["name"], f"{w}.name"), "key": key,
                      "teachable": teachable,
                      "front": asset_key(sp["front"], f"{w}.front") if sp.get("front") else 0,
                      "back": asset_key(sp["back"], f"{w}.back") if sp.get("back") else 0,
                      "base": base, "types": ts, "passives": ab, "growth": growth_names.index(g),
                      "exp_yield": integer(sp.get("exp_yield", DEFAULT_EXP_YIELD), 0, U16_MAX, f"{w}.exp_yield"),
                      "learnset": learn, "fields": fields})
   species.sort(key=lambda x: x["key"])
   for a, c in zip(species, species[1:]):
      if a["key"] == c["key"]:
         raise ManifestError(f"{where}: species {a['name']!r} and {c['name']!r} have the same key")
   return species


def passive_slot_of(v: Json, passive_slots: Sequence[Mapping[str, Any]], where: str) -> int:
   """!A member's passive slot: its index or its name.
   @param v              the manifest's value
   @param passive_slots  compile_passive_slots() result
   @param where          location
   @return the slot index
   """
   names = [sl["name"] for sl in passive_slots]
   if isinstance(v, str):
      if v not in names:
         raise ManifestError(f"{where}: one of the passive slots {', '.join(names)}, got {v!r}")
      return names.index(v)
   return integer(v, 0, len(names) - 1, where)


def compile_parties(entries: Sequence[Json], species: Sequence[Mapping[str, Any]], n: Lookups,
                    rules: Mapping[str, Any], where: str, enc: Texts, gene_max: int,
                    move_slots: int = DEFAULT_MOVE_SLOTS, party_size: int = PARTY_SIZE,
                    passive_slots: Sequence[Mapping[str, Any]] = DEFAULT_PASSIVE_SLOTS,
                    ai_profiles: Mapping[str, Any] | None = None) -> list[dict[str, Any]]:
   """!Parties with their members.
   @param entries   the manifest's parties
   @param species   compile_species() result (members name species; indices are in key order)
   @param n         lookups
   @param rules     compiled rules (level_max, rank_max)
   @param where     location
   @param enc       this compile's text encoders
   @param gene_max  highest gene the record layout holds (layout.role_max_value of stat_genes)
   @param move_slots  the layout's move slots (most moves a member names)
   @param party_size  the layout's party size (most members)
   @param passive_slots  compile_passive_slots() result (a member's "passive": a slot's index or name)
   @param ai_profiles  battlec.ai.compile_ai_profiles() result (a party's "ai" names one; G1), or None: the
                       default profile only
   @return compiled parties
   """
   profiles = ai_profiles if ai_profiles is not None else compile_ai_profiles(None, where, [])
   level_max = rules["level_max"]
   sp_index = {s["name"]: i for i, s in enumerate(species)}

   def species_ref(name: Json, w: str) -> int:
      """!A species' index in key order."""
      if name not in sp_index:
         raise ManifestError(f"{w}: unknown species {name!r}")
      return sp_index[name]
   parties = []
   for i, td in enumerate(entries):
      w = f"{where}: parties[{i}] ({td.get('name')})"
      check_keys(td, "party", w)
      members = []
      for j, pm in enumerate(td.get("members", [])):
         wj = f"{w}.members[{j}]"
         check_keys(pm, "member", wj)
         mv = pm.get("moves", [])
         if not isinstance(mv, list) or len(mv) > move_slots:
            raise ManifestError(f"{wj}.moves: up to {move_slots} (the layout's move_slots)")
         mv = [n["moves"](x, f"{wj}.moves") for x in mv] + [NONE16] * (MAX_MOVES - len(mv))
         members.append({"species": species_ref(pm.get("species"), wj),
                         "item": n["items"](pm["item"], f"{wj}.item") if pm.get("item") else NONE16,
                         "moves": mv, "level": integer(pm.get("level"), 1, level_max, f"{wj}.level"),
                         "passive": passive_slot_of(pm.get("passive", 0), passive_slots, f"{wj}.passive"),
                         "genes": integer(pm.get("genes", 0), 0, gene_max, f"{wj}.genes"),
                         "trait": n["traits"](pm["trait"], f"{wj}.trait") if pm.get("trait") else 0})

      # a party without members only brings presenters and rules: its participant gives records
      if len(members) > party_size:
         raise ManifestError(f"{w}.members: at most {party_size} creatures (the layout's party_size)")
      # G1: the party's AI profile (none: the default one)
      ai = party_profile(td.get("ai"), profiles, w)
      shown = td.get("presenters", [])
      if not isinstance(shown, list) or len(shown) > MAX_PRESENTERS:
         raise ManifestError(f"{w}.presenters: at most {MAX_PRESENTERS} presenter names")
      if len(set(shown)) != len(shown):
         raise ManifestError(f"{w}.presenters: a presenter is listed twice")
      refs = [n["presenters"](x, f"{w}.presenters") for x in shown] + [NONE8] * (MAX_PRESENTERS - len(shown))
      escape = td.get("escape", True)
      if not isinstance(escape, bool):
         raise ManifestError(f"{w}.escape: true or false")
      # G3: the items the party may use in battle (one use each; its AI profile's item rules choose them)
      used = td.get("items", [])
      if not isinstance(used, list) or len(used) > MAX_PARTY_ITEMS:
         raise ManifestError(f"{w}.items: at most {MAX_PARTY_ITEMS} item names")
      item_ids = [n["items"](x, f"{w}.items") for x in used]
      parties.append({"name": enc.text(td["name"], f"{w}.name"), "members": members, "presenters": refs,
                      "reward": integer(td.get("reward", 0), 0, U16_MAX, f"{w}.reward"),
                      "ai": profiles["names"][ai], "ai_profile": ai,
                      "rank": integer(td.get("rank", DEFAULT_RANK), 0, rules["rank_max"], f"{w}.rank"),
                      "flags": 0 if escape else PARTY_NO_ESCAPE,
                      "items": item_ids + [0] * (MAX_PARTY_ITEMS - len(item_ids)), "item_names": list(used)})
   return parties
