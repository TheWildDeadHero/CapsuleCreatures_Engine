"""!Resources of the battle compiler (D-25 decisions 1-5): every pool a battler
spends or loses is a manifest record of the structure manifest's
"resources" list, in the order costs are paid:

    {"name": "hp", "label": "HP", "scope": "battler", "stat": "hp", "empty": "faint", "persist": "current_hp"}
    {"name": "pp", "label": "PP", "scope": "move", "base": "pp", "max": "pp_max", "raises": "pp_ups",
     "persist": "pp"}
    {"name": "ep", "label": "EP", "scope": "battler", "stat": "ep", "regen": "ep_regen"}

- scope `battler`: one pool per battler; its maximum is the stat named by
  "stat" (an ordinary manifest stat). Exactly one battler resource has
  "empty": "faint" (an empty pool faints the battler): it is the pool stat's
  (hit points, Battler::hp); the others live in Battler::pool.
- scope `move`: one pool per move slot (Battler::move_pool); "base" names the
  move field every move states (an integer 0-255, e.g. "pp"); "max" names a
  ruleset formula of (base, raises) (named formula, two args in that order;
  without it the maximum is the base); "raises" names the record role holding
  each slot's raises (needs "max").
- "persist": the record role the pool is stored in and carried between
  battles (a battler resource: a one-element role, a move resource: one
  element per move slot; the registry's storage roles current_hp, pp and
  pp_ups or a custom role); without it the pool is refilled at the start of
  every battle. The role must hold the pool's highest value (check_persist_roles():
  a battler pool's stat's highest value, a move pool's max formula range or
  largest base), so a write-back never truncates the pool.
- "regen": an end-of-turn amount (a number within REGEN_RANGE, the engine's
  int16_t ResourceData::regen, or a ruleset formula of (value, max)), applied
  in the turn's "regen" end-of-turn phase.

A move's "cost" is {resource: amount}, the amount a number or a ruleset
formula of (value, max) of the user's pool; a move without "cost" pays 1 of
every move resource. A move whose cost cannot be paid is not selectable; a
cost no pool can ever pay is refused (check_costs(); a move resource's pool
is the move's own: its max formula over that move's base). A cost on the faint
resource (hit points) needs more than its amount: paying never faints the user
(the engine's can_pay()). rules.fallback_move pays nothing and states no cost.

The formulas' ids (the generated rules::resource_formula() dispatch) are the
distinct formula names in first-use order: the resources' max and regen
formulas, then the moves' cost formulas; at most MAX_FORMULAS of them.
"""
from __future__ import annotations

import re
from collections.abc import Mapping, Sequence
from typing import Any

import ruleset_compiler as rc

from .common import I16_MAX, NONE8, NONE16, U8_MAX, U16_MAX, Json, ManifestError, Texts, check_keys, integer, \
   register_keys
from .layout import CUSTOM_ROLE_BASE, ROLE_NAMES, role_info
from .turn import END_PHASES

