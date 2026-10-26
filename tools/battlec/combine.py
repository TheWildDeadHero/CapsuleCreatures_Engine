"""!Combination methods (D-28 section 1, package B1): structure.json `combine_methods`.

The engine has no idea of breeding, fusion or synthesis: N records go in, ordered result rules pick a species, the
`on combine` effect logic builds the result record and the engine delivers it. Every rule, table, number and name
is the manifest's. A method is {"name", "label", "kind", "inputs", "odds", "fee", "tables", "results", "level",
"moves", "afterwards", "delivery", "allow_eggs", "messages"}:

  - name: a lower-case identifier (effect logic `field.combine_method == NAME`, scripts `combine open NAME`, the ids
    header COMBINE_METHOD_<NAME>); label: what menus show (default the name);
  - kind: "immediate" (a script or menu combines records on the spot; B1) or "nursery" (records left in a nursery,
    the result appears over steps: B2, structure `nursery` runs it; a script or menu cannot combine with it);
  - inputs: {"count": N or [min, max] (2 to the platform's max_combine_inputs), "slots": [{"role": NAME,
    "requires": EXPR}] (one per input, max of them; the role names the slot for the other rules: `NAME.level`,
    and for the logic: `input.NAME`); requires is a growth `when` expression over that input's record (its roles,
    stats, species fields, the field action words; tools/battlec/growth.py WhenScope), compiled as one more growth
    rule with the script trigger};
  - odds (optional): {"value": EXPR}: the method's value through the ruleset pipeline combine_odds (0: the inputs
    are incompatible; an immediate method allows anything above 0; a nursery method's chance per check in B2);
  - fee (optional): {"value": EXPR} through the pipeline combine_fee (a script reads it and charges);
  - tables: {NAME: {"key": a species field (with value names or numbers) or "type" (the species' first type),
    "inputs": [i, j] (the two inputs keyed; default [0, 1]), "symmetric" (default true: a, b matches b, a),
    "cells": [[a, b, value], ...]}}: value is a group (a value of the key);
  - results: rules tried in order, the first that matches wins; each may have "when": EXPR:
      {"recipe": [matcher, ...], "result": SPECIES}: exactly as many inputs as matchers, matched in any order
      (a multiset); a matcher is a species name, {"FIELD": VALUE} or {"type": TYPE};
      {"table": NAME, "pick": EXPR or {"where": EXPR, "order": FIELD or "index", "choose": "first" | "last" |
      "random", "else": "none" | "first" | "last"}}: the table's cell for the inputs is a group; the species of
      the group (key value equal to the cell) for which where holds, the first / last by order or a random one;
      none: else's choice among the whole group, or the rule does not match;
      {"expression": EXPR}: the species EXPR gives (sp.NAME words; 0: no match);
    none matched: the combination is refused (messages.refused);
  - level: EXPR (default 1): the result's level (clamped to 1..level_max);
  - moves: {"sources": [{"kind": MOVE_SOURCES, "limit": N (0: no limit), "on": bool}], "limit": N (default the
    layout's move_slots), "chooser": "ordered" | "random" | "player" (a menu through the script port; random
    without one), "fill": bool (default true: the result's own moves fill the slots left)}; the sources:
    known (the inputs' moves), combine_moves (the result species' combine_moves list known by an input; "all":
    true takes the whole list), shared_level_up (moves of the result's learnset every input knows),
    teachable_known (the result's teachable moves an input knows); C9: a source's "from": ROLE reads that one
    input only, its moves in move slot order (e.g. one parent's egg moves); "push": true (default
    false) keeps the result's own moves first and learns the chosen ones after them in order, each pushing the
    first move out when the slots are full (a known one skipped; the ordered chooser then takes every candidate);
  - afterwards: "kept" or "consumed" (consumed inputs leave the party or boxes after the result is placed);
  - delivery: "immediate" (the party, else the storage boxes) or "egg" (B2: the result is an egg, structure
    `eggs`; tools/battlec/nursery.py);
  - cycle_steps (B2, egg delivery only): the steps of one egg cycle (default 256): the egg's hatch counter starts at
    the result species' hatch cycles field times it;
  - allow_eggs: whether eggs may be inputs (B2: the structure's egg role; default refused);
  - messages: {moment: message key} for MOMENTS (the battle's messages; {user} is the result, or the input).

Expressions (EXPR) are the ruleset language over: per input slot ROLE: ROLE.species (species index + 1; 0 for a
slot left empty), ROLE.base (its base stage: the transformation rules walked backwards, + 1), ROLE.type (its first
type), ROLE.FIELD (a species field), ROLE.R (a single-value record role R: ROLE.level, ROLE.gender, ...; 32-bit
roles as R_hi / R_lo); count, level_sum, level_avg, level_min, level_max, same_species; the transformation's
context values by name (time_of_day, a game's moon phase, ...); words sp.NAME (a species, index + 1), type.NAME,
FIELD.VALUE (a field's value name), the context values' names, the ruleset constants. A pick's where also reads
cand.species / cand.base / cand.type / cand.FIELD (the candidate species); the level expression also
result.species / result.base / result.type / result.FIELD (the result species), a fee those and result.level.

Emitted as capsule::combine::kCombine (include/capsule/combine/data.h) with rules::combine_value() in the
generated battle.cpp; battle_ids.h COMBINE_METHOD_<NAME>. preview(), move_candidates() and combine_record() are the
Python mirrors (the studio's previews and the tests).
"""
from __future__ import annotations

import re
from collections.abc import Mapping, Sequence
from typing import Any

import ruleset_compiler as rc

from .common import NONE8, NONE16, PAIR, Json, ManifestError, check_keys, ident, integer, register_keys
from .field import LEAD_TRIGGER
from .growth import WhenScope, compile_rule
from .nursery import DEFAULT_CYCLE_STEPS, MAX_CYCLE_STEPS, egg_initializer, nursery_initializer

## Key of the methods in structure.json.
COMBINE_KEY = "combine_methods"
## Method kinds in capsule::combine::MethodKind order.
KINDS = ["nursery", "immediate"]
## The kind that runs end to end in B1.
IMMEDIATE = "immediate"
## Deliveries in capsule::combine::Delivery order.
DELIVERIES = ["immediate", "egg"]
## What happens to the inputs, in capsule::combine::Afterwards order.
AFTERWARDS = ["kept", "consumed"]
## Move choosers in capsule::combine::Chooser order.
CHOOSERS = ["ordered", "random", "player"]
## Move sources in capsule::combine::MoveSource order.
MOVE_SOURCES = ["known", "combine_moves", "shared_level_up", "teachable_known"]
## Result rule kinds in capsule::combine::RuleKind order.
RULE_KINDS = ["recipe", "table", "expression"]
## A pick's choices in capsule::combine::PickChoose order.
PICK_CHOICES = ["first", "last", "random"]
## A pick's fallbacks in capsule::combine::PickElse order.
PICK_ELSE = ["none", "first", "last"]
## A pick's order by the species index.
ORDER_INDEX = "index"
## A table key (and a matcher) of the species' first type.
TYPE_KEY = "type"
## Matcher kinds in capsule::combine::MatcherKind order.
MATCHER_KINDS = ["species", "field", "type"]
## Key kinds of a table in capsule::combine::KeyKind order.
KEY_KINDS = ["field", "type"]
## The moments a method's messages name (capsule::combine::Moment order).
MOMENTS = ["unfit", "incompatible", "refused", "no_room", "combined"]
## The aggregates of the inputs an expression reads (capsule::combine::Aggregate order).
AGGREGATES = ["level_sum", "level_avg", "level_min", "level_max", "same_species"]
## The input count's name in expressions.
COUNT_INPUT = "count"
## Parts of an input besides its roles (capsule::combine::Part order: species, base, type, then a field).
INPUT_PARTS = ["species", "base", "type"]
## The prefix of a pick's candidate.
CANDIDATE = "cand"
## The prefix of a fee's result.
RESULT = "result"
## The result's parts a fee reads.
RESULT_PARTS = ["species", "level"]
## Prefix of a species word (its index + 1).
SPECIES_WORD = "sp."
## Prefix of a type word (its index).
TYPE_WORD = "type."
## Default of the result's level.
DEFAULT_LEVEL = "1"
## Most methods (capsule::combine::kMaxMethods; ruleset_compiler MAX_COMBINE_METHODS).
MAX_METHODS = rc.MAX_COMBINE_METHODS
## Fewest inputs of a method.
MIN_INPUTS = PAIR
## Most result rules of one method.
MAX_RULES = 32
## Most tables of one method.
MAX_TABLES = 8
## Most cells of one table.
MAX_CELLS = 1024
## Most move sources of one method (capsule::combine::kMaxMoveSources).
MAX_SOURCES = 4
## Most moves the sources gather (capsule::combine::kMaxCandidates).
MAX_CANDIDATES = 16
## Most combine moves of one species.
MAX_COMBINE_MOVES = 32
## Most combination expressions in all (16-bit ids, kNoExpr = none).
MAX_EXPRESSIONS = NONE16 - 1
## Longest label (letters).
MAX_LABEL_CHARS = 16
## A method's (and a slot role's) name.
NAME = re.compile(r"[a-z][a-z0-9_]*\Z")
## Names a slot role may not take (the aggregates and prefixes of the expressions).
RESERVED_ROLES = set(AGGREGATES) | {COUNT_INPUT, CANDIDATE, RESULT, "sp", "type"}
## MethodData::flags: the result's own moves fill the slots the sources leave.
FLAG_FILL = 1
## MethodData::flags: eggs may be inputs (B2's seam).
FLAG_ALLOW_EGGS = 2
## MethodData::flags: the result's own moves first, the chosen ones pushed after them (C9).
FLAG_PUSH = 4
## A move source's key naming the one slot role it reads (C9).
FROM_KEY = "from"
## A method's moves key: push the chosen moves after the result's own ones (C9).
PUSH_KEY = "push"
## MoveSourceData::from of a source reading every input (capsule::combine::kAnyInput).
ANY_INPUT = NONE8
## MoveSourceData::flags: the source is on.
SOURCE_ON = 1
## MoveSourceData::flags: combine_moves takes the whole list (not only the moves an input knows).
SOURCE_ALL = 2
## A rule, slot or method part without an expression (capsule::combine::kNoExpr).
NO_EXPR = NONE16
## The highest value the odds and fee expressions pass their pipelines (the engine clamps).
VALUE_MAX = rc.COMBINE_VALUE_MAX
## The battler of the first input in the `on combine` field context (capsule::battle::kFieldOther).
FIRST_INPUT_BATTLER = 1
## The preview statuses (capsule::combine::Status order).
STATUSES = ["ok", "wrong_count", "unfit", "incompatible", "refused", "not_immediate", "unsupported", "no_room",
            "bad_input"]

