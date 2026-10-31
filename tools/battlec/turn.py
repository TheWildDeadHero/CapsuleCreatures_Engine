"""!Battle geometry and turn structure of the battle compiler: formats
(positions, sides, reach masks, screen layout, stands), the engine's caps on
parties, positions, sides, battlers and escape attempts, and the turn
structure (D-22 A11, `turn`: action brackets, the speed tie rule, the
end-of-turn phases, the hook dispatch order and the battle start's
send-outs; capsule::battle::TurnRules).
"""
from __future__ import annotations

from collections.abc import Sequence
from typing import Any

from .common import PAIR, U8_MAX, Json, ManifestError, Texts, check_keys, integer, register_keys
from .layout import PARTY_SIZE

## Parties per battle (capsule::battle::kMaxParties).
MAX_PARTIES = 6
## Field positions per format (capsule::battle::kMaxPositions).
MAX_POSITIONS = 6
## Fewest sides of a format.
MIN_SIDES = 2
## Most sides of a format (capsule::battle::kMaxSides).
MAX_SIDES = 4
## A format's ranges: every position reaches every other ("all"), or only its neighbours ("adjacent").
FORMAT_RANGES = ("all", "adjacent")
## What a move does when its chosen target fainted first (FormatData::on_target_fainted order): pick another
## target, or fail.
ON_TARGET_FAINTED = ("redirect", "fail")
## Sides of a format whose range is "adjacent" (two facing rows).
ADJACENT_SIDES = 2
## Battler slots per battle (capsule::battle::kMaxBattlers): the most participants.
MAX_BATTLERS = 24
## Most escape attempts the engine counts.
ESCAPE_ATTEMPTS_MAX = 255


register_keys({
   "format": {"name", "sides", "range", "layout", "stands", "party_max", "on_target_fainted"},
})


def compile_formats(entries: Sequence[Json], where: str, enc: Texts,
                    party_size: int = PARTY_SIZE) -> list[dict[str, Any]]:
   """!Formats.
   @param entries     the manifest's formats
   @param where       location
   @param enc         this compile's text encoders
   @param party_size  the layout's party size (the most party_max may be; its default)
   @return compiled formats
   """
   formats = []
   for i, fd in enumerate(entries):
      w = f"{where}: formats[{i}] ({fd.get('name')})"
      check_keys(fd, "format", w)
      sides = fd.get("sides")
      if not isinstance(sides, list) or not MIN_SIDES <= len(sides) <= MAX_SIDES:
         raise ManifestError(f"{w}.sides: 2-4 lists of positions")
      side_of = {}
      for si, plist in enumerate(sides):
         if not isinstance(plist, list) or not plist:
            raise ManifestError(f"{w}.sides[{si}]: a non-empty list of positions")
         for p in plist:
            integer(p, 0, MAX_POSITIONS - 1, f"{w}.sides[{si}]")
            if p in side_of:
               raise ManifestError(f"{w}: position {p} is on two sides")
            side_of[p] = si
      npos = len(side_of)
      if sorted(side_of) != list(range(npos)):
         raise ManifestError(f"{w}: positions must be 0..{npos - 1}")
      rng_ = fd.get("range", FORMAT_RANGES[0])

      # reach[p]: bit q is set when position p can target position q
      reach = [0] * MAX_POSITIONS
      for p in range(npos):
         for q in range(npos):
            if p == q:
               continue
            if rng_ == FORMAT_RANGES[0]:
               reach[p] |= 1 << q
            elif rng_ == FORMAT_RANGES[1]:
               if len(sides) != ADJACENT_SIDES or len(sides[0]) != len(sides[1]):
                  raise ManifestError(f"{w}: adjacent range needs two sides of equal size")
               sp_, sq = side_of[p], side_of[q]
               ip, iq = sides[sp_].index(p), sides[sq].index(q)

               # neighbours on one side; across, the rows face each other, so the order is mirrored
               if sp_ == sq:
                  ok = abs(ip - iq) == 1
               else:
                  ok = abs((len(sides[sp_]) - 1 - ip) - iq) <= 1
               if ok:
                  reach[p] |= 1 << q
            else:
               raise ManifestError(f"{w}.range: {' or '.join(FORMAT_RANGES)}")
      lay = fd.get("layout", [])
      if not isinstance(lay, list) or len(lay) != npos:
         raise ManifestError(f"{w}.layout: one [x, y] per position")
      xs = [integer(v[0], 0, U8_MAX, f"{w}.layout") for v in lay]
      ys = [integer(v[1], 0, U8_MAX, f"{w}.layout") for v in lay]

      # where each side's presenters stand: given, or the side's first position
      stands = fd.get("stands", [lay[plist[0]] for plist in sides])
      if not isinstance(stands, list) or len(stands) != len(sides) or \
         any(not isinstance(v, list) or len(v) != PAIR for v in stands):
         raise ManifestError(f"{w}.stands: one [x, y] per side")
      stand_x = [integer(v[0], 0, U8_MAX, f"{w}.stands") for v in stands]
      stand_y = [integer(v[1], 0, U8_MAX, f"{w}.stands") for v in stands]
      otf = fd.get("on_target_fainted", ON_TARGET_FAINTED[0])
      if otf not in ON_TARGET_FAINTED:
         raise ManifestError(f"{w}.on_target_fainted: {' or '.join(ON_TARGET_FAINTED)}")
      formats.append({"name": enc.text(fd["name"], f"{w}.name"), "positions": npos, "sides": len(sides),
                      "side_of": [side_of.get(p, 0) for p in range(MAX_POSITIONS)], "reach": reach,
                      "x": xs + [0] * (MAX_POSITIONS - npos), "y": ys + [0] * (MAX_POSITIONS - npos),
                      "stand_x": stand_x + [0] * (MAX_SIDES - len(sides)),
                      "stand_y": stand_y + [0] * (MAX_SIDES - len(sides)),
                      "party_max": integer(fd.get("party_max", party_size), 1, party_size, f"{w}.party_max"),
                      "otf": ON_TARGET_FAINTED.index(otf)})
   return formats


