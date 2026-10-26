"""!Rules of the battle compiler: the non-formula rules (capsule::battle::Rules:
caps, the self-hit's power and category, the fallback move) and the check of
the ruleset's formula ranges against what the engine passes. The arithmetic
of a hit (multipliers, damage roll, step order, the crit stage rule) is the
ruleset's pipelines (battlec/pipelines.py); the stage tables and the crit
odds are the stages' (battlec/stats.py compile_stages()).
"""
from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

import ruleset_compiler as rc

from .common import NONE16, PERCENT, U8_MAX, U16_MAX, Json, Lookups, ManifestError, check_keys, integer, register_keys
from .layout import role_max_value
from .progression import DEFAULT_LEVEL_MAX, DEFAULT_RANK_MAX, MAX_LEVEL_MAX, MAX_REWARD, MIN_LEVEL_MAX
from .stats import is_damaging
from .turn import ESCAPE_ATTEMPTS_MAX, MAX_BATTLERS

## Rules keys that moved to the stages (E6a) and where they went.
MOVED_TO_STAGES = {"stat_stages": "each stat stage's \"table\"", "accuracy_stages": "the accuracy stage's \"table\"",
                   "crit_odds": "the crit stage's \"table\" (odds)"}
## Rule default: highest stat (neutral, D-22 decision 8: no cap below the 16 bits a stat is stored in).
DEFAULT_STAT_CAP = U16_MAX
## The per-stat record role feeding each per-stat input of the stat formulas (ruleset_compiler.STAT_INPUTS):
## genes, training and awakening values (check_formula_ranges() bounds each input by its role's width).
STAT_INPUT_ROLES = {"gene": "stat_genes", "training": "stat_training", "awaken": "stat_awaken"}
## Rule default: power of a self-hit preview and of effect `self_hit` without a power (neutral: the lowest power;
## a game states its own).
DEFAULT_SELF_HIT_POWER = 1


def check_formula_ranges(ruleset: Mapping[str, Any], rs_name: str, rules: Mapping[str, Any],
                         species: Sequence[Mapping[str, Any]], parties: Sequence[Mapping[str, Any]],
                         layout: Mapping[str, Any], stats: list[dict[str, Any]],
                         traits: Sequence[Mapping[str, Any]]) -> None:
   """!Checks a ruleset against what the engine passes to and expects from
   each formula: every input range must cover the values the engine can pass,
   and the outputs must fit where the engine stores them. Each stat's formula
   is also analysed over that stat's own inputs (its base stats, its trait
   percentages); the bounds are stored in the stat ("bounds").
   @param ruleset  compiled ruleset (ruleset_compiler.load)
   @param rs_name  ruleset file name (messages)
   @param rules    compiled rules
   @param species  compiled species
   @param parties  compiled parties
   @param layout   compiled record layout (the widths of the per-stat roles bound genes, training, awakening)
   @param stats    compiled stats (updated with "bounds")
   @param traits   compiled traits
   @return None
   """
   lm = rules["level_max"]
   bases = [v for sp in species for v in sp["base"]] or [1]
   yields = [sp["exp_yield"] for sp in species] or [0]
   rewards = [p["reward"] for p in parties] or [0]
   per_stat = {key: (0, role_max_value(layout, role)) for key, role in STAT_INPUT_ROLES.items()}
   engine = {   # input -> range of values the engine can pass (speeds: bind_pipelines() checks them)
      "base": (min(bases), max(bases)), **per_stat, "level": (1, lm), "yield": (min(yields), max(yields)),
      "victor_level": (1, lm), "participants": (1, MAX_BATTLERS), "foe_rank": (0, rules["rank_max"]),
      "outsider": (0, 1), "attempts": (0, ESCAPE_ATTEMPTS_MAX), "reward_base": (min(rewards), max(rewards)),
   }
   for key, (lo, hi) in engine.items():
      have = ruleset["inputs"].get(key)
      if have is None or have[0] > lo or have[1] < hi:
         raise ManifestError(f"{rs_name}: input {key!r} must cover [{lo}, {hi}] (the engine passes values in "
                             f"that range)" + (f"; it declares {list(have)}" if have else ""))
   outputs = {"reward": (0, MAX_REWARD, "rewards are not negative and six of them add up inside int32")}
   for slot, (lo, hi, why) in outputs.items():
      f = ruleset["formulas"].get(slot)
      if f is not None and (f["range"][0] < lo or f["range"][1] > hi):
         raise ManifestError(f"{rs_name}: {slot}.range must lie within [{lo}, {hi}] ({why})")

   # stats: each formula a stat names stores into 16 bits (at least 1); its bounds over that stat's own inputs
   for i, st in enumerate(stats):
      f = ruleset["stat_formulas"][st["formula"]]
      if f["range"][0] < 1 or f["range"][1] > U16_MAX:
         raise ManifestError(f"{rs_name}: stat_formulas.{st['formula']}.range must lie within [1, {U16_MAX}] "
                             f"(stats are stored in 16 bits and must be at least 1; stat {st['name']!r})")
      own = [sp["base"][i] for sp in species] or [1]
      pcts = [PERCENT] + [nd["pct"][i] for nd in traits]
      ranges = {**ruleset["inputs"], **per_stat, "base": (min(own), max(own)), "level": (1, lm),
                "trait_pct": (min(pcts), max(pcts))}
      lo, hi = rc.interval(f["tree"], ranges, ruleset["constants"], f"{rs_name}: stat {st['name']}", [])
      st["bounds"] = (max(lo, f["range"][0]), min(hi, f["range"][1]))