register_keys({
   "combine_method": {"name", "label", "kind", "inputs", "odds", "fee", "tables", "results", "level", "moves",
                      "afterwards", "delivery", "allow_eggs", "messages", "cycle_steps"},
   "combine_inputs": {"count", "slots"},
   "combine_slot": {"role", "requires"},
   "combine_value": {"value"},
   "combine_table": {"key", "inputs", "symmetric", "cells"},
   "combine_rule": {"recipe", "result", "table", "pick", "expression", "when"},
   "combine_pick": {"where", "order", "choose", "else"},
   "combine_moves": {"sources", "limit", "chooser", "fill", "push"},
   "combine_source": {"kind", "limit", "on", "all", "from"},
   "combine_messages": set(MOMENTS),
})


# -- the methods' names (the effect logic's and the script compiler's view) ----------------------------------------

def method_records(src: Json, max_inputs: int, where: str) -> list[dict[str, Any]]:
   """!The methods' names, kinds, labels, input counts and slot roles, without their rules (what effect logic and the
   script compiler read before the battle compiles the rest).
   @param src         structure.json combine_methods (None: none)
   @param max_inputs  the platform's max_combine_inputs
   @param where       location (the structure manifest)
   @return [{"name", "label", "kind", "min", "max", "roles", "slots" (the slot dicts)}]
   """
   if src is None:
      return []
   if not isinstance(src, list) or len(src) > MAX_METHODS:
      raise ManifestError(f"{where}: {COMBINE_KEY} must be a list of at most {MAX_METHODS} methods")
   out = []
   seen: set[str] = set()
   for i, m in enumerate(src):
      w = f"{where}: {COMBINE_KEY}[{i}]"
      check_keys(m, "combine_method", w)
      name = m.get("name")
      if not isinstance(name, str) or not NAME.fullmatch(name) or name in seen:
         raise ManifestError(f"{w}: needs a name of its own (a lower-case identifier)")
      seen.add(name)
      label = m.get("label", name)
      if not isinstance(label, str) or not 1 <= len(label) <= MAX_LABEL_CHARS:
         raise ManifestError(f"{w}.label: 1 to {MAX_LABEL_CHARS} letters")
      kind = m.get("kind", IMMEDIATE)
      if kind not in KINDS:
         raise ManifestError(f"{w}.kind: one of {', '.join(KINDS)}")
      inputs = m.get("inputs")
      check_keys(inputs, "combine_inputs", f"{w}.inputs")
      count = inputs.get("count", MIN_INPUTS)
      lo, hi = (count, count) if isinstance(count, int) and not isinstance(count, bool) else \
         tuple(count) if isinstance(count, list) and len(count) == PAIR else (None, None)
      if not (isinstance(lo, int) and isinstance(hi, int)) or not MIN_INPUTS <= lo <= hi <= max_inputs:
         raise ManifestError(f"{w}.inputs.count: N or [min, max], {MIN_INPUTS} to {max_inputs} (the platform's "
                             f"max_combine_inputs)")
      slots = inputs.get("slots")
      if not isinstance(slots, list) or len(slots) != hi:
         raise ManifestError(f"{w}.inputs.slots: one slot per input ({hi})")
      roles = []
      for k, sl in enumerate(slots):
         sw = f"{w}.inputs.slots[{k}]"
         check_keys(sl, "combine_slot", sw)
         role = sl.get("role")
         if not isinstance(role, str) or not NAME.fullmatch(role) or role in roles or role in RESERVED_ROLES:
            raise ManifestError(f"{sw}.role: a name of its own (a lower-case identifier; not "
                                f"{', '.join(sorted(RESERVED_ROLES))})")
         req = sl.get("requires")
         if req is not None and (not isinstance(req, str) or not req.strip()):
            raise ManifestError(f"{sw}.requires: an expression over the input's record (or leave it out)")
         roles.append(role)
      out.append({"name": name, "label": label, "kind": kind, "min": lo, "max": hi, "roles": roles,
                  "slots": slots, "source": m})
   return out


def slot_roles(methods: Sequence[Mapping[str, Any]]) -> dict[str, int | None]:
   """!The slot roles effect logic may name as `input.ROLE`: each role's input index (None when two methods give one
   name different indices: the logic names it by index then).
   @param methods  method_records() result
   @return role -> index or None
   """
   out: dict[str, int | None] = {}
   for m in methods:
      for k, role in enumerate(m["roles"]):
         if role in out and out[role] != k:
            out[role] = None
         else:
            out.setdefault(role, k)
   return out


def qualified_roles(methods: Sequence[Mapping[str, Any]]) -> dict[str, int]:
   """!C9: the slot roles effect logic may name with their method, `input.METHOD.ROLE` (unambiguous when methods
   share a role name: a rename in one method follows only its own qualified reads).
   @param methods  method_records() result
   @return "METHOD.ROLE" -> input index
   """
   return {f"{m['name']}.{role}": k for m in methods for k, role in enumerate(m["roles"])}


# -- the expression scope ---------------------------------------------------------------------------------------

