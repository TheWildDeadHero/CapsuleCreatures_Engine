"""!Eggs and the nursery (D-28 sections 2 and 3, package B2): structure.json `eggs` and `nursery`.

The engine has no idea of breeding: an egg is an ordinary record with an egg role set and a hatch counter role, and
the nursery is a few slots where records wait while a combination method of kind "nursery" checks them every N steps.
Every name and number is the manifest's.

**eggs** is {"role", "counter", "cycles_field", "step_effect", "hatch_script", "name", "sprite", "sprite_field"}:
  - role: a custom role of the layout (one element) that is non-zero while a record is an egg;
  - counter: a custom role (one element) holding the steps left until it hatches; a method delivering eggs seeds it
    with the result species' cycles_field value times the method's cycle_steps (the role must hold the largest);
  - cycles_field: the species field of the hatch cycles;
  - step_effect: the step effect (structure step_effects) whose runs are the egg cycles: each run takes its `every`
    steps off every egg's counter (the `hatch` effect-logic hook may change that: a warming passive
    doubles it), and an egg at 0 hatches (its egg role cleared, its stats derived, the hatch script run);
  - hatch_script (optional): the game's script the overworld runner starts for each egg that hatches (its party
    slot in the implicit variable egg_member; the script compiler resolves the name);
  - name: what an egg is called on screen (default DEFAULT_EGG_NAME, at most MAX_NAME_CHARS letters);
  - sprite (optional): the asset reference ("sprite:NAME") of the default egg sprite; sprite_field (optional): a
    species field giving each species' egg look instead (the game draws it).

**nursery** is {"slots", "method", "check", "odds_scale", "gain", "fee"}:
  - slots: how many records it holds (the method's inputs at least, up to the platform's max_nursery_slots);
  - method: a combination method of kind "nursery" (structure combine_methods);
  - check: the step effect whose runs are the nursery's checks: with the slots holding the method's inputs and no
    result waiting, each run draws the method's odds in odds_scale (default DEFAULT_ODDS_SCALE) and, on a hit,
    builds the result (an egg when the method delivers eggs) and keeps it until a script takes it;
  - gain: whether the held records gain experience per step (the ruleset pipeline nursery_gain; default 1 a step);
  - fee: whether taking a record back costs (the pipeline nursery_fee over its levels gained; default nothing).

The systems that would use a record refuse eggs unless their own flag allows them (default refused): battle setup
(structure rules.allow_eggs: an egg is never sent in), field actions (field_actions[].allow_eggs), item use
(battle.json items[].allow_eggs), recruitment (recruit.allow_eggs) and combination inputs (combine_methods[].
allow_eggs, B1); C9: the step effects' handlers (step_effects[].allow_eggs; the egg cycle counts eggs down anyway),
the encounter lead rules (lead.allow_eggs: an egg a rule picks still runs no passive or held item) and
transformations (transformation.allow_eggs). This module reads all of them into the egg tables.

C9, transfers: the egg role is keyed by the transfer name EGG_TRANSFER_NAME whatever it is called in the layout
(egg_transfer_key()), so every game knows an incoming egg. A build whose layout has no egg role refuses one
(capsule::battle::import_record()) unless the structure's `"transfer_eggs": "hatch"` receives it hatched
(TRANSFER_EGGS choices; the flag is emitted without an egg section too).

Emitted inside capsule::combine::kCombine (include/capsule/combine/data.h EggData / NurseryData); battle_ids.h
NURSERY_SLOTS. egg_record() and nursery_fee() are the Python mirrors (the studio's test saves and the tests).
"""
from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

import ruleset_compiler as rc
from sequence_compiler import asset_key

from .common import NONE8, U16_MAX, Json, ManifestError, check_keys, integer, register_keys
from .layout import role_info, role_key, role_max_value

