"""!Growth of the battle compiler (D-26 section 4, package G4): transformation
rules, forms and training values.

Structure manifest (structure.json):

  - transformation: {context_values [{name, values [names] | range [lo, hi],
    default}] (the named values a `when` expression may read besides the
    record; `time_of_day` is the game's clock port capsule/core/clock.h, the
    others the game's port growth::context_value()), cancel [triggers] (the
    triggers whose transformation the player may stop; default none),
    blocked (an expression: while it holds nothing transforms, e.g. a held
    item that stops it), learn (default true: a transformed creature learns
    its new species' moves of its level), form_role (the record role holding
    the form: a custom role)}.
  - training: {gain (a named ruleset formula of (yield, value, total,
    level): what one defeated foe's yield of a stat gives), stat_cap,
    total_cap (default: the stat_training role's top and every stat's cap
    together), yields {stat: species field}} (the receivers are the ones
    exp_policy shares a defeated foe's experience with).

Species (battle.json species[]):

  - transforms: [{into, when, on [triggers] (default every trigger the
    expression can hold for), takes_item (the held item is used up: a
    trade-with-item rule), also (a species made beside it into a free party
    slot: a companion)}], first that holds wins;
  - forms: [{name, types, base, passives, front, back, when, on}] (form 0 is
    the species itself; the others override what they give; a form with
    `when` is chosen by the same trigger rules, one without only by the
    effect primitive `form`; an entry named "base" with only when / on is
    the rule back to the species' own form).

`when` is the ruleset expression language (tools/ruleset_compiler.py) over:
the record's single-element roles (stored or unstored, through
battle::record_value(); 32-bit roles as NAME_hi / NAME_lo), per-stat roles
as ROLE.STAT (stat_training.attack), the permanent stats as stat.NAME, the
species' fields as species.NAME, `trigger` (TRIGGERS: level_up, item_use,
trade, after_battle, script, also words of the language), `item` (item_use:
the item used; trade: the held item; else 0; items by their lower-case
identifier or item.NAME) and the context values by name (their value names
are words too). Every expression is checked by the interval analysis; the
triggers a rule can hold for are worked out from it (a rule that can never
hold is an error).

Emitted as capsule::growth::kGrowth (include/capsule/growth/data.h) with
rules::growth_when() and rules::train_gain(); battle_ids.h TRIGGER_,
CONTEXT_ and FORM_ constants. find_change(), training_gain() and
form_view() are the Python mirrors (the studio's previews).
"""
from __future__ import annotations

import re
from collections.abc import Mapping, Sequence
from typing import Any

import ruleset_compiler as rc
from sequence_compiler import asset_key

from .common import (NONE8, NONE16, PAIR, PERCENT, U16_MAX, Json, Lookups, ManifestError, check_keys, ident,
                     integer, register_keys)
from .layout import CUSTOM_ROLE_BASE, PER_STAT_ROLES, ROLE_NAMES, role_info, role_max_value, unstored_info
from .progression import GEN_KINDS, SPECIES_PREFIX

## Trigger names, in capsule::growth::Trigger order.
TRIGGERS = ["level_up", "item_use", "trade", "after_battle", "script"]
## Triggers whose context carries an item (item_use: the item used; trade: the held item).
ITEM_TRIGGERS = ("item_use", "trade")
## Triggers the player may never stop: an item use's transformation is the item's logic (it fails or not).
UNCANCELLABLE = ("item_use",)
## Input name of the trigger.
TRIGGER_INPUT = "trigger"
## Input name of the context's item.
ITEM_INPUT = "item"
## Prefix of an item constant (item.NAME).
ITEM_PREFIX = "item."
## Prefix of a permanent stat input (stat.NAME).
STAT_PREFIX = "stat."
## The context value the clock port gives (capsule/core/clock.h).
TIME_OF_DAY = "time_of_day"
## Range of the clock's value (uint8_t).
CLOCK_RANGE = (0, 0xFF)
## Context value sources, in capsule::growth::ContextSource order.
CONTEXT_SOURCES = ["game", "clock"]
## Range of a context value declared without one.
DEFAULT_CONTEXT_RANGE = (0, 0xFF)
## Most context values (capsule::growth::kMaxContextValues).
MAX_CONTEXT_VALUES = 8
## Most transformation rules of one species.
MAX_TRANSFORMS = 8
## Most forms of one species besides the species itself (the form role's value fits 4 bits).
MAX_FORMS = 15
## Most distinct form names (effect logic's `form` names an 8-bit id; the base name is id 0).
MAX_FORM_NAMES = 0xFE
## Most rules in all (16-bit rule ids, kNone16 = none).
MAX_RULES = NONE16 - 1
## The name of form 0 (the species itself) in effect logic and battle_ids.h.
BASE_FORM = "base"
## A form's name: a capitalised or lower-case word (effect logic names it).
FORM_NAME = re.compile(r"[A-Za-z][A-Za-z0-9_]*\Z")
## The arguments the engine passes to the training gain formula, in order.
TRAIN_GAIN_ARGS = ("yield", "value", "total", "level")
## Highest training value of one stat (Battler::training is 8 bits).
MAX_TRAINING = 0xFF
## Highest total cap (16 bits).
MAX_TOTAL_CAP = U16_MAX
## TransformRule::flags bit: the held item is used up.
FLAG_TAKES_ITEM = 1
## Roles a `when` expression does not read whole (the species is species.NAME; lists are not single values).
NOT_WHEN_ROLES = ("species_key",)
## Bits of a role read whole (wider ones as halves, as derived expressions read them).
MAX_WHOLE_BITS = 31
## Bits of a half of a 32-bit role.
HALF_BITS = 16
## Mask of a half.
HALF_MASK = (1 << HALF_BITS) - 1
## Highest context value range bound (int32 analysis keeps a margin: values are int32).
CONTEXT_LIMIT = (1 << 30) - 1
## Default of learn (a transformed creature learns its new species' moves of its level).
DEFAULT_LEARN = True

register_keys({
   # C9: allow_eggs: whether an egg may transform (tools/battlec/nursery.py reads it)
   "transformation": {"context_values", "cancel", "blocked", "learn", "form_role", "allow_eggs"},
   "context_value": {"name", "values", "range", "default"},
   "training": {"gain", "stat_cap", "total_cap", "yields", "point"},
   "transform": {"into", "when", "on", "takes_item", "also"},
   "form": {"name", "types", "base", "passives", "front", "back", "when", "on"},
})


# -- context and inputs -----------------------------------------------------------------------------------------