class CombineScope:
   """!What one method's expressions read: the inputs by slot role, the aggregates, the context values and the words
   (the base scope; pick() and fee() add the candidate and the result)."""

   def __init__(self, roles: Sequence[str], lo: int, hi: int, when: WhenScope, species: Sequence[Mapping[str, Any]],
                species_fields: Sequence[Mapping[str, Any]], types: Sequence[str],
                contexts: Sequence[Mapping[str, Any]], level_max: int, where: str) -> None:
      """!Builds the base scope.
      @param roles           the slot roles (input order)
      @param lo              fewest inputs
      @param hi              most inputs
      @param when            the growth rules' WhenScope (the record roles, their accessors, the words)
      @param species         compiled species (key order)
      @param species_fields  compiled species fields
      @param types           the types' names
      @param contexts        the transformation's context values
      @param level_max       highest level
      @param where           location
      """
      ## Input name -> (lo, hi).
      self.ranges: dict[str, tuple[int, int]] = {}
      ## Input name -> accessor: ("role", slot, rid, elem, shift, mask), ("part", slot, part), ("field", slot, k),
      ## ("count",), ("agg", k), ("context", k), ("cand", part or "field", k), ("result", part).
      self.access: dict[str, tuple[Any, ...]] = {}
      ## Words.
      self.constants: dict[str, int] = dict(when.constants)
      ## Where errors point.
      self.where = where
      n = len(species)
      for k, role in enumerate(roles):
         for rname, parts in when.roles.items():
            for nm, part in parts:
               if part is not None and part not in ("hi", "lo"):
                  continue   # per-stat roles have a dot of their own: the logic reads them
               _kind, *found = when.access[nm]   # the role's id, element, shift and mask
               self._add(f"{role}.{nm}", when.ranges[nm], ("role", k, *found))
         for p, rng in zip(INPUT_PARTS, [(0, n), (0, n), (0, max(len(types) - 1, 0))]):
            self._add(f"{role}.{p}", rng, ("part", k, p))
         for fi, f in enumerate(species_fields):
            self._add(f"{role}.{f['name']}", (0, f["max"]), ("field", k, fi))
      self._add(COUNT_INPUT, (lo, hi), ("count",))
      levels = (0, level_max)
      for k, a in enumerate(AGGREGATES):
         rng = (0, level_max * hi) if a == "level_sum" else (0, 1) if a == "same_species" else levels
         self._add(a, rng, ("agg", k))
      for k, cv in enumerate(contexts):
         self._add(cv["name"], cv["range"], ("context", k))
      # the words: species, types, field value names
      for i, sp in enumerate(species):
         self._word(SPECIES_WORD + ident(sp["name"]).lower(), i + 1)
      for i, t in enumerate(types):
         self._word(TYPE_WORD + ident(t).lower(), i)
      for f in species_fields:
         for i, v in enumerate(f.get("values") or []):
            self._word(f"{f['name']}.{v}", i)
      ## The candidate's inputs (a pick's where).
      self.cand: dict[str, tuple[tuple[int, int], tuple[Any, ...]]] = {}
      for p, rng in zip(INPUT_PARTS, [(1, n), (1, n), (0, max(len(types) - 1, 0))]):
         self.cand[f"{CANDIDATE}.{p}"] = (rng, ("cand", p, 0))
      for fi, f in enumerate(species_fields):
         self.cand[f"{CANDIDATE}.{f['name']}"] = ((0, f["max"]), ("cand", "field", fi))
      ## The result's inputs (the level expression: its species, base stage, type and fields; a fee: its level too).
      self.result: dict[str, tuple[tuple[int, int], tuple[Any, ...]]] = {}
      for p, rng in zip(INPUT_PARTS, [(1, n), (1, n), (0, max(len(types) - 1, 0))]):
         self.result[f"{RESULT}.{p}"] = (rng, ("result", p, 0))
      for fi, f in enumerate(species_fields):
         self.result[f"{RESULT}.{f['name']}"] = ((0, f["max"]), ("result", "field", fi))
      ## The result's level (a fee only).
      self.result_level = {f"{RESULT}.level": ((1, level_max), ("result_level",))}

   def _add(self, name: str, rng: tuple[int, int], access: tuple[Any, ...]) -> None:
      """!Adds one input (a second one of a name is an error: a role and a field of one name).
      @param name    input name
      @param rng     its range
      @param access  its accessor
      @return None
      """
      if name in self.access:
         raise ManifestError(f"{self.where}: {name!r} names two inputs (a record role and a species field, or a "
                             f"part); rename one")
      self.ranges[name] = rng
      self.access[name] = access

   def _word(self, word: str, value: int) -> None:
      """!Adds one word (an input of that name keeps it; another value for a word is an error).
      @param word   the word
      @param value  its value
      @return None
      """
      if word in self.ranges:
         return
      if word in self.constants and self.constants[word] != value:
         raise ManifestError(f"{self.where}: the word {word!r} names two values; rename one")
      self.constants[word] = value

   def view(self, kind: str) -> tuple[dict[str, tuple[int, int]], dict[str, tuple[Any, ...]]]:
      """!The inputs of one kind of expression.
      @param kind  "base", "pick" (with the candidate), "level" (with the result) or "fee" (with the result and its
                   level)
      @return (ranges, accessors)
      """
      ranges, access = dict(self.ranges), dict(self.access)
      extra = self.cand if kind == "pick" else {**self.result, **self.result_level} if kind == "fee" else \
         self.result if kind == "level" else {}
      for nm, (rng, acc) in extra.items():
         ranges[nm] = rng
         access[nm] = acc
      return ranges, access


def cpp_access(acc: tuple[Any, ...]) -> str:
   """!The C++ of one input (inside namespace capsule::rules: `in` is combine::ValueInputs).
   @param acc  CombineScope accessor
   @return the expression
   """
   k = acc[0]
   if k == "role":
      _k, slot, rid, elem, shift, mask = acc
      get = f"combine::input_role(in, {slot}, monster::RoleId{{0x{rid:02X}}}, {elem})"
      return f"static_cast<int32_t>(({get} >> {shift}) & 0x{mask:X}u)" if mask else f"static_cast<int32_t>({get})"
   if k == "part":
      _k, slot, part = acc
      return f"combine::input_part(in, {slot}, combine::Part::{part.capitalize()})"
   if k == "field":
      _k, slot, field = acc
      return f"combine::input_field(in, {slot}, {field})"
   if k == "count":
      return "static_cast<int32_t>(in.count)"
   if k in ("agg", "context"):
      _k, index = acc
      return f"combine::aggregate(in, combine::Aggregate::{camel(AGGREGATES[index])})" if k == "agg" else \
         f"in.contexts[{index}]"
   if k in ("cand", "result"):
      _k, part, field = acc
      fn = "candidate_part" if k == "cand" else "result_part"
      return f"combine::{fn}(in, combine::Part::{'Field' if part == 'field' else part.capitalize()}, {field})"
   return "static_cast<int32_t>(in.result_level)"


def camel(name: str) -> str:
   """!CamelCase of a snake_case name.
   @param name  e.g. level_avg
   @return e.g. LevelAvg
   """
   return "".join(p.capitalize() for p in name.split("_"))


def compile_expr(text: Json, scope: CombineScope, kind: str, exprs: list[dict[str, Any]], owner: str,
                 where: str) -> int:
   """!One combination expression, appended to the expression list.
   @param text   the expression
   @param scope  the method's CombineScope
   @param kind   "base", "pick" or "fee"
   @param exprs  the expression list (appended to; an expression's id is its index)
   @param owner  what it belongs to (the C++ comment)
   @param where  location
   @return its id
   @throws ManifestError when it does not parse or can leave int32
   """
   ranges, access = scope.view(kind)
   if not isinstance(text, str) or not text.strip():
      raise ManifestError(f"{where}: an expression (the ruleset language)")
   try:
      tree = rc.parse(text, list(ranges), scope.constants, where)
      rng = rc.interval(tree, ranges, scope.constants, where, [])
   except rc.RulesetError as e:
      raise ManifestError(str(e))
   used = sorted(rc.used_inputs(tree))
   exprs.append({"tree": tree, "expr": text, "used": used, "range": rng, "owner": owner,
                 "names": {u: cpp_access(access[u]) for u in used}, "access": {u: access[u] for u in used},
                 "constants": scope.constants})
   if len(exprs) > MAX_EXPRESSIONS:
      raise ManifestError(f"{where}: more than {MAX_EXPRESSIONS} combination expressions")
   return len(exprs) - 1


# -- the sections ------------------------------------------------------------------------------------------------

def message_ids(src: Json, messages: Sequence[str | None], where: str) -> list[int]:
   """!A method's message keys at their ids.
   @param src       its messages ({moment: key} or None)
   @param messages  the battle's messages at their ids
   @param where     location
   @return MOMENTS order, NONE8 for none
   """
   src = {} if src is None else src
   check_keys(src, "combine_messages", where)
   known = [m for m in messages if m is not None]
   out = []
   for moment in MOMENTS:
      key = src.get(moment)
      if key is None:
         out.append(NONE8)
         continue
      if not isinstance(key, str) or key not in known:
         raise ManifestError(f"{where}.{moment}: {key!r} is not one of the battle's messages")
      out.append(list(messages).index(key))
   return out


