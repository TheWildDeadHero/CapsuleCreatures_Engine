"""!Pipelines and points of the battle compiler (D-20 D, D-23 section 1.1):
binds the ruleset's pipelines to the battle (its tables, caps and the ranges
of what the engine passes), compiles point handlers (the pure subset of the
effect language, tools/effect_compiler.py) and runs points in Python as the
engine does (point_runner(), for the studio's trace and the tests).

Point handlers are the `point NAME [when EXPR] { ... }` members of a
passive's, item's, status's or weather's `.ceffect` owner (E13b: also a
move's, a volatile's and a side condition's), or the same text
in its manifest entry's "handlers": {"point": NAME, "code": "[when EXPR] {
modify by 1.5; ... }"} (an entry's manifest handlers come before its
`.ceffect` ones). Handlers read the holder (whose passive, item or status it is; the user for a
weather), other (the pipeline's other battler), user, target, the move and
the field; names resolve in the battle's lists (statuses and items by their
manifest index).
"""
from __future__ import annotations

import json
from collections.abc import Callable, Mapping, Sequence
from typing import Any

import effect_compiler as ec
import ruleset_compiler as rc

from .common import NONE8, NONE16, Json, Lookups, ManifestError, check_keys, register_keys
from .chart import chart_battle_values
from .conditions import MOVE_FLAGS, VOLATILES
from .stats import is_damaging, stage_battle_values

## Hook number of point 0's handlers (capsule::battle::kPointBase).
POINT_BASE = 16
## Source kind of each owner list (a point names its sources by kind; E13b: moves, volatiles, side conditions).
OWNER_SOURCES = {"passives": "passive", "items": "item", "statuses": "status", "weathers": "weather", "moves": "move",
                 "volatiles": "volatile", "side_conditions": "side"}
## Owner kind words of the effect language, by owner list (labels of generated handlers).
OWNER_KINDS = {"passives": "passive", "items": "item", "statuses": "status", "weathers": "weather", "moves": "move",
               "volatiles": "volatile", "side_conditions": "side_condition"}
## The owner lists whose manifest entries take point handlers ("handlers": [{"point", "code"}]; X1: the studio's
## list): the others' point handlers are only `point NAME` members of their `.ceffect` logic (compile_logic_points()).
MANIFEST_HANDLER_LISTS = ("passives", "items", "statuses", "weathers")
## Engine index of the first status / item of the manifest lists (0 is "none").
FIRST_LISTED = 1
register_keys({
   "point_handler": {"point", "code"},
})


# -- binding the ruleset ---------------------------------------------------------------------------------------------

def hull(ranges: Sequence[tuple[int, int]], default: tuple[int, int]) -> tuple[int, int]:
   """!The smallest range holding every given range.
   @param ranges   ranges
   @param default  result when there are none
   @return (lo, hi)
   """
   if not ranges:
      return default
   return (min(r[0] for r in ranges), max(r[1] for r in ranges))


def stage_ids(stats: Sequence[Mapping[str, Any]], indices: Sequence[int]) -> tuple[int, int]:
   """!The range of stage ids the engine passes for some stats (NONE8 for a stat without a stage).
   @param stats    compiled stats
   @param indices  stat indices
   @return (lo, hi)
   """
   ids = [stats[i]["stage"] for i in indices] or [NONE8]
   return (min(ids), max(ids))