## Key of the egg section in structure.json.
EGGS_KEY = "eggs"
## Key of the nursery section in structure.json.
NURSERY_KEY = "nursery"
## The flag key of every system that may take eggs.
ALLOW_EGGS = "allow_eggs"
## What an egg is called without a name of its own.
DEFAULT_EGG_NAME = "Egg"
## Longest egg name (letters).
MAX_NAME_CHARS = 10
## The nursery checks' odds are out of this by default (percent).
DEFAULT_ODDS_SCALE = 100
## Most steps one egg cycle of a method may count (the 16-bit hatch counter role).
MAX_CYCLE_STEPS = U16_MAX
## Default steps of one egg cycle of a method that delivers eggs.
DEFAULT_CYCLE_STEPS = 256
## The implicit script variable holding the party slot of the egg that hatched (the hatch script reads it).
EGG_MEMBER_VAR = "egg_member"
## EggData::flags: eggs may battle (rules.allow_eggs).
EGG_FLAG_BATTLE = 1
## EggData::flags: eggs may be recruited (recruit.allow_eggs).
EGG_FLAG_RECRUIT = 2
## EggData::flags: the lead rules see eggs (lead.allow_eggs; C9).
EGG_FLAG_LEAD = 4
## EggData::flags: eggs may transform (transformation.allow_eggs; C9).
EGG_FLAG_TRANSFORM = 8
## EggData::flags: a transferred egg is received hatched by a layout without an egg role (C9).
EGG_FLAG_TRANSFER_HATCH = 16
## The structure key of the transfer rule for eggs (C9).
TRANSFER_EGGS_KEY = "transfer_eggs"
## Its choices: refuse an incoming egg (default), or receive it hatched.
TRANSFER_EGGS = ["refuse", "hatch"]
## The egg role's transfer name (capsule::monster::kEggTransferName; layout.role_key() of it).
EGG_TRANSFER_NAME = "egg"
## NurseryData::flags: held records gain experience per step.
NURSERY_FLAG_GAIN = 1
## NurseryData::flags: taking a record back costs a fee.
NURSERY_FLAG_FEE = 2
## The kind of method a nursery runs.
NURSERY_KIND = "nursery"
## The delivery that makes eggs.
EGG_DELIVERY = "egg"
## Index of the element count in layout.role_info()'s (ID, width, count).
ROLE_INFO_COUNT = 2

register_keys({
   EGGS_KEY: {"role", "counter", "cycles_field", "step_effect", "hatch_script", "name", "sprite", "sprite_field"},
   NURSERY_KEY: {"slots", "method", "check", "odds_scale", "gain", "fee"},
})


def single_custom_role(layout: Mapping[str, Any], name: Json, where: str) -> tuple[int, int]:
   """!A stored custom role of one element.
   @param layout  compile_layout() result
   @param name    its name
   @param where   location
   @return (role ID, highest value)
   """
   if not isinstance(name, str) or name not in layout["custom_ids"]:
      raise ManifestError(f"{where}: {name!r} is not a stored custom role of the layout")
   info = role_info(dict(layout), name)
   if info is None or info[ROLE_INFO_COUNT] != 1:
      raise ManifestError(f"{where}: {name!r} must be a custom role of one element")
   return info[0], role_max_value(dict(layout), name)


def field_index(species_fields: Sequence[Mapping[str, Any]], name: Json, where: str) -> int:
   """!A species field's index.
   @param species_fields  compiled species fields
   @param name            its name
   @param where           location
   @return the index
   """
   names = [f["name"] for f in species_fields]
   if name not in names:
      raise ManifestError(f"{where}: {name!r} is not a species field ({', '.join(names) or 'none declared'})")
   return names.index(name)


def step_index(step_effects: Sequence[Mapping[str, Any]], name: Json, where: str) -> int:
   """!A step effect's index.
   @param step_effects  compiled step effects
   @param name          its name
   @param where         location
   @return the index
   """
   names = [e["name"] for e in step_effects]
   if name not in names:
      raise ManifestError(f"{where}: {name!r} is not a step effect ({', '.join(names) or 'none declared'})")
   return names.index(name)


def flag(src: Mapping[str, Any] | None, key: str, where: str) -> bool:
   """!A true / false key (false when absent).
   @param src    the object, or None
   @param key    the key
   @param where  location
   @return the flag
   """
   v = (src or {}).get(key, False)
   if not isinstance(v, bool):
      raise ManifestError(f"{where}.{key}: true or false")
   return v