def group_value(v: Json, key: str, fields: Mapping[str, Mapping[str, Any]], types: Sequence[str],
                where: str) -> int:
   """!A table key's value (a field value name or number, or a type name).
   @param v       the manifest's value
   @param key     the table's key (a field name or TYPE_KEY)
   @param fields  species fields by name
   @param types   the types' names
   @param where   location
   @return the number
   """
   if key == TYPE_KEY:
      if v not in types:
         raise ManifestError(f"{where}: {v!r} is not a type ({', '.join(types)})")
      return list(types).index(v)
   f = fields[key]
   if isinstance(v, str):
      if v not in (f.get("values") or []):
         raise ManifestError(f"{where}: {v!r} is not a value of {key!r} ({', '.join(f.get('values') or []) or 'none'})")
      return f["values"].index(v)
   return integer(v, 0, f["max"], where)


def compile_tables(src: Json, roles: Sequence[str], fields: Mapping[str, Mapping[str, Any]], types: Sequence[str],
                   where: str) -> dict[str, dict[str, Any]]:
   """!A method's tables.
   @param src     its tables ({name: table} or None)
   @param roles   its slot roles
   @param fields  species fields by name
   @param types   the types' names
   @param where   location
   @return name -> {"key", "key_kind", "field", "inputs", "symmetric", "cells" [(a, b, value)]}
   """
   src = {} if src is None else src
   if not isinstance(src, dict) or len(src) > MAX_TABLES:
      raise ManifestError(f"{where}: {{name: table}}, at most {MAX_TABLES}")
   out = {}
   for name, t in src.items():
      w = f"{where}.{name}"
      if not NAME.fullmatch(str(name)):
         raise ManifestError(f"{w}: a table's name is a lower-case identifier")
      check_keys(t, "combine_table", w)
      key = t.get("key")
      if key != TYPE_KEY and key not in fields:
         raise ManifestError(f"{w}.key: a species field ({', '.join(fields) or 'none declared'}) or {TYPE_KEY!r}")
      inputs = t.get("inputs", [0, 1])
      if not (isinstance(inputs, list) and len(inputs) == PAIR and all(isinstance(i, int) and 0 <= i < len(roles)
                                                                        for i in inputs) and inputs[0] != inputs[1]):
         raise ManifestError(f"{w}.inputs: two different input slots (0 to {len(roles) - 1})")
      sym = t.get("symmetric", True)
      if not isinstance(sym, bool):
         raise ManifestError(f"{w}.symmetric: true or false")
      cells = t.get("cells", [])
      if not isinstance(cells, list) or len(cells) > MAX_CELLS:
         raise ManifestError(f"{w}.cells: a list of [a, b, value], at most {MAX_CELLS}")
      seen = set()
      rows = []
      for j, cell in enumerate(cells):
         cw = f"{w}.cells[{j}]"
         if not isinstance(cell, list) or len(cell) != PAIR + 1:
            raise ManifestError(f"{cw}: [a, b, value]")
         a, b, v = (group_value(x, key, fields, types, cw) for x in cell)
         pair = (min(a, b), max(a, b)) if sym else (a, b)
         if pair in seen:
            raise ManifestError(f"{cw}: the pair {cell[0]!r}, {cell[1]!r} is given twice")
         seen.add(pair)
         rows.append((a, b, v))
      out[name] = {"key": key, "key_kind": KEY_KINDS.index("type" if key == TYPE_KEY else "field"),
                   "field": NONE8 if key == TYPE_KEY else list(fields).index(key), "inputs": list(inputs),
                   "symmetric": sym, "cells": rows, "names": [list(c) for c in cells]}
   return out


def matcher(v: Json, species_index: Mapping[str, int], fields: Mapping[str, Mapping[str, Any]],
            types: Sequence[str], where: str) -> tuple[int, int, int]:
   """!A recipe matcher.
   @param v              a species name, {FIELD: VALUE} or {"type": TYPE}
   @param species_index  species name -> index
   @param fields         species fields by name
   @param types          the types' names
   @param where          location
   @return (MATCHER_KINDS index, field index or NONE8, value)
   """
   if isinstance(v, str):
      if v not in species_index:
         raise ManifestError(f"{where}: {v!r} is not a species")
      return (0, NONE8, species_index[v])
   if not isinstance(v, dict) or len(v) != 1:
      raise ManifestError(f"{where}: a species name, {{FIELD: VALUE}} or {{\"type\": TYPE}}")
   key, value = next(iter(v.items()))
   if key == TYPE_KEY:
      return (MATCHER_KINDS.index("type"), NONE8, group_value(value, key, fields, types, where))
   if key not in fields:
      raise ManifestError(f"{where}: {key!r} is not a species field ({', '.join(fields) or 'none declared'})")
   return (MATCHER_KINDS.index("field"), list(fields).index(key), group_value(value, key, fields, types, where))


def compile_pick(src: Json, scope: CombineScope, fields: Mapping[str, Mapping[str, Any]],
                 exprs: list[dict[str, Any]], owner: str, where: str) -> dict[str, Any]:
   """!A table rule's pick.
   @param src     the pick (an expression or {where, order, choose, else})
   @param scope   the method's scope
   @param fields  species fields by name
   @param exprs   the expression list
   @param owner   the C++ comment
   @param where   location
   @return {"where" (expr id or NO_EXPR), "order" (field index or NONE8), "choose", "else"}
   """
   if src is None:
      src = {}
   if isinstance(src, str):
      src = {"where": src}
   check_keys(src, "combine_pick", where)
   cond = src.get("where")
   wid = NO_EXPR if cond is None else compile_expr(cond, scope, "pick", exprs, owner, f"{where}.where")
   order = src.get("order", ORDER_INDEX)
   if order != ORDER_INDEX and order not in fields:
      raise ManifestError(f"{where}.order: {ORDER_INDEX!r} or a species field ({', '.join(fields) or 'none'})")
   choose = src.get("choose", PICK_CHOICES[0])
   if choose not in PICK_CHOICES:
      raise ManifestError(f"{where}.choose: one of {', '.join(PICK_CHOICES)}")
   other = src.get("else", PICK_ELSE[0])
   if other not in PICK_ELSE:
      raise ManifestError(f"{where}.else: one of {', '.join(PICK_ELSE)}")
   return {"where": wid, "order": NONE8 if order == ORDER_INDEX else list(fields).index(order),
           "order_name": order, "choose": choose, "else": other}


def compile_moves(src: Json, move_slots: int, where: str, roles: Sequence[str] = ()) -> dict[str, Any]:
   """!A method's move sources.
   @param src         its moves section (None: the result keeps its own moves)
   @param move_slots  the layout's move slots
   @param where       location
   @param roles       the method's slot roles (C9: a source's `from`)
   @return {"sources" [{"kind", "limit", "on", "all", "from" (a role or None), "from_slot"}], "limit", "chooser",
           "fill", "push"}
   """
   src = {} if src is None else src
   check_keys(src, "combine_moves", where)
   sources = src.get("sources", [])
   if not isinstance(sources, list) or len(sources) > MAX_SOURCES:
      raise ManifestError(f"{where}.sources: a list of at most {MAX_SOURCES} sources")
   out = []
   kinds = set()
   for j, s in enumerate(sources):
      sw = f"{where}.sources[{j}]"
      check_keys(s, "combine_source", sw)
      kind = s.get("kind")
      if kind not in MOVE_SOURCES or kind in kinds:
         raise ManifestError(f"{sw}.kind: one of {', '.join(MOVE_SOURCES)} (each once)")
      kinds.add(kind)
      limit = integer(s.get("limit", 0), 0, MAX_CANDIDATES, f"{sw}.limit")
      on = s.get("on", True)
      every = s.get("all", False)
      if not isinstance(on, bool) or not isinstance(every, bool):
         raise ManifestError(f"{sw}: on / all are true or false")
      if every and kind != "combine_moves":
         raise ManifestError(f"{sw}.all: only combine_moves takes the whole list")
      # C9: one input's moves only (its slot role), in its move slot order
      role = s.get(FROM_KEY)
      if role is not None and role not in roles:
         raise ManifestError(f"{sw}.{FROM_KEY}: a slot role of the method ({', '.join(roles) or 'none'})")
      if role is not None and every:
         raise ManifestError(f"{sw}: all takes the whole list; {FROM_KEY} reads one input's moves (not both)")
      out.append({"kind": kind, "limit": limit, "on": on, "all": every, "from": role,
                  "from_slot": list(roles).index(role) if role is not None else ANY_INPUT})
   limit = integer(src.get("limit", move_slots), 1, move_slots, f"{where}.limit")
   chooser = src.get("chooser", CHOOSERS[0])
   if chooser not in CHOOSERS:
      raise ManifestError(f"{where}.chooser: one of {', '.join(CHOOSERS)}")
   fill = src.get("fill", True)
   push = src.get(PUSH_KEY, False)
   if not isinstance(fill, bool) or not isinstance(push, bool):
      raise ManifestError(f"{where}: fill / {PUSH_KEY} are true or false")
   return {"sources": out, "limit": limit, "chooser": chooser, "fill": fill, "push": push}


