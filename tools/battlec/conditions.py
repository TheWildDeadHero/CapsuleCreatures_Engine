"""!Conditions of the battle compiler (D-22 A3-A5, D-23 section 2): primary
statuses (index 0 = none), volatiles, weathers, side conditions and field
conditions as manifest records, plus the move flags and the record texts.

A record keeps what the engine needs (names, texts, a volatile's counter /
word slot and when it is cleared, a side condition's layer cap; the
capsule::battle::*Data structs). Its declarative fields are TEMPLATES: they
expand at compile time into effect code (a `.ceffect` owner block,
template_source()), which tools/battlec/logic.py puts in front of the
condition's own `.ceffect` handlers. The expansion keeps the engine's former
order of draws and events exactly (the old can_act() and end-of-turn code):

- status: immune_types -> try_start `immune`; turns [min, max] -> start rolls
  status_counter, before_move counts it down (cure at 0, else skip);
  thawed_by_move (a move flag) / cure / skip odds -> before_move (in that
  order, one branch); thawed_by_hit (a type) -> damaging_hit cure; residual
  (+ escalating, a cap) -> residual damage with its text; escalating and
  cured_on_switch -> switch_out.
- volatile: duration "turn" (its bit ends with the turn) | "until_switch" |
  [min, max] (start sets its counter, from the vol_on turns or a roll; it
  counts down before each move, or at the end of each turn with
  "counts": "end_of_turn", and the volatile ends at 0); skip (true or odds) ->
  before_move skip; chain {den, cap, kept_by} (a volatile that stays on:
  until_switch) -> each `vol_on` of it (try_start the first time, already
  after that) rolls 1 in den^n for its counter n > 0 (the run that started
  it fails, the counter resets) and counts up to cap; before a move without
  the kept_by flag the counter resets; texts start / already / end / skip.
- weather: residual + immune_types -> field_residual damage with its hurts
  text; turns -> the duration `weather NAME;` gives it.
- side / field condition: turns -> the duration `side_on` / `field_on` give
  it; texts start / already / end.

Texts: a status's texts {inflicted, residual, skip, cured, already} and a
weather's {started, continues, ended, hurts} are the engine's StatusText /
WeatherText events (`text skip;` in the owner emits the event:
capsule::battle::kConditionText). A volatile's, side or field condition's
texts are owner texts (Text events) or {"message": NAME}, an engine message
(the templates `say` it at the holder).
"""
from __future__ import annotations

import json
from collections.abc import Mapping, Sequence
from typing import Any

from .chart import types_mask
from .common import U8_MAX, Json, Lookups, ManifestError, Texts, check_keys, integer, odds, q8, register_keys