## Resource scopes, in capsule::battle::ResourceScope order.
SCOPES = ["battler", "move"]
## What an empty pool means, in capsule::battle::ResourceEmpty order.
EMPTY_RULES = ["none", "faint"]
## Default of "empty".
DEFAULT_EMPTY = "none"
## "No role" (capsule::monster::kNoRole): a resource that is not persisted / has no raises.
NO_ROLE = 0xFF
## Highest value a pool holds per scope (Battler::pool is 16 bits, Battler::move_pool 8 bits).
STORAGE_MAX = {"battler": U16_MAX, "move": U8_MAX}
## A number regen's range: capsule::battle::ResourceData::regen is an int16_t (a formula's output may span a pool).
REGEN_RANGE = (-I16_MAX - 1, I16_MAX)
## Most resource formulas (ids 0 .. MAX_FORMULAS - 1; NONE8 is "no formula").
MAX_FORMULAS = NONE8
## Widest record role a pool is persisted in, per scope (the pool's storage).
STORAGE_ROLE_BITS = {"battler": 16, "move": 8}
## Registry roles that are plain storage a resource may name (no engine code reads them by name since D-25):
## role -> the scope its element count fits.
ENGINE_STORAGE_ROLES = {"current_hp": "battler", "pp": "move", "pp_ups": "move"}
## Argument order of each formula use (the engine passes them in this order).
FORMULA_ARGS = {"max": ("base", "raises"), "regen": ("value", "max"), "cost": ("value", "max")}
## Cost of a move without "cost": this many of every move resource.
DEFAULT_COST = 1
## The END_PHASES index of the regen phase (capsule::battle::EndPhase::Regen).
REGEN_PHASE = END_PHASES.index("regen")
## A resource's name: a lower-case identifier (effect logic and bench pokes name it).
RESOURCE_NAME = re.compile(r"[a-z][a-z0-9_]*\Z")
## Move keys a resource's "base" may not name (the move's own fields).
MOVE_FIELDS = {"name", "type", "category", "power", "accuracy", "priority", "target", "flags", "crit_stage", "cost",
               "editor"}

register_keys({
   "resource": {"name", "label", "scope", "stat", "base", "max", "raises", "persist", "empty", "regen"},
})


def _named_formula(name: Json, ruleset: Mapping[str, Any], rs_name: str, use: str, where: str) -> str:
   """!Checks that a resource formula names a named ruleset formula of two arguments.
   @param name     the manifest's value
   @param ruleset  compiled ruleset
   @param rs_name  ruleset file name (messages)
   @param use      max, regen or cost (FORMULA_ARGS)
   @param where    location
   @return the formula's name
   """
   formulas = rc.named_formulas(ruleset)
   args = FORMULA_ARGS[use]
   if not isinstance(name, str) or name not in formulas:
      raise ManifestError(f"{where}: names a formula of {rs_name} with \"args\" ({', '.join(args)}); "
                          f"its named formulas: {', '.join(formulas) or 'none'}")
   if len(formulas[name]["inputs"]) != len(args):
      raise ManifestError(f"{where}: {rs_name}'s {name} takes {len(formulas[name]['inputs'])} args; a {use} "
                          f"formula takes ({', '.join(args)})")
   return name


def _role(name: Json, layout: Mapping[str, Any], scope: str, what: str, where: str) -> int:
   """!A record role a resource is persisted in or reads its raises from.
   @param name    role name
   @param layout  compiled layout
   @param scope   the resource's scope (the role's element count must fit it)
   @param what    persist or raises (messages)
   @param where   location
   @return the role ID
   """
   if not isinstance(name, str):
      raise ManifestError(f"{where}.{what}: names a record role")
   info = role_info(layout, name)
   if info is None:
      raise ManifestError(f"{where}.{what}: the layout holds no role {name!r}")
   rid, width, count = info
   if rid < CUSTOM_ROLE_BASE and ENGINE_STORAGE_ROLES.get(ROLE_NAMES[rid]) != scope:
      fits = [r for r, s in ENGINE_STORAGE_ROLES.items() if s == scope]
      raise ManifestError(f"{where}.{what}: {name!r} is an engine role the engine fills itself; a {scope} resource "
                          f"names {', '.join(fits)} or a custom role")
   want = layout["move_slots"] if scope == "move" else 1
   if count != want:
      raise ManifestError(f"{where}.{what}: {name!r} holds {count} element(s); a {scope} resource needs {want} "
                          f"({'one per move slot' if scope == 'move' else 'one'})")
   if width > STORAGE_ROLE_BITS[scope]:
      raise ManifestError(f"{where}.{what}: {name!r} is {width} bits wide; a {scope} pool is stored in "
                          f"{STORAGE_ROLE_BITS[scope]}")
   return rid