## Hook dispatch sources in capsule::battle::EffectSource order (manifest names).
SOURCES = ["status", "volatile", "side", "field", "weather", "passive", "item"]
## End-of-turn phases in capsule::battle::EndPhase order ("regen": each active battler's resources regenerate,
## D-25; battlec/resources.py).
END_PHASES = ["weather", "battlers", "sides", "field", "regen", "delayed"]
## End-of-turn phases a turn may leave out but lists at most once (E13b: delayed holds the pending effects'
## countdowns; a battle whose logic uses `delay` needs it).
AT_MOST_ONCE_PHASES = ("delayed",)
## End-of-turn phases that hold a countdown: each must appear exactly once.
ONCE_PHASES = ("weather", "sides", "field")
## Who a sides / field phase's residual runs for, in capsule::battle::PhaseEach order.
PHASE_EACH = ["battler", "once"]
## The end-of-turn phases that say for whom their residual runs ("each": PHASE_EACH).
EACH_PHASES = ("sides", "field")
## Speed tie rules in capsule::battle::TieRule order.
TIE_RULES = ["random_byte", "position"]
## Battle start send-out orders in capsule::battle::SendOrder order.
SEND_ORDERS = ["foes_first", "position"]
## Battle start switch-in hook orders in capsule::battle::StartHooks order.
SWITCH_IN_ORDERS = ["speed", "send_out"]
## Most end-of-turn phases (capsule::battle::kMaxEndPhases).
MAX_END_PHASES = 8
## Lowest action bracket: the turn order key adds 64 (capsule::battle::detail kBracketBias) and keeps 7 bits.
BRACKET_MIN = -64
## Highest action bracket.
BRACKET_MAX = 63
## The "move" bracket that means each move's own priority.
MOVE_PRIORITY = "priority"
## Default bracket of running (the engine's former fixed bracket).
DEFAULT_RUN_BRACKET = 30
## Default bracket of switching (the engine's former fixed bracket).
DEFAULT_SWITCH_BRACKET = 20
## Default bracket of using an item (D-23 section 2; the Bag's item action is not an action yet).
DEFAULT_ITEM_BRACKET = 25
## The default turn structure: the engine's behaviour before the turn became data (E6b).
DEFAULT_TURN: dict[str, Any] = {
   "brackets": {"run": DEFAULT_RUN_BRACKET, "switch": DEFAULT_SWITCH_BRACKET, "item": DEFAULT_ITEM_BRACKET,
                "move": MOVE_PRIORITY},
   "tie": "random_byte",
   "end_of_turn": [{"run": "weather"}, {"run": "battlers"}, {"run": "sides"}, {"run": "field"}],
   "effect_order": list(SOURCES),
   "switching": {"send_out": "foes_first", "switch_in": "speed"},
}

register_keys({
   "turn": {"brackets", "tie", "end_of_turn", "effect_order", "switching"},
   "turn_brackets": {"run", "switch", "item", "move"},
   "turn_phase": {"run", "sources", "each"},
   "turn_switching": {"send_out", "switch_in"},
})


def choice(v: Json, names: Sequence[str], where: str) -> int:
   """!One of a list of names.
   @param v      value
   @param names  the allowed names (their index is the compiled value)
   @param where  location
   @return the index
   """
   if v not in names:
      raise ManifestError(f"{where}: one of {', '.join(names)}, got {v!r}")
   return names.index(v)