## Volatiles of the engine before E5 (the sample's; the studio's catalog reads them until S5b).
VOLATILES = ["flinch", "confusion", "protect"]
## The move flags of the engine before E5 (the sample declares these; the studio's catalog until S5b).
MOVE_FLAGS = ["contact", "sound", "protect", "thaws", "high_crit", "protection"]
## Most statuses (capsule::battle::kMaxStatuses; 6-bit ids).
MAX_STATUSES = 63
## Most volatiles (capsule::battle::kMaxVolatiles: Battler::vol bits).
MAX_VOLATILES = 32
## Volatile counter slots (capsule::battle::kMaxVolatileCounters).
MAX_COUNTERS = 8
## Volatile word slots (capsule::battle::kMaxVolatileWords).
MAX_WORDS = 4
## Volatile source slots (capsule::battle::kMaxBoundVolatiles: bound volatiles, G1).
MAX_BOUND = 4
## Most side conditions (capsule::battle::kMaxSideConds).
MAX_SIDE_CONDS = 16
## Most field conditions (capsule::battle::kMaxFieldConds).
MAX_FIELD_CONDS = 16
## Most move flags (capsule::battle::kMaxMoveFlags: MoveData::flags bits).
MAX_MOVE_FLAGS = 16
## Longest status length in turns (the status counter is a byte in battle; the record keeps what fits).
MAX_TURNS = U8_MAX
## Default cap of an escalating status's multiplier (neutral, D-22 decision 8: none below the battle's byte
## counter; a game states its own: "escalating": {"cap": n}).
DEFAULT_ESCALATION_CAP = U8_MAX
## Most chain steps (the odds table is one pick of den^1 .. den^cap: effect_compiler.MAX_PICK entries).
MAX_CHAIN_CAP = 8
## Largest chain odds denominator (den^cap must fit int32).
MAX_CHAIN_ODDS = 0x7FFFFFFF
## Entries of a [min, max] pair.
PAIR_LEN = 2
## Smallest chain odds denominator (1 would always work).
MIN_CHAIN_DEN = 2
## Bits of the Q8 shift in a share `holder.max_hp * q8 >> 8`.
Q8_SHIFT = 8
## Status texts in StatusText extra order (capsule::battle::kStatus*).
STATUS_TEXTS = ["inflicted", "residual", "skip", "cured", "already"]
## Weather texts in WeatherText extra order (capsule::battle::kWeather*).
WEATHER_TEXTS = ["started", "continues", "ended", "hurts"]
## Condition text id flag (capsule::battle::kConditionText).
CONDITION_TEXT = 0x8000
## Placeholders of status texts.
STATUS_TEXT_ARGS = {"user", "status"}
## Placeholders of weather texts.
WEATHER_TEXT_ARGS = {"user"}
## Placeholders of volatile / side / field condition texts (owner texts).
CONDITION_TEXT_ARGS = {"user", "target"}
## Volatile flag bits (capsule::battle::VolatileFlag).
VOLATILE_TURN = 1
## See VOLATILE_TURN.
VOLATILE_SWITCH_OUT = 2
## See VOLATILE_TURN.
VOLATILE_FAINT = 4
## See VOLATILE_TURN.
VOLATILE_PASSES = 8
## See VOLATILE_TURN.
VOLATILE_HUD = 16
## See VOLATILE_TURN (G1: `bound_to`).
VOLATILE_BOUND = 32
## What a volatile may be bound to (G1: `bound_to`): it ends when that battler leaves the field or faints.
BOUND_TO = ("source",)
## Events a volatile's clears_on may name, and their flag.
CLEARS_ON = {"switch_out": VOLATILE_SWITCH_OUT, "faint": VOLATILE_FAINT}
## Durations a volatile may name as a word.
DURATIONS = ("turn", "until_switch")
## Where a volatile with a [min, max] duration counts down.
COUNTS = ("before_move", "end_of_turn")
## "No slot" of a volatile's counter / word (capsule::battle::kNoSlot).
NO_SLOT = 0xFF
## Text slots a volatile's template shows (start / already / end and the lost turn's skip; its texts may hold more:
## owner texts its logic shows with `text`).
VOLATILE_TEXT_SLOTS = ("start", "already", "end", "skip")
## Text slots a side or field condition's template shows.
CONDITION_TEXT_SLOTS = ("start", "already", "end")
## Indent of generated template code.
INDENT = "   "

register_keys({
   "status": {"name", "texts", "residual", "skip", "cure", "turns", "immune_types", "escalating", "thawed_by_move",
              "thawed_by_hit", "cured_on_switch", "color", "handlers", "tags"},
   "status_texts": set(STATUS_TEXTS),
   "escalating": {"cap"},
   "weather": {"name", "texts", "residual", "immune_types", "turns", "handlers", "tags"},
   "weather_texts": set(WEATHER_TEXTS),
   "volatile": {"name", "duration", "counts", "counter", "word", "clears_on", "passes", "skip", "chain", "texts",
                "hud", "tags", "bound_to"},
   "chain": {"den", "cap", "kept_by"},
   "side_condition": {"name", "turns", "layers", "texts", "tags"},
   "field_condition": {"name", "turns", "texts", "tags"},
})


def quoted(name: str) -> str:
   """!A name as effect code writes it (always quoted, so any manifest name works).
   @param name  the name
   @return the code
   """
   return json.dumps(name)


def owner_block(kind: str, name: str, handlers: Sequence[str]) -> str:
   """!A `.ceffect` owner block of generated handlers.
   @param kind      owner kind
   @param name      its name
   @param handlers  handler texts (each "on HOOK [when C] { ... }" on one or more lines)
   @return the source ("" without handlers)
   """
   if not handlers:
      return ""
   body = "\n".join(INDENT + line for h in handlers for line in h.splitlines())
   return f"{kind} {quoted(name)} {{\n{body}\n}}\n"