def combine_moves_of(entries: Sequence[Json], species: Sequence[Mapping[str, Any]], moves: Sequence[str],
                     where: str) -> list[list[int]]:
   """!Each species' combine_moves (battle.json species[].combine_moves), in the compiled species' order.
   @param entries  the manifest's species
   @param species  compiled species (key order)
   @param moves    the moves' names
   @param where    location
   @return move indices per species
   """
   raw = {e.get("name"): (k, e) for k, e in enumerate(entries)}
   out = []
   for sp in species:
      k, e = raw[sp["name"]]
      lst = e.get("combine_moves", [])
      w = f"{where}: species[{k}] ({sp['name']}).combine_moves"
      if not isinstance(lst, list) or len(lst) > MAX_COMBINE_MOVES or len(set(map(str, lst))) != len(lst):
         raise ManifestError(f"{w}: a list of move names (each once), at most {MAX_COMBINE_MOVES}")
      for m in lst:
         if m not in moves:
            raise ManifestError(f"{w}: {m!r} is not a move")
      out.append([list(moves).index(m) for m in lst])
   return out


def base_stages(transform: Mapping[str, Any], count: int) -> list[int]:
   """!Each species' base stage: the transformation rules walked backwards (the first species, by index, that
   transforms into it, until none does).
   @param transform  compile_growth_rules() result
   @param count      species count
   @return species index per species
   """
   parent = [NONE16] * count
   for si, sg in enumerate(transform["species"]):
      for t in sg["transforms"]:
         if parent[t["into"]] == NONE16 and t["into"] != si:
            parent[t["into"]] = si
   out = []
   for si in range(count):
      cur, steps = si, 0
      while parent[cur] != NONE16 and steps < count:
         cur, steps = parent[cur], steps + 1
      out.append(cur)
   return out


def compile_combine(b: Mapping[str, Any], layout: Mapping[str, Any], species: Sequence[Mapping[str, Any]],
                    species_fields: Sequence[Mapping[str, Any]], names: Mapping[str, Any],
                    transform: Mapping[str, Any], rules: Mapping[str, Any], limits: Mapping[str, int],
                    messages: Sequence[str | None], enc: Any, swhere: str, where: str) -> dict[str, Any]:
   """!The structure's combination methods with their rules (the slots' requirements appended to the growth rules).
   @param b               the merged battle manifest
   @param layout          compiled layout
   @param species         compiled species (key order)
   @param species_fields  compiled species fields
   @param names           the battle's names (types, moves)
   @param transform       compile_growth_rules() result (its scope and rule list; the field actions' words added)
   @param rules           compiled rules (level_max)
   @param limits          the platform's limits (max_combine_inputs)
   @param messages        the battle's messages at their ids
   @param enc             this compile's text encoders (the labels)
   @param swhere          location of the structure manifest
   @param where           location of battle.json
   @return {"methods", "exprs", "combine_moves" (per species), "base" (per species), "slot_roles"}
   """
   records = method_records(b.get(COMBINE_KEY), limits["max_combine_inputs"], swhere)
   entries = b.get("species", [])
   cmoves = combine_moves_of(entries, species, names["moves"], where)
   if not records:
      return {"methods": [], "exprs": [], "combine_moves": cmoves, "base": [], "slot_roles": {}}
   when = transform["scope"]
   contexts = transform["transformation"]["contexts"]
   fields = {f["name"]: f for f in species_fields}
   sindex = {sp["name"]: i for i, sp in enumerate(species)}
   types = names["types"]
   level_max = rules["level_max"]
   exprs: list[dict[str, Any]] = []
   methods = []
   for i, rec in enumerate(records):
      m = rec["source"]
      w = f"{swhere}: {COMBINE_KEY}[{i}] ({rec['name']})"
      scope = CombineScope(rec["roles"], rec["min"], rec["max"], when, species, species_fields, types, contexts,
                           level_max, w)
      slots = []
      for k, sl in enumerate(rec["slots"]):
         req = sl.get("requires")
         if req is None:
            slots.append(NO_EXPR)
            continue
         rule = compile_rule(req, [LEAD_TRIGGER], when, f"{w}.inputs.slots[{k}].requires")
         rule["owner"] = f"{COMBINE_KEY}:{rec['name']}.{rec['roles'][k]}"
         transform["rules"].append(rule)
         slots.append(len(transform["rules"]) - 1)
      odds = fee = NO_EXPR
      if m.get("odds") is not None:
         check_keys(m["odds"], "combine_value", f"{w}.odds")
         odds = compile_expr(m["odds"].get("value"), scope, "base", exprs, f"{rec['name']} odds", f"{w}.odds.value")
      if m.get("fee") is not None:
         check_keys(m["fee"], "combine_value", f"{w}.fee")
         fee = compile_expr(m["fee"].get("value"), scope, "fee", exprs, f"{rec['name']} fee", f"{w}.fee.value")
      level = compile_expr(m.get("level", DEFAULT_LEVEL), scope, "level", exprs, f"{rec['name']} level",
                           f"{w}.level")
      tables = compile_tables(m.get("tables"), rec["roles"], fields, types, f"{w}.tables")
      results = m.get("results", [])
      if not isinstance(results, list) or not results or len(results) > MAX_RULES:
         raise ManifestError(f"{w}.results: 1 to {MAX_RULES} result rules")
      rules_out = []
      for j, r in enumerate(results):
         rw = f"{w}.results[{j}]"
         check_keys(r, "combine_rule", rw)
         kinds = [k for k in RULE_KINDS if k in r]
         if len(kinds) != 1:
            raise ManifestError(f"{rw}: one of recipe (with result), table (with pick) or expression")
         kind = kinds[0]
         owner = f"{rec['name']} rule {j}"
         rule: dict[str, Any] = {"kind": kind, "when": NO_EXPR, "expr": NO_EXPR, "matchers": [], "table": None,
                                 "result": NONE16, "pick": None}
         if "when" in r:
            rule["when"] = compile_expr(r["when"], scope, "base", exprs, f"{owner} when", f"{rw}.when")
         if kind == "recipe":
            lst = r["recipe"]
            if not isinstance(lst, list) or not rec["min"] <= len(lst) <= rec["max"]:
               raise ManifestError(f"{rw}.recipe: {rec['min']} to {rec['max']} matchers (one per input)")
            rule["matchers"] = [matcher(x, sindex, fields, types, f"{rw}.recipe[{q}]") for q, x in enumerate(lst)]
            res = r.get("result")
            if res not in sindex:
               raise ManifestError(f"{rw}.result: a species")
            rule["result"] = sindex[res]
            rule["result_name"] = res
         elif kind == "table":
            if r["table"] not in tables:
               raise ManifestError(f"{rw}.table: one of the method's tables ({', '.join(tables) or 'none'})")
            rule["table"] = list(tables).index(r["table"])
            rule["table_name"] = r["table"]
            rule["pick"] = compile_pick(r.get("pick"), scope, fields, exprs, f"{owner} pick", f"{rw}.pick")
         else:
            rule["expr"] = compile_expr(r["expression"], scope, "base", exprs, f"{owner} expression",
                                        f"{rw}.expression")
         if kind != "recipe" and "result" in r:
            raise ManifestError(f"{rw}.result: only a recipe names its result")
         if kind != "table" and "pick" in r:
            raise ManifestError(f"{rw}.pick: only a table rule picks")
         rules_out.append(rule)
      after = m.get("afterwards", AFTERWARDS[0])
      if after not in AFTERWARDS:
         raise ManifestError(f"{w}.afterwards: one of {', '.join(AFTERWARDS)}")
      delivery = m.get("delivery", DELIVERIES[0])
      if delivery not in DELIVERIES:
         raise ManifestError(f"{w}.delivery: one of {', '.join(DELIVERIES)}")
      eggs = m.get("allow_eggs", False)
      if not isinstance(eggs, bool):
         raise ManifestError(f"{w}.allow_eggs: true or false")
      # B2: an egg delivery's cycle length (the hatch counter starts at the species' cycles times it)
      if "cycle_steps" in m and delivery != "egg":
         raise ManifestError(f"{w}.cycle_steps: only a method delivering eggs counts egg cycles")
      cycle = integer(m.get("cycle_steps", DEFAULT_CYCLE_STEPS), 1, MAX_CYCLE_STEPS, f"{w}.cycle_steps")
      moves = compile_moves(m.get("moves"), layout["move_slots"], f"{w}.moves", rec["roles"])
      flags = (FLAG_FILL if moves["fill"] else 0) | (FLAG_ALLOW_EGGS if eggs else 0) | \
         (FLAG_PUSH if moves["push"] else 0)
      methods.append({**{k: rec[k] for k in ("name", "label", "kind", "min", "max", "roles")},
                      "label_text": enc.text(rec["label"], f"{w}.label"),
                      "slots": slots, "requires": [sl.get("requires") for sl in rec["slots"]], "odds": odds,
                      "fee": fee, "level": level, "tables": tables, "rules": rules_out, "afterwards": after,
                      "delivery": delivery, "allow_eggs": eggs, "moves": moves, "flags": flags,
                      "cycle_steps": cycle if delivery == "egg" else 0,
                      "messages": message_ids(m.get("messages"), messages, f"{w}.messages"),
                      "message_keys": dict(m.get("messages") or {})})
   return {"methods": methods, "exprs": exprs, "combine_moves": cmoves,
           "base": base_stages(transform, len(species)), "slot_roles": slot_roles(records)}


