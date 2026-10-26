"""!Types and the type chart of the battle compiler (D-22 A6): type entries,
the chart's tiers (capsule::battle::TierData), how the tiers of a defender's
types combine, how many types a species has and effects may add, and the
attacker-major matchup table of tier indices; type masks.

    "chart": {"tiers": [{"name": "none", "decisive": "zero", "message": "no_effect"},
                        {"name": "half", "q8": 128}, {"name": "normal", "q8": 256},
                        {"name": "twice", "q8": 512},
                        {"name": "ko", "decisive": "ko", "message": "super_effective"}],
              "combine": "exponent" | "multiply" | "sequential",
              "messages": {"above": "super_effective", "below": "not_very_effective"},
              "types_per_species": 2, "added_types": 1, "default": "normal",
              "matchups": {"Flame": {"Leaf": "twice", "Tide": 0.5, ...}, ...}}

A tier is a scaling tier (q8: its Q8 factor, 256 = x1) or decisive ("zero":
no effect; "ko": knocks out at once; the first decisive tier in the
defender's type order ends the matchup). Only a decisive tier has a message
(an engine message key, said when it applies); a scaling result says
messages.above / below when the combined factor is above / below x1. combine
"exponent" needs every scaling tier to be a power of two (the pipelines'
type_exp, pow2()); "multiply" combines the factors into one; "sequential"
applies them in turn (type_scale()). A matchup names a tier, or gives a
multiplier equal to one scaling tier's factor (0: the zero tier); matchups
not given take "default" (default: the tier whose factor is x1).
types_per_species (1 or more) + added_types (types effects may add) is at
most MAX_TYPES_PER (capsule::battle::kMaxTypesPer).
"""
from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

import ruleset_compiler as rc

from .common import NONE8, U8_MAX, U16_MAX, Json, Lookups, ManifestError, Texts, check_keys, integer, register_keys

## Types per battler, the species' and the added ones together (capsule::battle::kMaxTypesPer).
MAX_TYPES_PER = rc.MAX_TYPES_PER
## Most tiers (capsule::battle::kMaxTiers).
MAX_TIERS = 8
## Tier kinds in capsule::battle::TierKind order.
TIER_KINDS = ["scale", "zero", "ko"]
## Decisive tier kinds (a tier's "decisive").
DECISIVE = TIER_KINDS[1:]
## Combine modes in capsule::battle::Combine order.
COMBINE_MODES = rc.COMBINE_MODES
## Combine mode when the manifest names none.
DEFAULT_COMBINE = "multiply"
## Q8 factor of x1.
Q8_ONE = rc.Q8_ONE
## Largest exponent of a scaling tier in the exponent mode (pow2() doubles at most this often).
MAX_TIER_EXPONENT = rc.POW2_MAX_UP
## Smallest exponent (Q8 holds factors down to 1/256).
MIN_TIER_EXPONENT = -rc.Q8_SHIFT
## Types a species has when the manifest names no count.
DEFAULT_TYPES_PER_SPECIES = 1
## Types effects may add when the manifest names no count.
DEFAULT_ADDED_TYPES = 0
## Keys of chart.messages.
MESSAGE_KEYS = ["above", "below"]


def types_mask(names: Json, n: Lookups, where: str) -> int:
   """!Bit mask of type names.
   @param names  list
   @param n      lookups
   @param where  location
   @return int
   """
   if not isinstance(names, list):
      raise ManifestError(f"{where}: a list of types")
   m = 0
   for t in names:
      m |= 1 << n["types"](t, where)
   return m


register_keys({
   "type": {"name", "color"},
   "chart": {"tiers", "combine", "messages", "types_per_species", "added_types", "default", "matchups"},
   "tier": {"name", "q8", "decisive", "message"},
   "chart_messages": set(MESSAGE_KEYS),
})


def compile_types(entries: Sequence[Json], where: str, enc: Texts) -> list[dict[str, Any]]:
   """!Types (name, color).
   @param entries  the manifest's types
   @param where    location
   @param enc      this compile's text encoders
   @return compiled types
   """
   types = []
   for i, t in enumerate(entries):
      check_keys(t, "type", f"{where}: types[{i}]")
      types.append({"name": enc.text(t["name"], f"types[{i}].name"),
                    "color": integer(t.get("color", 0), 0, U8_MAX, f"types[{i}].color")})
   return types