def compile_contexts(src: Json, where: str) -> list[dict[str, Any]]:
   """!The context values a `when` expression reads besides the record (structure transformation.context_values).
   @param src    the manifest's list (or None)
   @param where  location
   @return [{"name", "values" (names or []), "range" (lo, hi), "default", "source" (CONTEXT_SOURCES index)}]
   """
   src = [] if src is None else src
   if not isinstance(src, list) or len(src) > MAX_CONTEXT_VALUES:
      raise ManifestError(f"{where}: a list of at most {MAX_CONTEXT_VALUES} context values")
   out = []
   seen = set()
   for k, e in enumerate(src):
      w = f"{where}[{k}]"
      check_keys(e, "context_value", w)
      name = e.get("name")
      if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", name):
         raise ManifestError(f"{w}.name: a lower-case identifier")
      if name in seen or name in (TRIGGER_INPUT, ITEM_INPUT):
         raise ManifestError(f"{w}.name: {name!r} is taken")
      seen.add(name)
      if "values" in e and "range" in e:
         raise ManifestError(f"{w}: give values (names) or range, not both")
      values: list[str] = []
      if "values" in e:
         values = e["values"]
         if not isinstance(values, list) or not values or \
               not all(isinstance(v, str) and re.fullmatch(r"[a-z][a-z0-9_]*", v) for v in values):
            raise ManifestError(f"{w}.values: a list of lower-case identifiers (value 0 first)")
         if len(set(values)) != len(values):
            raise ManifestError(f"{w}.values: a name is given twice")
         rng = (0, len(values) - 1)
      elif "range" in e:
         r = e["range"]
         if not (isinstance(r, list) and len(r) == PAIR and all(isinstance(x, int) for x in r) and r[0] <= r[1]):
            raise ManifestError(f"{w}.range: [lo, hi]")
         rng = (integer(r[0], -CONTEXT_LIMIT, CONTEXT_LIMIT, f"{w}.range"),
                integer(r[1], -CONTEXT_LIMIT, CONTEXT_LIMIT, f"{w}.range"))
      else:
         rng = CLOCK_RANGE if name == TIME_OF_DAY else DEFAULT_CONTEXT_RANGE
      source = CONTEXT_SOURCES.index("clock" if name == TIME_OF_DAY else "game")
      if name == TIME_OF_DAY and (rng[0] < CLOCK_RANGE[0] or rng[1] > CLOCK_RANGE[1]):
         raise ManifestError(f"{w}: the clock's time of day is 0 to {CLOCK_RANGE[1]}")
      default = e.get("default", rng[0])
      if isinstance(default, str):
         if default not in values:
            raise ManifestError(f"{w}.default: one of {', '.join(values) or 'its values (none named)'}")
         default = values.index(default)
      default = integer(default, rng[0], rng[1], f"{w}.default")
      out.append({"name": name, "values": list(values), "range": rng, "default": default, "source": source})
   return out


class WhenScope:
   """!What a `when` expression reads: inputs with their ranges and accessors, and the words (constants)."""

   def __init__(self, layout: Mapping[str, Any], stats: Sequence[Mapping[str, Any]],
                species_fields: Sequence[Mapping[str, Any]], contexts: Sequence[Mapping[str, Any]],
                item_names: Sequence[str], constants: Mapping[str, int], where: str) -> None:
      """!Builds the scope.
      @param layout          compiled layout
      @param stats           compiled stats
      @param species_fields  compiled species_fields
      @param contexts        compile_contexts() result
      @param item_names      the items' names (item ids 1..n)
      @param constants       the ruleset's constants with the battle constants
      @param where           location
      """
      ## Input name -> (lo, hi).
      self.ranges: dict[str, tuple[int, int]] = {}
      ## Input name -> accessor: ("role", id, element, shift, mask), ("stat", k), ("field", k), ("trigger",),
      ## ("item",) or ("context", k); F2's field actions add ("knows", move), ("passive",) and ("game", ref).
      self.access: dict[str, tuple[Any, ...]] = {}
      ## Word name -> value.
      self.constants: dict[str, int] = dict(constants)
      ## Role name -> [(input name, part)]: part None (the whole value), "hi" / "lo" (a half) or a stat name.
      self.roles: dict[str, list[tuple[str, str | None]]] = {}
      stat_names = [st["name"] for st in stats]
      names = [n for n in ROLE_NAMES if n not in layout["custom_ids"]] + list(layout["custom_ids"]) + \
         [u["name"] for u in layout.get("unstored", [])]
      for name in names:
         if name in NOT_WHEN_ROLES:
            continue
         info = role_info(layout, name) or unstored_info(layout, name)
         if info is None:
            continue
         rid, width, count = info
         top = role_max_value(layout, name)
         if count == 1:
            if width > MAX_WHOLE_BITS:
               self._add(f"{name}_hi", (0, HALF_MASK), ("role", rid, 0, HALF_BITS, HALF_MASK))
               self._add(f"{name}_lo", (0, HALF_MASK), ("role", rid, 0, 0, HALF_MASK))
               self.roles[name] = [(f"{name}_hi", "hi"), (f"{name}_lo", "lo")]
            else:
               self._add(name, (0, top), ("role", rid, 0, 0, 0))
               self.roles[name] = [(name, None)]
         elif name in PER_STAT_ROLES and count == len(stat_names):
            self.roles[name] = []
            for k, st in enumerate(stat_names):
               self._add(f"{name}.{st}", (0, top), ("role", rid, k, 0, 0))
               self.roles[name].append((f"{name}.{st}", st))
      for k, st in enumerate(stat_names):
         self._add(STAT_PREFIX + st, (0, U16_MAX), ("stat", k))
      for k, f in enumerate(species_fields):
         self._add(SPECIES_PREFIX + f["name"], (0, f["max"]), ("field", k))
      self._add(TRIGGER_INPUT, (0, len(TRIGGERS) - 1), ("trigger",))
      self._add(ITEM_INPUT, (0, len(item_names)), ("item",))
      for k, cv in enumerate(contexts):
         self._add(cv["name"], cv["range"], ("context", k))
      # the words: triggers, items, the context values' names
      words: dict[str, int] = {t: i for i, t in enumerate(TRIGGERS)}
      for i, item in enumerate(item_names):
         word = ident(item).lower()
         words[ITEM_PREFIX + word] = i + 1
         words.setdefault(word, i + 1)
      for cv in contexts:
         for i, v in enumerate(cv["values"]):
            if v in words and words[v] != i:
               raise ManifestError(f"{where}: the word {v!r} names two values (a trigger, an item or another "
                                   f"context value); rename one")
            words[v] = i
      for word, v in words.items():
         if word in self.ranges:
            if word.startswith(ITEM_PREFIX) or word in TRIGGERS:
               raise ManifestError(f"{where}: the word {word!r} is also an input name; rename that role or field")
            continue   # a bare item word shadowed by an input: item.NAME still names it
         if word in self.constants and self.constants[word] != v:
            raise ManifestError(f"{where}: the word {word!r} is also a ruleset constant")
         self.constants[word] = v

   def _add(self, name: str, rng: tuple[int, int], access: tuple[Any, ...]) -> None:
      """!Adds one input.
      @param name    input name
      @param rng     its range
      @param access  its accessor
      @return None
      """
      self.ranges[name] = rng
      self.access[name] = access

   def cpp_names(self, used: Sequence[str]) -> dict[str, str]:
      """!The C++ of each input an expression reads (inside namespace capsule::rules: `in` is growth::WhenInputs).
      @param used  the inputs it reads
      @return input name -> C++ expression
      """
      out = {}
      for n in used:
         a = self.access[n]
         if a[0] == "role":
            _k, rid, elem, shift, mask = a
            get = f"battle::record_value(*in.rec, monster::RoleId{{{rid}}}, {elem})"
            out[n] = (f"static_cast<int32_t>(({get} >> {shift}) & 0x{mask:X}u)" if mask
                      else f"static_cast<int32_t>({get})")
         elif a[0] == "stat":
            out[n] = f"static_cast<int32_t>(in.stats[{a[1]}])"
         elif a[0] == "field":
            out[n] = f"rules::species_field_of(static_cast<int32_t>(in.species), {a[1]})"
         elif a[0] == "trigger":
            out[n] = "static_cast<int32_t>(in.ctx->trigger)"
         elif a[0] == "item":
            out[n] = "static_cast<int32_t>(in.ctx->item)"
         elif a[0] == "knows":
            # F2: a field action's requirement (tools/battlec/field.py): the member knows move a[1]
            out[n] = f"rules::member_knows(in, {a[1]})"
         elif a[0] == "passive":
            out[n] = "rules::member_passive(in)"
         elif a[0] == "game":
            # F2: game state (a script flag or variable) through the script bank's reference a[1]
            out[n] = f"rules::field_state({a[1]})"
         else:
            out[n] = f"in.ctx->values[{a[1]}]"
      return out