def compile_eggs(b: Mapping[str, Any], layout: Mapping[str, Any], species: Sequence[Mapping[str, Any]],
                 species_fields: Sequence[Mapping[str, Any]], step_effects: Sequence[Mapping[str, Any]],
                 combine: Mapping[str, Any], enc: Any, swhere: str) -> dict[str, Any] | None:
   """!The structure's egg section (with the refusal flags of the systems that use records).
   @param b               the merged battle manifest
   @param layout          compiled layout
   @param species         compiled species
   @param species_fields  compiled species fields
   @param step_effects    compiled step effects
   @param combine         compile_combine() result (the methods' deliveries and cycle steps)
   @param enc             this compile's text encoders
   @param swhere          location of the structure manifest
   @return {"role", "role_name", "counter", "counter_name", "cycles_field", "cycles_name", "step_effect",
           "step_name", "hatch_script", "name", "name_text", "sprite", "sprite_name", "sprite_field",
           "sprite_field_name", "flags", "field_actions" (mask)}, or None without eggs
   """
   src = b.get(EGGS_KEY)
   egg_methods = [m for m in combine["methods"] if m["delivery"] == EGG_DELIVERY]
   if src is None:
      if egg_methods:
         raise ManifestError(f"{swhere}: combine_methods {egg_methods[0]['name']!r} delivers eggs: the structure needs "
                             f"its {EGGS_KEY} section")
      return None
   w = f"{swhere}: {EGGS_KEY}"
   check_keys(src, EGGS_KEY, w)
   role, _top = single_custom_role(layout, src.get("role"), f"{w}.role")
   counter, counter_max = single_custom_role(layout, src.get("counter"), f"{w}.counter")
   if src.get("counter") == src.get("role"):
      raise ManifestError(f"{w}: role and counter must be two roles")
   cycles = field_index(species_fields, src.get("cycles_field"), f"{w}.cycles_field")
   step = step_index(step_effects, src.get("step_effect"), f"{w}.step_effect")
   cname = species_fields[cycles]["name"]
   # the counter must hold every species' cycles times every egg method's cycle steps
   most = max((sp["fields"][cname] for sp in species), default=0)
   for m in egg_methods:
      if most * m["cycle_steps"] > counter_max:
         raise ManifestError(f"{w}.counter: {src.get('counter')!r} holds up to {counter_max}; {m['name']!r} seeds up "
                             f"to {most} x {m['cycle_steps']} = {most * m['cycle_steps']} steps")
   script = src.get("hatch_script")
   if script is not None and not isinstance(script, str):
      raise ManifestError(f"{w}.hatch_script: a script name (or leave it out)")
   name = src.get("name", DEFAULT_EGG_NAME)
   if not isinstance(name, str) or not 1 <= len(name) <= MAX_NAME_CHARS:
      raise ManifestError(f"{w}.name: 1 to {MAX_NAME_CHARS} letters")
   sprite = src.get("sprite")
   if sprite is not None and (not isinstance(sprite, str) or not sprite):
      raise ManifestError(f"{w}.sprite: an asset name (or leave it out)")
   sprite_field = NONE8
   if src.get("sprite_field") is not None:
      sprite_field = field_index(species_fields, src["sprite_field"], f"{w}.sprite_field")
   # the refusal flags of the systems that use records (default refused)
   flags = 0
   if flag(b.get("rules"), ALLOW_EGGS, f"{swhere}: rules"):
      flags |= EGG_FLAG_BATTLE
   if flag(b.get("recruit"), ALLOW_EGGS, f"{swhere}: recruit"):
      flags |= EGG_FLAG_RECRUIT
   if flag(b.get("lead"), ALLOW_EGGS, f"{swhere}: lead"):
      flags |= EGG_FLAG_LEAD
   if flag(b.get("transformation"), ALLOW_EGGS, f"{swhere}: transformation"):
      flags |= EGG_FLAG_TRANSFORM
   flags |= transfer_flags(b, swhere)
   actions = 0
   for k, a in enumerate(b.get("field_actions") or []):
      if isinstance(a, dict) and flag(a, ALLOW_EGGS, f"{swhere}: field_actions[{k}]"):
         actions |= 1 << k
   steps = 0
   for k, e in enumerate(b.get("step_effects") or []):
      if isinstance(e, dict) and flag(e, ALLOW_EGGS, f"{swhere}: step_effects[{k}]"):
         steps |= 1 << k
   return {"role": role, "role_name": src["role"], "counter": counter, "counter_name": src["counter"],
           "counter_max": counter_max, "cycles_field": cycles, "cycles_name": cname, "step_effect": step,
           "step_name": src["step_effect"], "hatch_script": script, "name": name,
           "name_text": enc.text(name, f"{w}.name"), "sprite": asset_key(sprite, f"{w}.sprite") if sprite else 0,
           "sprite_name": sprite, "sprite_field": sprite_field,
           "sprite_field_name": src.get("sprite_field"), "flags": flags, "field_actions": actions,
           "step_effects": steps}


