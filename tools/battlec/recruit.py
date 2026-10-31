"""!Recruitment (D-26 section 3, package G3): structure.json `recruit` (capsule::battle::RecruitData, kRecruit).

An item's use_battle logic calls `recruit WHO BONUS;` (effect_compiler PRIMITIVES); the engine makes `checks` draws,
each against the ruleset pipeline recruit_odds (ruleset_compiler PIPELINES: sure, chance in out_of), with an event per
check, and a success ends the battle (Outcome::Recruited): the record joins the party, else the storage boxes.

The section is {"checks", "rate", "status_bonus", "owned", "refusal_consumes", "keep", "item_role", "messages",
"break_free"}:
  - checks: draws a recruit makes (1 to MAX_CHECKS; all must pass);
  - rate: the species field (structure species_fields) the pipeline reads as recruit_rate (optional: 0);
  - status_bonus: {"none": n, STATUS: n, ...} the pipeline's status_bonus per primary status (0-255; a status left
    out takes the "none" value, which defaults to DEFAULT_STATUS_BONUS);
  - owned: "refuse" (a battler of an owned party cannot be recruited: the logic fails, the item stays) or "allow";
  - refusal_consumes (C6): the refusals that still use the item up (REFUSALS: refused, blocked, no_room; default
    none): the logic goes on (as after a failed check) instead of failing, so the Item action consumes the item
    (classic: a ball thrown at an owned creature is lost);
  - keep: the battle changes the record keeps (KEEPS: hp, status, resources; what it does not keep is reset: pools
    full, no status); default every one;
  - item_role: a record role that stores the item used (optional);
  - messages: {moment: message key} for MESSAGE_MOMENTS (check, recruited, refused, blocked, no_room, to_party,
    to_storage); a moment left out shows no text; the messages may use {user} (the item's user), {target} (the
    creature) and {value} (check: the checks passed; to_party: the member index; to_storage: the box, from 1);
  - break_free: the message keys a failed recruit says by the checks it passed (index = checks passed; the last
    one for more), or one key for all.
Message keys are the battle's messages (the engine's optional ones the manifest gives, or its own: C4).
"""
from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

from .common import NONE8, U8_MAX, Json, ManifestError, check_keys, integer, register_keys
from .layout import role_info, role_max_value

## Most checks (capsule::battle::kMaxRecruitChecks).
MAX_CHECKS = 8
## Most "broke free" messages (capsule::battle::kMaxBreakFreeMessages).
MAX_BREAK_FREE = MAX_CHECKS
## The moments of a recruit (capsule::battle::RecruitMoment order); "messages" names one for each but broke_free
## (break_free gives those, by the checks passed).
MOMENTS = ["check", "recruited", "broke_free", "refused", "blocked", "no_room", "to_party", "to_storage"]
## The moment whose messages are the break_free list.
BREAK_FREE_MOMENT = "broke_free"
## The moments "messages" may name.
MESSAGE_MOMENTS = [m for m in MOMENTS if m != BREAK_FREE_MOMENT]
## The battle changes a recruited record may keep (capsule::battle::RecruitKeep bit order).
KEEPS = ["hp", "status", "resources"]
## The choices of "owned" (index = the RECRUIT_OWNED flag).
OWNED = ["refuse", "allow"]
## The default of "owned".
DEFAULT_OWNED = "refuse"
## The refusals "refusal_consumes" may name, and their RecruitFlag bits (capsule::battle::RECRUIT_CONSUME_*).
REFUSALS = {"refused": 1, "blocked": 2, "no_room": 3}
## status_bonus key of a battler without a primary status (and the default of a status left out).
NO_STATUS = "none"
## A status bonus nobody gives (the pipeline sees it for every status).
DEFAULT_STATUS_BONUS = 0
## Highest status bonus (the pipeline's status_bonus input is a byte).
MAX_STATUS_BONUS = U8_MAX
## Highest item bonus `recruit WHO BONUS` passes (the pipeline's bonus input is a byte; the engine clamps).
MAX_BONUS = U8_MAX
## The pipeline the checks draw against (ruleset_compiler.PIPELINES).
RECRUIT_PIPELINE = "recruit_odds"
## Key of the section in structure.json.
RECRUIT_KEY = "recruit"
## The placeholder a recruit message may add (the moment's value).
VALUE_PLACEHOLDER = "value"

