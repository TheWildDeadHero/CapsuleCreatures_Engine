#!/usr/bin/env python3
"""!Effect logic compiler (D-22 decision 1, D-23 section 1): the `.ceffect`
language, its JSON AST (capsule.effect-ast/1), the canonical formatter, the
checks, the bytecode for capsule/battle/effect.h and the pure backend for
point handlers (generated C++ and a Python evaluator).

    effect_compiler.py FILE (--check | --format | --ast OWNER | --emit OUT | --disasm)
                       [--structure JSON] [--namespace NS]

FILE is a `.ceffect` source (names from --structure) or a manifest

    {"schema": "capsule.effects/1", "structure": "structure.json", "sources": ["moves.ceffect", ...]}

with paths relative to it. The structure gives the names the logic may use:
lists (of names, or of objects with "name") under "types", "categories",
"statuses", "volatiles" (objects may say "counter": true / "word": true to
get a counter / word slot), "weathers", "side_conditions",
"field_conditions", "moves", "items", "passives", "species", "stats",
"stages", "move_flags", "tags", "texts", "messages", "cues", "roles",
"species_fields", "move_fields" and "resources" (objects with "name" and
"scope" battler | move, D-25). A list's index is the name's id.

Grammar (statements end with a semicolon; a hash sign starts a comment that runs to the end of the line):

@verbatim
    file     := { owner | comment }
    owner    := KIND NAME "{" { member } "}"
    KIND     := move | passive | item | status | volatile | weather
              | side_condition | field_condition | global
    member   := "on" HOOK [ "per_foe" ] [ "when" expr ] block   (not moves; per_foe: switch_in, try_switch)
              | PHASE [ "when" expr ] block               (moves: use target try_hit hit after_move)
              | "point" POINT [ "when" expr ] block       (pure subset)
              | "sub" IDENT block
              | "tags" "[" NAME { "," NAME } "]" ";"
              | "announce" ";"
    block    := "{" { stmt } "}"
    stmt     := "let" IDENT "=" expr ";"
              | target ( "=" | "+=" | "-=" ) expr ";"     (a local, or a writable ref)
              | "if" expr block { "else" "if" expr block } [ "else" block ]
              | "chance" expr "/" expr block [ "else" block ]
              | "repeat" expr "max" INT block             (1 <= INT <= 16)
              | "for" IDENT "in" SET "(" subject ")" block
              | "call" IDENT ";" | "yield" ";" | "stop" ";" | "fail" ";" | "return" ";"
              | "ask" ASK_KIND subject "into" IDENT ";"
              | PRIMITIVE args ";"                       (see PRIMITIVES)
    expr     := C-like precedence: or, and, == !=, < <= > >=, << >>, + -, * / %, unary - and not
    atom     := INT | DECIMAL (Q8) | NAME | STRING (a name) | "(" expr ")"
              | FUNC "(" expr { "," expr } ")"              (FUNCTIONS)
              | subject "." field [ "(" arg [ "," INT ] ")" ]  | "side" "(" subject ")" "." field ...
              | "move" "." field ... | "field" "." field ... | turn | hits | crit | value | dealt | move | owner
    subject  := holder | other | user | target | a for-loop variable or a local
@endverbatim

Comments stand only between statements, members and owners; a comment on
the same line as the code before it stays there ("trailing"). The AST keeps
them as {"stmt": "comment"} nodes, so parse(format(ast)) == ast and
format(parse(text)) is the canonical text.

Checks (errors unless noted): syntax; unknown hooks, points, phases, subs,
names and refs (from the hook / point / ref catalogs below and the
structure); a hook on an owner kind that cannot hold it; `per_foe` on another
hook than switch_in (a per-foe handler also runs once per foe on the field,
with value 1, after the battler's own run with value 0: kHandlerPerFoe) or
try_switch (C5: it runs only when a foe of its holder switches out or runs:
other = that foe, value its battler; the others only for the holder's own);
writes to an unstored record role (C5: computed on read, it has no bits);
writes to read-only refs; results (modify, immune, block) where the hook has none; the operand
stack depth (proven on the bytecode, kEffectStack); locals (kEffectLocals);
the sub call graph (acyclic, kEffectCallDepth deep); loop bounds (repeat max
1-16); yield, ask and asking switches only in top-level phases (a move's
use / hit / after_move, a condition's residual); damage only in a move's hit
phase, set_target only in its target phase; the event budget (each op's cost
from OP_EVENTS plus the hooks it triggers, up to kMaxEffectDepth): a hook
program <= kHookEventBudget, each segment between yields of a top-level
program <= kEventsPerStep; point handlers use only the pure subset (reads,
arithmetic, let, if, modify, return). Warnings: unreachable statements, and
arithmetic whose analysed range (reusing ruleset_compiler.interval) can leave
int32 and saturate.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections.abc import Callable, Iterable, Mapping, Sequence
from fractions import Fraction
from pathlib import Path
from typing import Any, NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ruleset_compiler as rc  # noqa: E402
from script_compiler import Diagnostic, ScriptError, Token, VISITED, VISITING, parse_int, unescape  # noqa: E402

## AST schema (one owner, or a whole file).
SCHEMA_AST = "capsule.effect-ast/1"
## Manifest schema.
SCHEMA_MANIFEST = "capsule.effects/1"

# -- engine limits (capsule/battle/effect.h, battle/data/common.h, battle.h) -------------------------------------------

## Operand stack slots per run (kEffectStack).
EFFECT_STACK = 16
## Locals per run (kEffectLocals).
EFFECT_LOCALS = 8
## Nested sub calls (kEffectCallDepth).
EFFECT_CALL_DEPTH = 4
## Nested runs (kMaxEffectDepth).
MAX_EFFECT_DEPTH = 4
## Worst-case events of a hook program (kHookEventBudget).
HOOK_EVENT_BUDGET = 32
## Events of one battle step (kEventsPerStep): a top-level segment's budget.
EVENTS_PER_STEP = 128
## Move phases that never suspend but run as a step's unit of their own, so a step's budget is theirs (G1: a delayed
## hit's land phase, one unit of the end-of-turn delayed phase, works a whole hit out).
UNIT_PHASES = {"land"}
## Highest repeat bound (kMaxRepeat).
MAX_REPEAT = 16
## Operands a primitive pops at most (kMaxActionArgs; set_types takes this many types).
MAX_ACTION_ARGS = 3
## Battler slots (kMaxBattlers).
MAX_BATTLERS = 24
## Field positions (kMaxPositions): the rounds of a `for` loop.
MAX_POSITIONS = 6
## Sides (kMaxSides).
MAX_SIDES = 4
## Parties (kMaxParties).
MAX_PARTIES = 6
## Values of one pick() table.
MAX_PICK = 8
## Volatile counters per battler.
MAX_COUNTERS = 8
## Volatile words per battler.
MAX_WORDS = 4
## Owner tags (bits of Owner::tags).
MAX_TAGS = 16
## Hook / point mask bits.
MASK_BITS = 32
## No text (kNoText).
NO_TEXT = 0xFFFF
## Switch: the host picks the replacement (kHostPicks).
HOST_PICKS = -1
## Largest id of an 8-bit immediate.
MAX_ID8 = 0xFF
## Largest 16-bit id (kNoText excluded).
MAX_ID16 = 0xFFFE
## Largest program length / offset of a jump target.
MAX_PROGRAM_BYTES = 0xFFFF
## Highest stored events_max (a byte).
MAX_EVENTS_FIELD = 0xFF

## Subject ctx.holder (subj8 codes: kSubjHolder ... kSubjNone, kSubjLocal + n, the kSubjSide flag).
SUBJ_HOLDER = 0
## ctx.other.
SUBJ_OTHER = 1
## ctx.user.
SUBJ_USER = 2
## ctx.target.
SUBJ_TARGET = 3
## The context move.
SUBJ_MOVE = 4
## The field.
SUBJ_FIELD = 5
## Nothing (globals).
SUBJ_NONE = 6
## The battler in local n.
SUBJ_LOCAL = 0x10
## Flag: the battler's side.
SUBJ_SIDE = 0x80
## A subject without the side flag.
SUBJ_BASE_MASK = 0x7F
## Battler subject words, in code order.
BATTLER_SUBJECTS = ["holder", "other", "user", "target"]
## B1: a fixed battler (+ n: battler n; capsule::battle::effect::kSubjBattler): a combination's inputs.
SUBJ_BATTLER = 0x20
## B1: the subject word of a combination's inputs (`input.0`, `input.ROLE`: a slot role of the methods).
INPUT_SUBJECT = "input"
## B1: the battler of a combination's first input in the `on combine` field context (battle::kFieldOther).
FIRST_INPUT_BATTLER = 1
## B1: the tokens of an input subject after `input`: the dot and the index or slot role.
INPUT_TOKENS = 2

## One half (rounding Q8 literals).
ROUND_HALF = Fraction(1, 2)
## Bits of a Q8 number.
Q8_SHIFT = 8
## 1.0 in Q8.
Q8_ONE = 1 << Q8_SHIFT
## Lowest int32.
INT32_MIN = rc.INT32[0]
## Highest int32.
INT32_MAX = rc.INT32[1]
## Largest shift count.
MAX_SHIFT = 31
## Bits of an int8 below its sign.
INT8_BITS = 7
## Bits of an int16 below its sign.
INT16_BITS = 15
## Lowest int8 (Push8).
INT8_MIN = -(1 << INT8_BITS)
## Highest int8.
INT8_MAX = (1 << INT8_BITS) - 1
## Lowest int16 (Push16).
INT16_MIN = -(1 << INT16_BITS)
## Highest int16.
INT16_MAX = (1 << INT16_BITS) - 1
## Largest byte.
U8_MAX = 0xFF
## Largest 16-bit value.
U16_MAX = 0xFFFF
## Byte mask.
BYTE_MASK = 0xFF
## Bits per byte.
BYTE_BITS = 8

# -- opcodes (capsule::battle::effect::Op and kOpInfo) -----------------------------------------------------------------

## Operand kinds and their sizes in bytes (disassembler and encoder).
OPERAND_BYTES = {"i8": 1, "i16": 2, "i32": 4, "l": 1, "off": 2, "set": 1, "subj": 1, "prog": 2, "ref": 1, "arg": 1,
                 "kind": 1, "id": 1, "role": 1, "elem": 1, "n": 1, "flags": 1, "text": 2, "mode": 1}
## Every opcode in Op order: name, operand kinds, pops, pushes (Pick pops n more, SetTypes n).
OPS = [
   ("End", [], 0, 0), ("Push8", ["i8"], 0, 1), ("Push16", ["i16"], 0, 1), ("Push32", ["i32"], 0, 1),
   ("Dup", [], 1, 2), ("Drop", [], 1, 0), ("Swap", [], 2, 2), ("Load", ["l"], 0, 1), ("Store", ["l"], 1, 0),
   ("Add", [], 2, 1), ("Sub", [], 2, 1), ("Mul", [], 2, 1), ("Div", [], 2, 1), ("Mod", [], 2, 1),
   ("Neg", [], 1, 1), ("Abs", [], 1, 1), ("Min", [], 2, 1), ("Max", [], 2, 1), ("Clamp", [], 3, 1),
   ("MulQ8", [], 2, 1), ("Shl", [], 2, 1), ("Shr", [], 2, 1), ("Pick", ["n"], 1, 1),
   ("Eq", [], 2, 1), ("Ne", [], 2, 1), ("Lt", [], 2, 1), ("Le", [], 2, 1), ("Gt", [], 2, 1), ("Ge", [], 2, 1),
   ("Not", [], 1, 1),
   ("Jmp", ["off"], 0, 0), ("Jz", ["off"], 1, 0), ("Jnz", ["off"], 1, 0), ("Loop", ["l", "off"], 0, 0),
   ("ForEach", ["set", "subj", "l", "off"], 0, 0), ("Next", ["l", "off"], 0, 0), ("Call", ["prog"], 0, 0),
   ("Ret", [], 0, 0), ("Yield", [], 0, 0), ("Stop", [], 0, 0), ("Fail", [], 0, 0),
   ("Get", ["ref", "subj", "arg"], 0, 1), ("Has", ["kind", "subj", "id"], 0, 1), ("Subj", ["subj"], 0, 1),
   ("RoleGet", ["subj", "role", "elem"], 0, 1), ("Set", ["ref", "subj", "arg"], 1, 0),
   ("RoleSet", ["subj", "role", "elem"], 1, 0),
   ("Rand", [], 1, 1), ("Between", [], 2, 1), ("Chance", [], 2, 1),
   ("Damage", [], 0, 1), ("DamageHp", ["subj", "flags"], 1, 1), ("SelfHit", ["subj"], 1, 0),
   ("Heal", ["subj"], 1, 0), ("Inflict", ["subj", "id", "flags"], 0, 1), ("Cure", ["subj", "flags"], 0, 0),
   ("Stage", ["subj", "id"], 1, 1), ("VolOn", ["subj", "id"], 1, 0), ("VolOff", ["subj", "id"], 0, 0),
   ("SideOn", ["subj", "id", "flags"], 1, 0), ("SideOff", ["subj", "id"], 0, 0), ("FieldOn", ["id"], 1, 0),
   ("FieldOff", ["id"], 0, 0), ("Weather", ["id"], 1, 0), ("Switch", ["subj", "mode"], 1, 0),
   ("Lock", ["subj", "flags"], 0, 0), ("Unlock", ["subj"], 0, 0), ("UseMove", ["subj"], 1, 0),
   ("SetTarget", [], 1, 0), ("Credit", [], 1, 0), ("ConsumeItem", ["subj"], 0, 0), ("SetItem", ["subj"], 1, 0),
   ("SetPassive", ["subj"], 1, 0), ("SetTypes", ["subj", "n"], 0, 0), ("Modify", ["mode"], 1, 0),
   ("Immune", [], 1, 0), ("Block", ["text"], 0, 0), ("Text", ["text"], 0, 0), ("Say", ["id"], 0, 0),
   ("Announce", [], 0, 0), ("Cue", ["id"], 0, 0), ("Ask", ["kind", "subj"], 0, 1),
   ("SayAt", ["id", "subj", "subj"], 1, 0),
   ("Restore", ["subj", "id", "flags"], 1, 1), ("Raise", ["subj", "id", "flags"], 1, 1),
   ("Odds", [], 4, 1),
   ("ResetStages", ["subj"], 0, 1), ("Delay", ["subj"], 2, 1), ("Reward", [], 1, 0), ("EndBattle", [], 0, 0),
   ("Teach", ["subj"], 1, 1),
   # G1: use_move ... unless FLAG (fails the run on a flagged move or none), delayed hits, a decoy's mark
   ("CallMove", ["subj", "id"], 1, 0), ("DelayHit", ["subj", "id"], 1, 1), ("Absorb", [], 0, 0),
   # G3: the recruit checks of the battle's Item action (pushes 1: recruited, 0: it broke free)
   ("Recruit", ["subj"], 1, 1),
   # G4: a form by name, an item's transformation rules on a field context's member, a training value
   ("Form", ["subj", "id"], 0, 1), ("Transform", ["subj"], 0, 1), ("Train", ["subj", "id"], 1, 1),
   # F1: an encounter hook's change to the roll (mode = ENCOUNTER_KINDS, id = a type or a species field; three
   # operands), a timed field effect started (id = the effect; amount; pushes 1 started, 0 refused)
   ("Encounter", ["mode", "id"], 3, 0), ("FieldEffect", ["id"], 1, 1),
   # B1: an inheritance primitive of the combine hook (mode = INHERIT_KINDS, subj = the input, role; two operands;
   # pushes 1 when it changed the result's record)
   ("Inherit", ["subj", "role", "mode"], 2, 1),
]
## Opcode number by name.
OP = {name: i for i, (name, _operands, _pops, _pushes) in enumerate(OPS)}
## Ops that end the run (no fall-through).
TERMINAL_OPS = {"End", "Ret", "Stop", "Fail", "Immune", "Block"}
## Opcodes the compiler never emits (they exist for hand-written code and future use).
UNEMITTED_OPS = {"Swap"}

## Refs (Ref enum order; snake_case of the C++ names).
REFS = ["hp", "max_hp", "level", "stat", "stage", "status", "status_counter", "status_aux", "counter", "word", "type",
        "passive", "item", "species", "last_move", "taken_amount", "taken_from", "taken_move", "taken_category",
        "taken_turn", "position", "side", "party", "species_field", "alive", "can_switch", "move_type",
        "move_category", "move_power", "move_accuracy", "move_priority", "move_base", "move_field", "weather_id",
        "weather_turns", "field_turns", "side_turns", "side_layers", "side_active", "turn", "hits", "crit", "battler",
        "value", "dealt", "move", "owner", "acted", "locked", "resource", "resource_max", "move_cost", "targeted_by",
        "delayed_turns", "human", "unowned", "gene", "player_side", "absorbed",
        # F1: a timed field effect's state (capsule/script/field_effects.h)
        "field_effect_active", "field_effect_remaining"]
## Has kinds (HasKind order).
HAS_KINDS = ["status", "volatile", "tag", "type", "move_flag", "side_cond", "field_cond"]
## Battler sets (BattlerSet order).
BATTLER_SETS = ["foes", "adjacent_foes", "allies", "others", "all"]
## Modify modes (ModifyMode order).
MODIFY_MODES = ["by", "add", "set"]
## Switch modes (SwitchMode order).
SWITCH_MODES = ["pass", "ask", "random"]
## Request kinds (AskKind order).
ASK_KINDS = ["switch_target", "learn_move", "choice"]
## Owner kinds (OwnerKind order) and the structure list naming them.
OWNER_KINDS = {"move": "moves", "passive": "passives", "item": "items", "status": "statuses", "volatile": "volatiles",
               "weather": "weathers", "side_condition": "side_conditions", "field_condition": "field_conditions",
               "global": None}
## Condition owner kinds (they have start / already / end and residual phases).
CONDITION_KINDS = {"status", "volatile", "weather", "side_condition", "field_condition"}
## Owner kinds a battler holds (battler hooks run them).
BATTLER_KINDS = {"passive", "item", "status", "volatile", "weather", "side_condition", "field_condition", "global"}
## How a hook dispatch adds the handlers of one owner kind: a battler holds one passive, item, status and weather
## at a time (worst handler), but any number of volatiles and side / field conditions (all of them).
KIND_FAN = {"passive": "max", "item": "max", "status": "max", "weather": "max", "volatile": "sum",
            "side_condition": "sum", "field_condition": "sum", "global": "sum"}

# -- the hook and point catalogs (D-23 section 1.3) --------------------------------------------------------------------

## Owner kinds of the encounter hooks (F1): the game's global rules and the lead's passive and held item.
ENCOUNTER_OWNERS = {"global", "passive", "item"}
## Highest encounter method index (capsule::encounter::kMaxMethods - 1): the encounter hook's value.
MAX_ENCOUNTER_METHOD = 15
## Owner kinds of the steps hook (F3): the game's global rules and a party member's status, passive and held item.
STEP_OWNERS = {"global", "status", "passive", "item"}
## Highest step effect index (capsule::script::kMaxStepEffects - 1): the steps hook's value.
MAX_STEP_EFFECT = 15
## The steps hook (F3): `field.step_effect` reads its value.
STEPS_HOOK = "steps"
## The field read of the step effect running (F3; FIELD_REFS).
STEP_EFFECT_REF = "step_effect"
## B1: the combine hook (a combination's result record built from its inputs).
COMBINE_HOOK = "combine"
## B1: owner kinds of the combine hook: the game's global rules.
COMBINE_OWNERS = {"global"}
## B1: highest combination method index (capsule::combine::kMaxMethods - 1): the combine hook's value.
MAX_COMBINE_METHOD = 15
## B1: most inputs of a combination on any platform (capsule::combine::kMaxInputs).
MAX_COMBINE_INPUTS = 8
## B1: the field reads of the combine hook (FIELD_REFS): the method running, its input count.
COMBINE_METHOD_REF = "combine_method"
## B1: the input count's field read.
COMBINE_INPUTS_REF = "combine_inputs"
## B2: the hatch hook (an egg cycle's steps off each egg; capsule/combine/eggs.h).
HATCH_HOOK = "hatch"
## B2: owner kinds of the hatch hook: the game's global rules and a member's passive and held item.
HATCH_OWNERS = {"global", "passive", "item"}
## B2: the most steps an egg cycle takes off a counter (the 16-bit hatch counter role's range).
MAX_HATCH_STEPS = 0xFFFF
## A hook catalog entry: who may hold it, its context bindings, its value (None: no value; modify is not
## allowed), the verdicts it accepts and the engine's own draws around it.
HookSpec = dict[str, Any]


def _hook(name: str, owners: set[str], binds: str, value: tuple[int, int] | None = None,
          results: Iterable[str] = (), draws: str = "", pure: bool = False) -> HookSpec:
   """!Builds one hook catalog entry.
   @param name     DSL name
   @param owners   owner kinds that may have handlers for it
   @param binds    what the context registers hold (for the editor)
   @param value    range of ctx.value, or None when the hook has no value
   @param results  verdict statements allowed (immune, block)
   @param draws    the engine's draws around the hook (for the editor)
   @param pure     its handlers may only read, modify and give its verdicts (G1: the AI runs them to score)
   @return the entry
   """
   return {"name": name, "owners": owners, "binds": binds, "value": value, "results": set(results), "draws": draws,
           "pure": pure}


## Damage values a hook sees.
DAMAGE_RANGE = (0, U16_MAX)
## Stage deltas a stage_change hook sees.
STAGE_DELTA_RANGE = (INT8_MIN, INT8_MAX)
## Move phases (owner kind move).
MOVE_PHASES = ["use", "target", "try_hit", "hit", "after_move", "land", "ai_score"]
## Phases that may suspend (yield / ask): move phases and condition residual hooks.
YIELD_PHASES = {"use", "hit", "after_move"}
## Condition hooks that may suspend.
YIELD_HOOKS = {"residual", "side_residual", "field_residual"}
## Every hook and move phase in Hook order (capsule::battle::effect::Hook).
HOOKS = [
   _hook("switch_in", BATTLER_KINDS, "holder = the battler that entered"),
   _hook("switch_out", BATTLER_KINDS, "holder = the battler leaving (before resets)"),
   _hook("faint", BATTLER_KINDS, "holder = the fainted battler, other = who caused it"),
   _hook("turn_start", BATTLER_KINDS, "holder = each active battler"),
   _hook("before_move", BATTLER_KINDS, "holder = user = the battler acting, move = its move; stop: turn lost",
         draws="sources in turn.effect_order"),
   _hook("used_move", BATTLER_KINDS, "holder = user, after the move's costs were paid"),
   _hook("before_accuracy", BATTLER_KINDS, "holder = target, other = user", results=["block"],
         draws="the accuracy pipeline draws after it"),
   _hook("try_hit", BATTLER_KINDS | {"move"}, "holder = target, other = user", results=["immune", "block"]),
   _hook("before_damage", BATTLER_KINDS, "holder = target, other = user, value = damage", value=DAMAGE_RANGE),
   _hook("damaging_hit", BATTLER_KINDS, "holder = target, other = user, value = damage dealt", value=DAMAGE_RANGE),
   _hook("after_hit", BATTLER_KINDS, "holder = user, other = target, value = dealt", value=DAMAGE_RANGE),
   _hook("after_move", BATTLER_KINDS | {"move"}, "holder = user"),
   _hook("set_status", BATTLER_KINDS, "holder = the battler getting a status, other = its source",
         results=["block"]),
   _hook("stage_change", BATTLER_KINDS, "holder = the battler, other = the source, value = delta",
         value=STAGE_DELTA_RANGE, results=["block"]),
   _hook("update", BATTLER_KINDS, "holder = the battler whose HP changed"),
   _hook("residual", BATTLER_KINDS, "holder = each active battler, end of turn"),
   _hook("order", BATTLER_KINDS, "holder = the battler, value = its turn-order key", value=(INT32_MIN, INT32_MAX),
         draws="may draw (Quick Claw style)"),
   _hook("start", CONDITION_KINDS, "condition-own: holder = the battler (side: one of the side), value = the "
         "turns asked for (0: its own)"),
   _hook("already", CONDITION_KINDS, "condition-own: the condition was already there (a side layer was added)"),
   _hook("end", CONDITION_KINDS, "condition-own: the condition ended"),
   _hook("side_residual", {"side_condition"}, "holder = each active battler of the side (turn order), end of turn"),
   _hook("field_residual", {"weather", "field_condition"},
         "holder = each active battler (turn order), end of turn (the weather's before the battlers' residual)"),
   _hook("use", {"move"}, "move phase: holder = user = the move's user; may fail, lock, switch, use_move"),
   _hook("target", {"move"}, "move phase: holder = user; set_target chooses the target"),
   _hook("hit", {"move"}, "move phase per target: holder = user, target = the target; default `damage;`"),
   _hook("try_start", CONDITION_KINDS, "condition-own: about to start on holder (other = its source, value = the "
         "turns asked for); immune / block refuse it, fail fails the run that started it", results=["immune", "block"]),
   # E13b: trapping, redirection and delayed effects
   _hook("try_switch", BATTLER_KINDS, "holder = the battler choosing to switch out or run away, value = it; "
         "per_foe: holder = each active foe of it, other = it; block: it cannot (say why); forced switches do not "
         "ask", results=["block"]),
   _hook("redirect", BATTLER_KINDS, "holder = each active battler but the user (position order), other = user, "
         "target = the target the user's chosen single-target move picked, value = 255 (no claim); modify set "
         "BATTLER claims the move: the first valid claim wins", value=(0, U8_MAX)),
   _hook("land", {"move"}, "move phase: the move's pending effect (delay) lands at the end of a turn: holder = user = "
         "who used it, target = the battler at the position, value = the stored amount"),
   # G2 (D-26 section 2): an item used from the bag; fail: it has no effect (it is not consumed)
   _hook("use_field", {"item"}, "out of battle (a field context): holder = the party member the item is used on "
         "(its record as a battler; none for target none), move = the chosen move slot's move (target move_slot); "
         "fail: no effect, the item stays"),
   _hook("use_battle", {"item"}, "the battle's Item action (G3): holder = the target (member or battler), "
         "other = the user's active battler; fail: no effect"),
   # G1 (D-26 section 1): an AI profile's hook rule; pure (no draw, no event, no change)
   _hook("ai_score", BATTLER_KINDS | {"move"}, "the AI scores a move: user = the AI's battler, target = the move's "
         "target, holder = the user (the move's own handler, its owners) then the target (its owners; other = the "
         "user), value = the score so far; modify changes it, block vetoes the move; move none: a switching rule asks "
         "what switching out is worth (value 0 in; it switches when that beats the best move)",
         value=(INT32_MIN, INT32_MAX), results=["block"], pure=True),
   # C6 (review4 finding 19): the weather changed; forms that follow it change at once
   _hook("weather_changed", BATTLER_KINDS, "holder = each active battler (position order), other = who changed the "
         "weather, value = the weather acting now (255: none; a suppressed weather reads none)", value=(0, U8_MAX)),
   # F1 (D-27 section 1): an encounter roll out of battle (a field context: holder = the party's lead), before the
   # pick (the encounter_* primitives change the roll; block: no encounter) and after it (target = the creature
   # made, its record; role writes go into it; encounter_reroll picks again; block: no encounter)
   _hook("encounter", ENCOUNTER_OWNERS, "out of battle, before the pick: holder = user = the party's lead (a field "
         "context; the game's global rules, then its passive, then its held item), value = the method rolling; "
         "encounter_rate / encounter_levels / encounter_level_bias / encounter_skip_below / encounter_type / "
         "encounter_field change the roll, block: no encounter", value=(0, MAX_ENCOUNTER_METHOD), results=["block"]),
   _hook("encounter_made", ENCOUNTER_OWNERS, "out of battle, after the pick, before the battle: holder = user = the "
         "lead, target = other = the creature made (its record: `target.role(NAME) = ...` writes it), value = its "
         "level; encounter_reroll picks another, block: no encounter", value=(0, U8_MAX), results=["block"]),
   # F3 (D-27 section 4): a step effect's run out of battle, every N steps of the walker, on each party member (a
   # field context: its record as a battler; its role writes go into it); the engine applies the effect's damage
   # rule (faint / survive at 1) after the handlers
   _hook(STEPS_HOOK, STEP_OWNERS, "out of battle, every N steps: holder = user = target = each party member (a field "
         "context; the game's global rules, then its status, its passive, its held item), value = the step effect "
         "running (`field.step_effect == NAME`); resource writes (`holder.resource(hp) -= 1`: out of battle the "
         "member is on no position, damage_hp needs one), restore, cure and role writes change the member, the "
         "effect's damage rule keeps it at 1 or lets it faint; fail / block end its owner's run",
         value=(0, MAX_STEP_EFFECT), results=["block"]),
   # B1 (D-28 section 1): a combination's result record, built out of battle (a field context: holder = user = target
   # = the result, made at its level; input.0 .. input.N-1 / input.ROLE are the inputs' records, read only)
   _hook(COMBINE_HOOK, COMBINE_OWNERS, "out of battle, a combination: holder = user = target = the result (a field "
         "context; its record as a battler), input.0 / input.ROLE = the inputs (read only), value = the method "
         "(`field.combine_method == NAME`), field.combine_inputs = their count; role writes and the inherit "
         "primitives write the result's record (inherit ROLE from input.X, inherit_pick ROLE N, inherit_odds ROLE "
         "from input.X N D, inherit_add ROLE AMOUNT); fail / block refuse the combination",
         value=(0, MAX_COMBINE_METHOD), results=["block"]),
   # B2 (D-28 section 3): an egg cycle (the structure's eggs.step_effect running): each member that is not an egg is
   # asked how many steps the eggs' counters lose this run (a warming passive doubles them)
   _hook(HATCH_HOOK, HATCH_OWNERS, "out of battle, an egg cycle: holder = user = target = each party member that is "
         "not an egg (a field context; the game's global rules, then its passive, then its held item), value = the "
         "steps each egg's hatch counter loses this run (the egg cycle's every); modify changes it (`modify by 2`: "
         "hatching twice as fast); the largest answer of the members counts", value=(0, MAX_HATCH_STEPS)),
]
## Hook number by name.
HOOK = {h["name"]: i for i, h in enumerate(HOOKS)}
## Bits per word of an owner's hook mask (capsule::battle::effect::kHookMaskWordBits; G3: the mask is as many
## 32-bit words as the hooks need).
HOOK_MASK_WORD_BITS = 32
## The low bits of a hook mask word.
HOOK_MASK_WORD = (1 << HOOK_MASK_WORD_BITS) - 1


def hook_mask_words() -> int:
   """!Words of an owner's hook mask (capsule::battle::effect::kHookMaskWords): one per 32 hooks.
   @return the count
   """
   return (len(HOOKS) + HOOK_MASK_WORD_BITS - 1) // HOOK_MASK_WORD_BITS


def hook_mask_cpp(hooks: int) -> str:
   """!An owner's hook mask as a C++ initializer of capsule::battle::effect::HookMask.
   @param hooks  bit per hook (bit i: HOOKS[i])
   @return e.g. "{{0x00000001u, 0x00000000u}}"
   """
   words = [f"0x{(hooks >> (k * HOOK_MASK_WORD_BITS)) & HOOK_MASK_WORD:08X}u" for k in range(hook_mask_words())]
   return "{{" + ", ".join(words) + "}}"
## The default point catalog (a structure without "points"; a battle passes its ruleset's points, whose ids are their
## declaration order): name and the range of the value they modify.
POINTS = {"power": (0, U16_MAX), "move_type": (0, U8_MAX), "accuracy": (0, U16_MAX), "evasion": (0, U16_MAX),
          "crit": (0, U8_MAX), "damage": (0, U16_MAX), "speed": (0, U16_MAX), "offense": (0, U16_MAX),
          "defense": (0, U16_MAX), "priority": (INT8_MIN, INT8_MAX)}

# -- refs (what expressions read) --------------------------------------------------------------------------------------

class RefSpec(NamedTuple):
   """!What a ref reads."""

   ## The Ref (REFS name), None for a Has / role read.
   ref: str | None
   ## Kind of the value: int, bool, battler, or a catalog kind (type, status, move, ...).
   kind: str
   ## What the argument names: a structure list, "counters", "words", "index", "has" or None.
   arg: str | None
   ## Range of the value.
   rng: tuple[int, int]
   ## Whether `ref = expr;` may write it.
   writable: bool
   ## Has kind for has(...) reads ("?": looked up by name).
   has: str | None


def _ref(ref: str | None, kind: str = "int", arg: str | None = None, rng: tuple[int, int] = (0, U16_MAX),
         writable: bool = False, has: str | None = None) -> RefSpec:
   """!Builds one ref spec.
   @param ref       the Ref (REFS name), None for a Has / role read
   @param kind      value kind: int, bool, battler, or a catalog kind (type, status, move, ...)
   @param arg       what the argument names: a structure list, "counters", "words", "index" or None
   @param rng       range of the value
   @param writable  whether `ref = expr;` may write it
   @param has       Has kind for has(...) reads
   @return the spec
   """
   return RefSpec(ref, kind, arg, rng, writable, has)


## Range of a 0/1 value.
BOOL_RANGE = (0, 1)
## Range of a byte value.
BYTE_RANGE = (0, U8_MAX)
## Battler fields: SUBJECT.FIELD.
BATTLER_REFS = {
   "hp": _ref("hp"), "max_hp": _ref("max_hp"), "level": _ref("level", rng=BYTE_RANGE),
   "stat": _ref("stat", arg="stats"), "stage": _ref("stage", arg="stages", rng=(INT8_MIN, INT8_MAX)),
   "status": _ref("status", "status", rng=BYTE_RANGE),
   "status_counter": _ref("status_counter", rng=BYTE_RANGE, writable=True),
   "status_aux": _ref("status_aux", rng=BYTE_RANGE, writable=True),
   "counter": _ref("counter", arg="counters", rng=BYTE_RANGE, writable=True),
   "word": _ref("word", arg="words", writable=True),
   "type": _ref("type", "type", arg="index", rng=BYTE_RANGE), "passive": _ref("passive", "passive", rng=BYTE_RANGE),
   "item": _ref("item", "item"), "species": _ref("species", "species"), "last_move": _ref("last_move", "move"),
   "taken.amount": _ref("taken_amount"), "taken.from": _ref("taken_from", "battler", rng=(0, MAX_BATTLERS - 1)),
   "taken.move": _ref("taken_move", "move"), "taken.category": _ref("taken_category", "category", rng=BYTE_RANGE),
   "taken.turn": _ref("taken_turn"), "position": _ref("position", rng=(0, MAX_POSITIONS - 1)),
   "side": _ref("side", rng=(0, MAX_SIDES - 1)), "party": _ref("party", rng=(0, MAX_PARTIES - 1)),
   "species_field": _ref("species_field", arg="species_fields"),
   "alive": _ref("alive", "bool", rng=BOOL_RANGE), "can_switch": _ref("can_switch", "bool", rng=BOOL_RANGE),
   "acted": _ref("acted", "bool", rng=BOOL_RANGE), "locked": _ref("locked", "bool", rng=BOOL_RANGE),
   "has": _ref(None, "bool", arg="has", rng=BOOL_RANGE, has="?"),
   "has_type": _ref(None, "bool", arg="types", rng=BOOL_RANGE, has="type"),
   "role": _ref(None, "int", arg="roles", rng=(0, INT32_MAX)),
   # D-25 resources by name: a battler pool, or the context move's slot of a move pool (writes clamp to the max)
   "resource": _ref("resource", arg="resources", writable=True), "resource_max": _ref("resource_max", arg="resources"),
   # E13b: the last move that targeted it (any move), its position's pending effect, its party's controller and kind
   "targeted_by": _ref("targeted_by", "move"), "delayed_turns": _ref("delayed_turns", rng=BYTE_RANGE),
   "human": _ref("human", "bool", rng=BOOL_RANGE), "unowned": _ref("unowned", "bool", rng=BOOL_RANGE),
   # C5: on the human player's side (the side of the first human party), whoever controls the battler
   "player_side": _ref("player_side", "bool", rng=BOOL_RANGE),
   "gene": _ref("gene", arg="stats", rng=BYTE_RANGE),
}
## Move fields: move.FIELD (the context move).
MOVE_REFS = {
   "type": _ref("move_type", "type", rng=BYTE_RANGE), "category": _ref("move_category", "category", rng=BYTE_RANGE),
   "power": _ref("move_power"), "accuracy": _ref("move_accuracy"),
   "priority": _ref("move_priority", rng=(INT8_MIN, INT8_MAX)),
   "base": _ref("move_base", arg="resources", rng=BYTE_RANGE), "cost": _ref("move_cost", arg="resources"),
   "field": _ref("move_field", arg="move_fields"),
   "has": _ref(None, "bool", arg="move_flags", rng=BOOL_RANGE, has="move_flag"),
}
## Field fields: field.FIELD.
FIELD_REFS = {
   "weather": _ref("weather_id", "weather", rng=BYTE_RANGE), "weather_turns": _ref("weather_turns", rng=BYTE_RANGE),
   "turns": _ref("field_turns", arg="field_conditions", rng=BYTE_RANGE),
   "has": _ref(None, "bool", arg="field_conditions", rng=BOOL_RANGE, has="field_cond"),
   # F1: a timed field effect runs / what it has left (steps or minutes; 0 out of the engine library's programs)
   "effect_active": _ref("field_effect_active", "bool", arg="field_effects", rng=BOOL_RANGE),
   "effect_remaining": _ref("field_effect_remaining", arg="field_effects", rng=(0, U16_MAX)),
   # F3: the step effect a steps handler runs for (the hook's value; `field.step_effect == NAME`)
   STEP_EFFECT_REF: _ref("value", "step_effect", rng=(0, MAX_STEP_EFFECT)),
   # B1: the combination method a combine handler runs for (the hook's value) and its input count
   COMBINE_METHOD_REF: _ref("value", "combine_method", rng=(0, MAX_COMBINE_METHOD)),
   COMBINE_INPUTS_REF: _ref("dealt", rng=(0, MAX_COMBINE_INPUTS)),
}
## Side fields: side(SUBJECT).FIELD.
SIDE_REFS = {
   "turns": _ref("side_turns", arg="side_conditions", rng=BYTE_RANGE),
   "layers": _ref("side_layers", arg="side_conditions", rng=BYTE_RANGE),
   "active_count": _ref("side_active", rng=(0, MAX_POSITIONS)),
   "has": _ref(None, "bool", arg="side_conditions", rng=BOOL_RANGE, has="side_cond"),
}
## Global reads (bare words).
GLOBAL_REFS = {
   "turn": _ref("turn"), "hits": _ref("hits", rng=BYTE_RANGE), "crit": _ref("crit", "bool", rng=BOOL_RANGE),
   # G1: a decoy took the move's latest hit (its before_damage handler said `absorb`)
   "absorbed": _ref("absorbed", "bool", rng=BOOL_RANGE),
   "value": _ref("value", rng=(INT32_MIN, INT32_MAX)), "dealt": _ref("dealt", rng=(0, INT32_MAX)),
   "move": _ref("move", "move"), "owner": _ref("owner"),
}
## Kinds whose names are values (a name in an expression resolves in one of these lists).
VALUE_KINDS = {"type": "types", "category": "categories", "status": "statuses", "weather": "weathers",
               "move": "moves", "item": "items", "passive": "passives", "species": "species",
               # F3: a step effect's name (`field.step_effect == NAME`)
               "step_effect": "step_effects",
               # B1: a combination method's name (`field.combine_method == NAME`)
               "combine_method": "combine_methods"}
## The word for "none" of a kind (C5: `user.targeted_by == none`): what a ref of that kind reads when there is none.
NONE_WORD = "none"
## Its value per kind: 16-bit ids read 65535, 8-bit ones 255 (the engine's kNone16 / kNone8).
NONE_VALUES = {"move": U16_MAX, "item": U16_MAX, "species": U16_MAX, "type": U8_MAX, "category": U8_MAX,
               "status": U8_MAX, "weather": U8_MAX, "passive": U8_MAX}
## Kinds `holder.has(NAME)` looks a name up in, in order.
HAS_LOOKUP = [("volatile", "volatiles"), ("tag", "tags"), ("status", "statuses")]

## Functions: name -> (min args, max args).
FUNCTIONS = {"min": (2, 2), "max": (2, 2), "clamp": (3, 3), "abs": (1, 1), "pick": (2, MAX_PICK + 1),
             "scale": (2, 2), "chance": (2, 2), "between": (2, 2), "rand": (1, 1), "is_foe": (2, 2),
             "is_ally": (2, 2), "odds": (2, 4)}
## Battlers odds() is between when the logic names none: the user against the target (E9).
ODDS_DEFAULT_SUBJECTS = ("user", "target")
## Largest odds() operand (capsule::battle::effect_odds_of() clamps n and m to 1..this, kMaxU16): an operand whose
## analysed range passes it is refused; one whose range is unknown is clamped at run time.
ODDS_OPERAND_MAX = 0xFFFF
## The odds() operands' names (messages), in argument order.
ODDS_OPERANDS = ("n", "m")
## The argument index of odds()'s first battler.
ODDS_FIRST_BATTLER = 2
## Functions that draw.
RNG_FUNCTIONS = {"chance", "between", "rand", "odds"}
## Words that read a context register or a global (never local names).
CONTEXT_WORDS = set(BATTLER_SUBJECTS) | set(GLOBAL_REFS) | {"field", "side"}

# -- primitives (statement signatures) ---------------------------------------------------------------------------------

## A primitive's parameters: (AST field, kind). Kinds: subj, side (`side(subject)`), id:LIST (a name),
## opt_id:LIST, expr, opt_expr, exprs (comma list), flags:A,B (words -> booleans),
## mode:DEFAULT:A,B (one word, optional with a default), kw:WORD (`WORD expr`, optional).
PRIMITIVES: dict[str, list[tuple[str, str]]] = {
   "damage": [],
   "damage_hp": [("who", "subj"), ("amount", "expr"), ("", "flags:typed,quiet,no_faint_check")],
   "self_hit": [("power", "expr")],
   "heal": [("who", "subj"), ("amount", "expr")],
   "inflict": [("who", "subj"), ("status", "id:statuses"), ("", "flags:quiet")],
   "cure": [("who", "subj"), ("", "flags:quiet")],
   "stage": [("who", "subj"), ("stage", "id:stages"), ("delta", "expr"), ("", "flags:quiet,silent")],
   "vol_on": [("who", "subj"), ("volatile", "id:volatiles"), ("turns", "opt_expr")],
   "vol_off": [("who", "subj"), ("volatile", "id:volatiles")],
   "side_on": [("side", "side"), ("condition", "id:side_conditions"), ("turns", "opt_expr"), ("", "flags:add_layer")],
   "side_off": [("side", "side"), ("condition", "id:side_conditions")],
   "field_on": [("condition", "id:field_conditions"), ("turns", "opt_expr")],
   "field_off": [("condition", "id:field_conditions")],
   "weather": [("weather", "id:weathers"), ("turns", "opt_expr")],
   "switch": [("who", "subj"), ("mode", "mode:ask:pass,ask,random")],
   "lock": [("who", "subj"), ("", "flags:free")],
   "unlock": [("who", "subj")],
   # G1: `unless FLAG` refuses (fails the run on) none or a move with that move flag (uncallable, unmirrorable, ...)
   "use_move": [("who", "subj"), ("move", "expr"), ("unless", "kwid:unless:move_flags")],
   "set_target": [("who", "expr")],
   "credit": [("amount", "expr")],
   "consume_item": [("who", "subj")],
   "set_item": [("who", "subj"), ("item", "expr")],
   "set_passive": [("who", "subj"), ("passive", "expr")],
   "set_types": [("who", "subj"), ("types", "exprs")],
   "modify": [("mode", "mode::by,add,set"), ("value", "expr")],
   "immune": [("heal", "kw:heal")],
   "block": [("text", "opt_id:texts")],
   "text": [("text", "id:texts")],
   "say": [("msg", "id:messages"), ("at", "kws:at"), ("about", "kws:about"), ("value", "kw:value")],
   "announce": [],
   "cue": [("cue", "id:cues")],
   # D-25: refill a resource (by amount, or to its maximum without one) / add raises to a move resource's slots
   "restore": [("who", "subj"), ("resource", "id:resources"), ("amount", "opt_expr"), ("", "flags:every_slot")],
   "raise": [("who", "subj"), ("resource", "id:resources"), ("amount", "expr"), ("", "flags:every_slot")],
   # E13b: every stage back to 0 (no texts, no stage_change hooks); a pending effect of the move at a battler's
   # position (its land phase runs after `turns` ends of turn); the reward pool; the battle's end (nobody wins)
   "reset_stages": [("who", "subj")],
   "delay": [("who", "subj"), ("amount", "expr"), ("turns", "expr")],
   # G1: a delayed hit (structure delayed_hits): a snapshot of the user now, the land phase's `damage` works it out;
   # in before_damage, `absorb` marks the hit as taken by a decoy (ref `absorbed`)
   "delay_hit": [("who", "subj"), ("turns", "expr"), ("hit", "id:delayed_hits")],
   "absorb": [],
   "reward": [("amount", "expr")],
   "end_battle": [],
   # G2: offer a move as a level-up does (pushes 0: knows it or cannot, 1: learned, 2: offered, an answer pending)
   "teach": [("who", "subj"), ("move", "expr")],
   # G3 (D-26 section 3): an item's use_battle logic tries to recruit a battler (structure recruit: the checks
   # against the ruleset's recruit_odds with this bonus; success ends the battle; refused: the run fails)
   "recruit": [("who", "subj"), ("bonus", "expr")],
   # G4 (D-26 section 4): a form by name (`base`: the species' own; pushes 1 when it changed), the running item's
   # item_use transformation rules on a field context's member (none holds: the run fails), a training value
   # within the structure's caps (pushes the change made)
   "form": [("who", "subj"), ("form", "id:forms")],
   "transform": [("who", "subj")],
   "train": [("who", "subj"), ("stat", "id:stats"), ("amount", "expr")],
   # F1 (D-27 section 1): an encounter hook changes the roll (operands within ENCOUNTER_ARGS): the trigger odds
   # times N / D, the levels' bounds moved, the level pipeline's bias, no encounter below a level, the pick kept to
   # a type's or a species field value's entries with odds N in D, another pick (encounter_made only)
   "encounter_rate": [("n", "expr"), ("d", "expr")],
   "encounter_levels": [("lo", "expr"), ("hi", "expr")],
   "encounter_level_bias": [("amount", "expr")],
   "encounter_skip_below": [("level", "expr")],
   "encounter_type": [("type", "id:types"), ("n", "expr"), ("d", "expr")],
   "encounter_field": [("field", "id:species_fields"), ("value", "expr"), ("n", "expr"), ("d", "expr")],
   "encounter_reroll": [],
   # F1 (D-27 section 2): start a timed field effect by its stack rule (refused, e.g. Refuse while it runs: the run
   # fails, so an item used for it stays)
   "field_effect": [("mode", "mode::start"), ("effect", "id:field_effects"), ("amount", "expr")],
   # B1 (D-28 section 1): the combine hook's inheritance primitives on the result's record: copy a role from an
   # input, N elements of a role (a per-stat group) each from a random input, a role from an input with odds N in D,
   # add to a counter role (clamped to its range)
   "inherit": [("role", "id:roles"), ("from", "kws:from")],
   "inherit_pick": [("role", "id:roles"), ("count", "expr")],
   "inherit_odds": [("role", "id:roles"), ("from", "kws:from"), ("n", "expr"), ("d", "expr")],
   "inherit_add": [("role", "id:roles"), ("amount", "expr")],
}
## B1: the inheritance primitives in capsule::combine::InheritOp order (the Inherit op's mode).
INHERIT_KINDS = ["inherit", "inherit_pick", "inherit_odds", "inherit_add"]
## B1: operands the Inherit op pops (the unused ones are pushed as 0).
INHERIT_OPERANDS = 2
## B1: the most elements inherit_pick takes (a per-stat role's elements: capsule::battle::kMaxStats).
MAX_INHERIT_PICK = 32
## Encounter primitives in capsule::battle::EncounterOp order (the Encounter op's mode).
ENCOUNTER_KINDS = ["encounter_rate", "encounter_levels", "encounter_level_bias", "encounter_skip_below",
                   "encounter_type", "encounter_field", "encounter_reroll"]
## Largest odds operand of encounter_rate / encounter_type / encounter_field.
MAX_ENCOUNTER_ODDS = U8_MAX
## Largest level shift of encounter_levels.
MAX_LEVEL_SHIFT = U8_MAX
## Each encounter primitive's operands: (field, lo, hi), in push order (the Encounter op takes three).
ENCOUNTER_ARGS = {
   "encounter_rate": [("n", 0, MAX_ENCOUNTER_ODDS), ("d", 1, MAX_ENCOUNTER_ODDS)],
   "encounter_levels": [("lo", -MAX_LEVEL_SHIFT, MAX_LEVEL_SHIFT), ("hi", -MAX_LEVEL_SHIFT, MAX_LEVEL_SHIFT)],
   "encounter_level_bias": [("amount", 0, U8_MAX)],
   "encounter_skip_below": [("level", 0, U8_MAX)],
   "encounter_type": [("n", 0, MAX_ENCOUNTER_ODDS), ("d", 1, MAX_ENCOUNTER_ODDS)],
   "encounter_field": [("value", 0, U16_MAX), ("n", 0, MAX_ENCOUNTER_ODDS), ("d", 1, MAX_ENCOUNTER_ODDS)],
   "encounter_reroll": [],
}
## Operands the Encounter op pops (the unused ones are pushed as 0).
ENCOUNTER_OPERANDS = 3
## Hooks the encounter primitives belong to (encounter_reroll: encounter_made only).
ENCOUNTER_HOOKS = {"encounter", "encounter_made"}
## C8: the encounter hook without a creature: `target` is the holder's own record there, so writes through it are
## refused (encounter_made's target is the creature made).
ENCOUNTER_TARGETLESS_HOOK = "encounter"
## The subject name of a hook's target (BATTLER_SUBJECTS).
TARGET_SUBJECT = "target"
## Most checks one `recruit` makes (capsule::battle::kMaxRecruitChecks).
MAX_RECRUIT_CHECKS = 8
## Events of one recruit moment: its Recruit event and its message.
RECRUIT_MOMENT_EVENTS = 2
## Moments after the checks: the outcome and where the record went.
RECRUIT_END_MOMENTS = 2
## Events one `recruit` may queue: every check's moment, then the outcome's and the placement's.
RECRUIT_EVENTS = (MAX_RECRUIT_CHECKS + RECRUIT_END_MOMENTS) * RECRUIT_MOMENT_EVENTS
## Events of a form change (G4): its Transform event and an Hp event.
FORM_EVENTS = 2
## Highest amount `field_effect start NAME N` takes (battlec/field.py MAX_AMOUNT: the effect's u16 variable; C8).
FIELD_EFFECT_MAX_AMOUNT = U16_MAX
## Most moves one level teaches (battlec/content.py MAX_LEARN_PER_LEVEL), a Learned message each.
LEARN_PER_LEVEL = 4
## Events of a transformation (G4): its Hp and Transform events, then the learn messages of its level.
TRANSFORM_EVENTS = FORM_EVENTS + LEARN_PER_LEVEL
## Events of a training change (G4): an Hp event (the stats follow).
TRAIN_EVENTS = 1
## Flag bits per primitive flag word (effect.h kDamage* / kInflictQuiet / kSideAddLayer / kLockNoPp).
FLAG_BITS = {"typed": 1, "quiet": 2, "no_faint_check": 4, "add_layer": 1, "free": 1, "every_slot": 1}
## Restore without an amount: to the maximum (kResourceFull).
RESOURCE_FULL = 2
## Primitive flag words that are gone, with what replaced them.
RETIRED_FLAGS = {("lock", "no_pp"): "lock's no_pp is `free` now (D-25: the locked action pays no cost)"}
## Inflict's and cure's own flag bit for `quiet` (kInflictQuiet, kCureQuiet).
INFLICT_QUIET = 1
## Stage's `quiet`: a bit of its stage id8 (kStageQuiet; the stage is the bits below, kStageIdMask).
STAGE_QUIET = 0x80
## Stage's `silent`: no message at all (kStageSilent, E13b).
STAGE_SILENT = 0x40
## Largest stage id (kStageIdMask).
MAX_STAGE_ID = STAGE_SILENT - 1
## Primitive -> opcode (the ones that map one to one).
PRIMITIVE_OPS = {"damage_hp": "DamageHp", "self_hit": "SelfHit", "heal": "Heal", "inflict": "Inflict",
                 "cure": "Cure", "stage": "Stage", "vol_on": "VolOn", "vol_off": "VolOff", "side_on": "SideOn",
                 "side_off": "SideOff", "field_on": "FieldOn", "field_off": "FieldOff", "weather": "Weather",
                 "lock": "Lock", "unlock": "Unlock", "use_move": "UseMove", "set_target": "SetTarget",
                 "credit": "Credit", "consume_item": "ConsumeItem", "set_item": "SetItem",
                 "set_passive": "SetPassive", "set_types": "SetTypes", "text": "Text", "say": "Say",
                 "announce": "Announce", "cue": "Cue", "restore": "Restore", "raise": "Raise",
                 "reset_stages": "ResetStages", "delay": "Delay", "reward": "Reward", "end_battle": "EndBattle",
                 "teach": "Teach", "delay_hit": "DelayHit", "absorb": "Absorb", "recruit": "Recruit",
                 "form": "Form", "transform": "Transform", "train": "Train",
                 # F1: every encounter primitive is the Encounter op (its mode), field_effect start the FieldEffect op
                 **{k: "Encounter" for k in ENCOUNTER_KINDS}, "field_effect": "FieldEffect",
                 # B1: every inheritance primitive is the Inherit op (its mode)
                 **{k: "Inherit" for k in INHERIT_KINDS}}

## Event cost of each statement that emits events, and the hooks it triggers (battler hooks of the subject, or
## condition-own hooks of the named condition: "own:<list>:<start|end|try_start>"; start counts start or already).
OP_EVENTS: dict[str, tuple[int, list[str]]] = {
   "damage": (4, ["before_damage", "damaging_hit", "after_hit", "update", "faint"]),
   # E13b: a typed hit runs the target's before_damage hooks first (primitive_cost() skips them for an untyped one)
   "damage_hp": (2, ["before_damage", "update", "faint"]),
   "self_hit": (3, ["update", "faint"]),
   "heal": (1, ["update"]),
   "inflict": (2, ["own:statuses:try_start", "set_status", "own:statuses:start"]),
   "cure": (1, ["own:statuses:end"]),
   "stage": (1, ["stage_change"]),
   "vol_on": (1, ["own:volatiles:try_start", "own:volatiles:start"]),
   "vol_off": (1, ["own:volatiles:end"]),
   "side_on": (1, ["own:side_conditions:try_start", "own:side_conditions:start"]),
   "side_off": (1, ["own:side_conditions:end"]),
   "field_on": (1, ["own:field_conditions:try_start", "own:field_conditions:start"]),
   "field_off": (1, ["own:field_conditions:end"]),
   "weather": (2, ["own:weathers:try_start", "own:weathers:end", "own:weathers:start"]),
   "switch": (4, ["switch_out", "switch_in"]),
   "lock": (0, []), "unlock": (0, []), "use_move": (0, []), "set_target": (0, []), "credit": (0, []),
   "consume_item": (1, []), "set_item": (1, []), "set_passive": (1, []), "set_types": (1, []),
   "modify": (0, []), "immune": (1, ["update"]), "block": (1, []),
   "text": (1, []), "say": (1, []), "announce": (1, []), "cue": (1, []), "ask": (1, []),
   "restore": (1, ["update"]), "raise": (1, []),
   # E13b: reset_stages emits one Stage event per stage (primitive_cost() counts the structure's stages)
   "reset_stages": (0, []), "delay": (0, []), "reward": (0, []), "end_battle": (1, []),
   "teach": (1, []),
   "delay_hit": (0, []), "absorb": (0, []),
   # G3: an event per check, the outcome and where the record went (and a refusal's message)
   "recruit": (RECRUIT_EVENTS, []),
   # G4: a form change, a transformation (with the learn messages of its level), a training change
   "form": (FORM_EVENTS, []), "transform": (TRANSFORM_EVENTS, []), "train": (TRAIN_EVENTS, []),
   # F1: the encounter primitives change the roll's plan, field_effect a script variable (no events)
   **{k: (0, []) for k in ENCOUNTER_KINDS}, "field_effect": (0, []),
   # B1: the inheritance primitives write the result's record (no events)
   **{k: (0, []) for k in INHERIT_KINDS},
}

## Statement words (never local names).
STATEMENT_WORDS = {"let", "if", "else", "chance", "repeat", "max", "for", "in", "call", "yield", "stop", "fail",
                   "return", "ask", "into", "on", "point", "sub", "tags", "when", "and", "or", "not"}
## Words that end a block's flow.
TERMINAL_STATEMENTS = {"stop", "fail", "return", "block", "immune"}
## Indent of the canonical text.
INDENT = "   "
## Two indents.
INDENT2 = INDENT + INDENT
## Locals a `for` loop takes (the battler, the battlers left).
FOREACH_SLOTS = 2
## Bytes per line of generated byte arrays.
BYTES_PER_LINE = 16
## Separator before a trailing comment.
TRAILING_GAP = "   "
## Exit status of a usage error.
EXIT_USAGE = 2

## Diagnostic codes and their message templates (every one is exercised by tests/test_effect_compiler.py).
MESSAGES = {
   "syntax": "{text}",
   "duplicate-owner": "{kind} {name!r} has logic twice",
   "unknown-owner": "unknown {kind} {name!r} (not in the structure's {list})",
   "duplicate-sub": "sub {name!r} is defined twice",
   "unknown-hook": "unknown hook {name!r}",
   "hook-owner": "a {kind} cannot have `on {hook}`",
   "move-hook": "moves use phases ({phases}), not `on {hook}`",
   "phase-owner": "only moves have phases (`{phase}`); write `on {phase}`",
   "unknown-point": "unknown point {name!r}",
   "unknown-sub": "unknown sub {name!r}",
   "recursion": "recursion: {cycle} (subs may not call themselves)",
   "call-depth": "sub calls nest {depth} deep (at most {limit})",
   "unknown-name": "unknown name {name!r}",
   "ambiguous-name": "{name!r} is ambiguous ({kinds}); compare it with a value of the kind meant",
   "unknown-id": "unknown {what} {name!r}",
   "no-list": "the structure declares no {list} (needed for {name!r})",
   "id-range": "{what} {name!r} has id {id}, above {limit}",
   "unknown-ref": "unknown reference {ref!r}",
   "ref-arg": "{ref!r} {need}",
   "unknown-subject": "unknown subject {name!r}",
   "not-writable": "{ref!r} cannot be written (writable: counters, words, status_counter, status_aux, roles)",
   "unknown-function": "unknown function {name!r}",
   "arity": "{name}() takes {want} argument(s), got {got}",
   "redeclared": "{name!r} is already declared here",
   "reserved": "{name!r} is a reserved word",
   "undeclared": "assignment to {name!r}, which is not declared (use let)",
   "too-many-locals": "{label}: needs {count} locals (at most {limit})",
   "stack-depth": "{label}: needs {count} operand stack slots (at most {limit})",
   "repeat-bound": "repeat max must be 1-{limit}, got {value}",
   "yield-context": "{what} only in a top-level phase (a move's use / hit / after_move, a condition's residual)",
   "damage-context": "damage only in a move's hit or land phase",
   "target-context": "set_target only in a move's target phase",
   "no-value": "modify needs a hook with a value ({hook} has none)",
   "per-foe": "per_foe is a switch_in or try_switch handler's mark (not {hook}'s)",
   "none-kind": "none stands for no {kinds}: compare it with one (or give it where one is expected)",
   "unstored-role": "role {name!r} is not stored (\"stored\": false: computed on read); it cannot be written",
   "no-result": "{what} is not a verdict of {hook}",
   "pure": "point handlers are pure: {what} is not allowed",
   "pure-hook": "{hook} handlers are pure (the AI runs them to score moves): {what} is not allowed",
   "absorb-context": "absorb only in a before_damage handler (a decoy taking the hit)",
   "recruit-context": "recruit only in an item's use_battle handler (the battle's Item action)",
   "encounter-context": "{what} only in an encounter or encounter_made handler (an encounter roll out of battle)",
   "steps-context": "field.step_effect only in a steps handler (a step effect's run out of battle)",
   # B1: the combine hook's reads, subjects and primitives
   "combine-context": "{what} only in a combine handler (a combination out of battle)",
   "unknown-input": "unknown input {name!r} (input.N with N below {limit}, or a slot role of the methods: {roles})",
   "inherit-role": "{what}: role {name!r} cannot be inherited ({why})",
   "reroll-context": "encounter_reroll only in an encounter_made handler (after the pick)",
   "encounter-target": ("on encounter has no creature yet: target is the holder itself, so {ref} is not written "
                        "there (write it in on encounter_made, or through holder on purpose)"),
   "encounter-arg": "{what}'s {which} can reach {lo}..{hi}; it takes {min}..{max}",
   "field-effect-amount": "field_effect start: the amount is {amount}; it is 1 to {max}",
   "point-subject": "point handlers read holder, other, user or target only (not {name!r})",
   "hook-budget": "{label}: worst case {count} events (a hook program allows {limit})",
   "step-budget": "{label}: a segment between yields can emit {count} events (at most {limit} per step)",
   "too-many-types": "set_types takes 1-{limit} types",
   "too-many-tags": "an owner gives at most {limit} tags",
   "unreachable": "unreachable statement",
   "saturation": "{what} can reach [{lo}, {hi}] and saturate at the int32 limits",
   "odds-range": "odds() operand {which} can reach {hi}; the engine takes 1-{limit} (it would clamp it)",
   "program-size": "{label}: {count} bytes of code (at most {limit})",
}


class EffectError(ScriptError):
   """!A syntax error in a source, or a malformed AST."""
   pass


class EffectDiagnostic(Diagnostic):
   """!A diagnostic with a stable code (MESSAGES)."""

   def __init__(self, code: str, severity: str, message: str, file: str = "", line: int = 0, col: int = 0) -> None:
      """!Creates a diagnostic.
      @param code      MESSAGES key
      @param severity  'error' or 'warning'
      @param message   text
      @param file      source file
      @param line      1-based line (0 = none)
      @param col       1-based column
      """
      super().__init__(severity, message, file, line, col)
      ## MESSAGES key.
      self.code = code


# -- integer semantics (mirrors effect.h) ------------------------------------------------------------------------------

def sat(v: int) -> int:
   """!Clamps to int32.
   @param v  value
   @return v limited to int32
   """
   return max(INT32_MIN, min(INT32_MAX, v))


def safe_div(a: int, b: int) -> int:
   """!a / b truncated toward zero, 0 when b is 0, saturating (effect::safe_div).
   @param a  dividend
   @param b  divisor
   @return quotient
   """
   return 0 if b == 0 else sat(rc.cdiv(a, b))


def safe_mod(a: int, b: int) -> int:
   """!a % b with the dividend's sign, 0 when b is 0 (effect::safe_mod).
   @param a  dividend
   @param b  divisor
   @return remainder
   """
   return 0 if b == 0 else rc.cmod(a, b)


def mul_q8(a: int, q: int) -> int:
   """!(a * q) >> 8, saturating (effect::mul_q8).
   @param a  value
   @param q  Q8 factor
   @return scaled value
   """
   return sat((a * q) >> Q8_SHIFT)


def shift_count(b: int) -> int:
   """!A shift count clamped to 0-31.
   @param b  count
   @return clamped count
   """
   return max(0, min(MAX_SHIFT, b))


## Two-operand ops by AST operator: VM semantics.
BINARY = {
   "+": lambda a, b: sat(a + b), "-": lambda a, b: sat(a - b), "*": lambda a, b: sat(a * b), "/": safe_div,
   "%": safe_mod, "<<": lambda a, b: sat(a << shift_count(b)), ">>": lambda a, b: a >> shift_count(b),
   "==": lambda a, b: int(a == b), "!=": lambda a, b: int(a != b), "<": lambda a, b: int(a < b),
   "<=": lambda a, b: int(a <= b), ">": lambda a, b: int(a > b), ">=": lambda a, b: int(a >= b),
}
## Two-operand ops by AST operator: opcode.
BINARY_OPS = {"+": "Add", "-": "Sub", "*": "Mul", "/": "Div", "%": "Mod", "<<": "Shl", ">>": "Shr", "==": "Eq",
              "!=": "Ne", "<": "Lt", "<=": "Le", ">": "Gt", ">=": "Ge"}
## Comparison operators.
COMPARISONS = {"==", "!=", "<", "<=", ">", ">="}
## Two-operand ops by AST operator: the C++ of generated point handlers.
BINARY_CPP = {"+": "effect::sat_add", "-": "effect::sat_sub", "*": "effect::sat_mul", "/": "effect::safe_div",
              "%": "effect::safe_mod", "<<": "effect::sat_shl", ">>": "effect::arith_shr"}


def decimal_q8(text: str) -> int:
   """!A decimal literal in Q8 (rounded half up).
   @param text  e.g. "1.5"
   @return e.g. 384
   """
   v = Fraction(text) * Q8_ONE
   return int(v + ROUND_HALF) if v >= 0 else -int(-v + ROUND_HALF)


def normalize_decimal(text: str) -> str:
   """!The canonical spelling of a decimal literal (no trailing zeros beyond one digit).
   @param text  e.g. "1.50"
   @return e.g. "1.5"
   """
   whole, frac = text.split(".")
   frac = frac.rstrip("0") or "0"
   return f"{int(whole)}.{frac}"


# -- lexer -------------------------------------------------------------------------------------------------------------

## Token pattern: whitespace, comments, strings, decimals, numbers, names, symbols.
TOKEN_RE = re.compile(r"""
    (?P<ws>[ \t\r\n]+) |
    (?P<comment>\#[^\n]*) |
    (?P<string>"(?:[^"\\\n]|\\.)*") |
    (?P<dec>[0-9]+\.[0-9]+) |
    (?P<number>0[xX][0-9A-Fa-f]+|[0-9]+) |
    (?P<name>[A-Za-z_][A-Za-z0-9_]*) |
    (?P<sym><<|>>|==|!=|<=|>=|\+=|-=|[{}()\[\];,.=<>+\-*/%])