def transfer_flags(b: Mapping[str, Any], swhere: str) -> int:
   """!The structure's transfer rule for eggs (C9): EGG_FLAG_TRANSFER_HATCH for "hatch".
   @param b       the merged battle manifest
   @param swhere  location of the structure manifest
   @return the flag bits
   """
   rule = b.get(TRANSFER_EGGS_KEY, TRANSFER_EGGS[0])
   if rule not in TRANSFER_EGGS:
      raise ManifestError(f"{swhere}: {TRANSFER_EGGS_KEY}: one of {', '.join(TRANSFER_EGGS)}")
   return EGG_FLAG_TRANSFER_HATCH if rule == TRANSFER_EGGS[1] else 0


def egg_transfer_key(layout: dict[str, Any], eggs: Mapping[str, Any] | None, swhere: str) -> None:
   """!Keys the egg role by the transfer name EGG_TRANSFER_NAME (C9): a record transfer then carries it under the key
   every game knows (capsule::monster::kEggTransferKey), whatever the role is called here.
   @param layout  compiled layout (its custom roles' transfer_key changed in place)
   @param eggs    compile_eggs() result, or None
   @param swhere  location of the structure manifest
   @return None
   @throws ManifestError when another custom role already has that key (a role named like the transfer name)
   """
   if eggs is None:
      return
   key = role_key(EGG_TRANSFER_NAME)
   for c in layout["custom"]:
      if c["name"] != eggs["role_name"] and c["transfer_key"] == key:
         raise ManifestError(f"{swhere}: monster_layout: the custom role {c['name']!r} has the egg role's transfer key "
                             f"(the name {EGG_TRANSFER_NAME!r}); rename it or make it the egg role")
   for c in layout["custom"]:
      if c["name"] == eggs["role_name"]:
         c["transfer_key"] = key


def compile_nursery(b: Mapping[str, Any], combine: Mapping[str, Any], step_effects: Sequence[Mapping[str, Any]],
                    limits: Mapping[str, int], swhere: str) -> dict[str, Any] | None:
   """!The structure's nursery.
   @param b             the merged battle manifest
   @param combine       compile_combine() result
   @param step_effects  compiled step effects
   @param limits        the platform's limits (max_nursery_slots)
   @param swhere        location of the structure manifest
   @return {"slots", "method", "method_name", "check", "check_name", "odds_scale", "gain", "fee", "flags"}, or None
   """
   src = b.get(NURSERY_KEY)
   if src is None:
      return None
   w = f"{swhere}: {NURSERY_KEY}"
   check_keys(src, NURSERY_KEY, w)
   methods = {m["name"]: (k, m) for k, m in enumerate(combine["methods"])}
   name = src.get("method")
   if name not in methods or methods[name][1]["kind"] != NURSERY_KIND:
      kinds = [n for n, (_k, m) in methods.items() if m["kind"] == NURSERY_KIND]
      raise ManifestError(f"{w}.method: a combination method of kind {NURSERY_KIND!r} "
                          f"({', '.join(kinds) or 'none declared'})")
   index, m = methods[name]
   top = limits["max_nursery_slots"]
   slots = integer(src.get("slots", m["max"]), m["min"], top, f"{w}.slots")
   if slots < m["max"]:
      raise ManifestError(f"{w}.slots: {slots} slots; {name!r} takes up to {m['max']} inputs")
   check = step_index(step_effects, src.get("check"), f"{w}.check")
   scale = integer(src.get("odds_scale", DEFAULT_ODDS_SCALE), 1, U16_MAX, f"{w}.odds_scale")
   gain = flag(src, "gain", w)
   fee = flag(src, "fee", w)
   return {"slots": slots, "method": index, "method_name": name, "check": check, "check_name": src["check"],
           "odds_scale": scale, "gain": gain, "fee": fee,
           "flags": (NURSERY_FLAG_GAIN if gain else 0) | (NURSERY_FLAG_FEE if fee else 0)}


def check_nursery_methods(combine: Mapping[str, Any], nursery: Mapping[str, Any] | None, swhere: str) -> None:
   """!A nursery method needs the nursery that runs it.
   @param combine  compile_combine() result
   @param nursery  compile_nursery() result
   @param swhere   location
   @return None
   """
   for m in combine["methods"]:
      if m["kind"] == NURSERY_KIND and (nursery is None or nursery["method_name"] != m["name"]):
         raise ManifestError(f"{swhere}: combine_methods {m['name']!r} is a nursery method: the structure's "
                             f"{NURSERY_KEY} must run it")


