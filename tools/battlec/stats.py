"""!Stats of the battle compiler (D-20 C, D-22 A7): the manifest's permanent
stats (capsule::battle::StatInfo, each computed by a ruleset stat formula),
the stages (capsule::battle::StageData), move categories (which stats their
damage uses), traits (percent per stat), and preview_stats() for editors.

    "stats": [{"name": "hp", "label": "HP", "formula": "stat_hp", "pool": true},
              {"name": "atk", "label": "Attack", "formula": "stat_other"}, ...
              {"name": "spe", "label": "Speed", "formula": "stat_other", "order": true}],
    "stages": [{"name": "accuracy", "kind": "accuracy", "range": [-6, 6], "table": [[3, 9], ...]},
               {"name": "evasion", "kind": "evasion", "label": "evasiveness", "range": [-6, 6]},
               {"name": "critical", "kind": "crit", "label": "focus", "range": [0, 4],
                "table": ["1/16", "1/8", "1/4", "1/3", "1/2"]},
               {"stat": "atk", "range": [-6, 6], "table": [[2, 8], ...]}, ...]

Stat flags: pool (exactly one stat: the creature's hit points; trait
defaults to false), order (exactly one: turn order and escape use it), odds
(at most one, E9: the ruleset's effect_odds pipeline reads it for both
battlers as user_stat / target_stat; without one they read 0), trait
(default true: traits may raise or lower it), transfer (the stat's key in
transfers, default its name). label is the in-game text ({stat});
editor.short is for the studio only (stripped).

Stages, in manifest order (the stage index, Battler::stages): kind "stat"
(the default when "stat" names one: a stat's stage, any stat but the pool
stat, at most one each), "accuracy", "evasion" or "crit" (at most one each);
name (default the stat's name; effect code names stages by it: `stage f atk
-1`), label (in-game text, the stage messages' {stat}; default the stat's
label or the name), range [lo, hi] (lo <= 0 <= hi, within +-STAGE_LIMIT;
required) and either a table (hi - lo + 1 entries from stage lo: [num, den]
multipliers, odds such as "1/16" for the crit stage) or a formula (a named
ruleset formula of two arguments, the value and the stage value; not for the
crit stage), or neither (x1). The pipelines read them through stage_scale(),
staged() (the table every stat stage shares), accuracy_staged() and
crit_chance() / crit_out_of() (ruleset_compiler.py).
"""
from __future__ import annotations

import re
from collections.abc import Mapping, Sequence
from typing import Any

import ruleset_compiler as rc

from .common import (NONE8, PAIR, PERCENT, U8_MAX, Json, ManifestError, Names, Texts, check_keys, integer, odds,
                     register_keys)

## Stage kinds in capsule::battle::StageKind order.
STAGE_KINDS = ["stat", "accuracy", "evasion", "crit"]
## Stage kinds a battle has at most one of.
SINGLE_STAGE_KINDS = ["accuracy", "evasion", "crit"]
## A stage's range lies within [-STAGE_LIMIT, STAGE_LIMIT] (capsule::battle::kStageLimit).
STAGE_LIMIT = rc.STAGE_LIMIT
## Arguments of a stage formula (the value and the stage value).
STAGE_FORMULA_ARGS = 2
## Stat names of the six-stat layout the studio still offers until it reads the manifest's stats (S3).
STATS = ["hp", "atk", "def", "spa", "spd", "spe"]
## A stat's name: a lower-case identifier (stage references, battle_ids STAT_<NAME>).
STAT_NAME = re.compile(r"[a-z][a-z0-9_]*")
## Most move categories (capsule::battle::kMaxCategories).
MAX_CATEGORIES = 4
## Category flag: damaging (capsule::battle::CATEGORY_DAMAGING).
CATEGORY_DAMAGING = 1
## StatInfo flag: traits may change the stat (capsule::battle::STATINFO_TRAIT).
STATINFO_TRAIT = 1
## Largest stat stage change of one effect.
MAX_STAGE_DELTA = 12