def _amount(v: Json, ruleset: Mapping[str, Any], rs_name: str, use: str, lo: int, hi: int,
            where: str) -> tuple[int, str | None]:
   """!A regen or cost amount: a number or a named formula.
   @param v        the manifest's value
   @param ruleset  compiled ruleset
   @param rs_name  ruleset file name
   @param use      regen or cost
   @param lo       lowest number
   @param hi       highest number
   @param where    location
   @return (number (0 for a formula), formula name or None)
   """
   if isinstance(v, str):
      return 0, _named_formula(v, ruleset, rs_name, use, where)
   return integer(v, lo, hi, where), None


def compile_resources(entries: Json, stats: Sequence[Mapping[str, Any]], layout: Mapping[str, Any],
                      ruleset: Mapping[str, Any], rs_name: str, turn: Mapping[str, Any], limits: Mapping[str, int],
                      where: str, enc: Texts) -> list[dict[str, Any]]:
   """!The resources (see the module docstring).
   @param entries  the structure manifest's resources
   @param stats    compiled stats (a battler resource's maximum; the pool stat's is the faint resource)
   @param layout   compiled layout (persist and raises roles)
   @param ruleset  compiled ruleset (max / regen formulas)
   @param rs_name  ruleset file name (messages)
   @param turn     compiled turn structure (a regen needs a regen end-of-turn phase)
   @param limits   the platform's limits (max_battler_resources, max_move_resources)
   @param where    location (the structure manifest)
   @param enc      this compile's text encoders
   @return compiled resources: name, label, scope (SCOPES index), scope_name, empty (EMPTY_RULES index), stat,
           base (the move field or None), max (formula or None), raises / persist (role IDs or NO_ROLE),
           raises_name / persist_name, regen (number), regen_formula (name or None), slot (index in its scope's
           storage; NONE8 for the faint resource, which is Battler::hp)
   """
   w0 = f"{where}: resources"
   if not isinstance(entries, list) or not entries:
      raise ManifestError(f"{w0}: a list of resources; a battle needs at least the battler resource with "
                          f"\"empty\": \"faint\" on the pool stat (hit points, D-25)")
   stat_names = [st["name"] for st in stats]
   pool = next(st["name"] for st in stats if st["pool"])
   out: list[dict[str, Any]] = []
   roles_used: dict[int, str] = {}
   slots = {"battler": 0, "move": 0}
   for i, e in enumerate(entries):
      w = f"{w0}[{i}] ({e.get('name') if isinstance(e, dict) else e})"
      check_keys(e, "resource", w)
      name = e.get("name")
      if not isinstance(name, str) or not RESOURCE_NAME.match(name):
         raise ManifestError(f"{w}.name: a lower-case identifier, got {name!r}")
      if any(r["name"] == name for r in out):
         raise ManifestError(f"{w}: resource {name!r} is declared twice")
      scope = e.get("scope")
      if scope not in SCOPES:
         raise ManifestError(f"{w}.scope: one of {', '.join(SCOPES)}, got {scope!r}")
      empty = e.get("empty", DEFAULT_EMPTY)
      if empty not in EMPTY_RULES:
         raise ManifestError(f"{w}.empty: one of {', '.join(EMPTY_RULES)}, got {empty!r}")
      r: dict[str, Any] = {"name": name, "label": enc.text(e.get("label", name), f"{w}.label"),
                           "scope": SCOPES.index(scope), "scope_name": scope, "empty": EMPTY_RULES.index(empty),
                           "stat": NONE8, "base": None, "max": None, "raises": NO_ROLE, "raises_name": None,
                           "persist": NO_ROLE, "persist_name": None, "regen": 0, "regen_formula": None,
                           "slot": NONE8}
      if scope == "battler":
         for key in ("base", "max", "raises"):
            if key in e:
               raise ManifestError(f"{w}.{key}: only a move resource has a {key}")
         if e.get("stat") not in stat_names:
            raise ManifestError(f"{w}.stat: the stat that is its maximum, one of {', '.join(stat_names)}")
         r["stat"] = stat_names.index(e["stat"])
         if empty == "faint" and e["stat"] != pool:
            raise ManifestError(f"{w}: the faint resource is the pool stat's ({pool!r}): hit points")
      else:
         if "stat" in e:
            raise ManifestError(f"{w}.stat: a move resource's maximum is its \"max\" formula of the move's base")
         if empty == "faint":
            raise ManifestError(f"{w}.empty: only a battler resource faints its battler")
         base = e.get("base")
         if not isinstance(base, str) or not RESOURCE_NAME.match(base) or base in MOVE_FIELDS:
            raise ManifestError(f"{w}.base: the move field holding its base (a lower-case identifier that is not "
                                f"one of {', '.join(sorted(MOVE_FIELDS - {'editor'}))}), got {base!r}")
         if any(o["base"] == base for o in out):
            raise ManifestError(f"{w}.base: another resource reads the move field {base!r}")
         r["base"] = base
         if "max" in e:
            r["max"] = _named_formula(e["max"], ruleset, rs_name, "max", f"{w}.max")
         if "raises" in e:
            if r["max"] is None:
               raise ManifestError(f"{w}.raises: raises only change a \"max\" formula of (base, raises)")
            r["raises"] = _role(e["raises"], layout, scope, "raises", w)
            r["raises_name"] = e["raises"]
      if e.get("persist") is not None:
         r["persist"] = _role(e["persist"], layout, scope, "persist", w)
         r["persist_name"] = e["persist"]
      for rid, key in ((r["persist"], "persist"), (r["raises"], "raises")):
         if rid == NO_ROLE:
            continue
         if rid in roles_used:
            raise ManifestError(f"{w}.{key}: the role is also {roles_used[rid]}")
         roles_used[rid] = f"{name}'s {key}"
      if "regen" in e:
         # a number is stored in ResourceData::regen (int16_t); a formula's output is checked against the pool
         r["regen"], r["regen_formula"] = _amount(e["regen"], ruleset, rs_name, "regen", REGEN_RANGE[0],
                                                  REGEN_RANGE[1], f"{w}.regen")
      if empty != "faint":
         r["slot"] = slots[scope]
         slots[scope] += 1
      out.append(r)

   faint = [r for r in out if EMPTY_RULES[r["empty"]] == "faint"]
   if len(faint) != 1:
      raise ManifestError(f"{w0}: exactly one battler resource has \"empty\": \"faint\" (the pool stat's {pool!r}: "
                          f"hit points); found {len(faint)}")
   battler = sum(1 for r in out if r["scope_name"] == "battler")
   move = sum(1 for r in out if r["scope_name"] == "move")
   if battler > limits["max_battler_resources"]:
      raise ManifestError(f"{w0}: {battler} battler resources; this platform's engine holds "
                          f"{limits['max_battler_resources']} (hit points included)")
   if move > limits["max_move_resources"]:
      raise ManifestError(f"{w0}: {move} move resources; this platform's engine holds {limits['max_move_resources']}")
   regen = [r["name"] for r in out if r["regen"] or r["regen_formula"]]
   if regen and not any(e["phase"] == REGEN_PHASE for e in turn["end"]):
      raise ManifestError(f"{w0}: {', '.join(regen)} regenerate(s) at the end of the turn: the turn's end_of_turn "
                          f"needs a {{\"run\": \"regen\"}} phase")
   return out


