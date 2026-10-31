"""!The battle's effect logic (D-22 decision 1, D-23 section 1, E4b, E5):
every move, passive, item, status, weather, volatile, side condition and
field condition is an owner of the effect bank (capsule::battle::effect::Bank
in BattleData::effects), in that order. Its logic comes from the manifest's
`.ceffect` sources ("effects": [paths]); a condition's declarative fields
expand into handlers placed in front of its own (battlec/conditions.py).
Point handlers stay in the point handler table (battlec/pipelines.py) and
join their owner's handlers in the bank. An owner without logic has no
handlers, except a damaging move (a move of a damaging category with
power): without logic it hits each target once (STANDARD_HIT), and logic
without a hit phase gets `hit { damage; }`.

Owner texts (`text NAME;`) are the manifest's "texts" {name: text with {user}
/ {target}} and, inside a condition owner, its record's own texts; animation
cues (`cue NAME;`) its "cues" [names] and owner tags (`tags [...]`, or a
condition record's "tags") its "tags" [names]. A `.ceffect` file may hold
moves, passives, items and every condition kind; F1: global owners (the game's own encounter rules).
A source the manifest lists must exist; diagnostics name its file, line and
column.
"""
from __future__ import annotations

from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

import effect_compiler as ec
from ruleset_compiler import POINT_VALUE_RANGE
from sequence_compiler import asset_key

from .common import NONE8, Json, ManifestError, Sources, Texts
from .conditions import (MOVE_FLAGS, VOLATILES, status_template, text_only_template, volatile_template,
                         weather_template)
from .pipelines import POINT_BASE
from .texts import ABSENT_MESSAGE, MESSAGES

## Owner kinds a battle's `.ceffect` sources may hold, with the manifest list naming them.
LOGIC_KINDS = {"move": "moves", "passive": "passives", "item": "items", "status": "statuses", "weather": "weathers",
               "volatile": "volatiles", "side_condition": "side_conditions", "field_condition": "field_conditions"}
## The owner kind of a game's own rules (F1: `global "NAME" { on encounter { ... } }`; dispatched by the encounter
## hooks and, F3, the step effects' steps hook only).
GLOBAL_KIND = "global"
## The hooks a battle dispatches to global owners (F1: the encounter rolls, capsule/battle/encounter_lead.h; F3: the
## step effects, capsule/battle/step_effects.h).
GLOBAL_HOOKS = ("encounter", "encounter_made", "steps",
                # B1 (D-28 section 1): a combination's result built from its inputs
                "combine",
                # B2 (D-28 section 3): an egg cycle's steps off each egg (capsule/combine/eggs.h)
                "hatch")
## Owner kinds that have no point source in the ruleset's points yet (their point handlers would never run; E13b:
## moves, volatiles and side conditions have theirs).
NO_POINT_KINDS = {"field_condition"}
## Volatiles with a counter before E5 (a catalog without condition records: the studio until S5b).
VOLATILE_COUNTERS = ("confusion", "protect")
## Most owner texts (15-bit ids: capsule::battle::kConditionText marks a status's or weather's own texts).
MAX_TEXTS = 0x7FFF
## Most cues and tags.
MAX_CUES = 0xFF
## See MAX_CUES (Owner::tags bits).
MAX_TAGS = ec.MAX_TAGS
## Placeholders of owner texts.
TEXT_ARGS = {"user", "target"}
## Hooks whose programs may not suspend in this engine yet (condition residual phases run inside the end of turn).
NO_SUSPEND_HOOKS = ec.YIELD_HOOKS
## Positions of `.ceffect` nodes (effect_compiler.Positions) and the file of each owner member, by node id.
NodeFiles = dict[int, str]
## The hit phase of a damaging move without logic: one unit per target that hits once while the target and the
## user are there (each yield ends a unit: the legacy compiler's default damage component, so its bank stays the
## same).
STANDARD_HIT = """move "standard" {
   hit {
      if target.alive and user.alive {
         damage;
         yield;
      }
      yield;
   }
}
"""
## The hit phase a damaging move's logic gets when it has none.
DEFAULT_HIT = {"phase": "hit", "body": [{"stmt": "damage"}]}
## Suffix of a condition template's source name ("status Poison (template)": its diagnostics and debug line table
## name it; the bench's VM records give it as "template", apps/battle_bench/trace.cpp kTemplateSuffix).
TEMPLATE_SUFFIX = " (template)"