def truth(node: Any, ranges: Mapping[str, tuple[int, int]], constants: Mapping[str, int], where: str) -> set[int]:
   """!Which truth values an expression can take over input ranges (1: non-zero), sharper than the interval
   analysis for comparisons and the logic operators (a comparison of two ranges that cannot meet is decided).
   @param node       parsed tree
   @param ranges     input name -> (lo, hi)
   @param constants  name -> int
   @param where      location
   @return a subset of {0, 1}
   """
   k = node[0]
   if k == "not":
      return {1 - v for v in truth(node[1], ranges, constants, where)}
   if k == "bin" and node[1] in ("&&", "||"):
      _k, op, left, right = node
      a = truth(left, ranges, constants, where)
      b = truth(right, ranges, constants, where)
      if op == "&&":
         return ({1} if 1 in a and 1 in b else set()) | ({0} if 0 in a or 0 in b else set())
      return ({1} if 1 in a or 1 in b else set()) | ({0} if 0 in a and 0 in b else set())
   if k == "bin" and node[1] in ("<", "<=", ">", ">=", "==", "!="):
      _k, op, left, right = node
      a = rc.interval(left, ranges, constants, where, [])
      b = rc.interval(right, ranges, constants, where, [])
      sure = {"<": a[1] < b[0], "<=": a[1] <= b[0], ">": a[0] > b[1], ">=": a[0] >= b[1],
              "==": a[0] == a[1] == b[0] == b[1], "!=": a[1] < b[0] or b[1] < a[0]}[op]
      never = {"<": a[0] >= b[1], "<=": a[0] > b[1], ">": a[1] <= b[0], ">=": a[1] < b[0],
               "==": a[1] < b[0] or b[1] < a[0], "!=": a[0] == a[1] == b[0] == b[1]}[op]
      return {1} if sure else {0} if never else {0, 1}
   lo, hi = rc.interval(node, ranges, constants, where, [])
   return ({0} if lo <= 0 <= hi else set()) | ({1} if lo != 0 or hi != 0 else set())


def compile_rule(text: Json, on: Json, scope: WhenScope, where: str) -> dict[str, Any]:
   """!One `when` expression with its triggers.
   @param text   the expression
   @param on     trigger names (or None: every trigger it can hold for)
   @param scope  WhenScope
   @param where  location
   @return {"tree", "expr", "used", "triggers" (bit mask), "on" (names)}
   @throws ManifestError when it does not parse, can leave int32 or never holds
   """
   if on is None:
      wanted = list(TRIGGERS)
   else:
      if not isinstance(on, list) or not on or not all(t in TRIGGERS for t in on):
         raise ManifestError(f"{where}.on: a list of triggers ({', '.join(TRIGGERS)})")
      wanted = [t for t in TRIGGERS if t in on]
   try:
      tree = rc.parse(text, list(scope.ranges), scope.constants, f"{where}.when")
      rc.interval(tree, scope.ranges, scope.constants, f"{where}.when", [])
      mask = 0
      for t in wanted:
         ranges = dict(scope.ranges)
         ranges[TRIGGER_INPUT] = (TRIGGERS.index(t), TRIGGERS.index(t))
         if t not in ITEM_TRIGGERS:
            ranges[ITEM_INPUT] = (0, 0)
         if 1 in truth(tree, ranges, scope.constants, f"{where}.when"):
            mask |= 1 << TRIGGERS.index(t)
   except rc.RulesetError as e:
      raise ManifestError(str(e))
   if mask == 0:
      raise ManifestError(f"{where}.when: {text!r} can never hold on {', '.join(wanted)}")
   return {"tree": tree, "expr": text, "used": sorted(rc.used_inputs(tree)), "triggers": mask,
           "on": [t for t in TRIGGERS if mask >> TRIGGERS.index(t) & 1]}


# -- the structure's sections -------------------------------------------------------------------------------------

def compile_transformation(src: Json, layout: Mapping[str, Any], where: str) -> dict[str, Any]:
   """!The structure's transformation section (without its blocked expression: compile_growth_rules() compiles it).
   @param src     the manifest's transformation (or None)
   @param layout  compiled layout
   @param where   location
   @return {"contexts", "cancel" (mask), "learn", "form_role" (role ID or None), "form_role_name",
           "form_stored", "blocked_text"}
   """
   src = {} if src is None else src
   w = f"{where}: transformation"
   check_keys(src, "transformation", w)
   cancel = src.get("cancel", [])
   if not isinstance(cancel, list) or not all(t in TRIGGERS for t in cancel):
      raise ManifestError(f"{w}.cancel: a list of triggers ({', '.join(TRIGGERS)})")
   for t in cancel:
      if t in UNCANCELLABLE:
         raise ManifestError(f"{w}.cancel: {t!r} is the item's own logic (it transforms or fails); a player "
                             f"cannot stop it")
   learn = src.get("learn", DEFAULT_LEARN)
   if not isinstance(learn, bool):
      raise ManifestError(f"{w}.learn: true or false")
   role = src.get("form_role")
   rid = None
   stored = False
   if role is not None:
      info = role_info(layout, role)
      unstored = unstored_info(layout, role)
      if not isinstance(role, str) or (info is None and unstored is None) or \
            (role not in layout["custom_ids"] and role not in layout.get("unstored_ids", {})):
         raise ManifestError(f"{w}.form_role: a custom role of the layout (the record's form)")
      rid, _width, count = info or unstored
      if count != 1:
         raise ManifestError(f"{w}.form_role: {role!r} holds {count} elements; a form is one value")
      stored = info is not None
   return {"contexts": compile_contexts(src.get("context_values"), f"{w}.context_values"),
           "cancel": sum(1 << TRIGGERS.index(t) for t in set(cancel)), "learn": learn, "form_role": rid,
           "form_role_name": role, "form_stored": stored, "blocked_text": src.get("blocked")}