def compile_cost(m: Mapping[str, Any], resources: Sequence[Mapping[str, Any]], ruleset: Mapping[str, Any],
                 rs_name: str, where: str) -> list[dict[str, Any]]:
   """!A move's cost (manifest "cost"; default: DEFAULT_COST of every move resource), in resource order.
   @param m          the manifest's move
   @param resources  compiled resources
   @param ruleset    compiled ruleset (cost formulas)
   @param rs_name    ruleset file name
   @param where      location (the move)
   @return [{resource, amount, formula (name or None)}] without zero amounts
   """
   names = [r["name"] for r in resources]
   if "cost" not in m:
      return [{"resource": k, "amount": DEFAULT_COST, "formula": None}
              for k, r in enumerate(resources) if r["scope_name"] == "move"]
   cost = m["cost"]
   if not isinstance(cost, dict):
      raise ManifestError(f"{where}.cost: an object {{resource: amount or formula}}")
   out = []
   for name, v in cost.items():
      if name not in names:
         raise ManifestError(f"{where}.cost: unknown resource {name!r} (resources: {', '.join(names)})")
   for k, r in enumerate(resources):   # paid in the manifest's resource order
      if r["name"] not in cost:
         continue
      amount, formula = _amount(cost[r["name"]], ruleset, rs_name, "cost", 0, STORAGE_MAX[r["scope_name"]],
                                f"{where}.cost.{r['name']}")
      if amount or formula:
         out.append({"resource": k, "amount": amount, "formula": formula})
   return out


