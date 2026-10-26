"""!AI profiles (D-26 section 1, G1): battle.json `ai_profiles` and a party's `ai` (capsule/battle/data/ai.h).

A profile is {"name", "default": bool, "rules": [rule, ...], "switching": [rule, ...], "items": []}:
  - rules (scoring, in order; every rule runs for every usable move and target):
    {"rule": "noise", "max": n} a draw below n (the only rule that draws), {"rule": "damage", "weight": w,
    "ko_bonus": k} expected damage x accuracy x w (a knock-out: the target's HP + k), {"rule": "avoid_useless"}
    veto a damaging move with no effect on a foe, {"rule": "status", "score": n} per status inflicted on a foe
    without one, {"rule": "status_move", "score": n} a move without damage, {"rule": "save_resources",
    "whole_pool": n} minus the share of each pool the costs spend, {"rule": "pipeline", "veto_below": n} the
    ruleset pipeline ai_score's `score` (veto below n; optional), {"rule": "hook"} the effect hook ai_score;
  - switching (in order, after scoring; the first that says so switches to the first replacement that may come
    in): {"rule": "below", "score": n} the best move's score is below n, {"rule": "hook"} the ai_score hook of the
    battler's owners with move none (value 0 in) ends above the best move's score;
  - items (G3, in order, after switching; the first that says so uses the item on the battler as the turn's Item
    action, one of the party's `items` left): {"rule": "below_hp", "item": NAME, "percent": n} the battler's HP is
    below n percent of its maximum, {"rule": "hook", "item": NAME, "margin": n} the item's own ai_score handler
    (holder = the battler, move none, value 0 in) ends above the best move's score + n (block: never).
The numbers default to the engine's former built-in scorer's (RULE_ARGS). A party names its profile (`"ai": NAME`);
a party that names none, a party of records and an unowned one use the default profile (the one with "default":
true, else the first). A manifest without profiles gets DEFAULT_PROFILE (the former scorer without flags).
"""
from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

from .common import I16_MAX, U8_MAX, Json, ManifestError, check_keys, integer, register_keys

## Lowest signed 16-bit value (an AiRuleData argument).
I16_MIN = -I16_MAX - 1
## Scoring rules in capsule::battle::AiRuleKind order.
RULES = ["noise", "damage", "avoid_useless", "status", "status_move", "save_resources", "pipeline", "hook"]
## Switching rules (manifest names), their AiRuleKind ids following RULES.
SWITCH_RULES = ["below", "hook"]
## Item-use rules (manifest names, G3), their AiRuleKind ids following SWITCH_RULES.
ITEM_RULES = ["below_hp", "hook"]
## The rule key naming an item rule's item (its id is the rule's arg2).
ITEM_KEY = "item"
## A rule's arguments: name -> (AiRuleData field "arg" / "arg2", lowest, highest, default; None: optional, no
## default). The defaults are the engine's former built-in scorer's numbers (D-18). A damage weight stays a byte
## (the score of a hit, x100 percent, x the weight, keeps to 32 bits).
RULE_ARGS: dict[str, dict[str, tuple[str, int, int, int | None]]] = {
   "noise": {"max": ("arg", 1, I16_MAX, 16)},
   "damage": {"weight": ("arg", 0, U8_MAX, 4), "ko_bonus": ("arg2", 0, I16_MAX, 40)},
   "avoid_useless": {},
   "status": {"score": ("arg", I16_MIN, I16_MAX, 60)},
   "status_move": {"score": ("arg", I16_MIN, I16_MAX, 8)},
   "save_resources": {"whole_pool": ("arg", 0, I16_MAX, 40)},
   "pipeline": {"veto_below": ("arg", I16_MIN + 1, I16_MAX, None)},
   "hook": {},
}
## A switching rule's arguments (as RULE_ARGS).
SWITCH_ARGS: dict[str, dict[str, tuple[str, int, int, int | None]]] = {
   "below": {"score": ("arg", I16_MIN, I16_MAX, 0)},
   "hook": {},
}
## An item-use rule's numbers (as RULE_ARGS; the item itself is ITEM_KEY, every item rule names one).
ITEM_ARGS: dict[str, dict[str, tuple[str, int, int, int | None]]] = {
   "below_hp": {"percent": ("arg", 1, 100, 25)},
   "hook": {"margin": ("arg", I16_MIN, I16_MAX, 0)},
}
## AiRuleData::arg of a pipeline rule without a veto (capsule::battle::kAiNoVeto).
NO_VETO = I16_MIN
## Most profiles (capsule::battle::kMaxAiProfiles).
MAX_AI_PROFILES = 32
## Most scoring rules, and most switching rules, per profile (capsule::battle::kMaxAiRules).
MAX_AI_RULES = 16
## The profile of a manifest without ai_profiles: the engine's former scorer without flags (a draw, a status move's
## score), so a battle that named no AI behaves as before.
DEFAULT_PROFILE = {"name": "default", "rules": [{"rule": "noise"}, {"rule": "status_move"}]}
## The pipeline a pipeline rule runs (ruleset_compiler.PIPELINES).
AI_PIPELINE = "ai_score"
## The former party AI flags (capsule::battle::AiFlag before G1) and the rules each stood for, in the scorer's order
## (tools/battle_ai.py turns a party's flags into a profile).
FLAG_RULES = {"damage": ["damage"], "avoid_useless": ["avoid_useless"], "status": ["status"],
              "save_resources": ["save_resources"]}