# -- C++ -------------------------------------------------------------------------------------------------------

def emit_combine(c: Mapping[str, Any]) -> list[str]:
   """!C++ of capsule::combine::kCombine (include/capsule/combine/data.h) and rules::combine_value() (in the generated
   battle.cpp; a battle without methods gets the empty tables).
   @param c  compile_battle() result
   @return lines
   """
   cb = c["combine"]
   L = ["namespace capsule::rules {", "",
        "/// A combination expression's value (generated from the combine methods; B1).",
        "int32_t combine_value(int32_t expr, const combine::ValueInputs& in) {"]
   if not cb["exprs"]:
      L += ["    (void)expr;", "    (void)in;", "    return 0;   // no combination expressions", "}", ""]
   else:
      L.append("    switch (expr) {")
      for k, e in enumerate(cb["exprs"]):
         L.append(f"    case {k}:   // {e['owner']}: {e['expr']}")
         L.append(f"        return {rc.to_cpp(e['tree'], e['constants'], e['names'])};")
      L += ["    default:", "        (void)in;", "        return 0;", "    }", "}", ""]
   L += ["} // namespace capsule::rules", "", "namespace capsule::combine {", "", "namespace {", ""]
   methods = cb["methods"]
   # SlotData {requires (growth rule), reserved}
   L.append("const SlotData kSlots[] = {")
   for m in methods:
      for k, rid in enumerate(m["slots"]):
         L.append(f"    {{0x{rid:04X}, 0}},   // {m['name']}.{m['roles'][k]}")
   if not methods:
      L.append("    {0xFFFF, 0},")
   L.append("};")
   # MatcherData {kind, field, value}
   matchers = [(m, r) for m in methods for r in m["rules"] if r["kind"] == "recipe"]
   L.append("const MatcherData kMatchers[] = {")
   for m, r in matchers:
      for kind, field, value in r["matchers"]:
         L.append(f"    {{static_cast<MatcherKind>({kind}), 0x{field:02X}, {value}}},   // {m['name']} "
                  f"-> {r['result_name']}")
   if not matchers:
      L.append("    {MatcherKind::Species, 0, 0},")
   L.append("};")
   # TableData {first cell, cell count, key kind, field, first input, second input, symmetric, reserved}
   # CellData {a, b, value, reserved}
   tables = [(m, name, t) for m in methods for name, t in m["tables"].items()]
   L.append("const CellData kCells[] = {")
   for m, name, t in tables:
      for a, b, v in t["cells"]:
         L.append(f"    {{{a}, {b}, {v}, 0}},   // {m['name']}.{name}")
   if not any(t["cells"] for _m, _n, t in tables):
      L.append("    {0, 0, 0, 0},")
   L.append("};")
   L.append("const TableData kTables[] = {")
   first_cell = 0
   for m, name, t in tables:
      L.append(f"    {{{first_cell}, {len(t['cells'])}, static_cast<KeyKind>({t['key_kind']}), 0x{t['field']:02X}, "
               f"{t['inputs'][0]}, {t['inputs'][1]}, {1 if t['symmetric'] else 0}, {{0, 0, 0}}}},   "
               f"// {m['name']}.{name}")
      first_cell += len(t["cells"])
   if not tables:
      L.append("    {0, 0, KeyKind::Field, 0, 0, 1, 1, {0, 0, 0}},")
   L.append("};")
   # RuleData {when, expr, first, result, kind, count, order, choose, else, reserved}
   L.append("const RuleData kRules[] = {")
   first_matcher = 0
   first_table = 0
   for m in methods:
      for r in m["rules"]:
         first = 0
         count = 0
         order, choose, other = NONE8, 0, 0
         expr = r["expr"]
         if r["kind"] == "recipe":
            first, count = first_matcher, len(r["matchers"])
            first_matcher += count
         elif r["kind"] == "table":
            first = first_table + r["table"]
            p = r["pick"]
            expr, order = p["where"], p["order"]
            choose, other = PICK_CHOICES.index(p["choose"]), PICK_ELSE.index(p["else"])
         L.append(f"    {{0x{r['when']:04X}, 0x{expr:04X}, {first}, 0x{r['result']:04X}, "
                  f"static_cast<RuleKind>({RULE_KINDS.index(r['kind'])}), {count}, 0x{order:02X}, "
                  f"static_cast<PickChoose>({choose}), static_cast<PickElse>({other}), {{0, 0, 0}}}},   "
                  f"// {m['name']} {r['kind']}")
      first_table += len(m["tables"])
   if not any(m["rules"] for m in methods):
      L.append("    {0xFFFF, 0xFFFF, 0, 0xFFFF, RuleKind::Recipe, 0, 0xFF, PickChoose::First, PickElse::None, "
               "{0, 0, 0}},")
   L.append("};")
   # the labels
   for k, m in enumerate(methods):
      L.append(f"const text::Code kLabel{k}[] = {{{', '.join(f'0x{x:02X}' for x in m['label_text'])}}};")
   # MethodData
   L.append("const MethodData kMethods[] = {")
   first_slot = first_rule = 0
   for k, m in enumerate(methods):
      mv = m["moves"]
      srcs = [f"{{static_cast<MoveSource>({MOVE_SOURCES.index(s['kind'])}), {s['limit']}, "
              f"{(SOURCE_ON if s['on'] else 0) | (SOURCE_ALL if s['all'] else 0)}, 0x{s['from_slot']:02X}}}"
              for s in mv["sources"]]
      srcs += [f"{{MoveSource::Known, 0, 0, 0x{ANY_INPUT:02X}}}"] * (MAX_SOURCES - len(srcs))
      msgs = ", ".join(f"0x{x:02X}" for x in m["messages"])
      L.append(f"    {{kLabel{k}, {first_slot}, {first_rule}, 0x{m['odds']:04X}, 0x{m['fee']:04X}, "
               f"0x{m['level']:04X}, {len(m['rules'])}, static_cast<MethodKind>({KINDS.index(m['kind'])}), "
               f"static_cast<Delivery>({DELIVERIES.index(m['delivery'])}), "
               f"static_cast<Afterwards>({AFTERWARDS.index(m['afterwards'])}), "
               f"static_cast<Chooser>({CHOOSERS.index(mv['chooser'])}), {m['min']}, {m['max']}, {mv['limit']}, "
               f"{len(mv['sources'])}, {m['flags']}, {{{', '.join(srcs)}}}, {{{msgs}}}, {m['cycle_steps']}}},   "
               f"// {m['name']}")
      first_slot += len(m["slots"])
      first_rule += len(m["rules"])
   if not methods:
      L.append("    {nullptr, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0, MethodKind::Immediate, Delivery::Immediate, "
               "Afterwards::Kept, Chooser::Ordered, 2, 2, 1, 0, 0, {}, {}, 0},")
   L.append("};")
   # combine moves: offsets per species (count + 1), then the moves
   moves = cb["combine_moves"]
   offsets = [0]
   for lst in moves:
      offsets.append(offsets[-1] + len(lst))
   flat = [x for lst in moves for x in lst]
   L.append(f"const uint16_t kMoveFirst[] = {{{', '.join(str(x) for x in offsets)}}};")
   L.append(f"const uint16_t kMoves[] = {{{', '.join(str(x) for x in flat) or '0'}}};")
   base = cb["base"] or [0]
   L.append(f"const uint16_t kBase[] = {{{', '.join(str(x) for x in base)}}};")
   # B2: the egg and nursery tables
   egg_lines, eggs = egg_initializer(cb.get("eggs"), cb.get("transfer_flags", 0))
   L += egg_lines
   L += ["", "} // namespace", ""]
   L += [f"static_assert({len(methods)} <= kMaxMethods, \"more combination methods than the engine holds\");",
         "const CombineData kCombine = {",
         f"    kMethods, kSlots, kRules, kMatchers, kTables, kCells, kMoveFirst, kMoves, kBase, "
         f"{len(cb['base'])}, {len(methods)},",
         f"    {eggs},",
         f"    {nursery_initializer(cb.get('nursery'))}",
         "};", "", "} // namespace capsule::combine", ""]
   return L