def move_bases(m: Mapping[str, Any], resources: Sequence[Mapping[str, Any]], where: str) -> list[int]:
   """!A move's base value of each move resource (the field the resource names; every move states it).
   @param m          the manifest's move
   @param resources  compiled resources
   @param where      location (the move)
   @return bases in move-resource (slot) order
   """
   out = []
   for r in resources:
      if r["scope_name"] != "move":
         continue
      if r["base"] not in m:
         raise ManifestError(f"{where}: needs {r['base']!r} (the base of the move resource {r['name']!r}; no "
                             f"default)")
      out.append(integer(m[r["base"]], 0, U8_MAX, f"{where}.{r['base']}"))
   return out


def base_fields(resources: Sequence[Mapping[str, Any]]) -> set[str]:
   """!The move fields the move resources read (a move's allowed keys besides its own).
   @param resources  compiled resources
   @return field names
   """
   return {r["base"] for r in resources if r["base"] is not None}


def _covers(f: Mapping[str, Any], arg: int, need: tuple[int, int], rs_name: str, what: str) -> None:
   """!Checks that a formula's declared input range covers what the engine passes.
   @param f        compiled formula
   @param arg      argument index
   @param need     (lo, hi) the engine passes
   @param rs_name  ruleset file name
   @param what     the use (messages)
   @return None
   """
   key = f["inputs"][arg]
   have = f["ranges"][key]
   if have[0] > need[0] or have[1] < need[1]:
      raise ManifestError(f"{rs_name}: {f['name']} ({what}): input {key!r} must cover [{need[0]}, {need[1]}] (the "
                          f"engine passes values in that range); it declares {list(have)}")


def _within(f: Mapping[str, Any], lo: int, hi: int, rs_name: str, what: str) -> None:
   """!Checks that a formula's declared output range fits where the engine stores it.
   @param f        compiled formula
   @param lo       lowest stored value
   @param hi       highest stored value
   @param rs_name  ruleset file name
   @param what     the use (messages)
   @return None
   """
   if f["range"][0] < lo or f["range"][1] > hi:
      raise ManifestError(f"{rs_name}: {f['name']} ({what}): range must lie within [{lo}, {hi}]")


def pool_cap(r: Mapping[str, Any], layout: Mapping[str, Any], stat_cap: int,
             stats: Sequence[Mapping[str, Any]] | None = None) -> int:
   """!The highest value a resource's pool can hold (its storage, its persist role and, for a battler resource,
   its stat's highest value: the stat's analysed bounds when known, else the stat cap).
   @param r         compiled resource
   @param layout    compiled layout
   @param stat_cap  rules.stat_cap
   @param stats     compiled stats with their "bounds" (check_formula_ranges()), or None
   @return the cap
   """
   if r["scope_name"] == "move":
      cap = STORAGE_MAX["move"]
   elif stats is not None and stats[r["stat"]].get("bounds") is not None:
      cap = min(stats[r["stat"]]["bounds"][1], U16_MAX)
   else:
      cap = min(stat_cap, U16_MAX)
   if r["persist_name"] is not None:
      _rid, width, _count = role_info(layout, r["persist_name"])
      cap = min(cap, (1 << width) - 1)
   return cap