def load_sources(paths: Json, base: Path, where: str, deps: list[Path] | None,
                 sources: Sources | None = None, positions: ec.Positions | None = None,
                 files: NodeFiles | None = None) -> list[dict[str, Any]]:
   """!Parses the manifest's `.ceffect` sources (an editor's unsaved text in place of a file, as compile_battle()
   takes unsaved manifests). A listed source that is neither unsaved nor on disk is an error (never an empty
   source).
   @param paths      the "effects" list (paths relative to the manifest)
   @param base       the manifest's folder
   @param where      location
   @param deps       files read (appended), or None
   @param sources    {absolute Path: data} replacing files; a `.ceffect` source's data is its text (str)
   @param positions  node positions (filled), for diagnostics with lines and columns, or None
   @param files      id(owner member) -> its source path as listed (filled), or None
   @return owner nodes, in file order
   """
   if paths is None:
      return []
   if not isinstance(paths, list) or not all(isinstance(p, str) for p in paths):
      raise ManifestError(f"{where}: effects must be a list of .ceffect paths")
   given = sources or {}
   owners = []
   for rel in paths:
      path = (base / rel).resolve()
      if deps is not None:
         deps.append(path)
      if path in given and not isinstance(given[path], str):
         raise ManifestError(f"{where}: the unsaved source of {rel} must be its text")
      if path not in given and not path.is_file():
         raise ManifestError(f"{where}: effects lists {rel}, which does not exist")
      try:
         text = given[path] if path in given else path.read_text(encoding="utf-8")
         ast, pos = ec.parse_positions(text, rel)
      except OSError as e:
         raise ManifestError(f"{where}: cannot read {rel} ({e.strerror})")
      except ec.EffectError as e:
         raise ManifestError(f"{where}: {e}")
      if positions is not None:
         positions.update(pos)
      for o in ast["owners"]:
         if "owner" not in o:
            continue   # a comment
         if files is not None:
            for h in o.get("handlers", []):
               files[id(h)] = rel
         kind = o["owner"]["kind"]
         if kind not in LOGIC_KINDS and kind != GLOBAL_KIND:
            raise ManifestError(f"{rel}: {kind} {o['owner']['name']!r}: a battle's .ceffect holds "
                                f"{', '.join([*LOGIC_KINDS, GLOBAL_KIND])} owners")
         if kind == GLOBAL_KIND and any(h.get("on") not in GLOBAL_HOOKS for h in o.get("handlers", [])):
            raise ManifestError(f"{rel}: global {o['owner']['name']!r}: a battle runs a global owner's "
                                f"{', '.join(GLOBAL_HOOKS)} handlers only (no other hook, point or sub)")
         owners.append(o)
   return owners


def split_points(owners: Sequence[dict[str, Any]]) -> tuple[dict[tuple[str, str], dict[str, Any]],
                                                              dict[tuple[str, str], list[dict[str, Any]]]]:
   """!Indexes `.ceffect` owners and takes their point members out (they are point handlers, battlec/pipelines.py).
   @param owners  owner nodes
   @return ({(kind, name): owner without points}, {(kind, name): point members})
   @throws ManifestError when an owner has logic twice
   """
   logic: dict[tuple[str, str], dict[str, Any]] = {}
   points: dict[tuple[str, str], list[dict[str, Any]]] = {}
   for o in owners:
      key = (o["owner"]["kind"], o["owner"]["name"])
      if key in logic:
         raise ManifestError(f"{key[0]} {key[1]!r} has logic twice")
      members = o.get("handlers", [])
      pts = [h for h in members if "point" in h]
      if pts and key[0] in NO_POINT_KINDS:
         raise ManifestError(f"{key[0]} {key[1]!r}: {key[0].replace('_', ' ')}s have no point source (point handlers "
                             f"belong to passives, items, statuses, weathers, moves, volatiles and side conditions)")
      logic[key] = {**o, "handlers": [h for h in members if "point" not in h]}
      points[key] = pts
   return logic, points