## The rules every former flag set had (noise first, a status move's score after the status rule).
ALWAYS_RULES = ("noise", "status_move")
## The scorer's former order of every rule a flag set can have.
FLAG_ORDER = ["noise", "damage", "avoid_useless", "status", "status_move", "save_resources"]
## What a party's former flag list says now.
MIGRATION = "party AI flags are a profile now (D-26 section 1): run tools/battle_ai.py BATTLE.json"

register_keys({
   "ai_profile": {"name", "default", "rules", "switching", "items"},
   "ai_rule": {"rule", "max", "weight", "ko_bonus", "score", "whole_pool", "veto_below", "percent", "margin",
               "item"},
})


def flag_rules(flags: Sequence[str]) -> list[dict[str, str]]:
   """!The scoring rules a former party flag list stood for (the engine's former scorer, in its order).
   @param flags  the party's former "ai" flags
   @return the rules (default numbers)
   @throws ManifestError on an unknown flag
   """
   wanted = set(ALWAYS_RULES)
   for f in flags:
      if f not in FLAG_RULES:
         raise ManifestError(f"unknown AI flag {f!r} (flags: {', '.join(FLAG_RULES)})")
      wanted |= set(FLAG_RULES[f])
   return [{"rule": r} for r in FLAG_ORDER if r in wanted]


def compile_rule(r: Json, kinds: Sequence[str], args: Mapping[str, Mapping[str, tuple[str, int, int, int | None]]],
                 first: int, where: str) -> tuple[int, int, int]:
   """!One rule as AiRuleData (kind, arg, arg2).
   @param r      the rule
   @param kinds  the rule names allowed here
   @param args   their arguments
   @param first  the AiRuleKind id of kinds[0]
   @param where  location
   @return (kind id, arg, arg2)
   """
   check_keys(r, "ai_rule", where)
   name = r.get("rule")
   if name not in kinds:
      raise ManifestError(f"{where}.rule: one of {', '.join(kinds)}")
   values = {"arg": 0, "arg2": 0}
   if ITEM_KEY in r and kinds is not ITEM_RULES:
      raise ManifestError(f"{where}: only an item-use rule names an {ITEM_KEY!r}")
   for key in sorted(set(r) - {"rule", ITEM_KEY}):
      if key not in args[name]:
         raise ManifestError(f"{where}: a {name} rule takes {', '.join(args[name]) or 'no numbers'} (not {key!r})")
   for key, (field, lo, hi, default) in args[name].items():
      if key in r:
         values[field] = integer(r[key], lo, hi, f"{where}.{key}")
      elif default is not None:
         values[field] = default
      else:
         values[field] = NO_VETO
   return first + kinds.index(name), values["arg"], values["arg2"]