def branch_chain(parts: Sequence[tuple[str | None, str]]) -> str:
   """!An if / else-if chain: each (condition, body); a None condition is the unconditional last branch.
   @param parts  (condition or None, body statements)
   @return the statements
   """
   out = []
   for k, (cond, body) in enumerate(parts):
      if cond is None:
         out.append(body if k == 0 else f"else {{ {body} }}")
         break
      out.append(f"{'if' if k == 0 else 'else if'} {cond} {{ {body} }}")
   return " ".join(out)


def name_list(v: Json, where: str) -> list[str]:
   """!A list of distinct names.
   @param v      the value
   @param where  location
   @return the names
   """
   if not isinstance(v, list) or not all(isinstance(x, str) for x in v) or len(set(v)) != len(v):
      raise ManifestError(f"{where} must be a list of distinct names")
   return list(v)


def compile_move_flags(b: Mapping[str, Any], where: str) -> list[str]:
   """!The manifest's move flags (MoveData::flags bit n = move_flags[n]; effect `move.has(flag)`).
   @param b      the manifest
   @param where  location
   @return the names
   """
   flags = name_list(b.get("move_flags", []), f"{where}: move_flags")
   if len(flags) > MAX_MOVE_FLAGS:
      raise ManifestError(f"{where}: at most {MAX_MOVE_FLAGS} move_flags (MoveData::flags is 16 bits)")
   return flags


def type_test(types: Sequence[str]) -> str:
   """!The condition "the holder has one of these types".
   @param types  type names
   @return the expression
   """
   return " or ".join(f"holder.has_type({quoted(t)})" for t in types)


def odds_test(n: int, m: int) -> str | None:
   """!A `chance(n, m)` condition, as the engine's rng::chance() rolls it (n >= m: always, no draw).
   @param n  successes
   @param m  outcomes
   @return the condition, or None for "always"
   """
   return None if n >= m else f"chance({n}, {m})"


def status_template(name: str, s: Mapping[str, Any]) -> str:
   """!The effect code a status's declarative fields expand into (see the module comment).
   @param name  the status
   @param s     compiled status ("template" fields)
   @return the `.ceffect` owner block ("" when it has none)
   """
   t = s["template"]
   H = []
   if t["immune"]:
      H.append(f"on try_start when {type_test(t['immune'])} {{ immune; }}")
   tmin, tmax = t["turns"]
   if tmax:
      H.append(f"on start {{ holder.status_counter = between({tmin}, {tmax}); }}")
   parts: list[tuple[str | None, str]] = []
   if t["thaw_flag"] is not None:
      parts.append((f"move.has({quoted(t['thaw_flag'])})", "cure holder;"))
   if tmax:
      # a length counts down before each move: cured at 0, else the turn is lost
      parts.append((None, "if holder.status_counter > 0 { holder.status_counter -= 1; } "
                          "if holder.status_counter == 0 { cure holder; } else { text skip; stop; }"))
   else:
      for (n, m), body in ((t["cure"], "cure holder;"), (t["skip"], "text skip; stop;")):
         if n == 0:
            continue   # never (no draw)
         parts.append((odds_test(n, m), body))
         if n >= m:
            break      # always: the branches after it never run
   if parts:
      H.append(f"on before_move {{ {branch_chain(parts)} }}")
   if t["thaw_type"] is not None:
      H.append(f"on damaging_hit when move.type == {quoted(t['thaw_type'])} {{ cure holder; }}")
   if t["residual"]:
      share = f"holder.max_hp * {t['residual']}"
      grow = ""
      if t["escalation_cap"]:
         grow = f"holder.status_aux = min(holder.status_aux + 1, {t['escalation_cap']}); "
         share += " * holder.status_aux"
      H.append(f"on residual {{ {grow}text residual; damage_hp holder max(1, {share} >> {Q8_SHIFT}); }}")
   leave = []
   if t["escalation_cap"]:
      leave.append("holder.status_aux = 0;")
   if t["cured_on_switch"]:
      leave.append("cure holder quiet;")
   if leave:
      H.append(f"on switch_out {{ {' '.join(leave)} }}")
   return owner_block("status", name, H)


