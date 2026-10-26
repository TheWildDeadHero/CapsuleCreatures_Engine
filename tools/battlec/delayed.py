"""!Delayed hits (D-26 section 1, G1): structure.json `delayed_hits` (capsule::battle::DelayedHitData).

A record is {"name", "at_use": [...], "typed": bool, "crit": bool, "accuracy": n, "miss_message": key}. Effect
logic `delay_hit WHO TURNS NAME;` keeps a snapshot of the move's user at the target's position; when it lands (the
end-of-turn phase `delayed`), the move's land phase `damage` works the hit out through the normal damage pipeline:
  - at_use: what the landing reads from the snapshot (the user's stats, stages, level, types, and the target's
    stat that resists the move's category with its stage: target_defense; DelayedAtUse order); what it leaves out
    is read from the user (and the battler there) when it lands (default: all five);
  - typed: the hit has the move's type (the chart, the same-type check); false: no type, a neutral hit (default
    true);
  - crit: it may be critical (default true);
  - accuracy: the landing's accuracy percent (the target's before_accuracy hooks, then the accuracy pipeline;
    0 or absent: no check), miss_message: the engine message a miss says (default "missed").
The user may have left the field when it lands; its snapshot still hits.
"""
from __future__ import annotations

from collections.abc import Sequence
from typing import Any

from .common import NONE8, Json, ManifestError, check_keys, integer, register_keys

## What a landing may read from the snapshot (capsule::battle::DelayedAtUse bit order): the user's stats, stages,
## level and types, and the target's stat that resists the move's category with its stage (target_defense: the
## battler at the position when the move was used).
AT_USE = ["stats", "stages", "level", "types", "target_defense"]
## A record's flags (capsule::battle::DelayedHitFlag bit order).
HIT_FLAGS = ["typed", "crit"]
## Flags a record has unless it says otherwise.
DEFAULT_FLAGS = {"typed": True, "crit": True}
## The message a missed landing says unless the record names another.
DEFAULT_MISS_MESSAGE = "missed"
## Highest landing accuracy (a percent).
MAX_ACCURACY = 100
## Most delayed hits (8-bit ids: effect `delay_hit`'s id8, below kNone8).
MAX_DELAYED_HITS = NONE8

register_keys({"delayed_hit": {"name", "at_use", "typed", "crit", "accuracy", "miss_message"}})


def compile_delayed_hits(entries: Json, where: str, messages: Sequence[str | None]) -> list[dict[str, Any]]:
   """!The structure's delayed hits.
   @param entries   structure.json delayed_hits (None: none)
   @param where     location (the structure manifest)
   @param messages  the engine messages the manifest gives, at their Msg ids (None: left out)
   @return [{"name", "at_use", "flags", "accuracy", "missed", "miss_message"}]
   """
   if entries is None:
      return []
   if not isinstance(entries, list) or len(entries) > MAX_DELAYED_HITS:
      raise ManifestError(f"{where}: delayed_hits must be a list of at most {MAX_DELAYED_HITS} records")
   out = []
   names: set[str] = set()
   for i, d in enumerate(entries):
      w = f"{where}: delayed_hits[{i}]"
      check_keys(d, "delayed_hit", w)
      name = d.get("name")
      if not isinstance(name, str) or not name or name in names:
         raise ManifestError(f"{w}: needs a name of its own")
      names.add(name)
      at_use = d.get("at_use", list(AT_USE))
      if not isinstance(at_use, list) or any(a not in AT_USE for a in at_use) or len(set(at_use)) != len(at_use):
         raise ManifestError(f"{w}.at_use: a list of {', '.join(AT_USE)} (each once)")
      flags = 0
      for k, f in enumerate(HIT_FLAGS):
         v = d.get(f, DEFAULT_FLAGS[f])
         if not isinstance(v, bool):
            raise ManifestError(f"{w}.{f}: true or false")
         flags |= (1 << k) if v else 0
      accuracy = integer(d.get("accuracy", 0), 0, MAX_ACCURACY, f"{w}.accuracy")
      miss = d.get("miss_message", DEFAULT_MISS_MESSAGE)
      if not isinstance(miss, str):
         raise ManifestError(f"{w}.miss_message: an engine message key")
      known = [m for m in messages if m is not None]
      if miss not in known:
         raise ManifestError(f"{w}.miss_message: {miss!r} is not one of the battle's messages")
      out.append({"name": name, "at_use": sum(1 << AT_USE.index(a) for a in at_use), "flags": flags,
                  "accuracy": accuracy, "missed": list(messages).index(miss), "miss_message": miss})
   return out


def emit_delayed_hits(hits: Sequence[Any]) -> list[str]:
   """!The C++ table kDelayedHits (DelayedHitData {at_use, flags, accuracy, missed}).
   @param hits  compile_delayed_hits() result
   @return lines (inside the generated anonymous namespace)
   """
   rows = [f"    {{0x{h['at_use']:02X}, 0x{h['flags']:02X}, {h['accuracy']}, {h['missed']}}},   // {h['name']}"
           for h in hits]
   return ["const DelayedHitData kDelayedHits[] = {", *(rows or ["    {0, 0, 0, 0xFF},"]), "};"]