""", re.VERBOSE)


class EffectToken(Token):
   """!A token that knows whether it shares its line with the token before (comments)."""

   def __init__(self, kind: str, text: str, line: int, col: int, trailing: bool = False) -> None:
      """!Creates a token.
      @param kind      'comment', 'string', 'dec', 'number', 'name', 'sym' or 'eof'
      @param text      source text (strings: the decoded value; comments: the text after '#')
      @param line      1-based line
      @param col       1-based column
      @param trailing  a comment on the line of the token before it
      """
      super().__init__(kind, text, line, col)
      ## A comment on the same line as the code before it.
      self.trailing = trailing


def tokenize(text: str, file: str = "") -> list[EffectToken]:
   """!Splits a source into tokens.
   @param text  the source
   @param file  its name, for errors
   @return tokens, ending with an 'eof' token
   """
   out: list[EffectToken] = []
   pos = 0
   line = 1
   line_start = 0
   last_line = 0   # line of the last non-comment token
   while pos < len(text):
      m = TOKEN_RE.match(text, pos)
      if not m:
         raise EffectError(f"unexpected character {text[pos]!r}", file, line, pos - line_start + 1)
      kind = m.lastgroup
      value = m.group()
      col = pos - line_start + 1
      if kind == "comment":
         body = value[1:]
         body = body[1:] if body.startswith(" ") else body
         out.append(EffectToken("comment", body.rstrip(), line, col, trailing=last_line == line))
      elif kind == "string":
         out.append(EffectToken("string", unescape(value[1:-1], file, line, col), line, col))
         last_line = line
      elif kind != "ws":
         out.append(EffectToken(kind, value, line, col))
         last_line = line
      newlines = value.count("\n")
      if newlines:
         line += newlines
         line_start = pos + value.rfind("\n") + 1
      pos = m.end()
   out.append(EffectToken("eof", "", line, pos - line_start + 1))
   return out


# -- parser ------------------------------------------------------------------------------------------------------------

## Word operators -> the ruleset parser's symbols.
WORD_OPS = {"and": "&&", "or": "||", "not": "!"}
## The ruleset parser's symbols -> AST operator names.
AST_OPS = {"&&": "and", "||": "or"}
## Identifier pattern (names printed without quotes).
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
## AST position table: id(node) -> (line, col).
Positions = dict[int, tuple[int, int]]
## Debug line table (bench builds only, track_lines()): id(node) -> (node, file, line, col) of every node parsed
## while tracking is on (the node is kept so that its id stays unique).
NODE_SOURCES: dict[int, tuple[Any, str, int, int]] = {}
## Debug line table switch ([on]; track_lines()). Off: parsing and compiling record nothing (GBA builds unchanged).
LINE_TRACKING = [False]


class ExprParser(rc.Parser):
   """!The ruleset expression parser (precedence climbing, unary - and !), extended with shifts, word operators,
   decimals, refs, subjects and the effect functions. Works on the statement parser's token stream."""

   ## Binary operator precedence, lowest first (C order; shifts between comparisons and sums).
   LEVELS = [["||"], ["&&"], ["==", "!="], ["<", "<=", ">", ">="], ["<<", ">>"], ["+", "-"], ["*", "/", "%"]]

   def __init__(self, owner: EffectParser) -> None:
      """!Creates an expression parser at the statement parser's position.
      @param owner  the statement parser
      """
      # rc.Parser's state, set here (not through its __init__) so it starts at the statement parser's position
      ## Tokens in the ruleset parser's form.
      self.tokens = owner.rtoks
      ## Read position (shared with the statement parser).
      self.pos = owner.pos
      ## Input names (none: refs are parsed by atom()).
      self.names: set[str] = set()
      ## Constants (none).
      self.constants: Mapping[str, int] = {}
      ## Location for errors.
      self.where = owner.file
      ## The statement parser (tokens, positions).
      self.owner = owner

   def take(self, op: str | None = None) -> rc.Token:
      """!Consumes a token (errors carry the source position).
      @param op  required operator, or None for any token
      @return the token
      """
      t = self.peek()
      if t is None or t[0] == "eof" or (op is not None and t != ("op", op)):
         tok = self.owner.toks[self.pos]
         got = repr(tok.text) if tok.kind != "eof" else "the end"
         raise EffectError(f"expected {op or 'a value'}, got {got}", self.owner.file, tok.line, tok.col)
      self.pos += 1
      return t

   def expression(self) -> Any:
      """!Parses one expression and lowers it to the AST form.
      @return AST expression
      """
      return self.owner.lower(self.binary(0), self.owner.toks[self.pos])

   def atom(self) -> Any:
      """!Parses an atom: number, decimal, name, string, parentheses, call, ref or subject.
      @return AST expression (or a ruleset tuple, lowered by the caller)
      """
      start = self.owner.toks[self.pos]
      t = self.take()
      if t[0] == "num":
         return t[1]
      if t[0] == "dec":
         return self.owner.at({"dec": normalize_decimal(t[1])}, start)
      if t[0] == "str":
         return self.owner.at({"name": t[1]}, start)
      if t == ("op", "("):
         node = self.expression()
         self.take(")")
         return node
      if t[0] != "name":
         raise EffectError(f"unexpected {start.text!r}", self.owner.file, start.line, start.col)
      name = t[1]
      if name == "side" and self.peek() == ("op", "("):
         self.pos += 1
         subject = self.word("a subject")
         self.take(")")
         if self.peek() != ("op", "."):
            raise EffectError("side(...) needs a field (side(x).layers(c), ...)", self.owner.file, start.line,
                              start.col)
         return self.chain(["side"], start, of=subject)
      if self.peek() == ("op", "("):
         self.pos += 1
         args = [] if self.peek() == ("op", ")") else [self.expression()]
         while self.peek() == ("op", ","):
            self.pos += 1
            args.append(self.expression())
         self.take(")")
         return self.owner.at({"call": name, "args": args}, start)
      if self.peek() == ("op", "."):
         return self.chain([name], start)
      if name in BATTLER_SUBJECTS:
         return self.owner.at({"subject": name}, start)
      if name in GLOBAL_REFS:
         return self.owner.at({"ref": name}, start)
      return self.owner.at({"name": name}, start)

   def word(self, what: str) -> str:
      """!Consumes a name token.
      @param what  what is expected (for the error)
      @return its text
      """
      tok = self.owner.toks[self.pos]
      if tok.kind != "name":
         raise EffectError(f"expected {what}, got {tok.text!r}", self.owner.file, tok.line, tok.col)
      self.pos += 1
      return tok.text

   def chain(self, parts: list[str], start: EffectToken, of: str | None = None) -> dict[str, Any]:
      """!Parses the fields after a ref's base (.field.field, then an optional (arg) or (arg, index)).
      @param parts  the base ("side", "move", "field", a subject or a local)
      @param start  first token (position)
      @param of     side(...)'s subject
      @return the ref node
      """
      node: dict[str, Any] = {"ref": ""}
      if of is not None:
         node["of"] = of
      if parts == [INPUT_SUBJECT] and self.peek() == ("op", ".") and self.owner.toks[self.pos + 1].kind == "number":
         # B1: input.N (a combination's input by index) is a subject: its number is the next part
         self.pos += INPUT_TOKENS
         parts.append(self.owner.toks[self.pos - 1].text)
      while self.peek() == ("op", "."):
         self.pos += 1
         parts.append(self.word("a field name"))
      if self.peek() == ("op", "("):
         self.pos += 1
         node["arg"] = self.ref_arg()
         if self.peek() == ("op", ","):
            self.pos += 1
            index = self.ref_arg()
            if not isinstance(index, int):
               tok = self.owner.toks[self.pos - 1]
               raise EffectError("a ref's second argument is an element number", self.owner.file, tok.line, tok.col)
            node["index"] = index
         self.take(")")
      node["ref"] = ".".join(parts)
      return self.owner.at(node, start)

   def ref_arg(self) -> str | int:
      """!A ref argument: a name, a string or a number.
      @return the name or number
      """
      tok = self.owner.toks[self.pos]
      if tok.kind in ("name", "string"):
         self.pos += 1
         return tok.text
      if tok.kind == "number":
         self.pos += 1
         return parse_int(tok.text)
      raise EffectError(f"expected a name or a number, got {tok.text!r}", self.owner.file, tok.line, tok.col)