def compile_statuses(entries: Sequence[Json], n: Lookups, where: str, enc: Texts,
                     move_flags: Sequence[str]) -> list[dict[str, Any] | None]:
   """!Statuses (index 0 = none).
   @param entries     the manifest's statuses
   @param n           lookups
   @param where       location
   @param enc         this compile's text encoders (with terms)
   @param move_flags  the manifest's move flags (thawed_by_move names one)
   @return compiled statuses, None first ("template": the declarative fields, status_template())
   """
   if len(entries) > MAX_STATUSES:
      raise ManifestError(f"{where}: at most {MAX_STATUSES} statuses")
   statuses: list[dict[str, Any] | None] = [None]
   for i, s in enumerate(entries):
      w = f"{where}: statuses[{i}]"
      check_keys(s, "status", w)
      tx = s.get("texts", {})
      check_keys(tx, "status_texts", f"{w}.texts")
      for k in ("inflicted", "cured", "already"):
         if k not in tx:
            raise ManifestError(f"{w}.texts: needs {k}")
      sk = odds(s.get("skip", 0), f"{w}.skip", U8_MAX) if s.get("skip") else (0, 0)
      cu = odds(s.get("cure", 0), f"{w}.cure", U8_MAX) if s.get("cure") else (0, 0)
      turns_ = s.get("turns", [0, 0])
      tmin = integer(turns_[0], 0, MAX_TURNS, f"{w}.turns")
      tmax = integer(turns_[1], tmin, MAX_TURNS, f"{w}.turns")
      if (tmin == 0) != (tmax == 0):
         raise ManifestError(f"{w}.turns: [0, 0] (no length) or both at least 1")
      if tmax and (s.get("skip") or s.get("cure")):
         raise ManifestError(f"{w}: a status with turns skips every turn until it ends; it takes no skip or "
                             f"cure odds")
      thaw_flag = s.get("thawed_by_move")
      if thaw_flag is not None and (not isinstance(thaw_flag, str) or thaw_flag not in move_flags):
         raise ManifestError(f"{w}.thawed_by_move: name the move flag that cures it (move_flags: "
                             f"{', '.join(move_flags) or 'none'})")
      esc = s.get("escalating", False)
      if isinstance(esc, dict):
         check_keys(esc, "escalating", f"{w}.escalating")
         cap = integer(esc.get("cap", DEFAULT_ESCALATION_CAP), 1, U8_MAX, f"{w}.escalating.cap")
      else:
         cap = DEFAULT_ESCALATION_CAP if esc else 0
      immune = name_list(s.get("immune_types", []), f"{w}.immune_types")
      types_mask(immune, n, f"{w}.immune_types")   # the names must be types
      residual = q8(s.get("residual", 0), f"{w}.residual", 0, U8_MAX)
      thaw_type = s.get("thawed_by_hit")
      if thaw_type is not None:
         n["types"](thaw_type, f"{w}.thawed_by_hit")   # must be a type
      texts = {k: (enc.term_text(tx[k], f"{w}.texts.{k}", STATUS_TEXT_ARGS) if k in tx else None)
               for k in STATUS_TEXTS}
      if residual and texts["residual"] is None:
         raise ManifestError(f"{w}.texts: residual damage needs a residual text")
      if (sk != (0, 0) or tmax) and texts["skip"] is None:
         raise ManifestError(f"{w}.texts: lost turns need a skip text")
      statuses.append({
         "name": enc.text(s["name"], f"{w}.name"), "texts": texts,
         "color": integer(s.get("color", 0), 0, U8_MAX, f"{w}.color"),
         "tags": name_list(s.get("tags", []), f"{w}.tags"),
         "template": {"immune": immune, "turns": (tmin, tmax), "skip": sk, "cure": cu, "thaw_flag": thaw_flag,
                      "thaw_type": thaw_type,
                      "residual": residual, "escalation_cap": cap if residual else 0,
                      "cured_on_switch": bool(s.get("cured_on_switch"))},
         "first_handler": 0, "handler_count": 0, "point_mask": 0})
   return statuses


def weather_template(name: str, wd: Mapping[str, Any]) -> str:
   """!The effect code a weather's declarative fields expand into: residual damage (immune_types excepted).
   @param name  the weather
   @param wd    compiled weather
   @return the `.ceffect` owner block ("" when it has none)
   """
   t = wd["template"]
   if not t["residual"]:
      return ""
   when = f" when not ({type_test(t['immune'])})" if t["immune"] else ""
   return owner_block("weather", name, [
      f"on field_residual{when} {{ text hurts; damage_hp holder max(1, holder.max_hp * {t['residual']} >> "
      f"{Q8_SHIFT}); }}"])