def bind_pipelines(ruleset: Mapping[str, Any], rs_name: str, rules: Mapping[str, Any],
                   categories: Sequence[Mapping[str, int]], stats: Sequence[Mapping[str, Any]],
                   moves: Sequence[Mapping[str, Any]], stages: Sequence[Mapping[str, Any]],
                   chart: Mapping[str, Any], recruit: Mapping[str, Any] | None = None) -> dict[str, Any]:
   """!Analyses the ruleset's pipelines against this battle: its stages (ranges, tables, formulas, crit odds), its
   type chart (tiers, combine mode, types per battler), caps and the ranges of what the engine passes (stats from
   their analysed bounds, the moves' power, accuracy and crit stages, the stage ids of the stats the categories
   use). Also checks that escape_odds takes every speed the speed pipeline gives.
   @param ruleset     compiled ruleset (ruleset_compiler.load)
   @param rs_name     ruleset file name (messages)
   @param rules       compiled rules
   @param categories  compiled categories
   @param stats       compiled stats (with "bounds", check_formula_ranges(); "stage" set by compile_stages())
   @param moves       compiled moves ("formula": the move deals formula damage)
   @param stages      compiled stages (stats.compile_stages())
   @param chart       compiled chart (chart.compile_chart())
   @param recruit     compiled recruit section (battlec/recruit.py; G3: the recruit_odds pipeline's recruit_rate
                      and status_bonus ranges), or None
   @return the bound ruleset (ruleset_compiler.bind)
   """
   formula_moves = [m for m in moves if m["formula"]]
   damaging = [k for k in categories if k["offense"] != NONE8]
   conf = categories[rules["self_hit_category"]]
   power = (rules["self_hit_power"], rules["self_hit_power"])
   order = next(i for i, st in enumerate(stats) if st["order"])
   odds_stat = next((st["bounds"] for st in stats if st.get("odds")), (0, 0))
   inputs = {
      "damage": {"move_power": hull([(m["power"], m["power"]) for m in formula_moves], (1, 1)),
                 "offense_raw": hull([stats[k["offense"]]["bounds"] for k in damaging], (1, 1)),
                 "defense_raw": hull([stats[k["defense"]]["bounds"] for k in damaging], (1, 1)),
                 "offense_stage_id": stage_ids(stats, [k["offense"] for k in damaging]),
                 "defense_stage_id": stage_ids(stats, [k["defense"] for k in damaging])},
      "self_hit": {"move_power": power, "offense_raw": stats[conf["offense"]]["bounds"],
                   "defense_raw": stats[conf["defense"]]["bounds"],
                   "offense_stage_id": stage_ids(stats, [conf["offense"]]),
                   "defense_stage_id": stage_ids(stats, [conf["defense"]])},
      "speed": {"speed_raw": stats[order]["bounds"], "speed_stage_id": stage_ids(stats, [order])},
      "accuracy": {"move_accuracy": hull([(m["accuracy"], m["accuracy"]) for m in moves if m["accuracy"]], (1, 1))},
      "crit": {"move_crit_stage": hull([(m["crit_stage"], m["crit_stage"]) for m in formula_moves], (0, 0))},
      # E9: the odds stat of both battlers (0 without one)
      "effect_odds": {"user_stat": odds_stat, "target_stat": odds_stat},
      # G3: the species field read as the recruit rate, the status bonuses the section gives
      "recruit_odds": recruit_inputs(recruit),
   }
   battle = {"stat_cap": rules["stat_cap"], "level_max": rules["level_max"], **stage_battle_values(stages),
             "chart": chart_battle_values(chart), "inputs": inputs}
   try:
      bound = rc.bind(ruleset, battle, rs_name)
   except rc.RulesetError as e:
      raise ManifestError(str(e))
   speed = bound["pipelines"]["speed"]["ranges"]["speed"]
   for key in ("speed", "foe_speed"):
      have = ruleset["inputs"].get(key)
      if have is None or have[0] > speed[0] or have[1] < speed[1]:
         raise ManifestError(f"{rs_name}: input {key!r} must cover [{speed[0]}, {speed[1]}] (the speed pipeline's "
                             f"output, which escape_odds is given)" + (f"; it declares {list(have)}" if have else ""))
   return bound


def recruit_inputs(recruit: Mapping[str, Any] | None) -> dict[str, tuple[int, int]]:
   """!The ranges this battle passes the recruit_odds pipeline (G3): its species field's range as recruit_rate and its
   status bonuses' as status_bonus (0 without a recruit section).
   @param recruit  compiled recruit section, or None
   @return {input: (lo, hi)}
   """
   if recruit is None:
      return {"recruit_rate": (0, 0), "status_bonus": (0, 0)}
   return {"recruit_rate": tuple(recruit["rate_range"]), "status_bonus": tuple(recruit["bonus_range"])}


# -- point handlers --------------------------------------------------------------------------------------------------

def catalog(names: Mapping[str, Sequence[str]], stages: Lookups) -> ec.Catalog:
   """!The names point handlers may use, with the ids the engine's point_get / point_has give them.
   @param names   the battle's names by list (compile_battle's "names"; without move_flags, volatiles, side and field
                  conditions or tags: the engine's former move flags and volatiles, no others)
   @param stages  stage lookup (stage name -> stage index)
   @return the catalog
   """
   stage_names = [n for n, _i in sorted(stages.index.items(), key=lambda kv: kv[1])]
   return ec.Catalog({
      "types": list(names["types"]), "categories": list(names["categories"]), "statuses": list(names["statuses"]),
      "weathers": list(names["weathers"]), "moves": list(names["moves"]), "items": list(names["items"]),
      "passives": list(names["passives"]), "species": list(names["species"]), "stats": list(names["stats"]),
      "stages": stage_names, "move_flags": list(names.get("move_flags", MOVE_FLAGS)),
      "volatiles": list(names.get("volatiles", VOLATILES)), "side_conditions": list(names.get("side_conditions", [])),
      "field_conditions": list(names.get("field_conditions", [])), "tags": list(names.get("tags", [])),
      # E13b: point handlers read species fields too (point_get: rules::species_field_of())
      "species_fields": list(names.get("species_fields", [])),
   })