def _move_max_range(r: Mapping[str, Any], moves: Sequence[Mapping[str, Any]], ruleset: Mapping[str, Any],
                    layout: Mapping[str, Any]) -> int:
   """!The highest maximum a move resource's pool reaches: its max formula's analysed range over the moves' bases
   and the raises role's values (within the formula's declared range), or the largest base without one.
   @param r        compiled resource (move scope)
   @param moves    compiled moves (bases)
   @param ruleset  compiled ruleset
   @param layout   compiled layout
   @return the value (before the pool's storage clamps it)
   """
   bases = [m["base"][r["slot"]] for m in moves] or [0]
   if r["max"] is None:
      return max(bases)
   f = rc.named_formulas(ruleset)[r["max"]]
   raises = 0 if r["raises_name"] is None else (1 << role_info(layout, r["raises_name"])[1]) - 1
   ranges = {f["inputs"][0]: (min(bases), max(bases)), f["inputs"][1]: (0, raises)}
   try:
      _lo, hi = rc.interval(f["tree"], ranges, ruleset["constants"], f"{r['name']} max", [])
   except rc.RulesetError as e:
      raise ManifestError(str(e))
   return min(hi, f["range"][1])


def pool_range(r: Mapping[str, Any], moves: Sequence[Mapping[str, Any]], ruleset: Mapping[str, Any],
               layout: Mapping[str, Any], stats: Sequence[Mapping[str, Any]]) -> int:
   """!The highest value a resource's pool reaches in a battle (what its persist role must hold): a battler
   resource's stat's highest value (its analysed bounds), a move resource's highest maximum; within the pool's
   storage.
   @param r        compiled resource
   @param moves    compiled moves
   @param ruleset  compiled ruleset
   @param layout   compiled layout
   @param stats    compiled stats with their "bounds"
   @return the value
   """
   if r["scope_name"] == "move":
      return min(_move_max_range(r, moves, ruleset, layout), STORAGE_MAX["move"])
   return min(stats[r["stat"]]["bounds"][1], STORAGE_MAX["battler"])


def check_persist_roles(resources: Sequence[Mapping[str, Any]], moves: Sequence[Mapping[str, Any]],
                        ruleset: Mapping[str, Any], layout: Mapping[str, Any], stats: Sequence[Mapping[str, Any]],
                        where: str) -> None:
   """!Checks that every persisted resource's role holds the pool's highest value (pool_range()), the hit points'
   too: a narrower role would truncate the pool when the battle writes it back (capsule::battle store_resources()
   clamps and asserts, so this is the place to refuse it).
   @param resources  compiled resources
   @param moves      compiled moves
   @param ruleset    compiled ruleset
   @param layout     compiled layout
   @param stats      compiled stats with their "bounds"
   @param where      location (the structure manifest)
   @return None
   """
   for k, r in enumerate(resources):
      if r["persist_name"] is None:
         continue
      need = pool_range(r, moves, ruleset, layout, stats)
      width = role_info(layout, r["persist_name"])[1]
      held = (1 << width) - 1
      if need > held:
         if r["scope_name"] == "battler":
            why = f"its stat {stats[r['stat']]['name']!r} reaches {need}"
         else:
            why = f"its {'max formula ' + repr(r['max']) if r['max'] else 'largest base'} reaches {need}"
         raise ManifestError(f"{where}: resources[{k}] ({r['name']}).persist: the role {r['persist_name']!r} is "
                             f"{width} bits wide (up to {held}); {why}, so the write-back would truncate it")