def compile_training(src: Json, ruleset: Mapping[str, Any], rs_name: str, stats: Sequence[Mapping[str, Any]],
                     species_fields: Sequence[Mapping[str, Any]], layout: Mapping[str, Any],
                     rules: Mapping[str, Any], where: str) -> dict[str, Any] | None:
   """!The structure's training section.
   @param src             the manifest's training (or None: no training)
   @param ruleset         compiled ruleset
   @param rs_name         ruleset file name
   @param stats           compiled stats
   @param species_fields  compiled species_fields
   @param layout          compiled layout
   @param rules           compiled rules (level_max)
   @param where           location
   @return {"gain", "stat_cap", "total_cap", "yields" (field index or NONE8 per stat)}, or None
   """
   if src is None:
      return None
   w = f"{where}: training"
   check_keys(src, "training", w)
   info = role_info(layout, "stat_training")
   if info is None:
      raise ManifestError(f"{w}: training values live in the stat_training role, which the layout does not hold")
   top = min(role_max_value(layout, "stat_training"), MAX_TRAINING)
   stat_cap = integer(src.get("stat_cap", top), 0, top, f"{w}.stat_cap")
   total_cap = integer(src.get("total_cap", min(stat_cap * len(stats), MAX_TOTAL_CAP)), 0, MAX_TOTAL_CAP,
                       f"{w}.total_cap")
   formulas = rc.named_formulas(ruleset)
   gain = src.get("gain")
   if not isinstance(gain, str) or gain not in formulas:
      raise ManifestError(f"{w}.gain: names a formula of {rs_name} with \"args\" ({', '.join(TRAIN_GAIN_ARGS)}); "
                          f"its named formulas: {', '.join(formulas) or 'none'}")
   f = formulas[gain]
   if len(f["inputs"]) != len(TRAIN_GAIN_ARGS):
      raise ManifestError(f"{rs_name}: {gain} takes {len(f['inputs'])} args; the training gain takes "
                          f"({', '.join(TRAIN_GAIN_ARGS)})")
   yields_src = src.get("yields", {})
   if not isinstance(yields_src, dict):
      raise ManifestError(f"{w}.yields: {{stat: species field}}")
   stat_names = [st["name"] for st in stats]
   field_names = [sf["name"] for sf in species_fields]
   yields = [NONE8] * len(stats)
   top_yield = 0
   for st, field in yields_src.items():
      if st not in stat_names:
         raise ManifestError(f"{w}.yields.{st}: not a stat ({', '.join(stat_names)})")
      if field not in field_names:
         raise ManifestError(f"{w}.yields.{st}: {field!r} is not a species field "
                             f"({', '.join(field_names) or 'none declared'}; species_fields)")
      yields[stat_names.index(st)] = field_names.index(field)
      top_yield = max(top_yield, species_fields[field_names.index(field)]["max"])
   need = [(0, top_yield), (0, stat_cap), (0, total_cap), (1, rules["level_max"])]   # TRAIN_GAIN_ARGS order
   for arg, (lo, hi) in enumerate(need):
      key = f["inputs"][arg]
      have = f["ranges"][key]
      if have[0] > lo or have[1] < hi:
         raise ManifestError(f"{rs_name}: {gain} (training gain): input {key!r} ({TRAIN_GAIN_ARGS[arg]}) must cover "
                             f"[{lo}, {hi}] (the engine passes values in that range); it declares {list(have)}")
   if f["range"][0] < -MAX_TRAINING or f["range"][1] > MAX_TRAINING:
      raise ManifestError(f"{rs_name}: {gain} (training gain): range must lie within [{-MAX_TRAINING}, "
                          f"{MAX_TRAINING}]")
   # C6 (review4 finding 19): the ruleset point each gain goes through in battle (held items, passives), or none
   point = src.get("point")
   points = ruleset.get("points") or {}
   if point is not None and point not in points:
      raise ManifestError(f"{w}.point: names a point of {rs_name} ({', '.join(points) or 'none declared'}), "
                          f"got {point!r}")
   return {"gain": gain, "stat_cap": stat_cap, "total_cap": total_cap, "yields": yields, "point": point}


# -- species rules and forms ----------------------------------------------------------------------------------------