def message_code(v: Json, messages: Sequence[str], where: str) -> int:
   """!An engine message by key (capsule::battle::Msg), or NONE8 for none.
   @param v         message key or None
   @param messages  the engine's message keys in Msg order (battlec.texts.declared_messages(): None for an optional
                    message the manifest leaves out)
   @param where     location
   @return the Msg code
   """
   if v is None:
      return NONE8
   if v not in messages:
      raise ManifestError(f"{where}: an engine message key the manifest gives "
                          f"({', '.join(m for m in messages if m is not None)}) or null")
   return messages.index(v)


def exponent_of(q8: int) -> int | None:
   """!The exponent of a power-of-two Q8 factor.
   @param q8  factor
   @return e with q8 == 256 * 2^e, or None
   """
   for e in range(MIN_TIER_EXPONENT, MAX_TIER_EXPONENT + 1):
      if (Q8_ONE << e if e >= 0 else Q8_ONE >> -e) == q8:
         return e
   return None


def compile_tiers(src: Json, combine: str, messages: Sequence[str], where: str) -> list[dict[str, Any]]:
   """!The chart's tiers.
   @param src       chart.tiers
   @param combine   the combine mode
   @param messages  engine message keys
   @param where     location
   @return tiers (name, kind, kind_id, q8, exp, message)
   """
   if not isinstance(src, list) or not 1 <= len(src) <= MAX_TIERS:
      raise ManifestError(f"{where}: chart.tiers lists 1 to {MAX_TIERS} tiers")
   tiers = []
   for i, t in enumerate(src):
      w = f"{where}: chart.tiers[{i}]"
      check_keys(t, "tier", w)
      name = t.get("name")
      if not isinstance(name, str) or not name or name in (x["name"] for x in tiers):
         raise ManifestError(f"{w}.name: a name no other tier uses")
      decisive = t.get("decisive")
      if decisive is not None:
         if decisive not in DECISIVE or "q8" in t:
            raise ManifestError(f"{w} ({name}): decisive is \"zero\" or \"ko\", without q8")
         tiers.append({"name": name, "kind": decisive, "kind_id": TIER_KINDS.index(decisive), "q8": 0, "exp": 0,
                       "message": message_code(t.get("message"), messages, f"{w}.message")})
         continue
      if "message" in t:
         raise ManifestError(f"{w} ({name}): only a decisive tier has a message (a scaling result says "
                             f"chart.messages.above / below)")
      q8 = integer(t.get("q8"), 0, U16_MAX, f"{w}.q8 ({name}: a scaling tier's Q8 factor, 256 = x1)")
      exp = exponent_of(q8)
      if combine == "exponent" and exp is None:
         raise ManifestError(f"{w}.q8 ({name}): combine \"exponent\" needs a power of two times 256 "
                             f"(2^{MIN_TIER_EXPONENT} to 2^{MAX_TIER_EXPONENT}), got {q8}")
      tiers.append({"name": name, "kind": "scale", "kind_id": 0, "q8": q8,
                    "exp": exp if combine == "exponent" else 0, "message": NONE8})
   return tiers


def tier_of(v: Json, tiers: Sequence[Mapping[str, Any]], where: str) -> int:
   """!A matchup's tier: a tier name, or a multiplier equal to one scaling tier's factor (0: the zero tier).
   @param v      tier name or multiplier
   @param tiers  compile_tiers() result
   @param where  location
   @return the tier index
   """
   names = [t["name"] for t in tiers]
   if isinstance(v, str) and v in names:
      return names.index(v)
   if isinstance(v, (int, float)) and not isinstance(v, bool):
      if v == 0:
         found = [i for i, t in enumerate(tiers) if t["kind"] == "zero"]
      else:
         found = [i for i, t in enumerate(tiers) if t["kind"] == "scale" and t["q8"] == v * Q8_ONE]
      if len(found) == 1:
         return found[0]
   raise ManifestError(f"{where}: a tier ({', '.join(names)}) or the multiplier of exactly one tier, got {v!r}")