def source_list(v: Json, where: str, complete: bool) -> list[int]:
   """!A list of dispatch source names without repeats.
   @param v         value
   @param where     location
   @param complete  every source must be listed (else a non-empty subset)
   @return EffectSource values in order
   """
   if not isinstance(v, list) or not v:
      raise ManifestError(f"{where}: a non-empty list of {', '.join(SOURCES)}")
   ids = [choice(name, SOURCES, where) for name in v]
   if len(set(ids)) != len(ids):
      raise ManifestError(f"{where}: a source is listed twice")
   if complete and len(ids) != len(SOURCES):
      missing = [SOURCES[k] for k in range(len(SOURCES)) if k not in ids]
      raise ManifestError(f"{where}: must list every source (missing {', '.join(missing)}): a source left out "
                          "would never run its handlers")
   return ids


def bracket(v: Json, where: str) -> int:
   """!An action bracket.
   @param v      value
   @param where  location
   @return the bracket
   """
   return integer(v, BRACKET_MIN, BRACKET_MAX, where)


def compile_turn(td: Json, where: str) -> dict[str, Any]:
   """!The turn structure (capsule::battle::TurnRules). Missing keys take DEFAULT_TURN's.
   @param td     the manifest's turn (or None)
   @param where  location
   @return compiled turn: brackets {run, switch, item, move, move_priority}, tie, send_order, start_hooks,
           effect_order [EffectSource], end [{phase, each, sources}]
   """
   td = {} if td is None else td
   w = f"{where}: turn"
   check_keys(td, "turn", w)
   check_keys(td.get("brackets", {}), "turn_brackets", f"{w}.brackets")
   br = {**DEFAULT_TURN["brackets"], **td.get("brackets", {})}
   move = br["move"]
   brackets = {"run": bracket(br["run"], f"{w}.brackets.run"),
               "switch": bracket(br["switch"], f"{w}.brackets.switch"),
               "item": bracket(br["item"], f"{w}.brackets.item"),
               "move": 0 if move == MOVE_PRIORITY else bracket(move, f"{w}.brackets.move "
                                                                        f"({MOVE_PRIORITY!r} or a bracket)"),
               "move_priority": 1 if move == MOVE_PRIORITY else 0}
   effect_order = source_list(td.get("effect_order", DEFAULT_TURN["effect_order"]), f"{w}.effect_order", True)

   # the end of turn: weather, sides and field once each (they hold the countdowns), battlers at least once
   phases = td.get("end_of_turn", DEFAULT_TURN["end_of_turn"])
   if not isinstance(phases, list) or not 1 <= len(phases) <= MAX_END_PHASES:
      raise ManifestError(f"{w}.end_of_turn: a list of 1-{MAX_END_PHASES} phases")
   end = []
   for k, ph in enumerate(phases):
      pw = f"{w}.end_of_turn[{k}]"
      check_keys(ph, "turn_phase", pw)
      run = END_PHASES[choice(ph.get("run"), END_PHASES, f"{pw}.run")]
      if "sources" in ph and run != "battlers":
         raise ManifestError(f"{pw}.sources: only a battlers phase lists sources")
      if "each" in ph and run not in EACH_PHASES:
         raise ManifestError(f"{pw}.each: only a sides or field phase says for whom its residual runs")
      sources = source_list(ph["sources"], f"{pw}.sources", False) if "sources" in ph else effect_order
      if run != "battlers":
         sources = []
      end.append({"phase": END_PHASES.index(run), "each": choice(ph.get("each", "battler"), PHASE_EACH, f"{pw}.each"),
                  "sources": sources})
   runs = [END_PHASES[e["phase"]] for e in end]
   for name in ONCE_PHASES:
      if runs.count(name) != 1:
         raise ManifestError(f"{w}.end_of_turn: the {name} phase must appear exactly once (the {name} countdowns "
                             "run in it)")
   if "battlers" not in runs:
      raise ManifestError(f"{w}.end_of_turn: needs a battlers phase (the residual hooks)")
   for name in AT_MOST_ONCE_PHASES:
      if runs.count(name) > 1:
         raise ManifestError(f"{w}.end_of_turn: the {name} phase may appear only once")

   check_keys(td.get("switching", {}), "turn_switching", f"{w}.switching")
   sw = {**DEFAULT_TURN["switching"], **td.get("switching", {})}
   return {"brackets": brackets, "tie": choice(td.get("tie", DEFAULT_TURN["tie"]), TIE_RULES, f"{w}.tie"),
           "send_order": choice(sw["send_out"], SEND_ORDERS, f"{w}.switching.send_out"),
           "start_hooks": choice(sw["switch_in"], SWITCH_IN_ORDERS, f"{w}.switching.switch_in"),
           "effect_order": effect_order, "end": end}