class EffectParser:
   """!Recursive-descent parser for one `.ceffect` source."""

   def __init__(self, text: str, file: str = "") -> None:
      """!Creates a parser.
      @param text  the source
      @param file  its name, for errors
      """
      ## Source file.
      self.file = file
      ## Tokens.
      self.toks = tokenize(text, file)
      ## The same tokens for the ruleset expression parser: (kind, value).
      self.rtoks: list[rc.Token] = [self.rtoken(t) for t in self.toks]
      ## Read position.
      self.pos = 0
      ## Node positions.
      self.positions: Positions = {}

   @staticmethod
   def rtoken(t: EffectToken) -> rc.Token:
      """!A token in the ruleset parser's form.
      @param t  the token
      @return (kind, value)
      """
      if t.kind == "number":
         return ("num", parse_int(t.text))
      if t.kind == "dec":
         return ("dec", t.text)
      if t.kind == "string":
         return ("str", t.text)
      if t.kind == "name":
         return ("op", WORD_OPS[t.text]) if t.text in WORD_OPS else ("name", t.text)
      if t.kind == "sym":
         return ("op", t.text)
      return (t.kind, t.text)

   # -- helpers --

   def at(self, node: dict[str, Any], tok: EffectToken) -> dict[str, Any]:
      """!Records a node's position.
      @param node  the node
      @param tok   its first token
      @return the node
      """
      self.positions[id(node)] = (tok.line, tok.col)
      if LINE_TRACKING[0]:
         NODE_SOURCES[id(node)] = (node, self.file, tok.line, tok.col)
      return node

   def peek(self, ahead: int = 0) -> EffectToken:
      """!A token ahead.
      @param ahead  how far (0 = the next one)
      @return the token (eof at the end)
      """
      return self.toks[min(self.pos + ahead, len(self.toks) - 1)]

   def is_sym(self, text: str) -> bool:
      """!Whether the next token is a symbol.
      @param text  the symbol
      @return true when it is
      """
      t = self.peek()
      return t.kind == "sym" and t.text == text

   def is_word(self, text: str) -> bool:
      """!Whether the next token is a name.
      @param text  the name
      @return true when it is
      """
      t = self.peek()
      return t.kind == "name" and t.text == text

   def error(self, message: str, tok: EffectToken | None = None) -> EffectError:
      """!A syntax error at a token.
      @param message  what is wrong
      @param tok      where (default: the next token)
      @return the error (to raise)
      """
      t = tok or self.peek()
      return EffectError(message, self.file, t.line, t.col)

   def expect(self, text: str) -> EffectToken:
      """!Consumes a symbol.
      @param text  the symbol
      @return its token
      """
      t = self.peek()
      if not (t.kind == "sym" and t.text == text):
         raise self.error(f"expected {text!r}, got {t.text or 'the end'!r}")
      self.pos += 1
      return t

   def name(self, what: str) -> str:
      """!Consumes a name token (B1: a subject may be a combination's input, `input.N` or `input.ROLE`).
      @param what  what is expected
      @return its text
      """
      t = self.peek()
      if t.kind != "name":
         raise self.error(f"expected {what}, got {t.text or 'the end'!r}")
      self.pos += 1
      if t.text == INPUT_SUBJECT and self.is_sym("."):
         nxt = self.toks[self.pos + 1]
         if nxt.kind in ("name", "number"):
            self.pos += INPUT_TOKENS
            # C9: `input.METHOD.ROLE` (one method's own slot role; a subject is never followed by a field)
            if nxt.kind == "name" and self.is_sym(".") and self.toks[self.pos + 1].kind == "name":
               role = self.toks[self.pos + 1].text
               self.pos += INPUT_TOKENS
               return f"{INPUT_SUBJECT}.{nxt.text}.{role}"
            return f"{INPUT_SUBJECT}.{nxt.text}"
      return t.text

   def name_or_string(self, what: str) -> str:
      """!Consumes a name or a string (names with spaces are strings).
      @param what  what is expected
      @return the name
      """
      t = self.peek()
      if t.kind not in ("name", "string"):
         raise self.error(f"expected {what}, got {t.text or 'the end'!r}")
      self.pos += 1
      return t.text

   def expr(self) -> Any:
      """!Parses an expression at the current position.
      @return AST expression
      """
      p = ExprParser(self)
      node = p.expression()
      self.pos = p.pos
      return node

   def lower(self, tree: Any, tok: EffectToken) -> Any:
      """!Converts the ruleset parser's tuples to AST nodes (negated numbers fold into literals).
      @param tree  tuple tree with AST leaves
      @param tok   position for new nodes
      @return AST expression
      """
      if not isinstance(tree, tuple):
         return tree
      if tree[0] == "bin":
         _, op, a, b = tree
         return self.at({"op": AST_OPS.get(op, op), "args": [self.lower(a, tok), self.lower(b, tok)]}, tok)
      inner = self.lower(tree[1], tok)
      if tree[0] == "neg" and isinstance(inner, int):
         return -inner
      return self.at({"op": tree[0], "args": [inner]}, tok)

   def comment(self) -> dict[str, Any]:
      """!Consumes a comment token.
      @return the comment node
      """
      t = self.toks[self.pos]
      self.pos += 1
      node: dict[str, Any] = {"stmt": "comment", "text": t.text}
      if t.trailing:
         node["trailing"] = True
      return self.at(node, t)

   # -- file and owners --

   def parse_file(self) -> dict[str, Any]:
      """!Parses the whole source.
      @return the file AST
      """
      items: list[dict[str, Any]] = []
      while self.peek().kind != "eof":
         if self.peek().kind == "comment":
            line = self.peek().line
            node = self.comment()
            # A top-level comment followed by a blank line stands alone (a file header); else it is the next
            # owner's.
            if not node.get("trailing") and self.peek().kind != "eof" and self.peek().line > line + 1:
               node["gap"] = True
            items.append(node)
         else:
            items.append(self.owner())
      return {"schema": SCHEMA_AST, "owners": items}

   def owner(self) -> dict[str, Any]:
      """!Parses an owner block.
      @return the owner node
      """
      start = self.peek()
      kind = self.name("an owner kind (" + ", ".join(OWNER_KINDS) + ")")
      if kind not in OWNER_KINDS:
         raise self.error(f"unknown owner kind {kind!r} (" + ", ".join(OWNER_KINDS) + ")", start)
      t = self.peek()
      if t.kind != "string":
         raise self.error("expected the owner's name as a string")
      self.pos += 1
      node: dict[str, Any] = {"owner": {"kind": kind, "name": t.text}}
      handlers: list[dict[str, Any]] = []
      self.expect("{")
      while not self.is_sym("}"):
         if self.peek().kind == "eof":
            raise self.error("missing '}' at the end of the owner")
         if self.peek().kind == "comment":
            handlers.append(self.comment())
            continue
         self.member(node, handlers)
      self.expect("}")
      node["handlers"] = handlers
      return self.at(node, start)

   def member(self, owner: dict[str, Any], handlers: list[dict[str, Any]]) -> None:
      """!Parses one owner member into the owner or its handler list.
      @param owner     the owner node (tags, announce)
      @param handlers  its handler list
      @return None
      """
      start = self.peek()
      word = self.name("on, point, sub, a phase, tags or announce")
      if word == "tags":
         if "tags" in owner:
            raise self.error("tags are given twice", start)
         self.expect("[")
         tags = [self.name_or_string("a tag")]
         while self.is_sym(","):
            self.pos += 1
            tags.append(self.name_or_string("a tag"))
         self.expect("]")
         self.expect(";")
         owner["tags"] = tags
         return
      if word == "announce":
         self.expect(";")
         owner["announce"] = True
         return
      if word == "sub":
         node: dict[str, Any] = {"sub": self.name("a sub name")}
         node["body"] = self.block()
         handlers.append(self.at(node, start))
         return
      if word in ("on", "point"):
         node = {word: self.name("a hook name" if word == "on" else "a point name")}
         if word == "on" and self.is_word(PER_FOE_WORD):
            self.pos += 1
            node[PER_FOE_WORD] = True
      elif word in MOVE_PHASES:
         node = {"phase": word}
      else:
         raise self.error(f"unknown member {word!r} (on, point, sub, {', '.join(MOVE_PHASES)}, tags, announce)",
                          start)
      if self.is_word("when"):
         self.pos += 1
         node["when"] = self.expr()
      node["body"] = self.block()
      handlers.append(self.at(node, start))

   # -- statements --

   def block(self) -> list[dict[str, Any]]:
      """!Parses a braced block of statements.
      @return the statements
      """
      self.expect("{")
      out: list[dict[str, Any]] = []
      while not self.is_sym("}"):
         if self.peek().kind == "eof":
            raise self.error("missing '}' at the end of a block")
         out.append(self.statement())
      self.expect("}")
      return out

   def statement(self) -> dict[str, Any]:
      """!Parses one statement.
      @return the statement node
      """
      start = self.peek()
      if start.kind == "comment":
         return self.comment()
      if start.kind != "name":
         raise self.error(f"expected a statement, got {start.text!r}")
      word = start.text
      if word == "let":
         self.pos += 1
         node: dict[str, Any] = {"stmt": "let", "name": self.name("a local name")}
         self.expect("=")
         node["value"] = self.expr()
         self.expect(";")
         return self.at(node, start)
      if word == "if":
         return self.if_statement()
      if word == "chance":
         return self.chance_statement()
      if word == "repeat":
         self.pos += 1
         node = {"stmt": "repeat", "count": self.expr()}
         if not self.is_word("max"):
            raise self.error("repeat needs `max N` (its bound)")
         self.pos += 1
         t = self.peek()
         if t.kind != "number":
            raise self.error("repeat max takes a number")
         self.pos += 1
         node["max"] = parse_int(t.text)
         node["body"] = self.block()
         return self.at(node, start)
      if word == "for":
         self.pos += 1
         node = {"stmt": "for", "var": self.name("a loop variable")}
         if not self.is_word("in"):
            raise self.error("expected `in`")
         self.pos += 1
         node["set"] = self.name("a battler set (" + ", ".join(BATTLER_SETS) + ")")
         self.expect("(")
         node["of"] = self.name("a subject")
         self.expect(")")
         node["body"] = self.block()
         return self.at(node, start)
      if word == "call":
         self.pos += 1
         node = {"stmt": "call", "sub": self.name("a sub name")}
         self.expect(";")
         return self.at(node, start)
      if word in ("yield", "stop", "fail", "return"):
         self.pos += 1
         self.expect(";")
         return self.at({"stmt": word}, start)
      if word == "ask":
         self.pos += 1
         node = {"stmt": "ask", "kind": self.name("a request kind (" + ", ".join(ASK_KINDS) + ")"),
                 "who": self.name("a subject")}
         if not self.is_word("into"):
            raise self.error("expected `into NAME`")
         self.pos += 1
         node["into"] = self.name("a local name")
         self.expect(";")
         return self.at(node, start)
      if word in PRIMITIVES and not (self.peek(1).kind == "sym" and self.peek(1).text in ("=", "+=", "-=", ".")):
         return self.primitive()
      return self.assignment()

   def if_statement(self) -> dict[str, Any]:
      """!Parses an if statement with its else-if chain and else block.
      @return the if node
      """
      start = self.peek()
      self.pos += 1
      node: dict[str, Any] = {"stmt": "if", "cond": self.expr(), "then": self.block()}
      if self.is_word("else"):
         self.pos += 1
         node["else"] = [self.if_statement()] if self.is_word("if") else self.block()
      return self.at(node, start)

   def chance_statement(self) -> dict[str, Any]:
      """!Parses a chance n/m statement with its optional else block.
      @return the chance node
      """
      start = self.peek()
      self.pos += 1
      odds = self.expr()
      if not (isinstance(odds, dict) and odds.get("op") == "/"):
         raise self.error("chance takes odds n/m", start)
      node: dict[str, Any] = {"stmt": "chance", "n": odds["args"][0], "m": odds["args"][1], "body": self.block()}
      if self.is_word("else"):
         self.pos += 1
         node["else"] = self.block()
      return self.at(node, start)

   def assignment(self) -> dict[str, Any]:
      """!Parses an assignment (=, += or -=).
      @return the assign node
      """
      start = self.peek()
      p = ExprParser(self)
      target = p.atom()
      self.pos = p.pos
      if not (isinstance(target, dict) and ("ref" in target or "name" in target)):
         raise self.error("expected a statement", start)
      t = self.peek()
      if not (t.kind == "sym" and t.text in ("=", "+=", "-=")):
         raise self.error(f"expected a statement (unknown word {start.text!r})", start)
      self.pos += 1
      node = {"stmt": "assign", "target": target, "op": t.text, "value": self.expr()}
      self.expect(";")
      return self.at(node, start)

   def primitive(self) -> dict[str, Any]:
      """!Parses a primitive statement by its signature (PRIMITIVES).
      @return the primitive node
      """
      start = self.peek()
      word = start.text
      self.pos += 1
      node: dict[str, Any] = {"stmt": word}
      for field, kind in PRIMITIVES[word]:
         head, _, rest = kind.partition(":")
         if head == "subj":
            node[field] = self.name("a subject")
         elif head == "side":
            if not self.is_word("side"):
               raise self.error(f"{word} takes side(subject)")
            self.pos += 1
            self.expect("(")
            node[field] = self.name("a subject")
            self.expect(")")
         elif head == "id":
            node[field] = self.name_or_string(f"a name from {rest}")
         elif head == "opt_id":
            if not self.is_sym(";"):
               node[field] = self.name_or_string(f"a name from {rest}")
         elif head == "expr":
            node[field] = self.expr()
         elif head == "opt_expr":
            _head, _colon, flag_words = PRIMITIVES[word][-1][1].partition(":")
            flags = set(flag_words.split(","))
            if not self.is_sym(";") and not (self.peek().kind == "name" and self.peek().text in flags):
               node[field] = self.expr()
         elif head == "exprs":
            node[field] = [self.expr()]
            while self.is_sym(","):
               self.pos += 1
               node[field].append(self.expr())
         elif head == "flags":
            words = rest.split(",")
            while self.peek().kind == "name" and self.peek().text in words:
               flag = self.peek().text
               if flag in node:
                  raise self.error(f"{flag} is given twice")
               node[flag] = True
               self.pos += 1
            retired = RETIRED_FLAGS.get((word, self.peek().text)) if self.peek().kind == "name" else None
            if retired is not None:
               raise self.error(retired)
         elif head == "mode":
            default, _, choices = rest.partition(":")
            words = choices.split(",")
            if self.peek().kind == "name" and self.peek().text in words:
               node[field] = self.peek().text
               self.pos += 1
            elif default:
               node[field] = default
            else:
               raise self.error(f"{word} takes one of {', '.join(words)}")
         elif head == "kw":
            if self.is_word(rest):
               self.pos += 1
               node[field] = self.expr()
         elif head == "kws":
            if self.is_word(rest):
               self.pos += 1
               node[field] = self.name("a subject")
         elif head == "kwid":
            # G1: `WORD NAME`, optional (a name from a structure list, e.g. use_move ... unless FLAG)
            keyword, _, names = rest.partition(":")
            if self.is_word(keyword):
               self.pos += 1
               node[field] = self.name_or_string(f"a name from {names}")
      self.expect(";")
      return self.at(node, start)