register_keys({"recruit": {"checks", "rate", "status_bonus", "owned", "refusal_consumes", "keep", "item_role",
                           "messages", "break_free",
                           # B2: whether an egg may be recruited (tools/battlec/nursery.py reads it)
                           "allow_eggs"},
               "recruit_messages": set(MESSAGE_MOMENTS)})


def message_id(key: Json, messages: Sequence[str | None], where: str) -> int:
   """!A message key's id.
   @param key       the key
   @param messages  the battle's messages at their ids (None: an optional one left out)
   @param where     location
   @return its id in BattleData::messages
   """
   if not isinstance(key, str) or key not in [m for m in messages if m is not None]:
      raise ManifestError(f"{where}: {key!r} is not one of the battle's messages")
   return list(messages).index(key)


def compile_recruit(src: Json, statuses: Sequence[Mapping[str, Any]], species_fields: Sequence[Mapping[str, Any]],
                    layout: dict[str, Any], item_count: int, messages: Sequence[str | None], pipelines: Sequence[str],
                    where: str) -> dict[str, Any] | None:
   """!The structure's recruit section.
   @param src             structure.json recruit (None: the battle cannot recruit)
   @param statuses        the manifest's statuses (index 0 is "none": the list starts at status 1)
   @param species_fields  compiled species_fields
   @param layout          compiled layout
   @param item_count      items with "none" (the item role must hold the last id)
   @param messages        the battle's messages at their ids
   @param pipelines       the pipelines the ruleset declares (recruit_odds must be among them)
   @param where           location (the structure manifest)
   @return {"checks", "rate_field", "rate_name", "rate_range", "status_bonus" (per status id), "flags", "keep",
           "keep_names", "item_role", "item_role_name", "messages" (per moment, NONE8), "message_keys",
           "break_free" (ids), "break_free_keys"}, or None without a section
   """
   if src is None:
      return None
   w = f"{where}: {RECRUIT_KEY}"
   check_keys(src, "recruit", w)
   if RECRUIT_PIPELINE not in pipelines:
      raise ManifestError(f"{w}: the checks draw against the ruleset's {RECRUIT_PIPELINE} pipeline; the ruleset has "
                          f"none")
   checks = integer(src.get("checks"), 1, MAX_CHECKS, f"{w}.checks")
   rate_name = src.get("rate")
   rate_field = NONE8
   rate_range = (0, 0)
   if rate_name is not None:
      names = [f["name"] for f in species_fields]
      if rate_name not in names:
         raise ManifestError(f"{w}.rate: {rate_name!r} is not a species field (species_fields: "
                             f"{', '.join(names) or 'none'})")
      rate_field = names.index(rate_name)
      rate_range = (0, species_fields[rate_field]["max"])
   # status bonuses: per status id (0: none), the "none" value for a status left out
   bonus_src = src.get("status_bonus", {})
   if not isinstance(bonus_src, dict):
      raise ManifestError(f"{w}.status_bonus: an object of status name -> bonus")
   status_names = [s.get("name") for s in statuses]
   for key in bonus_src:
      if key != NO_STATUS and key not in status_names:
         raise ManifestError(f"{w}.status_bonus: {key!r} is not a status (or {NO_STATUS!r})")
   none = integer(bonus_src.get(NO_STATUS, DEFAULT_STATUS_BONUS), 0, MAX_STATUS_BONUS, f"{w}.status_bonus.none")
   bonus = [none] + [integer(bonus_src.get(n, none), 0, MAX_STATUS_BONUS, f"{w}.status_bonus.{n}")
                     for n in status_names]
   owned = src.get("owned", DEFAULT_OWNED)
   if owned not in OWNED:
      raise ManifestError(f"{w}.owned: one of {', '.join(OWNED)}")
   consumes = src.get("refusal_consumes", [])
   if not isinstance(consumes, list) or any(c not in REFUSALS for c in consumes) or len(set(consumes)) != len(consumes):
      raise ManifestError(f"{w}.refusal_consumes: a list of {', '.join(REFUSALS)} (each once)")
   flags = OWNED.index(owned) | sum(1 << REFUSALS[c] for c in consumes)
   keep_names = src.get("keep", list(KEEPS))
   if not isinstance(keep_names, list) or any(k not in KEEPS for k in keep_names) or \
         len(set(keep_names)) != len(keep_names):
      raise ManifestError(f"{w}.keep: a list of {', '.join(KEEPS)} (each once)")
   item_role_name = src.get("item_role")
   item_role = NONE8
   if item_role_name is not None:
      info = role_info(layout, item_role_name) if isinstance(item_role_name, str) else None
      if info is None:
         raise ManifestError(f"{w}.item_role: {item_role_name!r} is not a role the layout stores")
      if role_max_value(layout, item_role_name) < item_count - 1:
         raise ManifestError(f"{w}.item_role: {item_role_name!r} holds values up to "
                             f"{role_max_value(layout, item_role_name)}; the items need {item_count - 1}")
      item_role = info[0]
   msg_src = src.get("messages", {})
   check_keys(msg_src, "recruit_messages", f"{w}.messages")
   moment_ids = [message_id(msg_src[m], messages, f"{w}.messages.{m}") if m in msg_src else NONE8 for m in MOMENTS]
   free = src.get("break_free", [])
   free = [free] if isinstance(free, str) else free
   if not isinstance(free, list) or len(free) > MAX_BREAK_FREE:
      raise ManifestError(f"{w}.break_free: a message key or a list of at most {MAX_BREAK_FREE}")
   free_ids = [message_id(k, messages, f"{w}.break_free[{i}]") for i, k in enumerate(free)]
   return {"checks": checks, "rate_field": rate_field, "rate_name": rate_name, "rate_range": rate_range,
           "status_bonus": bonus, "bonus_range": (min(bonus), max(bonus)), "flags": flags,
           "owned": owned, "refusal_consumes": consumes, "keep": sum(1 << KEEPS.index(k) for k in keep_names),
           "keep_names": keep_names,
           "item_role": item_role, "item_role_name": item_role_name, "messages": moment_ids,
           "message_keys": {m: msg_src[m] for m in MOMENTS if m in msg_src}, "break_free": free_ids,
           "break_free_keys": list(free)}