def compile_weathers(entries: Sequence[Json], n: Lookups, where: str, enc: Texts) -> list[dict[str, Any]]:
   """!Weathers.
   @param entries  the manifest's weathers
   @param n        lookups
   @param where    location
   @param enc      this compile's text encoders (with terms)
   @return compiled weathers ("template", weather_template(); "turns": the duration `weather NAME;` gives)
   """
   weathers = []
   for i, wd in enumerate(entries):
      w = f"{where}: weathers[{i}]"
      check_keys(wd, "weather", w)
      tx = wd.get("texts", {})
      check_keys(tx, "weather_texts", f"{w}.texts")
      for k in ("started", "continues", "ended"):
         if k not in tx:
            raise ManifestError(f"{w}.texts: needs {k}")
      res = q8(wd.get("residual", 0), f"{w}.residual", 0, U8_MAX)
      if res and "hurts" not in tx:
         raise ManifestError(f"{w}.texts: residual damage needs a hurts text")
      immune = name_list(wd.get("immune_types", []), f"{w}.immune_types")
      types_mask(immune, n, f"{w}.immune_types")
      weathers.append({"name": enc.text(wd["name"], f"{w}.name"),
                       "texts": {k: (enc.term_text(tx[k], f"{w}.texts.{k}", WEATHER_TEXT_ARGS) if k in tx else None)
                                 for k in WEATHER_TEXTS},
                       "turns": integer(wd.get("turns", 0), 0, U8_MAX, f"{w}.turns"),
                       "tags": name_list(wd.get("tags", []), f"{w}.tags"),
                       "template": {"residual": res, "immune": immune},
                       "first_handler": 0, "handler_count": 0, "point_mask": 0})
   return weathers


def own_texts(tx: Json, w: str, enc: Texts, messages: Sequence[str]) -> dict[str, Any]:
   """!A volatile's, side or field condition's texts: owner texts or engine messages.
   @param tx        the record's "texts" {slot: text or {"message": name}}
   @param w         location
   @param enc       this compile's text encoders (with terms)
   @param messages  engine message names
   @return {slot: ("text", codes) or ("message", name)}
   """
   if not isinstance(tx, dict):
      raise ManifestError(f"{w}.texts must map names to texts or {{\"message\": name}}")
   out: dict[str, Any] = {}
   for slot, v in tx.items():
      if isinstance(v, dict):
         if set(v) != {"message"} or v["message"] not in messages:
            raise ManifestError(f"{w}.texts.{slot}: {{\"message\": NAME}} names an engine message the manifest "
                                f"gives (messages: {', '.join(m for m in messages if m is not None)})")
         out[slot] = ("message", v["message"])
      else:
         out[slot] = ("text", enc.term_text(v, f"{w}.texts.{slot}", CONDITION_TEXT_ARGS))
   return out


def show(texts: Mapping[str, Any], slot: str) -> str:
   """!The statement that shows one of a record's texts (`text SLOT;` or `say MESSAGE at holder;`), or "".
   @param texts  own_texts()
   @param slot   the slot
   @return the statement
   """
   if slot not in texts:
      return ""
   kind, v = texts[slot]
   return f"text {quoted(slot)};" if kind == "text" else f"say {quoted(v)} at holder;"