def move_pool_top(r: Mapping[str, Any], move: Mapping[str, Any], ruleset: Mapping[str, Any] | None,
                  layout: Mapping[str, Any]) -> tuple[int, str]:
   """!The most one move's own pool of a move resource holds: its max formula over that move's base and the raises
   role's values (the formula's analysed range), or its base without one; within the pool's storage and persist
   role (pool_cap()).
   @param r        compiled resource (move scope)
   @param move     compiled move (its "base")
   @param ruleset  compiled ruleset (the max formula), or None: the pool's storage alone
   @param layout   compiled layout
   @return (the value, what decides it: for messages)
   """
   cap = pool_cap(r, layout, STORAGE_MAX["move"])   # a move pool: its storage and persist role
   base = move["base"][r["slot"]]
   if r["max"] is None:
      return min(base, cap), f"the move's base {r['base']!r} is {base}"
   if ruleset is None:
      return cap, "its storage"
   return (min(_move_max_range(r, [move], ruleset, layout), cap),
           f"its max formula {r['max']!r} of the move's base {base} and the raises")


def check_costs(resources: Sequence[Mapping[str, Any]], moves: Sequence[Mapping[str, Any]],
                layout: Mapping[str, Any], rules: Mapping[str, Any], stats: Sequence[Mapping[str, Any]],
                where: str, entries: Sequence[Json] = (), ruleset: Mapping[str, Any] | None = None) -> None:
   """!Checks the moves' costs: a number cost a pool can never pay is refused (a battler pool reaches its stat's
   highest value, a move's own pool of a move resource its max formula over that move's base and raises:
   move_pool_top(); the faint resource needs more than the amount: paying never faints), and the fallback move
   states no cost (it pays nothing, D-25 decision 4).
   @param resources  compiled resources
   @param moves      compiled moves
   @param layout     compiled layout
   @param rules      compiled rules (fallback_move, stat_cap)
   @param stats      compiled stats with their "bounds"
   @param where      location (the battle manifest)
   @param entries    the manifest's moves (the fallback's own "cost"), or none
   @param ruleset    compiled ruleset (the move resources' max formulas), or None: a move pool's storage bounds it
   @return None
   """
   fallback = rules["fallback_move"]
   if fallback != NONE16 and fallback < len(entries):
      stated = entries[fallback].get("cost") if isinstance(entries[fallback], Mapping) else None
      if isinstance(stated, Mapping) and any(v != 0 for v in stated.values()):
         raise ManifestError(f"{where}: moves[{fallback}] ({entries[fallback].get('name')}) is rules.fallback_move, "
                             f"which pays nothing (D-25): give it no cost")
   for i, m in enumerate(moves):
      if i == fallback:
         continue
      for c in m["cost"]:
         if c["formula"] is not None:
            continue
         r = resources[c["resource"]]
         if r["scope_name"] == "move":
            top, why = move_pool_top(r, m, ruleset, layout)
         else:
            top = pool_cap(r, layout, rules["stat_cap"], stats)
            why = f"its stat {stats[r['stat']]['name']!r}" if r.get("stat") is not None else "its stat"
         faint = EMPTY_RULES[r["empty"]] == "faint"
         if c["amount"] > top or (faint and c["amount"] >= top):
            # the faint resource (hit points) keeps at least one point: its pool must hold more than the cost
            rule = ("paying never faints the user, so the pool must hold more than the cost" if faint
                    else "the move is usable only while the pool holds at least the cost")
            raise ManifestError(f"{where}: moves[{i}].cost.{r['name']}: {c['amount']} can never be paid: the "
                                f"{r['name']} pool holds at most {top} ({why}), and {rule}")