def preview_stats(compiled: Mapping[str, Any], species_name: str, level: int, genes: Sequence[int] | None = None,
                  training: Sequence[int] | None = None, awaken: Sequence[int] | None = None,
                  trait: str | None = None) -> list[int]:
   """!A creature's stats as the engine computes them (capsule::battle::record_stats()).
   @param compiled      from compile_battle()
   @param species_name  species name
   @param level         level
   @param genes         one value per stat (default 0)
   @param training      one value per stat (default 0)
   @param awaken        one value per stat (default 0)
   @param trait         trait name or None
   @return one stat per stat of the manifest
   """
   sp = next(s for s in compiled["species"] if s["name"] == species_name)
   count = len(compiled["stats"])
   genes = genes or [0] * count
   training = training or [0] * count
   awaken = awaken or [0] * count
   pct = [PERCENT] * count
   if trait is not None:
      pct = compiled["traits"][compiled["names"]["traits"].index(trait)]["pct"]
   rs = compiled["ruleset"]
   return [rc.run(rs, st["formula"], base=sp["base"][i], gene=genes[i], training=training[i], awaken=awaken[i],
                  level=level, trait_pct=pct[i]) for i, st in enumerate(compiled["stats"])]


register_keys({
   "stat": {"name", "label", "formula", "pool", "order", "odds", "trait", "transfer"},
   "stage": {"name", "kind", "stat", "label", "range", "table", "formula"},
   "category": {"name", "offense", "defense"},
   "trait": {"name", "up", "down", "pct"},
})


def flag(entry: Mapping[str, Json], key: str, default: bool, where: str) -> bool:
   """!A true/false key of a stat.
   @param entry    the stat
   @param key      the key
   @param default  value when absent
   @param where    location
   @return the flag
   """
   v = entry.get(key, default)
   if not isinstance(v, bool):
      raise ManifestError(f"{where}.{key}: true or false")
   return v


def compile_stats(entries: Json, ruleset: Mapping[str, Any], rs_name: str, max_stats: int, where: str,
                  enc: Texts) -> list[dict[str, Any]]:
   """!The stats section: names, labels, formulas, flags and stage indices.
   @param entries    the manifest's stats
   @param ruleset    compiled ruleset (its stat_formulas)
   @param rs_name    ruleset file name (messages)
   @param max_stats  most stats (the platform's limit, build_settings.PLATFORM_LIMITS)
   @param where      location
   @param enc        this compile's text encoders
   @return compiled stats (name, label, formula, pool, order, odds, stage, trait, transfer, flags)
   """
   if not isinstance(entries, list) or not entries:
      raise ManifestError(f"{where}: stats must list at least one stat")
   if len(entries) > max_stats:
      raise ManifestError(f"{where}: at most {max_stats} stats on this platform, got {len(entries)}")
   Names("stat", entries, f"{where}: stats")   # names present and unique
   stats = []
   transfers = set()
   for i, sd in enumerate(entries):
      w = f"{where}: stats[{i}] ({sd.get('name')})"
      if isinstance(sd, dict) and "stage" in sd:
         raise ManifestError(f"{w}.stage is gone (E6a): a stat has a stage when \"stages\" lists one for it "
                             f"({{\"stat\": \"{sd.get('name')}\", \"range\": [lo, hi], \"table\": [...]}})")
      check_keys(sd, "stat", w)
      name = sd["name"]
      if not STAT_NAME.fullmatch(name):
         raise ManifestError(f"{w}: a stat name is a lower-case identifier")
      formula = sd.get("formula")
      if formula not in ruleset["stat_formulas"]:
         raise ManifestError(f"{w}.formula: names a stat formula of {rs_name} "
                             f"({', '.join(ruleset['stat_formulas']) or 'it has none'}), got {formula!r}")
      pool = flag(sd, "pool", False, w)
      transfer = sd.get("transfer", name)
      if not isinstance(transfer, str) or not transfer or transfer in transfers:
         raise ManifestError(f"{w}.transfer: a key no other stat uses")
      transfers.add(transfer)
      stats.append({"name": name, "label": enc.text(sd.get("label", name), f"{w}.label"), "formula": formula,
                    "pool": pool, "order": flag(sd, "order", False, w), "odds": flag(sd, "odds", False, w),
                    "trait": flag(sd, "trait", not pool, w), "stage": NONE8, "transfer": transfer})
   for key in ("pool", "order"):
      chosen = [s["name"] for s in stats if s[key]]
      if len(chosen) != 1:
         raise ManifestError(f"{where}: exactly one stat is the {key} stat, got {len(chosen)}"
                             + (f" ({', '.join(chosen)})" if chosen else ""))
   odds = [s["name"] for s in stats if s["odds"]]
   if len(odds) > 1:
      raise ManifestError(f"{where}: at most one stat is the odds stat, got {len(odds)} ({', '.join(odds)})")
   for st in stats:
      st["flags"] = STATINFO_TRAIT if st["trait"] else 0
   return stats