def message_args(recruit: Mapping[str, Any] | None) -> dict[str, set[str]]:
   """!The placeholders the recruit section's messages may use besides {user} and {target} (battlec/texts.py
   compile_messages()): {value}.
   @param recruit  compile_recruit() result, or None
   @return {message key: {"value"}}
   """
   if recruit is None:
      return {}
   keys = list(recruit["message_keys"].values()) + list(recruit["break_free_keys"])
   return {k: {VALUE_PLACEHOLDER} for k in keys}


def emit_recruit(recruit: Mapping[str, Any] | None) -> list[str]:
   """!The C++ of capsule::battle::kRecruit (inside namespace capsule::battle, after the battle's tables).
   @param recruit  compile_recruit() result, or None (no checks: the primitive fails)
   @return lines
   """
   if recruit is None:
      return ["const RecruitData kRecruit = {nullptr, 0, 0xFF, 0, 0, 0xFF, 0, "
              f"{{{', '.join(['0xFF'] * len(MOMENTS))}}}, {{{', '.join(['0xFF'] * MAX_BREAK_FREE)}}}}};"]
   free = recruit["break_free"] + [NONE8] * (MAX_BREAK_FREE - len(recruit["break_free"]))
   bonus = ", ".join(str(b) for b in recruit["status_bonus"])
   return ["namespace", "{", f"const uint8_t kRecruitStatusBonus[] = {{{bonus}}};", "} // namespace",
           f"const RecruitData kRecruit = {{kRecruitStatusBonus, {recruit['checks']}, {recruit['rate_field']}, "
           f"0x{recruit['keep']:02X}, {recruit['flags']}, {recruit['item_role']}, {len(recruit['break_free'])}, "
           f"{{{', '.join(str(m) for m in recruit['messages'])}}}, {{{', '.join(str(m) for m in free)}}}}};",
           f"static_assert({len(MOMENTS)} == kRecruitMoments && {MAX_BREAK_FREE} == kMaxBreakFreeMessages);"]