def compile_chart(src: Json, n: Lookups, nt: int, where: str, messages: Sequence[str]) -> dict[str, Any]:
   """!The type chart.
   @param src       chart {tiers, combine, messages, types_per_species, added_types, default, matchups}
   @param n         lookups
   @param nt        number of types
   @param where     location
   @param messages  engine message keys (battlec.texts.MESSAGES)
   @return {tiers, combine, combine_id, above, below, types_per_species, added_types, matchups (tier indices,
           attacker-major)}
   """
   if not isinstance(src, dict) or "tiers" not in src:
      raise ManifestError(f"{where}: chart is {{tiers, combine, messages, types_per_species, added_types, default, "
                          f"matchups}} (E6a): declare chart.tiers and move the attacker rows under chart.matchups")
   check_keys(src, "chart", f"{where}: chart")
   combine = src.get("combine", DEFAULT_COMBINE)
   if combine not in COMBINE_MODES:
      raise ManifestError(f"{where}: chart.combine is one of {', '.join(COMBINE_MODES)}")
   tiers = compile_tiers(src["tiers"], combine, messages, where)
   msgs = src.get("messages", {})
   check_keys(msgs, "chart_messages", f"{where}: chart.messages")
   above, below = (message_code(msgs.get(k), messages, f"{where}: chart.messages.{k}") for k in MESSAGE_KEYS)
   per_species = integer(src.get("types_per_species", DEFAULT_TYPES_PER_SPECIES), 1, MAX_TYPES_PER,
                         f"{where}: chart.types_per_species")
   added = integer(src.get("added_types", DEFAULT_ADDED_TYPES), 0, MAX_TYPES_PER - per_species,
                   f"{where}: chart.added_types (types_per_species + added_types <= {MAX_TYPES_PER})")
   if "default" in src:
      default = tier_of(src["default"], tiers, f"{where}: chart.default")
   else:
      neutral = [i for i, t in enumerate(tiers) if t["kind"] == "scale" and t["q8"] == Q8_ONE]
      if not neutral:
         raise ManifestError(f"{where}: chart.default: name the tier of matchups not given (no tier has q8 256)")
      default = neutral[0]

   # attacker-major: matchups[attacker * types + defender]
   matchups = [default] * (nt * nt)
   for an, row in (src.get("matchups") or {}).items():
      a = n["types"](an, f"{where}: chart.matchups")
      if not isinstance(row, dict):
         raise ManifestError(f"{where}: chart.matchups.{an} must map defending types to tiers")
      for dn, v in row.items():
         d = n["types"](dn, f"{where}: chart.matchups.{an}")
         matchups[a * nt + d] = tier_of(v, tiers, f"{where}: chart.matchups.{an}.{dn}")
   return {"tiers": tiers, "combine": combine, "combine_id": COMBINE_MODES.index(combine), "above": above,
           "below": below, "types_per_species": per_species, "added_types": added, "matchups": matchups}


def chart_battle_values(chart: Mapping[str, Any]) -> dict[str, Any]:
   """!The chart as ruleset_compiler battle values (bind(): combine, the scaling tiers' factors and exponents, most
   types per battler).
   @param chart  compile_chart() result
   @return {combine, factors, exps, types}
   """
   scale = [t for t in chart["tiers"] if t["kind"] == "scale"] or [{"q8": Q8_ONE, "exp": 0}]
   return {"combine": chart["combine"], "factors": [t["q8"] for t in scale], "exps": [t["exp"] for t in scale],
           "types": chart["types_per_species"] + chart["added_types"]}


def type_effect(chart: Mapping[str, Any], nt: int, move_type: int,
                defender: Sequence[int]) -> dict[str, Any]:
   """!A move type's chart result against a defender's types, as the engine computes it (damage.cpp
   type_effect()).
   @param chart      compile_chart() result
   @param nt         number of types
   @param move_type  attacking type
   @param defender   the defender's types (NONE8 entries skipped)
   @return {"decisive": None | "zero" | "ko", "tier": decisive tier index or None, "exp": exponent sum,
           "q8": combined factor, "factors": the scaling tiers' factors in type order (run_pipeline types=)}
   """
   out: dict[str, Any] = {"decisive": None, "tier": None, "exp": 0, "q8": Q8_ONE, "factors": []}
   if not 0 <= move_type < nt:
      return out
   for t in defender:
      if not 0 <= t < nt:
         continue
      index = chart["matchups"][move_type * nt + t]
      tier = chart["tiers"][index]
      if tier["kind"] != "scale":
         return {**out, "decisive": tier["kind"], "tier": index, "exp": 0, "q8": Q8_ONE, "factors": []}
      out["exp"] += tier["exp"]
      out["q8"] = min(U16_MAX, (out["q8"] * tier["q8"]) >> rc.Q8_SHIFT)
      out["factors"].append(tier["q8"])
   if chart["combine"] == "exponent":
      out["q8"] = rc.pow2(Q8_ONE, out["exp"])
   return out