def quoted(name: str) -> str:
   """!A name as an effect-language string.
   @param name  the name
   @return "name" (escaped)
   """
   return json.dumps(name)


class PointHandlers:
   """!The point handlers of one battle: their code, ids (generated functions) and the handler table entries.

   @var entries
   Compiled handlers in function order: {"label", "point", "node", "ir", "owner"}.
   """

   def __init__(self, points: Mapping[str, Any], cat: ec.Catalog) -> None:
      """!Starts an empty set.
      @param points  the bound ruleset's points
      @param cat     names the handlers use (catalog())
      """
      ## Point name -> {"id", "sources", ...}.
      self.points = points
      ## Names the handlers use.
      self.cat = cat
      ## Compiled handlers in function order.
      self.entries: list[dict[str, Any]] = []

   def add(self, owner_list: str, owner: str, point: str, code: str, where: str) -> dict[str, int]:
      """!Compiles one point handler.
      @param owner_list  an OWNER_SOURCES key ("passives", "items", "statuses", "weathers", "moves", ...)
      @param owner       the owner's name
      @param point       point name
      @param code        effect-language text after `point NAME`
      @param where       location
      @return the handler table entry {"hook", "count", "first"}
      """
      text = f"{OWNER_KINDS[owner_list]} {quoted(owner)} {{\n   point {point} {code}\n}}\n"
      try:
         node = ec.parse(text, where)["owners"][0]["handlers"][0]
      except ec.EffectError as e:
         raise ManifestError(f"{where}: {e}")
      if "point" not in node:
         raise ManifestError(f"{where}: not a point handler: {code!r}")
      return self.add_node(owner_list, owner, node, where)

   def add_node(self, owner_list: str, owner: str, node: Mapping[str, Any], where: str) -> dict[str, int]:
      """!Compiles one point handler given as its AST node (a `.ceffect` owner's `point` member).
      @param owner_list  an OWNER_SOURCES key
      @param owner       the owner's name
      @param node        the handler node ({"point", "when"?, "body"})
      @param where       location
      @return the handler table entry {"hook", "count", "first"}
      """
      point = node["point"]
      if point not in self.points:
         raise ManifestError(f"{where}: unknown point {point!r} (the ruleset's points: "
                             f"{', '.join(self.points) or 'none'})")
      kind = OWNER_SOURCES[owner_list]
      sources = self.points[point]["sources"]
      if not any(s == kind or s.endswith(f".{kind}") for s in sources):
         raise ManifestError(f"{where}: point {point} has no {kind} source (its sources: "
                             f"{', '.join(sources) or 'none'}), so this handler would never run")
      try:
         ir = ec.pure_ir(node, self.cat)
      except ec.EffectError as e:
         raise ManifestError(f"{where}: {e}")
      self.entries.append({"label": f"{OWNER_KINDS[owner_list]} {owner}: point {point}", "point": point,
                           "node": node, "ir": ir, "owner": (owner_list, owner)})
      return {"hook": POINT_BASE + self.points[point]["id"], "count": 0, "first": len(self.entries) - 1}

   def handler(self, owner_list: str, owner: str, h: Json, where: str) -> list[dict[str, int]]:
      """!The handler table entries of one manifest point handler ({"point": NAME, "code": text}).
      @param owner_list  owner list
      @param owner       owner name
      @param h           the handler object
      @param where       location
      @return entries
      """
      if not isinstance(h, dict) or "point" not in h:
         raise ManifestError(f"{where}: a manifest handler is a point handler {{\"point\": name, \"code\": text}} "
                             f"(other logic belongs in the owner's .ceffect source)")
      check_keys(h, "point_handler", where)
      point = h["point"]
      if not isinstance(point, str):
         raise ManifestError(f"{where}: a point handler names its point")
      if not isinstance(h.get("code"), str):
         raise ManifestError(f"{where}.code must be effect-language text (\"[when EXPR] {{ modify by 1.5; }}\")")
      return [self.add(owner_list, owner, point, h["code"], where)]