# -- C++ -------------------------------------------------------------------------------------------------------

def egg_initializer(eggs: Mapping[str, Any] | None, flags: int = 0) -> tuple[list[str], str]:
   """!The egg table's C++ (its name text and the EggData initializer).
   @param eggs   compile_eggs() result
   @param flags  without eggs: the flags still emitted (C9: transfer_flags())
   @return (lines before kCombine, the initializer)
   """
   if eggs is None:
      return [], (f"{{nullptr, 0, 0, 0, monster::kNoRole, monster::kNoRole, kNoField, kNoField, kNoStepEffect, "
                  f"{flags}}}")
   codes = ", ".join(f"0x{x:02X}" for x in eggs["name_text"])
   lines = [f"const text::Code kEggName[] = {{{codes}}};"]
   return lines, (f"{{kEggName, 0x{eggs['sprite']:08X}u, 0x{eggs['field_actions']:08X}u, "
                  f"0x{eggs['step_effects']:08X}u, {eggs['role']}, {eggs['counter']}, {eggs['cycles_field']}, "
                  f"0x{eggs['sprite_field']:02X}, {eggs['step_effect']}, {eggs['flags']}}}")


def nursery_initializer(nursery: Mapping[str, Any] | None) -> str:
   """!The NurseryData initializer.
   @param nursery  compile_nursery() result
   @return the initializer
   """
   if nursery is None:
      return "{0, 0, 0, kNoStepEffect, 0, {0, 0}}"
   return (f"{{{nursery['odds_scale']}, {nursery['slots']}, {nursery['method']}, {nursery['check']}, "
           f"{nursery['flags']}, {{0, 0}}}}")


def nursery_ids(c: Mapping[str, Any]) -> list[str]:
   """!battle_ids.h NURSERY_SLOTS (only with a nursery).
   @param c  compile_battle() result
   @return lines
   """
   n = c["combine"].get("nursery")
   if n is None:
      return []
   return [f"inline constexpr uint8_t NURSERY_SLOTS = {n['slots']};   ///< The nursery's slots.", ""]


# -- Python mirrors -----------------------------------------------------------------------------------------------

def hatch_steps(c: Mapping[str, Any], method: Mapping[str, Any], species: str) -> int:
   """!The hatch counter an egg of a species starts with (its cycles times the method's cycle steps).
   @param c        compile_battle() result
   @param method   the compiled method
   @param species  the species' name
   @return steps
   """
   eggs = c["combine"]["eggs"]
   sp = next(s for s in c["species"] if s["name"] == species)
   return int(sp["fields"][eggs["cycles_name"]]) * int(method["cycle_steps"])


def egg_record(c: Mapping[str, Any], record: Mapping[str, Any], steps: int) -> dict[str, Any]:
   """!A record dict made an egg (the studio's test saves): its egg role 1 and its hatch counter.
   @param c       compile_battle() result
   @param record  record dict ("species", roles)
   @param steps   the hatch counter
   @return the egg's record dict
   """
   eggs = c["combine"]["eggs"]
   if eggs is None:
      raise ManifestError("the structure has no eggs section")
   return {**record, eggs["role_name"]: 1, eggs["counter_name"]: max(0, min(eggs["counter_max"], int(steps)))}


def nursery_fee(c: Mapping[str, Any], levels_gained: int, level: int, count: int) -> int:
   """!What taking a record back costs (rules::nursery_fee(); 0 without a fee).
   @param c              compile_battle() result
   @param levels_gained  the levels it gained there
   @param level          its level now
   @param count          the records held
   @return the fee
   """
   n = c["combine"].get("nursery")
   if n is None or not n["fee"]:
      return 0
   got = rc.run_pipeline(c["pipelines"], "nursery_fee", {"levels_gained": levels_gained, "level": level,
                                                          "count": count})
   return int(got["outputs"]["fee"])


def nursery_gain(c: Mapping[str, Any], level: int, slot: int) -> int:
   """!The experience per step a held record gains (rules::nursery_gain(); 0 without gain).
   @param c      compile_battle() result
   @param level  its level when left there
   @param slot   its slot
   @return experience per step
   """
   n = c["combine"].get("nursery")
   if n is None or not n["gain"]:
      return 0
   got = rc.run_pipeline(c["pipelines"], "nursery_gain", {"level": level, "slot": slot})
   return int(got["outputs"]["gain"])