def check_formulas(resources: Sequence[Mapping[str, Any]], moves: Sequence[Mapping[str, Any]],
                   ruleset: Mapping[str, Any], rs_name: str, layout: Mapping[str, Any], stat_cap: int,
                   stats: Sequence[Mapping[str, Any]] | None = None) -> None:
   """!Checks the resources' formulas against what the engine passes: a max formula's base covers the moves' bases
   and its raises the raises role's values (0 without one); regen and cost formulas get (value, max) of a pool
   (0 to its cap). Outputs must fit the pools.
   @param resources  compiled resources
   @param moves      compiled moves (bases, costs)
   @param ruleset    compiled ruleset
   @param rs_name    ruleset file name
   @param layout     compiled layout
   @param stat_cap   rules.stat_cap
   @param stats      compiled stats with their "bounds" (a battler pool's cap), or None (the stat cap)
   @return None
   """
   formulas = rc.named_formulas(ruleset)
   for r in resources:
      cap = pool_cap(r, layout, stat_cap, stats)
      if r["max"] is not None:
         f = formulas[r["max"]]
         bases = [m["base"][r["slot"]] for m in moves] or [0]
         raises = 0 if r["raises_name"] is None else (1 << role_info(layout, r["raises_name"])[1]) - 1
         _covers(f, 0, (min(bases), max(bases)), rs_name, f"{r['name']} max")
         _covers(f, 1, (0, raises), rs_name, f"{r['name']} max")
         _within(f, 0, STORAGE_MAX["move"], rs_name, f"{r['name']} max")
      if r["regen_formula"] is not None:
         f = formulas[r["regen_formula"]]
         for arg in (0, 1):
            _covers(f, arg, (0, cap), rs_name, f"{r['name']} regen")
         _within(f, -STORAGE_MAX[r["scope_name"]], STORAGE_MAX[r["scope_name"]], rs_name, f"{r['name']} regen")
   for m in moves:
      for c in m["cost"]:
         if c["formula"] is None:
            continue
         r = resources[c["resource"]]
         f = formulas[c["formula"]]
         for arg in (0, 1):
            _covers(f, arg, (0, pool_cap(r, layout, stat_cap, stats)), rs_name, f"a {r['name']} cost")
         _within(f, 0, STORAGE_MAX[r["scope_name"]], rs_name, f"a {r['name']} cost")


def formula_ids(resources: Sequence[Mapping[str, Any]], moves: Sequence[Mapping[str, Any]]) -> list[str]:
   """!The formulas rules::resource_formula() dispatches, in id order (first use: the resources' max and regen,
   then the moves' costs).
   @param resources  compiled resources
   @param moves      compiled moves
   @return formula names
   """
   names: list[str] = []
   for name in [x for r in resources for x in (r["max"], r["regen_formula"])] + \
         [c["formula"] for m in moves for c in m["cost"]]:
      if name is not None and name not in names:
         names.append(name)
   if len(names) > MAX_FORMULAS:
      raise ManifestError(f"{len(names)} distinct resource formulas; the engine numbers at most {MAX_FORMULAS} "
                          f"({NONE8} is \"no formula\")")
   return names


def formula_dispatch(names: Sequence[str]) -> list[str]:
   """!C++ definition of capsule::rules::resource_formula(): a resource formula's id -> its named formula (a, b).
   @param names  formula_ids()
   @return list of lines
   """
   L = ["int32_t resource_formula(int32_t formula, int32_t a, int32_t b) {"]
   if not names:
      return L + ["    (void)formula;", "    (void)b;", "    return a;   // no resource formulas", "}"]
   L.append("    switch (formula) {")
   L += [f"    case {k}: return {rc.FORMULA_FUNCTION_PREFIX}{name}(a, b);" for k, name in enumerate(names)]
   return L + ["    default: return a;", "    }", "}"]


def resource_names(resources: Sequence[Mapping[str, Any]]) -> list[str]:
   """!The resources' names in order (effect logic's `resources` list, bench pokes, battle_ids RESOURCE_).
   @param resources  compiled resources
   @return names
   """
   return [r["name"] for r in resources]