def compile_condition_handlers(entries: Sequence[Json], owner_list: str, compiled: list[dict[str, Any] | None],
                               ph: PointHandlers, handlers: list[dict[str, int]], where: str,
                               logic_points: Mapping[tuple[str, str], Sequence[Json]] | None = None) -> None:
   """!The point handlers of the statuses or weathers: their manifest "handlers", then the points of their
   `.ceffect` logic. Sets each compiled entry's first_handler, handler_count and point_mask.
   @param entries       the manifest's statuses or weathers
   @param owner_list    "statuses" or "weathers"
   @param compiled      compiled statuses (index 0 = none) or weathers
   @param ph            the battle's point handlers
   @param handlers      the handler table (appended)
   @param where         location
   @param logic_points  `.ceffect` point members by (kind, name)
   @return None
   """
   offset = FIRST_LISTED if owner_list == "statuses" else 0
   for i, e in enumerate(entries):
      w = f"{where}: {owner_list}[{i}] ({e.get('name')})"
      first = len(handlers)
      hs = e.get("handlers", [])
      if not isinstance(hs, list):
         raise ManifestError(f"{w}.handlers must be a list")
      for j, h in enumerate(hs):
         handlers += ph.handler(owner_list, e["name"], h, f"{w}.handlers[{j}]")
      for node in (logic_points or {}).get((OWNER_KINDS[owner_list], e["name"]), []):
         handlers.append(ph.add_node(owner_list, e["name"], node, f"{w}: .ceffect point {node['point']}"))
      own = handlers[first:]
      c = compiled[i + offset]
      c["first_handler"] = first if own else 0
      c["handler_count"] = len(own)
      c["point_mask"] = 0
      for h in own:
         c["point_mask"] |= 1 << (h["hook"] - POINT_BASE)


def compile_logic_points(compiled: Sequence[dict[str, Any]], owner_list: str, names: Sequence[str],
                         ph: PointHandlers, handlers: list[dict[str, int]], where: str,
                         logic_points: Mapping[tuple[str, str], Sequence[Json]]) -> None:
   """!The point handlers of the moves, volatiles or side conditions (E13b): the point members of their `.ceffect`
   logic (they have no manifest "handlers"). Sets each compiled entry's first_handler and handler_count (the owner's
   point handlers, battlec/logic.py compile_logic() adds them to its owner) and point_mask.
   @param compiled      compiled moves, volatile records or side condition records
   @param owner_list    "moves", "volatiles" or "side_conditions"
   @param names         their names, in the same order
   @param ph            the battle's point handlers
   @param handlers      the handler table (appended)
   @param where         location
   @param logic_points  `.ceffect` point members by (kind, name)
   @return None
   """
   kind = OWNER_KINDS[owner_list]
   for e, name in zip(compiled, names):
      first = len(handlers)
      for node in logic_points.get((kind, name), []):
         handlers.append(ph.add_node(owner_list, name, node, f"{where}: {kind} {name}: .ceffect point {node['point']}"))
      own = handlers[first:]
      e["first_handler"] = first if own else 0
      e["handler_count"] = len(own)
      e["point_mask"] = 0
      for h in own:
         e["point_mask"] |= 1 << (h["hook"] - POINT_BASE)


# -- running points in Python (the engine's run_point and point_get) -------------------------------------------------

## One battler as point handlers read it: hp, max_hp, level, stats, stages, status (engine index, 0 = none),
## types, passive, item (engine index, 0 = none), species, last_move, position, side, party, volatiles (bits),
## side_conditions (bits of the conditions on its side), side_active (battlers active on its side), genes (per stat;
## E13b).
BattlerView = Mapping[str, Any]