def texts_of(b: Mapping[str, Any], where: str, enc: Texts) -> tuple[list[str], list[list[int]]]:
   """!The manifest's owner texts.
   @param b      the manifest
   @param where  location
   @param enc    this compile's text encoders
   @return (names, encoded texts) in declaration order
   """
   src = b.get("texts", {})
   if not isinstance(src, dict) or len(src) > MAX_TEXTS:
      raise ManifestError(f"{where}: texts must map names to texts (at most {MAX_TEXTS})")
   names = list(src)
   return names, [enc.text(src[k], f"{where}: texts.{k}", TEXT_ARGS) for k in names]


def names_list(b: Mapping[str, Any], key: str, limit: int, where: str) -> list[str]:
   """!A manifest list of plain names (cues, tags).
   @param b      the manifest
   @param key    its key
   @param limit  most names
   @param where  location
   @return the names
   """
   v = b.get(key, [])
   if not isinstance(v, list) or not all(isinstance(x, str) for x in v) or len(set(v)) != len(v) or len(v) > limit:
      raise ManifestError(f"{where}: {key} must be a list of distinct names (at most {limit})")
   return list(v)


def battle_catalog(names: Mapping[str, Sequence[str]], stage_names: Sequence[str], layout: Mapping[str, Any],
                   points: Mapping[str, Any], texts: Sequence[str], cues: Sequence[str],
                   tags: Sequence[str], conditions: Mapping[str, Sequence[Any]] | None = None,
                   move_flags: Sequence[str] | None = None) -> ec.Catalog:
   """!The names the battle's effect logic uses, with the ids the engine's host functions give them.
   @param names        the battle's names by list
   @param stage_names  stage names in index order
   @param layout       compiled layout (role IDs)
   @param points       the bound ruleset's points (ids are their order)
   @param texts        owner text names
   @param cues         cue names
   @param tags         tag names
   @param conditions   conditions.condition_entries() (counter / word slots, own texts, durations), or None: the
                       statuses and weathers of @p names and the engine's former volatiles
   @param move_flags   the manifest's move flags, or None: names["move_flags"], else the engine's former set
   @return the catalog
   """
   if conditions is None:
      vols = names.get("volatiles", VOLATILES)
      conditions = {"statuses": list(names["statuses"]), "weathers": list(names["weathers"]),
                    "volatiles": [{"name": v, "counter": v in VOLATILE_COUNTERS} for v in vols],
                    "side_conditions": list(names.get("side_conditions", [])),
                    "field_conditions": list(names.get("field_conditions", []))}
   flags = list(move_flags if move_flags is not None else names.get("move_flags", MOVE_FLAGS))
   ordered = sorted(points.items(), key=lambda kv: kv[1]["id"])
   return ec.Catalog({
      "types": list(names["types"]), "categories": list(names["categories"]),
      "statuses": list(conditions["statuses"]), "volatiles": list(conditions["volatiles"]),
      "weathers": list(conditions["weathers"]), "side_conditions": list(conditions["side_conditions"]),
      "field_conditions": list(conditions["field_conditions"]), "moves": list(names["moves"]),
      # G4: species in the battle data's order (sorted by key, capsule::battle::kData.species: the ids
      # `holder.species` reads), not the manifest's
      "items": list(names["items"]), "passives": list(names["passives"]),
      "species": sorted(names["species"], key=lambda n: asset_key(f"species:{n}")),
      "stats": list(names["stats"]), "stages": list(stage_names), "move_flags": flags,
      # the messages the manifest gives at their Msg ids (ABSENT_MESSAGE: an optional one it leaves out)
      "tags": list(tags), "texts": list(texts),
      "messages": [ABSENT_MESSAGE if m is None else m for m in names.get("messages", MESSAGES)],
      "cues": list(cues),
      # C5: an unstored custom role says so (effect logic reads it, never writes it)
      "roles": [{"name": n, "id": i, **({"stored": False} if n in layout.get("unstored_ids", {}) else {})}
                for n, i in layout["ids"].items()],
      # E13a / E13b: the species' data columns (`holder.species_field(NAME)`)
      "species_fields": list(names.get("species_fields", [])),
      # D-25: resources by name (scope battler or move; `holder.resource(NAME)`, restore, raise)
      "resources": [dict(r) if isinstance(r, Mapping) else {"name": r, "scope": "battler"}
                    for r in names.get("resources", [])],
      # every point handler sees its value clamped to [0, 65535] (run_point)
      "points": [{"name": n, "range": list(POINT_VALUE_RANGE)} for n, _p in ordered],
      # G1: the structure's delayed hits (effect `delay_hit`)
      "delayed_hits": list(names.get("delayed_hits", [])),
      # G4: the species' form names (effect `form`; "base" is the species' own form, id 0)
      "forms": list(names.get("forms", ["base"])),
      # F1: the timed field effects (effect `field_effect start NAME N`, `field.effect_active(NAME)`)
      "field_effects": list(names.get("field_effects", [])),
      # F3: the step effects (`field.step_effect == NAME` in an `on steps` handler)
      "step_effects": list(names.get("step_effects", [])),
      # B1: the combination methods (`field.combine_method == NAME`) and their slot roles (`input.ROLE`)
      "combine_methods": list(names.get("combine_methods", [])),
      "combine_slots": [dict(e) for e in names.get("combine_slots", [])],
   })