def parse_positions(text: str, file: str = "") -> tuple[dict[str, Any], Positions]:
   """!Parses a source and keeps the node positions (for diagnostics).
   @param text  the source
   @param file  its name
   @return (file AST, positions)
   @throws EffectError on a syntax error
   """
   p = EffectParser(text, file)
   return p.parse_file(), p.positions


def parse(text: str, file: str = "") -> dict[str, Any]:
   """!Parses a `.ceffect` source.
   @param text  the source
   @param file  its name, for errors
   @return the file AST (capsule.effect-ast/1: {"schema", "owners": [owner or comment, ...]})
   @throws EffectError on a syntax error
   """
   return parse_positions(text, file)[0]


# -- formatter ---------------------------------------------------------------------------------------------------------

## Precedence of AST operators (higher binds tighter); atoms above every operator.
PRECEDENCE = {"or": 1, "and": 2, "==": 3, "!=": 3, "<": 4, "<=": 4, ">": 4, ">=": 4, "<<": 5, ">>": 5, "+": 6,
              "-": 6, "*": 7, "/": 7, "%": 7}
## Precedence of unary operators.
UNARY_PRECEDENCE = 8
## Precedence of atoms.
ATOM_PRECEDENCE = 9


def format_name(name: str) -> str:
   """!A name as source: bare when it is a plain identifier that reads back as a name, else quoted.
   @param name  the name
   @return the text
   """
   plain = IDENT_RE.fullmatch(name) and name not in CONTEXT_WORDS and name not in WORD_OPS and \
      name not in FUNCTIONS
   return name if plain else json.dumps(name, ensure_ascii=False)


def format_id(name: str) -> str:
   """!A name in a primitive's or ref's argument position (only spaces and symbols need quotes).
   @param name  the name
   @return the text
   """
   return name if IDENT_RE.fullmatch(name) else json.dumps(name, ensure_ascii=False)


def format_expr(e: Any, prec: int = 0) -> str:
   """!An expression as source, with only the parentheses its precedence needs.
   @param e     AST expression
   @param prec  the precedence the context needs
   @return the text
   """
   if isinstance(e, bool) or not isinstance(e, (int, dict)):
      raise EffectError(f"not an expression: {e!r}")
   if isinstance(e, int):
      text = str(e)
      return f"({text})" if e < 0 and prec > UNARY_PRECEDENCE else text
   if "dec" in e:
      return e["dec"]
   if "name" in e:
      return format_name(e["name"])
   if "subject" in e:
      return e["subject"]
   if "ref" in e:
      parts = e["ref"].split(".")
      text = f"side({e['of']})." + ".".join(parts[1:]) if "of" in e else e["ref"]
      if "arg" in e:
         arg = e["arg"] if isinstance(e["arg"], int) else format_id(e["arg"])
         text += f"({arg}, {e['index']})" if "index" in e else f"({arg})"
      return text
   if "call" in e:
      return f"{e['call']}(" + ", ".join(format_expr(a) for a in e["args"]) + ")"
   op = e["op"]
   if op in ("not", "neg"):
      inner = format_expr(e["args"][0], UNARY_PRECEDENCE)
      text = f"not {inner}" if op == "not" else f"-{inner}"
      return f"({text})" if prec > UNARY_PRECEDENCE else text
   p = PRECEDENCE[op]
   a, b = e["args"]
   text = f"{format_expr(a, p)} {op} {format_expr(b, p + 1)}"
   return f"({text})" if prec > p else text


def format_primitive(s: dict[str, Any]) -> str:
   """!A primitive statement as source (without the ';').
   @param s  the node
   @return the text
   """
   word = s["stmt"]
   parts = [word]
   for field, kind in PRIMITIVES[word]:
      head, _, rest = kind.partition(":")
      if head == "subj" or head == "mode":
         parts.append(s[field])
      elif head == "side":
         parts.append(f"side({s[field]})")
      elif head in ("id", "opt_id"):
         if field in s:
            parts.append(format_id(s[field]))
      elif head in ("expr", "opt_expr"):
         if field in s:
            parts.append(format_expr(s[field]))
      elif head == "exprs":
         parts.append(", ".join(format_expr(x) for x in s[field]))
      elif head == "flags":
         parts.extend(f for f in rest.split(",") if s.get(f))
      elif head == "kw" and field in s:
         parts.extend([rest, format_expr(s[field])])
      elif head == "kws" and field in s:
         parts.extend([rest, s[field]])
      elif head == "kwid" and field in s:
         parts.extend([rest.partition(":")[0], format_id(s[field])])
   return " ".join(parts)


class Formatter:
   """!Writes the canonical text of a file AST."""

   def __init__(self) -> None:
      """!Creates a formatter."""
      ## Output lines.
      self.lines: list[str] = []

   def comment(self, c: Mapping[str, Any], depth: int) -> None:
      """!Writes a comment (trailing ones join the line above).
      @param c      the comment node
      @param depth  indent level
      @return None
      """
      text = "#" + (f" {c['text']}" if c["text"] else "")
      if c.get("trailing") and self.lines:
         self.lines[-1] += TRAILING_GAP + text
      else:
         self.lines.append(INDENT * depth + text)

   def block(self, head: str, body: Sequence[Mapping[str, Any]], depth: int) -> None:
      """!Writes the head and an opening brace, the statements and the closing brace (an empty block: both braces
      on the head's line).
      @param head   the text before the brace
      @param body   statements
      @param depth  indent level of the head
      @return None
      """
      lead = INDENT * depth
      if not body:
         self.lines.append(lead + head + " {}")
         return
      self.lines.append(lead + head + " {")
      for s in body:
         self.statement(s, depth + 1)
      self.lines.append(lead + "}")

   def else_part(self, rest: Sequence[Mapping[str, Any]], depth: int) -> None:
      """!Writes an else block (or an else-if) joined to the closing brace just written.
      @param rest   the else statements
      @param depth  indent level
      @return None
      """
      if len(rest) == 1 and rest[0].get("stmt") == "if":
         self.if_chain(rest[0], depth, "else if")
         return
      if not rest:
         self.lines[-1] += " else {}"
         return
      self.lines[-1] += " else {"
      for x in rest:
         self.statement(x, depth + 1)
      self.lines.append(INDENT * depth + "}")

   def if_chain(self, s: Mapping[str, Any], depth: int, head: str) -> None:
      """!Writes an if statement and its else-if chain.
      @param s      the if node
      @param depth  indent level
      @param head   "if" (a new line) or "else if" (joins the closing brace above)
      @return None
      """
      opener = f"{head} {format_expr(s['cond'])}"
      if head == "if":
         self.block(opener, s["then"], depth)
      else:
         self.lines[-1] += " " + opener + (" {" if s["then"] else " {}")
         if s["then"]:
            for x in s["then"]:
               self.statement(x, depth + 1)
            self.lines.append(INDENT * depth + "}")
      if "else" in s:
         self.else_part(s["else"], depth)

   def statement(self, s: Mapping[str, Any], depth: int) -> None:
      """!Writes one statement.
      @param s      the node
      @param depth  indent level
      @return None
      """
      lead = INDENT * depth
      k = s["stmt"]
      if k == "comment":
         self.comment(s, depth)
      elif k == "let":
         self.lines.append(f"{lead}let {s['name']} = {format_expr(s['value'])};")
      elif k == "assign":
         self.lines.append(f"{lead}{format_expr(s['target'])} {s['op']} {format_expr(s['value'])};")
      elif k == "if":
         self.if_chain(s, depth, "if")
      elif k == "chance":
         head = f"chance {format_expr(s['n'], PRECEDENCE['/'])}/{format_expr(s['m'], PRECEDENCE['/'] + 1)}"
         self.block(head, s["body"], depth)
         if "else" in s:
            if s["else"]:
               self.lines[-1] += " else {"
               for x in s["else"]:
                  self.statement(x, depth + 1)
               self.lines.append(lead + "}")
            else:
               self.lines[-1] += " else {}"
      elif k == "repeat":
         self.block(f"repeat {format_expr(s['count'])} max {s['max']}", s["body"], depth)
      elif k == "for":
         self.block(f"for {s['var']} in {s['set']}({s['of']})", s["body"], depth)
      elif k == "call":
         self.lines.append(f"{lead}call {s['sub']};")
      elif k in ("yield", "stop", "fail", "return"):
         self.lines.append(f"{lead}{k};")
      elif k == "ask":
         self.lines.append(f"{lead}ask {s['kind']} {s['who']} into {s['into']};")
      elif k in PRIMITIVES:
         self.lines.append(f"{lead}{format_primitive(s)};")
      else:
         raise EffectError(f"unknown statement {k!r}")

   def handler(self, h: Mapping[str, Any], depth: int) -> None:
      """!Writes one owner member.
      @param h      the handler node
      @param depth  indent level
      @return None
      """
      if h.get("stmt") == "comment":
         self.comment(h, depth)
         return
      if "sub" in h:
         head = f"sub {h['sub']}"
      elif "on" in h:
         head = f"on {h['on']}" + (f" {PER_FOE_WORD}" if h.get(PER_FOE_WORD) else "")
      elif "point" in h:
         head = f"point {h['point']}"
      else:
         head = h["phase"]
      if "when" in h:
         head += f" when {format_expr(h['when'])}"
      self.block(head, h["body"], depth)

   def owner(self, o: Mapping[str, Any]) -> None:
      """!Writes one owner block.
      @param o  the owner node
      @return None
      """
      head = f"{o['owner']['kind']} {json.dumps(o['owner']['name'], ensure_ascii=False)}"
      members = o.get("handlers", [])
      if not members and "tags" not in o and not o.get("announce"):
         self.lines.append(head + " {}")
         return
      self.lines.append(head + " {")
      # A trailing comment right after "{" stays on the header line; tags and announce follow it.
      rest = list(members)
      while rest and rest[0].get("stmt") == "comment" and rest[0].get("trailing"):
         self.comment(rest.pop(0), 1)
      if "tags" in o:
         self.lines.append(INDENT + "tags [" + ", ".join(format_id(t) for t in o["tags"]) + "];")
      if o.get("announce"):
         self.lines.append(INDENT + "announce;")
      for h in rest:
         self.handler(h, 1)
      self.lines.append("}")


def format(ast: Mapping[str, Any]) -> str:   # noqa: A001 (the API name D-23 gives)
   """!The canonical text of an AST (a file AST, or one owner).
   @param ast  the AST
   @return the text (ends with a newline)
   """
   items = ast["owners"] if "owners" in ast else [ast]
   f = Formatter()
   previous: Mapping[str, Any] | None = None
   for item in items:
      is_comment = item.get("stmt") == "comment"
      # One blank line between top-level items; a comment sticks to the owner after it unless it has a gap.
      sticks = previous is not None and previous.get("stmt") == "comment" and not previous.get("trailing") and \
         not previous.get("gap")
      if previous is not None and not (is_comment and item.get("trailing")) and not sticks:
         f.lines.append("")
      if is_comment:
         f.comment(item, 0)
      else:
         f.owner(item)
      previous = item
   return "\n".join(f.lines) + "\n" if f.lines else ""


# -- JSON --------------------------------------------------------------------------------------------------------------

def _normalize(node: Any) -> Any:
   """!Folds negated integer literals (the canonical AST form) and checks basic shapes.
   @param node  JSON value
   @return the normalized value
   """
   if isinstance(node, list):
      return [_normalize(x) for x in node]
   if not isinstance(node, dict):
      return node
   out = {k: _normalize(v) for k, v in node.items()}
   if out.get("op") == "neg" and isinstance(out.get("args"), list) and len(out["args"]) == 1 and \
         isinstance(out["args"][0], int) and not isinstance(out["args"][0], bool):
      return -out["args"][0]
   return out


def to_json(ast: Mapping[str, Any]) -> str:
   """!An AST as JSON text.
   @param ast  the AST
   @return JSON
   """
   return json.dumps(ast, indent=len(INDENT), ensure_ascii=False) + "\n"


def from_json(data: str | Mapping[str, Any]) -> dict[str, Any]:
   """!An AST from JSON (text or parsed), normalized.
   @param data  JSON text or object
   @return the AST
   @throws EffectError when it is not a capsule.effect-ast/1 file or owner
   """
   obj = json.loads(data) if isinstance(data, str) else data
   if not isinstance(obj, dict) or obj.get("schema", SCHEMA_AST) != SCHEMA_AST:
      raise EffectError(f"not a {SCHEMA_AST} document")
   if "owners" not in obj and "owner" not in obj:
      raise EffectError("an effect AST has \"owners\" (a file) or \"owner\" (one owner)")
   return _normalize(dict(obj))


def owner_ast(ast: Mapping[str, Any], name: str) -> dict[str, Any]:
   """!One owner of a file AST, as a standalone AST (schema included).
   @param ast   the file AST
   @param name  the owner's name, or "kind:name"
   @return the owner AST
   @throws EffectError when there is no such owner, or several
   """
   kind, _, bare = name.rpartition(":") if ":" in name else ("", "", name)
   found = [o for o in ast["owners"] if "owner" in o and o["owner"]["name"] == bare and
            (not kind or o["owner"]["kind"] == kind)]
   if len(found) != 1:
      raise EffectError(f"{'no' if not found else 'more than one'} owner {name!r} (use kind:name)")
   return {"schema": SCHEMA_AST, **found[0]}


# -- names (the structure's lists) -------------------------------------------------------------------------------------

## Structure lists the catalog reads.
LIST_KEYS = ["types", "categories", "statuses", "volatiles", "weathers", "side_conditions", "field_conditions",
             "moves", "items", "passives", "species", "stats", "stages", "move_flags", "tags", "texts", "messages",
             "cues", "roles", "species_fields", "move_fields", "points", "resources", "delayed_hits",
             # G4: the species' form names (effect `form`; "base" first: a species' own form)
             "forms",
             # F1: the timed field effects (effect `field_effect start NAME N`, `field.effect_active(NAME)`)
             "field_effects",
             # F3: the step effects (`field.step_effect == NAME` in a steps handler)
             "step_effects",
             # B1: the combination methods (`field.combine_method == NAME`) and their slot roles (`input.ROLE`:
             # entries {"name", "input": index or None when two methods disagree})
             "combine_methods", "combine_slots"]


class Catalog:
   """!The names a structure declares, with their ids (and the volatile counter / word slots).

   Attributes are documented where __init__ sets them; the ones below are repeated here because Doxygen's
   Python parser otherwise reports their later uses as undocumented members.

   @var counters
   Volatile name -> counter slot.
   @var words
   Volatile name -> word slot.
   @var points
   Point name -> range of its value, in id order.
   """

   def __init__(self, structure: Mapping[str, Any] | None = None) -> None:
      """!Reads the lists of a structure.
      @param structure  parsed structure JSON (None: no names)
      """
      structure = structure or {}
      ## list key -> name -> id (None when the structure has no such list).
      self.ids: dict[str, dict[str, int] | None] = {}
      ## list key -> entries as given.
      self.entries: dict[str, list[Any]] = {}
      for key in LIST_KEYS:
         items = structure.get(key)
         if items is None:
            self.ids[key] = None
            continue
         if not isinstance(items, list):
            raise EffectError(f"structure: {key} must be a list")
         names = [i if isinstance(i, str) else i.get("name") if isinstance(i, dict) else None for i in items]
         if any(not isinstance(n, str) for n in names):
            raise EffectError(f"structure: every {key} entry is a name or an object with \"name\"")
         self.entries[key] = list(items)
         self.ids[key] = {n: i for i, n in enumerate(names)}
      ## Volatile name -> counter slot.
      self.counters: dict[str, int] = {}
      ## Volatile name -> word slot.
      self.words: dict[str, int] = {}
      for v in self.entries.get("volatiles", []):
         if isinstance(v, dict):
            if v.get("counter"):
               self.counters[v["name"]] = len(self.counters)
            if v.get("word"):
               self.words[v["name"]] = len(self.words)
      if len(self.counters) > MAX_COUNTERS or len(self.words) > MAX_WORDS:
         raise EffectError(f"structure: at most {MAX_COUNTERS} volatile counters and {MAX_WORDS} words")
      ## (owner kind, name) -> {text name: text id}: a condition entry's "texts" ({name: id}), which `text NAME`
      ## and `block NAME` in that owner look up before the structure's texts.
      self.owner_texts: dict[tuple[str, str], dict[str, int]] = {}
      ## List key -> {name: turns}: a condition entry's "turns", the duration `vol_on` / `side_on` / `field_on` /
      ## `weather` give it when the statement names none (0 when the entry has none).
      self.turns: dict[str, dict[str, int]] = {}
      for kind in CONDITION_KINDS:
         key = OWNER_KINDS[kind]
         for e in self.entries.get(key, []):
            if not isinstance(e, dict):
               continue
            texts = e.get("texts", {})
            if not isinstance(texts, dict) or not all(isinstance(v, int) and 0 <= v <= MAX_ID16
                                                      for v in texts.values()):
               raise EffectError(f"structure: {key} {e['name']!r}: texts must map names to text ids")
            if texts:
               self.owner_texts[(kind, e["name"])] = dict(texts)
            if "turns" in e:
               if not isinstance(e["turns"], int) or not 0 <= e["turns"] <= U8_MAX:
                  raise EffectError(f"structure: {key} {e['name']!r}: turns must be 0-{U8_MAX}")
               self.turns.setdefault(key, {})[e["name"]] = e["turns"]
      ## Point name -> range of its value, in id order (the structure's "points": names, or objects with "name" and
      ## "range" [lo, hi]; POINTS when it has none).
      self.points: dict[str, tuple[int, int]] = dict(POINTS)
      if self.ids["points"] is not None:
         self.points = {}
         for entry in self.entries["points"]:
            name = entry if isinstance(entry, str) else entry["name"]
            lo, hi = entry.get("range", (0, U16_MAX)) if isinstance(entry, dict) else (0, U16_MAX)
            self.points[name] = (lo, hi)
      ## Role name -> role ID (an entry's "id", else its index).
      self.roles: dict[str, int] = {}
      ## Roles the record does not store (an entry's "stored": false; E13a): read on demand, never written (C5).
      self.unstored_roles: set[str] = set()
      for i, r in enumerate(self.entries.get("roles", [])):
         name = r if isinstance(r, str) else r["name"]
         self.roles[name] = r.get("id", i) if isinstance(r, dict) else i
         if isinstance(r, dict) and r.get("stored") is False:
            self.unstored_roles.add(name)

   def lookup(self, key: str, name: str) -> int | None:
      """!A name's id in a list.
      @param key   list key
      @param name  the name
      @return the id, or None
      """
      ids = self.ids.get(key)
      return None if ids is None else ids.get(name)


# -- interval analysis (reuses ruleset_compiler.interval) --------------------------------------------------------------

## An inclusive range, or None when unknown.
Range = tuple[int, int] | None


def _shallow(fn: str, ranges: Sequence[tuple[int, int]], where: str, op: str | None = None) -> tuple[int, int]:
   """!The ruleset analysis of one node whose children are known ranges.
   @param fn      function name ("min", ...) or "bin" with op
   @param ranges  the children's ranges
   @param where   location for errors
   @param op      binary operator for "bin", "neg" for negation
   @return the range (outside int32 when it can saturate)
   @throws rc.RulesetError when ruleset analysis rejects it
   """
   names = [f"a{i}" for i in range(len(ranges))]
   env = dict(zip(names, ranges))
   leaves: list[rc.Node] = [("var", n) for n in names]
   if fn == "bin":
      node: rc.Node = ("bin", op, leaves[0], leaves[1])
   elif fn == "neg":
      node = ("neg", leaves[0])
   else:
      node = ("call", fn, leaves)
   return rc.interval(node, env, {}, where, [])


def _corners(f: Callable[[int, int], int], a: tuple[int, int], b: tuple[int, int]) -> tuple[int, int]:
   """!The range of a monotone-per-argument function over two ranges (its corners).
   @param f  the function (without saturation)
   @param a  left range
   @param b  right range
   @return (lo, hi)
   """
   v = [f(x, y) for x in a for y in b]
   return (min(v), max(v))


# -- code generation ---------------------------------------------------------------------------------------------------