def compile_volatiles(entries: Json, where: str, enc: Texts, move_flags: Sequence[str],
                      messages: Sequence[str]) -> list[dict[str, Any]]:
   """!Volatiles: their records, counter and word slots (in declaration order, as the effect compiler allocates
   them) and templates.
   @param entries     the manifest's volatiles
   @param where       location
   @param enc         this compile's text encoders (with terms)
   @param move_flags  the manifest's move flags (a chain's kept_by)
   @param messages    engine message names
   @return compiled volatiles
   """
   if not isinstance(entries, list) or len(entries) > MAX_VOLATILES:
      raise ManifestError(f"{where}: volatiles must be a list of at most {MAX_VOLATILES} records")
   out = []
   counters = words = sources = 0
   names = set()
   for i, v in enumerate(entries):
      w = f"{where}: volatiles[{i}]"
      check_keys(v, "volatile", w)
      name = v.get("name")
      if not isinstance(name, str) or name in names:
         raise ManifestError(f"{w}: needs a name of its own")
      names.add(name)
      flags = 0
      dur = v.get("duration", "until_switch")
      rolled = None
      if isinstance(dur, list):
         lo = integer(dur[0] if dur else None, 0, U8_MAX, f"{w}.duration")
         hi = integer(dur[1] if len(dur) > 1 else None, lo, U8_MAX, f"{w}.duration")
         if len(dur) != PAIR_LEN:
            raise ManifestError(f"{w}.duration: [min, max]")
         rolled = (lo, hi)
      elif dur == "turn":
         flags |= VOLATILE_TURN
      elif dur != "until_switch":
         raise ManifestError(f"{w}.duration: [min, max], \"turn\" or \"until_switch\"")
      counts = v.get("counts", "before_move")
      if counts not in COUNTS or ("counts" in v and rolled is None):
         raise ManifestError(f"{w}.counts: {' or '.join(COUNTS)}, for a [min, max] duration")
      chain = v.get("chain")
      if chain is not None:
         check_keys(chain, "chain", f"{w}.chain")
         den = integer(chain.get("den"), MIN_CHAIN_DEN, U8_MAX, f"{w}.chain.den")
         cap = integer(chain.get("cap"), 1, MAX_CHAIN_CAP, f"{w}.chain.cap")
         if den ** cap > MAX_CHAIN_ODDS:
            raise ManifestError(f"{w}.chain: den^cap must stay below 2^31")
         kept = chain.get("kept_by")
         if kept is not None and kept not in move_flags:
            raise ManifestError(f"{w}.chain.kept_by: a move flag (move_flags: {', '.join(move_flags) or 'none'})")
         chain = {"den": den, "cap": cap, "kept_by": kept}
         if rolled is not None:
            raise ManifestError(f"{w}: a chain and a [min, max] duration would share its counter")
         if dur != "until_switch":
            raise ManifestError(f"{w}: a chain's volatile stays on (duration until_switch): its handlers count and "
                                f"reset the chain while it is on")
      has_counter = bool(v.get("counter")) or rolled is not None or chain is not None
      counter = NO_SLOT
      if has_counter:
         counter, counters = counters, counters + 1
      word = NO_SLOT
      if v.get("word"):
         word, words = words, words + 1
      if counters > MAX_COUNTERS or words > MAX_WORDS:
         raise ManifestError(f"{w}: at most {MAX_COUNTERS} volatile counters and {MAX_WORDS} words in all")
      # G1: a volatile bound to the battler that started it (a hold, a bind, a link) ends when that one leaves
      source = NO_SLOT
      bound = v.get("bound_to")
      if bound is not None:
         if bound not in BOUND_TO:
            raise ManifestError(f"{w}.bound_to: {' or '.join(repr(b) for b in BOUND_TO)}")
         source, sources = sources, sources + 1
         flags |= VOLATILE_BOUND
         if sources > MAX_BOUND:
            raise ManifestError(f"{w}: at most {MAX_BOUND} bound volatiles (bound_to) in all")
      clears = name_list(v.get("clears_on", list(CLEARS_ON)), f"{w}.clears_on")
      for c in clears:
         if c not in CLEARS_ON:
            raise ManifestError(f"{w}.clears_on: {', '.join(CLEARS_ON)}")
         flags |= CLEARS_ON[c]
      flags |= (VOLATILE_PASSES if v.get("passes") else 0) | (VOLATILE_HUD if v.get("hud") else 0)
      skip = v.get("skip", False)
      if skip is True:
         skip = (1, 1)
      elif skip:
         skip = odds(skip, f"{w}.skip", U8_MAX)
      texts = own_texts(v.get("texts", {}), w, enc, messages)
      if skip and "skip" not in texts:
         raise ManifestError(f"{w}.texts: a lost turn needs a skip text")
      out.append({"name": enc.text(name, f"{w}.name"), "source_name": name, "counter": counter, "word": word,
                  "source": source, "flags": flags, "texts": texts,
                  "tags": name_list(v.get("tags", []), f"{w}.tags"),
                  "template": {"rolled": rolled, "counts": counts, "skip": skip or None, "chain": chain},
                  "owner": 0})
   return out