register_keys({
   "rules": {"stat_cap", "level_max", "rank_max", "self_hit", "fallback_move",
             # B2: whether eggs may battle (tools/battlec/nursery.py reads it)
             "allow_eggs"},
   "self_hit": {"power", "category"},
})


def compile_rules(r: Json, category_entries: Sequence[Json], categories: Sequence[Mapping[str, int]], n: Lookups,
                  where: str) -> dict[str, Any]:
   """!The rules section.
   @param r                 the manifest's rules
   @param category_entries  the manifest's categories (names)
   @param categories        compile_categories() result
   @param n                 lookups (categories, moves)
   @param where             location
   @return compiled rules
   """
   for old in ("confusion", "protect_chain"):
      if old in r:
         raise ManifestError(f"{where}: rules.{old} is gone (E5): confusion and protection are volatile records "
                             f"(\"volatiles\": duration, chain {{den, cap}}, ...) and rules.self_hit {{power, "
                             f"category}} is the self-hit's")
   for old, now in MOVED_TO_STAGES.items():
      if old in r:
         raise ManifestError(f"{where}: rules.{old} is gone (E6a): the stages hold their tables (\"stages\": "
                             f"{now}, with the stage's \"range\")")
   check_keys(r, "rules", f"{where}: rules")
   cat_names = n["categories"]
   hit = r.get("self_hit", {})
   check_keys(hit, "self_hit", f"{where}: rules.self_hit")
   damaging = [c["name"] for i, c in enumerate(category_entries) if is_damaging(categories, i)]
   hit_cat = hit.get("category", damaging[0] if damaging else None)
   if hit_cat is None or not is_damaging(categories, cat_names(hit_cat, f"{where}: rules.self_hit.category")):
      raise ManifestError(f"{where}: rules.self_hit.category must name a damaging category")
   level_max = integer(r.get("level_max", DEFAULT_LEVEL_MAX), MIN_LEVEL_MAX, MAX_LEVEL_MAX, f"{where}: rules.level_max")
   return {
      "stat_cap": integer(r.get("stat_cap", DEFAULT_STAT_CAP), 1, U16_MAX, f"{where}: rules.stat_cap"),
      "level_max": level_max,
      "rank_max": integer(r.get("rank_max", DEFAULT_RANK_MAX), 0, U8_MAX, f"{where}: rules.rank_max"),
      "self_hit_power": integer(hit.get("power", DEFAULT_SELF_HIT_POWER), 1, U8_MAX, f"{where}: rules.self_hit.power"),
      "self_hit_category": cat_names(hit_cat, f"{where}: rules.self_hit.category"),
      "fallback_move": NONE16 if r.get("fallback_move") is None else n["moves"](r["fallback_move"],
                                                                                  f"{where}: rules.fallback_move"),
   }