def point_runner(c: Mapping[str, Any], user: BattlerView | None, target: BattlerView | None,
                 move: int | None = None, weather: int = NONE8, crit: int = 0,
                 turn: int = 1, move_type: int | None = None) -> Callable[[str, int], int | None]:
   """!The point callback of ruleset_compiler.run_pipeline() for one situation: runs each source's handlers in
   order (every handler sees the value clamped to [0, 65535]) with effect_compiler.run_pure, reading the
   battlers as the engine's point_get / point_has do; None when no source has a handler (the value stays).
   @param c          from compile_battle()
   @param user       the user, or None
   @param target     the target, or None
   @param move       move index, or None
   @param weather    weather index, or NONE8
   @param crit       1 for a critical hit
   @param turn       turn number
   @param move_type  the type the hit has (move_type_of(); the engine's PipelineCtx.move_type: handlers' move.type
                     reads it, and move.category the category it gives, category_of()), or None for the move's own
   @return callback (point name, value) -> value or None
   """
   handlers = c["handlers"]
   entries = c["point_handlers"]
   points = c["pipelines"]["points"]
   mv = c["moves"][move] if move is not None else None
   # the hit's type (and the category it gives, C5) replace the move's own where handlers read them (X1: no copy of
   # the compiled data)
   if mv is not None and move is not None and move_type is not None:
      mv = {**mv, "type": move_type, "category": category_of(c, move, move_type)}

   def owner_range(b: BattlerView, kind: str) -> tuple[int, int]:
      """!(first, count) of a battler's passive, item or status handlers."""
      if kind == "passive":
         p = b.get("passive", NONE8)
         return (c["passives"][p]["first"], c["passives"][p]["count"]) if p < len(c["passives"]) else (0, 0)
      if kind == "item":
         it = b.get("item", 0)
         return (c["items"][it]["first"], c["items"][it]["count"]) if 0 < it < len(c["items"]) else (0, 0)
      st = b.get("status", 0)
      if 0 < st < len(c["statuses"]):
         return c["statuses"][st]["first_handler"], c["statuses"][st]["handler_count"]
      return (0, 0)

   def reader(holder: BattlerView | None, other: BattlerView | None) -> Callable[[str, int, int, int], int]:
      """!The read callback of one handler call (subjects resolved against its holder and other)."""
      def battler(subj: int) -> BattlerView | None:
         """!The battler a subject code names."""
         if subj & ec.SUBJ_SIDE:
            return None
         return {ec.SUBJ_HOLDER: holder, ec.SUBJ_OTHER: other, ec.SUBJ_USER: user,
                 ec.SUBJ_TARGET: target}.get(subj & ec.SUBJ_BASE_MASK)

      def read(kind: str, index: int, subj: int, arg: int) -> int:
         """!point_get / point_has."""
         if kind == "has":
            what = ec.HAS_KINDS[index]
            if what == "move_flag":
               return int(mv is not None and (mv["flags"] >> arg) & 1 == 1)
            # a side condition takes a battler or a side subject (the battler's side)
            b = battler(subj & ~ec.SUBJ_SIDE if what == "side_cond" else subj)
            if b is None:
               return 0
            if what == "status":
               return int(b.get("status", 0) == arg + FIRST_LISTED)
            if what == "volatile":
               return int((b.get("volatiles", 0) >> arg) & 1 == 1)
            if what == "type":
               return int(arg in b.get("types", []))
            if what == "side_cond":
               return int((b.get("side_conditions", 0) >> arg) & 1 == 1)
            return 0
         ref = ec.REFS[index]
         move_fields = {"move_type": "type", "move_category": "category", "move_power": "power",
                        "move_accuracy": "accuracy", "move_priority": "priority"}
         resources = c.get("resources", [])
         if ref == "move_base":
            # a move resource's base (by resource index; a battler resource has none)
            r = resources[arg] if arg < len(resources) else None
            ok = mv is not None and r is not None and r["scope_name"] == "move"
            return mv["base"][r["slot"]] if ok else 0
         if ref == "move_cost":
            # the move's fixed cost of a resource (0 for none or a formula cost)
            return next((k["amount"] for k in mv["cost"] if k["resource"] == arg), 0) if mv is not None else 0
         if ref in move_fields:
            none = NONE8 if ref in ("move_type", "move_category") else 0
            return mv[move_fields[ref]] if mv is not None else none
         globals_ = {"weather_id": weather, "weather_turns": 0, "turn": turn, "crit": crit,
                     "move": move if move is not None else NONE16}
         if ref in globals_:
            return globals_[ref]
         # `side(SUBJECT).active_count` reads the subject battler's side (E13b)
         b = battler(subj & ~ec.SUBJ_SIDE if ref == "side_active" else subj)
         if ref == "battler":
            return b.get("index", NONE8) if b is not None else NONE8
         if b is None:
            return NONE8 if ref in ("status", "passive", "type") else NONE16 if ref in ("item", "species",
                                                                                       "last_move") else 0
         if ref == "status":
            return b["status"] - FIRST_LISTED if b.get("status", 0) else NONE8
         if ref == "item":
            return b["item"] - FIRST_LISTED if b.get("item", 0) else NONE16
         if ref == "stat":
            return b["stats"][arg] if arg < len(b["stats"]) else 0
         if ref == "stage":
            stages = b.get("stages", [])
            return stages[arg] if arg < len(stages) else 0
         if ref == "gene":
            genes = b.get("genes", [])
            return genes[arg] if arg < len(genes) else 0
         if ref == "type":
            types = list(b.get("types", [])) + [NONE8] * rc.MAX_TYPES_PER
            return types[arg] if arg < rc.MAX_TYPES_PER else NONE8
         if ref == "alive":
            return int(b["hp"] > 0)
         return b.get(ref, 0)
      return read

   def bit_ranges(bits: int, entries_: Sequence[Mapping[str, Any]]) -> list[tuple[int, int]]:
      """!(first, count) of each entry whose bit is set, in data order (volatiles, side conditions)."""
      return [(e.get("first_handler", 0), e.get("handler_count", 0)) for k, e in enumerate(entries_)
              if (bits >> k) & 1]

   def run(name: str, v: int) -> int | None:
      """!Runs point @p name on @p v."""
      p = points[name]
      hook = POINT_BASE + p["id"]
      calls = []
      for src in p["sources"]:
         ranges: list[tuple[int, int]] = []
         if src in ("weather", "move"):
            holder, other = user, target
            if holder is None and src == "move":
               continue
            if src == "move":
               if mv is None:
                  continue
               ranges = [(mv.get("first_handler", 0), mv.get("handler_count", 0))]
            else:
               if weather == NONE8 or weather >= len(c["weathers"]):
                  continue
               w = c["weathers"][weather]
               ranges = [(w["first_handler"], w["handler_count"])]
         else:
            side, kind = src.split(".")
            holder, other = (user, target) if side == "user" else (target, user)
            if holder is None:
               continue
            if kind == "volatile":
               ranges = bit_ranges(holder.get("volatiles", 0), c["volatiles"])
            elif kind == "side":
               ranges = bit_ranges(holder.get("side_conditions", 0), c["side_conditions"])
            else:
               ranges = [owner_range(holder, kind)]
         for first, count in ranges:
            for h in handlers[first:first + count]:
               if h["hook"] == hook:
                  calls.append((entries[h["first"]]["ir"], holder, other))
      if not calls:
         return None
      for ir, holder, other in calls:
         v = ec.run_pure(ir, max(rc.POINT_VALUE_RANGE[0], min(rc.POINT_VALUE_RANGE[1], v)), reader(holder, other))
      return v
   return run