def empty_owner(kind: str, name: str, announce: bool = False) -> dict[str, Any]:
   """!An owner without logic.
   @param kind      owner kind
   @param name      its name
   @param announce  its announce flag
   @return the owner node
   """
   node: dict[str, Any] = {"owner": {"kind": kind, "name": name}, "handlers": []}
   if announce:
      node["announce"] = True
   return node


def standard_hit(name: str) -> dict[str, Any]:
   """!The owner of a damaging move without logic (STANDARD_HIT).
   @param name  the move
   @return the owner node
   """
   node = ec.parse(STANDARD_HIT)["owners"][0]
   node["owner"]["name"] = name
   return node


def with_template(node: dict[str, Any] | None, template: str, kind: str, name: str,
                  tags: Sequence[str]) -> dict[str, Any]:
   """!A condition's owner: the handlers its declarative fields expand into, then its own `.ceffect` handlers; its
   record's tags and its code's.
   @param node      its `.ceffect` owner (point members taken out), or None
   @param template  conditions.*_template() source ("" for none)
   @param kind      owner kind
   @param name      its name
   @param tags      the record's tags
   @return the owner node
   """
   own = node or empty_owner(kind, name)
   if not template and not tags:
      return own
   generated = ec.parse(template, f"{kind} {name}{TEMPLATE_SUFFIX}")["owners"][0]["handlers"] if template else []
   merged = {**own, "handlers": generated + list(own.get("handlers", []))}
   all_tags = list(tags) + [t for t in own.get("tags", []) if t not in tags]
   if all_tags:
      merged["tags"] = all_tags
   return merged