class Asm:
   """!Bytes of one program, with labels and the locals it references (patched after the bases are known).

   Attributes are documented where __init__ sets them; the ones below are repeated here because Doxygen's
   Python parser otherwise reports their later uses as undocumented members.

   @var labels
   Label positions (None until placed).
   """

   def __init__(self) -> None:
      """!Creates an empty program."""
      ## Code.
      self.code = bytearray()
      ## Label positions (None until placed).
      self.labels: list[int | None] = []
      ## (position of a 16-bit target, label).
      self.patches: list[tuple[int, int]] = []
      ## Positions of local index bytes and of local subject bytes (their base is added later).
      self.local_refs: list[int] = []
      ## Program indices this program calls.
      self.calls: set[int] = set()

   def label(self) -> int:
      """!A new label.
      @return its number
      """
      self.labels.append(None)
      return len(self.labels) - 1

   def place(self, label: int) -> None:
      """!Places a label here.
      @param label  the label
      @return None
      """
      self.labels[label] = len(self.code)

   def here(self) -> int:
      """!The current offset.
      @return offset
      """
      return len(self.code)

   def emit(self, op: str, *operands: int, target: int | None = None) -> None:
      """!Emits an instruction.
      @param op        opcode name
      @param operands  operand values in OPS order (an "off" operand comes from target)
      @param target    label of an "off" operand (forward), or None when operands hold the offset
      @return None
      """
      self.code.append(OP[op])
      values = list(operands)
      for kind in OPS[OP[op]][1]:
         size = OPERAND_BYTES[kind]
         if kind == "off" and target is not None:
            self.patches.append((len(self.code), target))
            self.code.extend(b"\0\0")
            continue
         v = values.pop(0)
         if kind == "l":
            self.local_refs.append(len(self.code))
         elif kind == "subj" and SUBJ_LOCAL <= (v & SUBJ_BASE_MASK) < SUBJ_LOCAL + EFFECT_LOCALS:
            self.local_refs.append(len(self.code))
         elif kind == "prog":
            self.calls.add(v)
         self.code.extend((v & ((1 << (BYTE_BITS * size)) - 1)).to_bytes(size, "little"))

   def push(self, v: int) -> None:
      """!Pushes a constant with the smallest encoding.
      @param v  the value (saturated to int32)
      @return None
      """
      v = sat(v)
      if INT8_MIN <= v <= INT8_MAX:
         self.emit("Push8", v)
      elif INT16_MIN <= v <= INT16_MAX:
         self.emit("Push16", v)
      else:
         self.emit("Push32", v)

   def finish(self) -> bytes:
      """!Resolves the jump targets.
      @return the code
      """
      for pos, label in self.patches:
         target = self.labels[label]
         assert target is not None, "unplaced label"
         # an oversized program is reported (program-size); its targets are only masked here
         self.code[pos:pos + OPERAND_BYTES["off"]] = (target & U16_MAX).to_bytes(OPERAND_BYTES["off"], "little")
      return bytes(self.code)


def instructions(code: bytes) -> list[tuple[int, str, list[int]]]:
   """!Decodes a program.
   @param code  its bytes
   @return (offset, op name, operand values) per instruction
   """
   out = []
   pc = 0
   while pc < len(code):
      name, kinds, _pops, _pushes = OPS[code[pc]]
      at = pc
      pc += 1
      values = []
      for kind in kinds:
         size = OPERAND_BYTES[kind]
         signed = kind in ("i8", "i16", "i32")
         values.append(int.from_bytes(code[pc:pc + size], "little", signed=signed))
         pc += size
      out.append((at, name, values))
   return out


def prove_stack(code: bytes, callee_stack: Mapping[int, int]) -> int:
   """!The deepest operand stack a program reaches, from its bytecode (every path; join points must agree).
   @param code          the program
   @param callee_stack  program index -> its own deepest stack (subs it calls)
   @return slots
   @throws EffectError on an inconsistent stack (a compiler bug)
   """
   decoded = instructions(code)
   index = {at: i for i, (at, _n, _v) in enumerate(decoded)}
   depth: dict[int, int] = {0: 0}
   todo = [0]
   peak = 0
   while todo:
      at = todo.pop()
      d = depth[at]
      i = index[at]
      _, name, values = decoded[i]
      _n, kinds, pops, pushes = OPS[OP[name]]
      if name == "Pick":
         pops += values[0]
      elif name == "SetTypes":
         pops += values[1]
      if d < pops:
         raise EffectError(f"internal: stack underflow at {at} ({name})")
      after = d - pops + pushes
      peak = max(peak, after, d + callee_stack.get(values[0], 0) if name == "Call" else 0)
      nexts: list[int] = []
      if name not in TERMINAL_OPS and name != "Jmp" and i + 1 < len(decoded):
         nexts.append(decoded[i + 1][0])
      if "off" in kinds:
         nexts.append(values[kinds.index("off")])
      for n in nexts:
         if n in depth:
            if depth[n] != after:
               raise EffectError(f"internal: stack depth {depth[n]} and {after} meet at {n}")
            continue
         depth[n] = after
         todo.append(n)
   return peak


class Seg:
   """!Worst-case events of code between suspensions: through (a path without a yield; None when every path
   yields), first (start to the first yield), inner (between two yields) and last (the last yield to the end)."""

   def __init__(self, through: int | None = 0, first: int | None = None, inner: int | None = None,
                last: int | None = None) -> None:
      """!Creates a segment summary.
      @param through  events of a path with no yield
      @param first    events before the first yield
      @param inner    events between yields
      @param last     events after the last yield
      """
      ## Path without a yield.
      self.through = through
      ## Before the first yield.
      self.first = first
      ## Between two yields.
      self.inner = inner
      ## After the last yield.
      self.last = last

   def worst(self) -> int:
      """!The worst segment.
      @return events
      """
      return max(v for v in (self.through, self.first, self.inner, self.last, 0) if v is not None)


def _mx(*values: int | None) -> int | None:
   """!Maximum ignoring None.
   @param values  values
   @return the maximum, or None when all are None
   """
   present = [v for v in values if v is not None]
   return max(present) if present else None


def _add(a: int | None, b: int | None) -> int | None:
   """!Sum, None when either is None.
   @param a  left
   @param b  right
   @return a + b or None
   """
   return None if a is None or b is None else a + b


def seq(a: Seg, b: Seg) -> Seg:
   """!Code a then code b.
   @param a  first
   @param b  second
   @return the summary
   """
   return Seg(_add(a.through, b.through), _mx(a.first, _add(a.through, b.first)),
              _mx(a.inner, b.inner, _add(a.last, b.first)), _mx(b.last, _add(a.last, b.through)))


def alt(a: Seg, b: Seg) -> Seg:
   """!Code a or code b.
   @param a  one branch
   @param b  the other
   @return the summary
   """
   return Seg(_mx(a.through, b.through), _mx(a.first, b.first), _mx(a.inner, b.inner), _mx(a.last, b.last))


## A suspension point.
YIELD_SEG = Seg(None, 0, None, 0)


class Program:
   """!One compiled program (a handler or a sub)."""

   def __init__(self, index: int, owner: int, label: str, node: Mapping[str, Any], kind: str, hook: str) -> None:
      """!Creates a program entry.
      @param index  program index
      @param owner  owner index
      @param label  description ('status "Freeze" on before_move')
      @param node   the handler or sub node
      @param kind   'handler' or 'sub'
      @param hook   hook or phase name ('' for subs)
      """
      ## Program index.
      self.index = index
      ## Owner index.
      self.owner = owner
      ## Description.
      self.label = label
      ## Handler or sub node.
      self.node = node
      ## 'handler' or 'sub'.
      self.kind = kind
      ## Hook or phase.
      self.hook = hook
      ## Code (after finish()).
      self.code = b""
      ## Assembler (until finished).
      self.asm = Asm()
      ## Locals it uses itself.
      self.own_locals = 0
      ## Base of its locals (subs: above every caller's).
      self.base = 0
      ## Locals in use with its subs.
      self.locals = 0
      ## Deepest stack with its subs.
      self.max_stack = 0
      ## kProgram* flags.
      self.flags = 0
      ## Worst-case events per segment.
      self.events = 0
      ## May suspend (handlers: allowed; subs: does).
      self.yields = False
      ## Debug line entries (pc, file, line, col), in pc order (only while track_lines() is on).
      self.lines: list[tuple[int, str, int, int]] = []


## kProgramYields.
PROGRAM_YIELDS = 1
## kProgramAsks.
PROGRAM_ASKS = 2
## kProgramRng.
PROGRAM_RNG = 4
## kHandlerPoint.
HANDLER_POINT = 1
## kHandlerPerFoe: a switch_in handler that also runs once per foe (`on switch_in per_foe`); a try_switch handler
## that runs for a foe's switch or run only (C5).
HANDLER_PER_FOE = 2
## The hook whose handlers may run per foe (the first of PER_FOE_HOOKS; kept for its readers).
PER_FOE_HOOK = "switch_in"
## The hooks whose handlers may be marked per_foe (C5: try_switch, a foe's trap).
PER_FOE_HOOKS = (PER_FOE_HOOK, "try_switch")
## The word marking a per-foe handler.
PER_FOE_WORD = "per_foe"
## kOwnerAnnounce.
OWNER_ANNOUNCE = 1


class Scope:
   """!Locals of one program: names to slots, block by block.

   Attributes are documented where __init__ sets them; the ones below are repeated here because Doxygen's
   Python parser otherwise reports their later uses as undocumented members.

   @var blocks
   Blocks: name -> (slot, kind, range).
   """

   def __init__(self) -> None:
      """!Creates an empty scope stack."""
      ## Blocks: name -> (slot, kind, range).
      self.blocks: list[dict[str, tuple[int, str, Range]]] = [{}]
      ## Next free slot.
      self.next = 0
      ## Most slots in use.
      self.high = 0

   def find(self, name: str) -> tuple[int, str, Range] | None:
      """!A visible local.
      @param name  its name
      @return (slot, kind, range) or None
      """
      for b in reversed(self.blocks):
         if name in b:
            return b[name]
      return None

   def alloc(self, count: int = 1) -> int:
      """!Allocates consecutive slots.
      @param count  how many
      @return the first
      """
      slot = self.next
      self.next += count
      self.high = max(self.high, self.next)
      return slot

   def open(self) -> int:
      """!Opens a block.
      @return the slot mark to close it with
      """
      self.blocks.append({})
      return self.next

   def close(self, mark: int) -> None:
      """!Closes a block (its slots become free).
      @param mark  open()'s result
      @return None
      """
      self.blocks.pop()
      self.next = mark