def compile_species_growth(entries: Sequence[Json], species: Sequence[Mapping[str, Any]], n: Lookups,
                           scope: WhenScope, transformation: Mapping[str, Any], stat_names: Sequence[str],
                           passive_slots: Sequence[Mapping[str, Any]], types_per_species: int,
                           rules: list[dict[str, Any]], where: str) -> list[dict[str, Any]]:
   """!Each species' transformation rules and forms, in the compiled species' (key) order.
   @param entries            the manifest's species (manifest order)
   @param species            compiled species (key order)
   @param n                  lookups (types, passives)
   @param scope              WhenScope
   @param transformation     compile_transformation() result (the form role)
   @param stat_names         the stats' names
   @param passive_slots      compiled passive slots
   @param types_per_species  most types a species has
   @param rules              the rule list (appended to; a rule's id is its index)
   @param where              location (battle.json)
   @return per species {"name", "transforms" [{"into", "also", "rule", "flags", "triggers"}], "forms" [{"name",
           "base", "types", "passives", "front", "back", "rule" (id or None), "triggers"}]}
   """
   index = {sp["name"]: i for i, sp in enumerate(species)}
   raw = {e.get("name"): (k, e) for k, e in enumerate(entries)}
   out = []
   for sp in species:
      k, e = raw[sp["name"]]
      w = f"{where}: species[{k}] ({sp['name']})"
      transforms = []
      src = e.get("transforms", [])
      if not isinstance(src, list) or len(src) > MAX_TRANSFORMS:
         raise ManifestError(f"{w}.transforms: a list of at most {MAX_TRANSFORMS} rules")
      for j, t in enumerate(src):
         tw = f"{w}.transforms[{j}]"
         check_keys(t, "transform", tw)
         into = t.get("into")
         if into not in index or into == sp["name"]:
            raise ManifestError(f"{tw}.into: another species")
         also = t.get("also")
         if also is not None and also not in index:
            raise ManifestError(f"{tw}.also: a species (made beside it into a free party slot)")
         takes = t.get("takes_item", False)
         if not isinstance(takes, bool):
            raise ManifestError(f"{tw}.takes_item: true or false")
         if "when" not in t:
            raise ManifestError(f"{tw}.when: required (the rule's expression)")
         rule = compile_rule(t["when"], t.get("on"), scope, tw)
         rule["owner"] = f"{sp['name']} -> {into}"
         rules.append(rule)
         transforms.append({"into": index[into], "into_name": into, "also": NONE16 if also is None else index[also],
                            "also_name": also, "rule": len(rules) - 1, "flags": FLAG_TAKES_ITEM if takes else 0,
                            "triggers": rule["triggers"]})
      forms = []
      fsrc = e.get("forms", [])
      if not isinstance(fsrc, list) or len(fsrc) > MAX_FORMS:
         raise ManifestError(f"{w}.forms: a list of at most {MAX_FORMS} forms")
      if fsrc and transformation["form_role"] is None:
         raise ManifestError(f"{w}.forms: the structure's transformation.form_role names no record role for the "
                             f"form")
      seen = {BASE_FORM}
      base_rule = None
      for j, f in enumerate(fsrc):
         fw = f"{w}.forms[{j}]"
         check_keys(f, "form", fw)
         name = f.get("name")
         if isinstance(name, str) and name.lower() == BASE_FORM:
            # the rule that brings it back to its own form: only `when` / `on`, no overrides
            if base_rule is not None or set(f) - {"name", "when", "on"} or "when" not in f:
               raise ManifestError(f"{fw}: a {BASE_FORM!r} entry gives the rule back to the species' own form: "
                                   f"only when (and on), once")
            if not transformation["form_stored"]:
               raise ManifestError(f"{fw}.when: a form chosen by rules is written to the record: the form role "
                                   f"{transformation['form_role_name']!r} must be stored")
            base_rule = compile_rule(f["when"], f.get("on"), scope, fw)
            base_rule["owner"] = f"{sp['name']} form {BASE_FORM}"
            rules.append(base_rule)
            base_rule["id"] = len(rules) - 1
            continue
         if not isinstance(name, str) or not FORM_NAME.match(name) or name.lower() in seen:
            raise ManifestError(f"{fw}.name: a new word ({BASE_FORM!r} is the species itself)")
         seen.add(name.lower())
         form = {"name": name, "base": list(sp["base"]), "types": list(sp["types"]),
                 "passives": list(sp["passives"]), "front": sp["front"], "back": sp["back"], "rule": None,
                 "triggers": 0}
         if "types" in f:
            tl = f["types"]
            if not isinstance(tl, list) or not 1 <= len(tl) <= types_per_species:
               raise ManifestError(f"{fw}.types: 1 to {types_per_species} types")
            form["types"] = [n["types"](x, f"{fw}.types") for x in tl] + \
               [NONE8] * (len(sp["types"]) - len(tl))
         if "base" in f:
            base = f["base"]
            if not isinstance(base, list) or len(base) != len(stat_names):
               raise ManifestError(f"{fw}.base: {len(stat_names)} base stats ({' '.join(stat_names)})")
            form["base"] = [integer(v, 1, U16_MAX, f"{fw}.base") for v in base]
         if "passives" in f:
            ab = f["passives"]
            if not isinstance(ab, list) or len(ab) > len(passive_slots):
               raise ManifestError(f"{fw}.passives: up to {len(passive_slots)}, one per passive slot")
            form["passives"] = [NONE8 if a is None else n["passives"](a, f"{fw}.passives") for a in ab] + \
               [NONE8] * (len(passive_slots) - len(ab))
         for side in ("front", "back"):
            if side in f:
               form[side] = asset_key(f[side], f"{fw}.{side}") if f[side] else 0
         if "when" in f:
            if not transformation["form_stored"]:
               raise ManifestError(f"{fw}.when: a form chosen by rules is written to the record: the form role "
                                   f"{transformation['form_role_name']!r} must be stored")
            rule = compile_rule(f["when"], f.get("on"), scope, fw)
            rule["owner"] = f"{sp['name']} form {name}"
            rules.append(rule)
            form["rule"] = len(rules) - 1
            form["triggers"] = rule["triggers"]
         elif "on" in f:
            raise ManifestError(f"{fw}.on: only a form with `when` has triggers")
         forms.append(form)
      out.append({"name": sp["name"], "transforms": transforms, "forms": forms,
                  "base_rule": None if base_rule is None else base_rule["id"],
                  "base_triggers": 0 if base_rule is None else base_rule["triggers"]})
   return out


def species_roles(layout: Mapping[str, Any], generation: Sequence[Mapping[str, Any]],
                  form_role: str | None) -> list[str]:
   """!The stored derived roles a transformation re-derives: the ones whose expressions read a species field or the
   form role (the others keep their values: an imported record keeps what its sender gave).
   @param layout      compiled layout
   @param generation  compiled generation list
   @param form_role   the form role's name, or None
   @return role names
   """
   out = []
   for e in generation:
      if GEN_KINDS[e["kind"]] != "derived" or role_info(layout, e["role"]) is None:
         continue
      used = e["formula"]["used"]
      if any(u.startswith(SPECIES_PREFIX) for u in used) or (form_role is not None and form_role in used):
         out.append(e["role"])
   return out