def stage_table(v: Json, kind: str, count: int, where: str) -> tuple[list[tuple[int, int]] | None,
                                                                   list[tuple[int, int]] | None]:
   """!A stage's table: multipliers, or odds for the crit stage.
   @param v      the manifest's table
   @param kind   stage kind
   @param count  entries it needs (hi - lo + 1)
   @param where  location
   @return (multipliers or None, odds or None)
   """
   what = "odds" if kind == "crit" else "[num, den] pairs"
   if not isinstance(v, list) or len(v) != count:
      raise ManifestError(f"{where}: {count} {what}, one per stage of the range (lowest first)")
   if kind == "crit":
      return None, [odds(x, where) for x in v]
   for x in v:
      if not isinstance(x, list) or len(x) != PAIR:
         raise ManifestError(f"{where}: {count} {what}, one per stage of the range (lowest first)")
   return [(integer(x[0], 1, U8_MAX, where), integer(x[1], 1, U8_MAX, where)) for x in v], None


def stage_range(v: Json, where: str) -> tuple[int, int]:
   """!A stage's range.
   @param v      [lo, hi]
   @param where  location
   @return (lo, hi)
   """
   if not isinstance(v, list) or len(v) != PAIR:
      raise ManifestError(f"{where}: [lo, hi] with lo <= 0 <= hi")
   lo = integer(v[0], -STAGE_LIMIT, 0, where)
   hi = integer(v[1], 0, STAGE_LIMIT, where)
   return lo, hi