class Compiler:
   """!Checks a file AST against a catalog and compiles it to a bank.

   Attributes are documented where __init__ sets them; the ones below are repeated here because Doxygen's
   Python parser otherwise reports their later uses as undocumented members.

   @var points
   Point handler entries.
   @var edges
   Program -> programs it calls.
   @var prog
   The program being generated.
   @var context
   What is being compiled.
   @var where_node
   The statement being compiled (diagnostic positions).
   """

   def __init__(self, ast: Mapping[str, Any], catalog: Catalog, file: str = "",
                positions: Positions | None = None, files: Mapping[int, str] | None = None) -> None:
      """!Creates a compiler.
      @param ast        the file AST (or several files' owners concatenated)
      @param catalog    the structure's names
      @param file       source file (diagnostics)
      @param positions  node positions from parse_positions()
      @param files      id(owner member node) -> its source file, when the owners come from several files
      """
      ## File AST.
      self.ast = ast
      ## Names.
      self.catalog = catalog
      ## Source file.
      self.file = file
      ## Node positions.
      self.positions = positions or {}
      ## Source file of each owner member (handler, sub or point node) by its id.
      self.files: Mapping[int, str] = files or {}
      ## Diagnostics.
      self.diagnostics: list[EffectDiagnostic] = []
      ## Owners: dicts with node, kind, name, id, tags, flags, handlers [(hook, flags, program)].
      self.owners: list[dict[str, Any]] = []
      ## Programs.
      self.programs: list[Program] = []
      ## Point handlers: dicts with owner, point, node, label, ir.
      self.points: list[dict[str, Any]] = []
      ## (owner index, sub name) -> program index.
      self.subs: dict[tuple[int, str], int] = {}
      ## Cost memo: (program, depth) -> Seg.
      self.cost_memo: dict[tuple[int, int], Seg] = {}
      ## Hook dispatch cost memo: (hook, depth) -> events.
      self.hook_memo: dict[tuple[str, int], int] = {}
      ## Program -> programs it calls.
      self.edges: dict[int, list[int]] = {}
      ## Programs on a call cycle.
      self.cyclic: set[int] = set()
      ## Warnings already given: (code, node id).
      self.warned: set[tuple[str, int]] = set()
      ## The program being generated.
      self.prog: Program | None = None
      ## Its locals.
      self.scope = Scope()
      ## What is being compiled: label, owner kind, hook, hook spec, may_yield, kind, repeat depth, point.
      self.context: dict[str, Any] = {}
      ## The statement being compiled (diagnostic positions).
      self.where_node: Any = None

   def program_list(self) -> list[Program]:
      """!The programs (a method, so loops over them do not read as members to Doxygen).
      @return programs
      """
      return self.programs

   def owner_list(self) -> list[dict[str, Any]]:
      """!The owners.
      @return owners
      """
      return self.owners

   # -- diagnostics --

   def report(self, severity: str, code: str, node: Any = None, **fields: Any) -> None:
      """!Adds a diagnostic.
      @param severity  'error' or 'warning'
      @param code      MESSAGES key
      @param node      node it is about (for the position), or None
      @param fields    the message's fields
      @return None
      """
      line, col = self.positions.get(id(node), (0, 0)) if node is not None else (0, 0)
      if line == 0 and self.where_node is not None:
         line, col = self.positions.get(id(self.where_node), (0, 0))
      prefix = f"{self.context['label']}: " if self.context.get("label") and code not in (
         "too-many-locals", "stack-depth", "hook-budget", "step-budget", "program-size") else ""
      file = (self.files.get(id(node), "") if node is not None else "") or self.context.get("file") or self.file
      self.diagnostics.append(EffectDiagnostic(code, severity, prefix + MESSAGES[code].format(**fields), file,
                                               line, col))

   def err(self, code: str, node: Any = None, **fields: Any) -> None:
      """!Adds an error.
      @param code    MESSAGES key
      @param node    node it is about
      @param fields  the message's fields
      @return None
      """
      self.report("error", code, node, **fields)

   def warn(self, code: str, node: Any = None, **fields: Any) -> None:
      """!Adds a warning (once per node).
      @param code    MESSAGES key
      @param node    node it is about
      @param fields  the message's fields
      @return None
      """
      key = (code, id(node))
      if key in self.warned:
         return
      self.warned.add(key)
      self.report("warning", code, node, **fields)

   def errors(self) -> list[EffectDiagnostic]:
      """!The errors so far.
      @return errors
      """
      return [d for d in self.diagnostics if d.severity == "error"]

   # -- names --

   def lookup_id(self, key: str, name: str, node: Any, what: str, limit: int = MAX_ID8) -> int:
      """!A name's id in a structure list (diagnosed when unknown).
      @param key    list key
      @param name   the name
      @param node   node (position)
      @param what   what it is, for the message
      @param limit  largest id the immediate holds
      @return the id (0 when unknown)
      """
      if self.catalog.ids.get(key) is None:
         self.err("no-list", node, list=key, name=name)
         return 0
      i = self.catalog.lookup(key, name)
      if i is None:
         self.err("unknown-id", node, what=what, name=name)
         return 0
      if i > limit:
         self.err("id-range", node, what=what, name=name, id=i, limit=limit)
         return 0
      return i

   def text_id(self, name: str, node: Any) -> int:
      """!The id of a text an owner shows: one of its own condition texts (Catalog.owner_texts), else a structure
      text.
      @param name  the text's name
      @param node  node (position)
      @return the id
      """
      own = self.catalog.owner_texts.get((self.context.get("owner_kind", ""), self.context.get("owner_name", "")))
      if own is not None and name in own:
         return own[name]
      return self.lookup_id("texts", name, node, "text", MAX_ID16)

   def default_turns(self, s: Mapping[str, Any]) -> int:
      """!The duration a condition statement without turns gives its condition (Catalog.turns; 0 = its own, decided
      by its own logic, or until it is ended).
      @param s  a vol_on / side_on / field_on / weather node
      @return turns
      """
      key, field = {"vol_on": ("volatiles", "volatile"), "side_on": ("side_conditions", "condition"),
                    "field_on": ("field_conditions", "condition"), "weather": ("weathers", "weather")}[s["stmt"]]
      return self.catalog.turns.get(key, {}).get(s.get(field, ""), 0)

   # -- the whole compile --

   def run(self) -> Compiler:
      """!Checks and compiles everything.
      @return self
      """
      owners = [o for o in self.ast.get("owners", []) if "owner" in o]
      seen: set[tuple[str, str]] = set()
      for o in owners:
         self.declare_owner(o, seen)
      self.check_calls()
      for p in self.program_list():
         self.generate(p)
      for pt in self.points:
         self.generate_point(pt)
      self.context = {}
      if not self.errors():
         self.link()
      return self

   def declare_owner(self, o: Mapping[str, Any], seen: set[tuple[str, str]]) -> None:
      """!Registers an owner, its programs, subs and points.
      @param o     the owner node
      @param seen  (kind, name) pairs already declared
      @return None
      """
      kind = o["owner"]["kind"]
      name = o["owner"]["name"]
      oi = len(self.owners)
      self.context = {"label": f"{kind} {json.dumps(name)}"}
      if (kind, name) in seen:
         self.err("duplicate-owner", o, kind=kind, name=name)
      seen.add((kind, name))
      key = OWNER_KINDS.get(kind)
      if key is None and kind != "global":
         self.err("syntax", o, text=f"unknown owner kind {kind!r}")
         key = None
      ident = sum(1 for x in self.owners if x["kind"] == "global") if kind == "global" else 0
      if key is not None:
         if self.catalog.ids.get(key) is None or self.catalog.lookup(key, name) is None:
            self.err("unknown-owner", o, kind=kind, name=name, list=key)
         else:
            ident = self.catalog.lookup(key, name)
      tags = 0
      for t in o.get("tags", []):
         tags |= 1 << self.lookup_id("tags", t, o, "tag")
      if len(o.get("tags", [])) > MAX_TAGS:
         self.err("too-many-tags", o, limit=MAX_TAGS)
      entry = {"node": o, "kind": kind, "name": name, "id": ident, "tags": tags & U16_MAX,
               "flags": OWNER_ANNOUNCE if o.get("announce") else 0, "handlers": []}
      self.owners.append(entry)
      members = [h for h in o.get("handlers", []) if h.get("stmt") != "comment"]
      for h in members:
         if "sub" in h:
            if (oi, h["sub"]) in self.subs:
               self.err("duplicate-sub", h, name=h["sub"])
               continue
            p = Program(len(self.programs), oi, f"{kind} {json.dumps(name)} sub {h['sub']}", h, "sub", "")
            self.subs[(oi, h["sub"])] = p.index
            self.programs.append(p)
      for h in members:
         if "sub" in h:
            continue
         if "point" in h:
            point_ids = list(self.catalog.points)
            if h["point"] not in point_ids:
               self.err("unknown-point", h, name=h["point"])
               continue
            label = f"{kind} {json.dumps(name)} point {h['point']}"
            entry["handlers"].append((point_ids.index(h["point"]), HANDLER_POINT, len(self.points)))
            self.points.append({"owner": oi, "point": h["point"], "node": h, "label": label})
            continue
         hook = h.get("on", h.get("phase"))
         if "phase" in h and kind != "move":
            self.err("phase-owner", h, phase=hook)
            continue
         if "on" in h:
            if hook not in HOOK:
               self.err("unknown-hook", h, name=hook)
               continue
            if kind == "move":
               self.err("move-hook", h, phases=", ".join(MOVE_PHASES), hook=hook)
               continue
            if kind not in HOOKS[HOOK[hook]]["owners"]:
               self.err("hook-owner", h, kind=kind, hook=hook)
               continue
            if h.get(PER_FOE_WORD) and hook not in PER_FOE_HOOKS:
               self.err("per-foe", h, hook=hook)
               continue
         p = Program(len(self.programs), oi, f"{kind} {json.dumps(name)} {'on ' if 'on' in h else ''}{hook}", h,
                     "handler", hook)
         p.yields = (kind == "move" and hook in YIELD_PHASES) or (kind in CONDITION_KINDS and hook in YIELD_HOOKS)
         entry["handlers"].append((HOOK[hook], HANDLER_PER_FOE if h.get(PER_FOE_WORD) else 0, p.index))
         self.programs.append(p)

   # -- the call graph --

   def calls_of(self, body: Iterable[Mapping[str, Any]]) -> list[Mapping[str, Any]]:
      """!Every call statement in a body (nested blocks included).
      @param body  statements
      @return the call nodes
      """
      out: list[Mapping[str, Any]] = []
      for s in body:
         k = s.get("stmt")
         if k == "call":
            out.append(s)
         for key in ("then", "else", "body"):
            if isinstance(s.get(key), list):
               out.extend(self.calls_of(s[key]))
      return out

   def check_calls(self) -> None:
      """!Resolves sub calls, rejects recursion and checks the call depth and which subs suspend.
      @return None
      """
      edges: dict[int, list[int]] = {}
      for p in self.program_list():
         self.context = {"label": p.label}
         edges[p.index] = []
         for c in self.calls_of(p.node["body"]):
            target = self.subs.get((p.owner, c["sub"]))
            if target is None:
               self.err("unknown-sub", c, name=c["sub"])
               continue
            edges[p.index].append(target)
      # program index -> VISITING / VISITED, and the call depth below it
      state: dict[int, int] = {}
      depth: dict[int, int] = {}

      def visit(i: int, path: list[int]) -> int:
         """!Depth-first walk: call depth below a program, cycles reported.
         @param i     program index
         @param path  programs on the current path
         @return calls nested below it
         """
         if state.get(i) == VISITING:
            cycle = path[path.index(i):] + [i]
            names = [self.programs[x].node.get("sub", self.programs[x].hook) for x in cycle]
            self.context = {"label": self.programs[i].label}
            self.err("recursion", self.programs[i].node, cycle=" -> ".join(names))
            self.cyclic.update(cycle)
            return 0
         if state.get(i) == VISITED:
            return depth[i]
         state[i] = VISITING
         d = 0
         for t in edges[i]:
            d = max(d, 1 + visit(t, path + [i]))
         state[i] = VISITED
         depth[i] = d
         return d

      for p in self.program_list():
         d = visit(p.index, [])
         if p.kind == "handler" and d > EFFECT_CALL_DEPTH:
            self.context = {"label": p.label}
            self.err("call-depth", p.node, depth=d, limit=EFFECT_CALL_DEPTH)
      self.edges = edges
      # A sub suspends when its body (or a sub it calls) yields or asks.
      memo: dict[int, bool] = {}

      def suspends(i: int) -> bool:
         """!Whether a program can suspend (subs it calls included).
         @param i  program index
         @return true when it can
         """
         if i in memo:
            return memo[i]
         memo[i] = False
         memo[i] = self.body_suspends(self.programs[i].node["body"]) or any(suspends(t) for t in edges[i])
         return memo[i]

      for p in self.program_list():
         if p.kind == "sub":
            p.yields = suspends(p.index)

   def body_suspends(self, body: Iterable[Mapping[str, Any]]) -> bool:
      """!Whether statements yield or ask themselves (not through calls).
      @param body  statements
      @return true when they can suspend
      """
      for s in body:
         k = s.get("stmt")
         if k in ("yield", "ask") or (k == "switch" and s.get("mode") in ("ask", "pass")):
            return True
         if k == "repeat" and any(x.get("stmt") == "damage" for x in self.flat(s["body"])):
            return True
         for key in ("then", "else", "body"):
            if isinstance(s.get(key), list) and self.body_suspends(s[key]):
               return True
      return False

   def flat(self, body: Iterable[Mapping[str, Any]]) -> list[Mapping[str, Any]]:
      """!Every statement of a body, nested ones included.
      @param body  statements
      @return the statements
      """
      out: list[Mapping[str, Any]] = []
      for s in body:
         out.append(s)
         for key in ("then", "else", "body"):
            if isinstance(s.get(key), list):
               out.extend(self.flat(s[key]))
      return out

   # -- programs --

   def generate(self, p: Program) -> None:
      """!Generates one program's code (locals relative to its base).
      @param p  the program
      @return None
      """
      owner = self.owners[p.owner]
      self.prog = p
      self.scope = Scope()
      hook = p.hook
      spec = HOOKS[HOOK[hook]] if hook in HOOK else None
      self.context = {"label": p.label, "owner_kind": owner["kind"], "owner_name": owner["name"], "hook": hook,
                      "spec": spec, "may_yield": p.yields, "kind": p.kind, "repeat": 0, "point": False,
                      "pure": bool(spec and spec.get("pure")),
                      "file": self.files.get(id(p.node), "")}
      a = p.asm
      end = a.label()
      self.mark_line(p.node)
      if "when" in p.node:
         self.where_node = p.node
         self.jump_false(p.node["when"], end)
      self.body(p.node["body"])
      a.place(end)
      self.mark_line(p.node)
      a.emit("Ret" if p.kind == "sub" else "End")
      p.own_locals = self.scope.high
      p.code = a.finish()
      if len(p.code) > MAX_PROGRAM_BYTES:
         self.err("program-size", p.node, label=p.label, count=len(p.code), limit=MAX_PROGRAM_BYTES)
      self.where_node = None

   def body(self, stmts: Sequence[Mapping[str, Any]]) -> bool:
      """!Generates a block (its own scope).
      @param stmts  statements
      @return true when the block ends the program on every path
      """
      mark = self.scope.open()
      ended = False
      warned = False
      for s in stmts:
         if s.get("stmt") == "comment":
            continue
         if ended and not warned:
            self.warn("unreachable", s)
            warned = True
         self.where_node = s
         self.mark_line(s)
         if self.statement(s):
            ended = True
      self.scope.close(mark)
      return ended

   def mark_line(self, node: Any) -> None:
      """!Debug line table: the code generated from here on comes from a node's source line (only while
      track_lines() is on and the node was parsed then; a repeated line is kept once).
      @param node  the statement or handler node
      @return None
      """
      src = NODE_SOURCES.get(id(node)) if LINE_TRACKING[0] else None
      if src is None or src[0] is not node:
         return
      _node, file, line, col = src
      entry = (self.prog.asm.here(), file, line, col)
      lines = self.prog.lines
      if lines and lines[-1][0] == entry[0]:
         lines[-1] = entry   # nothing was generated for the previous one
      elif not lines or lines[-1][1:] != entry[1:]:
         lines.append(entry)

   def declare(self, name: str, node: Any, kind: str, rng: Range, slots: int = 1) -> int:
      """!Declares a local.
      @param name   its name
      @param node   the declaring node
      @param kind   value kind
      @param rng    value range (None: unknown)
      @param slots  slots it takes
      @return its slot
      """
      if name in CONTEXT_WORDS or name in STATEMENT_WORDS or name in PRIMITIVES or name in FUNCTIONS or \
            name == NONE_WORD:
         self.err("reserved", node, name=name)
      elif self.scope.find(name) is not None:
         self.err("redeclared", node, name=name)
      slot = self.scope.alloc(slots)
      self.scope.blocks[-1][name] = (slot, kind, rng)
      return slot

   def statement(self, s: Mapping[str, Any]) -> bool:
      """!Generates one statement.
      @param s  the node
      @return true when it ends the program on every path
      """
      a = self.prog.asm
      k = s["stmt"]
      ctx = self.context
      if ctx.get("pure") and k in ("assign", "call"):
         # G1: a pure hook (ai_score) changes nothing (a sub may: it is not called from one)
         self.err("pure-hook", s, hook=ctx["hook"], what="writing a ref" if k == "assign" else "calling a sub")
         return False
      if k == "let":
         kind = self.value(s["value"])
         rng = self.range_of(s["value"])
         slot = self.declare(s["name"], s, kind, rng)
         a.emit("Store", slot)
         return False
      if k == "assign":
         self.assign(s)
         return False
      if k == "if":
         other = a.label()
         end = a.label()
         self.jump_false(s["cond"], other)
         then_ends = self.body(s["then"])
         if "else" in s:
            a.emit("Jmp", target=end)
            a.place(other)
            else_ends = self.body(s["else"])
            a.place(end)
            return then_ends and else_ends
         a.place(other)
         a.place(end)
         return False
      if k == "chance":
         self.chance_odds(s["n"], s["m"])
         other = a.label()
         end = a.label()
         a.emit("Jz", target=other)
         then_ends = self.body(s["body"])
         if "else" in s:
            a.emit("Jmp", target=end)
         a.place(other)
         else_ends = self.body(s["else"]) if "else" in s else False
         a.place(end)
         return then_ends and else_ends
      if k == "repeat":
         bound = s["max"]
         if not isinstance(bound, int) or not 1 <= bound <= MAX_REPEAT:
            self.err("repeat-bound", s, limit=MAX_REPEAT, value=bound)
         self.value(s["count"])
         a.push(0)
         a.push(bound if isinstance(bound, int) else 1)
         a.emit("Clamp")
         a.emit("Dup")
         mark = self.scope.open()
         counter = self.scope.alloc()
         a.emit("Store", counter)
         end = a.label()
         a.emit("Jz", target=end)
         top = a.here()
         ctx["repeat"] += 1
         self.body(s["body"])
         ctx["repeat"] -= 1
         a.emit("Loop", counter, top)
         a.place(end)
         self.scope.close(mark)
         return False
      if k == "for":
         if s["set"] not in BATTLER_SETS:
            self.err("unknown-id", s, what="battler set", name=s["set"])
         subj = self.subject(s["of"], s)
         mark = self.scope.open()
         var = self.declare(s["var"], s, "battler", (0, MAX_BATTLERS - 1), slots=FOREACH_SLOTS)
         end = a.label()
         a.emit("ForEach", BATTLER_SETS.index(s["set"]) if s["set"] in BATTLER_SETS else 0, subj, var, target=end)
         top = a.here()
         self.body(s["body"])
         a.emit("Next", var, top)
         a.place(end)
         self.scope.close(mark)
         return False
      if k == "call":
         target = self.subs.get((self.prog.owner, s["sub"]))
         if target is not None:
            if self.programs[target].yields and not ctx["may_yield"]:
               self.err("yield-context", s, what=f"sub {s['sub']} suspends; it can be called")
            a.emit("Call", target)
         return False
      if k == "yield":
         if not ctx["may_yield"]:
            self.err("yield-context", s, what="yield")
         a.emit("Yield")
         self.prog.flags |= PROGRAM_YIELDS
         return False
      if k in ("stop", "fail"):
         a.emit("Stop" if k == "stop" else "Fail")
         return True
      if k == "return":
         a.emit("Ret" if ctx["kind"] == "sub" else "End")
         return True
      if k == "ask":
         if not ctx["may_yield"]:
            self.err("yield-context", s, what="ask")
         if s["kind"] not in ASK_KINDS:
            self.err("unknown-id", s, what="request kind", name=s["kind"])
         subj = self.subject(s["who"], s)
         a.emit("Ask", ASK_KINDS.index(s["kind"]) if s["kind"] in ASK_KINDS else 0, subj)
         slot = self.declare(s["into"], s, "int", None)
         a.emit("Store", slot)
         self.prog.flags |= PROGRAM_ASKS | PROGRAM_YIELDS
         return False
      if k in PRIMITIVES:
         return self.primitive(s)
      self.err("syntax", s, text=f"unknown statement {k!r}")
      return False

   def assign(self, s: Mapping[str, Any]) -> None:
      """!Generates an assignment (=, += or -=).
      @param s  the assign node
      @return None
      """
      a = self.prog.asm
      t = s["target"]
      op = s["op"]
      if "name" in t:
         found = self.scope.find(t["name"])
         if found is None:
            self.err("undeclared", s, name=t["name"])
            return
         slot, kind, _rng = found
         if op != "=":
            a.emit("Load", slot)
         self.value(s["value"], kind)
         if op != "=":
            a.emit("Add" if op == "+=" else "Sub")
         a.emit("Store", slot)
         # the range is no longer the initializer's
         for b in reversed(self.scope.blocks):
            if t["name"] in b:
               b[t["name"]] = (slot, kind, None)
               break
         return
      resolved = self.resolve_ref(t)
      if resolved is None:
         return
      form, spec, subj, arg = resolved
      if not spec.writable and form != "role":
         self.err("not-writable", s, ref=t["ref"])
         return
      if form == "role" and t.get("arg") in self.catalog.unstored_roles:
         self.err("unstored-role", s, name=t["arg"])   # C5: the engine's role_set would drop it silently
         return
      # C8: in `on encounter` no creature is picked yet: target is the holder's own record (the field context has
      # no other), so a write through it would change the lead; encounter_made's target is the creature
      if self.context.get("hook") == ENCOUNTER_TARGETLESS_HOOK and subj == BATTLER_SUBJECTS.index(TARGET_SUBJECT):
         self.err("encounter-target", s, ref=t.get("ref", TARGET_SUBJECT))
         return
      if op != "=":
         self.emit_read(form, spec, subj, arg)
      self.value(s["value"])
      if op != "=":
         a.emit("Add" if op == "+=" else "Sub")
      if form == "role":
         a.emit("RoleSet", subj, arg[0], arg[1])
      else:
         a.emit("Set", REFS.index(spec.ref), subj, arg)

   # -- subjects and refs --

   def subject(self, name: str, node: Any) -> int:
      """!A subject code.
      @param name  holder / other / user / target or a local holding a battler
      @param node  node (position)
      @return subj8
      """
      if name in BATTLER_SUBJECTS:
         return BATTLER_SUBJECTS.index(name)
      if name.startswith(INPUT_SUBJECT + "."):
         return self.input_subject(name, node)
      found = self.scope.find(name)
      if found is not None:
         if self.context.get("point"):
            self.err("point-subject", node, name=name)
         return SUBJ_LOCAL + found[0]
      self.err("unknown-subject", node, name=name)
      return SUBJ_HOLDER

   def input_subject(self, name: str, node: Any) -> int:
      """!The subject code of a combination's input (B1): `input.N` (N below MAX_COMBINE_INPUTS), `input.ROLE` (a
      slot role the methods give one index) or (C9) `input.METHOD.ROLE` (that method's slot role: unambiguous when
      methods share a role name), only in a combine handler.
      @param name  "input.N", "input.ROLE" or "input.METHOD.ROLE"
      @param node  node (position)
      @return SUBJ_BATTLER + the input's battler
      """
      if self.context.get("hook") != COMBINE_HOOK:
         self.err("combine-context", node, what=name)
      which = name[len(INPUT_SUBJECT) + 1:]
      slots = {e["name"]: e.get("input") for e in self.catalog.entries.get("combine_slots", [])
               if isinstance(e, dict)}
      index = int(which) if which.isdigit() else slots.get(which)
      if index is None or not 0 <= index < MAX_COMBINE_INPUTS:
         self.err("unknown-input", node, name=name, limit=MAX_COMBINE_INPUTS,
                  roles=", ".join(sorted(n for n, i in slots.items() if i is not None)) or "none")
         return SUBJ_BATTLER + FIRST_INPUT_BATTLER
      return SUBJ_BATTLER + FIRST_INPUT_BATTLER + index

   def ref_head(self, parts: Sequence[str]) -> tuple[str, list[str]]:
      """!A ref's base and fields (B1: `input.X` is one base; C9: `input.METHOD.ROLE` too, when the catalog names
      that method's role: it wins over a role X's field ROLE).
      @param parts  the ref's dotted parts
      @return (base, field parts)
      """
      head, *rest = parts
      if head == INPUT_SUBJECT and rest:
         which, *fields = rest
         if fields and f"{which}.{fields[0]}" in self.qualified_inputs():
            return f"{head}.{which}.{fields[0]}", fields[1:]
         return f"{head}.{which}", fields
      return head, rest

   def qualified_inputs(self) -> set[str]:
      """!The method-qualified slot roles the catalog names (C9: METHOD.ROLE).
      @return the names
      """
      entries = self.catalog.entries.get("combine_slots", [])
      return {e["name"] for e in entries if isinstance(e, dict) and "." in str(e.get("name"))}

   def resolve_ref(self, e: Mapping[str, Any]) -> tuple[str, RefSpec, int, Any] | None:
      """!Resolves a ref node.
      @param e  the ref node
      @return (form 'get' / 'has' / 'role', spec, subj8, arg) or None when it is unknown (diagnosed)
      """
      ref = e["ref"]
      head, rest = self.ref_head(ref.split("."))
      parts = [head] + rest
      if len(parts) == 1:
         if ref not in GLOBAL_REFS:
            self.err("unknown-ref", e, ref=ref)
            return None
         if self.context.get("point") and ref in ("dealt", "owner"):
            self.err("pure", e, what=f"reading {ref}")
         return self.ref_arg("get", GLOBAL_REFS[ref], SUBJ_NONE, e)
      field = ".".join(parts[1:])
      if head == "move":
         table, subj = MOVE_REFS, SUBJ_MOVE
      elif head == "field":
         table, subj = FIELD_REFS, SUBJ_FIELD
         if field == STEP_EFFECT_REF:
            # F3: the steps hook's value under its own name (a plain context read, no field subject)
            subj = SUBJ_NONE
            if self.context.get("hook") != STEPS_HOOK:
               self.err("steps-context", e)
         elif field in (COMBINE_METHOD_REF, COMBINE_INPUTS_REF):
            # B1: the combine hook's value (the method) and its input count (the context's dealt register)
            subj = SUBJ_NONE
            if self.context.get("hook") != COMBINE_HOOK:
               self.err("combine-context", e, what=f"field.{field}")
      elif head == "side":
         table = SIDE_REFS
         if "of" not in e:
            self.err("ref-arg", e, ref=ref, need="needs side(subject)")
            return None
         subj = self.subject(e["of"], e) | SUBJ_SIDE
      else:
         table = BATTLER_REFS
         subj = self.subject(head, e)
      if field not in table:
         self.err("unknown-ref", e, ref=ref)
         return None
      spec = table[field]
      if spec.has is not None:
         return self.has_ref(spec, subj, e)
      if field == "role":
         if not isinstance(e.get("arg"), str):
            self.err("ref-arg", e, ref=ref, need="needs a role name")
            return None
         if e["arg"] not in self.catalog.roles:
            self.err("unknown-id", e, what="role", name=e["arg"])
            return None
         return ("role", spec, subj, (self.catalog.roles[e["arg"]] & BYTE_MASK, e.get("index", 0)))
      return self.ref_arg("get", spec, subj, e)

   def ref_arg(self, form: str, spec: RefSpec, subj: int, e: Mapping[str, Any]) -> tuple[str, RefSpec, int, Any]:
      """!Resolves a get ref's argument.
      @param form  'get'
      @param spec  the ref spec
      @param subj  subj8
      @param e     the node
      @return (form, spec, subj, arg byte)
      """
      want = spec.arg
      arg = e.get("arg")
      if "index" in e:
         self.err("ref-arg", e, ref=e["ref"], need="takes one argument")
      if want is None:
         if arg is not None:
            self.err("ref-arg", e, ref=e["ref"], need="takes no argument")
         return (form, spec, subj, 0)
      if arg is None:
         self.err("ref-arg", e, ref=e["ref"], need=f"needs an argument ({want})")
         return (form, spec, subj, 0)
      if want == "index":
         if not isinstance(arg, int) or not 0 <= arg < MAX_ACTION_ARGS:
            self.err("ref-arg", e, ref=e["ref"], need=f"takes a slot number 0-{MAX_ACTION_ARGS - 1}")
            return (form, spec, subj, 0)
         return (form, spec, subj, arg)
      if not isinstance(arg, str):
         self.err("ref-arg", e, ref=e["ref"], need=f"takes a name ({want})")
         return (form, spec, subj, 0)
      if want in ("counters", "words"):
         slots = self.catalog.counters if want == "counters" else self.catalog.words
         if arg not in slots:
            self.lookup_id("volatiles", arg, e, "volatile")
            if self.catalog.lookup("volatiles", arg) is not None:
               self.err("ref-arg", e, ref=e["ref"],
                        need=f"needs a volatile with a {want[:-1]} ({arg!r} has none)")
            return (form, spec, subj, 0)
         return (form, spec, subj, slots[arg])
      return (form, spec, subj, self.lookup_id(want, arg, e, want.rstrip("s").replace("_", " ")))

   def has_ref(self, spec: RefSpec, subj: int, e: Mapping[str, Any]) -> tuple[str, RefSpec, int, Any] | None:
      """!Resolves a has(...) / has_type(...) ref.
      @param spec  the ref spec
      @param subj  subj8
      @param e     the node
      @return ('has', spec, subj, (kind index, id))
      """
      arg = e.get("arg")
      if not isinstance(arg, str):
         self.err("ref-arg", e, ref=e["ref"], need="needs a name")
         return None
      kind = spec.has
      if kind == "?":
         for k, key in HAS_LOOKUP:
            if self.catalog.lookup(key, arg) is not None:
               return ("has", spec, subj, (HAS_KINDS.index(k), self.lookup_id(key, arg, e, k)))
         self.err("unknown-id", e, what="volatile, tag or status", name=arg)
         return None
      key = spec.arg
      return ("has", spec, subj, (HAS_KINDS.index(kind), self.lookup_id(key, arg, e, kind.replace("_", " "))))

   def emit_read(self, form: str, spec: RefSpec, subj: int, arg: Any) -> None:
      """!Emits a resolved read.
      @param form  'get', 'has' or 'role'
      @param spec  the ref spec
      @param subj  subj8
      @param arg   arg byte, (kind, id) or (role, element)
      @return None
      """
      a = self.prog.asm
      if form == "has":
         a.emit("Has", arg[0], subj, arg[1])
      elif form == "role":
         a.emit("RoleGet", subj, arg[0], arg[1])
      else:
         a.emit("Get", REFS.index(spec.ref), subj, arg)

   # -- expressions --

   def static_kind(self, e: Any) -> str | None:
      """!The kind of an expression's value, when it is evident (for name resolution).
      @param e  AST expression
      @return a kind, or None
      """
      if isinstance(e, int):
         return "int"
      if not isinstance(e, dict):
         return None
      if "subject" in e:
         return "battler"
      if "name" in e:
         found = self.scope.find(e["name"])
         return found[1] if found else None
      if "ref" in e:
         head, rest = self.ref_head(e["ref"].split("."))
         parts = [head] + rest
         field = ".".join(parts[1:])
         table = {"move": MOVE_REFS, "field": FIELD_REFS, "side": SIDE_REFS}.get(parts[0], BATTLER_REFS)
         if len(parts) == 1:
            return GLOBAL_REFS[e["ref"]].kind if e["ref"] in GLOBAL_REFS else None
         return table[field].kind if field in table else None
      if "call" in e and e["call"] in ("min", "max", "clamp") and e["args"]:
         return self.static_kind(e["args"][0])
      return None

   def name_value(self, e: Mapping[str, Any], expect: str | None) -> tuple[str, int | None, Range]:
      """!Resolves a bare name: a local, or a name of the expected kind (or the one kind that has it).
      @param e       the name node
      @param expect  the kind the context expects, or None
      @return (kind, constant id or None for a local, range)
      """
      name = e["name"]
      found = self.scope.find(name)
      if found is not None:
         _slot, kind, rng = found
         return (kind, None, rng)
      if name == NONE_WORD:
         # C5: the none value of the kind it is compared with or given as
         if expect not in NONE_VALUES:
            self.err("none-kind", e, kinds=", ".join(NONE_VALUES))
            return ("int", 0, (0, 0))
         none = NONE_VALUES[expect]
         return (expect, none, (none, none))
      if expect in VALUE_KINDS:
         i = self.lookup_id(VALUE_KINDS[expect], name, e, expect, MAX_ID16)
         return (expect, i, (i, i))
      matches = [(k, self.catalog.lookup(key, name)) for k, key in VALUE_KINDS.items()
                 if self.catalog.lookup(key, name) is not None]
      if len(matches) == 1:
         k, i = matches[0]
         return (k, i, (i, i))
      if matches:
         self.err("ambiguous-name", e, name=name, kinds=", ".join(k for k, _i in matches))
      else:
         self.err("unknown-name", e, name=name)
      return ("int", 0, (0, 0))

   def value(self, e: Any, expect: str | None = None) -> str:
      """!Generates code pushing an expression's value.
      @param e       AST expression
      @param expect  kind expected for bare names
      @return the value's kind
      """
      a = self.prog.asm
      if isinstance(e, bool) or not isinstance(e, (int, dict)):
         self.err("syntax", None, text=f"not an expression: {e!r}")
         return "int"
      if isinstance(e, int):
         a.push(e)
         return "int"
      if "dec" in e:
         a.push(decimal_q8(e["dec"]))
         return "int"
      if "name" in e:
         kind, ident, _rng = self.name_value(e, expect)
         if ident is None:
            a.emit("Load", self.scope.find(e["name"])[0])
         else:
            a.push(ident)
         return kind
      if "subject" in e:
         a.emit("Subj", self.subject(e["subject"], e))
         return "battler"
      if "ref" in e:
         resolved = self.resolve_ref(e)
         if resolved is None:
            a.push(0)
            return "int"
         form, spec, subj, arg = resolved
         self.emit_read(form, spec, subj, arg)
         return spec.kind
      if "call" in e:
         return self.call(e, expect)
      op = e.get("op")
      args = e.get("args", [])
      if op in ("and", "or"):
         false = a.label()
         end = a.label()
         self.jump_false(e, false)
         a.push(1)
         a.emit("Jmp", target=end)
         a.place(false)
         a.push(0)
         a.place(end)
         return "bool"
      if op == "not":
         self.value(args[0])
         a.emit("Not")
         return "bool"
      if op == "neg":
         self.value(args[0])
         a.emit("Neg")
         return "int"
      if op in BINARY_OPS:
         left, right = args
         self.value(left, self.static_kind(right))
         self.value(right, self.static_kind(left))
         a.emit(BINARY_OPS[op])
         self.range_of(e)   # saturation warnings
         return "bool" if op in COMPARISONS else "int"
      self.err("syntax", e, text=f"unknown operator {op!r}")
      return "int"

   def battler_subject(self, e: Any, node: Any) -> int:
      """!A subject code for a battler-valued expression (a temp local unless it is a subject or local).
      @param e     AST expression
      @param node  node (position)
      @return subj8
      """
      if isinstance(e, dict) and "subject" in e:
         return self.subject(e["subject"], node)
      if isinstance(e, dict) and "name" in e and self.scope.find(e["name"]) is not None:
         return SUBJ_LOCAL + self.scope.find(e["name"])[0]
      self.value(e)
      slot = self.scope.alloc()
      self.prog.asm.emit("Store", slot)
      return SUBJ_LOCAL + slot

   def call(self, e: Mapping[str, Any], expect: str | None) -> str:
      """!Generates a function call.
      @param e       the call node
      @param expect  kind expected for bare names
      @return the value's kind
      """
      a = self.prog.asm
      fn = e["call"]
      args = e["args"]
      if fn not in FUNCTIONS:
         self.err("unknown-function", e, name=fn)
         a.push(0)
         return "int"
      lo, hi = FUNCTIONS[fn]
      if not lo <= len(args) <= hi:
         self.err("arity", e, name=fn, want=str(lo) if lo == hi else f"{lo}-{hi}", got=len(args))
         a.push(0)
         return "int"
      if fn in RNG_FUNCTIONS:
         if self.context.get("point"):
            self.err("pure", e, what=f"{fn}() (it draws)")
         elif self.context.get("pure"):
            self.err("pure-hook", e, hook=self.context["hook"], what=f"{fn}() (it draws)")
         self.prog.flags |= PROGRAM_RNG
      if fn == "odds":
         # odds(n, m[, by[, on]]): the ruleset's effect_odds pipeline for battler by against on (E9)
         for which, operand in zip(ODDS_OPERANDS, args):
            r = self.range_of(operand)
            if r is not None and r[1] > ODDS_OPERAND_MAX:
               self.err("odds-range", operand, which=which, hi=r[1], limit=ODDS_OPERAND_MAX)
         self.value(args[0])
         self.value(args[1])
         for i, default in enumerate(ODDS_DEFAULT_SUBJECTS):
            arg = ODDS_FIRST_BATTLER + i
            if arg < len(args):
               self.value(args[arg], "battler")
            else:
               a.emit("Subj", self.subject(default, e))
         a.emit("Odds")
         self.range_of(e)
         return "bool"
      if fn in ("is_foe", "is_ally"):
         first = self.battler_subject(args[0], e)
         second = self.battler_subject(args[1], e)
         side = REFS.index("side")
         a.emit("Get", side, first, 0)
         a.emit("Get", side, second, 0)
         a.emit("Ne" if fn == "is_foe" else "Eq")
         return "bool"
      kind = "int"
      for i, x in enumerate(args):
         # min / max / clamp keep their arguments' kind; pick's values (not its index) do too
         passes_kind = fn in ("min", "max", "clamp") or (fn == "pick" and i > 0)
         k = self.value(x, expect if passes_kind else None)
         if i == 0:
            kind = k
      ops = {"min": "Min", "max": "Max", "clamp": "Clamp", "abs": "Abs", "scale": "MulQ8", "chance": "Chance",
             "between": "Between", "rand": "Rand"}
      if fn == "pick":
         a.emit("Pick", len(args) - 1)
         return "int"
      a.emit(ops[fn])
      self.range_of(e)
      return "bool" if fn == "chance" else (kind if fn in ("min", "max", "clamp") else "int")

   def chance_odds(self, n: Any, m: Any) -> None:
      """!Pushes n and m and draws (Chance).
      @param n  successes
      @param m  outcomes
      @return None
      """
      if self.context.get("point"):
         self.err("pure", None, what="chance (it draws)")
      elif self.context.get("pure"):
         self.err("pure-hook", None, hook=self.context["hook"], what="chance (it draws)")
      self.value(n)
      self.value(m)
      self.prog.asm.emit("Chance")
      self.prog.flags |= PROGRAM_RNG

   def jump_false(self, e: Any, label: int) -> None:
      """!Jumps to a label when a condition is false.
      @param e      condition
      @param label  target
      @return None
      """
      a = self.prog.asm
      if isinstance(e, dict) and e.get("op") == "and":
         self.jump_false(e["args"][0], label)
         self.jump_false(e["args"][1], label)
      elif isinstance(e, dict) and e.get("op") == "or":
         true = a.label()
         self.jump_true(e["args"][0], true)
         self.jump_false(e["args"][1], label)
         a.place(true)
      elif isinstance(e, dict) and e.get("op") == "not":
         self.jump_true(e["args"][0], label)
      else:
         self.value(e)
         a.emit("Jz", target=label)

   def jump_true(self, e: Any, label: int) -> None:
      """!Jumps to a label when a condition is true.
      @param e      condition
      @param label  target
      @return None
      """
      a = self.prog.asm
      if isinstance(e, dict) and e.get("op") == "or":
         self.jump_true(e["args"][0], label)
         self.jump_true(e["args"][1], label)
      elif isinstance(e, dict) and e.get("op") == "and":
         false = a.label()
         self.jump_false(e["args"][0], false)
         self.jump_true(e["args"][1], label)
         a.place(false)
      elif isinstance(e, dict) and e.get("op") == "not":
         self.jump_false(e["args"][0], label)
      else:
         self.value(e)
         a.emit("Jnz", target=label)

   # -- primitives --

   def q8_value(self, e: Any) -> None:
      """!Pushes a Q8 argument (a bare integer means that many times: 2 = 512).
      @param e  AST expression
      @return None
      """
      if isinstance(e, int) and not isinstance(e, bool):
         self.prog.asm.push(e * Q8_ONE)
      else:
         self.value(e)

   def primitive(self, s: Mapping[str, Any]) -> bool:
      """!Generates a primitive statement.
      @param s  the node
      @return true when it ends the program
      """
      a = self.prog.asm
      k = s["stmt"]
      ctx = self.context
      spec = ctx["spec"]
      is_move = ctx["owner_kind"] == "move"
      if ctx.get("point") and k != "modify":
         self.err("pure", s, what=k)
         return False
      if ctx.get("pure") and k not in ("modify", "block"):
         self.err("pure-hook", s, hook=ctx["hook"], what=k)
         return False
      if k == "absorb" and ctx["hook"] != "before_damage":
         self.err("absorb-context", s)
      if k == "recruit" and not (ctx["owner_kind"] == "item" and ctx["hook"] == "use_battle"):
         self.err("recruit-context", s)
      if k in ENCOUNTER_KINDS:
         return self.encounter_primitive(s)
      if k in INHERIT_KINDS:
         return self.inherit_primitive(s)
      if k == "damage":
         if not (is_move and ctx["hook"] in ("hit", "land") and ctx["kind"] == "handler"):
            self.err("damage-context", s)   # G1: a delayed hit's land phase too
         a.emit("Damage")
         a.emit("Drop")
         if ctx["repeat"]:
            # every hit of a multi-hit move is its own step
            a.emit("Yield")
            self.prog.flags |= PROGRAM_YIELDS
         return False
      if k == "set_target" and not (is_move and ctx["hook"] == "target"):
         self.err("target-context", s)
      if k == "modify":
         if ctx["kind"] == "handler" and (spec is None or spec["value"] is None):
            self.err("no-value", s, hook=ctx["hook"] or "a sub")
         mode = s["mode"]
         if mode not in MODIFY_MODES:
            self.err("unknown-id", s, what="modify mode", name=mode)
            return False
         if mode == "by":
            self.q8_value(s["value"])
         else:
            self.value(s["value"])
         a.emit("Modify", MODIFY_MODES.index(mode))
         return False
      if k in ("immune", "block"):
         if ctx["kind"] == "handler" and (spec is None or k not in spec["results"]):
            self.err("no-result", s, what=k, hook=ctx["hook"] or "a sub")
         if k == "immune":
            if "heal" in s:
               self.q8_value(s["heal"])
            else:
               a.push(0)
            a.emit("Immune")
         else:
            text = self.text_id(s["text"], s) if "text" in s else NO_TEXT
            a.emit("Block", text)
         return True
      if k == "switch":
         subj = self.subject(s["who"], s)
         mode = s["mode"]
         if mode not in SWITCH_MODES:
            self.err("unknown-id", s, what="switch mode", name=mode)
            return False
         if mode == "random":
            a.push(HOST_PICKS)
         else:
            if not ctx["may_yield"]:
               self.err("yield-context", s, what=f"switch ... {mode} (it asks)")
            a.emit("Ask", ASK_KINDS.index("switch_target"), subj)
            self.prog.flags |= PROGRAM_ASKS | PROGRAM_YIELDS
         a.emit("Switch", subj, SWITCH_MODES.index(mode))
         return False
      return self.plain_primitive(s)

   def plain_primitive(self, s: Mapping[str, Any]) -> bool:
      """!Generates a primitive that maps to one opcode.
      @param s  the node
      @return false (they never end the program)
      """
      a = self.prog.asm
      k = s["stmt"]
      op = PRIMITIVE_OPS[k]
      flags = 0
      for field, kind in PRIMITIVES[k]:
         if kind.startswith("flags:"):
            for f in kind[len("flags:"):].split(","):
               if s.get(f):
                  if k == "stage":
                     flags |= STAGE_QUIET if f == "quiet" else STAGE_SILENT
                  else:
                     flags |= INFLICT_QUIET if k in ("inflict", "cure") else FLAG_BITS[f]
      # operands first (in push order), then the opcode with its immediates
      if k in ("damage_hp", "heal", "train"):
         self.value(s["amount"])
      elif k == "self_hit":
         self.value(s["power"])
      elif k == "stage":
         self.value(s["delta"])
      elif k in ("vol_on", "side_on", "field_on", "weather"):
         if "turns" in s:
            self.value(s["turns"])
         else:
            a.push(self.default_turns(s))   # the condition's own duration (0: its logic decides)
      elif k in ("use_move", "teach"):
         self.value(s["move"], "move")
      elif k == "set_item":
         self.value(s["item"], "item")
      elif k == "set_passive":
         self.value(s["passive"], "passive")
      elif k == "set_types":
         if not 1 <= len(s["types"]) <= MAX_ACTION_ARGS:
            self.err("too-many-types", s, limit=MAX_ACTION_ARGS)
         for t in s["types"][:MAX_ACTION_ARGS]:
            self.value(t, "type")
      elif k == "set_target":
         self.value(s["who"], "battler")
      elif k in ("restore", "raise"):
         if "amount" in s:
            self.value(s["amount"])
         else:
            a.push(0)
            flags |= RESOURCE_FULL   # restore to the maximum
      elif k in ("credit", "reward"):
         self.value(s["amount"])
      elif k in ("recruit",):
         self.value(s["bonus"])
      elif k == "field_effect":
         # F1: the steps or minutes; C8: a constant amount the effect's variable cannot hold is refused (the
         # script compiler's check; a computed one is clamped by the engine, below 1 it does not start)
         r = self.range_of(s["amount"])
         if r is not None and r[0] == r[1] and not 1 <= r[0] <= FIELD_EFFECT_MAX_AMOUNT:
            self.err("field-effect-amount", s["amount"] if isinstance(s["amount"], dict) else s, amount=r[0],
                     max=FIELD_EFFECT_MAX_AMOUNT)
         self.value(s["amount"])
      elif k == "delay":
         self.value(s["amount"])
         self.value(s["turns"])
      elif k == "delay_hit":
         self.value(s["turns"])
      elif k == "say" and any(f in s for f in ("at", "about", "value")):
         # an engine message about chosen subjects with a value (SayAt)
         if "value" in s:
            self.value(s["value"])
         else:
            a.push(0)
         about = self.subject(s["about"], s) if "about" in s else SUBJ_NONE
         a.emit("SayAt", self.lookup_id("messages", s["msg"], s, "message"),
                self.subject(s["at"], s) if "at" in s else SUBJ_HOLDER, about)
         return False
      subj = SUBJ_HOLDER
      if "who" in s and k != "set_target":
         subj = self.subject(s["who"], s)
      elif "side" in s:
         subj = self.subject(s["side"], s) | SUBJ_SIDE
      if k == "self_hit":
         a.emit(op, SUBJ_HOLDER)
      elif k in ("damage_hp", "lock"):
         a.emit(op, subj, flags)
      elif k == "cure":
         a.emit(op, subj, flags)
      elif k == "use_move" and "unless" in s:
         # G1: refused (the run fails) for none or a move with the flag
         op = "CallMove"
         a.emit(op, subj, self.lookup_id("move_flags", s["unless"], s, "move flag"))
      elif k in ("heal", "unlock", "use_move", "consume_item", "set_item", "set_passive", "reset_stages", "delay",
                 "teach", "recruit"):
         a.emit(op, subj)
      elif k == "delay_hit":
         a.emit(op, subj, self.lookup_id("delayed_hits", s["hit"], s, "delayed hit"))
      elif k == "transform":
         a.emit(op, subj)   # G4: the running item's item_use rules
      elif k == "form":
         a.emit(op, subj, self.lookup_id("forms", s["form"], s, "form"))   # G4: a form name ("base" is 0)
      elif k == "train":
         a.emit(op, subj, self.lookup_id("stats", s["stat"], s, "stat"))   # G4: amount on the stack
      elif k == "field_effect":
         a.emit(op, self.lookup_id("field_effects", s["effect"], s, "field effect"))   # F1: amount on the stack
      elif k == "inflict":
         a.emit(op, subj, self.lookup_id("statuses", s["status"], s, "status"), flags)
      elif k == "stage":
         a.emit(op, subj, self.lookup_id("stages", s["stage"], s, "stage", MAX_STAGE_ID) | flags)
      elif k in ("vol_on", "vol_off"):
         a.emit(op, subj, self.lookup_id("volatiles", s["volatile"], s, "volatile"))
      elif k == "side_on":
         a.emit(op, subj, self.lookup_id("side_conditions", s["condition"], s, "side condition"), flags)
      elif k == "side_off":
         a.emit(op, subj, self.lookup_id("side_conditions", s["condition"], s, "side condition"))
      elif k in ("field_on", "field_off"):
         a.emit(op, self.lookup_id("field_conditions", s["condition"], s, "field condition"))
      elif k == "weather":
         a.emit(op, self.lookup_id("weathers", s["weather"], s, "weather"))
      elif k == "set_types":
         a.emit(op, subj, min(len(s["types"]), MAX_ACTION_ARGS))
      elif k == "text":
         a.emit(op, self.text_id(s["text"], s))
      elif k == "say":
         a.emit(op, self.lookup_id("messages", s["msg"], s, "message"))
      elif k == "cue":
         a.emit(op, self.lookup_id("cues", s["cue"], s, "cue"))
      elif k in ("restore", "raise"):
         a.emit(op, subj, self.lookup_id("resources", s["resource"], s, "resource"), flags)
      else:
         a.emit(op)
      _name, _kinds, _pops, pushes = OPS[OP[op]]
      if pushes:
         a.emit("Drop")   # a statement drops the primitive's result (dealt, ok, applied)
      return False

   def inherit_primitive(self, s: Mapping[str, Any]) -> bool:
      """!Generates an inheritance primitive (B1): its operands (zeros for the unused ones), the Inherit op with the
      input's subject (SUBJ_NONE for inherit_pick / inherit_add), the role and the mode; the result is dropped.
      @param s  the node
      @return false (they never end the program)
      """
      a = self.prog.asm
      k = s["stmt"]
      if self.context["hook"] != COMBINE_HOOK:
         self.err("combine-context", s, what=k)
      role = s["role"]
      rid = 0
      if role not in self.catalog.roles:
         self.err("unknown-id", s, what="role", name=role)
      elif role in self.catalog.unstored_roles:
         self.err("inherit-role", s, what=k, name=role, why="it is not stored: computed on read")
      else:
         rid = self.catalog.roles[role] & BYTE_MASK
      subj = SUBJ_NONE
      if k in ("inherit", "inherit_odds"):
         if "from" not in s:
            self.err("ref-arg", s, ref=k, need="needs `from input.N` (or input.ROLE)")
         else:
            subj = self.subject(s["from"], s)
      operands = {"inherit": [], "inherit_pick": ["count"], "inherit_odds": ["n", "d"], "inherit_add": ["amount"]}[k]
      if k == "inherit_pick":
         r = self.range_of(s["count"])
         if r is not None and (r[0] < 0 or r[1] > MAX_INHERIT_PICK):
            self.err("encounter-arg", s["count"] if isinstance(s["count"], dict) else s, what=k, which="count",
                     lo=r[0], hi=r[1], min=0, max=MAX_INHERIT_PICK)
      for field in operands:
         self.value(s[field])
      for _pad in range(INHERIT_OPERANDS - len(operands)):
         a.push(0)
      a.emit("Inherit", subj, rid, INHERIT_KINDS.index(k))
      a.emit("Drop")
      return False

   def encounter_primitive(self, s: Mapping[str, Any]) -> bool:
      """!Generates an encounter primitive (F1): its operands (each within ENCOUNTER_ARGS: an analysed range past
      them is an error, an unknown one is clamped by the engine), zeros for the rest, the Encounter op.
      @param s  the node
      @return false (they never end the program)
      """
      a = self.prog.asm
      k = s["stmt"]
      hook = self.context["hook"]
      if hook not in ENCOUNTER_HOOKS:
         self.err("encounter-context", s, what=k)
      elif k == "encounter_reroll" and hook != "encounter_made":
         self.err("reroll-context", s)
      for field, lo, hi in ENCOUNTER_ARGS[k]:
         r = self.range_of(s[field])
         if r is not None and (r[0] < lo or r[1] > hi):
            self.err("encounter-arg", s[field] if isinstance(s[field], dict) else s, what=k, which=field,
                     lo=r[0], hi=r[1], min=lo, max=hi)
         self.value(s[field])
      for _pad in range(ENCOUNTER_OPERANDS - len(ENCOUNTER_ARGS[k])):
         a.push(0)
      extra = 0
      if k == "encounter_type":
         extra = self.lookup_id("types", s["type"], s, "type")
      elif k == "encounter_field":
         extra = self.lookup_id("species_fields", s["field"], s, "species field")
      a.emit("Encounter", ENCOUNTER_KINDS.index(k), extra)
      return False

   # -- ranges --

   def range_of(self, e: Any) -> Range:
      """!The analysed range of an expression (warns where saturation can engage).
      @param e  AST expression
      @return (lo, hi), or None when a read's range is unknown
      """
      if isinstance(e, bool):
         return None
      if isinstance(e, int):
         return (e, e)
      if not isinstance(e, dict):
         return None
      if "dec" in e:
         q = decimal_q8(e["dec"])
         return (q, q)
      if "subject" in e:
         return (0, MAX_BATTLERS - 1)
      if "name" in e:
         found = self.scope.find(e["name"])
         if found is not None:
            _slot, _kind, rng = found
            return rng
         if e["name"] == NONE_WORD:
            return (0, U16_MAX)   # C5: 255 or 65535 by the kind it stands for
         i = self.catalog.lookup(VALUE_KINDS.get(self.static_kind(e) or "", ""), e["name"])
         return (i, i) if i is not None else (0, MAX_ID16)
      if "ref" in e:
         head, rest = self.ref_head(e["ref"].split("."))
         parts = [head] + rest
         if len(parts) == 1:
            spec = GLOBAL_REFS.get(e["ref"])
            if e["ref"] == "value":
               hook = self.context.get("spec")
               if self.context.get("point"):
                  return self.catalog.points.get(self.context.get("hook", ""))
               return hook["value"] if hook else None
            return spec.rng if spec else None
         table = {"move": MOVE_REFS, "field": FIELD_REFS, "side": SIDE_REFS}.get(parts[0], BATTLER_REFS)
         spec = table.get(".".join(parts[1:]))
         return spec.rng if spec else None
      if "call" in e:
         return self.call_range(e)
      op = e.get("op")
      args = e.get("args", [])
      if op in ("and", "or", "not") or op in COMPARISONS:
         return BOOL_RANGE
      ranges = [self.range_of(x) for x in args]
      if any(r is None for r in ranges):
         return None
      if op in ("/", "%"):
         a, b = ranges
         parts = [r for r in ((b[0], min(b[1], -1)), (max(b[0], 1), b[1])) if r[0] <= r[1]]
         out = [_shallow("bin", [a, d], "effect", op) for d in parts]
         if b[0] <= 0 <= b[1]:
            out.append((0, 0))   # dividing by 0 gives 0
         lo = min(r[0] for r in out)
         hi = max(r[1] for r in out)
         return self.saturating(e, (lo, hi), "quotient" if op == "/" else "remainder")
      if op in ("<<", ">>"):
         a, b = ranges
         counts = (shift_count(b[0]), shift_count(b[1]))
         f = (lambda x, y: x << y) if op == "<<" else (lambda x, y: x >> y)
         return self.saturating(e, _corners(f, a, counts), "shift")
      if op == "neg":
         return self.saturating(e, (-ranges[0][1], -ranges[0][0]), "negation")
      corners = {"+": lambda x, y: x + y, "-": lambda x, y: x - y, "*": lambda x, y: x * y}
      try:
         return _shallow("bin", ranges, "effect", op)
      except rc.RulesetError:
         return self.saturating(e, _corners(corners[op], ranges[0], ranges[1]),
                                {"+": "sum", "-": "difference", "*": "product"}[op])

   def saturating(self, e: Any, r: tuple[int, int], what: str) -> tuple[int, int]:
      """!Warns when a range leaves int32, and clamps it.
      @param e     the node
      @param r     the unclamped range
      @param what  what it is
      @return the clamped range
      """
      if r[0] < INT32_MIN or r[1] > INT32_MAX:
         self.warn("saturation", e, what=what, lo=r[0], hi=r[1])
      return (sat(r[0]), sat(r[1]))

   def call_range(self, e: Mapping[str, Any]) -> Range:
      """!The analysed range of a function call.
      @param e  the call node
      @return (lo, hi) or None
      """
      fn = e["call"]
      if fn in ("chance", "is_foe", "is_ally", "odds"):
         return BOOL_RANGE
      if fn not in FUNCTIONS:
         return None
      ranges = [self.range_of(x) for x in e["args"]]
      if any(r is None for r in ranges) or not ranges:
         return None
      if fn == "pick":
         values = ranges[1:]
         return (min(r[0] for r in values), max(r[1] for r in values)) if values else None
      if fn == "between" and len(ranges) == FUNCTIONS[fn][0]:
         return (ranges[0][0], max(ranges[0][0], ranges[1][1]))
      if fn == "rand":
         return (0, max(0, ranges[0][1] - 1))
      if fn == "scale" and len(ranges) == FUNCTIONS[fn][0]:
         return self.saturating(e, _corners(lambda x, y: (x * y) >> Q8_SHIFT, ranges[0], ranges[1]), "scale")
      if len(ranges) != FUNCTIONS[fn][0]:
         return None
      try:
         return _shallow(fn, ranges, "effect")
      except rc.RulesetError:
         return self.saturating(e, (-ranges[0][1], max(-ranges[0][0], ranges[0][1])), "abs")

   # -- point handlers (pure subset) --

   def generate_point(self, pt: dict[str, Any]) -> None:
      """!Checks a point handler and builds its pure IR (also type-checks it by generating throwaway code).
      @param pt  the point entry
      @return None
      """
      owner = self.owners[pt["owner"]]
      node = pt["node"]
      self.prog = Program(-1, pt["owner"], pt["label"], node, "point", pt["point"])
      self.scope = Scope()
      self.context = {"label": pt["label"], "owner_kind": owner["kind"], "hook": pt["point"], "spec": None,
                      "may_yield": False, "kind": "point", "repeat": 0, "point": True}
      self.where_node = node
      ir = PureBuilder(self).handler(node)
      pt["ir"] = ir
      self.where_node = None

   # -- linking: locals, stack, events --

   def link(self) -> None:
      """!Assigns sub local bases, patches the locals, proves the stacks and computes the event costs.
      @return None
      """
      callers: dict[int, list[int]] = {p.index: [] for p in self.program_list()}
      for i, targets in self.edges.items():
         for t in targets:
            callers[t].append(i)
      # callers before callees (the call graph is acyclic here)
      order: list[int] = []
      seen: set[int] = set()

      def place(i: int) -> None:
         """!Orders a program after every caller.
         @param i  program index
         @return None
         """
         if i in seen:
            return
         seen.add(i)
         for c in callers[i]:
            place(c)
         order.append(i)

      for p in self.program_list():
         place(p.index)
      for i in order:
         p = self.programs[i]
         p.base = max([self.programs[c].base + self.programs[c].own_locals for c in callers[i]], default=0)
         code = bytearray(p.code)
         for pos in p.asm.local_refs:
            code[pos] += p.base
         p.code = bytes(code)
      for i in reversed(order):
         p = self.programs[i]
         p.locals = max([p.base + p.own_locals] + [self.programs[t].locals for t in self.edges[i]])
         p.max_stack = prove_stack(p.code, {t: self.programs[t].max_stack for t in self.edges[i]})
         for t in self.edges[i]:
            p.flags |= self.programs[t].flags
         self.context = {"label": ""}
         if p.locals > EFFECT_LOCALS:
            self.err("too-many-locals", p.node, label=p.label, count=p.locals, limit=EFFECT_LOCALS)
         if p.max_stack > EFFECT_STACK:
            self.err("stack-depth", p.node, label=p.label, count=p.max_stack, limit=EFFECT_STACK)
      for p in self.program_list():
         if p.kind != "handler":
            continue
         seg = self.cost(p.index, 1)
         p.events = seg.worst()
         unit = self.owners[p.owner]["kind"] == "move" and p.hook in UNIT_PHASES
         if p.yields or unit:
            if p.events > EVENTS_PER_STEP:
               self.err("step-budget", p.node, label=p.label, count=p.events, limit=EVENTS_PER_STEP)
         elif p.events > HOOK_EVENT_BUDGET:
            self.err("hook-budget", p.node, label=p.label, count=p.events, limit=HOOK_EVENT_BUDGET)
      for p in self.program_list():
         if p.kind == "sub":
            p.events = self.cost(p.index, 1).worst()

   # -- event costs --

   def cost(self, program: int, depth: int) -> Seg:
      """!Worst-case events of a program run at a nesting depth.
      @param program  program index
      @param depth    1 for a run the engine starts
      @return the summary
      """
      key = (program, depth)
      if key not in self.cost_memo:
         self.cost_memo[key] = Seg(0)   # cut cycles (they are errors already)
         self.cost_memo[key] = self.body_cost(self.programs[program].node["body"], depth, self.programs[program])
      return self.cost_memo[key]

   def body_cost(self, body: Sequence[Mapping[str, Any]], depth: int, p: Program, repeat: bool = False) -> Seg:
      """!Worst-case events of statements.
      @param body    statements
      @param depth   nesting depth
      @param p       the program (for its owner's subs)
      @param repeat  inside a repeat (damage yields after each hit)
      @return the summary
      """
      out = Seg(0)
      for s in body:
         out = seq(out, self.stmt_cost(s, depth, p, repeat))
      return out

   def stmt_cost(self, s: Mapping[str, Any], depth: int, p: Program, repeat: bool) -> Seg:
      """!Worst-case events of one statement.
      @param s       the node
      @param depth   nesting depth
      @param p       the program
      @param repeat  inside a repeat
      @return the summary
      """
      k = s.get("stmt")
      if k == "if":
         return alt(self.body_cost(s["then"], depth, p, repeat), self.body_cost(s.get("else", []), depth, p, repeat))
      if k == "chance":
         return alt(self.body_cost(s["body"], depth, p, repeat), self.body_cost(s.get("else", []), depth, p, repeat))
      if k == "repeat":
         rounds = s["max"] if isinstance(s["max"], int) and 1 <= s["max"] <= MAX_REPEAT else 1
         return self.rounds(self.body_cost(s["body"], depth, p, True), rounds)
      if k == "for":
         return self.rounds(self.body_cost(s["body"], depth, p, repeat), MAX_POSITIONS)
      if k == "call":
         target = self.subs.get((p.owner, s["sub"]))
         return self.cost(target, depth) if target is not None and target not in self.cyclic else Seg(0)
      if k == "yield":
         return YIELD_SEG
      if k == "ask":
         return seq(Seg(OP_EVENTS["ask"][0]), YIELD_SEG)
      if k == "switch" and s.get("mode") in ("ask", "pass"):
         return seq(seq(Seg(OP_EVENTS["ask"][0]), YIELD_SEG), self.primitive_cost(s, depth))
      if k in OP_EVENTS:
         c = self.primitive_cost(s, depth)
         return seq(c, YIELD_SEG) if k == "damage" and repeat else c
      return Seg(0)

   @staticmethod
   def rounds(body: Seg, count: int) -> Seg:
      """!A loop of 0 to count rounds.
      @param body   one round
      @param count  most rounds
      @return the summary
      """
      acc = Seg(0)
      for _ in range(count):
         acc = seq(acc, body)
      return alt(Seg(0), acc)

   def primitive_cost(self, s: Mapping[str, Any], depth: int) -> Seg:
      """!Events of a primitive: its own plus the hooks it triggers one level deeper.
      @param s      the node
      @param depth  nesting depth of the run executing it
      @return the summary (no yields)
      """
      own, triggers = OP_EVENTS[s["stmt"]]
      total = own
      if s["stmt"] == "reset_stages":
         total += len(self.catalog.ids.get("stages") or {})   # a Stage event per stage it changes
      for t in triggers:
         if t == "before_damage" and s["stmt"] == "damage_hp" and not s.get("typed"):
            continue   # only a typed hit runs the target's before_damage hooks (E13b)
         if t.startswith("own:"):
            _, key, which = t.split(":")
            field = {"statuses": "status", "volatiles": "volatile", "side_conditions": "condition",
                     "field_conditions": "condition", "weathers": "weather"}[key]
            hooks = ["start", "already"] if which == "start" else [which]
            # a new weather ends the one before it (any weather's end hook)
            name = None if (key, which) == ("weathers", "end") else s.get(field)
            total += self.own_cost(key, name, hooks, depth + 1)
         else:
            total += self.hook_cost(t, depth + 1)
      return Seg(total)

   def own_cost(self, key: str, name: str | None, hooks: Sequence[str], depth: int) -> int:
      """!Worst-case events of a condition's own hooks (the worst of `hooks`).
      @param key    owner list ("statuses", ...)
      @param name   the condition, or None for any condition of the list
      @param hooks  hook names (one of them runs)
      @param depth  nesting depth they run at
      @return events
      """
      if depth > MAX_EFFECT_DEPTH:
         return 0
      kind = next(k for k, v in OWNER_KINDS.items() if v == key)
      worst = 0
      for o in self.owner_list():
         if o["kind"] != kind or (name is not None and o["name"] != name):
            continue
         for h in hooks:
            total = sum(self.cost(prog, depth).worst() for hook, flags, prog in o["handlers"]
                        if not flags & HANDLER_POINT and hook == HOOK[h])
            worst = max(worst, total)
      return worst

   def hook_cost(self, hook: str, depth: int) -> int:
      """!Worst-case events of dispatching a battler hook (KIND_FAN adds the owner kinds).
      @param hook   hook name
      @param depth  nesting depth the handlers run at
      @return events
      """
      if depth > MAX_EFFECT_DEPTH:
         return 0
      key = (hook, depth)
      memo = self.hook_memo   # a local alias (Doxygen 1.12 reports `x in self.attr` as an undocumented member)
      if key in memo:
         return memo[key]
      self.hook_memo[key] = 0
      per_kind: dict[str, list[int]] = {}
      for o in self.owner_list():
         if o["kind"] == "move":
            continue
         total = sum(self.cost(prog, depth).worst() for h, flags, prog in o["handlers"]
                     if not flags & HANDLER_POINT and h == HOOK[hook])
         if total:
            per_kind.setdefault(o["kind"], []).append(total)
      result = sum(max(v) if KIND_FAN[k] == "max" else sum(v) for k, v in per_kind.items())
      self.hook_memo[key] = result
      return result