def combine_ids(c: Mapping[str, Any]) -> list[str]:
   """!battle_ids.h COMBINE_METHOD_<NAME> (only with methods: the ids header of a battle without them stays as it
   was).
   @param c  compile_battle() result
   @return lines
   """
   methods = c["combine"]["methods"]
   if not methods:
      return []
   L = [f"inline constexpr uint8_t COMBINE_METHOD_{ident(m['name'])} = {k};   ///< Combination method {m['label']}."
        for k, m in enumerate(methods)]
   L.append(f"inline constexpr int NUM_COMBINE_METHODS = {len(methods)};")
   return L + [""]


# -- Python mirrors (tools, the studio's previews) ---------------------------------------------------------------

def method_of(c: Mapping[str, Any], name: str) -> tuple[int, Mapping[str, Any]]:
   """!A method by name.
   @param c     compile_battle() result
   @param name  its name
   @return (index, compiled method)
   @throws ManifestError for an unknown name
   """
   for k, m in enumerate(c["combine"]["methods"]):
      if m["name"] == name:
         return k, m
   raise ManifestError(f"no combination method {name!r} ({', '.join(m['name'] for m in c['combine']['methods'])})")


def record_values(c: Mapping[str, Any], record: Mapping[str, Any]) -> dict[str, int]:
   """!A record's single-value roles as the expressions read them (stored or derived; growth when_values()).
   @param c       compile_battle() result
   @param record  record dict ("species": name, roles)
   @return input name -> value (the growth scope's role names)
   """
   from .growth import when_values
   return when_values(c, record, LEAD_TRIGGER)


def input_values(c: Mapping[str, Any], method: Mapping[str, Any], records: Sequence[Mapping[str, Any]],
                 context: Mapping[str, Any] | None = None) -> dict[str, int]:
   """!Every base input of a method's expressions for some records (rules::combine_value()'s inputs).
   @param c        compile_battle() result
   @param method   compiled method
   @param records  the inputs (record dicts, slot order)
   @param context  context value name -> value (a number or one of its names); the others their defaults
   @return input name -> value
   """
   from .growth import species_index
   when = c["transform"]["scope"]
   values: dict[str, int] = {}
   species = c["species"]
   fields = c.get("species_fields", [])
   levels = []
   kinds = []
   for k, role in enumerate(method["roles"]):
      rec = records[k] if k < len(records) else None
      rv = record_values(c, rec) if rec is not None else {}
      for rname, parts in when.roles.items():
         for nm, part in parts:
            if part is not None and part not in ("hi", "lo"):
               continue
            values[f"{role}.{nm}"] = int(rv.get(nm, 0))
      si = species_index(c, rec) if rec is not None else None
      values[f"{role}.species"] = 0 if si is None else si + 1
      values[f"{role}.base"] = 0 if si is None else c["combine"]["base"][si] + 1
      values[f"{role}.type"] = 0 if si is None else species[si]["types"][0]
      for f in fields:
         values[f"{role}.{f['name']}"] = 0 if si is None else species[si]["fields"][f["name"]]
      if rec is not None:
         levels.append(int(rv.get("level", 1)))
         kinds.append(si)
   values[COUNT_INPUT] = len(records)
   values["level_sum"] = sum(levels)
   values["level_avg"] = sum(levels) // len(levels) if levels else 0
   values["level_min"] = min(levels) if levels else 0
   values["level_max"] = max(levels) if levels else 0
   values["same_species"] = 1 if kinds and all(s == kinds[0] for s in kinds) else 0
   context = context or {}
   for cv in c["transform"]["transformation"]["contexts"]:
      v = context.get(cv["name"], cv["default"])
      v = cv["values"].index(v) if isinstance(v, str) else int(v)
      values[cv["name"]] = max(cv["range"][0], min(cv["range"][1], v))
   return values


def value_of(c: Mapping[str, Any], expr: int, values: Mapping[str, int]) -> int:
   """!A combination expression's value (rules::combine_value()).
   @param c       compile_battle() result
   @param expr    expression id
   @param values  input_values() (with cand.* / result.* where the expression reads them)
   @return its value
   """
   e = c["combine"]["exprs"][expr]
   return rc.evaluate(e["tree"], values, e["constants"])


def key_of(c: Mapping[str, Any], table: Mapping[str, Any], si: int) -> int:
   """!A species' value of a table's key.
   @param c      compile_battle() result
   @param table  compiled table
   @param si     species index
   @return its field value or its first type
   """
   sp = c["species"][si]
   return sp["types"][0] if table["key"] == TYPE_KEY else sp["fields"][table["key"]]


def candidate_values(c: Mapping[str, Any], si: int) -> dict[str, int]:
   """!A pick's candidate inputs for one species.
   @param c   compile_battle() result
   @param si  species index
   @return cand.* -> value
   """
   sp = c["species"][si]
   out = {f"{CANDIDATE}.species": si + 1, f"{CANDIDATE}.base": c["combine"]["base"][si] + 1,
          f"{CANDIDATE}.type": sp["types"][0]}
   for f in c.get("species_fields", []):
      out[f"{CANDIDATE}.{f['name']}"] = sp["fields"][f["name"]]
   return out


def recipe_matches(c: Mapping[str, Any], matchers: Sequence[tuple[int, int, int]], kinds: Sequence[int]) -> bool:
   """!Whether input species match a recipe's matchers in some order (a multiset; the engine's backtracking).
   @param c         compile_battle() result
   @param matchers  (kind, field, value) per matcher
   @param kinds     species index per input
   @return true when each input takes one matcher
   """
   if len(matchers) != len(kinds):
      return False
   fields = c.get("species_fields", [])

   def fits(mt: tuple[int, int, int], si: int) -> bool:
      """!One matcher against one species."""
      kind, field, value = mt
      sp = c["species"][si]
      if MATCHER_KINDS[kind] == "species":
         return si == value
      if MATCHER_KINDS[kind] == "type":
         return sp["types"][0] == value
      return sp["fields"][fields[field]["name"]] == value

   def place(k: int, used: int) -> bool:
      """!Places input k onward."""
      if k == len(kinds):
         return True
      return any(not used >> j & 1 and fits(mt, kinds[k]) and place(k + 1, used | 1 << j)
                 for j, mt in enumerate(matchers))
   return place(0, 0)