def compile_growth_rules(b: Mapping[str, Any], layout: Mapping[str, Any], stats: Sequence[Mapping[str, Any]],
                   species: Sequence[Mapping[str, Any]], species_fields: Sequence[Mapping[str, Any]], n: Lookups,
                   item_names: Sequence[str], ruleset: Mapping[str, Any], rs_name: str, rules: Mapping[str, Any],
                   passive_slots: Sequence[Mapping[str, Any]], types_per_species: int,
                   generation: Sequence[Mapping[str, Any]], swhere: str, where: str) -> dict[str, Any]:
   """!The whole growth section: transformation rules, forms, context values and training.
   @param b                  the merged battle manifest
   @param layout             compiled layout
   @param stats              compiled stats
   @param species            compiled species (key order)
   @param species_fields     compiled species_fields
   @param n                  lookups
   @param item_names         the items' names
   @param ruleset            compiled ruleset
   @param rs_name            ruleset file name
   @param rules              compiled rules (level_max, stat_cap)
   @param passive_slots      compiled passive slots
   @param types_per_species  most types a species has
   @param generation         compiled generation list (the derived roles a transformation re-derives)
   @param swhere             location of the structure manifest
   @param where              location of battle.json
   @return {"transformation", "training", "rules" (each {"tree", "expr", "used", "triggers", "on", "owner"}),
           "species" (compile_species_growth()), "form_names" (BASE_FORM first), "blocked" (rule id or None),
           "rederive" (role names), "scope" (WhenScope)}
   """
   tr = compile_transformation(b.get("transformation"), layout, swhere)
   constants = {**ruleset["constants"], **{k: rules[v] for k, v in rc.BATTLE_CONSTANTS.items()}}
   scope = WhenScope(layout, stats, species_fields, tr["contexts"], item_names, constants,
                     f"{swhere}: transformation")
   rule_list: list[dict[str, Any]] = []
   per_species = compile_species_growth(b.get("species", []), species, n, scope, tr,
                                        [st["name"] for st in stats], passive_slots, types_per_species, rule_list,
                                        where)
   blocked = None
   if tr["blocked_text"] is not None:
      rule = compile_rule(tr["blocked_text"], None, scope, f"{swhere}: transformation.blocked")
      rule["owner"] = "blocked"
      rule_list.append(rule)
      blocked = len(rule_list) - 1
   if len(rule_list) > MAX_RULES:
      raise ManifestError(f"{where}: more than {MAX_RULES} growth rules")
   form_names = form_names_of(b.get("species", []))
   if len(form_names) > MAX_FORM_NAMES:
      raise ManifestError(f"{where}: more than {MAX_FORM_NAMES} distinct form names")
   for sg in per_species:
      for f in sg["forms"]:
         f["name_id"] = form_names.index(f["name"])
   training = compile_training(b.get("training"), ruleset, rs_name, stats, species_fields, layout, rules, swhere)
   return {"transformation": tr, "training": training, "rules": rule_list, "species": per_species,
           "form_names": form_names, "blocked": blocked,
           "rederive": species_roles(layout, generation, tr["form_role_name"]), "scope": scope}


def form_names_of(entries: Sequence[Json]) -> list[str]:
   """!Every species' form names, BASE_FORM first, then in manifest order without repeats (their ids: effect logic's
   `form` and battle_ids.h FORM_<NAME>).
   @param entries  the manifest's species
   @return names
   """
   out = [BASE_FORM]
   for e in entries:
      forms = e.get("forms", []) if isinstance(e, dict) else []
      for f in forms if isinstance(forms, list) else []:
         name = f.get("name") if isinstance(f, dict) else None
         if isinstance(name, str) and name not in out and name.lower() != BASE_FORM:
            out.append(name)
   return out


def reads_fields(g: Mapping[str, Any]) -> bool:
   """!Whether growth reads a species field (rules::species_field_of() then needs the table).
   @param g  compile_growth_rules() result
   @return true for a rule reading species.NAME or training with yields
   """
   if g["training"] is not None and any(y != NONE8 for y in g["training"]["yields"]):
      return True
   return any(u.startswith(SPECIES_PREFIX) for r in g["rules"] for u in r["used"])


# -- C++ -------------------------------------------------------------------------------------------------------

def role_set(layout: Mapping[str, Any], names: Sequence[str]) -> tuple[int, int]:
   """!A monster::RoleSet of roles.
   @param layout  compiled layout
   @param names   role names
   @return (engine word, custom word)
   """
   engine = custom = 0
   for name in names:
      rid = (role_info(layout, name) or unstored_info(layout, name))[0]
      if rid >= CUSTOM_ROLE_BASE:
         custom |= 1 << (rid - CUSTOM_ROLE_BASE)
      else:
         engine |= 1 << rid
   return engine, custom