def compile_stages(entries: Json, stats: list[dict[str, Any]], ruleset: Mapping[str, Any], rs_name: str,
                   max_stats: int, where: str, enc: Texts) -> list[dict[str, Any]]:
   """!The stages section (capsule::battle::StageData, D-22 A7). Sets each staged stat's "stage" (its index).
   @param entries    the manifest's stages (None: none)
   @param stats      compile_stats() result (updated)
   @param ruleset    compiled ruleset (named formulas of formula stages)
   @param rs_name    ruleset file name (messages)
   @param max_stats  most stats (the platform's limit): at most 3 + max_stats - 1 stages (kMaxStages)
   @param where      location
   @param enc        this compile's text encoders
   @return compiled stages (name, label, kind, kind_id, stat, range, table, odds, formula, formula_id)
   """
   entries = [] if entries is None else entries
   if not isinstance(entries, list):
      raise ManifestError(f"{where}: stages must be a list")
   if len(entries) > len(SINGLE_STAGE_KINDS) + max_stats - 1:
      raise ManifestError(f"{where}: at most {len(SINGLE_STAGE_KINDS) + max_stats - 1} stages on this platform")
   named = rc.named_formulas(ruleset)
   formula_ids: dict[str, int] = {}
   stages: list[dict[str, Any]] = []
   names: set[str] = set()
   for i, sd in enumerate(entries):
      w = f"{where}: stages[{i}]"
      check_keys(sd, "stage", w)
      kind = sd.get("kind", "stat" if "stat" in sd else None)
      if kind not in STAGE_KINDS:
         raise ManifestError(f"{w}.kind: one of {', '.join(STAGE_KINDS)} (\"stat\" with \"stat\": a stat name)")
      stat = NONE8
      if kind == "stat":
         stat = stat_index(stats, sd.get("stat"))
         if stat is None or stats[stat]["pool"]:
            raise ManifestError(f"{w}.stat: one of {', '.join(st['name'] for st in stats if not st['pool'])} "
                                f"(a stat other than the pool stat)")
         if stats[stat]["stage"] != NONE8:
            raise ManifestError(f"{w}.stat: {sd['stat']} already has a stage")
      elif "stat" in sd:
         raise ManifestError(f"{w}.stat: only a stage of kind stat names a stat")
      elif kind in (st["kind"] for st in stages):
         raise ManifestError(f"{w}: a second {kind} stage (at most one)")
      name = sd.get("name", stats[stat]["name"] if stat != NONE8 else None)
      if not isinstance(name, str) or not STAT_NAME.fullmatch(name) or name in names:
         raise ManifestError(f"{w}.name: a lower-case identifier no other stage uses (a stat's stage defaults to "
                             f"the stat's name)")
      names.add(name)
      lo, hi = stage_range(sd.get("range"), f"{w}.range ({name})")
      table, crit_odds, formula = None, None, sd.get("formula")
      if "table" in sd and formula is not None:
         raise ManifestError(f"{w} ({name}): a table or a formula, not both")
      if "table" in sd:
         table, crit_odds = stage_table(sd["table"], kind, hi - lo + 1, f"{w}.table ({name})")
      if formula is not None:
         f = named.get(formula)
         if kind == "crit" or f is None or len(f["inputs"]) != STAGE_FORMULA_ARGS:
            raise ManifestError(f"{w}.formula ({name}): a named formula of {rs_name} with two args (the value and "
                                f"the stage value); the crit stage takes odds instead")
         formula_ids.setdefault(formula, len(formula_ids))
      label = enc.text(sd["label"], f"{w}.label") if "label" in sd else (
         stats[stat]["label"] if stat != NONE8 else enc.text(name, f"{w}.name"))
      if stat != NONE8:
         stats[stat]["stage"] = i
      stages.append({"name": name, "label": label, "kind": kind, "kind_id": STAGE_KINDS.index(kind), "stat": stat,
                     "range": (lo, hi), "table": table, "odds": crit_odds, "formula": formula,
                     "formula_id": formula_ids[formula] if formula is not None else NONE8})
   return stages


def stage_lookup(stages: Sequence[Mapping[str, Any]], where: str) -> Names:
   """!Stage names -> stage index (effect code: `stage f atk -1`).
   @param stages  compile_stages() result
   @param where   location
   @return lookup
   """
   return Names("stage", [{"name": st["name"]} for st in stages], where)


def stage_of_kind(stages: Sequence[Mapping[str, Any]], kind: str) -> int:
   """!The stage of a single kind (accuracy, evasion, crit).
   @param stages  compile_stages() result
   @param kind    the kind
   @return its index, or NONE8
   """
   return next((i for i, st in enumerate(stages) if st["kind"] == kind), NONE8)


def shared_stat_stage(stages: Sequence[Mapping[str, Any]]) -> int:
   """!The stat stage whose table every stat stage shares (the pipelines' staged()).
   @param stages  compile_stages() result
   @return its index, or NONE8 when there are no stat stages or they differ (range, table or formula)
   """
   staged = [(i, st) for i, st in enumerate(stages) if st["kind"] == "stat"]
   if not staged:
      return NONE8
   first = staged[0][1]
   same = all((st["range"], st["table"], st["formula"]) == (first["range"], first["table"], first["formula"])
              for _i, st in staged)
   return staged[0][0] if same else NONE8