def item_rule(r: Json, items: Sequence[str], first: int, where: str) -> tuple[int, int, int]:
   """!One item-use rule (G3) as AiRuleData: its number, and the item it names as arg2.
   @param r      the rule
   @param items  the battle's item names (ids from 1)
   @param first  the AiRuleKind id of ITEM_RULES[0]
   @param where  location
   @return (kind id, arg, item id)
   """
   kind, arg, _arg2 = compile_rule(r, ITEM_RULES, ITEM_ARGS, first, where)
   name = r.get(ITEM_KEY)
   if name not in items:
      raise ManifestError(f"{where}.{ITEM_KEY}: an item of the battle ({', '.join(items) or 'none'}), got {name!r}")
   return kind, arg, list(items).index(name) + 1


def compile_ai_profiles(entries: Json, where: str, pipelines: Sequence[str],
                        items: Sequence[str] = ()) -> dict[str, Any]:
   """!The battle's AI profiles.
   @param entries    battle.json ai_profiles (None: DEFAULT_PROFILE)
   @param where      location
   @param pipelines  the pipelines the ruleset declares (a pipeline rule needs AI_PIPELINE among them)
   @param items      the battle's item names (item rules name them; G3)
   @return {"profiles": [{"name", "rules": [(kind, arg, arg2)], "switching": [...], "items": [...]}], "names",
           "default"}
   """
   given = entries is not None
   if not given:
      entries = [DEFAULT_PROFILE]
   if not isinstance(entries, list) or not 1 <= len(entries) <= MAX_AI_PROFILES:
      raise ManifestError(f"{where}: ai_profiles must be a list of 1 to {MAX_AI_PROFILES} profiles")
   profiles = []
   names: list[str] = []
   default = None
   for i, p in enumerate(entries):
      w = f"{where}: ai_profiles[{i}]"
      check_keys(p, "ai_profile", w)
      name = p.get("name")
      if not isinstance(name, str) or not name or name in names:
         raise ManifestError(f"{w}: needs a name of its own")
      names.append(name)
      if p.get("default", False) is True:
         if default is not None:
            raise ManifestError(f"{w}.default: only one profile is the default ({names[default]!r} is)")
         default = i
      elif p.get("default", False) is not False:
         raise ManifestError(f"{w}.default: true or false")
      lists = {}
      for key, kinds, args, first in (("rules", RULES, RULE_ARGS, 0),
                                      ("switching", SWITCH_RULES, SWITCH_ARGS, len(RULES))):
         rules = p.get(key, [])
         if not isinstance(rules, list) or len(rules) > MAX_AI_RULES:
            raise ManifestError(f"{w}.{key}: a list of at most {MAX_AI_RULES} rules")
         lists[key] = [compile_rule(r, kinds, args, first, f"{w}.{key}[{k}]") for k, r in enumerate(rules)]
         if key == "rules" and any(r.get("rule") == "pipeline" for r in rules) and AI_PIPELINE not in pipelines:
            raise ManifestError(f"{w}.rules: a pipeline rule runs the ruleset's {AI_PIPELINE} pipeline; the ruleset "
                                f"has none")
      item_rules = p.get("items", [])
      if not isinstance(item_rules, list) or len(item_rules) > MAX_AI_RULES:
         raise ManifestError(f"{w}.items: a list of at most {MAX_AI_RULES} rules")
      first_item = len(RULES) + len(SWITCH_RULES)
      lists["items"] = [item_rule(r, items, first_item, f"{w}.items[{k}]") for k, r in enumerate(item_rules)]
      profiles.append({"name": name, "rules": lists["rules"], "switching": lists["switching"], "items": lists["items"]})
   return {"profiles": profiles, "names": names, "default": default if default is not None else 0,
           "given": given}