def volatile_template(name: str, v: Mapping[str, Any]) -> str:
   """!The effect code a volatile's declarative fields expand into (see the module comment).
   @param name  the volatile
   @param v     compiled volatile
   @return the `.ceffect` owner block ("" when it has none)
   """
   t = v["template"]
   me = quoted(name)
   counter = f"holder.counter({me})"
   H = []
   start = []
   if t["chain"] is not None:
      den, cap, kept = t["chain"]["den"], t["chain"]["cap"], t["chain"]["kept_by"]
      table = ", ".join(str(den ** k) for k in range(1, cap + 1))
      # started again right after it succeeded n times: works with odds 1 in den^n (else the starting run fails)
      roll = (f"let n = {counter}; if n > 0 and not chance(1, pick(n - 1, {table})) {{ {counter} = 0; fail; }} "
              f"{counter} = min(n + 1, {cap});")
      H.append(f"on try_start {{ {roll} }}")
      H.append(f"on already {{ {roll} }}")
      if kept is not None:
         H.append(f"on before_move when not move.has({quoted(kept)}) {{ {counter} = 0; }}")
   if t["rolled"] is not None:
      lo, hi = t["rolled"]
      start.append(f"if value > 0 {{ {counter} = value; }} else {{ {counter} = between({lo}, {hi}); }}")
   if show(v["texts"], "start"):
      start.append(show(v["texts"], "start"))
   if start:
      H.append(f"on start {{ {' '.join(start)} }}")
   if show(v["texts"], "already"):
      H.append(f"on already {{ {show(v['texts'], 'already')} }}")
   if show(v["texts"], "end"):
      H.append(f"on end {{ {show(v['texts'], 'end')} }}")
   if t["rolled"] is not None:
      # counted down before each move (or at the end of each turn): it ends at 0
      hook = "before_move" if t["counts"] == "before_move" else "residual"
      H.append(f"on {hook} {{ if {counter} == 0 {{ vol_off holder {me}; }} else {{ {counter} -= 1; }} }}")
   if t["skip"] is not None:
      n, m = t["skip"]
      cond = odds_test(n, m)
      when = " and ".join(([f"holder.has({me})"] if t["rolled"] is not None else []) + ([cond] if cond else []))
      H.append(f"on before_move{' when ' + when if when else ''} {{ {show(v['texts'], 'skip')} stop; }}")
   return owner_block("volatile", name, H)


def compile_side_conditions(entries: Json, where: str, enc: Texts, messages: Sequence[str]) -> list[dict[str, Any]]:
   """!Side conditions.
   @param entries   the manifest's side_conditions
   @param where     location
   @param enc       this compile's text encoders (with terms)
   @param messages  engine message names
   @return compiled side conditions ("turns": the duration `side_on` gives; "layers": the cap)
   """
   if not isinstance(entries, list) or len(entries) > MAX_SIDE_CONDS:
      raise ManifestError(f"{where}: side_conditions must be a list of at most {MAX_SIDE_CONDS} records")
   out = []
   for i, c in enumerate(entries):
      w = f"{where}: side_conditions[{i}]"
      check_keys(c, "side_condition", w)
      out.append({"name": enc.text(c.get("name"), f"{w}.name"), "source_name": c["name"],
                  "turns": integer(c.get("turns", 0), 0, U8_MAX, f"{w}.turns"),
                  "layers": integer(c.get("layers", 1), 1, U8_MAX, f"{w}.layers"),
                  "texts": own_texts(c.get("texts", {}), w, enc, messages),
                  "tags": name_list(c.get("tags", []), f"{w}.tags"), "owner": 0})
   check_distinct(out, f"{where}: side_conditions")
   return out


def compile_field_conditions(entries: Json, where: str, enc: Texts, messages: Sequence[str]) -> list[dict[str, Any]]:
   """!Field conditions.
   @param entries   the manifest's field_conditions
   @param where     location
   @param enc       this compile's text encoders (with terms)
   @param messages  engine message names
   @return compiled field conditions ("turns": the duration `field_on` gives)
   """
   if not isinstance(entries, list) or len(entries) > MAX_FIELD_CONDS:
      raise ManifestError(f"{where}: field_conditions must be a list of at most {MAX_FIELD_CONDS} records")
   out = []
   for i, c in enumerate(entries):
      w = f"{where}: field_conditions[{i}]"
      check_keys(c, "field_condition", w)
      out.append({"name": enc.text(c.get("name"), f"{w}.name"), "source_name": c["name"],
                  "turns": integer(c.get("turns", 0), 0, U8_MAX, f"{w}.turns"),
                  "texts": own_texts(c.get("texts", {}), w, enc, messages),
                  "tags": name_list(c.get("tags", []), f"{w}.tags"), "owner": 0})
   check_distinct(out, f"{where}: field_conditions")
   return out