def preview(c: Mapping[str, Any], name: str, records: Sequence[Mapping[str, Any]],
            context: Mapping[str, Any] | None = None) -> dict[str, Any]:
   """!What a combination would give, without its random parts (combine::preview()): the count, each slot's
   requirement, the odds, then the result rules in order; a random pick gives its group (C9: and the highest fee
   over its candidates).
   @param c        compile_battle() result
   @param name     the method's name
   @param records  the inputs (record dicts in slot order: "species" name, "level", roles)
   @param context  context values (input_values())
   @return {"status" (STATUSES name), "slot" (the failing slot or None), "rule" (index or None), "species" (name or
           None), "group" (a random pick's group value or None), "level", "odds", "fee"}
   """
   from .field import field_action_values
   from .growth import holds, species_index
   idx, m = method_of(c, name)
   out: dict[str, Any] = {"status": "ok", "slot": None, "rule": None, "species": None, "group": None, "level": 0,
                          "odds": 0, "fee": 0}
   # B2: a nursery method and an egg delivery preview as any other (the nursery runs them; the result is an egg)
   if not m["min"] <= len(records) <= m["max"]:
      return {**out, "status": "wrong_count"}
   for k, rule in enumerate(m["slots"][:len(records)]):
      if rule != NO_EXPR and not holds(c, rule, field_action_values(c, records[k])):
         return {**out, "status": "unfit", "slot": k}
   values = input_values(c, m, records, context)
   odds = 1
   if m["odds"] != NO_EXPR:
      value = max(0, min(VALUE_MAX, value_of(c, m["odds"], values)))
      odds = run_pipeline(c, "combine_odds", idx, values, value, "odds")
   out["odds"] = odds
   if odds <= 0:
      return {**out, "status": "incompatible"}
   kinds = [species_index(c, r) for r in records]
   names = [sp["name"] for sp in c["species"]]
   pool: list[int] = []
   for j, r in enumerate(m["rules"]):
      if r["when"] != NO_EXPR and value_of(c, r["when"], values) == 0:
         continue
      got = rule_result(c, m, r, values, kinds, pool)
      if got is None:
         continue
      out["rule"] = j
      if got[0] == "random":
         out["group"] = got[1]
      else:
         out["species"] = names[got[1]]
      break
   else:
      return {**out, "status": "refused"}
   rv = result_values(c, names.index(out["species"])) if out["species"] is not None else \
      {k: 0 for k in input_scope_names(c, RESULT)}
   level = max(1, min(c["rules"]["level_max"], value_of(c, m["level"], {**values, **rv})))
   out["level"] = level
   if m["fee"] != NO_EXPR and out["species"] is not None:
      out["fee"] = fee_value(c, m, idx, values, rv, level)
   elif m["fee"] != NO_EXPR and pool:
      # C9: a random pick shows the highest fee over its candidates (the most it can cost before the draw)
      fees = []
      for si in pool:
         cv = result_values(c, si)
         lv = max(1, min(c["rules"]["level_max"], value_of(c, m["level"], {**values, **cv})))
         fees.append(fee_value(c, m, idx, values, cv, lv))
      out["fee"] = max(fees)
   return out


def fee_value(c: Mapping[str, Any], m: Mapping[str, Any], idx: int, values: Mapping[str, int],
              rv: Mapping[str, int], level: int) -> int:
   """!The fee of a known result (rules::combine_fee() over the method's fee expression).
   @param c       compile_battle() result
   @param m       compiled method
   @param idx     its index
   @param values  input_values()
   @param rv      result_values() of the result species
   @param level   the result's level
   @return the fee
   """
   fv = {**values, **rv, f"{RESULT}.level": level}
   return int(run_pipeline(c, "combine_fee", idx, values, max(0, min(VALUE_MAX, value_of(c, m["fee"], fv))),
                           "fee", level))


def input_scope_names(c: Mapping[str, Any], prefix: str) -> list[str]:
   """!The names of a species' parts under a prefix (cand or result).
   @param c       compile_battle() result
   @param prefix  CANDIDATE or RESULT
   @return the names (parts, then the species fields)
   """
   return [f"{prefix}.{p}" for p in INPUT_PARTS] + [f"{prefix}.{f['name']}" for f in c.get("species_fields", [])]


def result_values(c: Mapping[str, Any], si: int) -> dict[str, int]:
   """!The result's inputs of the level and fee expressions for one species.
   @param c   compile_battle() result
   @param si  species index
   @return result.* -> value (the level apart)
   """
   return {k.replace(CANDIDATE, RESULT, 1): v for k, v in candidate_values(c, si).items()}


def run_pipeline(c: Mapping[str, Any], pipeline: str, method: int, values: Mapping[str, int], value: int,
                 output: str, result_level: int = 1) -> int:
   """!Runs a combination pipeline (rules::combine_odds() / combine_fee()).
   @param c             compile_battle() result
   @param pipeline      "combine_odds" or "combine_fee"
   @param method        method index
   @param values        input_values()
   @param value         the method's value (clamped)
   @param output        the pipeline's output name
   @param result_level  the result's level (the fee)
   @return the output
   """
   inputs = {"method": method, "count": values[COUNT_INPUT], "level_min": max(1, values["level_min"]),
             "level_max": max(1, values["level_max"]), "level_avg": max(1, values["level_avg"]),
             "same_species": values["same_species"], "result_level": result_level, "value": value}
   wanted = rc.PIPELINES[pipeline]["inputs"]
   got = rc.run_pipeline(c["pipelines"], pipeline, {k: inputs[k] for k in wanted})
   return int(got["outputs"][output])


def group_members(c: Mapping[str, Any], table: Mapping[str, Any], group: int) -> list[int]:
   """!The species of a table's group (key value equal to the group), by index.
   @param c      compile_battle() result
   @param table  compiled table
   @param group  the group value
   @return species indices
   """
   return [si for si in range(len(c["species"])) if key_of(c, table, si) == group]


def rule_result(c: Mapping[str, Any], m: Mapping[str, Any], r: Mapping[str, Any], values: Mapping[str, int],
                kinds: Sequence[int], pool_out: list[int] | None = None) -> tuple[str, int] | None:
   """!What one result rule gives.
   @param c         compile_battle() result
   @param m         compiled method
   @param r         compiled rule
   @param values    input_values()
   @param kinds     species index per input
   @param pool_out  C9: receives a random pick's candidates (species indices), or None
   @return ("species", index), ("random", group) for a random pick, or None (no match)
   """
   if r["kind"] == "recipe":
      return ("species", r["result"]) if recipe_matches(c, r["matchers"], kinds) else None
   if r["kind"] == "expression":
      v = value_of(c, r["expr"], values)
      return ("species", v - 1) if 1 <= v <= len(c["species"]) else None
   table = list(m["tables"].values())[r["table"]]
   a, b = (key_of(c, table, kinds[i]) if i < len(kinds) else None for i in table["inputs"])
   cell = next((v for x, y, v in table["cells"] if (x, y) == (a, b) or (table["symmetric"] and (y, x) == (a, b))),
               None)
   if cell is None:
      return None
   p = r["pick"]
   group = group_members(c, table, cell)
   if not group:
      return None
   fields = c.get("species_fields", [])

   def order(si: int) -> tuple[int, int]:
      """!A candidate's order key."""
      return ((c["species"][si]["fields"][fields[p["order"]]["name"]] if p["order"] != NONE8 else si), si)
   group.sort(key=order)
   ok = [si for si in group if p["where"] == NO_EXPR or
         value_of(c, p["where"], {**values, **candidate_values(c, si)}) != 0]
   pool = ok
   choose = p["choose"]
   if not pool:
      if p["else"] == "none":
         return None
      pool, choose = group, p["else"]
   if choose == "random":
      if len(pool) > 1 and pool_out is not None:
         pool_out[:] = pool
      return ("random", cell) if len(pool) > 1 else ("species", pool[0])
   return ("species", pool[0] if choose == "first" else pool[-1])


def move_candidates(c: Mapping[str, Any], name: str, result: str, records: Sequence[Mapping[str, Any]]) -> list[str]:
   """!The moves a method's sources gather for a result (combine's candidate list, before the chooser).
   @param c        compile_battle() result
   @param name     the method's name
   @param result   the result species' name
   @param records  the inputs ("moves": move names or indices)
   @return move names in source order, each once
   """
   _idx, m = method_of(c, name)
   moves = c["names"]["moves"]
   si = [sp["name"] for sp in c["species"]].index(result)
   sp = c["species"][si]
   known = [[moves.index(x) if isinstance(x, str) else int(x) for x in (r.get("moves") or [])] for r in records]
   out: list[int] = []
   for s in m["moves"]["sources"]:
      if not s["on"]:
         continue
      if s.get("from") is not None:
         # C9: one input's moves in its slot order, those the source's list holds
         one = known[s["from_slot"]] if s["from_slot"] < len(known) else []
         listed = {"known": None, "combine_moves": c["combine"]["combine_moves"][si],
                   "shared_level_up": [mv for _lv, mv in sp["learnset"]],
                   "teachable_known": sp.get("teachable") or []}[s["kind"]]
         got = [x for x in one if listed is None or x in listed]
      elif s["kind"] == "known":
         got = [x for lst in known for x in lst]
      elif s["kind"] == "combine_moves":
         got = [x for x in c["combine"]["combine_moves"][si] if s["all"] or any(x in lst for lst in known)]
      elif s["kind"] == "shared_level_up":
         got = [mv for _lv, mv in sp["learnset"] if all(mv in lst for lst in known)]
      else:
         got = [x for x in (sp.get("teachable") or []) if any(x in lst for lst in known)]
      taken = 0
      for x in got:
         if x in out or len(out) >= MAX_CANDIDATES:
            continue
         if s["limit"] and taken >= s["limit"]:
            break
         out.append(x)
         taken += 1
   return [moves[x] for x in out]