def party_profile(ai: Json, profiles: Mapping[str, Any], where: str) -> int:
   """!The profile a party names (`"ai": NAME`; none: the default one).
   @param ai        the party's "ai"
   @param profiles  compile_ai_profiles() result
   @param where     location
   @return the profile index
   """
   if ai is None:
      return profiles["default"]
   if isinstance(ai, list):
      raise ManifestError(f"{where}.ai: {MIGRATION}")
   if ai not in profiles["names"]:
      raise ManifestError(f"{where}.ai: unknown AI profile {ai!r} (ai_profiles: {', '.join(profiles['names'])})")
   return profiles["names"].index(ai)


def emit_ai(profiles: Mapping[str, Any]) -> list[str]:
   """!The C++ tables of the profiles: kAiRules (every profile's scoring rules, then its switching rules, then its
   item-use rules) and kAiProfiles.
   @param profiles  compile_ai_profiles() result
   @return lines (inside the generated anonymous namespace)
   """
   kinds = RULES + SWITCH_RULES + ITEM_RULES
   rules: list[str] = []
   rows = []
   for p in profiles["profiles"]:
      first = len(rules)
      for key in ("rules", "switching", "items"):
         if key == "switching":
            first_switch = len(rules)
         rules += [f"    {{static_cast<AiRuleKind>({k}), 0, {a}, {b}}},   // {p['name']}: {kinds[k]}"
                   for k, a, b in p.get(key, [])]
      rows.append(f"    {{{first}, {first_switch}, {len(p['rules'])}, {len(p['switching'])}, "
                  f"{len(p.get('items', []))}, 0}},   // {p['name']}")
   L = ["const AiRuleData kAiRules[] = {", *(rules or ["    {AiRuleKind::Noise, 0, 1, 0},"]), "};",
        "const AiProfileData kAiProfiles[] = {", *rows, "};",
        f"static_assert({len(profiles['profiles'])} <= kMaxAiProfiles);",
        f"static_assert(static_cast<int>(AiRuleKind::NUM_KINDS) == {len(kinds)}, "
        "\"AI rule kinds differ from battlec.ai.RULES / SWITCH_RULES / ITEM_RULES\");"]
   return L


def check_ai_items(profiles: Mapping[str, Any], item_infos: Sequence[Mapping[str, Any] | None],
                   hooks: Sequence[set[str]], item_names: Sequence[str], where: str) -> None:
   """!The items the item-use rules name (G3): each is used in battle (use battle or both, with use_battle logic);
   a hook rule's item has an ai_score handler of its own.
   @param profiles    compile_ai_profiles() result
   @param item_infos  per item id: its bag and use fields (battlec/inventory.py item_fields(); [0]: None)
   @param hooks       per item id: the hooks its logic handles (inventory.item_hooks())
   @param item_names  item names (ids from 1)
   @param where       location
   @return None
   @throws ManifestError naming the first rule whose item does not fit
   """
   hook_kind = len(RULES) + len(SWITCH_RULES) + ITEM_RULES.index("hook")
   for p in profiles["profiles"]:
      for k, (kind, _arg, item) in enumerate(p.get("items", [])):
         w = f"{where}: ai_profiles {p['name']!r} items[{k}]"
         fields = item_infos[item]
         if fields is None or fields["use"] not in ("battle", "both"):
            raise ManifestError(f"{w}: {item_names[item - 1]!r} is not used in battle (its use is battle or both)")
         if kind == hook_kind and "ai_score" not in hooks[item]:
            raise ManifestError(f"{w}: a hook rule runs {item_names[item - 1]!r}'s own ai_score handler; it has "
                                f"none")