def check_distinct(records: Sequence[Mapping[str, Any]], where: str) -> None:
   """!Refuses two records with one name.
   @param records  compiled records ("source_name")
   @param where    location
   @return None
   """
   names = [r["source_name"] for r in records]
   if len(set(names)) != len(names):
      raise ManifestError(f"{where}: two records share a name")


def text_only_template(kind: str, name: str, c: Mapping[str, Any]) -> str:
   """!The texts of a side or field condition as start / already / end handlers.
   @param kind  owner kind
   @param name  its name
   @param c     compiled condition
   @return the `.ceffect` owner block ("" when it has none)
   """
   H = [f"on {slot} {{ {show(c['texts'], slot)} }}" for slot in CONDITION_TEXT_SLOTS if show(c["texts"], slot)]
   return owner_block(kind, name, H)


def condition_entries(statuses: Sequence[Mapping[str, Any] | None], weathers: Sequence[Mapping[str, Any]],
                      volatiles: Sequence[Mapping[str, Any]], sides: Sequence[Mapping[str, Any]],
                      fields: Sequence[Mapping[str, Any]], names: Mapping[str, Sequence[str]],
                      own_text_ids: Mapping[tuple[str, str], Mapping[str, int]]) -> dict[str, list[dict[str, Any]]]:
   """!The effect catalog's condition lists: names with their counter / word slots, own text ids and durations.
   @param statuses      compiled statuses (index 0 = none)
   @param weathers      compiled weathers
   @param volatiles     compiled volatiles
   @param sides         compiled side conditions
   @param fields        compiled field conditions
   @param names         the battle's names by list
   @param own_text_ids  (kind, name) -> {slot: owner text id} of the volatile / side / field texts
   @return {"statuses", "weathers", "volatiles", "side_conditions", "field_conditions"} entry lists
   """
   def ids(texts: Mapping[str, Any], order: Sequence[str]) -> dict[str, int]:
      """!Condition text ids of a status or weather (kConditionText + extra)."""
      return {k: CONDITION_TEXT + order.index(k) for k in order if texts[k] is not None}
   return {
      "statuses": [{"name": nm, "texts": ids(s["texts"], STATUS_TEXTS)} for nm, s in zip(names["statuses"],
                                                                                         statuses[1:])],
      "weathers": [{"name": nm, "texts": ids(wd["texts"], WEATHER_TEXTS), "turns": wd["turns"]}
                   for nm, wd in zip(names["weathers"], weathers)],
      "volatiles": [{"name": v["source_name"], "counter": v["counter"] != NO_SLOT, "word": v["word"] != NO_SLOT,
                     "texts": dict(own_text_ids.get(("volatile", v["source_name"]), {}))} for v in volatiles],
      "side_conditions": [{"name": c["source_name"], "turns": c["turns"],
                           "texts": dict(own_text_ids.get(("side_condition", c["source_name"]), {}))} for c in sides],
      "field_conditions": [{"name": c["source_name"], "turns": c["turns"],
                            "texts": dict(own_text_ids.get(("field_condition", c["source_name"]), {}))}
                           for c in fields],
   }


def own_text_table(volatiles: Sequence[Mapping[str, Any]], sides: Sequence[Mapping[str, Any]],
                   fields: Sequence[Mapping[str, Any]], first: int) -> tuple[list[str], list[list[int]],
                                                                           dict[tuple[str, str], dict[str, int]]]:
   """!The owner texts of the volatile, side and field condition records, after the manifest's texts.
   @param volatiles  compiled volatiles
   @param sides      compiled side conditions
   @param fields     compiled field conditions
   @param first      id of the first one (the manifest's text count)
   @return (names, encoded texts, (kind, name) -> {slot: id})
   """
   names: list[str] = []
   codes: list[list[int]] = []
   ids: dict[tuple[str, str], dict[str, int]] = {}
   for kind, records in (("volatile", volatiles), ("side_condition", sides), ("field_condition", fields)):
      for r in records:
         for slot, (what, v) in r["texts"].items():
            if what != "text":
               continue
            ids.setdefault((kind, r["source_name"]), {})[slot] = first + len(names)
            names.append(f"{kind} {r['source_name']}: {slot}")
            codes.append(v)
   return names, codes, ids