def emit_growth(c: Mapping[str, Any]) -> list[str]:
   """!C++ of capsule::growth::kGrowth (include/capsule/growth/data.h), rules::growth_when() and
   rules::train_gain() (in the generated battle.cpp, after the battle tables: the forms' learnsets are its
   kLearn arrays).
   @param c  compile_battle() result
   @return lines
   """
   g = c["transform"]
   scope = g["scope"]
   L = ["namespace capsule::rules {", "",
        "/// Whether growth rule @p rule holds (generated from the transformation rules and forms; G4).",
        "bool growth_when(int32_t rule, const growth::WhenInputs& in) {"]
   if not g["rules"]:
      L += ["    (void)rule;", "    (void)in;", "    return false;   // no growth rules", "}", ""]
   else:
      L.append("    switch (rule) {")
      for k, r in enumerate(g["rules"]):
         names = scope.cpp_names([u for u in r["used"]])
         L.append(f"    case {k}:   // {r['owner']}: {r['expr']}")
         L.append(f"        return {rc.to_cpp(r['tree'], scope.constants, names)} != 0;")
      L += ["    default:", "        (void)in;", "        return false;", "    }", "}", ""]
   tg = g["training"]
   L += ["/// The training gain of one stat for one defeated foe (structure training.gain; G4).",
         "int32_t train_gain(int32_t yield, int32_t value, int32_t total, int32_t level) {"]
   if tg is None:
      L += ["    (void)yield;", "    (void)value;", "    (void)total;", "    (void)level;",
            "    return 0;   // no training", "}"]
   else:
      L += [f"    return {rc.FORMULA_FUNCTION_PREFIX}{tg['gain']}(yield, value, total, level);", "}"]
   L += ["", "} // namespace capsule::rules", "", "namespace capsule::growth {", "", "namespace {", ""]
   species = c["species"]
   transforms = [t for sg in g["species"] for t in sg["transforms"]]
   forms = [(si, f) for si, sg in enumerate(g["species"]) for f in sg["forms"]]
   # TransformRule {into, also, rule, triggers, flags}
   L.append("const TransformRule kTransforms[] = {")
   for sg in g["species"]:
      for t in sg["transforms"]:
         L.append(f"    {{{t['into']}, 0x{t['also']:04X}, {t['rule']}, 0x{t['triggers']:02X}, {t['flags']}}},"
                  f"   // {sg['name']} -> {t['into_name']}")
   if not transforms:
      L.append("    {0, 0, 0, 0, 0},")
   L.append("};")
   # FormData {species view, rule, name id, triggers}: the view is the species with the form's overrides
   for k, (si, _f) in enumerate(forms):
      L.append(f"const text::Code kFormSpeciesName{k}[] = "
               f"{{{', '.join(f'0x{x:02X}' for x in species[si]['text'])}}};")
   L.append("const FormData kForms[] = {")
   for k, (si, f) in enumerate(forms):
      sp = species[si]
      L.append(f"    {{{{kFormSpeciesName{k}, battle::kLearn{si}, AssetKey{{0x{sp['key']:08X}u}}, "
               f"AssetKey{{0x{f['front']:08X}u}}, AssetKey{{0x{f['back']:08X}u}}, "
               f"{{{', '.join(str(v) for v in f['base'])}}}, {{{', '.join(str(v) for v in f['types'])}}}, "
               f"{{{', '.join(str(v) for v in f['passives'])}}}, {sp['growth']}, {sp['exp_yield']}, "
               f"{len(sp['learnset'])}, 0}}, 0x{NONE16 if f['rule'] is None else f['rule']:04X}, {f['name_id']}, "
               f"0x{f['triggers']:02X}}},   // {sp['name']} {f['name']}")
   if not forms:
      L.append("    {{nullptr, nullptr, AssetKey{0}, AssetKey{0}, AssetKey{0}, {0}, {0}, {0}, 0, 0, 0, 0}, 0, 0, 0},")
   L.append("};")
   # SpeciesGrowth {first transform, first form, base rule, transform count, form count, base triggers}
   L.append("const SpeciesGrowth kSpeciesGrowth[] = {")
   ft = ff = 0
   for sg in g["species"]:
      base = NONE16 if sg["base_rule"] is None else sg["base_rule"]
      L.append(f"    {{{ft}, {ff}, 0x{base:04X}, {len(sg['transforms'])}, {len(sg['forms'])}, "
               f"0x{sg['base_triggers']:02X}, {{0, 0, 0}}}},   // {sg['name']}")
      ft += len(sg["transforms"])
      ff += len(sg["forms"])
   if not g["species"]:
      L.append("    {0, 0, 0xFFFF, 0, 0, 0, {0, 0, 0}},")
   L.append("};")
   # ContextValue {default, lo, hi, source}
   ctx = g["transformation"]["contexts"]
   L.append("const ContextValue kContexts[] = {")
   for cv in ctx:
      L.append(f"    {{{cv['default']}, {cv['range'][0]}, {cv['range'][1]}, "
               f"static_cast<ContextSource>({cv['source']}), {{0, 0, 0}}}},   // {cv['name']}")
   if not ctx:
      L.append("    {0, 0, 0, ContextSource::Game, {0, 0, 0}},")
   L.append("};")
   L += ["", "} // namespace", ""]
   tr = g["transformation"]
   role = "monster::kNoRole" if tr["form_role"] is None else f"monster::RoleId{{0x{tr['form_role']:02X}}}"
   engine, custom = role_set(c["layout"], g["rederive"])
   if tg is None:
      training = "{0, 0, {0}, 0, 0xFF}"
   else:
      yields = ", ".join(str(y) for y in tg["yields"])
      point = c["pipelines"]["points"][tg["point"]]["id"] if tg.get("point") else NONE8
      training = f"{{{tg['stat_cap']}, {tg['total_cap']}, {{{yields}}}, 1, 0x{point:02X}}}"
   blocked = NONE16 if g["blocked"] is None else g["blocked"]
   L += [f"static_assert({len(ctx)} <= kMaxContextValues, \"more context values than the engine holds\");",
         "const GrowthData kGrowth = {",
         f"    kTransforms, kForms, kSpeciesGrowth, kContexts, {len(transforms)}, {len(forms)}, "
         f"{len(g['species'])}, 0x{blocked:04X},",
         f"    {{0x{engine:08X}u, 0x{custom:08X}u}}, {role}, {len(ctx)}, 0x{tr['cancel']:02X}, "
         f"{1 if tr['learn'] else 0},",
         f"    {training}",
         "};", "", "} // namespace capsule::growth", ""]
   return L


def growth_ids(c: Mapping[str, Any], src_name: str) -> list[str]:
   """!battle_ids.h constants: TRIGGER_<NAME>, CONTEXT_<NAME> (and CONTEXT_<NAME>_<VALUE> for named values),
   FORM_<NAME> (form name ids; FORM_BASE = 0).
   @param c         compile_battle() result
   @param src_name  manifest name (messages)
   @return lines
   """
   g = c["transform"]
   L = []
   seen = set()

   def const(idn: str, ty: str, v: int) -> None:
      """!One constant (an identifier given twice is an error)."""
      if idn in seen:
         raise ManifestError(f"{src_name}: two growth names give the identifier {idn}")
      seen.add(idn)
      L.append(f"inline constexpr {ty} {idn} = {v};")

   for i, t in enumerate(TRIGGERS):
      const(f"TRIGGER_{ident(t)}", "uint8_t", i)
   L.append(f"inline constexpr int NUM_TRIGGERS = {len(TRIGGERS)};")
   for k, cv in enumerate(g["transformation"]["contexts"]):
      const(f"CONTEXT_{ident(cv['name'])}", "uint8_t", k)
      for i, v in enumerate(cv["values"]):
         const(f"CONTEXT_{ident(cv['name'])}_{ident(v)}", "int32_t", i)
   L.append(f"inline constexpr int NUM_CONTEXTS = {len(g['transformation']['contexts'])};")
   for i, name in enumerate(g["form_names"]):
      const(f"FORM_{ident(name)}", "uint8_t", i)
   L.append(f"inline constexpr int NUM_FORM_NAMES = {len(g['form_names'])};")
   L.append("")
   return L


# -- Python mirrors (tools, the studio's previews) ---------------------------------------------------------------

def form_view(c: Mapping[str, Any], species_index: int, form: int) -> Mapping[str, Any]:
   """!A species as one of its forms gives it (growth::species_view()): base stats, types, passives, sprites.
   @param c              compile_battle() result
   @param species_index  species index (key order)
   @param form           form index (0: the species itself; one past its forms: the species too)
   @return a dict with "base", "types", "passives", "front", "back" (and the species' other keys)
   """
   sp = c["species"][species_index]
   forms = c["transform"]["species"][species_index]["forms"]
   if 1 <= form <= len(forms):
      return {**sp, **{k: forms[form - 1][k] for k in ("base", "types", "passives", "front", "back")}}
   return sp


def record_stats(c: Mapping[str, Any], record: Mapping[str, Any]) -> list[int]:
   """!A record's permanent stats as battle::record_stats() computes them (its form's base stats).
   @param c       compile_battle() result
   @param record  record dict (tools/record_transfer.py unpack() form; roles left out read 0)
   @return one stat per stat
   """
   si = species_index(c, record)
   sp = form_view(c, si, form_of(c, record))
   stat_names = [st["name"] for st in c["stats"]]

   def per_stat(role: str) -> list[int]:
      """!A per-stat role's values in stat order."""
      v = record.get(role, {})
      return [int(v.get(s, 0)) if isinstance(v, dict) else 0 for s in stat_names]
   genes, training, awaken = per_stat("stat_genes"), per_stat("stat_training"), per_stat("stat_awaken")
   trait = int(record.get("trait", 0))
   pct = c["traits"][trait]["pct"] if trait < len(c["traits"]) else [PERCENT] * len(stat_names)
   level = max(1, min(int(record.get("level", 1)), c["rules"]["level_max"]))
   return [max(1, min(U16_MAX, rc.run(c["ruleset"], st["formula"], base=sp["base"][i], gene=genes[i],
                                      training=training[i], awaken=awaken[i], level=level, trait_pct=pct[i])))
           for i, st in enumerate(c["stats"])]