# -- the pure backend (point handlers) ---------------------------------------------------------------------------------

## Pure IR expression: ("const", v) | ("get", ref, subj, arg) | ("has", kind, subj, id) | ("local", name) |
## ("value",) | ("op", op, [args]) | ("call", fn, [args]). A bare subject reads Ref battler.
PureExpr = tuple[Any, ...]


class PureBuilder:
   """!Builds the pure IR of a point handler (names resolved through the compiler).

   @var blocks
   Locals in scope (names), block by block.
   """

   def __init__(self, compiler: Compiler) -> None:
      """!Creates a builder.
      @param compiler  the compiler (catalog, diagnostics, scope)
      """
      ## The compiler.
      self.c = compiler
      ## Locals in scope (names), block by block.
      self.blocks: list[dict[str, str]] = [{}]

   def handler(self, node: Mapping[str, Any]) -> dict[str, Any]:
      """!The IR of a point handler.
      @param node  the handler node
      @return {"when": expr or None, "body": statements}
      """
      when = self.expr(node["when"]) if "when" in node else None
      return {"when": when, "body": self.body(node["body"])}

   def body(self, stmts: Sequence[Mapping[str, Any]]) -> list[tuple[Any, ...]]:
      """!The IR of a block.
      @param stmts  statements
      @return IR statements
      """
      self.blocks.append({})
      out: list[tuple[Any, ...]] = []
      for s in stmts:
         k = s.get("stmt")
         if k == "let":
            if any(s["name"] in b for b in self.blocks):
               self.c.err("redeclared", s, name=s["name"])
            e = self.expr(s["value"])
            self.blocks[-1][s["name"]] = "int"
            out.append(("let", s["name"], e))
         elif k == "assign" and "name" in s["target"]:
            name = s["target"]["name"]
            if not any(name in b for b in self.blocks):
               self.c.err("undeclared", s, name=name)
               continue
            e = self.expr(s["value"])
            if s["op"] != "=":
               e = ("op", "+" if s["op"] == "+=" else "-", [("local", name), e])
            out.append(("set", name, e))
         elif k == "if":
            out.append(("if", self.expr(s["cond"]), self.body(s["then"]), self.body(s.get("else", []))))
         elif k == "modify":
            mode = s["mode"]
            v = s["value"]
            e = ("const", v * Q8_ONE) if mode == "by" and isinstance(v, int) and not isinstance(v, bool) else \
               self.expr(v)
            out.append(("modify", MODIFY_MODES.index(mode) if mode in MODIFY_MODES else 0, e))
         elif k == "return":
            out.append(("return",))
         elif k == "assign":
            self.c.err("pure", s, what="writing a ref")
         elif k != "comment":
            self.c.err("pure", s, what=f"`{k}`")
      self.blocks.pop()
      return out

   def expr(self, e: Any, expect: str | None = None) -> PureExpr:
      """!The IR of an expression.
      @param e       AST expression
      @param expect  kind expected for bare names
      @return IR
      """
      c = self.c
      if isinstance(e, bool) or not isinstance(e, (int, dict)):
         c.err("syntax", None, text=f"not an expression: {e!r}")
         return ("const", 0)
      if isinstance(e, int):
         return ("const", sat(e))
      if "dec" in e:
         return ("const", decimal_q8(e["dec"]))
      if "name" in e:
         if any(e["name"] in b for b in self.blocks):
            return ("local", e["name"])
         _kind, ident, _rng = c.name_value(e, expect)
         return ("const", ident or 0)
      if "subject" in e:
         return ("get", REFS.index("battler"), BATTLER_SUBJECTS.index(e["subject"]), 0)
      if "ref" in e:
         if e["ref"] == "value":
            return ("value",)
         head = e["ref"].split(".")[0]
         if head not in BATTLER_SUBJECTS and head not in ("move", "field", "side") and "." in e["ref"]:
            c.err("point-subject", e, name=head)
            return ("const", 0)
         if "of" in e and e["of"] not in BATTLER_SUBJECTS:
            c.err("point-subject", e, name=e["of"])
            return ("const", 0)
         resolved = c.resolve_ref(e)
         if resolved is None:
            return ("const", 0)
         form, spec, subj, arg = resolved
         if form == "has":
            return ("has", arg[0], subj, arg[1])
         if form == "role":
            c.err("pure", e, what="record roles")
            return ("const", 0)
         return ("get", REFS.index(spec.ref), subj, arg)
      if "call" in e:
         fn = e["call"]
         if fn not in FUNCTIONS:
            c.err("unknown-function", e, name=fn)
            return ("const", 0)
         lo, hi = FUNCTIONS[fn]
         if not lo <= len(e["args"]) <= hi:
            c.err("arity", e, name=fn, want=str(lo) if lo == hi else f"{lo}-{hi}", got=len(e["args"]))
            return ("const", 0)
         if fn in RNG_FUNCTIONS:
            c.err("pure", e, what=f"{fn}() (it draws)")
            return ("const", 0)
         if fn in ("is_foe", "is_ally"):
            subs = []
            for x in e["args"]:
               if not (isinstance(x, dict) and "subject" in x):
                  c.err("point-subject", e, name=format_expr(x))
                  return ("const", 0)
               subs.append(("get", REFS.index("side"), BATTLER_SUBJECTS.index(x["subject"]), 0))
            return ("op", "!=" if fn == "is_foe" else "==", subs)
         return ("call", fn, [self.expr(x, expect) for x in e["args"]])
      op = e["op"]
      args = e["args"]
      if op in ("not", "neg"):
         return ("op", op, [self.expr(args[0])])
      left = self.expr(args[0], c.static_kind(args[1]) if len(args) > 1 else None)
      right = self.expr(args[1], c.static_kind(args[0]))
      c.range_of(e)
      return ("op", op, [left, right])