def stage_battle_values(stages: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
   """!The stages as ruleset_compiler battle values (bind(): stages, stat_stage, accuracy_stage, evasion_stage,
   crit_stage; None for a missing one, and for a crit stage without odds).
   @param stages  compile_stages() result
   @return battle values
   """
   def index(i: int) -> int | None:
      """!An index, None for NONE8."""
      return None if i == NONE8 else i
   crit = stage_of_kind(stages, "crit")
   return {"stages": [{"kind": st["kind"], "range": st["range"], "table": st["table"], "odds": st["odds"],
                       "formula": st["formula"]} for st in stages],
           "stat_stage": index(shared_stat_stage(stages)),
           "accuracy_stage": index(stage_of_kind(stages, "accuracy")),
           "evasion_stage": index(stage_of_kind(stages, "evasion")),
           "crit_stage": index(crit) if crit != NONE8 and stages[crit]["odds"] is not None else None}


def stat_index(stats: Sequence[Mapping[str, Any]], name: Json) -> int | None:
   """!A stat's index by name.
   @param stats  compile_stats() result
   @param name   stat name
   @return index, or None
   """
   return next((i for i, st in enumerate(stats) if st["name"] == name), None)


def compile_categories(entries: Sequence[Json], stats: Sequence[Mapping[str, Any]],
                       where: str) -> list[dict[str, int]]:
   """!Move categories: which stats their damage uses (named; any stat but the pool stat).
   @param entries  the manifest's categories
   @param stats    compile_stats() result
   @param where    location
   @return compiled categories (offense, defense, flags)
   """
   usable = [st["name"] for st in stats if not st["pool"]]
   categories = []
   for i, cd in enumerate(entries):
      w = f"{where}: categories[{i}] ({cd.get('name')})"
      check_keys(cd, "category", w)
      if ("offense" in cd) != ("defense" in cd):
         raise ManifestError(f"{w}: a damaging category names both offense and defense (neither: non-damaging)")
      refs = []
      for key in ("offense", "defense"):
         if key in cd and cd[key] not in usable:
            raise ManifestError(f"{w}.{key}: one of {', '.join(usable)} (a stat other than the pool stat)")
         refs.append(stat_index(stats, cd[key]) if key in cd else NONE8)
      flags = 0
      if "offense" in cd:
         flags = CATEGORY_DAMAGING
      categories.append({"offense": refs[0], "defense": refs[1], "flags": flags})
   return categories


## The structure key of the category rule by type (E11).
CATEGORY_BY_TYPE_KEY = "category_by_type"
## The structure key of the hit types' categories (C5): a move whose hit type is not its own takes this category.
HIT_CATEGORY_KEY = "hit_category_by_type"


def compile_category_by_type(spec: Json, type_entries: Sequence[Json], category_entries: Sequence[Json],
                             categories: Sequence[Mapping[str, int]], where: str,
                             key: str = CATEGORY_BY_TYPE_KEY) -> list[int] | None:
   """!A category rule by type (E11): `"category_by_type": {type: damaging category}` makes a damaging move's
   category its type's (the rule of rulesets whose types decide physical or special); every type is listed. A move
   then states no category, or a non-damaging one (a status move), or its type's (content.compile_moves()).
   @param spec              the structure's category_by_type, or None (each move states its category)
   @param type_entries      the manifest's types
   @param category_entries  the manifest's categories
   @param categories        compile_categories() result
   @param where             location
   @param key               the structure key the table is under (messages; hit_category_by_type has the same shape)
   @return the category index per type (types order), or None without the rule
   """
   if spec is None:
      return None
   w = f"{where}: {key}"
   if not isinstance(spec, dict):
      raise ManifestError(f"{w}: an object of type name -> damaging category name")
   type_names = [t.get("name") for t in type_entries]
   category_names = [c.get("name") for c in category_entries]
   unknown = [str(k) for k in spec if k not in type_names]
   if unknown:
      raise ManifestError(f"{w}: unknown type(s) {', '.join(unknown)} (types: {', '.join(map(str, type_names))})")
   missing = [str(t) for t in type_names if t not in spec]
   if missing:
      raise ManifestError(f"{w}: every type names its category; missing {', '.join(missing)}")
   out = []
   for name in type_names:
      cat = spec[name]
      if cat not in category_names:
         raise ManifestError(f"{w}.{name}: one of {', '.join(map(str, category_names))}")
      index = category_names.index(cat)
      if not is_damaging(categories, index):
         raise ManifestError(f"{w}.{name}: {cat} is not a damaging category (one with offense and defense)")
      out.append(index)
   return out


def compile_hit_category_by_type(spec: Json, type_entries: Sequence[Json], category_entries: Sequence[Json],
                                 categories: Sequence[Mapping[str, int]], by_type: list[int] | None,
                                 where: str) -> list[int] | None:
   """!The categories hit types give (C5, BattleData::hit_categories): a damaging move whose hit type is not its own
   (the ruleset's move_type point changed it: Hidden Power, Weather Ball) hits with the category this table names for
   the hit type: `"hit_category_by_type": {type: damaging category}`, every type listed, for a structure whose moves
   state their categories. A structure with category_by_type needs none: its table is used (a damaging move's
   category is its type's, the hit type's too), and giving both is refused.
   @param spec              the structure's hit_category_by_type, or None
   @param type_entries      the manifest's types
   @param category_entries  the manifest's categories
   @param categories        compile_categories() result
   @param by_type           compile_category_by_type() result (None without category_by_type)
   @param where             location
   @return the category index per type (types order), or None: every move hits with its own category
   """
   if spec is None:
      return by_type
   if by_type is not None:
      raise ManifestError(f"{where}: {HIT_CATEGORY_KEY}: the structure's {CATEGORY_BY_TYPE_KEY} already decides the "
                          "category of every type (hit types too); leave one out")
   return compile_category_by_type(spec, type_entries, category_entries, categories, where, HIT_CATEGORY_KEY)


def is_damaging(categories: Sequence[Mapping[str, int]], index: int) -> bool:
   """!Whether a category deals formula damage.
   @param categories  compile_categories() result
   @param index       category index
   @return True if it has CATEGORY_DAMAGING
   """
   return (categories[index]["flags"] & CATEGORY_DAMAGING) != 0


def compile_traits(entries: Sequence[Json], stats: Sequence[Mapping[str, Any]], ruleset: Mapping[str, Any],
                   rs_name: str, where: str, enc: Texts) -> list[dict[str, Any]]:
   """!Traits: a percentage per stat (100 for the stats they leave alone); up and down name trait stats. Checked
   against the ruleset's trait_pct input.
   @param entries  the manifest's traits
   @param stats    compile_stats() result
   @param ruleset  compiled ruleset
   @param rs_name  ruleset file name (messages)
   @param where    location
   @param enc      this compile's text encoders
   @return compiled traits (name, pct)
   """
   trait_stats = [st["name"] for st in stats if st["trait"]]
   traits = []
   for i, nd in enumerate(entries):
      w = f"{where}: traits[{i}]"
      check_keys(nd, "trait", w)
      up, down = nd.get("up"), nd.get("down")

      # a trait that raises or lowers a stat states by how much (no default percentages, D-22 decision 8)
      up_pct, down_pct = PERCENT, PERCENT
      if up is not None or down is not None:
         given = nd.get("pct")
         if not isinstance(given, list) or len(given) != len(("up", "down")):
            raise ManifestError(f"{w}.pct: [raised %, lowered %] (a trait with \"up\" or \"down\" states its "
                                f"percentages)")
         up_pct, down_pct = given

      # one percentage per stat, 100 = unchanged
      pct = [PERCENT] * len(stats)
      for key, v, p in (("up", up, up_pct), ("down", down, down_pct)):
         if v is None:
            continue
         if v not in trait_stats:
            raise ManifestError(f"{w}.{key}: one of {', '.join(trait_stats)} (a stat with the trait flag)")
         pct[stat_index(stats, v)] = integer(p, 1, U8_MAX, f"{w}.pct")
      if up is not None and up == down:
         pct[stat_index(stats, up)] = PERCENT
      traits.append({"name": enc.text(nd["name"], f"{w}.name"), "pct": pct})
   trait_range = ruleset["inputs"].get("trait_pct", (PERCENT, PERCENT))
   for nd in traits:
      for p in nd["pct"] + [PERCENT]:
         if not trait_range[0] <= p <= trait_range[1]:
            raise ManifestError(f"{rs_name}: input trait_pct range {list(trait_range)} must include {p}")
   return traits