def compile_logic(names: Mapping[str, Sequence[str]], moves: Sequence[dict[str, Any]],
                  passives: Sequence[dict[str, Any]],
                  items: Sequence[dict[str, Any] | None], statuses: Sequence[dict[str, Any] | None],
                  weathers: Sequence[dict[str, Any]], logic: Mapping[tuple[str, str], dict[str, Any]],
                  catalog: ec.Catalog, handlers: Sequence[Mapping[str, int]],
                  categories: Sequence[Mapping[str, int]], where: str,
                  conditions: Mapping[str, Sequence[dict[str, Any]]] | None = None,
                  positions: ec.Positions | None = None, files: NodeFiles | None = None) -> dict[str, Any]:
   """!Compiles every owner's logic into one bank and sets each compiled entry's "owner" (its owner index).
   @param names       names by list
   @param moves       compiled moves (from content.compile_moves)
   @param passives    compiled passives
   @param items       compiled items (index 0 = none)
   @param statuses    compiled statuses (index 0 = none)
   @param weathers    compiled weathers
   @param logic       `.ceffect` owners by (kind, name), point members taken out
   @param catalog     battle_catalog()
   @param handlers    the point handler table
   @param categories  compiled categories (a move's default hit)
   @param where       location
   @param conditions  {"volatiles", "side_conditions", "field_conditions"}: compiled records, or None (none)
   @param positions   node positions of the `.ceffect` sources (load_sources()), for diagnostics
   @param files       their files by owner member (load_sources())
   @return {"bank": effect_compiler.Bank, "owners": owner count}
   """
   conditions = conditions or {}
   records = {"volatiles": conditions.get("volatiles", []), "side_conditions": conditions.get("side_conditions", []),
              "field_conditions": conditions.get("field_conditions", [])}
   known = {**names, **{k: [r["source_name"] for r in v] for k, v in records.items()}}
   for kind, name in logic:
      if kind == GLOBAL_KIND:
         continue   # F1: the game's own rules have no list to be in
      if name not in known.get(LOGIC_KINDS[kind], []):
         raise ManifestError(f"{where}: .ceffect logic of an unknown {kind} {name!r} (not in the manifest's "
                             f"{LOGIC_KINDS[kind]})")
   nodes: list[dict[str, Any]] = []
   entries: list[dict[str, Any]] = []      # the compiled entry of each owner (its point handlers, its "owner")
   for i, m in enumerate(moves):
      name = names["moves"][i]
      node = logic.get(("move", name))
      damaging = categories[m["category"]]["offense"] != NONE8 and m["power"]
      if node is None:
         node = standard_hit(name) if damaging else empty_owner("move", name)
      elif damaging and not any(h.get("phase") == "hit" for h in node["handlers"]):
         node = {**node, "handlers": node["handlers"] + [DEFAULT_HIT]}
      nodes.append(node)
      entries.append(m)
   for kind, compiled, key in (("passive", passives, "passives"), ("item", items[1:], "items")):
      for i, e in enumerate(compiled):
         name = names[key][i]
         node = logic.get((kind, name))
         if node is None:
            node = empty_owner(kind, name, e["announce"])
         elif e["announce"]:
            node = {**node, "announce": True}
         nodes.append(node)
         entries.append(e)
   for kind, compiled, key, template in (("status", statuses[1:], "statuses", status_template),
                                         ("weather", weathers, "weathers", weather_template)):
      for i, e in enumerate(compiled):
         name = names[key][i]
         nodes.append(with_template(logic.get((kind, name)), template(name, e), kind, name, e["tags"]))
         entries.append(e)
   for kind, key in (("volatile", "volatiles"), ("side_condition", "side_conditions"),
                     ("field_condition", "field_conditions")):
      for e in records[key]:
         name = e["source_name"]
         source = volatile_template(name, e) if kind == "volatile" else text_only_template(kind, name, e)
         nodes.append(with_template(logic.get((kind, name)), source, kind, name, e["tags"]))
         entries.append(e)
   # F1: the game's global owners (the encounter hooks' rules), after every other owner (their ids stay)
   for (kind, name), node in logic.items():
      if kind == GLOBAL_KIND:
         nodes.append(node)
         entries.append({"name": name})
   comp = ec.Compiler({"schema": ec.SCHEMA_AST, "owners": nodes}, catalog, where, positions, files).run()
   errors = comp.errors()
   if errors:
      raise ManifestError("; ".join(str(d) for d in errors))
   for oi, (o, e) in enumerate(zip(comp.owner_list(), entries)):
      e["owner"] = oi
      # lifecycle handlers in declaration order (with their flags: per_foe), then the point handlers
      hs = list(o["handlers"])
      # the owner's point handlers (pipelines.PointHandlers: hook POINT_BASE + point, first = function)
      first, count = point_range(e)
      for h in handlers[first:first + count]:
         hs.append((h["hook"] - POINT_BASE, ec.HANDLER_POINT, h["first"]))
      o["handlers"] = hs
   for p in comp.program_list():
      owner = comp.owner_list()[p.owner]
      if owner["kind"] in ec.CONDITION_KINDS and p.hook in NO_SUSPEND_HOOKS and p.flags & ec.PROGRAM_YIELDS:
         raise ManifestError(f"{where}: {p.label}: residual phases cannot yield or ask in this engine yet")
   return {"bank": ec.Bank(comp), "owners": len(nodes)}


def point_range(e: Mapping[str, Any]) -> tuple[int, int]:
   """!(first, count) of a compiled entry's point handlers in the point handler table.
   @param e  compiled move, passive, item, status or weather
   @return the range ((0, 0) for a move)
   """
   if "first_handler" in e:
      return e["first_handler"], e["handler_count"]
   if "first" in e:
      return e["first"], e["count"]
   return 0, 0