def evaluate_ir(e: PureExpr, env: dict[str, int], value: int, read: Callable[[str, int, int, int], int]) -> int:
   """!Evaluates a pure IR expression (VM semantics).
   @param e      IR
   @param env    locals
   @param value  the point's current value
   @param read   read(kind 'get' or 'has', ref or Has kind index, subj8, arg) -> int
   @return the value
   """
   k = e[0]
   if k == "const":
      return e[1]
   if k == "local":
      return env[e[1]]
   if k == "value":
      return value
   if k in ("get", "has"):
      _k, index, subj, arg = e
      v = read(k, index, subj, arg)
      return int(v != 0) if k == "has" else sat(v)
   if k == "op":
      _k, op, args = e
      if op == "and":
         return int(evaluate_ir(args[0], env, value, read) != 0 and evaluate_ir(args[1], env, value, read) != 0)
      if op == "or":
         return int(evaluate_ir(args[0], env, value, read) != 0 or evaluate_ir(args[1], env, value, read) != 0)
      v = [evaluate_ir(x, env, value, read) for x in args]
      if op == "not":
         return int(v[0] == 0)
      if op == "neg":
         return sat(-v[0])
      return BINARY[op](v[0], v[1])
   _k, fn, args = e
   v = [evaluate_ir(x, env, value, read) for x in args]
   if fn == "min":
      return min(v)
   if fn == "max":
      return max(v)
   if fn == "clamp":
      x, lo, hi = v
      return max(lo, min(hi, x))
   if fn == "abs":
      return sat(abs(v[0]))
   if fn == "scale":
      return mul_q8(v[0], v[1])
   index, table = v[0], v[1:]
   return table[max(0, min(len(table) - 1, index))]   # pick


def run_ir(stmts: Sequence[tuple[Any, ...]], env: dict[str, int], value: list[int],
           read: Callable[[str, int, int, int], int]) -> bool:
   """!Runs pure IR statements.
   @param stmts  IR statements
   @param env    locals
   @param value  [the point's value] (updated)
   @param read   the read callback
   @return true when a return ended the handler
   """
   for s in stmts:
      k = s[0]
      if k in ("let", "set"):
         _k, name, e = s
         env[name] = evaluate_ir(e, env, value[0], read)
      elif k == "if":
         _k, cond, then, other = s
         if run_ir(then if evaluate_ir(cond, env, value[0], read) != 0 else other, env, value, read):
            return True
      elif k == "modify":
         _k, index, e = s
         arg = evaluate_ir(e, env, value[0], read)
         mode = MODIFY_MODES[index]
         value[0] = mul_q8(value[0], arg) if mode == "by" else sat(value[0] + arg) if mode == "add" else arg
      elif k == "return":
         return True
   return False


def pure_ir(handler: Mapping[str, Any], catalog: Catalog) -> dict[str, Any]:
   """!The pure IR of one point handler, checked on its own (no owner around it; the battle compiler builds its
   point handlers through this, the point names being the ruleset's).
   @param handler  the point handler node ({"point", "when"?, "body"})
   @param catalog  the structure's names
   @return {"when": IR or None, "body": IR statements}, as emit_pure_cpp() takes them under "ir"
   @throws EffectError when the handler is not valid pure code
   """
   c = Compiler({"owners": []}, catalog)
   c.prog = Program(-1, 0, "point", handler, "point", handler.get("point", ""))
   c.context = {"label": f"point {handler.get('point', '')}", "point": True, "spec": None,
                "hook": handler.get("point", ""), "kind": "point"}
   ir = PureBuilder(c).handler(handler)
   if c.errors():
      # the caller names the place (str(d) would add "manifest: " for a handler without a file)
      raise EffectError("; ".join(f"{d.severity}: {d.message}" for d in c.errors()))
   return ir


def run_pure(ir: Mapping[str, Any], value: int, read: Callable[[str, int, int, int], int]) -> int:
   """!Runs the pure IR of a point handler (pure_ir()) as its generated C++ does.
   @param ir     from pure_ir()
   @param value  the value coming in
   @param read   the read callback (see evaluate_pure())
   @return the value going out
   """
   env: dict[str, int] = {}
   if ir["when"] is not None and evaluate_ir(ir["when"], env, value, read) == 0:
      return value
   box = [value]
   run_ir(ir["body"], env, box, read)
   return box[0]


def evaluate_pure(handler: Mapping[str, Any], catalog: Catalog, value: int,
                  read: Callable[[str, int, int, int], int]) -> int:
   """!Runs a point handler in Python, exactly as its generated C++ does (the studio's pipeline trace).
   @param handler  the point handler node ({"point", "when"?, "body"})
   @param catalog  the structure's names
   @param value    the value coming in
   @param read     read(kind, index, subj8, arg): kind 'get' (index = Ref) or 'has' (index = HasKind), the
                   same arguments the generated C++ passes to point_get / point_has
   @return the value going out
   @throws EffectError when the handler is not valid pure code
   """
   ir = pure_ir(handler, catalog)
   env: dict[str, int] = {}
   if ir["when"] is not None and evaluate_ir(ir["when"], env, value, read) == 0:
      return value
   box = [value]
   run_ir(ir["body"], env, box, read)
   return box[0]


def cpp_expr(e: PureExpr) -> str:
   """!C++ for a pure IR expression (effect.h helpers, so the semantics match the VM).
   @param e  IR
   @return C++ expression text
   """
   k = e[0]
   if k == "const":
      return f"int32_t{{{e[1]}}}" if e[1] != INT32_MIN else "INT32_MIN"
   if k == "local":
      return f"l_{e[1]}"
   if k == "value":
      return "v"
   if k in ("get", "has"):
      _k, index, subj, arg = e
      if k == "get":
         return f"effect::point_get(c, effect::Ref::{camel(REFS[index])}, {subj}, {arg})"
      return f"int32_t(effect::point_has(c, effect::HasKind::{camel(HAS_KINDS[index])}, {subj}, {arg}))"
   if k == "op":
      _k, op, args = e
      v = [cpp_expr(x) for x in args]
      if op == "and":
         return f"int32_t(({v[0]}) != 0 && ({v[1]}) != 0)"
      if op == "or":
         return f"int32_t(({v[0]}) != 0 || ({v[1]}) != 0)"
      if op == "not":
         return f"int32_t(({v[0]}) == 0)"
      if op == "neg":
         return f"effect::sat_neg({v[0]})"
      if op in COMPARISONS:
         return f"int32_t(({v[0]}) {op} ({v[1]}))"
      return f"{BINARY_CPP[op]}({v[0]}, {v[1]})"
   _k, fn, args = e
   v = [cpp_expr(x) for x in args]
   if fn in ("min", "max"):
      return f"effect::{fn}_value({v[0]}, {v[1]})"
   if fn == "clamp":
      return "effect::clamp_value(" + ", ".join(v) + ")"
   if fn == "abs":
      return f"effect::sat_abs({v[0]})"
   if fn == "scale":
      return f"effect::mul_q8({v[0]}, {v[1]})"
   table = ", ".join(v[1:])
   return f"effect::pick_value({v[0]}, {{{table}}})"


def camel(name: str) -> str:
   """!CamelCase of a snake_case name.
   @param name  e.g. "before_move"
   @return e.g. "BeforeMove"
   """
   return "".join(p[:1].upper() + p[1:] for p in name.split("_"))


def cpp_body(stmts: Sequence[tuple[Any, ...]], depth: int) -> list[str]:
   """!C++ statements of pure IR.
   @param stmts  IR statements
   @param depth  indent level
   @return lines
   """
   lead = INDENT * depth
   out: list[str] = []
   for s in stmts:
      k = s[0]
      if k == "let":
         _k, name, e = s
         out.append(f"{lead}int32_t l_{name} = {cpp_expr(e)};")
         out.append(f"{lead}(void)l_{name};")
      elif k == "set":
         _k, name, e = s
         out.append(f"{lead}l_{name} = {cpp_expr(e)};")
      elif k == "if":
         _k, cond, then, other = s
         out.append(f"{lead}if (({cpp_expr(cond)}) != 0)")
         out.append(lead + "{")
         out.extend(cpp_body(then, depth + 1))
         out.append(lead + "}")
         if other:
            out.append(lead + "else")
            out.append(lead + "{")
            out.extend(cpp_body(other, depth + 1))
            out.append(lead + "}")
      elif k == "modify":
         _k, index, e = s
         mode = camel(MODIFY_MODES[index])
         out.append(f"{lead}v = effect::modify_value(effect::ModifyMode::{mode}, v, {cpp_expr(e)});")
      elif k == "return":
         out.append(f"{lead}return v;")
   return out


def emit_pure_cpp(points: Sequence[Mapping[str, Any]], function: str = "point_handler") -> str:
   """!C++ of point handlers: one function per handler and a dispatcher
   int32_t FUNCTION(uint16_t fn, effect::PointCtx& c, int32_t v) (fn = Handler::program of a point handler).
   @param points    the compiler's point entries (label, ir), in fn order
   @param function  the dispatcher's name
   @return C++ source text (inside the caller's namespace; needs capsule/battle/effect.h)
   """
   lines: list[str] = []
   for i, pt in enumerate(points):
      lines.append(f"/// {pt['label']}.")
      lines.append(f"[[maybe_unused]] static int32_t point_{i}(effect::PointCtx& c, int32_t v)")
      lines.append("{")
      lines.append(INDENT + "(void)c;")
      ir = pt["ir"]
      if ir["when"] is not None:
         lines.append(f"{INDENT}if (({cpp_expr(ir['when'])}) == 0)")
         lines.append(INDENT + "{")
         lines.append(INDENT2 + "return v;")
         lines.append(INDENT + "}")
      lines.extend(cpp_body(ir["body"], 1))
      lines.append(INDENT + "return v;")
      lines.append("}")
      lines.append("")
   lines.append("/// Runs point handler @p fn on @p v.")
   lines.append(f"int32_t {function}(uint16_t fn, effect::PointCtx& c, int32_t v)")
   lines.append("{")
   lines.append(INDENT + "switch (fn)")
   lines.append(INDENT + "{")
   for i in range(len(points)):
      lines.append(f"{INDENT}case {i}:")
      lines.append(f"{INDENT2}return point_{i}(c, v);")
   lines.append(f"{INDENT}default:")
   lines.append(f"{INDENT2}(void)c;")
   lines.append(f"{INDENT2}return v;")
   lines.append(INDENT + "}")
   lines.append("}")
   return "\n".join(lines) + "\n"


# -- banks -------------------------------------------------------------------------------------------------------------

class Bank:
   """!A compiled bank: code, programs, handlers, owners and point handlers."""

   def __init__(self, compiler: Compiler) -> None:
      """!Lays out a compiled unit.
      @param compiler  a compiler that ran without errors
      """
      ## Program code, concatenated.
      self.code = b""
      ## ProgramInfo tuples (offset, length, max_stack, locals, flags, events_max) and labels.
      self.programs: list[tuple[int, int, int, int, int, int]] = []
      ## Program labels.
      self.labels: list[str] = []
      for p in compiler.programs:
         self.programs.append((len(self.code), len(p.code), p.max_stack, p.locals, p.flags,
                               min(p.events, MAX_EVENTS_FIELD)))
         self.labels.append(p.label)
         self.code += p.code
      ## Handlers (hook, flags, program).
      self.handlers: list[tuple[int, int, int]] = []
      ## Owners (hook_mask, point_mask, name id, first_handler, tags, count, kind, flags) and their names.
      self.owners: list[tuple[int, int, int, int, int, int, int, int]] = []
      ## "kind Name" per owner.
      self.owner_names: list[str] = []
      for o in compiler.owners:
         first = len(self.handlers)
         hooks = points = 0
         for hook, flags, prog in o["handlers"]:
            self.handlers.append((hook, flags, prog))
            if flags & HANDLER_POINT:
               points |= 1 << hook
            else:
               hooks |= 1 << hook
         self.owners.append((hooks, points, o["id"], first, o["tags"], len(o["handlers"]),
                             list(OWNER_KINDS).index(o["kind"]), o["flags"]))
         self.owner_names.append(f"{o['kind']} {o['name']}")
      ## Point handlers in fn order.
      self.points = compiler.points
      ## Point names in id order (the disassembly's).
      self.point_names = list(compiler.catalog.points)
      ## Hook or phase name per program ('' for a sub).
      self.hooks = [p.hook for p in compiler.programs]
      ## Debug line entries (pc, file, line, col) per program (empty unless compiled under track_lines()).
      self.lines = [list(p.lines) for p in compiler.programs]


# -- debug line table (bench builds only; D-23 section 3.3) ------------------------------------------------------------

def track_lines(on: bool) -> None:
   """!Turns the debug line table on or off for the parses and compiles that follow: with it on, every parsed node
   keeps its file, line and column, and every program records which source line each stretch of its code comes
   from (Bank.lines; emit_line_table()). Off by default; the bytecode is the same either way.
   @param on  record positions
   @return None
   """
   LINE_TRACKING[0] = on
   if not on:
      NODE_SOURCES.clear()


def emit_line_table(bank: Bank, name: str = "kEffectLines") -> list[str]:
   """!The bank's pc -> source table as C++ (capsule::battle::effect::LineTable `name`, with its entries
   `name`Entries and files `name`Files): entries sorted by program and pc; an instruction at pc of a program comes
   from the last entry of that program at or before pc.
   @param bank  a bank compiled under track_lines(True)
   @param name  the table's identifier
   @return lines (definitions; the caller puts them in a namespace and includes capsule/battle/effect.h)
   @throws EffectError when a line, column or file count does not fit 16 bits
   """
   files: dict[str, int] = {}
   rows: list[str] = []
   for program, entries in enumerate(bank.lines):
      for pc, file, line, col in entries:
         fi = files.setdefault(file, len(files))
         if max(line, col, fi + 1, program, pc) > U16_MAX:
            raise EffectError(f"{file}:{line}:{col}: past the debug line table's 16-bit fields")
         rows.append(f"{INDENT}{{{program}, {pc}, {line}, {col}, {fi}, 0}},")
   count = len(rows)
   if not rows:
      rows.append(f"{INDENT}{{0, 0, 0, 0, 0, 0}},")   # an empty table keeps one entry its count leaves out
   names = [json.dumps(f) for f in files] or ["nullptr"]
   return ([f"const effect::SourceLine {name}Entries[] = {{"] + rows + ["};",
            f"const char* const {name}Files[] = {{{', '.join(names)}}};",
            f"const effect::LineTable {name} = {{{name}Entries, {name}Files, {count}, {len(files)}}};"])


def _cpp_bytes(data: bytes, per_line: int = BYTES_PER_LINE) -> list[str]:
   """!Byte array initializer lines.
   @param data      bytes
   @param per_line  bytes per line
   @return lines
   """
   return [INDENT + ", ".join(f"0x{b:02X}" for b in data[i:i + per_line]) + "," for i in range(0, len(data), per_line)]


def _enum_asserts() -> list[str]:
   """!static_asserts that tie the C++ enums to this compiler's tables.
   @return lines
   """
   out = [f"static_assert(effect::kOpCount == {len(OPS)}, \"effect opcode table drifted\");"]
   for i, (name, kinds, pops, pushes) in enumerate(OPS):
      size = sum(OPERAND_BYTES[k] for k in kinds)
      out.append(f"static_assert(static_cast<int>(effect::Op::{name}) == {i} && effect::op_info(effect::Op::{name})"
                 f".size == {size} && effect::op_info(effect::Op::{name}).pops == {pops} && "
                 f"effect::op_info(effect::Op::{name}).pushes == {pushes});")
   tables = [("Ref", REFS, "NUM_REFS"), ("HasKind", HAS_KINDS, "NUM_KINDS"), ("Hook", [h["name"] for h in HOOKS],
             "NUM_HOOKS"), ("OwnerKind", list(OWNER_KINDS), "NUM_KINDS"),
             ("BattlerSet", BATTLER_SETS, "NUM_SETS"), ("ModifyMode", MODIFY_MODES, "NUM_MODES"),
             ("SwitchMode", SWITCH_MODES, "NUM_MODES"), ("AskKind", ASK_KINDS, "NUM_KINDS")]
   for enum, names, count in tables:
      checks = " && ".join(f"static_cast<int>(effect::{enum}::{camel(n)}) == {i}" for i, n in enumerate(names))
      out.append(f"static_assert({checks} && static_cast<int>(effect::{enum}::{count}) == {len(names)});")
   out.append(f"static_assert(effect::kHookMaskWords == {hook_mask_words()}, \"hook mask words drifted\");")
   return out


def emit_cpp(bank: Bank, namespace: str, source: str = "") -> str:
   """!The C++ of a bank: kCode, kPrograms, kHandlers, kOwners, kOwnerNames, kBank and point_handler().
   @param bank       the bank
   @param namespace  C++ namespace for the tables
   @param source     what it was compiled from (comment)
   @return C++ source text
   """
   lines = [f"// Generated by tools/effect_compiler.py from {source or 'effect sources'}. Do not edit.",
            "#include \"capsule/battle/effect.h\"", "", "#include <cstdint>", "",
            f"namespace {namespace}", "{", "", "using namespace capsule::battle;", ""]
   lines.extend(_enum_asserts())
   lines.append("")
   # extern: namespace-scope const objects would otherwise have internal linkage
   lines.append("extern const effect::Bank kBank;")
   lines.append(f"extern const char* const kOwnerNames[{max(1, len(bank.owner_names))}];")
   lines.append("int32_t point_handler(uint16_t fn, effect::PointCtx& c, int32_t v);")
   lines.append("")
   code = bank.code or b"\0"
   lines.append(f"/// Every program's code ({len(bank.code)} bytes).")
   lines.append(f"const uint8_t kCode[{len(code)}] = {{")
   lines.extend(_cpp_bytes(code))
   lines.append("};")
   lines.append("/// Programs.")
   lines.append(f"const effect::ProgramInfo kPrograms[{max(1, len(bank.programs))}] = {{")
   for (off, length, stack, locals, flags, events), label in zip(bank.programs, bank.labels):
      lines.append(f"{INDENT}{{{off}, {length}, {stack}, {locals}, {flags}, {events}, 0}},   // {label}")
   if not bank.programs:
      lines.append(INDENT + "{0, 0, 0, 0, 0, 0, 0},")
   lines.append("};")
   lines.append("/// Handlers, owner by owner.")
   lines.append(f"const effect::Handler kHandlers[{max(1, len(bank.handlers))}] = {{")
   for hook, flags, prog in bank.handlers:
      lines.append(f"{INDENT}{{{hook}, {flags}, {prog}}},")
   if not bank.handlers:
      lines.append(INDENT + "{0, 0, 0},")
   lines.append("};")
   lines.append("/// Owners.")
   lines.append(f"const effect::Owner kOwners[{max(1, len(bank.owners))}] = {{")
   for (hooks, points, ident, first, tags, count, kind, flags), name in zip(bank.owners, bank.owner_names):
      lines.append(f"{INDENT}{{{hook_mask_cpp(hooks)}, 0x{points:08X}u, {ident}, {first}, {tags}, {count}, "
                   f"static_cast<effect::OwnerKind>({kind}), {flags}, {{0, 0, 0}}}},   // {name}")
   if not bank.owners:
      lines.append(INDENT + "{" + hook_mask_cpp(0) + ", 0, 0, 0, 0, 0, effect::OwnerKind::Move, 0, {0, 0, 0}},")
   lines.append("};")
   lines.append("/// Owner names (\"kind Name\"), for tests and tools.")
   lines.append(f"const char* const kOwnerNames[{max(1, len(bank.owner_names))}] = {{")
   for name in bank.owner_names or [""]:
      lines.append(f"{INDENT}{json.dumps(name, ensure_ascii=True)},")
   lines.append("};")
   lines.append("/// The bank.")
   lines.append(f"const effect::Bank kBank = {{kCode, kPrograms, kHandlers, kOwners, {len(bank.code)}, "
                f"{len(bank.programs)}, {len(bank.handlers)}, {len(bank.owners)}, 0}};")
   lines.append("")
   lines.append(emit_pure_cpp(bank.points))
   lines.append(f"}} // namespace {namespace}")
   return "\n".join(lines) + "\n"


def disassemble(bank: Bank) -> str:
   """!A readable listing of a bank (programs, handlers, owners).
   @param bank  the bank
   @return text
   """
   out: list[str] = []
   for i, ((off, length, stack, locals, flags, events), label) in enumerate(zip(bank.programs, bank.labels)):
      names = [n for bit, n in ((PROGRAM_YIELDS, "yields"), (PROGRAM_ASKS, "asks"), (PROGRAM_RNG, "rng"))
               if flags & bit]
      out.append(f"program {i}: {label} (bytes {length}, stack {stack}, locals {locals}, events {events}"
                 + (", " + ", ".join(names) if names else "") + ")")
      for at, name, values in instructions(bank.code[off:off + length]):
         out.append(f"   {at:04d}  {name}" + ("  " + " ".join(str(v) for v in values) if values else ""))
   for (hooks, points, ident, first, tags, count, kind, flags), name in zip(bank.owners, bank.owner_names):
      hs = []
      for hook, hflags, prog in bank.handlers[first:first + count]:
         hs.append(f"point {bank.point_names[hook]} -> fn {prog}" if hflags & HANDLER_POINT else
                   f"{HOOKS[hook]['name']}{' per_foe' if hflags & HANDLER_PER_FOE else ''} -> program {prog}")
      out.append(f"owner {name} (id {ident}, tags 0x{tags:04X}, flags {flags}): " + "; ".join(hs))
   return "\n".join(out) + "\n"


# -- files -------------------------------------------------------------------------------------------------------------

class Unit:
   """!A compile unit: sources (text and AST), their structure and the result.

   Attributes are documented where __init__ sets them; the ones below are repeated here because Doxygen's
   Python parser otherwise reports their later uses as undocumented members.

   @var compiler
   The compiler (None when a source does not parse).
   """

   def __init__(self, sources: Sequence[tuple[str, str]], structure: Mapping[str, Any] | None,
                compile_sources: bool = True) -> None:
      """!Parses and compiles sources.
      @param sources          (file name, text) in order
      @param structure        the structure (names), or None
      @param compile_sources  false: parse only (formatting, ASTs)
      """
      ## (file, text, AST, positions) per source.
      self.sources: list[tuple[str, str, dict[str, Any], Positions]] = []
      ## Every diagnostic.
      self.diagnostics: list[EffectDiagnostic] = []
      ## The compiler (None when a source does not parse).
      self.compiler: Compiler | None = None
      ## The catalog.
      self.catalog = Catalog(structure)
      positions: Positions = {}
      owners: list[Any] = []
      for file, text in sources:
         try:
            ast, pos = parse_positions(text, file)
         except ScriptError as e:
            self.diagnostics.append(EffectDiagnostic("syntax", "error", e.message, e.file, e.line, e.col))
            continue
         self.sources.append((file, text, ast, pos))
         positions.update(pos)
         owners.extend(ast["owners"])
      if any(d.severity == "error" for d in self.diagnostics) or not compile_sources:
         return
      file = sources[0][0] if len(sources) == 1 else ""
      self.compiler = Compiler({"schema": SCHEMA_AST, "owners": owners}, self.catalog, file, positions).run()
      self.diagnostics.extend(self.compiler.diagnostics)

   def ok(self) -> bool:
      """!Whether it compiled without errors.
      @return true when there are no errors
      """
      return not any(d.severity == "error" for d in self.diagnostics)

   def bank(self) -> Bank:
      """!The compiled bank.
      @return the bank
      @throws EffectError when there were errors
      """
      if not self.ok() or self.compiler is None:
         raise EffectError("the sources have errors")
      return Bank(self.compiler)


def check(ast: Mapping[str, Any], structure: Mapping[str, Any] | Catalog | None,
          positions: Positions | None = None, file: str = "") -> list[EffectDiagnostic]:
   """!Checks an AST (D-23 API).
   @param ast        file AST
   @param structure  the structure (or a Catalog)
   @param positions  node positions, when it came from text
   @param file       source file name
   @return diagnostics
   """
   catalog = structure if isinstance(structure, Catalog) else Catalog(structure)
   return Compiler(ast, catalog, file, positions).run().diagnostics


def emit_bytecode(ast: Mapping[str, Any], structure: Mapping[str, Any] | Catalog | None) -> Bank:
   """!Compiles an AST to a bank (D-23 API).
   @param ast        file AST
   @param structure  the structure (or a Catalog)
   @return the bank
   @throws EffectError when it has errors
   """
   catalog = structure if isinstance(structure, Catalog) else Catalog(structure)
   c = Compiler(ast, catalog).run()
   if c.errors():
      raise EffectError("; ".join(str(d) for d in c.errors()))
   return Bank(c)


def load_unit(path: str | Path, structure_path: str | Path | None = None, compile_sources: bool = True) -> Unit:
   """!Loads a `.ceffect` or a capsule.effects/1 manifest and compiles it.
   @param path             the file
   @param structure_path   structure JSON for a `.ceffect` (manifests name their own)
   @param compile_sources  false: parse only
   @return the unit
   @throws EffectError on a manifest error
   """
   path = Path(path)
   structure = None
   if path.suffix == ".json":
      manifest = json.loads(path.read_text(encoding="utf-8"))
      if manifest.get("schema") != SCHEMA_MANIFEST:
         raise EffectError(f"{path}: not a {SCHEMA_MANIFEST} manifest")
      if "structure" in manifest:
         structure = json.loads((path.parent / manifest["structure"]).read_text(encoding="utf-8"))
      files = [path.parent / s for s in manifest.get("sources", [])]
   else:
      files = [path]
   if structure_path is not None:
      structure = json.loads(Path(structure_path).read_text(encoding="utf-8"))
   for f in files:
      # a listed source that is missing is an error, never an empty source
      if not f.is_file():
         raise EffectError(f"{path}: the source {f.name} does not exist ({f})")
   return Unit([(str(f), f.read_text(encoding="utf-8")) for f in files], structure, compile_sources)


def main(argv: Sequence[str]) -> int:
   """!Command line.
   @param argv  arguments (without the program name)
   @return exit status
   """
   ap = argparse.ArgumentParser(description="Effect logic compiler (D-23 section 1)")
   ap.add_argument("file", help=".ceffect source or capsule.effects/1 manifest")
   mode = ap.add_mutually_exclusive_group(required=True)
   mode.add_argument("--check", action="store_true", help="report diagnostics")
   mode.add_argument("--format", action="store_true", help="print the canonical text")
   mode.add_argument("--ast", metavar="OWNER", help="print an owner's JSON AST (name or kind:name)")
   mode.add_argument("--emit", metavar="OUT", help="write the bank as C++")
   mode.add_argument("--disasm", action="store_true", help="print the bytecode listing")
   ap.add_argument("--structure", help="structure JSON (names) for a .ceffect")
   ap.add_argument("--namespace", default="effect_data", help="C++ namespace of --emit (default effect_data)")
   args = ap.parse_args(argv)
   try:
      unit = load_unit(args.file, args.structure, compile_sources=not (args.format or args.ast))
   except (EffectError, OSError, json.JSONDecodeError) as e:
      print(f"error: {e}", file=sys.stderr)
      return 1
   for d in unit.diagnostics:
      print(d, file=sys.stderr)
   if args.format or args.ast:
      if not unit.sources or len(unit.sources) != len(load_files(args.file)):
         return 1
      if args.format:
         many = len(unit.sources) > 1
         for file, _text, ast, _pos in unit.sources:
            sys.stdout.write((f"# file: {file}\n" if many else "") + format(ast))
         return 0
      owners = [o for _f, _t, ast, _p in unit.sources for o in ast["owners"]]
      try:
         sys.stdout.write(to_json(owner_ast({"owners": owners}, args.ast)))
      except EffectError as e:
         print(f"error: {e}", file=sys.stderr)
         return 1
      return 0
   if not unit.ok():
      return 1
   if args.emit:
      Path(args.emit).parent.mkdir(parents=True, exist_ok=True)
      Path(args.emit).write_text(emit_cpp(unit.bank(), args.namespace, Path(args.file).name), encoding="utf-8")
   elif args.disasm:
      sys.stdout.write(disassemble(unit.bank()))
   return 0


def load_files(path: str | Path) -> list[Path]:
   """!The sources a file names (itself, or a manifest's sources).
   @param path  .ceffect or manifest
   @return source paths
   """
   path = Path(path)
   if path.suffix != ".json":
      return [path]
   manifest = json.loads(path.read_text(encoding="utf-8"))
   return [path.parent / s for s in manifest.get("sources", [])]


if __name__ == "__main__":
   sys.exit(main(sys.argv[1:]))