def species_index(c: Mapping[str, Any], record: Mapping[str, Any]) -> int:
   """!A record's species index.
   @param c       compile_battle() result
   @param record  record dict ("species": name, or "species_key")
   @return index (key order)
   @throws KeyError for an unknown species
   """
   if "species" in record:
      return [sp["name"] for sp in c["species"]].index(record["species"])
   key = int(record.get("species_key", 0))
   return next(i for i, sp in enumerate(c["species"]) if sp["key"] == key)


def form_of(c: Mapping[str, Any], record: Mapping[str, Any]) -> int:
   """!A record's form (its form role's value; 0 without one).
   @param c       compile_battle() result
   @param record  record dict
   @return form index
   """
   name = c["transform"]["transformation"]["form_role_name"]
   if name is None:
      return 0
   if name in record:
      return int(record[name])
   from .progression import derived_values
   return int(derived_values(c, record).get(name, 0))


def when_values(c: Mapping[str, Any], record: Mapping[str, Any], trigger: str, item: str | None = None,
                context: Mapping[str, Any] | None = None) -> dict[str, int]:
   """!Every input a `when` expression reads, for a record and a trigger context.
   @param c        compile_battle() result
   @param record   record dict (roles left out read their derived value or 0)
   @param trigger  a TRIGGERS name
   @param item     the context's item name (item_use / trade), or None
   @param context   context value name -> value (a number or one of its names); the others take their defaults
   @return input name -> value
   """
   from .progression import derived_values
   g = c["transform"]
   scope = g["scope"]
   derived = derived_values(c, record)
   full = {**derived, **record}
   values: dict[str, int] = {}
   for role, parts in scope.roles.items():
      v = full.get(role, 0)
      for nm, part in parts:
         if part is None:
            values[nm] = int(v)
         elif part == "hi":
            values[nm] = (int(v) >> HALF_BITS) & HALF_MASK
         elif part == "lo":
            values[nm] = int(v) & HALF_MASK
         else:
            values[nm] = int(v.get(part, 0)) if isinstance(v, dict) else 0
   for nm, st in zip([STAT_PREFIX + s["name"] for s in c["stats"]], record_stats(c, record)):
      values[nm] = st
   sp = c["species"][species_index(c, record)]
   for f in c.get("species_fields", []):
      values[SPECIES_PREFIX + f["name"]] = sp["fields"][f["name"]]
   values[TRIGGER_INPUT] = TRIGGERS.index(trigger)
   items = c["names"]["items"]
   values[ITEM_INPUT] = items.index(item) + 1 if item is not None and trigger in ITEM_TRIGGERS else 0
   context = context or {}
   for cv in g["transformation"]["contexts"]:
      v = context.get(cv["name"], cv["default"])
      values[cv["name"]] = cv["values"].index(v) if isinstance(v, str) else int(v)
   return values


def holds(c: Mapping[str, Any], rule: int, values: Mapping[str, int]) -> bool:
   """!Whether a rule holds for when_values() (as rules::growth_when()).
   @param c       compile_battle() result
   @param rule    rule id
   @param values  when_values() result
   @return true when its expression is not 0
   """
   r = c["transform"]["rules"][rule]
   return rc.evaluate(r["tree"], values, c["transform"]["scope"].constants) != 0


def find_change(c: Mapping[str, Any], record: Mapping[str, Any], trigger: str, item: str | None = None,
                context: Mapping[str, Any] | None = None) -> dict[str, Any] | None:
   """!Which transformation or form change fires for a record in a trigger context (growth::find()): the blocked
   expression first, then the species' rules in order (the first whose triggers include this one and whose `when`
   holds), then its forms with rules (a form other than the record's).
   @param c        compile_battle() result
   @param record   record dict
   @param trigger  a TRIGGERS name
   @param item     the context's item name, or None
   @param context  context values (when_values())
   @return {"kind": "species", "into", "also", "rule", "takes_item"} or {"kind": "form", "form", "name", "rule"},
           {"kind": "blocked", "rule"} when the blocked expression holds, or None
   """
   g = c["transform"]
   values = when_values(c, record, trigger, item, context)
   bit = 1 << TRIGGERS.index(trigger)
   if g["blocked"] is not None and holds(c, g["blocked"], values):
      return {"kind": "blocked", "rule": g["blocked"]}
   sg = g["species"][species_index(c, record)]
   for t in sg["transforms"]:
      if t["triggers"] & bit and holds(c, t["rule"], values):
         return {"kind": "species", "into": t["into_name"], "also": t["also_name"], "rule": t["rule"],
                 "takes_item": bool(t["flags"] & FLAG_TAKES_ITEM)}
   current = form_of(c, record)
   for k, f in enumerate(sg["forms"]):
      if f["rule"] is not None and f["triggers"] & bit and k + 1 != current and holds(c, f["rule"], values):
         return {"kind": "form", "form": k + 1, "name": f["name"], "rule": f["rule"]}
   if current != 0 and sg["base_rule"] is not None and sg["base_triggers"] & bit and holds(c, sg["base_rule"], values):
      return {"kind": "form", "form": 0, "name": BASE_FORM, "rule": sg["base_rule"]}
   return None


def training_gain(c: Mapping[str, Any], training: Sequence[int], foe: str, level: int) -> list[int]:
   """!The training values after defeating one foe (growth's gain: each stat in order, the total cap first, then the
   stat cap; a stat whose yield is 0 gains nothing).
   @param c         compile_battle() result
   @param training  the receiver's training values, one per stat
   @param foe       the defeated foe's species name
   @param level     the receiver's level
   @return the new values
   """
   tg = c["transform"]["training"]
   out = list(training)
   if tg is None:
      return out
   sp = next(s for s in c["species"] if s["name"] == foe)
   fields = [f["name"] for f in c.get("species_fields", [])]
   formula = rc.named_formulas(c["ruleset"])[tg["gain"]]
   total = sum(out)
   for k, field in enumerate(tg["yields"]):
      if field == NONE8:
         continue
      y = sp["fields"][fields[field]]
      if y == 0:
         continue
      args = dict(zip(formula["inputs"], (y, out[k], total, level)))
      gain = max(formula["range"][0], min(formula["range"][1], rc.evaluate(formula["tree"], args,
                                                                         formula["constants"])))
      gain = min(gain, tg["total_cap"] - total, tg["stat_cap"] - out[k])
      if gain <= 0:
         continue
      out[k] += gain
      total += gain
   return out