def point_handled(c: Mapping[str, Any], name: str) -> bool:
   """!Whether any owner has a handler for a point (C5: the engine is not given a move_type point no handler can
   change, Rules::move_type_point kNone8, so no hit runs it).
   @param c     from compile_battle()
   @param name  point name
   @return True when the ruleset declares the point and some handler modifies it
   """
   p = c["pipelines"]["points"].get(name)
   return p is not None and any(h.get("hook") == POINT_BASE + p["id"] for h in c["handlers"])


def category_of(c: Mapping[str, Any], move: int, hit_type: int) -> int:
   """!The category a move hits with when its hit type is @p hit_type, as the engine's category_of() (damage.cpp,
   C5): the structure's hit_category_by_type (compile_battle()'s "hit_categories"; category_by_type gives it too)
   for a damaging move whose hit type is not its own type, else the move's category.
   @param c         from compile_battle()
   @param move      move index
   @param hit_type  the type it hits with (move_type_of())
   @return category index
   """
   mv = c["moves"][move]
   table = c.get("hit_categories")
   if table is None or hit_type == mv["type"] or not 0 <= hit_type < len(table) or \
         not is_damaging(c["categories"], mv["category"]):
      return int(mv["category"])
   return int(table[hit_type])


def move_type_of(c: Mapping[str, Any], run_point: Callable[[str, int], int | None], move: int) -> int:
   """!The type a move hits with, as the engine's move_type_of() computes it (damage.cpp, E13b): the ruleset's
   reserved move_type point (ruleset_compiler.MOVE_TYPE_POINT) run on the move's own type; a value that names no
   type keeps it.
   @param c          from compile_battle()
   @param run_point  point_runner() callback of the hit (no crit yet, as the engine's)
   @param move       move index
   @return type index (the move's own when the ruleset has no such point)
   """
   own = c["moves"][move]["type"]
   if rc.MOVE_TYPE_POINT not in c["pipelines"]["points"]:
      return own
   t = run_point(rc.MOVE_TYPE_POINT, own)
   return t if t is not None and 0 <= t < len(c["types"]) else own
